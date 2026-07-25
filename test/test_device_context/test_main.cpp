// SPDX-License-Identifier: MIT
//
// DeviceContext: the poll rhythm, and how the driver's frame-level tallies reach Diagnostics.
//
// The tallies are the reason this suite exists. They used to be a switch over the PollResult,
// which made the checksum metric a function of the poll VERDICT -- and a driver reading several
// blocks returns Ok as soon as one of them decodes. The counter therefore rose only on a bus
// that had already stopped working, and stayed flat on one that was merely losing a third of
// its frames. These tests pin the difference-based path so it cannot quietly regress to that.

#include <unity.h>

#include "device/device_context.h"
#include "diagnostics/diagnostics.h"
#include "drivers/inverter_driver.h"
#include "state/state_store.h"

using namespace heliograph;

void setUp() {}
void tearDown() {}

namespace {

/// A driver whose wire behaviour is dictated per poll: what it reports, and what it counted
/// while getting there. Those two are deliberately independent -- that is the whole point.
class ScriptedDriver : public InverterDriver {
public:
    PollResult     verdict = PollResult::Ok;
    BusErrorCounts counts{};

    const DriverDescriptor& descriptor() const override {
        static const DriverDescriptor d = [] {
            DriverDescriptor x;
            x.id = "scripted";
            return x;
        }();
        return d;
    }
    bool                 begin(Transport&) override { return true; }
    ProbeResult          probe() override { return {}; }
    PollResult           poll(DeviceState&) override { return verdict; }
    DeviceIdentity       identity() const override { return {}; }
    InverterCapabilities capabilities() const override { return {}; }
    CommandResult        execute(const InverterCommand&) override {
        return CommandResult::Unsupported;
    }
    BusErrorCounts busErrors() const override { return counts; }
};

uint64_t g_now = 0;
uint64_t clockFn() { return g_now; }

}  // namespace

// The case the old code could not express at all: the poll succeeds, and the bus was still
// corrupting frames while it did.
static void test_a_successful_poll_still_reports_the_frames_it_lost() {
    ScriptedDriver driver;
    StateStore     store;
    Diagnostics    diagnostics;
    DeviceContext  context(driver, store, diagnostics, clockFn);

    driver.verdict = PollResult::Ok;
    driver.counts.checksumErrors = 3;
    TEST_ASSERT_EQUAL(PollResult::Ok, context.pollOnce());

    const auto snapshot = diagnostics.snapshot();
    TEST_ASSERT_EQUAL_UINT32(3, snapshot.checksumErrorTotal);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.pollSuccessTotal);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.pollFailureTotal);
}

// The driver's counters are cumulative; the metric must move by the difference, not by the
// total, or every poll would re-report the entire history.
static void test_only_the_new_errors_are_added() {
    ScriptedDriver driver;
    StateStore     store;
    Diagnostics    diagnostics;
    DeviceContext  context(driver, store, diagnostics, clockFn);

    driver.counts.checksumErrors = 2;
    context.pollOnce();
    driver.counts.checksumErrors = 5;
    context.pollOnce();
    TEST_ASSERT_EQUAL_UINT32(5, diagnostics.snapshot().checksumErrorTotal);

    // A quiet poll adds nothing.
    context.pollOnce();
    TEST_ASSERT_EQUAL_UINT32(5, diagnostics.snapshot().checksumErrorTotal);
}

// Discovery probes the bus through the same driver before this context exists. Those errors are
// reported by the wizard; replaying them into the installation's metrics would put a step at
// every driver switch that no cable fault explains.
static void test_errors_from_before_the_context_are_not_replayed() {
    ScriptedDriver driver;
    driver.counts.checksumErrors = 9;
    driver.counts.timeouts       = 4;

    StateStore    store;
    Diagnostics   diagnostics;
    DeviceContext context(driver, store, diagnostics, clockFn);

    context.pollOnce();
    const auto snapshot = diagnostics.snapshot();
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.checksumErrorTotal);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.rs485TimeoutTotal);
}

// The three counters stay distinct: they point at three different faults, and docs/prometheus.md
// tells the reader to act on which one moved.
static void test_the_three_counters_do_not_bleed_into_each_other() {
    ScriptedDriver driver;
    StateStore     store;
    Diagnostics    diagnostics;
    DeviceContext  context(driver, store, diagnostics, clockFn);

    driver.verdict               = PollResult::Timeout;
    driver.counts.timeouts       = 2;
    driver.counts.invalidFrames  = 1;
    context.pollOnce();

    const auto snapshot = diagnostics.snapshot();
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.checksumErrorTotal);
    TEST_ASSERT_EQUAL_UINT32(2, snapshot.rs485TimeoutTotal);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.invalidFrameTotal);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.pollFailureTotal);
}

// A failure the wire had nothing to do with -- no driver, or a UART that would not open. Moving
// a bus counter here would send someone to check the cabling on a fault that never reached it.
static void test_a_transport_failure_moves_no_bus_counter() {
    ScriptedDriver driver;
    StateStore     store;
    Diagnostics    diagnostics;
    DeviceContext  context(driver, store, diagnostics, clockFn);

    driver.verdict = PollResult::TransportError;
    context.pollOnce();

    const auto snapshot = diagnostics.snapshot();
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.checksumErrorTotal);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.rs485TimeoutTotal);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.invalidFrameTotal);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.pollFailureTotal);
}

// uint32 counters on a bridge that runs for years: the difference has to survive the wrap, or
// the metric would go backwards by four billion once and be useless from then on.
static void test_the_difference_survives_a_counter_wrap() {
    ScriptedDriver driver;
    driver.counts.checksumErrors = 0xFFFFFFFEu;

    StateStore    store;
    Diagnostics   diagnostics;
    DeviceContext context(driver, store, diagnostics, clockFn);

    driver.counts.checksumErrors = 1;  // wrapped past 0xFFFFFFFF: three more errors
    context.pollOnce();
    TEST_ASSERT_EQUAL_UINT32(3, diagnostics.snapshot().checksumErrorTotal);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_successful_poll_still_reports_the_frames_it_lost);
    RUN_TEST(test_only_the_new_errors_are_added);
    RUN_TEST(test_errors_from_before_the_context_are_not_replayed);
    RUN_TEST(test_the_three_counters_do_not_bleed_into_each_other);
    RUN_TEST(test_a_transport_failure_moves_no_bus_counter);
    RUN_TEST(test_the_difference_survives_a_counter_wrap);
    return UNITY_END();
}
