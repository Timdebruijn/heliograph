// SPDX-License-Identifier: MIT
// SHA-256 against published vectors.
//
// This is the one piece of the update path where being wrong is silent: a subtly broken hash
// accepts a corrupted image instead of refusing it, and nothing in normal operation would show
// it. So it is checked against values nobody here produced -- FIPS 180-4's own examples and the
// two digests every implementation is tested with -- rather than against itself.

#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "ota/sha256.h"

using heliograph::ota::Digest;
using heliograph::ota::hexDigestEquals;
using heliograph::ota::Sha256;

void setUp() {}
void tearDown() {}

static std::string hashOf(const std::string& input) {
    Sha256 h;
    h.update(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    return h.finishHex();
}

// --- published vectors ------------------------------------------------------------------------

/// The empty string. Its digest is the most widely reproduced constant in the algorithm, which
/// makes it the cheapest possible check that the initial state and the padding are both right.
static void test_the_empty_message() {
    TEST_ASSERT_EQUAL_STRING("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                             hashOf("").c_str());
}

/// FIPS 180-4, appendix B.1: the one-block example.
static void test_fips_one_block_example() {
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                             hashOf("abc").c_str());
}

/// FIPS 180-4, appendix B.2: 56 bytes, which is exactly the boundary where the length no longer
/// fits in the final block and a second padding block is required. The case a naive
/// implementation gets wrong, so it is here on purpose.
static void test_fips_two_block_example() {
    TEST_ASSERT_EQUAL_STRING(
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        hashOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").c_str());
}

/// A million 'a', the long-message vector. Exercises the streaming path over many blocks rather
/// than the single-shot one -- which is how the OTA route actually uses this, a megabyte at a
/// time in whatever chunks the TCP stack produced.
static void test_the_long_vector() {
    Sha256              h;
    const std::string   chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) {
        h.update(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size());
    }
    TEST_ASSERT_EQUAL_STRING("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                             h.finishHex().c_str());
}

// --- the streaming contract -------------------------------------------------------------------

/// Chunk boundaries must not change the answer. The OTA path is fed whatever the network hands
/// it, so anything that depended on 64-byte alignment would work in every test and fail on a
/// real upload.
static void test_the_chunking_does_not_change_the_digest() {
    const std::string message =
        "The quick brown fox jumps over the lazy dog, repeatedly, at some length.";
    const std::string expected = hashOf(message);

    for (size_t chunk : {size_t{1}, size_t{7}, size_t{63}, size_t{64}, size_t{65}, size_t{1000}}) {
        Sha256 h;
        for (size_t offset = 0; offset < message.size(); offset += chunk) {
            const size_t n = std::min(chunk, message.size() - offset);
            h.update(reinterpret_cast<const uint8_t*>(message.data() + offset), n);
        }
        TEST_ASSERT_EQUAL_STRING(expected.c_str(), h.finishHex().c_str());
    }
}

/// Every length across two block boundaries. Padding is the part that goes wrong, and it goes
/// wrong at specific lengths -- 55, 56 and 64 especially -- so this walks all of them rather
/// than sampling.
static void test_every_length_across_two_blocks_is_self_consistent() {
    for (size_t len = 0; len <= 130; ++len) {
        const std::string message(len, 'x');
        Sha256            whole;
        whole.update(reinterpret_cast<const uint8_t*>(message.data()), message.size());
        const std::string a = whole.finishHex();

        Sha256 byByte;
        for (char c : message) {
            byByte.update(reinterpret_cast<const uint8_t*>(&c), 1);
        }
        TEST_ASSERT_EQUAL_STRING_MESSAGE(a.c_str(), byByte.finishHex().c_str(),
                                         std::to_string(len).c_str());
    }
}

static void test_reset_makes_the_object_reusable() {
    Sha256 h;
    h.update(reinterpret_cast<const uint8_t*>("abc"), 3);
    h.finishHex();
    h.reset();
    h.update(reinterpret_cast<const uint8_t*>("abc"), 3);
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                             h.finishHex().c_str());
}

static void test_a_null_chunk_is_ignored_not_hashed() {
    Sha256 h;
    h.update(nullptr, 0);
    h.update(reinterpret_cast<const uint8_t*>("abc"), 3);
    h.update(nullptr, 17);  // a length with no data is still nothing
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                             h.finishHex().c_str());
}

// --- hex rendering ------------------------------------------------------------------------

/// Lowercase, two characters per byte, leading zeroes kept. A renderer that dropped the high
/// nibble of 0x0f would produce a 63-character string that no comparison could ever match, and
/// the OTA error message would show a digest that is not the one that was computed.
static void test_hex_renders_every_byte_as_two_lowercase_characters() {
    const uint8_t bytes[] = {0x00, 0x0f, 0xa5, 0xff};
    TEST_ASSERT_EQUAL_STRING("000fa5ff", heliograph::ota::toHex(bytes, sizeof(bytes)).c_str());
    TEST_ASSERT_EQUAL_STRING("", heliograph::ota::toHex(bytes, 0).c_str());
    // The length is honoured, not the array: OtaManager renders a 32-byte digest out of a
    // buffer it sizes itself, and a renderer that ignored len would read past it.
    TEST_ASSERT_EQUAL_STRING("000f", heliograph::ota::toHex(bytes, 2).c_str());
}

// --- the comparison ---------------------------------------------------------------------------

static void test_a_matching_hex_digest_compares_equal() {
    Sha256 h;
    h.update(reinterpret_cast<const uint8_t*>("abc"), 3);
    Digest  digest{};
    h.finish(digest);
    TEST_ASSERT_TRUE(hexDigestEquals(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", digest));
    // Tools print these in both cases and an operator may paste either.
    TEST_ASSERT_TRUE(hexDigestEquals(
        "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", digest));
}

/// Malformed is REFUSED, not compared. A truncated expectation that happened to prefix-match
/// must not be able to pass, and a hash field somebody left empty must never mean "fine".
static void test_a_malformed_expectation_is_refused() {
    Sha256 h;
    h.update(reinterpret_cast<const uint8_t*>("abc"), 3);
    Digest  digest{};
    h.finish(digest);

    TEST_ASSERT_FALSE(hexDigestEquals("", digest));
    TEST_ASSERT_FALSE(hexDigestEquals("ba7816bf", digest));  // right prefix, too short
    TEST_ASSERT_FALSE(hexDigestEquals(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015a", digest));   // 63
    TEST_ASSERT_FALSE(hexDigestEquals(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015add", digest));  // 65
    TEST_ASSERT_FALSE(hexDigestEquals(
        "zzzz16bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", digest));
}

static void test_one_wrong_bit_is_refused() {
    Sha256 h;
    h.update(reinterpret_cast<const uint8_t*>("abc"), 3);
    Digest  digest{};
    h.finish(digest);
    // Last nibble changed: the case that matters, because a comparison that stopped early
    // would accept it.
    TEST_ASSERT_FALSE(hexDigestEquals(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ae", digest));
    // ...and the first, for the same reason in the other direction.
    TEST_ASSERT_FALSE(hexDigestEquals(
        "aa7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", digest));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_the_empty_message);
    RUN_TEST(test_fips_one_block_example);
    RUN_TEST(test_fips_two_block_example);
    RUN_TEST(test_the_long_vector);
    RUN_TEST(test_the_chunking_does_not_change_the_digest);
    RUN_TEST(test_every_length_across_two_blocks_is_self_consistent);
    RUN_TEST(test_reset_makes_the_object_reusable);
    RUN_TEST(test_a_null_chunk_is_ignored_not_hashed);
    RUN_TEST(test_hex_renders_every_byte_as_two_lowercase_characters);
    RUN_TEST(test_a_matching_hex_digest_compares_equal);
    RUN_TEST(test_a_malformed_expectation_is_refused);
    RUN_TEST(test_one_wrong_bit_is_refused);
    return UNITY_END();
}
