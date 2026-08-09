// SPDX-License-Identifier: MIT
//
// MaxTalk RS485 protocol — framing layer, shared by every driver that speaks it.
//
// An ASCII request/response protocol used by a family of legacy string inverters, at 19200 8N1.
// A frame is brace-delimited and carries named three- or four-character query codes rather than
// register numbers:
//
//     request  {FB;05;36|64:CAC;KHR;KDY;KMT;KYR;KT0;KLD;KLM;KLY|0D34}
//     reply    {05;FB;59|64:CAC=1F3E;KHR=26D6;KDY=8E;...|154C}
//
// This file is the framing only. What a code MEANS, and what to divide its value by, is brand
// knowledge and stays in a driver under src/drivers/ — which is also why no brand is named here,
// exactly as in the PMU family beside it.
//
// The protocol was re-implemented from a published reverse-engineering writeup and an
// independent open-source implementation; no code was copied. See the protocol notes under
// docs/ for which claims those two sources agree on and which rest on one.
//
// This translation unit is platform independent: it must never include Arduino or ESP-IDF
// headers, so the whole codec is tested on the host (env:native).

#pragma once

#include <cstddef>
#include <cstdint>

namespace heliograph::maxtalk {

/// The address a querying computer uses for itself. Devices are addressed 0x00-0xFF, set in the
/// inverter's own display menu -- there is no registration handshake and nothing is assigned over
/// the wire, so a probe changes no state on the bus.
inline constexpr uint8_t kHostAddress = 0xFB;

/// Codes are three or four characters (`PAC`, `KDY`, `UD01`).
inline constexpr size_t kMaxCodeLength = 4;

/// Generous: a real frame asking for a dozen codes is around 100 bytes. The cap exists so a bus
/// emitting noise cannot make the reader grow a buffer without bound.
inline constexpr size_t kMaxFrame = 512;

/// One decoded pair from a reply. `value` is the raw hex as transmitted, unscaled and
/// uninterpreted: this layer does not know that one code is milliwatts and another is a count.
struct Reading {
    char     code[kMaxCodeLength + 1] = {};
    uint32_t value                    = 0;
};

enum class ParseResult : uint8_t {
    Ok,
    /// No complete frame in the buffer yet. The caller should read more rather than give up.
    Incomplete,
    /// Bytes arrived but they are not a frame at all -- no opening brace where one must be.
    NotAFrame,
    /// Structurally a frame, but the checksum does not match: the wire corrupted it.
    BadChecksum,
    /// The declared length disagrees with the actual frame. Kept apart from BadChecksum because
    /// it points at a different fault: a device that builds frames wrongly, not a noisy line.
    LengthMismatch,
    /// A well-formed frame from someone else. On a shared bus this is normal traffic, not an
    /// error, and a caller polling several devices must not count it as one.
    WrongSender,
    /// The frame parsed but its payload does not follow `CODE=HEX` pairs.
    Malformed,
    /// More pairs than the caller made room for. The readings already written stay valid.
    TooManyReadings,
};

const char* parseResultName(ParseResult result);

/// The 16-bit sum used by this protocol: every character between the opening brace and the
/// checksum field, added up, truncated to 16 bits.
///
/// Truncation is explicit rather than incidental. Frames are short enough today that a 32-bit
/// accumulator would give the same answer, so an implementation that forgot to mask would pass
/// every test written against real traffic and diverge only on a frame longer than any device
/// currently sends.
uint16_t checksum(const char* body, size_t length);

/// Builds a request. Returns the number of characters written, or 0 when the codes do not fit
/// in `outSize` -- never a truncated frame, which would be a valid-looking request for something
/// nobody asked for.
///
/// `out` is NOT null-terminated; the return value is the length.
size_t buildRequest(uint8_t destination, const char* const* codes, size_t codeCount, char* out,
                    size_t outSize);

/// Length of the first complete frame in `buffer`, or 0 if there is not one yet.
///
/// This is the whole of framing for this protocol, and it is why the codec needs no timing rule:
/// a frame ends at `}`, so a reader knows it has one without measuring silence between bytes and
/// without trusting the declared length first. The transport contract puts frame delimiting on
/// the caller, and this is the helper for it.
size_t frameLength(const char* buffer, size_t length);

/// Decodes a complete frame into readings.
///
/// `expectedSender` is the device address the caller asked; a frame from any other address
/// returns WrongSender rather than being decoded, so traffic between two other parties on the
/// same bus can never be mistaken for an answer.
ParseResult parseReply(const char* frame, size_t length, uint8_t expectedSender, Reading* out,
                       size_t outCapacity, size_t& outCount);

/// Finds one code in a decoded set. Returns nullptr when the device did not answer it -- which
/// is normal: a device answers only the codes it implements, and asking for one it does not have
/// is how a driver discovers that.
const Reading* find(const Reading* readings, size_t count, const char* code);

}  // namespace heliograph::maxtalk
