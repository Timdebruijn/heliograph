// SPDX-License-Identifier: MIT
// The read-only guarantee, as a tested contract rather than a promise.

#include <unity.h>

#include <cmath>

#include "commands/command_dispatcher.h"
#include "commands/command_queue.h"
#include "drivers/eversolar_legacy/eversolar_driver.h"
#include "drivers/mock/mock_driver.h"
#include "outputs/json_util.h"
#include "support/mock_transport.h"

using namespace heliograph;
using heliograph::json_util::CommandRequestError;
using heliograph::json_util::parseCommandRequest;
using test::MockTransport;

static uint64_t g_now = 0;
static uint64_t clockFn() { return g_now; }

void setUp() { g_now = 100000; }
void tearDown() {}

static JsonDocument parse(const std::string& json) {
    JsonDocument doc;
    const auto   err = deserializeJson(doc, json);
    TEST_ASSERT_EQUAL_MESSAGE(DeserializationError::Ok, err.code(), "payload is not valid JSON");
    return doc;
}

static InverterCommand cmd(InverterCommandType type, double value) {
    InverterCommand c;
    c.type         = type;
    c.numericValue = value;
    c.source       = CommandSource::Rest;
    c.requestId    = "test";
    return c;
}

// --- read-only mode ------------------------------------------------------------------------

static void test_read_only_mode_rejects_every_command_type() {
    // The MVP ships with this on. Nothing that can move an inverter gets past here.
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(true);

    for (size_t i = 0; i < kCommandTypeCount; ++i) {
        const auto out = d.dispatch(cmd(static_cast<InverterCommandType>(i), 50.0), driver);
        TEST_ASSERT_EQUAL(CommandResult::ReadOnlyMode, out.result);
    }
    TEST_ASSERT_EQUAL_UINT32(0, driver.acceptedCommands());
}

static void test_read_only_mode_beats_a_driver_that_claims_write_capability() {
    // Checked first and independently, so enabling it is sufficient on its own -- a driver
    // advertising capabilities it should not have cannot defeat it.
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver driver(clockFn, o);
    TEST_ASSERT_FALSE(driver.capabilities().isReadOnly());

    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(true);
    const auto out = d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 50.0), driver);
    TEST_ASSERT_EQUAL(CommandResult::ReadOnlyMode, out.result);
}

static void test_read_only_mode_is_the_default() {
    CommandDispatcher d(clockFn);
    TEST_ASSERT_TRUE(d.readOnlyMode());
}

// --- capability gating ---------------------------------------------------------------------

static void test_read_only_driver_rejects_on_capability_not_identity() {
    // The dispatcher never asks which driver this is; it asks what it can do.
    mock::MockOptions o;
    o.writable = false;
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(false);

    for (size_t i = 0; i < kCommandTypeCount; ++i) {
        const auto out = d.dispatch(cmd(static_cast<InverterCommandType>(i), 50.0), driver);
        TEST_ASSERT_EQUAL(CommandResult::Unsupported, out.result);
    }
}

static void test_eversolar_driver_rejects_everything_even_with_the_gate_open() {
    // Even with read-only mode off, the driver has no write capability, because the protocol
    // has no write operation to expose.
    MockTransport            t;
    eversolar::EversolarDriver driver(t);
    driver.begin(t);

    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(false);

    for (size_t i = 0; i < kCommandTypeCount; ++i) {
        const auto out = d.dispatch(cmd(static_cast<InverterCommandType>(i), 50.0), driver);
        TEST_ASSERT_EQUAL(CommandResult::Unsupported, out.result);
    }
    TEST_ASSERT_EQUAL_size_t(3, t.writes.size());  // only the re-register broadcast from begin()
}

static void test_a_capability_the_driver_lacks_is_rejected() {
    mock::MockOptions o;
    o.writable = true;  // grants SetActivePowerLimit and StartStop only
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(false);

    const auto out = d.dispatch(cmd(InverterCommandType::SetMinimumSoc, 20.0), driver);
    TEST_ASSERT_EQUAL(CommandResult::Unsupported, out.result);
}

static void test_a_granted_capability_reaches_the_driver() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(false);

    const auto out = d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 60.0), driver);
    TEST_ASSERT_EQUAL(CommandResult::Ok, out.result);
    TEST_ASSERT_EQUAL_UINT32(1, driver.acceptedCommands());
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 60.0, driver.lastAcceptedValue());
}

// --- range validation ----------------------------------------------------------------------

static void test_value_above_the_maximum_is_rejected() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(false);

    const auto out = d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 101.0), driver);
    TEST_ASSERT_EQUAL(CommandResult::OutOfRange, out.result);
    TEST_ASSERT_EQUAL_UINT32(0, driver.acceptedCommands());  // never reached the driver
}

static void test_value_below_the_minimum_is_rejected() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(false);

    TEST_ASSERT_EQUAL(CommandResult::OutOfRange,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, -1.0), driver)
                          .result);
}

static void test_the_bounds_themselves_are_accepted() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(false);

    TEST_ASSERT_EQUAL(CommandResult::Ok,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 0.0), driver)
                          .result);
    TEST_ASSERT_EQUAL(CommandResult::Ok,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 100.0), driver)
                          .result);
}

static void test_nan_is_rejected() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(false);

    const auto out =
        d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, std::nan("")), driver);
    TEST_ASSERT_EQUAL(CommandResult::OutOfRange, out.result);
}

static void test_a_value_off_the_step_grid_is_rejected() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(false);

    // Watts have a step of 10.
    TEST_ASSERT_EQUAL(CommandResult::OutOfRange,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitWatts, 1005.0), driver)
                          .result);
    TEST_ASSERT_EQUAL(CommandResult::Ok,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitWatts, 1000.0), driver)
                          .result);
}

static void test_a_missing_value_for_a_numeric_command_is_rejected() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(false);

    InverterCommand c;
    c.type = InverterCommandType::SetActivePowerLimitPercent;  // no numericValue
    TEST_ASSERT_EQUAL(CommandResult::OutOfRange, d.dispatch(c, driver).result);
}

static void test_a_non_numeric_command_needs_no_value() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(false);

    InverterCommand c;
    c.type = InverterCommandType::Stop;  // StartStop has no numeric bounds
    TEST_ASSERT_EQUAL(CommandResult::Ok, d.dispatch(c, driver).result);
}

// --- rate limiting -------------------------------------------------------------------------

static void test_burst_is_allowed_then_throttled() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver driver(clockFn, o);
    RateLimitPolicy  rl;
    rl.minIntervalMs = 1000;
    rl.burst         = 3;
    CommandDispatcher d(clockFn, rl);
    d.setReadOnlyMode(false);

    for (int i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL(CommandResult::Ok,
                          d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 50.0),
                                     driver)
                              .result);
    }
    TEST_ASSERT_EQUAL(CommandResult::RateLimited,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 50.0), driver)
                          .result);
}

static void test_the_allowance_refills_after_a_quiet_period() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver driver(clockFn, o);
    RateLimitPolicy  rl;
    rl.minIntervalMs = 1000;
    rl.burst         = 3;
    CommandDispatcher d(clockFn, rl);
    d.setReadOnlyMode(false);

    for (int i = 0; i < 4; ++i) {
        d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 50.0), driver);
    }
    g_now += 2000;
    TEST_ASSERT_EQUAL(CommandResult::Ok,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 50.0), driver)
                          .result);
}

static void test_a_rejected_command_does_not_consume_the_allowance() {
    // Otherwise a client sending nonsense could rate-limit a legitimate command out.
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver driver(clockFn, o);
    RateLimitPolicy  rl;
    rl.minIntervalMs = 1000;
    rl.burst         = 3;
    CommandDispatcher d(clockFn, rl);
    d.setReadOnlyMode(false);

    for (int i = 0; i < 10; ++i) {
        d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 999.0), driver);  // invalid
    }
    for (int i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL(CommandResult::Ok,
                          d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 50.0),
                                     driver)
                              .result);
    }
}

// --- reporting -----------------------------------------------------------------------------

static void test_every_rejection_explains_itself() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn);

    // The reason is shown to a user in the web UI or returned over REST, so it must never be
    // empty and must never contain internals.
    auto out = d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 50.0), driver);
    TEST_ASSERT_TRUE(out.reason.find("read-only") != std::string::npos);

    d.setReadOnlyMode(false);
    out = d.dispatch(cmd(InverterCommandType::SetMinimumSoc, 20.0), driver);
    TEST_ASSERT_TRUE(out.reason.find("set_minimum_soc") != std::string::npos);

    out = d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 500.0), driver);
    TEST_ASSERT_TRUE(out.reason.find("range") != std::string::npos);
}

// --- gate 3 must not be skippable ----------------------------------------------------------

// A driver that stands in for the realistic mistake: it claims a capability it can write but
// never fills in the matching NumericCapability. That combination used to skip range checking
// entirely, because the check lived inside `if (nc.supported && nc.writable)`.
namespace {
class BoundlessDriver : public InverterDriver {
public:
    const DriverDescriptor& descriptor() const override {
        static const DriverDescriptor d = [] {
            DriverDescriptor x;
            x.id = "boundless";
            return x;
        }();
        return d;
    }
    bool       begin(Transport&) override { return true; }
    ProbeResult probe() override { return {}; }
    PollResult  poll(DeviceState&) override { return PollResult::Ok; }
    DeviceIdentity identity() const override { return {}; }
    BusErrorCounts busErrors() const override { return {}; }
    InverterCapabilities capabilities() const override {
        InverterCapabilities c;
        c.addWrite(InverterCapability::SetActivePowerLimit);  // ...and no numeric[] entry
        return c;
    }
    CommandResult execute(const InverterCommand& command) override {
        last = command;
        ++executed;
        return CommandResult::Ok;
    }
    InverterCommand last{};
    uint32_t        executed = 0;
};
}  // namespace

static void test_a_declared_write_without_published_bounds_is_refused_not_waved_through() {
    BoundlessDriver   driver;
    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(false);

    const auto out = d.dispatch(cmd(InverterCommandType::SetActivePowerLimitWatts, 1e9), driver);

    TEST_ASSERT_EQUAL(CommandResult::Unsupported, out.result);
    TEST_ASSERT_EQUAL_UINT32(0, driver.executed);  // 1e9 W never reached the driver
}

static void test_a_value_less_command_on_a_boundless_driver_is_also_refused() {
    BoundlessDriver   driver;
    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(false);

    InverterCommand c;
    c.type = InverterCommandType::SetActivePowerLimitPercent;  // no numericValue at all
    const auto out = d.dispatch(c, driver);

    TEST_ASSERT_EQUAL(CommandResult::Unsupported, out.result);
    TEST_ASSERT_EQUAL_UINT32(0, driver.executed);
}

// Reaching gate 3's enum branch needs a driver that actually declares the mode write -- the
// mock does not, so it stops at gate 2. An earlier version of this test used the mock and
// accepted either result, which meant it passed with the enum branch deleted entirely.
namespace {
/// Declares the mode write AND the modes it accepts, which is what a real profile-driven device
/// publishes. Values 0/2/3 with a gap, deliberately: a driver whose modes are numbered
/// consecutively cannot catch an index-for-value mix-up.
constexpr EnumOption kModes[] = {{0, "Self-consumption"}, {2, "Forced"}, {3, "External EMS"}};

class ModeDriver : public BoundlessDriver {
public:
    InverterCapabilities capabilities() const override {
        InverterCapabilities c;
        c.addWrite(InverterCapability::SetBatteryOperatingMode);
        EnumCapability& e =
            c.enums[static_cast<size_t>(InverterCommandType::SetBatteryOperatingMode)];
        e.supported   = true;
        e.writable    = true;
        e.options     = kModes;
        e.optionCount = 3;
        return c;
    }
};

/// Declares the mode write and no modes -- the enum twin of BoundlessDriver.
class BoundlessModeDriver : public BoundlessDriver {
public:
    InverterCapabilities capabilities() const override {
        InverterCapabilities c;
        c.addWrite(InverterCapability::SetBatteryOperatingMode);  // ...and no enums[] entry
        return c;
    }
};
}  // namespace

// The enum half of the lesson the numeric branch already learned: a driver that sets the write bit
// without publishing what it accepts is refused, not trusted. Before the EnumCapability existed
// this case passed the value through unvalidated, so any mode number reached the register --
// including one the device does not implement, which is a setting nobody has ever tested rather
// than a value slightly too large.
static void test_a_declared_mode_write_without_published_modes_is_refused() {
    BoundlessModeDriver driver;
    CommandDispatcher   d(clockFn);
    d.setReadOnlyMode(false);

    InverterCommand c;
    c.type      = InverterCommandType::SetBatteryOperatingMode;
    c.enumValue = 2;
    const auto out = d.dispatch(c, driver);

    TEST_ASSERT_EQUAL(CommandResult::Unsupported, out.result);
    TEST_ASSERT_EQUAL_UINT32(0, driver.executed);
}

// A mode the device never declared is refused by the dispatcher, before the driver is asked.
static void test_an_undeclared_mode_is_refused_by_the_gate() {
    ModeDriver        driver;
    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(false);

    InverterCommand c;
    c.type      = InverterCommandType::SetBatteryOperatingMode;
    c.enumValue = 1;  // sits in the gap between two declared modes
    const auto out = d.dispatch(c, driver);

    TEST_ASSERT_EQUAL(CommandResult::OutOfRange, out.result);
    TEST_ASSERT_EQUAL_UINT32(0, driver.executed);
}

static void test_a_mode_command_without_a_selection_is_refused() {
    ModeDriver        driver;
    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(false);

    InverterCommand c;
    c.type = InverterCommandType::SetBatteryOperatingMode;  // no enumValue
    const auto out = d.dispatch(c, driver);

    TEST_ASSERT_EQUAL(CommandResult::OutOfRange, out.result);
    TEST_ASSERT_EQUAL_UINT32(0, driver.executed);
}

static void test_a_mode_command_with_a_selection_reaches_the_driver() {
    ModeDriver        driver;
    CommandDispatcher d(clockFn);
    d.setReadOnlyMode(false);

    InverterCommand c;
    c.type      = InverterCommandType::SetBatteryOperatingMode;
    c.enumValue = 2;
    TEST_ASSERT_EQUAL(CommandResult::Ok, d.dispatch(c, driver).result);
    TEST_ASSERT_EQUAL_UINT32(1, driver.executed);
    // The selection reaches the driver unchanged. It is the DEVICE's mode number, not an index
    // into the option list, and the dispatcher must not renumber it on the way through.
    TEST_ASSERT_EQUAL_INT32(2, *driver.last.enumValue);
}

// --- lifting a restriction is never throttled -----------------------------------------------

// The rate limiter must never be the reason a curtailed inverter stays curtailed. Which way is
// "safe" is a deliberate choice: this project's failsafe is "bridge dead, inverter keeps
// producing" (docs/drm.md), so the protected direction is the one that gives production back --
// the opposite of what a grid-protection stance would pick.
static void test_start_is_never_throttled() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn, RateLimitPolicy{1000, 1});
    d.setReadOnlyMode(false);

    // Burn the whole allowance on a restricting command.
    TEST_ASSERT_EQUAL(CommandResult::Ok,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 20.0),
                                 driver).result);
    TEST_ASSERT_EQUAL(CommandResult::RateLimited,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 30.0),
                                 driver).result);

    InverterCommand start;
    start.type = InverterCommandType::Start;
    TEST_ASSERT_EQUAL(CommandResult::Ok, d.dispatch(start, driver).result);
}

// A limit at its maximum is NOT exempt, deliberately. An earlier version treated "value >=
// declared maximum" as "explicitly no limit" -- which a driver declaring minimum == maximum, or
// leaving both at their 0 defaults, turned inside out: full curtailment then satisfied the test
// and skipped the limiter entirely. Deriving intent from bounds the driver may not have thought
// about is the same trust gate 3 just removed.
static void test_a_limit_at_its_maximum_still_pays_for_a_token() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn, RateLimitPolicy{1000, 1});
    d.setReadOnlyMode(false);

    TEST_ASSERT_EQUAL(CommandResult::Ok,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 20.0),
                                 driver).result);
    TEST_ASSERT_EQUAL(CommandResult::RateLimited,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 100.0),
                                 driver).result);
}

// Run/stop commands ride their own track, so a burst of restricting traffic can never swallow
// them -- but they are still spaced, so a loop cannot saturate the bus with them either.
static void test_run_state_commands_are_spaced_but_never_starved() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn, RateLimitPolicy{1000, 2});
    d.setReadOnlyMode(false);

    InverterCommand start;
    start.type = InverterCommandType::Start;
    TEST_ASSERT_EQUAL(CommandResult::Ok, d.dispatch(start, driver).result);
    // Immediately again: spaced, not starved.
    TEST_ASSERT_EQUAL(CommandResult::RateLimited, d.dispatch(start, driver).result);
    g_now += 1000;
    TEST_ASSERT_EQUAL(CommandResult::Ok, d.dispatch(start, driver).result);

    // ...and none of that touched the restricting allowance: both burst slots are intact.
    TEST_ASSERT_EQUAL(CommandResult::Ok,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 10.0),
                                 driver).result);
    TEST_ASSERT_EQUAL(CommandResult::Ok,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 11.0),
                                 driver).result);
    TEST_ASSERT_EQUAL(CommandResult::RateLimited,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 12.0),
                                 driver).result);
}

// Both directions ride that track. Which of stopping or starting is the safer failure depends on
// whose hazard you reason about, and there is no need to choose: neither carries a value that
// could be wrong, and RateLimited here is a DROP -- nothing queues or retries -- so losing
// either is worse than losing "run at 60%", which an automation resends on its next tick.
static void test_stop_is_not_starved_by_restricting_traffic() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn, RateLimitPolicy{1000, 1});
    d.setReadOnlyMode(false);

    TEST_ASSERT_EQUAL(CommandResult::Ok,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 20.0),
                                 driver).result);
    TEST_ASSERT_EQUAL(CommandResult::RateLimited,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 30.0),
                                 driver).result);

    InverterCommand stop;
    stop.type = InverterCommandType::Stop;
    TEST_ASSERT_EQUAL(CommandResult::Ok, d.dispatch(stop, driver).result);
}

// A clock that jumps backwards must not refill the allowance. On unsigned types the naive
// subtraction wraps to an enormous value, which reads as "ages ago" -- the throttle would fail
// open, which is the wrong direction for a gate.
static void test_a_backwards_clock_does_not_refill_the_allowance() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver  driver(clockFn, o);
    CommandDispatcher d(clockFn, RateLimitPolicy{1000, 1});
    d.setReadOnlyMode(false);

    g_now = 100000;
    TEST_ASSERT_EQUAL(CommandResult::Ok,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 20.0),
                                 driver).result);
    g_now = 50;  // clock went backwards
    TEST_ASSERT_EQUAL(CommandResult::RateLimited,
                      d.dispatch(cmd(InverterCommandType::SetActivePowerLimitPercent, 30.0),
                                 driver).result);
}

// --- CommandQueue ---------------------------------------------------------------------------
//
// The request/consume slot a future write path drains from rs485Task, generalising
// g_manualPollRequested's shape (one outstanding request, request-only) to carry an actual
// command instead of a bare flag.

static CommandQueue::Request queuedRequest(const std::string& deviceId,
                                            const std::string& requestId) {
    CommandQueue::Request r;
    r.deviceId          = deviceId;
    r.command           = cmd(InverterCommandType::SetActivePowerLimitPercent, 50.0);
    r.command.requestId = requestId;
    return r;
}

static void test_a_submitted_request_is_taken_exactly_once() {
    CommandQueue q;
    TEST_ASSERT_TRUE(q.submit(queuedRequest("inv-1", "r1")));

    auto taken = q.take();
    TEST_ASSERT_TRUE(taken.has_value());
    TEST_ASSERT_EQUAL_STRING("inv-1", taken->deviceId.c_str());
    TEST_ASSERT_EQUAL_STRING("r1", taken->command.requestId.c_str());

    // The slot is empty again: a second take() finds nothing left to consume.
    TEST_ASSERT_FALSE(q.take().has_value());
}

static void test_take_on_an_empty_queue_returns_nothing() {
    CommandQueue q;
    TEST_ASSERT_FALSE(q.take().has_value());
}

// Mirrors the one-in-flight rule g_manualPollRequested already applies: a second submission
// while one is pending must not silently replace or reorder the first.
static void test_a_second_submission_is_refused_while_one_is_pending() {
    CommandQueue q;
    TEST_ASSERT_TRUE(q.submit(queuedRequest("inv-1", "first")));
    TEST_ASSERT_FALSE(q.submit(queuedRequest("inv-1", "second")));

    // The ORIGINAL request is still the one waiting -- the refused second submission left no
    // trace.
    auto taken = q.take();
    TEST_ASSERT_TRUE(taken.has_value());
    TEST_ASSERT_EQUAL_STRING("first", taken->command.requestId.c_str());
}

static void test_a_taken_slot_accepts_a_new_submission() {
    CommandQueue q;
    TEST_ASSERT_TRUE(q.submit(queuedRequest("inv-1", "first")));
    q.take();
    TEST_ASSERT_TRUE(q.submit(queuedRequest("inv-1", "second")));
}

static void test_outcome_is_unknown_before_it_is_recorded() {
    CommandQueue q;
    TEST_ASSERT_FALSE(q.outcomeFor("inv1", "r1").has_value());
}

static void test_a_recorded_outcome_is_readable_by_its_request_id() {
    CommandQueue q;
    q.recordOutcome("inv1", "r1", DispatchOutcome{CommandResult::Ok, "accepted"});

    auto outcome = q.outcomeFor("inv1", "r1");
    TEST_ASSERT_TRUE(outcome.has_value());
    TEST_ASSERT_EQUAL(CommandResult::Ok, outcome->result);
    TEST_ASSERT_EQUAL_STRING("accepted", outcome->reason.c_str());
}

// Only one outcome is remembered at a time, matching submit()'s one-slot rule: a caller asking
// about a request that has since been superseded gets "unknown", not a stale answer.
static void test_a_later_outcome_supersedes_an_earlier_one() {
    CommandQueue q;
    q.recordOutcome("inv1", "first", DispatchOutcome{CommandResult::Ok, "accepted"});
    q.recordOutcome("inv1", "second",
                    DispatchOutcome{CommandResult::RateLimited, "too many commands"});

    TEST_ASSERT_FALSE(q.outcomeFor("inv1", "first").has_value());
    TEST_ASSERT_TRUE(q.outcomeFor("inv1", "second").has_value());
}

// A request id is the CALLER's to choose (REST and MQTT both accept one), so two devices can
// legitimately carry the same id at the same time. Keyed on the id alone, asking about device A
// returned device B's result -- the right answer to somebody else's question, which is worse than
// no answer because it looks like one.
//
// Dispatch was never affected: that is scoped by deviceId in Request, so the correct device always
// received the correct command. Only the report was wrong.
static void test_an_outcome_is_not_readable_through_another_device() {
    CommandQueue q;
    q.recordOutcome("inverter_b", "shared-id",
                    DispatchOutcome{CommandResult::Ok, "accepted"});

    TEST_ASSERT_FALSE(q.outcomeFor("inverter_a", "shared-id").has_value());
    TEST_ASSERT_TRUE(q.outcomeFor("inverter_b", "shared-id").has_value());
}

// --- commandTypeFromName / parseCommandRequest ----------------------------------------------
//
// The shared REST/MQTT wire parser: one rule for "which commands need a value vs an enum vs
// neither", read by both entry points instead of duplicated.

static void test_command_type_from_name_round_trips_every_type() {
    for (size_t i = 0; i < kCommandTypeCount; ++i) {
        const auto          type = static_cast<InverterCommandType>(i);
        InverterCommandType parsed{};
        TEST_ASSERT_TRUE(commandTypeFromName(commandTypeName(type), parsed));
        TEST_ASSERT_EQUAL(static_cast<int>(type), static_cast<int>(parsed));
    }
}

static void test_command_type_from_name_rejects_an_unknown_name() {
    InverterCommandType parsed{};
    TEST_ASSERT_FALSE(commandTypeFromName("not_a_real_command", parsed));
}

static void test_parse_command_request_rejects_invalid_json() {
    InverterCommand     out;
    CommandRequestError error;
    TEST_ASSERT_FALSE(parseCommandRequest("{not json", out, error));
    TEST_ASSERT_EQUAL_STRING("body", error.field.c_str());
}

static void test_parse_command_request_rejects_a_missing_type() {
    InverterCommand     out;
    CommandRequestError error;
    TEST_ASSERT_FALSE(parseCommandRequest("{}", out, error));
    TEST_ASSERT_EQUAL_STRING("type", error.field.c_str());
}

static void test_parse_command_request_rejects_an_unknown_type() {
    InverterCommand     out;
    CommandRequestError error;
    TEST_ASSERT_FALSE(
        parseCommandRequest(R"({"type":"not_a_real_command"})", out, error));
    TEST_ASSERT_EQUAL_STRING("type", error.field.c_str());
}

static void test_parse_command_request_reads_a_numeric_command() {
    InverterCommand     out;
    CommandRequestError error;
    TEST_ASSERT_TRUE(parseCommandRequest(
        R"({"type":"set_active_power_limit_percent","value":42.5})", out, error));
    TEST_ASSERT_EQUAL(static_cast<int>(InverterCommandType::SetActivePowerLimitPercent),
                      static_cast<int>(out.type));
    TEST_ASSERT_TRUE(out.numericValue.has_value());
    TEST_ASSERT_EQUAL_DOUBLE(42.5, *out.numericValue);
}

static void test_parse_command_request_rejects_a_numeric_command_without_a_value() {
    InverterCommand     out;
    CommandRequestError error;
    TEST_ASSERT_FALSE(
        parseCommandRequest(R"({"type":"set_active_power_limit_percent"})", out, error));
    TEST_ASSERT_EQUAL_STRING("value", error.field.c_str());
}

static void test_parse_command_request_reads_an_enum_command() {
    InverterCommand     out;
    CommandRequestError error;
    TEST_ASSERT_TRUE(parseCommandRequest(
        R"({"type":"set_battery_operating_mode","enum_value":2})", out, error));
    TEST_ASSERT_TRUE(out.enumValue.has_value());
    TEST_ASSERT_EQUAL(2, *out.enumValue);
}

static void test_parse_command_request_rejects_an_enum_command_without_a_selection() {
    InverterCommand     out;
    CommandRequestError error;
    TEST_ASSERT_FALSE(
        parseCommandRequest(R"({"type":"set_battery_operating_mode"})", out, error));
    TEST_ASSERT_EQUAL_STRING("enum_value", error.field.c_str());
}

static void test_parse_command_request_needs_neither_field_for_start() {
    InverterCommand     out;
    CommandRequestError error;
    TEST_ASSERT_TRUE(parseCommandRequest(R"({"type":"start"})", out, error));
    TEST_ASSERT_EQUAL(static_cast<int>(InverterCommandType::Start),
                      static_cast<int>(out.type));
}

static void test_parse_command_request_carries_a_supplied_request_id() {
    InverterCommand     out;
    CommandRequestError error;
    TEST_ASSERT_TRUE(
        parseCommandRequest(R"({"type":"start","request_id":"caller-123"})", out, error));
    TEST_ASSERT_EQUAL_STRING("caller-123", out.requestId.c_str());
}

static void test_parse_command_request_leaves_request_id_empty_when_omitted() {
    InverterCommand     out;
    CommandRequestError error;
    TEST_ASSERT_TRUE(parseCommandRequest(R"({"type":"start"})", out, error));
    TEST_ASSERT_TRUE(out.requestId.empty());
}

// kAutoRequestIdPrefix is reserved for submitCommand()'s own auto-generated ids (main.cpp) --
// a caller that supplies one anyway must be refused, not silently accepted, or the two id
// spaces could collide and CommandQueue::outcomeFor() would hand back the wrong request's
// outcome (review, 2026-07-30).
static void test_parse_command_request_rejects_a_caller_id_using_the_reserved_prefix() {
    InverterCommand     out;
    CommandRequestError error;
    TEST_ASSERT_FALSE(
        parseCommandRequest(R"({"type":"start","request_id":"auto-3"})", out, error));
    TEST_ASSERT_EQUAL_STRING("request_id", error.field.c_str());
}

static void test_parse_command_request_rejects_an_oversized_request_id() {
    InverterCommand     out;
    CommandRequestError error;
    const std::string   tooLong(kMaxRequestIdLength + 1, 'x');
    const std::string   json = R"({"type":"start","request_id":")" + tooLong + "\"}";
    TEST_ASSERT_FALSE(parseCommandRequest(json, out, error));
    TEST_ASSERT_EQUAL_STRING("request_id", error.field.c_str());
}

static void test_parse_command_request_accepts_a_request_id_at_the_length_limit() {
    InverterCommand     out;
    CommandRequestError error;
    const std::string   atLimit(kMaxRequestIdLength, 'x');
    const std::string   json = R"({"type":"start","request_id":")" + atLimit + "\"}";
    TEST_ASSERT_TRUE(parseCommandRequest(json, out, error));
    TEST_ASSERT_EQUAL_STRING(atLimit.c_str(), out.requestId.c_str());
}

static void test_parse_command_request_rejects_a_non_finite_value() {
    InverterCommand     out;
    CommandRequestError error;
    // 1e400 overflows a double to +Infinity when ArduinoJson parses it.
    TEST_ASSERT_FALSE(parseCommandRequest(
        R"({"type":"set_active_power_limit_percent","value":1e400})", out, error));
    TEST_ASSERT_EQUAL_STRING("value", error.field.c_str());
}

// --- shared command payload builders (json_util.h) ------------------------------------------
//
// The REST 202/GET-outcome bodies and MQTT's command_topic acks all go through these; one
// definition instead of each transport formatting its own JSON by hand.

static void test_build_command_accepted_payload() {
    std::string json;
    TEST_ASSERT_TRUE(json_util::buildCommandAcceptedPayload("req-7", json));
    auto doc = parse(json);
    TEST_ASSERT_EQUAL_STRING("accepted", doc["status"]);
    TEST_ASSERT_EQUAL_STRING("req-7", doc["request_id"]);
}

static void test_build_command_rejected_payload() {
    std::string json;
    TEST_ASSERT_TRUE(
        json_util::buildCommandRejectedPayload("a command is already pending", json));
    auto doc = parse(json);
    TEST_ASSERT_EQUAL_STRING("rejected", doc["status"]);
    TEST_ASSERT_EQUAL_STRING("a command is already pending", doc["reason"]);
}

static void test_build_command_outcome_payload_without_a_request_id() {
    std::string json;
    TEST_ASSERT_TRUE(json_util::buildCommandOutcomePayload(
        DispatchOutcome{CommandResult::OutOfRange, "value too high"}, json));
    auto doc = parse(json);
    TEST_ASSERT_EQUAL_STRING("out_of_range", doc["result"]);
    TEST_ASSERT_EQUAL_STRING("value too high", doc["reason"]);
    TEST_ASSERT_FALSE(doc["request_id"].is<const char*>());
}

static void test_build_command_outcome_payload_with_a_request_id() {
    std::string json;
    TEST_ASSERT_TRUE(json_util::buildCommandOutcomePayload(
        "req-9", DispatchOutcome{CommandResult::Ok, "accepted"}, json));
    auto doc = parse(json);
    TEST_ASSERT_EQUAL_STRING("req-9", doc["request_id"]);
    TEST_ASSERT_EQUAL_STRING("ok", doc["result"]);
    TEST_ASSERT_EQUAL_STRING("accepted", doc["reason"]);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_read_only_mode_rejects_every_command_type);
    RUN_TEST(test_read_only_mode_beats_a_driver_that_claims_write_capability);
    RUN_TEST(test_read_only_mode_is_the_default);
    RUN_TEST(test_read_only_driver_rejects_on_capability_not_identity);
    RUN_TEST(test_eversolar_driver_rejects_everything_even_with_the_gate_open);
    RUN_TEST(test_a_capability_the_driver_lacks_is_rejected);
    RUN_TEST(test_a_granted_capability_reaches_the_driver);
    RUN_TEST(test_value_above_the_maximum_is_rejected);
    RUN_TEST(test_value_below_the_minimum_is_rejected);
    RUN_TEST(test_the_bounds_themselves_are_accepted);
    RUN_TEST(test_nan_is_rejected);
    RUN_TEST(test_a_value_off_the_step_grid_is_rejected);
    RUN_TEST(test_a_missing_value_for_a_numeric_command_is_rejected);
    RUN_TEST(test_a_non_numeric_command_needs_no_value);
    RUN_TEST(test_burst_is_allowed_then_throttled);
    RUN_TEST(test_the_allowance_refills_after_a_quiet_period);
    RUN_TEST(test_a_rejected_command_does_not_consume_the_allowance);
    RUN_TEST(test_every_rejection_explains_itself);
    RUN_TEST(test_a_declared_write_without_published_bounds_is_refused_not_waved_through);
    RUN_TEST(test_a_value_less_command_on_a_boundless_driver_is_also_refused);
    RUN_TEST(test_a_declared_mode_write_without_published_modes_is_refused);
    RUN_TEST(test_an_undeclared_mode_is_refused_by_the_gate);
    RUN_TEST(test_a_mode_command_without_a_selection_is_refused);
    RUN_TEST(test_a_mode_command_with_a_selection_reaches_the_driver);

    RUN_TEST(test_start_is_never_throttled);
    RUN_TEST(test_a_limit_at_its_maximum_still_pays_for_a_token);
    RUN_TEST(test_run_state_commands_are_spaced_but_never_starved);
    RUN_TEST(test_stop_is_not_starved_by_restricting_traffic);
    RUN_TEST(test_a_backwards_clock_does_not_refill_the_allowance);

    RUN_TEST(test_a_submitted_request_is_taken_exactly_once);
    RUN_TEST(test_take_on_an_empty_queue_returns_nothing);
    RUN_TEST(test_a_second_submission_is_refused_while_one_is_pending);
    RUN_TEST(test_a_taken_slot_accepts_a_new_submission);
    RUN_TEST(test_outcome_is_unknown_before_it_is_recorded);
    RUN_TEST(test_a_recorded_outcome_is_readable_by_its_request_id);
    RUN_TEST(test_a_later_outcome_supersedes_an_earlier_one);
    RUN_TEST(test_an_outcome_is_not_readable_through_another_device);

    RUN_TEST(test_command_type_from_name_round_trips_every_type);
    RUN_TEST(test_command_type_from_name_rejects_an_unknown_name);
    RUN_TEST(test_parse_command_request_rejects_invalid_json);
    RUN_TEST(test_parse_command_request_rejects_a_missing_type);
    RUN_TEST(test_parse_command_request_rejects_an_unknown_type);
    RUN_TEST(test_parse_command_request_reads_a_numeric_command);
    RUN_TEST(test_parse_command_request_rejects_a_numeric_command_without_a_value);
    RUN_TEST(test_parse_command_request_reads_an_enum_command);
    RUN_TEST(test_parse_command_request_rejects_an_enum_command_without_a_selection);
    RUN_TEST(test_parse_command_request_needs_neither_field_for_start);
    RUN_TEST(test_parse_command_request_carries_a_supplied_request_id);
    RUN_TEST(test_parse_command_request_leaves_request_id_empty_when_omitted);
    RUN_TEST(test_parse_command_request_rejects_a_caller_id_using_the_reserved_prefix);
    RUN_TEST(test_parse_command_request_rejects_an_oversized_request_id);
    RUN_TEST(test_parse_command_request_accepts_a_request_id_at_the_length_limit);
    RUN_TEST(test_parse_command_request_rejects_a_non_finite_value);

    RUN_TEST(test_build_command_accepted_payload);
    RUN_TEST(test_build_command_rejected_payload);
    RUN_TEST(test_build_command_outcome_payload_without_a_request_id);
    RUN_TEST(test_build_command_outcome_payload_with_a_request_id);
    return UNITY_END();
}
