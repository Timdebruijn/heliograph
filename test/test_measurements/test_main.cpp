// SPDX-License-Identifier: MIT
// Measurement model, staleness rules and capability gating.

#include <unity.h>

#include <cstring>

#include "device/capability.h"
#include "device/command.h"
#include "device/device_state.h"
#include "device/measurement.h"

using namespace heliograph;

void setUp() {}
void tearDown() {}

static MeasurementSet makeSet() {
    MeasurementSet s;
    s.declare(measurement_id::kAcPowerTotal, MeasurementType::Power, Unit::Watt, "AC Power");
    s.declare(measurement_id::kEnergyTotal, MeasurementType::Energy, Unit::KilowattHour,
              "Total Energy");
    s.declare(measurement_id::kDcPowerTotal, MeasurementType::Power, Unit::Watt, "DC Power",
              /*derived=*/true);
    return s;
}

// --- declaring and setting ----------------------------------------------------------------

static void test_declared_channel_starts_supported_but_invalid() {
    auto        s = makeSet();
    const auto* m = s.find(measurement_id::kAcPowerTotal);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_TRUE(m->supported);
    TEST_ASSERT_FALSE(m->valid);
    TEST_ASSERT_FALSE(m->stale);
}

static void test_set_records_value_and_marks_valid() {
    auto s = makeSet();
    s.set(measurement_id::kAcPowerTotal, 1842.0, 1000);

    const auto* m = s.find(measurement_id::kAcPowerTotal);
    TEST_ASSERT_TRUE(m->valid);
    TEST_ASSERT_FALSE(m->stale);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1842.0, m->value);
    TEST_ASSERT_EQUAL_UINT64(1000, m->timestampMs);
}

static void test_undeclared_channel_is_ignored() {
    // A driver must advertise a channel before reporting on it, otherwise capability
    // filtering downstream would be meaningless.
    auto s = makeSet();
    s.set("battery.soc", 55.0, 1000);
    TEST_ASSERT_NULL(s.find("battery.soc"));
    TEST_ASSERT_EQUAL_size_t(3, s.size());
}

static void test_redeclaring_keeps_the_value() {
    auto s = makeSet();
    s.set(measurement_id::kAcPowerTotal, 1842.0, 1000);
    s.declare(measurement_id::kAcPowerTotal, MeasurementType::Power, Unit::Watt, "AC Power");

    const auto* m = s.find(measurement_id::kAcPowerTotal);
    TEST_ASSERT_EQUAL_size_t(3, s.size());
    TEST_ASSERT_TRUE(m->valid);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1842.0, m->value);
}

static void test_derived_flag_survives() {
    auto        s = makeSet();
    const auto* m = s.find(measurement_id::kDcPowerTotal);
    TEST_ASSERT_TRUE(m->derived);
    TEST_ASSERT_FALSE(s.find(measurement_id::kAcPowerTotal)->derived);
}

static void test_zero_is_a_real_value_not_an_absence() {
    // The whole point of the validity flags: 0 W at night is data, not "unknown".
    auto s = makeSet();
    s.set(measurement_id::kAcPowerTotal, 0.0, 1000);
    TEST_ASSERT_TRUE(s.isValid(measurement_id::kAcPowerTotal));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, s.find(measurement_id::kAcPowerTotal)->value);
}

static void test_declare_unsupported_marks_the_channel_unreadable() {
    // The other way to say "not available": present in the schema, but unreadable. Without
    // this the `supported` flag would be permanently true and the checks that honour it in
    // MQTT, Modbus and discovery would be unreachable code.
    auto s = makeSet();
    s.declareUnsupported("battery.soc", MeasurementType::Ratio, Unit::Percent, "Battery SoC");

    const auto* m = s.find("battery.soc");
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_FALSE(m->supported);
    TEST_ASSERT_FALSE(m->valid);
    TEST_ASSERT_FALSE(s.isValid("battery.soc"));
}

static void test_an_unsupported_channel_refuses_values() {
    // A driver reporting a value for a channel it declared unreadable is a driver bug;
    // accepting it would publish data the capabilities deny.
    auto s = makeSet();
    s.declareUnsupported("battery.soc", MeasurementType::Ratio, Unit::Percent, "Battery SoC");
    s.set("battery.soc", 55.0, 1000);

    TEST_ASSERT_FALSE(s.find("battery.soc")->valid);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, s.find("battery.soc")->value);
}

static void test_redeclaring_an_unsupported_channel_makes_it_supported_again() {
    // A driver that learns more about the device on a later poll can promote the channel.
    auto s = makeSet();
    s.declareUnsupported("battery.soc", MeasurementType::Ratio, Unit::Percent, "Battery SoC");
    s.declare("battery.soc", MeasurementType::Ratio, Unit::Percent, "Battery SoC");
    s.set("battery.soc", 55.0, 1000);

    TEST_ASSERT_TRUE(s.find("battery.soc")->supported);
    TEST_ASSERT_TRUE(s.isValid("battery.soc"));
}

static void test_invalidate_keeps_channel_supported() {
    auto s = makeSet();
    s.set(measurement_id::kAcPowerTotal, 1842.0, 1000);
    s.invalidate(measurement_id::kAcPowerTotal);

    const auto* m = s.find(measurement_id::kAcPowerTotal);
    TEST_ASSERT_TRUE(m->supported);
    TEST_ASSERT_FALSE(m->valid);
    TEST_ASSERT_FALSE(s.isValid(measurement_id::kAcPowerTotal));
}

// --- staleness ------------------------------------------------------------------------------

static void test_staleness_is_based_on_age() {
    auto s = makeSet();
    s.set(measurement_id::kAcPowerTotal, 1842.0, 1000);

    s.updateStaleness(/*now=*/20000, /*maxAge=*/30000);
    TEST_ASSERT_FALSE(s.find(measurement_id::kAcPowerTotal)->stale);

    s.updateStaleness(/*now=*/40000, /*maxAge=*/30000);
    TEST_ASSERT_TRUE(s.find(measurement_id::kAcPowerTotal)->stale);
}

static void test_stale_value_is_retained() {
    auto s = makeSet();
    s.set(measurement_id::kAcPowerTotal, 1842.0, 1000);
    s.updateStaleness(40000, 30000);

    const auto* m = s.find(measurement_id::kAcPowerTotal);
    TEST_ASSERT_TRUE(m->valid);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1842.0, m->value);  // still the best we know
}

static void test_fresh_reading_clears_stale() {
    auto s = makeSet();
    s.set(measurement_id::kAcPowerTotal, 1842.0, 1000);
    s.updateStaleness(40000, 30000);
    s.set(measurement_id::kAcPowerTotal, 1900.0, 41000);
    TEST_ASSERT_FALSE(s.find(measurement_id::kAcPowerTotal)->stale);
}

static void test_staleness_ignores_invalid_channels() {
    auto s = makeSet();
    s.updateStaleness(99999, 30000);
    TEST_ASSERT_FALSE(s.find(measurement_id::kAcPowerTotal)->stale);
}

static void test_staleness_survives_a_clock_that_went_backwards() {
    auto s = makeSet();
    s.set(measurement_id::kAcPowerTotal, 1842.0, 50000);
    s.updateStaleness(/*now=*/1000, /*maxAge=*/30000);  // must not underflow into "very old"
    TEST_ASSERT_FALSE(s.find(measurement_id::kAcPowerTotal)->stale);
}

// --- poll state machine -----------------------------------------------------------------------

static DeviceState makeState() {
    DeviceState st;
    st.measurements = makeSet();
    st.measurements.set(measurement_id::kAcPowerTotal, 1842.0, 1000);
    StalenessPolicy p;
    st.recordPollSuccess(1000, p);
    return st;
}

static void test_successful_poll_marks_everything_healthy() {
    auto st = makeState();
    TEST_ASSERT_TRUE(st.inverterOnline);
    TEST_ASSERT_TRUE(st.dataValid);
    TEST_ASSERT_FALSE(st.dataStale);
    TEST_ASSERT_EQUAL_UINT32(0, st.consecutiveFailures);
    TEST_ASSERT_EQUAL_UINT64(1000, st.lastSuccessfulPollMs);
}

static void test_one_failed_poll_keeps_data_valid() {
    // A single dropped frame must not punch a hole in Home Assistant's history.
    auto            st = makeState();
    StalenessPolicy p;
    st.recordPollFailure(11000, p);

    TEST_ASSERT_EQUAL_UINT32(1, st.consecutiveFailures);
    TEST_ASSERT_TRUE(st.inverterOnline);
    TEST_ASSERT_TRUE(st.dataValid);
    TEST_ASSERT_FALSE(st.dataStale);
    TEST_ASSERT_EQUAL_UINT64(1000, st.lastSuccessfulPollMs);  // unchanged
}

static void test_three_failed_polls_mark_data_stale_but_still_valid() {
    auto            st = makeState();
    StalenessPolicy p;
    for (int i = 0; i < 3; ++i) {
        st.recordPollFailure(11000 + i * 10000, p);
    }
    TEST_ASSERT_TRUE(st.dataStale);
    TEST_ASSERT_TRUE(st.dataValid);      // keep publishing the last known values
    TEST_ASSERT_TRUE(st.inverterOnline);
    TEST_ASSERT_TRUE(st.measurements.find(measurement_id::kAcPowerTotal)->stale);
}

static void test_ten_failed_polls_take_the_inverter_offline() {
    // The nightly case. Must not reboot, must not wipe values, must not lie about them.
    auto            st = makeState();
    StalenessPolicy p;
    for (int i = 0; i < 10; ++i) {
        st.recordPollFailure(11000 + i * 10000, p);
    }
    TEST_ASSERT_FALSE(st.inverterOnline);
    TEST_ASSERT_FALSE(st.dataValid);
    TEST_ASSERT_TRUE(st.dataStale);
    TEST_ASSERT_EQUAL_UINT32(10, st.consecutiveFailures);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1842.0,
                              st.measurements.find(measurement_id::kAcPowerTotal)->value);
}

static void test_recovery_after_prolonged_outage() {
    auto            st = makeState();
    StalenessPolicy p;
    for (int i = 0; i < 20; ++i) {
        st.recordPollFailure(11000 + i * 10000, p);
    }
    TEST_ASSERT_FALSE(st.inverterOnline);

    st.measurements.set(measurement_id::kAcPowerTotal, 12.0, 300000);
    st.recordPollSuccess(300000, p);

    TEST_ASSERT_TRUE(st.inverterOnline);
    TEST_ASSERT_TRUE(st.dataValid);
    TEST_ASSERT_FALSE(st.dataStale);
    TEST_ASSERT_EQUAL_UINT32(0, st.consecutiveFailures);
    TEST_ASSERT_FALSE(st.measurements.find(measurement_id::kAcPowerTotal)->stale);
}

static void test_error_code_is_unsupported_by_default() {
    // The EverSolar protocol has no readable error code field, so 0 must not be published
    // as "no fault".
    const DeviceState st;
    TEST_ASSERT_FALSE(st.errorCodeSupported);
}

// --- capabilities ---------------------------------------------------------------------------

static void test_readonly_capabilities_have_no_write_bits() {
    InverterCapabilities caps;
    caps.addRead(InverterCapability::ReadAcPower);
    caps.addRead(InverterCapability::ReadEnergyTotal);

    TEST_ASSERT_TRUE(caps.isReadOnly());
    TEST_ASSERT_TRUE(caps.has(InverterCapability::ReadAcPower));
    TEST_ASSERT_FALSE(caps.has(InverterCapability::ReadBatteryState));
    TEST_ASSERT_FALSE(caps.canWrite(InverterCapability::SetActivePowerLimit));
}

static void test_any_write_bit_makes_it_not_readonly() {
    InverterCapabilities caps;
    caps.addWrite(InverterCapability::SetActivePowerLimit);
    TEST_ASSERT_FALSE(caps.isReadOnly());
}

static void test_command_types_map_to_capabilities() {
    TEST_ASSERT_EQUAL(InverterCapability::SetActivePowerLimit,
                      requiredCapability(InverterCommandType::SetActivePowerLimitWatts));
    TEST_ASSERT_EQUAL(InverterCapability::SetActivePowerLimit,
                      requiredCapability(InverterCommandType::SetActivePowerLimitPercent));
    TEST_ASSERT_EQUAL(InverterCapability::StartStop, requiredCapability(InverterCommandType::Stop));
    TEST_ASSERT_EQUAL(InverterCapability::SetMinimumSoc,
                      requiredCapability(InverterCommandType::SetMinimumSoc));
}

static void test_every_command_type_maps_to_a_real_capability() {
    // Guards against a new command type silently getting no capability gate, which would
    // let it slip past the dispatcher's check.
    for (size_t i = 0; i < kCommandTypeCount; ++i) {
        const auto type = static_cast<InverterCommandType>(i);
        TEST_ASSERT_NOT_EQUAL(InverterCapability::_Count, requiredCapability(type));
    }
}

static void test_every_command_type_has_a_name() {
    for (size_t i = 0; i < kCommandTypeCount; ++i) {
        const auto type = static_cast<InverterCommandType>(i);
        TEST_ASSERT_TRUE(std::strcmp(commandTypeName(type), "unknown") != 0);
    }
}

static void test_unit_symbols() {
    TEST_ASSERT_EQUAL_STRING("W", unitSymbol(Unit::Watt));
    TEST_ASSERT_EQUAL_STRING("kWh", unitSymbol(Unit::KilowattHour));
    TEST_ASSERT_EQUAL_STRING("", unitSymbol(Unit::None));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_declared_channel_starts_supported_but_invalid);
    RUN_TEST(test_set_records_value_and_marks_valid);
    RUN_TEST(test_undeclared_channel_is_ignored);
    RUN_TEST(test_redeclaring_keeps_the_value);
    RUN_TEST(test_derived_flag_survives);
    RUN_TEST(test_zero_is_a_real_value_not_an_absence);
    RUN_TEST(test_declare_unsupported_marks_the_channel_unreadable);
    RUN_TEST(test_an_unsupported_channel_refuses_values);
    RUN_TEST(test_redeclaring_an_unsupported_channel_makes_it_supported_again);
    RUN_TEST(test_invalidate_keeps_channel_supported);
    RUN_TEST(test_staleness_is_based_on_age);
    RUN_TEST(test_stale_value_is_retained);
    RUN_TEST(test_fresh_reading_clears_stale);
    RUN_TEST(test_staleness_ignores_invalid_channels);
    RUN_TEST(test_staleness_survives_a_clock_that_went_backwards);
    RUN_TEST(test_successful_poll_marks_everything_healthy);
    RUN_TEST(test_one_failed_poll_keeps_data_valid);
    RUN_TEST(test_three_failed_polls_mark_data_stale_but_still_valid);
    RUN_TEST(test_ten_failed_polls_take_the_inverter_offline);
    RUN_TEST(test_recovery_after_prolonged_outage);
    RUN_TEST(test_error_code_is_unsupported_by_default);
    RUN_TEST(test_readonly_capabilities_have_no_write_bits);
    RUN_TEST(test_any_write_bit_makes_it_not_readonly);
    RUN_TEST(test_command_types_map_to_capabilities);
    RUN_TEST(test_every_command_type_maps_to_a_real_capability);
    RUN_TEST(test_every_command_type_has_a_name);
    RUN_TEST(test_unit_symbols);
    return UNITY_END();
}
