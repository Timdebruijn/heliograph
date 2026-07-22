// SPDX-License-Identifier: MIT
// The read-only guarantee, as a tested contract rather than a promise.

#include <unity.h>

#include <cmath>

#include "commands/command_dispatcher.h"
#include "drivers/eversolar_legacy/eversolar_driver.h"
#include "drivers/mock/mock_driver.h"
#include "support/mock_transport.h"

using namespace heliograph;
using test::MockTransport;

static uint64_t g_now = 0;
static uint64_t clockFn() { return g_now; }

void setUp() { g_now = 100000; }
void tearDown() {}

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
    return UNITY_END();
}
