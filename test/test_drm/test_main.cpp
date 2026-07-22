// SPDX-License-Identifier: MIT
//
// DRM semantics: roles -> selectable modes, mode -> relay pattern, mask -> mode. Pure
// logic, so every edge that could ever assert the wrong contact is pinned here.

#include <unity.h>

#include "relays/drm.h"

using namespace heliograph::drm;

void setUp() {}
void tearDown() {}

static void test_role_validation() {
    TEST_ASSERT_TRUE(isValidRole("none"));
    TEST_ASSERT_TRUE(isValidRole("drm0"));
    TEST_ASSERT_TRUE(isValidRole("drm8"));
    TEST_ASSERT_FALSE(isValidRole("drm9"));
    TEST_ASSERT_FALSE(isValidRole("drm"));
    TEST_ASSERT_FALSE(isValidRole("DRM0"));
    TEST_ASSERT_FALSE(isValidRole(""));
    TEST_ASSERT_FALSE(isValidRole("normal"));  // a mode, never a role
}

static void test_options_derive_from_roles() {
    // No roles: no options, so no select entity exists at all.
    TEST_ASSERT_TRUE(optionsFor({"none", "none"}).empty());

    const auto options = optionsFor({"drm0", "none", "drm5"});
    TEST_ASSERT_EQUAL_UINT32(3, options.size());
    TEST_ASSERT_EQUAL_STRING("normal", options[0].c_str());
    TEST_ASSERT_EQUAL_STRING("drm0", options[1].c_str());
    TEST_ASSERT_EQUAL_STRING("drm5", options[2].c_str());

    // Duplicate roles (two relays on one line) appear once.
    TEST_ASSERT_EQUAL_UINT32(2, optionsFor({"drm0", "drm0"}).size());
}

static void test_pattern_asserts_exactly_the_role() {
    std::vector<bool> pattern;
    TEST_ASSERT_TRUE(patternFor({"drm0", "none", "drm5"}, "drm5", pattern));
    TEST_ASSERT_FALSE(pattern[0]);
    TEST_ASSERT_FALSE(pattern[1]);
    TEST_ASSERT_TRUE(pattern[2]);

    // "normal" releases everything.
    TEST_ASSERT_TRUE(patternFor({"drm0", "none", "drm5"}, "normal", pattern));
    for (const bool on : pattern) {
        TEST_ASSERT_FALSE(on);
    }

    // A duplicated role energises every relay carrying it.
    TEST_ASSERT_TRUE(patternFor({"drm0", "drm0"}, "drm0", pattern));
    TEST_ASSERT_TRUE(pattern[0]);
    TEST_ASSERT_TRUE(pattern[1]);
}

static void test_invalid_modes_are_refused() {
    std::vector<bool> pattern;
    // A role no relay carries.
    TEST_ASSERT_FALSE(patternFor({"drm0"}, "drm5", pattern));
    // "custom" is a reported state, never a command.
    TEST_ASSERT_FALSE(patternFor({"drm0"}, "custom", pattern));
    // "none" is a role, never a mode.
    TEST_ASSERT_FALSE(patternFor({"drm0"}, "none", pattern));
    // "normal" without any roles is meaningless.
    TEST_ASSERT_FALSE(patternFor({"none"}, "normal", pattern));
    // Refusal always leaves a released pattern behind, so a caller that ignores the
    // return value can still never assert something by accident.
    for (const bool on : pattern) {
        TEST_ASSERT_FALSE(on);
    }
}

static void test_mode_from_mask() {
    const std::vector<std::string> roles = {"drm0", "none", "drm5"};
    TEST_ASSERT_EQUAL_STRING("normal", modeFrom(roles, 0b000).c_str());
    TEST_ASSERT_EQUAL_STRING("drm0", modeFrom(roles, 0b001).c_str());
    TEST_ASSERT_EQUAL_STRING("drm5", modeFrom(roles, 0b100).c_str());
    // The role-less relay energised, or a combination: no single mode explains it.
    TEST_ASSERT_EQUAL_STRING("custom", modeFrom(roles, 0b010).c_str());
    TEST_ASSERT_EQUAL_STRING("custom", modeFrom(roles, 0b101).c_str());

    // Duplicated role: BOTH its relays energised is the mode; one of the two is custom.
    const std::vector<std::string> dup = {"drm0", "drm0"};
    TEST_ASSERT_EQUAL_STRING("drm0", modeFrom(dup, 0b11).c_str());
    TEST_ASSERT_EQUAL_STRING("custom", modeFrom(dup, 0b01).c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_role_validation);
    RUN_TEST(test_options_derive_from_roles);
    RUN_TEST(test_pattern_asserts_exactly_the_role);
    RUN_TEST(test_invalid_modes_are_refused);
    RUN_TEST(test_mode_from_mask);
    return UNITY_END();
}
