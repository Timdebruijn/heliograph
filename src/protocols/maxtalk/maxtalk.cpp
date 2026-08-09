// SPDX-License-Identifier: MIT

#include "maxtalk.h"

#include <cstring>

namespace heliograph::maxtalk {
namespace {

constexpr char kOpen  = '{';
constexpr char kClose = '}';
/// The literal that sits between the header and the payload in every frame either source
/// carries. Its meaning is not established -- it may be a port or device selector -- so it is
/// reproduced verbatim rather than constructed from anything.
constexpr char kPayloadMarker[] = "|64:";

char hexDigit(uint8_t nibble) {
    return static_cast<char>(nibble < 10 ? '0' + nibble : 'A' + (nibble - 10));
}

/// -1 for anything that is not a hex digit, so callers can reject rather than silently read 0.
int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool writeHex(char* out, uint32_t value, size_t digits) {
    for (size_t i = 0; i < digits; ++i) {
        out[digits - 1 - i] = hexDigit(static_cast<uint8_t>((value >> (4 * i)) & 0xF));
    }
    return true;
}

/// Parses up to `maxDigits` hex characters. Returns the count consumed, 0 on the first
/// non-digit -- an empty value is a malformed pair, not a zero reading.
size_t parseHex(const char* p, size_t available, size_t maxDigits, uint32_t& out) {
    uint32_t v    = 0;
    size_t   used = 0;
    while (used < available && used < maxDigits) {
        const int d = hexValue(p[used]);
        if (d < 0) break;
        v = (v << 4) | static_cast<uint32_t>(d);
        ++used;
    }
    out = v;
    return used;
}

}  // namespace

const char* parseResultName(ParseResult result) {
    switch (result) {
        case ParseResult::Ok:              return "ok";
        case ParseResult::Incomplete:      return "incomplete";
        case ParseResult::NotAFrame:       return "not a frame";
        case ParseResult::BadChecksum:     return "bad checksum";
        case ParseResult::LengthMismatch:  return "length mismatch";
        case ParseResult::WrongSender:     return "wrong sender";
        case ParseResult::Malformed:       return "malformed";
        case ParseResult::TooManyReadings: return "too many readings";
    }
    return "unknown";
}

uint16_t checksum(const char* body, size_t length) {
    // No per-iteration mask, despite the published implementation carrying one. It cannot change
    // the answer: the final narrowing cast reduces modulo 2^16, and truncating a sum that was
    // itself taken modulo 2^32 gives the same residue -- 2^32 is a multiple of 2^16, so even an
    // accumulator that wrapped would land on the identical value.
    //
    // Written down because a mask was here first and MUTATION TESTING FOUND IT DEAD: removing it
    // left every test passing, including the one written specifically to cover it. A line that
    // cannot fail is not a safeguard, it is a claim nobody can check.
    uint32_t sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum += static_cast<uint8_t>(body[i]);
    }
    return static_cast<uint16_t>(sum);
}

size_t buildRequest(uint8_t destination, const char* const* codes, size_t codeCount, char* out,
                    size_t outSize) {
    if (out == nullptr || codes == nullptr || codeCount == 0) {
        return 0;
    }

    // Header is fixed width: '{' + 2 + ';' + 2 + ';' + 2 = 9 characters, and the length field is
    // written last because it counts characters that do not exist yet.
    constexpr size_t kHeader = 9;
    constexpr size_t kTail   = 5;  // '|' is inside the payload; this is 4 checksum digits + '}'

    size_t payload = sizeof(kPayloadMarker) - 1;  // "|64:"
    for (size_t i = 0; i < codeCount; ++i) {
        if (codes[i] == nullptr) return 0;
        const size_t len = std::strlen(codes[i]);
        if (len == 0 || len > kMaxCodeLength) return 0;
        payload += len + (i == 0 ? 0 : 1);  // separating ';' before every code but the first
    }
    ++payload;  // the '|' that closes the payload

    const size_t total = kHeader + payload + kTail;
    // The length field is two hex digits, so a frame longer than 255 characters cannot describe
    // itself. Refuse rather than emit one whose header lies about it.
    if (total > outSize || total > 0xFF || total > kMaxFrame) {
        return 0;
    }

    size_t n = 0;
    out[n++] = kOpen;
    writeHex(out + n, kHostAddress, 2);
    n += 2;
    out[n++] = ';';
    writeHex(out + n, destination, 2);
    n += 2;
    out[n++] = ';';
    writeHex(out + n, static_cast<uint32_t>(total), 2);
    n += 2;

    std::memcpy(out + n, kPayloadMarker, sizeof(kPayloadMarker) - 1);
    n += sizeof(kPayloadMarker) - 1;
    for (size_t i = 0; i < codeCount; ++i) {
        if (i != 0) out[n++] = ';';
        const size_t len = std::strlen(codes[i]);
        std::memcpy(out + n, codes[i], len);
        n += len;
    }
    out[n++] = '|';

    // Everything after the opening brace, up to and including that '|'.
    const uint16_t sum = checksum(out + 1, n - 1);
    writeHex(out + n, sum, 4);
    n += 4;
    out[n++] = kClose;
    return n;
}

size_t frameLength(const char* buffer, size_t length) {
    if (buffer == nullptr) return 0;
    for (size_t i = 0; i < length; ++i) {
        if (buffer[i] == kClose) {
            return i + 1;
        }
    }
    return 0;
}

ParseResult parseReply(const char* frame, size_t length, uint8_t expectedSender, Reading* out,
                       size_t outCapacity, size_t& outCount) {
    outCount = 0;
    if (frame == nullptr || length == 0) return ParseResult::Incomplete;
    if (frame[0] != kOpen) return ParseResult::NotAFrame;
    if (frame[length - 1] != kClose) return ParseResult::Incomplete;
    // '{' + 2 + ';' + 2 + ';' + 2 + "|64:" + '|' + 4 + '}' is 19 with an empty payload, and an
    // empty payload is itself malformed. Stated exactly rather than one short: a bound that is
    // loose by one is only harmless by accident of what the later checks happen to reject.
    if (length < 19) return ParseResult::Malformed;

    // Header: sender;recipient;length
    uint32_t sender = 0, recipient = 0, declared = 0;
    if (parseHex(frame + 1, 2, 2, sender) != 2 || frame[3] != ';') return ParseResult::Malformed;
    if (parseHex(frame + 4, 2, 2, recipient) != 2 || frame[6] != ';') return ParseResult::Malformed;
    if (parseHex(frame + 7, 2, 2, declared) != 2) return ParseResult::Malformed;

    // Checked before the checksum on purpose: a frame whose header lies about its own size is a
    // different fault from a corrupted one, and reporting it as a checksum error would send
    // somebody looking at their cabling.
    if (declared != length) return ParseResult::LengthMismatch;

    const size_t markerAt = 9;
    if (length < markerAt + sizeof(kPayloadMarker) - 1) return ParseResult::Malformed;
    if (std::memcmp(frame + markerAt, kPayloadMarker, sizeof(kPayloadMarker) - 1) != 0) {
        return ParseResult::Malformed;
    }

    // The checksum field is the last four characters before '}', preceded by the closing '|'.
    const size_t checksumAt = length - 5;
    if (frame[checksumAt - 1] != '|') return ParseResult::Malformed;
    uint32_t declaredSum = 0;
    if (parseHex(frame + checksumAt, 4, 4, declaredSum) != 4) return ParseResult::Malformed;
    if (checksum(frame + 1, checksumAt - 1) != declaredSum) return ParseResult::BadChecksum;

    // Only now, with the frame proven intact, does the sender matter. A corrupted frame that
    // happens to carry someone else's address should be reported as corrupt, not as somebody
    // else's traffic.
    if (sender != expectedSender) return ParseResult::WrongSender;
    (void)recipient;

    const size_t payloadStart = markerAt + sizeof(kPayloadMarker) - 1;
    const size_t payloadEnd   = checksumAt - 1;  // the closing '|'
    size_t       p            = payloadStart;
    while (p < payloadEnd) {
        // CODE
        size_t codeLen = 0;
        while (p + codeLen < payloadEnd && frame[p + codeLen] != '=' &&
               frame[p + codeLen] != ';') {
            ++codeLen;
        }
        if (codeLen == 0 || codeLen > kMaxCodeLength) return ParseResult::Malformed;
        if (p + codeLen >= payloadEnd || frame[p + codeLen] != '=') return ParseResult::Malformed;

        Reading r;
        std::memcpy(r.code, frame + p, codeLen);
        r.code[codeLen] = '\0';
        p += codeLen + 1;

        const size_t consumed = parseHex(frame + p, payloadEnd - p, 8, r.value);
        if (consumed == 0) return ParseResult::Malformed;
        p += consumed;

        if (outCount >= outCapacity) return ParseResult::TooManyReadings;
        out[outCount++] = r;

        if (p < payloadEnd) {
            if (frame[p] != ';') return ParseResult::Malformed;
            ++p;
            // A trailing separator with nothing after it is malformed, not an empty reading.
            if (p >= payloadEnd) return ParseResult::Malformed;
        }
    }

    return ParseResult::Ok;
}

const Reading* find(const Reading* readings, size_t count, const char* code) {
    if (readings == nullptr || code == nullptr) return nullptr;
    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(readings[i].code, code) == 0) return &readings[i];
    }
    return nullptr;
}

}  // namespace heliograph::maxtalk
