// SPDX-License-Identifier: MIT
//
// Status LED colour policy. One light has to stand in for the UI's four dots, so the rules
// that matter most are the ones about NOT crying wolf: a disabled output, or a board with no
// inverter, must never dim the light. Every branch is pinned here.

#include <unity.h>

#include "status/status_led.h"

using namespace heliograph::status;

void setUp() {}
void tearDown() {}

// A fully healthy, fully enabled bridge -- the baseline the tests mutate one field at a time.
static LedInputs healthy() {
    LedInputs in;
    in.provisioned      = true;
    in.wifiConnected    = true;
    in.inverterExpected = true;
    in.inverterOnline   = true;
    in.dataValid        = true;
    in.dataStale        = false;
    in.mqttEnabled      = true;
    in.mqttConnected    = true;
    in.modbusEnabled    = true;
    in.modbusListening  = true;
    return in;
}

static void test_all_healthy_is_green() {
    const auto ind = decide(healthy());
    TEST_ASSERT_EQUAL(LedColor::Green, ind.color);
    TEST_ASSERT_FALSE(ind.blink);
}

static void test_unprovisioned_is_blue_over_everything() {
    LedInputs in = healthy();
    in.provisioned = false;
    // Even with WiFi down and no inverter, setup wins: blue invites configuration.
    in.wifiConnected  = false;
    in.inverterOnline = false;
    TEST_ASSERT_EQUAL(LedColor::Blue, decide(in).color);
}

static void test_factory_reset_hold_blinks_red_over_health() {
    LedInputs in = healthy();  // otherwise perfectly green
    in.factoryResetHolding = true;
    const auto ind = decide(in);
    TEST_ASSERT_EQUAL(LedColor::Red, ind.color);
    TEST_ASSERT_TRUE(ind.blink);
}

static void test_no_wifi_is_amber() {
    LedInputs in = healthy();
    in.wifiConnected = false;
    const auto ind = decide(in);
    TEST_ASSERT_EQUAL(LedColor::Amber, ind.color);
    TEST_ASSERT_FALSE(ind.blink);
}

static void test_inverter_offline_is_red_when_expected() {
    LedInputs in = healthy();
    in.inverterOnline = false;
    TEST_ASSERT_EQUAL(LedColor::Red, decide(in).color);
}

static void test_stale_or_invalid_data_is_amber() {
    LedInputs stale = healthy();
    stale.dataStale = true;
    TEST_ASSERT_EQUAL(LedColor::Amber, decide(stale).color);

    LedInputs invalid = healthy();
    invalid.dataValid = false;
    TEST_ASSERT_EQUAL(LedColor::Amber, decide(invalid).color);
}

// The don't-cry-wolf core: a relay-only board with no driver has no data to miss.
static void test_relay_only_board_without_driver_is_green() {
    LedInputs in = healthy();
    in.inverterExpected = false;
    in.inverterOnline   = false;  // nothing polling, and that is correct
    in.dataValid        = false;
    in.dataStale        = true;
    TEST_ASSERT_EQUAL(LedColor::Green, decide(in).color);
}

static void test_disabled_outputs_do_not_dim_the_light() {
    // MQTT off and disconnected, Modbus off and not listening: none of it is a fault,
    // because the owner never turned them on.
    LedInputs in = healthy();
    in.mqttEnabled     = false;
    in.mqttConnected   = false;
    in.modbusEnabled   = false;
    in.modbusListening = false;
    TEST_ASSERT_EQUAL(LedColor::Green, decide(in).color);
}

static void test_enabled_output_that_fails_is_amber() {
    LedInputs mqtt = healthy();
    mqtt.mqttConnected = false;  // enabled, but the broker is gone
    TEST_ASSERT_EQUAL(LedColor::Amber, decide(mqtt).color);

    LedInputs modbus = healthy();
    modbus.modbusListening = false;  // enabled, but the server did not bind
    TEST_ASSERT_EQUAL(LedColor::Amber, decide(modbus).color);
}

// Red (core broken) must outrank amber (a degraded output): a bridge with no data AND a
// dead broker is red, not amber.
static void test_red_outranks_amber() {
    LedInputs in = healthy();
    in.inverterOnline = false;  // red-worthy
    in.mqttConnected  = false;  // also amber-worthy
    TEST_ASSERT_EQUAL(LedColor::Red, decide(in).color);
}

static void test_color_names() {
    TEST_ASSERT_EQUAL_STRING("green", colorName(LedColor::Green));
    TEST_ASSERT_EQUAL_STRING("amber", colorName(LedColor::Amber));
    TEST_ASSERT_EQUAL_STRING("red", colorName(LedColor::Red));
    TEST_ASSERT_EQUAL_STRING("blue", colorName(LedColor::Blue));
    TEST_ASSERT_EQUAL_STRING("off", colorName(LedColor::Off));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_all_healthy_is_green);
    RUN_TEST(test_unprovisioned_is_blue_over_everything);
    RUN_TEST(test_factory_reset_hold_blinks_red_over_health);
    RUN_TEST(test_no_wifi_is_amber);
    RUN_TEST(test_inverter_offline_is_red_when_expected);
    RUN_TEST(test_stale_or_invalid_data_is_amber);
    RUN_TEST(test_relay_only_board_without_driver_is_green);
    RUN_TEST(test_disabled_outputs_do_not_dim_the_light);
    RUN_TEST(test_enabled_output_that_fails_is_amber);
    RUN_TEST(test_red_outranks_amber);
    RUN_TEST(test_color_names);
    return UNITY_END();
}
