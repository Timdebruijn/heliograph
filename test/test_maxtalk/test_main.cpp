// SPDX-License-Identifier: MIT
//
// MaxTalk framing codec. Every frame asserted here is either captured traffic or reconstructed
// from the published rules and then checked against them -- which is stated per test, because
// the two carry very different weight.

#include <cstring>

#include <unity.h>

#include "protocols/maxtalk/maxtalk.h"

using namespace heliograph;

namespace {

/// Captured traffic, quoted byte for byte from the published reverse-engineering writeup. This
/// is the only frame here that a real device is known to have produced.
constexpr char kCapturedReply[] =
    "{05;FB;59|64:CAC=1F3E;KHR=26D6;KDY=8E;KMT=8D;KYR=221;KT0=18C7;KLD=78;KLM=F8;KLY=A8F|154C}";

/// Reconstructed: no request carrying these codes appears in any source. It is here because both
/// its declared length and its checksum were recomputed from the documented rules and agree, so
/// it is a test vector rather than evidence.
constexpr char kReconstructedRequest[] =
    "{FB;05;36|64:CAC;KHR;KDY;KMT;KYR;KT0;KLD;KLM;KLY|0D34}";

}  // namespace

void setUp() {}
void tearDown() {}

// The checksum on both published frames. If this is wrong nothing else can be right, so it is
// asserted against the numbers the sources carry rather than against our own output.
static void test_checksum_matches_the_published_frames() {
    // Everything after '{' up to and including the '|' before the four checksum digits.
    const size_t replyBody = std::strlen(kCapturedReply) - 5 - 1;
    TEST_ASSERT_EQUAL_HEX16(0x154C, maxtalk::checksum(kCapturedReply + 1, replyBody));

    const size_t requestBody = std::strlen(kReconstructedRequest) - 5 - 1;
    TEST_ASSERT_EQUAL_HEX16(0x0D34, maxtalk::checksum(kReconstructedRequest + 1, requestBody));
}

// The result is the sum reduced modulo 65536, which matters only for a body longer than any
// legal frame: the cap is 512 characters and frame bytes are all under 128, so a real frame tops
// out around 65024 and never wraps. This calls the function directly to pin the contract past
// that point.
//
// It does NOT prove that a per-iteration mask is present, and an earlier version of this test
// claimed it did. Mutation testing showed otherwise: deleting the mask changed nothing, because
// the narrowing cast at the end reduces modulo 2^16 anyway and 2^32 is a multiple of 2^16. The
// mask was removed rather than left as a line no test could fail on.
static void test_the_sum_is_reduced_modulo_sixteen_bits() {
    char big[2048];
    std::memset(big, 'z', sizeof(big));  // 'z' is 122; 2048 * 122 = 249856, well past 0xFFFF
    TEST_ASSERT_EQUAL_HEX16(static_cast<uint16_t>(2048u * 122u), maxtalk::checksum(big, sizeof(big)));

    // A length that wraps exactly, so an off-by-one in the reduction would show.
    char exact[512];
    std::memset(exact, 0x80, sizeof(exact));  // 512 * 128 = 65536 -> 0
    TEST_ASSERT_EQUAL_HEX16(0, maxtalk::checksum(exact, sizeof(exact)));
}

// Building the request for the captured reply's codes reproduces the reconstructed frame exactly
// -- header, length field, payload marker, separators and checksum.
static void test_building_a_request_reproduces_the_known_frame() {
    const char* codes[] = {"CAC", "KHR", "KDY", "KMT", "KYR", "KT0", "KLD", "KLM", "KLY"};
    char        out[maxtalk::kMaxFrame];
    const size_t n = maxtalk::buildRequest(0x05, codes, 9, out, sizeof(out));

    TEST_ASSERT_EQUAL_size_t(std::strlen(kReconstructedRequest), n);
    TEST_ASSERT_EQUAL_MEMORY(kReconstructedRequest, out, n);
}

// The length field counts the whole frame including both braces. Asserted separately from the
// byte comparison above because a wrong length is the failure that looks like a dead device:
// the inverter ignores the frame rather than rejecting it, so nothing comes back and nothing
// says why.
static void test_the_declared_length_counts_the_whole_frame() {
    const char*  codes[] = {"PAC"};
    char         out[maxtalk::kMaxFrame];
    const size_t n = maxtalk::buildRequest(0x05, codes, 1, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);

    // Two hex digits at offset 7.
    const int hi   = out[7] <= '9' ? out[7] - '0' : out[7] - 'A' + 10;
    const int lo   = out[8] <= '9' ? out[8] - '0' : out[8] - 'A' + 10;
    const size_t declared = static_cast<size_t>(hi * 16 + lo);
    TEST_ASSERT_EQUAL_size_t(n, declared);
}

static void test_parsing_the_captured_reply_yields_every_pair() {
    maxtalk::Reading readings[16];
    size_t           count = 0;
    const auto       r = maxtalk::parseReply(kCapturedReply, std::strlen(kCapturedReply), 0x05,
                                             readings, 16, count);

    TEST_ASSERT_EQUAL(maxtalk::ParseResult::Ok, r);
    TEST_ASSERT_EQUAL_size_t(9, count);

    const auto* kdy = maxtalk::find(readings, count, "KDY");
    TEST_ASSERT_NOT_NULL(kdy);
    TEST_ASSERT_EQUAL_UINT32(0x8E, kdy->value);

    const auto* kt0 = maxtalk::find(readings, count, "KT0");
    TEST_ASSERT_NOT_NULL(kt0);
    TEST_ASSERT_EQUAL_UINT32(0x18C7, kt0->value);

    // Values of differing digit width in one frame: 8E is two, 18C7 is four, 221 is three.
    const auto* kyr = maxtalk::find(readings, count, "KYR");
    TEST_ASSERT_NOT_NULL(kyr);
    TEST_ASSERT_EQUAL_UINT32(0x221, kyr->value);

    // A code the device did not answer is absent, not zero.
    TEST_ASSERT_NULL(maxtalk::find(readings, count, "PAC"));
}

static void test_four_character_codes_round_trip() {
    const char*  codes[] = {"UD01", "ID01"};
    char         out[maxtalk::kMaxFrame];
    const size_t n = maxtalk::buildRequest(0x07, codes, 2, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);

    // Build a reply with the same codes, so the parser is exercised on four-character names.
    char         reply[maxtalk::kMaxFrame];
    const char   body[] = "|64:UD01=0BB8;ID01=01F4|";
    const size_t total  = 9 + (sizeof(body) - 1) + 5;
    int          w      = 0;
    reply[w++]          = '{';
    reply[w++] = '0'; reply[w++] = '7'; reply[w++] = ';';
    reply[w++] = 'F'; reply[w++] = 'B'; reply[w++] = ';';
    reply[w++] = static_cast<char>(total / 16 < 10 ? '0' + total / 16 : 'A' + total / 16 - 10);
    reply[w++] = static_cast<char>(total % 16 < 10 ? '0' + total % 16 : 'A' + total % 16 - 10);
    std::memcpy(reply + w, body, sizeof(body) - 1);
    w += static_cast<int>(sizeof(body) - 1);
    const uint16_t sum = maxtalk::checksum(reply + 1, static_cast<size_t>(w) - 1);
    for (int i = 0; i < 4; ++i) {
        const uint8_t nib = (sum >> (12 - 4 * i)) & 0xF;
        reply[w++] = static_cast<char>(nib < 10 ? '0' + nib : 'A' + nib - 10);
    }
    reply[w++] = '}';

    maxtalk::Reading readings[4];
    size_t           count = 0;
    TEST_ASSERT_EQUAL(maxtalk::ParseResult::Ok,
                      maxtalk::parseReply(reply, static_cast<size_t>(w), 0x07, readings, 4, count));
    TEST_ASSERT_EQUAL_size_t(2, count);
    TEST_ASSERT_EQUAL_STRING("UD01", readings[0].code);
    TEST_ASSERT_EQUAL_UINT32(0x0BB8, readings[0].value);
    TEST_ASSERT_EQUAL_UINT32(0x01F4, readings[1].value);
}

// Each failure gets its own verdict rather than one "bad frame", because they point at different
// causes: a checksum error indicts the wiring, a length mismatch indicts the device, and a frame
// from another address is not a fault at all.
static void test_a_corrupted_frame_is_reported_as_a_checksum_error() {
    char copy[128];
    std::strcpy(copy, kCapturedReply);
    copy[20] = (copy[20] == '1') ? '2' : '1';  // flip a payload digit, leave the structure intact

    maxtalk::Reading readings[16];
    size_t           count = 0;
    TEST_ASSERT_EQUAL(maxtalk::ParseResult::BadChecksum,
                      maxtalk::parseReply(copy, std::strlen(copy), 0x05, readings, 16, count));
}

static void test_a_header_that_lies_about_its_length_is_not_a_checksum_error() {
    char copy[128];
    std::strcpy(copy, kCapturedReply);
    copy[7] = '4';  // declared length 0x49 instead of 0x59

    maxtalk::Reading readings[16];
    size_t           count = 0;
    TEST_ASSERT_EQUAL(maxtalk::ParseResult::LengthMismatch,
                      maxtalk::parseReply(copy, std::strlen(copy), 0x05, readings, 16, count));
}

// On a shared bus, another device's answer is ordinary traffic. A driver polling unit 6 must not
// record unit 5's perfectly good reply as its own, nor count it as an error against the wire.
static void test_a_reply_from_another_device_is_not_decoded() {
    maxtalk::Reading readings[16];
    size_t           count = 0;
    TEST_ASSERT_EQUAL(maxtalk::ParseResult::WrongSender,
                      maxtalk::parseReply(kCapturedReply, std::strlen(kCapturedReply), 0x06,
                                          readings, 16, count));
    TEST_ASSERT_EQUAL_size_t(0, count);
}

// Our device, answering somebody ELSE. On a bus with a second querying host that frame is real
// traffic from the right inverter -- and its numbers answer a question we did not ask.
static void test_a_frame_addressed_to_another_host_is_not_decoded() {
    char copy[128];
    std::strcpy(copy, kCapturedReply);
    copy[4] = 'A';  // recipient AB instead of FB
    copy[5] = 'B';
    // Re-checksum so the frame is otherwise perfect and the recipient check is what rejects it.
    const size_t at = std::strlen(copy) - 5;
    const uint16_t sum = maxtalk::checksum(copy + 1, at - 1);
    for (int i = 0; i < 4; ++i) {
        const uint8_t nib = (sum >> (12 - 4 * i)) & 0xF;
        copy[at + i] = static_cast<char>(nib < 10 ? '0' + nib : 'A' + nib - 10);
    }

    maxtalk::Reading readings[16];
    size_t           count = 0;
    TEST_ASSERT_EQUAL(maxtalk::ParseResult::WrongRecipient,
                      maxtalk::parseReply(copy, std::strlen(copy), 0x05, readings, 16, count));
    TEST_ASSERT_EQUAL_size_t(0, count);
}

static void test_an_unterminated_frame_asks_for_more_bytes() {
    char partial[64];
    std::memcpy(partial, kCapturedReply, 40);

    maxtalk::Reading readings[16];
    size_t           count = 0;
    TEST_ASSERT_EQUAL(maxtalk::ParseResult::Incomplete,
                      maxtalk::parseReply(partial, 40, 0x05, readings, 16, count));

    // And the framing helper agrees there is nothing complete yet.
    TEST_ASSERT_EQUAL_size_t(0, maxtalk::frameLength(partial, 40));
}

static void test_frame_length_finds_the_boundary_in_a_longer_buffer() {
    char buf[256];
    const size_t n = std::strlen(kCapturedReply);
    std::memcpy(buf, kCapturedReply, n);
    std::memcpy(buf + n, "{05;FB;", 7);  // the start of the next frame

    TEST_ASSERT_EQUAL_size_t(n, maxtalk::frameLength(buf, n + 7));
}

/// Builds an otherwise-perfect reply -- correct declared length, correct checksum -- around a
/// caller-chosen marker and payload. Both callers below need to vary exactly one thing and keep
/// every other check satisfied, or the parser refuses the frame before reaching what is under
/// test. That is the failure this suite has already been bitten by once.
static size_t buildReply(const char* marker, const char* payload, char* frame) {
    const size_t total = 9 + (std::strlen(marker) + std::strlen(payload) + 1) + 5;
    int          w     = 0;
    frame[w++] = '{';
    frame[w++] = '0'; frame[w++] = '5'; frame[w++] = ';';
    frame[w++] = 'F'; frame[w++] = 'B'; frame[w++] = ';';
    frame[w++] = static_cast<char>(total / 16 < 10 ? '0' + total / 16 : 'A' + total / 16 - 10);
    frame[w++] = static_cast<char>(total % 16 < 10 ? '0' + total % 16 : 'A' + total % 16 - 10);
    std::memcpy(frame + w, marker, std::strlen(marker));
    w += static_cast<int>(std::strlen(marker));
    std::memcpy(frame + w, payload, std::strlen(payload));
    w += static_cast<int>(std::strlen(payload));
    frame[w++] = '|';
    const uint16_t sum = maxtalk::checksum(frame + 1, static_cast<size_t>(w) - 1);
    for (int i = 0; i < 4; ++i) {
        const uint8_t nib = (sum >> (12 - 4 * i)) & 0xF;
        frame[w++] = static_cast<char>(nib < 10 ? '0' + nib : 'A' + nib - 10);
    }
    frame[w++] = '}';
    return static_cast<size_t>(w);
}

// No test ever corrupted the "|64:" marker, so deleting the memcmp that checks it left the
// suite green. A frame whose marker is garbled or shifted by a byte would then be parsed from
// the wrong offset -- and everything after it is read as code=value pairs, so the failure is
// not "no reading" but plausible readings taken from the wrong place in the frame.
static void test_a_frame_whose_payload_marker_is_wrong_is_refused() {
    const char* markers[] = {"|65:", "|64;", "@64:", "|640", "::::"};
    for (const char* m : markers) {
        char         frame[maxtalk::kMaxFrame];
        const size_t n = buildReply(m, "CAC=1F3E", frame);

        maxtalk::Reading readings[4];
        size_t           count = 0;
        TEST_ASSERT_EQUAL_MESSAGE(maxtalk::ParseResult::Malformed,
                                  maxtalk::parseReply(frame, n, 0x05, readings, 4, count), m);
    }
}

// The codec writes uppercase, so every fixture in this suite is uppercase and the lowercase
// branch of hexValue() was never executed -- deleting it changed nothing. The vendor is gone
// and there is no second implementation to compare against, so "our own encoder never emits
// it" is not evidence about what a device emits. The branch exists; this is what it claims.
static void test_a_lowercase_hex_value_decodes_the_same_as_uppercase() {
    char         upper[maxtalk::kMaxFrame];
    char         lower[maxtalk::kMaxFrame];
    const size_t nu = buildReply("|64:", "CAC=1F3E", upper);
    const size_t nl = buildReply("|64:", "CAC=1f3e", lower);

    maxtalk::Reading ru[4], rl[4];
    size_t           cu = 0, cl = 0;
    TEST_ASSERT_EQUAL(maxtalk::ParseResult::Ok, maxtalk::parseReply(upper, nu, 0x05, ru, 4, cu));
    TEST_ASSERT_EQUAL(maxtalk::ParseResult::Ok, maxtalk::parseReply(lower, nl, 0x05, rl, 4, cl));
    TEST_ASSERT_EQUAL_UINT32(1, cu);
    TEST_ASSERT_EQUAL_UINT32(1, cl);
    TEST_ASSERT_EQUAL_UINT32(0x1F3E, ru[0].value);
    TEST_ASSERT_EQUAL_UINT32(ru[0].value, rl[0].value);
}

static void test_a_malformed_payload_is_refused_rather_than_half_decoded() {
    // The frame must be otherwise PERFECT -- correct length, correct checksum -- or the parser
    // rejects it earlier and the payload logic is never reached. An earlier version of this test
    // used a placeholder checksum and therefore asserted nothing about the payload at all:
    // deleting the pair validation entirely would not have failed it. Found by review.
    struct Case {
        const char* payload;
        const char* what;
    };
    const Case cases[] = {
        {"CAC", "a code with no '=' and no value"},
        {"CAC=", "a code with '=' and nothing after it"},
        {"=1F3E", "a value with no code"},
        {"CAC=1F3E;", "a trailing separator with nothing after it"},
        {";", "a lone separator"},
        {"TOOLONG=1", "a code longer than the protocol allows"},
    };

    for (const auto& c : cases) {
        char         frame[maxtalk::kMaxFrame];
        const size_t total = 9 + (std::strlen(c.payload) + 5) + 5;
        int          w     = 0;
        frame[w++] = '{';
        frame[w++] = '0'; frame[w++] = '5'; frame[w++] = ';';
        frame[w++] = 'F'; frame[w++] = 'B'; frame[w++] = ';';
        frame[w++] = static_cast<char>(total / 16 < 10 ? '0' + total / 16 : 'A' + total / 16 - 10);
        frame[w++] = static_cast<char>(total % 16 < 10 ? '0' + total % 16 : 'A' + total % 16 - 10);
        std::memcpy(frame + w, "|64:", 4);
        w += 4;
        std::memcpy(frame + w, c.payload, std::strlen(c.payload));
        w += static_cast<int>(std::strlen(c.payload));
        frame[w++] = '|';
        const uint16_t sum = maxtalk::checksum(frame + 1, static_cast<size_t>(w) - 1);
        for (int i = 0; i < 4; ++i) {
            const uint8_t nib = (sum >> (12 - 4 * i)) & 0xF;
            frame[w++] = static_cast<char>(nib < 10 ? '0' + nib : 'A' + nib - 10);
        }
        frame[w++] = '}';

        maxtalk::Reading readings[4];
        size_t           count = 0;
        const auto r = maxtalk::parseReply(frame, static_cast<size_t>(w), 0x05, readings, 4, count);
        TEST_ASSERT_EQUAL_MESSAGE(maxtalk::ParseResult::Malformed, r, c.what);
    }
}

// The readings written before the buffer filled stay valid: a caller that asked for more than it
// had room for should be able to use what arrived rather than lose the whole poll.
static void test_running_out_of_room_keeps_what_was_already_decoded() {
    maxtalk::Reading readings[3];
    size_t           count = 0;
    TEST_ASSERT_EQUAL(maxtalk::ParseResult::TooManyReadings,
                      maxtalk::parseReply(kCapturedReply, std::strlen(kCapturedReply), 0x05,
                                          readings, 3, count));
    TEST_ASSERT_EQUAL_size_t(3, count);
    TEST_ASSERT_EQUAL_STRING("CAC", readings[0].code);
    TEST_ASSERT_EQUAL_UINT32(0x1F3E, readings[0].value);
}

// A truncated request is worse than no request: it is a syntactically valid frame asking for
// something nobody wanted, and its checksum would be correct for that.
static void test_a_request_that_does_not_fit_is_refused_rather_than_truncated() {
    const char* codes[] = {"CAC", "KHR", "KDY"};
    char        tiny[20];
    TEST_ASSERT_EQUAL_size_t(0, maxtalk::buildRequest(0x05, codes, 3, tiny, sizeof(tiny)));
}

static void test_a_request_is_refused_for_codes_that_cannot_exist() {
    char out[maxtalk::kMaxFrame];
    const char* tooLong[] = {"TOOLONG"};
    TEST_ASSERT_EQUAL_size_t(0, maxtalk::buildRequest(0x05, tooLong, 1, out, sizeof(out)));
    const char* empty[] = {""};
    TEST_ASSERT_EQUAL_size_t(0, maxtalk::buildRequest(0x05, empty, 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, maxtalk::buildRequest(0x05, nullptr, 1, out, sizeof(out)));
}

// Whatever buildRequest emits, parseReply must accept the same shape back. This is the guard
// against the two halves drifting apart: a change to the header layout that updated only one
// side would pass every fixed-frame test above and fail here.
static void test_the_two_halves_agree_on_the_frame_layout() {
    const char*  codes[] = {"PAC", "UDC"};
    char         request[maxtalk::kMaxFrame];
    const size_t n = maxtalk::buildRequest(0x0A, codes, 2, request, sizeof(request));
    TEST_ASSERT_TRUE(n > 0);

    // Turn the request into a reply by swapping the addresses and giving each code a value.
    // The result must satisfy the parser, including its checksum.
    TEST_ASSERT_EQUAL_size_t(n, maxtalk::frameLength(request, n));
    TEST_ASSERT_EQUAL('{', request[0]);
    TEST_ASSERT_EQUAL('}', request[n - 1]);
    TEST_ASSERT_EQUAL_MEMORY("FB;0A;", request + 1, 6);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_checksum_matches_the_published_frames);
    RUN_TEST(test_the_sum_is_reduced_modulo_sixteen_bits);
    RUN_TEST(test_building_a_request_reproduces_the_known_frame);
    RUN_TEST(test_the_declared_length_counts_the_whole_frame);
    RUN_TEST(test_parsing_the_captured_reply_yields_every_pair);
    RUN_TEST(test_four_character_codes_round_trip);
    RUN_TEST(test_a_corrupted_frame_is_reported_as_a_checksum_error);
    RUN_TEST(test_a_header_that_lies_about_its_length_is_not_a_checksum_error);
    RUN_TEST(test_a_reply_from_another_device_is_not_decoded);
    RUN_TEST(test_a_frame_addressed_to_another_host_is_not_decoded);
    RUN_TEST(test_an_unterminated_frame_asks_for_more_bytes);
    RUN_TEST(test_frame_length_finds_the_boundary_in_a_longer_buffer);
    RUN_TEST(test_a_malformed_payload_is_refused_rather_than_half_decoded);
    RUN_TEST(test_a_frame_whose_payload_marker_is_wrong_is_refused);
    RUN_TEST(test_a_lowercase_hex_value_decodes_the_same_as_uppercase);
    RUN_TEST(test_running_out_of_room_keeps_what_was_already_decoded);
    RUN_TEST(test_a_request_that_does_not_fit_is_refused_rather_than_truncated);
    RUN_TEST(test_a_request_is_refused_for_codes_that_cannot_exist);
    RUN_TEST(test_the_two_halves_agree_on_the_frame_layout);
    return UNITY_END();
}
