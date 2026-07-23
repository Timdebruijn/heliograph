// SPDX-License-Identifier: MIT
//
// BOOT-hold factory-reset detector. A factory reset wipes the device, so the two properties
// that must never break are: a short press does nothing, and a long hold fires exactly once
// (not once per loop pass for as long as the button stays down). Clock is injected, so this
// is pure -- no GPIO, no real time.

#include <unity.h>

#include "status/boot_button.h"

using namespace heliograph::status;

void setUp() {}
void tearDown() {}

static void test_short_press_never_triggers() {
    HoldDetector d(5000);
    TEST_ASSERT_EQUAL(HoldDetector::Event::Holding, d.update(true, 0));
    TEST_ASSERT_EQUAL(HoldDetector::Event::Holding, d.update(true, 4999));  // just short
    TEST_ASSERT_EQUAL(HoldDetector::Event::Idle, d.update(false, 5200));    // released
}

static void test_hold_triggers_once_at_the_threshold() {
    HoldDetector d(5000);
    d.update(true, 1000);                                                   // press starts
    TEST_ASSERT_EQUAL(HoldDetector::Event::Holding, d.update(true, 5999));  // 4999 ms in
    TEST_ASSERT_EQUAL(HoldDetector::Event::Triggered, d.update(true, 6000));// exactly 5000 ms
}

static void test_trigger_does_not_repeat_while_held() {
    HoldDetector d(5000);
    d.update(true, 0);
    TEST_ASSERT_EQUAL(HoldDetector::Event::Triggered, d.update(true, 5000));
    // Still held well past the threshold: must stay Idle, not fire again every pass.
    TEST_ASSERT_EQUAL(HoldDetector::Event::Idle, d.update(true, 5100));
    TEST_ASSERT_EQUAL(HoldDetector::Event::Idle, d.update(true, 9000));
}

static void test_release_rearms_for_a_second_reset() {
    HoldDetector d(5000);
    d.update(true, 0);
    TEST_ASSERT_EQUAL(HoldDetector::Event::Triggered, d.update(true, 5000));
    TEST_ASSERT_EQUAL(HoldDetector::Event::Idle, d.update(false, 5500));  // release
    // A fresh hold arms and fires again.
    d.update(true, 6000);
    TEST_ASSERT_EQUAL(HoldDetector::Event::Triggered, d.update(true, 11000));
}

static void test_a_release_before_the_threshold_resets_the_timer() {
    HoldDetector d(5000);
    d.update(true, 0);
    d.update(true, 3000);              // 3 s in
    d.update(false, 3100);             // released early: timer must not carry over
    d.update(true, 3200);              // new press
    TEST_ASSERT_EQUAL(HoldDetector::Event::Holding, d.update(true, 8100));   // only 4900 ms
    TEST_ASSERT_EQUAL(HoldDetector::Event::Triggered, d.update(true, 8200)); // now 5000 ms
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_short_press_never_triggers);
    RUN_TEST(test_hold_triggers_once_at_the_threshold);
    RUN_TEST(test_trigger_does_not_repeat_while_held);
    RUN_TEST(test_release_rearms_for_a_second_reset);
    RUN_TEST(test_a_release_before_the_threshold_resets_the_timer);
    return UNITY_END();
}
