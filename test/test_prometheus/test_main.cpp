// SPDX-License-Identifier: MIT
// The Prometheus exposition: which channels reach a scraper, and in what shape.
//
// This output had no suite of its own. It was exercised sideways from test_rest, which is how
// it went unnoticed that it exported 8 of the 33 channels the model carries -- both MPPT
// strings, every battery reading, the second and third phase and the operating hours reached
// the REST API and MQTT and stopped here.
//
// The shape matters as much as the coverage. A metric name is an external contract: once it is
// in somebody's dashboard and alert rules, it is not a name any more, it is an interface.

#include <unity.h>

#include <string>

#include "device/bridge_info.h"
#include "device/device_state.h"
#include "device/measurement.h"
#include "diagnostics/diagnostics.h"
#include "outputs/prometheus/prometheus_metrics.h"

using namespace heliograph;

void setUp() {}
void tearDown() {}

namespace {

BridgeInfo makeBridge() {
    BridgeInfo b;
    b.firmwareVersion = "0.19.1";
    b.boardName       = "rs485-can";
    return b;
}

/// Declares a channel and gives it a reading in one step, because a declared channel with no
/// value is not what a scraper sees.
void report(DeviceState& s, const char* id, MeasurementType type, Unit unit, const char* name,
            double value) {
    s.measurements.declare(id, type, unit, name);
    s.measurements.set(id, value, 1000);
}

/// A single-phase inverter with one string: the shape of Tim's TL3000.
DeviceState singlePhase() {
    DeviceState s;
    s.identity.driverId = "eversolar_legacy";
    report(s, measurement_id::kAcPowerTotal, MeasurementType::Power, Unit::Watt, "AC Power", 2070);
    report(s, measurement_id::kAcL1Voltage, MeasurementType::Voltage, Unit::Volt, "V L1", 240.5);
    report(s, measurement_id::kAcL1Current, MeasurementType::Current, Unit::Ampere, "A L1", 8.4);
    report(s, measurement_id::kDcMppt1Voltage, MeasurementType::Voltage, Unit::Volt, "MPPT1 V",
           322.0);
    report(s, measurement_id::kDcMppt1Power, MeasurementType::Power, Unit::Watt, "MPPT1 W",
           2125.2);
    report(s, measurement_id::kOperatingHours, MeasurementType::Duration, Unit::Hour, "Hours",
           47547);
    s.dataValid = true;
    return s;
}

/// Three phases and a battery: everything the single-phase case does not have.
DeviceState hybrid() {
    DeviceState s = singlePhase();
    s.identity.driverId = "growatt_modbus";
    report(s, measurement_id::kAcL2Voltage, MeasurementType::Voltage, Unit::Volt, "V L2", 238.1);
    report(s, measurement_id::kAcL3Voltage, MeasurementType::Voltage, Unit::Volt, "V L3", 239.9);
    report(s, measurement_id::kBatterySoc, MeasurementType::Ratio, Unit::Percent, "SOC", 64);
    report(s, measurement_id::kBatteryPower, MeasurementType::Power, Unit::Watt, "Batt W", -1180);
    report(s, measurement_id::kBatteryTemperature, MeasurementType::Temperature, Unit::Celsius,
           "Batt °C", 28.4);
    return s;
}

std::string metricsOf(const DeviceState& s, const char* id = "inv-1") {
    DiagnosticsSnapshot d;
    return prometheus::buildMetrics({{id, &s}}, makeBridge(), d);
}

size_t occurrences(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

}  // namespace

static void test_the_channels_that_used_to_be_dropped_are_exported() {
    const std::string text = metricsOf(singlePhase());
    // Each of these reached the REST API and MQTT and never Prometheus.
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_mppt_voltage_volts{device=\"inv-1\","
                               "string=\"1\"} 322.000") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_mppt_power_watts{device=\"inv-1\","
                               "string=\"1\"} 2125.200") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_operating_hours{device=\"inv-1\"} 47547.000")
                     != std::string::npos);
}

static void test_a_phase_is_a_label_not_a_name() {
    const std::string text = metricsOf(hybrid());
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_ac_voltage_volts{device=\"inv-1\","
                               "phase=\"l1\"} 240.500") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_ac_voltage_volts{device=\"inv-1\","
                               "phase=\"l3\"} 239.900") != std::string::npos);
    // The number must not have leaked into the metric name, which would make the three phases
    // three families that cannot be summed without naming all three.
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_ac_voltage_l2_volts") == std::string::npos);
}

static void test_one_help_and_one_type_however_many_phases() {
    const std::string text = metricsOf(hybrid());
    // Three series share this family. Repeating HELP or TYPE is a parse error in strict
    // parsers, and grouping the table by name is the only thing preventing it.
    TEST_ASSERT_EQUAL_UINT32(1, occurrences(text, "# HELP heliograph_inverter_ac_voltage_volts"));
    TEST_ASSERT_EQUAL_UINT32(1, occurrences(text, "# TYPE heliograph_inverter_ac_voltage_volts"));
    TEST_ASSERT_EQUAL_UINT32(3, occurrences(text, "heliograph_inverter_ac_voltage_volts{device="));
}

static void test_a_battery_is_its_own_metric_prefix() {
    const std::string text = metricsOf(hybrid());
    TEST_ASSERT_TRUE(text.find("heliograph_battery_state_of_charge_percent{device=\"inv-1\"} "
                               "64.000") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_battery_power_watts{device=\"inv-1\"} -1180.000")
                     != std::string::npos);
    // A query for the inverter's temperature must not have to exclude the battery's.
    TEST_ASSERT_TRUE(text.find("heliograph_battery_temperature_celsius") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_temperature_celsius") == std::string::npos);
}

static void test_a_channel_the_inverter_lacks_produces_nothing_at_all() {
    const std::string text = metricsOf(singlePhase());
    // Not a zero, and not a bare HELP/TYPE with no samples under it -- which is the shape the
    // lazy header exists to prevent and the reason the loop walks families rather than devices.
    TEST_ASSERT_TRUE(text.find("heliograph_battery_state_of_charge_percent") == std::string::npos);
    TEST_ASSERT_TRUE(text.find("# HELP heliograph_battery_power_watts") == std::string::npos);
    TEST_ASSERT_TRUE(text.find("phase=\"l2\"") == std::string::npos);
    TEST_ASSERT_TRUE(text.find("string=\"2\"") == std::string::npos);
}

static void test_the_array_total_is_not_in_the_same_family_as_its_strings() {
    const std::string text = metricsOf(singlePhase());
    // Parts and their sum in one family is how a sum() silently double-counts.
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_mppt_power_watts{device=\"inv-1\","
                               "string=\"1\"}") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_ac_power_watts{device=\"inv-1\"} 2070.000")
                     != std::string::npos);
    // The whole-inverter AC total carries no phase label, so it can never be mistaken for one.
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_ac_power_watts{device=\"inv-1\",phase=")
                     == std::string::npos);
}

static void test_an_invalidated_reading_is_omitted_rather_than_reported() {
    DeviceState s = singlePhase();
    s.measurements.invalidate(measurement_id::kDcMppt1Power);
    const std::string text = metricsOf(s);
    // Omitted, never zeroed: a 0 W string reads as a dead panel, which is a different fault
    // from a reading the bridge does not currently have.
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_mppt_power_watts") == std::string::npos);
    // The rest of the array survives: validity is per channel, not per metric family.
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_mppt_voltage_volts") != std::string::npos);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_the_channels_that_used_to_be_dropped_are_exported);
    RUN_TEST(test_a_phase_is_a_label_not_a_name);
    RUN_TEST(test_one_help_and_one_type_however_many_phases);
    RUN_TEST(test_a_battery_is_its_own_metric_prefix);
    RUN_TEST(test_a_channel_the_inverter_lacks_produces_nothing_at_all);
    RUN_TEST(test_the_array_total_is_not_in_the_same_family_as_its_strings);
    RUN_TEST(test_an_invalidated_reading_is_omitted_rather_than_reported);
    return UNITY_END();
}
