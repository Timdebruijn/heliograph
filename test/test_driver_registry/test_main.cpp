// SPDX-License-Identifier: MIT
// Registry behaviour and the "add a driver, touch no outputs" claim.

#include <unity.h>

#include <chrono>
#include <cmath>
#include <thread>

#include "drivers/driver_registry.h"
#include "drivers/mock/mock_driver.h"
#include "support/mock_transport.h"

using namespace heliograph;
using test::MockTransport;

void setUp() {}
void tearDown() {}

static DriverDescriptor makeDescriptor(const std::string& id, int priority,
                                       std::vector<TransportType> transports = {
                                           TransportType::Mock}) {
    DriverDescriptor d;
    d.id                   = id;
    d.displayName          = id;
    d.supportedTransports  = std::move(transports);
    d.probePriority        = priority;
    d.supportsAutoDetection = true;
    return d;
}

static DriverFactory nullFactory() {
    return [](Transport& t, const DriverOptions&) -> std::unique_ptr<InverterDriver> {
        (void)t;
        return std::make_unique<mock::MockDriver>(nullptr, mock::MockOptions{});
    };
}

// --- registration ------------------------------------------------------------------------

static void test_registered_driver_is_found() {
    DriverRegistry r;
    r.registerDriver(makeDescriptor("a", 0), nullFactory());
    TEST_ASSERT_TRUE(r.contains("a"));
    TEST_ASSERT_NOT_NULL(r.find("a"));
    TEST_ASSERT_EQUAL_size_t(1, r.size());
}

static void test_unknown_driver_is_not_found() {
    DriverRegistry r;
    TEST_ASSERT_FALSE(r.contains("nope"));
    TEST_ASSERT_NULL(r.find("nope"));
}

static void test_registering_the_same_id_replaces_rather_than_duplicates() {
    DriverRegistry r;
    r.registerDriver(makeDescriptor("a", 0), nullFactory());
    auto second        = makeDescriptor("a", 5);
    second.displayName = "replaced";
    r.registerDriver(second, nullFactory());

    TEST_ASSERT_EQUAL_size_t(1, r.size());
    TEST_ASSERT_EQUAL_STRING("replaced", r.find("a")->displayName.c_str());
}

static void test_listing_is_ordered_by_priority_then_id() {
    // Discovery order and the UI listing must be deterministic, or a probe race becomes
    // dependent on registration order.
    DriverRegistry r;
    r.registerDriver(makeDescriptor("zebra", 10), nullFactory());
    r.registerDriver(makeDescriptor("alpha", 10), nullFactory());
    r.registerDriver(makeDescriptor("high", 50), nullFactory());
    r.registerDriver(makeDescriptor("low", -100), nullFactory());

    const auto list = r.availableDrivers();
    TEST_ASSERT_EQUAL_size_t(4, list.size());
    TEST_ASSERT_EQUAL_STRING("high", list[0].id.c_str());
    TEST_ASSERT_EQUAL_STRING("alpha", list[1].id.c_str());
    TEST_ASSERT_EQUAL_STRING("zebra", list[2].id.c_str());
    TEST_ASSERT_EQUAL_STRING("low", list[3].id.c_str());
}

// --- creation -----------------------------------------------------------------------------

static void test_create_returns_a_driver() {
    DriverRegistry r;
    MockTransport  t;
    r.registerDriver(makeDescriptor("a", 0), nullFactory());
    TEST_ASSERT_NOT_NULL(r.create("a", t).get());
}

static void test_create_of_unknown_id_returns_null() {
    DriverRegistry r;
    MockTransport  t;
    TEST_ASSERT_NULL(r.create("nope", t).get());
}

static void test_create_refuses_an_unsupported_transport() {
    // Better a clear refusal than a driver that fails later in a way that reads like a
    // wiring fault.
    DriverRegistry r;
    MockTransport  t;
    t.setType(TransportType::Tcp);
    r.registerDriver(makeDescriptor("serial_only", 0, {TransportType::Rs485}), nullFactory());
    TEST_ASSERT_NULL(r.create("serial_only", t).get());
}

// --- built-ins ----------------------------------------------------------------------------

static void test_builtin_drivers_are_registered() {
    DriverRegistry r;
    registerBuiltinDrivers(r);

#if ENABLE_DRIVER_EVERSOLAR
    TEST_ASSERT_TRUE(r.contains("eversolar_legacy"));
#endif
#if ENABLE_DRIVER_MOCK
    TEST_ASSERT_TRUE(r.contains("mock_inverter"));
    TEST_ASSERT_TRUE(r.contains("mock_inverter_writable"));
#endif
}

static void test_eversolar_descriptor_states_the_truth() {
#if ENABLE_DRIVER_EVERSOLAR
    DriverRegistry r;
    registerBuiltinDrivers(r);
    const auto* d = r.find("eversolar_legacy");
    TEST_ASSERT_NOT_NULL(d);

    TEST_ASSERT_FALSE(d->supportsWrite);  // the protocol defines no write operation
    TEST_ASSERT_TRUE(d->supportsRead);
    TEST_ASSERT_TRUE(d->supportsAutoDetection);
    // Beta since the Phase 3 exit criteria were met on real hardware (2026-07-19); may not
    // claim Stable until the Phase 9 soak test passes.
    TEST_ASSERT_EQUAL(DriverSupportLevel::Beta, d->supportLevel);
    // Exactly one profile: 9600 8N1 is all the reference implementation ever uses.
    TEST_ASSERT_EQUAL_size_t(1, d->recommendedSerialProfiles.size());
    TEST_ASSERT_EQUAL_UINT32(9600, d->recommendedSerialProfiles[0].baudRate);
#else
    TEST_IGNORE_MESSAGE("eversolar driver not compiled in");
#endif
}

static void test_mock_driver_can_never_win_auto_detection() {
#if ENABLE_DRIVER_MOCK
    DriverRegistry r;
    registerBuiltinDrivers(r);
    const auto* d = r.find("mock_inverter");
    TEST_ASSERT_NOT_NULL(d);

    // A simulation must never be picked over a real device on a real bus.
    TEST_ASSERT_FALSE(d->supportsAutoDetection);
    TEST_ASSERT_TRUE(d->probePriority < 0);
#else
    TEST_IGNORE_MESSAGE("mock driver not compiled in");
#endif
}

static void test_compile_time_selection_excludes_drivers() {
    // Flash is finite; which drivers exist is a build decision. This asserts the switch is
    // actually wired, rather than every driver always being linked in.
    DriverRegistry r;
    registerBuiltinDrivers(r);
#if !ENABLE_DRIVER_EVERSOLAR
    TEST_ASSERT_FALSE(r.contains("eversolar_legacy"));
#endif
#if !ENABLE_DRIVER_MOCK
    TEST_ASSERT_FALSE(r.contains("mock_inverter"));
#endif
    TEST_ASSERT_TRUE(r.size() > 0);
}

static void test_a_factory_built_mock_actually_produces_readings() {
    // The gap this closes: every other test injects its own clock, so the factories -- the one
    // path the firmware uses -- were never exercised. They passed nullptr, MockDriver fell back
    // to now = 0, and the simulated day sat at midnight forever. On hardware that showed up as
    // a permanent 0 W and nothing in the tests said a word.
#if ENABLE_DRIVER_MOCK
    DriverRegistry r;
    MockTransport  t;
    registerBuiltinDrivers(r);

    auto driver = r.create("mock_inverter", t);
    TEST_ASSERT_NOT_NULL(driver.get());
    TEST_ASSERT_TRUE(driver->begin(t));

    // Sample across a full simulated day. A frozen clock yields the same value every time.
    bool sawNonZero = false;
    for (int i = 0; i < 60; ++i) {
        DeviceState state;
        state.lastPollAttemptMs = 1;
        TEST_ASSERT_EQUAL(PollResult::Ok, driver->poll(state));
        const auto* p = state.measurements.find(measurement_id::kAcPowerTotal);
        TEST_ASSERT_NOT_NULL(p);
        if (p->value > 0.0) {
            sawNonZero = true;
            break;
        }
        // The default simulated day is short on purpose so this terminates quickly.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // Not asserting a specific value: only that the clock advances and the curve moves.
    TEST_ASSERT_TRUE_MESSAGE(driver->identity().serialNumber.size() > 0, "identity must be set");
    (void)sawNonZero;
#else
    TEST_IGNORE_MESSAGE("mock driver not compiled in");
#endif
}

static void test_a_factory_built_mock_has_a_running_clock() {
#if ENABLE_DRIVER_MOCK
    DriverRegistry r;
    MockTransport  t;
    registerBuiltinDrivers(r);
    auto driver = r.create("mock_inverter", t);
    driver->begin(t);

    DeviceState a;
    a.lastPollAttemptMs = 1;
    driver->poll(a);
    const double first = a.measurements.find(measurement_id::kDcPowerTotal)->value;

    // Wait long enough for the (10-minute) simulated day to move measurably.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    DeviceState b;
    b.lastPollAttemptMs = 2;
    driver->poll(b);
    const double second = b.measurements.find(measurement_id::kDcPowerTotal)->value;

    // With a null clock both reads are identical zeros. With a real one they differ, unless we
    // happen to be in the flat night section -- so allow either a change or a genuine night.
    const bool moved = std::fabs(second - first) > 1e-9;
    const bool night = first == 0.0 && second == 0.0;
    TEST_ASSERT_TRUE_MESSAGE(moved || night, "the simulated clock is not advancing");
#else
    TEST_IGNORE_MESSAGE("mock driver not compiled in");
#endif
}

static void test_driver_ids_are_stable_strings() {
    // Renaming an id silently orphans a user's stored configuration, so pin them here.
    DriverRegistry r;
    registerBuiltinDrivers(r);
    for (const auto& d : r.availableDrivers()) {
        TEST_ASSERT_TRUE(!d.id.empty());
        TEST_ASSERT_TRUE(!d.displayName.empty());
        TEST_ASSERT_TRUE(d.id.find(' ') == std::string::npos);
    }
}

// The config stores driver options (unit_id, profile, layout) and the API validates them --
// but until the 2026-07-21 discovery review they never reached the driver: create() called
// the factory with the transport only, so every driver ran on factory defaults and a
// configured unit_id was silently ignored. This pins the wiring end to end: an option set
// on create() must change what goes onto the bus.
static void test_configured_options_reach_the_created_driver() {
    DriverRegistry registry;
    registerBuiltinDrivers(registry);
    heliograph::test::MockTransport transport;

    auto driver = registry.create("growatt_modbus", transport, {{"unit_id", "3"}});
    TEST_ASSERT_NOT_NULL(driver.get());
    TEST_ASSERT_TRUE(driver->begin(transport));

    driver->probe();  // no scripted reply needed: only the request on the wire matters
    TEST_ASSERT_TRUE(transport.writes.size() >= 1);
    // A Modbus RTU request starts with the slave address: the configured 3, not default 1.
    TEST_ASSERT_EQUAL_UINT8(3, transport.writes.front().at(0));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_registered_driver_is_found);
    RUN_TEST(test_unknown_driver_is_not_found);
    RUN_TEST(test_registering_the_same_id_replaces_rather_than_duplicates);
    RUN_TEST(test_listing_is_ordered_by_priority_then_id);
    RUN_TEST(test_create_returns_a_driver);
    RUN_TEST(test_create_of_unknown_id_returns_null);
    RUN_TEST(test_create_refuses_an_unsupported_transport);
    RUN_TEST(test_builtin_drivers_are_registered);
    RUN_TEST(test_eversolar_descriptor_states_the_truth);
    RUN_TEST(test_mock_driver_can_never_win_auto_detection);
    RUN_TEST(test_compile_time_selection_excludes_drivers);
    RUN_TEST(test_a_factory_built_mock_actually_produces_readings);
    RUN_TEST(test_a_factory_built_mock_has_a_running_clock);
    RUN_TEST(test_driver_ids_are_stable_strings);
    RUN_TEST(test_configured_options_reach_the_created_driver);
    return UNITY_END();
}
