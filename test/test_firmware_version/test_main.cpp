// SPDX-License-Identifier: MIT
// Version comparison: the logic that decides whether to tell someone an update exists.
//
// Every failure here is quiet. Compare as strings and "0.9.0" sorts above "0.14.0", so the
// dashboard nags forever about a downgrade. Get the parse wrong on the build stamp this
// firmware actually reports and it never fires at all. Neither shows up as an error.

#include <unity.h>

#include <string>

#include "ota/firmware_version.h"

using heliograph::ota::isNewer;
using heliograph::ota::parseSemVer;
using heliograph::ota::SemVer;

void setUp() {}
void tearDown() {}

static SemVer parsed(const char* text) {
    SemVer v;
    TEST_ASSERT_TRUE_MESSAGE(parseSemVer(text, v), text);
    return v;
}

// --- parsing --------------------------------------------------------------------------------

static void test_a_plain_version_parses() {
    const SemVer v = parsed("0.14.0");
    TEST_ASSERT_EQUAL_UINT16(0, v.major);
    TEST_ASSERT_EQUAL_UINT16(14, v.minor);
    TEST_ASSERT_EQUAL_UINT16(0, v.patch);
}

/// What this firmware actually reports about itself. kFirmwareVersion appends a compile-time
/// build stamp, so a parser that expected a clean string would fail on the one input it is
/// guaranteed to be given.
static void test_the_build_stamp_this_firmware_reports_is_ignored() {
    const SemVer v = parsed("0.14.0 (Jul 26 2026 17:31:45)");
    TEST_ASSERT_EQUAL_UINT16(14, v.minor);
    TEST_ASSERT_EQUAL_UINT16(0, v.patch);
}

/// Release tags are written with a leading v, and a feed may hand back either form.
static void test_a_leading_v_is_accepted() {
    TEST_ASSERT_TRUE(parsed("v1.2.3") == parsed("1.2.3"));
    TEST_ASSERT_TRUE(parsed(" 1.2.3") == parsed("1.2.3"));
}

static void test_a_prerelease_suffix_is_ignored() {
    TEST_ASSERT_TRUE(parsed("1.2.3-rc1") == parsed("1.2.3"));
}

/// Refusing is the point: an unparseable version must not compare as older or newer, because
/// both answers are a guess and one of them offers somebody a firmware image on it.
static void test_what_will_not_parse_is_refused() {
    SemVer v;
    TEST_ASSERT_FALSE(parseSemVer("", v));
    TEST_ASSERT_FALSE(parseSemVer("1.2", v));           // no patch
    TEST_ASSERT_FALSE(parseSemVer("1", v));
    TEST_ASSERT_FALSE(parseSemVer("latest", v));
    TEST_ASSERT_FALSE(parseSemVer("1.2.x", v));
    TEST_ASSERT_FALSE(parseSemVer("..", v));
    TEST_ASSERT_FALSE(parseSemVer("<!DOCTYPE html>", v));  // a captive portal, not a feed
}

/// A fourth numeric component is a versioning scheme we do not understand. Reading "1.2.3.4" as
/// 1.2.3 would make 1.2.3.4 and 1.2.3.5 compare equal -- an update that exists and is never
/// offered.
static void test_a_fourth_component_is_refused_rather_than_truncated() {
    SemVer v;
    TEST_ASSERT_FALSE(parseSemVer("1.2.3.4", v));
    TEST_ASSERT_FALSE(parseSemVer("0.14.0.1", v));
    // A dot that is not the start of another number is just text, and text is ignored.
    TEST_ASSERT_TRUE(parseSemVer("1.2.3.", v));
    TEST_ASSERT_TRUE(parsed("1.2.3 (build 4)") == parsed("1.2.3"));
}

/// Bounded, so a long run of digits cannot wrap into a small number and offer a downgrade as
/// an upgrade.
static void test_an_absurd_number_is_refused_not_wrapped() {
    SemVer v;
    TEST_ASSERT_FALSE(parseSemVer("65536.0.0", v));
    TEST_ASSERT_FALSE(parseSemVer("0.99999999999.0", v));
    TEST_ASSERT_TRUE(parseSemVer("65535.0.0", v));  // the boundary itself is fine
}

// --- comparison -----------------------------------------------------------------------------

/// The one that string comparison gets wrong, and the reason this file exists: "0.9.0" sorts
/// ABOVE "0.14.0" as text.
static void test_a_double_digit_minor_is_newer_than_a_single_digit_one() {
    TEST_ASSERT_TRUE(isNewer("0.9.0", "0.14.0"));
    TEST_ASSERT_FALSE(isNewer("0.14.0", "0.9.0"));
}

static void test_each_component_is_compared_in_order() {
    TEST_ASSERT_TRUE(isNewer("0.14.0", "1.0.0"));
    TEST_ASSERT_TRUE(isNewer("0.14.0", "0.15.0"));
    TEST_ASSERT_TRUE(isNewer("0.14.0", "0.14.1"));
    TEST_ASSERT_FALSE(isNewer("1.0.0", "0.99.99"));
}

static void test_the_same_version_is_not_newer() {
    TEST_ASSERT_FALSE(isNewer("0.14.0", "0.14.0"));
    // ...including when only one side carries the build stamp, which is the everyday case:
    // the bridge reports a stamped version and the feed reports a clean one.
    TEST_ASSERT_FALSE(isNewer("0.14.0 (Jul 26 2026 17:31:45)", "0.14.0"));
    TEST_ASSERT_FALSE(isNewer("0.14.0 (Jul 26 2026 17:31:45)", "v0.14.0"));
}

static void test_a_newer_release_is_seen_through_the_build_stamp() {
    TEST_ASSERT_TRUE(isNewer("0.14.0 (Jul 26 2026 17:31:45)", "0.14.1"));
}

/// A feed that has been replaced by something unexpected -- an error page, a redirect, a
/// captive portal -- produces no notification rather than one nobody can trust.
static void test_an_unparseable_side_never_reports_an_update() {
    TEST_ASSERT_FALSE(isNewer("0.14.0", "latest"));
    TEST_ASSERT_FALSE(isNewer("0.14.0", ""));
    TEST_ASSERT_FALSE(isNewer("", "0.14.0"));
    TEST_ASSERT_FALSE(isNewer("unknown", "0.15.0"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_a_plain_version_parses);
    RUN_TEST(test_the_build_stamp_this_firmware_reports_is_ignored);
    RUN_TEST(test_a_leading_v_is_accepted);
    RUN_TEST(test_a_prerelease_suffix_is_ignored);
    RUN_TEST(test_what_will_not_parse_is_refused);
    RUN_TEST(test_a_fourth_component_is_refused_rather_than_truncated);
    RUN_TEST(test_an_absurd_number_is_refused_not_wrapped);
    RUN_TEST(test_a_double_digit_minor_is_newer_than_a_single_digit_one);
    RUN_TEST(test_each_component_is_compared_in_order);
    RUN_TEST(test_the_same_version_is_not_newer);
    RUN_TEST(test_a_newer_release_is_seen_through_the_build_stamp);
    RUN_TEST(test_an_unparseable_side_never_reports_an_update);
    return UNITY_END();
}
