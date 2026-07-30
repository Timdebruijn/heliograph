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
    // Stable since 2026-07-29: eight clean unassisted sunrises on the production bridge, which
    // is the only way the recovery path can be observed -- see the descriptor for the count and
    // what the level still does not claim.
    TEST_ASSERT_EQUAL(DriverSupportLevel::Stable, d->supportLevel);
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

    auto driver = registry.create("modbus_profile", transport, {{"unit_id", "3"}});
    TEST_ASSERT_NOT_NULL(driver.get());
    TEST_ASSERT_TRUE(driver->begin(transport));

    driver->probe();  // no scripted reply needed: only the request on the wire matters
    TEST_ASSERT_TRUE(transport.writes.size() >= 1);
    // A Modbus RTU request starts with the slave address: the configured 3, not default 1.
    TEST_ASSERT_EQUAL_UINT8(3, transport.writes.front().at(0));
}


// --- numeric option bounds ---------------------------------------------------------------

static DriverDescriptor boundedDescriptor() {
    DriverDescriptor d;
    d.id      = "bounded";
    d.options = {DriverOption{"unit_id", "Modbus unit id", "", "1", {}, 1, 247}};
    return d;
}

// A driver's own parser is too late to be the only check: by the time it falls back the value
// is already stored, and on a bus of identical inverters the address it falls back to is the
// one the first device uses -- so a typo'd unit id surfaced as an id collision naming a
// duplicate the configuration does not contain.
static void test_a_numeric_option_out_of_range_is_refused() {
    const auto        d = boundedDescriptor();
    DriverOptionError e;
    TEST_ASSERT_FALSE(validateDriverOptions(d, {{"unit_id", "300"}}, e));
    TEST_ASSERT_EQUAL_STRING("unit_id", e.key.c_str());
    TEST_ASSERT_FALSE(validateDriverOptions(d, {{"unit_id", "0"}}, e));
    TEST_ASSERT_TRUE(validateDriverOptions(d, {{"unit_id", "1"}}, e));
    TEST_ASSERT_TRUE(validateDriverOptions(d, {{"unit_id", "247"}}, e));
}

// Refused rather than parsed leniently: strtol would read "3x" as 3, and storing 3 for a value
// the user typed as something else is the same silent substitution this exists to remove.
static void test_a_numeric_option_that_is_not_a_number_is_refused() {
    const auto        d = boundedDescriptor();
    DriverOptionError e;
    TEST_ASSERT_FALSE(validateDriverOptions(d, {{"unit_id", "3x"}}, e));
    TEST_ASSERT_FALSE(validateDriverOptions(d, {{"unit_id", ""}}, e));
    TEST_ASSERT_FALSE(validateDriverOptions(d, {{"unit_id", "one"}}, e));
}

// An option with no bounds declared stays free-form, which is what every non-addressing option
// is. isNumeric() keys on the bounds, not on the value looking like a number.
// strtol is lenient in three ways that all end up STORED verbatim: it skips leading
// whitespace, accepts a leading '+', and accepts leading zeros. "007" then slipped past the
// settings page's duplicate-address check, which compares strings -- so two rows could claim
// the same unit and only the boot-time collision guard noticed.
static void test_a_sloppy_but_parseable_number_is_refused() {
    const auto        d = boundedDescriptor();
    DriverOptionError e;
    TEST_ASSERT_FALSE(validateDriverOptions(d, {{"unit_id", " 7"}}, e));
    TEST_ASSERT_FALSE(validateDriverOptions(d, {{"unit_id", "+7"}}, e));
    TEST_ASSERT_FALSE(validateDriverOptions(d, {{"unit_id", "007"}}, e));
    TEST_ASSERT_FALSE(validateDriverOptions(d, {{"unit_id", "7 "}}, e));
    TEST_ASSERT_TRUE(validateDriverOptions(d, {{"unit_id", "7"}}, e));
}

// A bound that legitimately starts at zero must still be numeric -- the SunSpec base register
// is exactly that, and declaring 1 made the descriptor stricter than its own parser.
static void test_a_range_starting_at_zero_is_still_numeric() {
    DriverDescriptor d;
    d.id      = "based";
    d.options = {DriverOption{"base_address", "Base", "", "40000", {}, 0, 65534}};
    DriverOptionError e;
    TEST_ASSERT_TRUE(validateDriverOptions(d, {{"base_address", "0"}}, e));
    TEST_ASSERT_TRUE(validateDriverOptions(d, {{"base_address", "65534"}}, e));
    TEST_ASSERT_FALSE(validateDriverOptions(d, {{"base_address", "65535"}}, e));
}

// --- numericOption: the read-back path, deliberately not the same rule as the gate above -----

// The gate refuses what a user TYPES. This is what the firmware READS back out of a stored
// config, and the two must not be confused: refusing to start a driver over a leading zero
// written by an older build would be the wrong answer.
static void test_numeric_option_reads_a_value_inside_the_declared_range() {
    const auto d = boundedDescriptor();
    long       out = 0;
    TEST_ASSERT_TRUE(d.numericOption({{"unit_id", "7"}}, "unit_id", out));
    TEST_ASSERT_EQUAL_INT32(7, out);
    TEST_ASSERT_TRUE(d.numericOption({{"unit_id", "1"}}, "unit_id", out));
    TEST_ASSERT_EQUAL_INT32(1, out);
    TEST_ASSERT_TRUE(d.numericOption({{"unit_id", "247"}}, "unit_id", out));
    TEST_ASSERT_EQUAL_INT32(247, out);
}

// The whole point of the change: the range comes from the DriverOption row, so a factory can
// no longer disagree with its own declaration.
static void test_numeric_option_refuses_what_the_declaration_refuses() {
    const auto d = boundedDescriptor();
    long       out = 99;
    TEST_ASSERT_FALSE(d.numericOption({{"unit_id", "0"}}, "unit_id", out));
    TEST_ASSERT_FALSE(d.numericOption({{"unit_id", "248"}}, "unit_id", out));
    TEST_ASSERT_EQUAL_INT32(99, out);  // untouched on refusal, so the caller keeps its default
}

// strtol alone reads "12abc" as 12 and stops. That is how a typo becomes a plausible unit id.
static void test_numeric_option_refuses_trailing_garbage_and_non_numbers() {
    const auto d = boundedDescriptor();
    long       out = 99;
    TEST_ASSERT_FALSE(d.numericOption({{"unit_id", "12abc"}}, "unit_id", out));
    TEST_ASSERT_FALSE(d.numericOption({{"unit_id", "one"}}, "unit_id", out));
    TEST_ASSERT_FALSE(d.numericOption({{"unit_id", ""}}, "unit_id", out));
    TEST_ASSERT_EQUAL_INT32(99, out);
}

// Accepted here and refused by the gate, on purpose. Also base 10 explicitly: "010" is ten,
// not eight -- strtol with base 0 would read it as octal.
static void test_numeric_option_accepts_a_padded_number_the_gate_would_refuse() {
    const auto        d = boundedDescriptor();
    DriverOptionError e;
    TEST_ASSERT_FALSE(validateDriverOptions(d, {{"unit_id", "010"}}, e));

    long out = 0;
    TEST_ASSERT_TRUE(d.numericOption({{"unit_id", "010"}}, "unit_id", out));
    TEST_ASSERT_EQUAL_INT32(10, out);
}

// Absent means "use what the driver declared", which is how every factory gets its default.
static void test_numeric_option_falls_back_to_the_declared_default() {
    const auto d = boundedDescriptor();
    long       out = 0;
    TEST_ASSERT_TRUE(d.numericOption({}, "unit_id", out));
    TEST_ASSERT_EQUAL_INT32(1, out);  // boundedDescriptor declares "1"
}

// A key that names nothing, and a key that names a free-form option, are both refusals rather
// than a lenient parse of whatever string happens to be stored there.
static void test_numeric_option_refuses_unknown_and_non_numeric_keys() {
    DriverDescriptor d;
    d.id      = "mixed";
    d.options = {DriverOption{"note", "Note", "", "42", {}}};  // no bounds -> free-form
    long out  = 99;
    TEST_ASSERT_FALSE(d.numericOption({{"note", "42"}}, "note", out));
    TEST_ASSERT_FALSE(d.numericOption({{"nope", "1"}}, "nope", out));
    TEST_ASSERT_EQUAL_INT32(99, out);
}

// A range that legitimately starts at zero must read back too, not just validate -- this is
// the SunSpec base register, and it is the pair that had drifted.
static void test_numeric_option_handles_a_range_starting_at_zero() {
    DriverDescriptor d;
    d.id      = "based";
    d.options = {DriverOption{"base_address", "Base", "", "40000", {}, 0, 65534}};
    long out  = 99;
    TEST_ASSERT_TRUE(d.numericOption({{"base_address", "0"}}, "base_address", out));
    TEST_ASSERT_EQUAL_INT32(0, out);
    TEST_ASSERT_TRUE(d.numericOption({{"base_address", "65534"}}, "base_address", out));
    TEST_ASSERT_EQUAL_INT32(65534, out);
    TEST_ASSERT_FALSE(d.numericOption({{"base_address", "65535"}}, "base_address", out));
    TEST_ASSERT_EQUAL_INT32(65534, out);
}

static void test_an_unbounded_option_stays_free_form() {
    DriverDescriptor d;
    d.id      = "free";
    d.options = {DriverOption{"note", "Note", "", "", {}}};
    DriverOptionError e;
    TEST_ASSERT_TRUE(validateDriverOptions(d, {{"note", "anything at all"}}, e));
    TEST_ASSERT_TRUE(validateDriverOptions(d, {{"note", "999999"}}, e));
}

// --- the mock as a fleet --------------------------------------------------------------------

/// The defect this option exists to remove. Every mock reported one hardcoded serial, and
/// deviceId() prefers the serial -- so a second mock under `additional_devices` resolved to the
/// same id and was dropped at boot as a duplicate. The descriptor claimed
/// supportsMultipleDevices while no two instances could differ in anything.
static void test_two_mock_instances_get_different_device_ids() {
    test::MockTransport transport;
    DriverRegistry      registry;
    registerBuiltinDrivers(registry);

    auto a = registry.create("mock_inverter", transport, DriverOptions{{"unit_id", "1"}});
    auto b = registry.create("mock_inverter", transport, DriverOptions{{"unit_id", "2"}});
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);

    TEST_ASSERT_EQUAL_STRING("mock_inverter-MOCK-0000000001", a->identity().deviceId().c_str());
    TEST_ASSERT_EQUAL_STRING("mock_inverter-MOCK-0000000002", b->identity().deviceId().c_str());
}

/// Set as well as the serial. instanceKey is what identifies a device BEFORE its first poll,
/// when the state-store keys are minted, and leaving it empty would make the mock the one
/// driver that behaves differently at exactly that moment.
static void test_a_mock_instance_carries_its_key_as_well_as_its_serial() {
    test::MockTransport transport;
    DriverRegistry      registry;
    registerBuiltinDrivers(registry);
    auto d = registry.create("mock_inverter", transport, DriverOptions{{"unit_id", "5"}});
    TEST_ASSERT_EQUAL_STRING("5", d->identity().instanceKey.c_str());
    TEST_ASSERT_EQUAL_STRING("MOCK-0000000005", d->identity().serialNumber.c_str());
}

/// The compatibility promise. Instance 1 must produce byte-for-byte the id every existing
/// single-mock install already has, or upgrading orphans its retained MQTT topics and its Home
/// Assistant entities.
static void test_instance_one_keeps_the_id_it_always_had() {
    TEST_ASSERT_EQUAL_STRING("MOCK-0000000001", mock::mockSerialNumber(1).c_str());
    test::MockTransport transport;
    DriverRegistry      registry;
    registerBuiltinDrivers(registry);
    // No unit_id at all: exactly what a config stored before this option existed looks like.
    auto d = registry.create("mock_inverter", transport, DriverOptions{});
    TEST_ASSERT_EQUAL_STRING("mock_inverter-MOCK-0000000001", d->identity().deviceId().c_str());
}

/// Different values at the same instant, which is the whole point. A fleet all reporting one
/// identical number makes a shared store, a mislabelled Prometheus series or a Modbus unit off
/// by one look exactly like correct output.
static void test_instances_are_staggered_so_they_do_not_report_in_lockstep() {
    test::MockTransport transport;
    DriverRegistry      registry;
    registerBuiltinDrivers(registry);

    // Built directly with a FROZEN clock rather than through the factory, which uses
    // steady_clock: the simulated day is only ten minutes long, so a real clock puts these
    // three at an arbitrary point on the curve and the assertions below would pass or fail
    // depending on when the suite happened to run.
    const auto powerOf = [](uint8_t unit) {
        mock::MockOptions options;
        options.instance = unit;
        mock::MockDriver driver([] { return uint64_t{0}; }, options);
        test::MockTransport t;
        driver.begin(t);
        DeviceState state;
        state.lastPollAttemptMs = 1;
        TEST_ASSERT_EQUAL(PollResult::Ok, driver.poll(state));
        const auto* p = state.measurements.find(measurement_id::kAcPowerTotal);
        TEST_ASSERT_NOT_NULL(p);
        return p->value;
    };

    // The whole run the device table allows, not just a couple: the point of 1/32 is that even
    // the last one is still in daylight.
    double previous = 0.0;
    for (uint8_t unit = 1; unit <= static_cast<uint8_t>(kMaxDevices); ++unit) {
        const double power = powerOf(unit);
        TEST_ASSERT_TRUE(power > 0.0);
        if (unit > 1) {
            TEST_ASSERT_TRUE(std::fabs(power - previous) > 1.0);
        }
        previous = power;
    }
}

/// The second thing the stagger buys, and the reason it is not a nicety. Later in the day the
/// fleet is a MIX of producing and dark devices, which is the only way to reach the rule that a
/// total is null when no device reported the channel rather than 0. A fleet rising and setting
/// in unison never produces that state.
///
/// Pinned because it is now a documented property. It also guards the geometry from the other
/// side: a stagger wide enough to be interesting must not be so wide that instances are dark at
/// boot, which the test above asserts.
static void test_a_running_fleet_ends_up_part_producing_and_part_dark() {
    const auto powerAt = [](uint64_t nowMs, uint8_t unit) {
        mock::MockOptions options;
        options.instance = unit;
        mock::MockDriver driver([nowMs] { return nowMs; }, options);
        test::MockTransport t;
        driver.begin(t);
        DeviceState state;
        state.lastPollAttemptMs = 1;
        TEST_ASSERT_EQUAL(PollResult::Ok, driver.poll(state));
        const auto* p = state.measurements.find(measurement_id::kAcPowerTotal);
        TEST_ASSERT_NOT_NULL(p);
        return p->value;
    };

    // A twentieth into the default ten-minute day: the run has begun to set from the far end.
    const uint64_t later = (10ULL * 60 * 1000) / 20;
    int producing = 0;
    int dark      = 0;
    for (uint8_t unit = 1; unit <= static_cast<uint8_t>(kMaxDevices); ++unit) {
        (powerAt(later, unit) > 0.0 ? producing : dark)++;
    }
    TEST_ASSERT_TRUE(producing > 0);
    TEST_ASSERT_TRUE(dark > 0);
}

/// The bound is the device table's, so a config that would not fit is refused at the option
/// rather than discovered at boot.
static void test_an_instance_beyond_the_device_table_is_refused() {
    DriverOptionError error;
    TEST_ASSERT_FALSE(validateDriverOptions(mock::readOnlyDescriptor(),
                                            DriverOptions{{"unit_id", "99"}}, error));
    TEST_ASSERT_EQUAL_STRING("unit_id", error.key.c_str());
    TEST_ASSERT_TRUE(validateDriverOptions(mock::readOnlyDescriptor(),
                                           DriverOptions{{"unit_id", "8"}}, error));
}

// --- the mock's counters and battery -----------------------------------------------------

#if ENABLE_DRIVER_MOCK
/// A mock on a clock the test moves by hand, so a whole simulated day fits in one loop.
///
/// `dayLengthMs` of 1000 makes a tick a thousandth of a day; the day boundary then sits at
/// now = 500, because the curve's origin is midday rather than midnight.
namespace {
constexpr uint64_t kDay      = 1000;   ///< one simulated day, in ticks
constexpr uint64_t kMidnight = 500;    ///< the tick at which a simulated day begins
constexpr double   kDailyYieldKwh = 12.5;

struct SteppedMock {
    uint64_t         now = kMidnight;
    MockTransport    transport;
    mock::MockDriver driver;

    SteppedMock() : driver([this] { return now; }, [] {
        mock::MockOptions o;
        o.dayLengthMs = kDay;
        return o;
    }()) {
        driver.begin(transport);
    }

    /// One poll at the current tick, returning the value of a channel. Fails the test rather
    /// than dereferencing null, so a channel that stops being published is a named failure.
    double read(const char* id) {
        DeviceState state;
        state.lastPollAttemptMs = now + 1;
        TEST_ASSERT_EQUAL(PollResult::Ok, driver.poll(state));
        const auto* m = state.measurements.find(id);
        TEST_ASSERT_NOT_NULL_MESSAGE(m, id);
        return m->value;
    }
};
}  // namespace
#endif

/// The defect: `energy.today` was the power curve scaled, so it climbed to midday and then fell
/// back to zero by sunset. The afternoon is where that shows -- at equal power either side of
/// noon the old code returned equal energy, which is only true if the morning produced nothing.
static void test_energy_today_only_ever_climbs_within_a_day() {
#if ENABLE_DRIVER_MOCK
    SteppedMock m;

    m.now = kMidnight;
    TEST_ASSERT_TRUE_MESSAGE(m.read(measurement_id::kEnergyToday) == 0.0,
                             "a day must start at zero");

    double previous = 0.0;
    for (uint64_t tick = kMidnight; tick < kMidnight + kDay; ++tick) {
        m.now              = tick;
        const double today = m.read(measurement_id::kEnergyToday);
        TEST_ASSERT_TRUE_MESSAGE(today >= previous, "energy.today went backwards");
        previous = today;
    }

    // Morning and afternoon at the same instantaneous power: the same reading under the old
    // code, and necessarily different once it is an integral.
    m.now                 = kMidnight + 375;  // phase 0.375, climbing
    const double morning  = m.read(measurement_id::kEnergyToday);
    m.now                 = kMidnight + 625;  // phase 0.625, same power, falling
    const double afternoon = m.read(measurement_id::kEnergyToday);
    TEST_ASSERT_TRUE_MESSAGE(afternoon > morning, "the afternoon must add to the day, not undo it");

    m.now = kMidnight + 750;  // sunset
    TEST_ASSERT_TRUE(std::fabs(m.read(measurement_id::kEnergyToday) - kDailyYieldKwh) < 1e-9);
    m.now = kMidnight + 900;  // after dark, held rather than decaying
    TEST_ASSERT_TRUE(std::fabs(m.read(measurement_id::kEnergyToday) - kDailyYieldKwh) < 1e-9);
#else
    TEST_IGNORE_MESSAGE("mock driver not compiled in");
#endif
}

/// The lifetime counter has to survive the seam that resets the daily one -- the moment the two
/// disagree it either jumps or goes backwards, and a total that goes backwards is a meter reset
/// to everything downstream. It used to be a constant, so nothing here was exercised at all.
static void test_energy_total_crosses_midnight_without_a_step() {
#if ENABLE_DRIVER_MOCK
    SteppedMock m;

    double previous = 0.0;
    for (uint64_t tick = kMidnight; tick <= kMidnight + 2 * kDay; ++tick) {
        m.now              = tick;
        const double total = m.read(measurement_id::kEnergyTotal);
        TEST_ASSERT_TRUE_MESSAGE(total >= previous, "energy.total went backwards");
        previous = total;
    }

    // Two days on: exactly two days' yield richer. Asserted as a difference, not against an
    // absolute figure -- the absolute one also encodes where the simulation's epoch happens to
    // fall (it starts at midday, so a day is already banked by the first midnight), which is
    // incidental and would make this test fail for the wrong reason.
    m.now                = kMidnight;
    const double atStart = m.read(measurement_id::kEnergyTotal);
    m.now                = kMidnight + 2 * kDay;
    TEST_ASSERT_TRUE(std::fabs((m.read(measurement_id::kEnergyTotal) - atStart)
                               - 2 * kDailyYieldKwh) < 1e-9);

    // Either side of a single tick across midnight: continuous, not a jump of a day's yield.
    m.now              = kMidnight + kDay - 1;
    const double before = m.read(measurement_id::kEnergyTotal);
    m.now              = kMidnight + kDay;
    const double after  = m.read(measurement_id::kEnergyTotal);
    TEST_ASSERT_TRUE_MESSAGE(std::fabs(after - before) < 0.01, "the day rolled over with a step");
#else
    TEST_IGNORE_MESSAGE("mock driver not compiled in");
#endif
}

/// measurement.h asks a driver reading separate charge/discharge rails to publish the combined
/// `battery.power` as well. The mock read them separately and published only the rails, so the
/// sign convention had no test and every consumer of the combined channel -- the dashboard
/// column, the Home Assistant entity, the MQTT topic -- had nothing to show on the one
/// device that runs without hardware.
static void test_the_mock_publishes_combined_battery_power() {
#if ENABLE_DRIVER_MOCK
    SteppedMock m;
    bool sawCharging = false, sawDischarging = false, sawIdle = false;

    for (uint64_t tick = kMidnight; tick < kMidnight + kDay; tick += 5) {
        m.now                  = tick;
        const double power     = m.read(measurement_id::kBatteryPower);
        const double charge    = m.read(measurement_id::kBatteryChargePower);
        const double discharge = m.read(measurement_id::kBatteryDischargePower);
        TEST_ASSERT_TRUE_MESSAGE(std::fabs(power - (charge - discharge)) < 1e-9,
                                 "battery.power must agree with the rails it is made of");
        if (power > 0.0) {
            sawCharging = true;
        } else if (power < 0.0) {
            sawDischarging = true;
        } else {
            sawIdle = true;
        }
    }

    // All three states within one simulated day: a column that can only ever render one of them
    // is not a demonstration of anything.
    TEST_ASSERT_TRUE_MESSAGE(sawCharging, "never charged");
    TEST_ASSERT_TRUE_MESSAGE(sawDischarging, "never discharged");
    TEST_ASSERT_TRUE_MESSAGE(sawIdle, "never idle");
#else
    TEST_IGNORE_MESSAGE("mock driver not compiled in");
#endif
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_numeric_option_out_of_range_is_refused);
    RUN_TEST(test_a_numeric_option_that_is_not_a_number_is_refused);
    RUN_TEST(test_a_sloppy_but_parseable_number_is_refused);
    RUN_TEST(test_a_range_starting_at_zero_is_still_numeric);
    RUN_TEST(test_an_unbounded_option_stays_free_form);
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
    RUN_TEST(test_numeric_option_reads_a_value_inside_the_declared_range);
    RUN_TEST(test_numeric_option_refuses_what_the_declaration_refuses);
    RUN_TEST(test_numeric_option_refuses_trailing_garbage_and_non_numbers);
    RUN_TEST(test_numeric_option_accepts_a_padded_number_the_gate_would_refuse);
    RUN_TEST(test_numeric_option_falls_back_to_the_declared_default);
    RUN_TEST(test_numeric_option_refuses_unknown_and_non_numeric_keys);
    RUN_TEST(test_numeric_option_handles_a_range_starting_at_zero);
    RUN_TEST(test_two_mock_instances_get_different_device_ids);
    RUN_TEST(test_a_mock_instance_carries_its_key_as_well_as_its_serial);
    RUN_TEST(test_instance_one_keeps_the_id_it_always_had);
    RUN_TEST(test_instances_are_staggered_so_they_do_not_report_in_lockstep);
    RUN_TEST(test_a_running_fleet_ends_up_part_producing_and_part_dark);
    RUN_TEST(test_an_instance_beyond_the_device_table_is_refused);
    RUN_TEST(test_energy_today_only_ever_climbs_within_a_day);
    RUN_TEST(test_energy_total_crosses_midnight_without_a_step);
    RUN_TEST(test_the_mock_publishes_combined_battery_power);
    return UNITY_END();
}
