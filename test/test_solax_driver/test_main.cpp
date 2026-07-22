// SPDX-License-Identifier: MIT
// SolaX X1 driver: pure payload decoding, and the registration/poll flow against a
// simulated inverter. The payload layout is transcribed and unvalidated hardware-wise;
// these tests pin the DECODER and the SESSION DISCIPLINE (register once, keep it, recover
// without broadcasts), so a wrong value on the bench means a wrong offset in the parser,
// not a wrong state machine.

#include <unity.h>

#include <cstring>
#include <vector>

#include "device/device_state.h"
#include "drivers/solax_x1/solax_driver.h"
#include "drivers/solax_x1/solax_parser.h"
#include "support/fake_solax_device.h"
#include "support/mock_transport.h"

using namespace heliograph;
using namespace heliograph::solax;
using heliograph::test::FakeSolaxDevice;
using heliograph::test::MockTransport;

void setUp() {}
void tearDown() {}

// --- pure decoding ------------------------------------------------------------------------

static std::vector<uint8_t> samplePayload() {
    // The 52-byte G1 payload built directly with known values.
    std::vector<uint8_t> b;
    auto u16 = [&](uint16_t v) {
        b.push_back(static_cast<uint8_t>(v >> 8));
        b.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    u16(static_cast<uint16_t>(int16_t{-7}));  // temperature: signed
    u16(71);                                  // energy today 7.1
    u16(2103);                                // pv1 V 210.3
    u16(3001);                                // pv2 V 300.1
    u16(41);                                  // pv1 A 4.1
    u16(12);                                  // pv2 A 1.2
    u16(37);                                  // ac A 3.7
    u16(2318);                                // ac V 231.8
    u16(4999);                                // 49.99 Hz
    u16(856);                                 // 856 W
    u16(0);                                   // unused
    // energy total 12345.6 kWh -> 123456, big-endian u32
    b.insert(b.end(), {0x00, 0x01, 0xE2, 0x40});
    // runtime 9876 h
    b.insert(b.end(), {0x00, 0x00, 0x26, 0x94});
    u16(2);  // mode Normal
    for (int i = 0; i < 7; ++i) {
        u16(0);  // fault thresholds
    }
    // error bitmask LITTLE-endian: bit 0 set -> 01 00 00 00
    b.insert(b.end(), {0x01, 0x00, 0x00, 0x00});
    u16(0);  // G1 tail (CT Pgrid)
    return b;
}

static void test_status_report_decodes_every_field_with_its_scale() {
    const auto  payload = samplePayload();
    StatusReport r;
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decodeStatusReport(payload.data(), payload.size(), r)));
    TEST_ASSERT_EQUAL_DOUBLE(-7.0, r.temperatureC);  // signed survives
    TEST_ASSERT_EQUAL_DOUBLE(7.1, r.energyTodayKwh);
    TEST_ASSERT_EQUAL_DOUBLE(210.3, r.pv1Voltage);
    TEST_ASSERT_EQUAL_DOUBLE(300.1, r.pv2Voltage);
    TEST_ASSERT_EQUAL_DOUBLE(4.1, r.pv1Current);
    TEST_ASSERT_EQUAL_DOUBLE(1.2, r.pv2Current);
    TEST_ASSERT_EQUAL_DOUBLE(3.7, r.acCurrent);
    TEST_ASSERT_EQUAL_DOUBLE(231.8, r.acVoltage);
    TEST_ASSERT_EQUAL_DOUBLE(49.99, r.frequencyHz);
    TEST_ASSERT_EQUAL_DOUBLE(856.0, r.acPowerW);
    TEST_ASSERT_EQUAL_DOUBLE(12345.6, r.energyTotalKwh);
    TEST_ASSERT_EQUAL_UINT32(9876, r.runtimeHours);
    TEST_ASSERT_EQUAL_UINT16(2, r.mode);
    TEST_ASSERT_EQUAL_UINT32(1, r.errorBits);  // little-endian read
}

static void test_a_truncated_status_report_is_rejected_not_guessed() {
    const auto  payload = samplePayload();
    StatusReport r;
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::TooShort),
                      static_cast<int>(decodeStatusReport(payload.data(), 49, r)));
}

static void test_a_g3_length_payload_decodes_via_the_common_prefix() {
    auto payload = samplePayload();
    payload.resize(56, 0x00);  // G3 sends 56 bytes; the tail is ignored
    StatusReport r;
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decodeStatusReport(payload.data(), payload.size(), r)));
    TEST_ASSERT_EQUAL_DOUBLE(856.0, r.acPowerW);
}

static void test_mode_text_covers_the_documented_table_and_nothing_more() {
    TEST_ASSERT_EQUAL_STRING("Wait", modeText(0));
    TEST_ASSERT_EQUAL_STRING("Normal", modeText(2));
    TEST_ASSERT_EQUAL_STRING("Self Test", modeText(6));
    TEST_ASSERT_EQUAL_STRING("", modeText(7));  // unknown: no invented name
}

static void test_device_info_fields_are_trimmed_ascii() {
    std::vector<uint8_t> b;
    b.push_back(0x01);
    auto pad = [&](const char* s, size_t w) {
        size_t i = 0;
        for (; s[i] != '\0'; ++i) {
            b.push_back(static_cast<uint8_t>(s[i]));
        }
        for (; i < w; ++i) {
            b.push_back(' ');
        }
    };
    pad("1100", 6);
    pad("1.09", 5);
    pad("X1-1.1-S-D", 14);
    pad("SolaxPower", 14);
    pad("XM3B11ABCDEF", 14);
    pad("380", 4);
    TEST_ASSERT_EQUAL_UINT32(kDeviceInfoBytes, b.size());

    DeviceInfo info;
    TEST_ASSERT_TRUE(decodeDeviceInfo(b.data(), b.size(), info));
    TEST_ASSERT_EQUAL_STRING("X1-1.1-S-D", info.moduleName.c_str());
    TEST_ASSERT_EQUAL_STRING("SolaxPower", info.factoryName.c_str());
    TEST_ASSERT_EQUAL_STRING("XM3B11ABCDEF", info.serialNumber.c_str());
    TEST_ASSERT_EQUAL_STRING("1.09", info.firmwareVersion.c_str());
    TEST_ASSERT_FALSE(decodeDeviceInfo(b.data(), b.size() - 1, info));
}

static void test_a_garbage_serial_cannot_register() {
    const uint8_t zeros[kSerialNumberBytes] = {};
    TEST_ASSERT_FALSE(serialLooksValid(zeros, sizeof(zeros)));
    TEST_ASSERT_FALSE(serialLooksValid(zeros, 5));  // wrong length
    TEST_ASSERT_TRUE(serialLooksValid(FakeSolaxDevice::kSerial, kSerialNumberBytes));
}

// --- session discipline over the bus ------------------------------------------------------

static void test_a_full_poll_registers_at_0x0a_and_decodes_measurements() {
    MockTransport   transport;
    FakeSolaxDevice device;
    device.installOn(transport);

    SolaxDriver driver(transport);
    TEST_ASSERT_TRUE(driver.begin(transport));

    DeviceState state;
    state.lastPollAttemptMs = 1000;
    TEST_ASSERT_EQUAL(static_cast<int>(PollResult::Ok), static_cast<int>(driver.poll(state)));

    TEST_ASSERT_EQUAL_UINT8(0x0A, device.assignedAddress);  // the reference convention
    const auto* power = state.measurements.find(measurement_id::kAcPowerTotal);
    TEST_ASSERT_NOT_NULL(power);
    TEST_ASSERT_TRUE(power->valid);
    TEST_ASSERT_EQUAL_DOUBLE(856.0, power->value);
    const auto* today = state.measurements.find(measurement_id::kEnergyToday);
    TEST_ASSERT_EQUAL_DOUBLE(7.1, today->value);
    // Identity came from the structured device info.
    TEST_ASSERT_EQUAL_STRING("X1-1.1-S-D", state.identity.model.c_str());
    TEST_ASSERT_EQUAL_STRING("SolaxPower", state.identity.manufacturer.c_str());
    TEST_ASSERT_EQUAL_STRING("Normal", state.statusText.c_str());
    TEST_ASSERT_TRUE(state.errorCodeSupported);
}

static void test_registration_survives_a_missing_ack_via_the_status_query() {
    MockTransport   transport;
    FakeSolaxDevice device;
    device.withholdAck = true;
    device.installOn(transport);

    SolaxDriver driver(transport);
    TEST_ASSERT_TRUE(driver.begin(transport));

    DeviceState state;
    state.lastPollAttemptMs = 1000;
    // The reference implementation never waits for the ACK, so real firmware may not send
    // one. Registration must then be verified by the device answering at its new address.
    TEST_ASSERT_EQUAL(static_cast<int>(PollResult::Ok), static_cast<int>(driver.poll(state)));
    TEST_ASSERT_TRUE(driver.registered());
}

static void test_a_cold_boot_against_a_still_registered_inverter_needs_no_broadcast() {
    MockTransport   transport;
    FakeSolaxDevice device;
    device.registered      = true;  // kept its address across OUR reboot
    device.assignedAddress = 0x0A;
    device.installOn(transport);

    SolaxDriver driver(transport);
    TEST_ASSERT_TRUE(driver.begin(transport));

    DeviceState state;
    state.lastPollAttemptMs = 1000;
    // The offline query is ignored (family asymmetry); the driver must fall through to a
    // direct status query at the deterministic address instead of broadcasting anything.
    TEST_ASSERT_EQUAL(static_cast<int>(PollResult::Ok), static_cast<int>(driver.poll(state)));
    TEST_ASSERT_TRUE(driver.registered());
    // This is the COMMON restart path and registration is sticky afterwards: the identity
    // must be read here too, not only on a fresh registration (review, 2026-07-21).
    TEST_ASSERT_EQUAL_STRING("X1-1.1-S-D", state.identity.model.c_str());
    TEST_ASSERT_EQUAL_STRING("XM3B11ABCDEF", state.identity.serialNumber.c_str());
}

static void test_a_run_of_timeouts_keeps_the_registration() {
    MockTransport   transport;
    FakeSolaxDevice device;
    device.installOn(transport);

    SolaxDriver driver(transport);
    TEST_ASSERT_TRUE(driver.begin(transport));

    DeviceState state;
    state.lastPollAttemptMs = 1000;
    TEST_ASSERT_EQUAL(static_cast<int>(PollResult::Ok), static_cast<int>(driver.poll(state)));

    // The inverter goes dark (dusk) -- polls time out, registration must be KEPT.
    device.offline = true;
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT_EQUAL(static_cast<int>(PollResult::Timeout),
                          static_cast<int>(driver.poll(state)));
    }
    TEST_ASSERT_TRUE(driver.registered());

    // Sunrise: the device is back, address forgotten. The recovery probe (a plain offline
    // query, never a broadcast) must pick it up again.
    device.offline    = false;
    device.registered = false;
    TEST_ASSERT_EQUAL(static_cast<int>(PollResult::Ok), static_cast<int>(driver.poll(state)));
    TEST_ASSERT_EQUAL_UINT8(0x0A, device.assignedAddress);
}

static void test_silence_from_an_empty_bus_is_not_registered() {
    MockTransport transport;  // no responder: nothing on the bus
    SolaxDriver   driver(transport);
    TEST_ASSERT_TRUE(driver.begin(transport));

    DeviceState state;
    TEST_ASSERT_EQUAL(static_cast<int>(PollResult::NotRegistered),
                      static_cast<int>(driver.poll(state)));
    TEST_ASSERT_FALSE(driver.registered());
}

static void test_probe_reports_serial_and_identity_evidence() {
    MockTransport   transport;
    FakeSolaxDevice device;
    device.installOn(transport);

    SolaxDriver driver(transport);
    TEST_ASSERT_TRUE(driver.begin(transport));

    const ProbeResult result = driver.probe();
    TEST_ASSERT_TRUE(result.responded);
    TEST_ASSERT_TRUE(result.checksumValid);
    TEST_ASSERT_TRUE(result.confidenceScore >= 80);  // eligible for auto-selection
    TEST_ASSERT_EQUAL_STRING("X1MINI1100ABCD", result.serialNumber.c_str());
    TEST_ASSERT_EQUAL_STRING("X1-1.1-S-D", result.detectedModel.c_str());
}

static void test_noise_before_the_reply_does_not_cost_the_reply() {
    MockTransport   transport;
    FakeSolaxDevice device;
    device.prependNoise = true;
    device.installOn(transport);

    SolaxDriver driver(transport);
    TEST_ASSERT_TRUE(driver.begin(transport));

    DeviceState state;
    state.lastPollAttemptMs = 1000;
    TEST_ASSERT_EQUAL(static_cast<int>(PollResult::Ok), static_cast<int>(driver.poll(state)));
}

static void test_pv2_channels_appear_only_when_pv2_shows_real_voltage() {
    MockTransport   transport;
    FakeSolaxDevice device;
    device.installOn(transport);

    SolaxDriver driver(transport);
    TEST_ASSERT_TRUE(driver.begin(transport));

    DeviceState state;
    state.lastPollAttemptMs = 1000;
    TEST_ASSERT_EQUAL(static_cast<int>(PollResult::Ok), static_cast<int>(driver.poll(state)));
    // Datasheet says single MPPT; with PV2 at 0 V the channel must not exist at all.
    TEST_ASSERT_NULL(state.measurements.find(measurement_id::kDcMppt2Voltage));
    TEST_ASSERT_EQUAL_UINT8(1, state.capabilities.mpptCount);

    // A dual-MPPT variant proves itself by real voltage; the channel then appears.
    device.pv2Voltage10 = 1980;  // 198.0 V
    device.pv2Current10 = 21;
    TEST_ASSERT_EQUAL(static_cast<int>(PollResult::Ok), static_cast<int>(driver.poll(state)));
    const auto* pv2 = state.measurements.find(measurement_id::kDcMppt2Voltage);
    TEST_ASSERT_NOT_NULL(pv2);
    TEST_ASSERT_EQUAL_DOUBLE(198.0, pv2->value);
    TEST_ASSERT_EQUAL_UINT8(2, state.capabilities.mpptCount);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_status_report_decodes_every_field_with_its_scale);
    RUN_TEST(test_a_truncated_status_report_is_rejected_not_guessed);
    RUN_TEST(test_a_g3_length_payload_decodes_via_the_common_prefix);
    RUN_TEST(test_mode_text_covers_the_documented_table_and_nothing_more);
    RUN_TEST(test_device_info_fields_are_trimmed_ascii);
    RUN_TEST(test_a_garbage_serial_cannot_register);
    RUN_TEST(test_a_full_poll_registers_at_0x0a_and_decodes_measurements);
    RUN_TEST(test_registration_survives_a_missing_ack_via_the_status_query);
    RUN_TEST(test_a_cold_boot_against_a_still_registered_inverter_needs_no_broadcast);
    RUN_TEST(test_a_run_of_timeouts_keeps_the_registration);
    RUN_TEST(test_silence_from_an_empty_bus_is_not_registered);
    RUN_TEST(test_probe_reports_serial_and_identity_evidence);
    RUN_TEST(test_noise_before_the_reply_does_not_cost_the_reply);
    RUN_TEST(test_pv2_channels_appear_only_when_pv2_shows_real_voltage);
    return UNITY_END();
}
