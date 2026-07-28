// SPDX-License-Identifier: MIT
// Which configured devices get started, and which are refused before they can do damage.
//
// This logic lived in setup(), the one file the host build does not compile, so the rule it
// enforces had no test at all -- and it is not a cosmetic rule. For a driver that cannot share
// a bus, begin() IS the damage: the AA55 handshake opens with a bus-wide RE_REGISTER broadcast,
// so a second instance starting up tells the first, already-polling inverter to forget its
// address. Every case below describes a bus that would be corrupted if the answer were wrong.

#include <unity.h>

#include <string>

#include "app/device_plan.h"
#include "config/configuration.h"
#include "drivers/driver_registry.h"

using namespace heliograph;

void setUp() {}
void tearDown() {}

namespace {

/// A registry with two drivers: one that can share the bus and one that cannot.
DriverRegistry makeRegistry() {
    DriverRegistry registry;

    DriverDescriptor shareable;
    shareable.id                     = "growatt_modbus";
    shareable.supportsMultipleDevices = true;
    registry.registerDriver(shareable, [](Transport&, const DriverOptions&) { return nullptr; });

    DriverDescriptor exclusive;
    exclusive.id                      = "eversolar_legacy";
    exclusive.supportsMultipleDevices = false;
    registry.registerDriver(exclusive, [](Transport&, const DriverOptions&) { return nullptr; });

    return registry;
}

/// A configuration with `driver` set and however many additional devices are named.
Configuration configWith(const std::string& first, const std::vector<std::string>& extra,
                         const std::vector<std::string>& labels = {}) {
    Configuration config;
    config.driver.id = first;
    for (size_t i = 0; i < extra.size(); ++i) {
        DriverSettings device;
        device.id    = extra[i];
        device.label = i < labels.size() ? labels[i] : "";
        config.additionalDevices.push_back(device);
    }
    return config;
}

}  // namespace

static void test_a_driver_that_shares_the_bus_may_appear_many_times() {
    const auto registry = makeRegistry();
    const auto plan     = app::planDevices(
        configWith("growatt_modbus", {"growatt_modbus", "growatt_modbus"}), "growatt_modbus",
        registry);

    // Three MIC TL-X on one bus is the case this bridge was extended for. Refusing any of them
    // would be the bug.
    TEST_ASSERT_EQUAL_UINT32(3, plan.size());
    for (const auto& device : plan) {
        TEST_ASSERT_TRUE(device.shouldStart());
        TEST_ASSERT_TRUE(device.problem.empty());
    }
}

static void test_a_second_exclusive_device_is_refused_before_it_can_speak() {
    const auto registry = makeRegistry();
    const auto plan     = app::planDevices(configWith("eversolar_legacy", {"eversolar_legacy"}),
                                           "eversolar_legacy", registry);

    TEST_ASSERT_EQUAL_UINT32(2, plan.size());
    // The FIRST one keeps the bus. Which one is refused is not arbitrary: it decides which
    // inverter goes on reporting and which goes quiet.
    TEST_ASSERT_TRUE(plan[0].shouldStart());
    TEST_ASSERT_FALSE(plan[1].shouldStart());
    TEST_ASSERT_TRUE(plan[1].problem.find("only one device per bridge") != std::string::npos);
}

static void test_a_third_copy_is_refused_too_and_names_its_own_row() {
    const auto registry = makeRegistry();
    const auto plan     = app::planDevices(
        configWith("eversolar_legacy", {"eversolar_legacy", "eversolar_legacy"}),
        "eversolar_legacy", registry);

    TEST_ASSERT_TRUE(plan[0].shouldStart());
    TEST_ASSERT_FALSE(plan[1].shouldStart());
    TEST_ASSERT_FALSE(plan[2].shouldStart());
    // Each refusal names its OWN row. Three inverters sharing a driver id produced three
    // identical messages before rows were carried, which is useless on the very bus this
    // exists for.
    TEST_ASSERT_TRUE(plan[1].problem.find("device 2") != std::string::npos);
    TEST_ASSERT_TRUE(plan[2].problem.find("device 3") != std::string::npos);
}

static void test_the_label_goes_in_the_refusal() {
    const auto registry = makeRegistry();
    const auto plan     = app::planDevices(
        configWith("eversolar_legacy", {"eversolar_legacy"}, {"Schuur"}), "eversolar_legacy",
        registry);

    // "device 2 could not be started" sends someone to count rows on a settings page.
    // "device 2 (Schuur)" sends them to the shed.
    TEST_ASSERT_TRUE(plan[1].problem.find("device 2 (Schuur)") != std::string::npos);
}

static void test_an_unknown_driver_is_planned_and_left_to_fail_later() {
    const auto registry = makeRegistry();
    const auto plan     = app::planDevices(configWith("not_a_driver", {"not_a_driver"}),
                                           "not_a_driver", registry);

    // The registry has no opinion, so this rule has none either: both are planned, and both
    // fail at create() with their own message. Refusing here would report the wrong reason --
    // "only one per bridge" for a driver that does not exist.
    TEST_ASSERT_EQUAL_UINT32(2, plan.size());
    TEST_ASSERT_TRUE(plan[0].shouldStart());
    TEST_ASSERT_TRUE(plan[1].shouldStart());
}

static void test_two_different_exclusive_drivers_do_not_collide() {
    const auto registry = makeRegistry();
    const auto plan     = app::planDevices(configWith("eversolar_legacy", {"growatt_modbus"}),
                                           "eversolar_legacy", registry);

    // The rule is per DRIVER, not per bus. Two different drivers each get their one instance.
    TEST_ASSERT_TRUE(plan[0].shouldStart());
    TEST_ASSERT_TRUE(plan[1].shouldStart());
}

static void test_an_empty_driver_id_leaves_it_out_of_the_plan() {
    const auto registry = makeRegistry();
    const auto plan     = app::planDevices(configWith("", {"growatt_modbus"}), "", registry);

    // Nothing configured and nothing compiled in: the additional device is device 1, not
    // device 2 with a hole where the first should be.
    TEST_ASSERT_EQUAL_UINT32(1, plan.size());
    TEST_ASSERT_EQUAL_STRING("growatt_modbus", plan[0].id.c_str());
    TEST_ASSERT_EQUAL_UINT32(1, plan[0].row);
}

static void test_rows_are_numbered_as_the_settings_page_shows_them() {
    const auto registry = makeRegistry();
    const auto plan     = app::planDevices(
        configWith("growatt_modbus", {"growatt_modbus", "growatt_modbus"}), "growatt_modbus",
        registry);

    TEST_ASSERT_EQUAL_UINT32(1, plan[0].row);
    TEST_ASSERT_EQUAL_UINT32(2, plan[1].row);
    TEST_ASSERT_EQUAL_UINT32(3, plan[2].row);
}

static void test_describe_row_names_the_label_when_there_is_one() {
    TEST_ASSERT_EQUAL_STRING("device 1", app::describeRow(1, "").c_str());
    TEST_ASSERT_EQUAL_STRING("device 2 (Schuur)", app::describeRow(2, "Schuur").c_str());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_a_driver_that_shares_the_bus_may_appear_many_times);
    RUN_TEST(test_a_second_exclusive_device_is_refused_before_it_can_speak);
    RUN_TEST(test_a_third_copy_is_refused_too_and_names_its_own_row);
    RUN_TEST(test_the_label_goes_in_the_refusal);
    RUN_TEST(test_an_unknown_driver_is_planned_and_left_to_fail_later);
    RUN_TEST(test_two_different_exclusive_drivers_do_not_collide);
    RUN_TEST(test_an_empty_driver_id_leaves_it_out_of_the_plan);
    RUN_TEST(test_rows_are_numbered_as_the_settings_page_shows_them);
    RUN_TEST(test_describe_row_names_the_label_when_there_is_one);
    return UNITY_END();
}
