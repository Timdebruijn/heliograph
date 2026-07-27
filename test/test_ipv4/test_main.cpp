// SPDX-License-Identifier: MIT
// IPv4 parsing and subnet arithmetic.
//
// These rules decide whether a bridge is still reachable after a reboot, so they are checked
// here rather than trusted. The parse is deliberately stricter than the C library's: every
// shorthand inet_aton() accepts is a way to type something that reads like one address and
// configures another, and the symptom of that is a device nobody can reach.

#include <unity.h>

#include <cstring>
#include <string>

#include "network/ipv4.h"

using namespace heliograph::net;

void setUp() {}
void tearDown() {}

static uint32_t ip(const char* text) {
    uint32_t v = 0;
    TEST_ASSERT_TRUE_MESSAGE(parseIpv4(text, v), text);
    return v;
}

// --- parsing ----------------------------------------------------------------------------------

static void test_the_ordinary_forms_parse() {
    TEST_ASSERT_EQUAL_HEX32(0xC0A80101u, ip("192.168.1.1"));
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, ip("0.0.0.0"));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, ip("255.255.255.255"));
    TEST_ASSERT_EQUAL_HEX32(0x0A140018u, ip("10.20.0.24"));
}

/// Everything inet_aton() would have accepted, refused. Each of these reads like an address a
/// person meant to type and resolves to a different one.
static void test_the_shorthands_the_c_library_accepts_are_refused() {
    uint32_t v = 0;
    TEST_ASSERT_FALSE_MESSAGE(parseIpv4("10.1", v), "two octets");
    TEST_ASSERT_FALSE_MESSAGE(parseIpv4("10.0.1", v), "three octets");
    TEST_ASSERT_FALSE_MESSAGE(parseIpv4("1.2.3.4.5", v), "five octets");
    TEST_ASSERT_FALSE_MESSAGE(parseIpv4("0x0A.1.1.1", v), "hex octet");
    // The one that bites hardest: 010 is 8 to some tools and 10 to others.
    TEST_ASSERT_FALSE_MESSAGE(parseIpv4("010.1.1.1", v), "leading zero");
    TEST_ASSERT_FALSE_MESSAGE(parseIpv4("192.168.001.1", v), "leading zero mid-address");
}

static void test_malformed_input_is_refused() {
    uint32_t v = 0;
    for (const char* bad : {"", ".", "1.2.3.", "1.2.3.4.", ".1.2.3.4", "1.2..4", "256.1.1.1",
                            "1.2.3.256", "-1.2.3.4", "1.2.3.4 ", " 1.2.3.4", "1.2.3.4:80",
                            "192.168.1.1/24", "hostname", "pool.ntp.org", "::1"}) {
        TEST_ASSERT_FALSE_MESSAGE(parseIpv4(bad, v), bad);
    }
}

/// 255 is the boundary the octet check sits on, so both sides of it are pinned.
static void test_the_octet_boundary() {
    uint32_t v = 0;
    TEST_ASSERT_TRUE(parseIpv4("255.255.255.255", v));
    TEST_ASSERT_FALSE(parseIpv4("256.255.255.255", v));
    TEST_ASSERT_FALSE(parseIpv4("255.255.255.256", v));
    // A value that overflows only after several digits must not wrap into range.
    TEST_ASSERT_FALSE(parseIpv4("99999.1.1.1", v));
}

static void test_format_round_trips_the_parse() {
    for (const char* text : {"0.0.0.0", "192.168.1.1", "255.255.255.255", "10.20.0.24"}) {
        TEST_ASSERT_EQUAL_STRING(text, formatIpv4(ip(text)).c_str());
    }
}

// --- masks ------------------------------------------------------------------------------------

static void test_contiguous_masks_are_accepted() {
    for (const char* text : {"255.0.0.0", "255.255.0.0", "255.255.255.0", "255.255.255.252",
                             "255.255.254.0", "128.0.0.0"}) {
        TEST_ASSERT_TRUE_MESSAGE(isContiguousMask(ip(text)), text);
    }
}

static void test_a_mask_with_a_hole_is_refused() {
    for (const char* text : {"255.0.255.0", "255.255.0.255", "0.255.255.255", "255.255.255.253"}) {
        TEST_ASSERT_FALSE_MESSAGE(isContiguousMask(ip(text)), text);
    }
}

/// Both degenerate ends. 0.0.0.0 would put every address in one subnet and make every check
/// below vacuously true; 255.255.255.255 leaves no host addresses at all.
static void test_the_degenerate_masks_are_refused() {
    TEST_ASSERT_FALSE(isContiguousMask(ip("0.0.0.0")));
    TEST_ASSERT_FALSE(isContiguousMask(ip("255.255.255.255")));
}

static void test_prefix_lengths() {
    TEST_ASSERT_EQUAL_UINT8(8, maskPrefixLength(ip("255.0.0.0")));
    TEST_ASSERT_EQUAL_UINT8(24, maskPrefixLength(ip("255.255.255.0")));
    TEST_ASSERT_EQUAL_UINT8(30, maskPrefixLength(ip("255.255.255.252")));
    TEST_ASSERT_EQUAL_UINT8(23, maskPrefixLength(ip("255.255.254.0")));
}

// --- subnet arithmetic ------------------------------------------------------------------------

static void test_same_subnet() {
    const uint32_t mask = ip("255.255.255.0");
    TEST_ASSERT_TRUE(sameSubnet(ip("192.168.1.10"), ip("192.168.1.1"), mask));
    TEST_ASSERT_FALSE(sameSubnet(ip("192.168.1.10"), ip("192.168.2.1"), mask));
    // A wider mask puts them together again -- the case a /23 network actually has.
    TEST_ASSERT_TRUE(sameSubnet(ip("192.168.1.10"), ip("192.168.0.1"), ip("255.255.254.0")));
}

static void test_network_and_broadcast_are_recognised() {
    const uint32_t mask = ip("255.255.255.0");
    TEST_ASSERT_TRUE(isNetworkAddress(ip("192.168.1.0"), mask));
    TEST_ASSERT_FALSE(isNetworkAddress(ip("192.168.1.1"), mask));
    TEST_ASSERT_TRUE(isBroadcastAddress(ip("192.168.1.255"), mask));
    TEST_ASSERT_FALSE(isBroadcastAddress(ip("192.168.1.254"), mask));
    // On a /30 the usable range is two addresses, and both ends are unusable.
    const uint32_t p30 = ip("255.255.255.252");
    TEST_ASSERT_TRUE(isNetworkAddress(ip("10.0.0.4"), p30));
    TEST_ASSERT_TRUE(isBroadcastAddress(ip("10.0.0.7"), p30));
    TEST_ASSERT_FALSE(isNetworkAddress(ip("10.0.0.5"), p30));
    TEST_ASSERT_FALSE(isBroadcastAddress(ip("10.0.0.6"), p30));
}

// --- the literal-vs-name distinction ----------------------------------------------------------

/// Drives the rule that a static configuration with no DNS must not be accepted while something
/// is still configured by name. Anything not a literal counts as a name, which is the
/// conservative direction: it demands a resolver rather than assuming none is needed.
static void test_names_are_not_mistaken_for_addresses() {
    TEST_ASSERT_TRUE(looksLikeIpLiteral("192.168.1.10"));
    TEST_ASSERT_FALSE(looksLikeIpLiteral("pool.ntp.org"));
    TEST_ASSERT_FALSE(looksLikeIpLiteral("mqtt.local"));
    TEST_ASSERT_FALSE(looksLikeIpLiteral("homeassistant"));
    TEST_ASSERT_FALSE(looksLikeIpLiteral(""));
    // A name that begins like an address is still a name.
    TEST_ASSERT_FALSE(looksLikeIpLiteral("10.0.0.1.nip.io"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_the_ordinary_forms_parse);
    RUN_TEST(test_the_shorthands_the_c_library_accepts_are_refused);
    RUN_TEST(test_malformed_input_is_refused);
    RUN_TEST(test_the_octet_boundary);
    RUN_TEST(test_format_round_trips_the_parse);
    RUN_TEST(test_contiguous_masks_are_accepted);
    RUN_TEST(test_a_mask_with_a_hole_is_refused);
    RUN_TEST(test_the_degenerate_masks_are_refused);
    RUN_TEST(test_prefix_lengths);
    RUN_TEST(test_same_subnet);
    RUN_TEST(test_network_and_broadcast_are_recognised);
    RUN_TEST(test_names_are_not_mistaken_for_addresses);
    return UNITY_END();
}
