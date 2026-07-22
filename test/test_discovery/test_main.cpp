// SPDX-License-Identifier: MIT
// Discovery scoring, and above all: when NOT to decide.

#include <unity.h>

#include <string>

#include "app/discovery_runner.h"
#include "drivers/discovery_engine.h"
#include "drivers/driver_registry.h"
#include "support/mock_transport.h"

using namespace heliograph;
using test::MockTransport;

static uint64_t g_now = 0;
static uint64_t clockFn() { return g_now; }

void setUp() { g_now = 1000; }
void tearDown() {}

/// A driver that reports whatever the test tells it to. Lets the scoring rules be tested
/// without any protocol in the way.
class FakeDriver : public InverterDriver {
public:
    struct Script {
        bool        responded  = true;
        bool        checksumValid = true;
        int         score      = 0;
        std::string serial     = "SER-1";
        std::string model      = "Model-1";
        /// Second and later probes report this serial instead, simulating an unstable match.
        std::string serialOnRepeat;
        bool        writeAttempted = false;
    };

    FakeDriver(DriverDescriptor d, Script* script) : descriptor_(std::move(d)), script_(script) {}

    const DriverDescriptor& descriptor() const override { return descriptor_; }
    bool begin(Transport&) override { return true; }

    ProbeResult probe() override {
        ++probeCount_;
        ProbeResult r;
        r.responded      = script_->responded;
        r.checksumValid  = script_->checksumValid;
        r.confidenceScore = script_->score;
        r.detectedModel  = script_->model;
        r.serialNumber = (probeCount_ > 1 && !script_->serialOnRepeat.empty())
                             ? script_->serialOnRepeat
                             : script_->serial;
        r.evidence.push_back("fake probe");
        return r;
    }

    PollResult           poll(DeviceState&) override { return PollResult::Ok; }
    DeviceIdentity       identity() const override { return {}; }
    InverterCapabilities capabilities() const override { return {}; }
    CommandResult        execute(const InverterCommand&) override {
        script_->writeAttempted = true;  // discovery must never reach this
        return CommandResult::Ok;
    }

private:
    DriverDescriptor descriptor_;
    Script*          script_;
    int              probeCount_ = 0;
};

static DriverDescriptor desc(const std::string& id, int priority, bool autoDetect = true) {
    DriverDescriptor d;
    d.id                    = id;
    d.displayName           = id;
    d.supportedTransports   = {TransportType::Mock};
    d.recommendedSerialProfiles = {SerialProfile{9600, SerialParity::None, 8, 1, 1000, 3}};
    d.probePriority         = priority;
    d.supportsAutoDetection = autoDetect;
    return d;
}

static void addDriver(DriverRegistry& r, const DriverDescriptor& d, FakeDriver::Script* script) {
    r.registerDriver(d,
                     [d, script](Transport&, const DriverOptions&) -> std::unique_ptr<InverterDriver> {
                         return std::make_unique<FakeDriver>(d, script);
                     });
}

// --- the happy path ------------------------------------------------------------------------

static void test_single_convincing_match_is_auto_selected() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("eversolar_like", 10), &s);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_TRUE(out.autoSelected);
    TEST_ASSERT_EQUAL_STRING("eversolar_like", out.selectedDriverId.c_str());
    TEST_ASSERT_EQUAL_size_t(1, out.candidates.size());
}

static void test_clear_winner_over_a_weak_match_is_auto_selected() {
    // The example from the brief: 97 vs 35 is not a close call.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script strong;
    strong.score = 97;
    strong.serial = "A";
    FakeDriver::Script weak;
    weak.score = 35;
    weak.serial = "B";
    addDriver(reg, desc("eversolar_like", 10), &strong);
    addDriver(reg, desc("generic_serial", 1), &weak);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_TRUE(out.autoSelected);
    TEST_ASSERT_EQUAL_STRING("eversolar_like", out.selectedDriverId.c_str());
    TEST_ASSERT_EQUAL_size_t(2, out.candidates.size());
    TEST_ASSERT_EQUAL_INT(97, out.candidates[0].probe.confidenceScore);  // sorted best first
}

// --- when not to decide ---------------------------------------------------------------------

static void test_two_close_candidates_are_never_auto_selected() {
    // The other example from the brief: Growatt MIC 82 vs MIN 78. Guessing here means
    // silently reading the wrong register map, which looks like plausible data.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script a;
    a.score = 82;
    a.serial = "A";
    FakeDriver::Script b;
    b.score = 78;
    b.serial = "B";
    addDriver(reg, desc("growatt_mic", 10), &a);
    addDriver(reg, desc("growatt_min", 10), &b);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_FALSE(out.autoSelected);
    TEST_ASSERT_TRUE(out.selectedDriverId.empty());
    TEST_ASSERT_EQUAL_size_t(2, out.candidates.size());
    // The user has to be able to judge it, so both candidates and a reason must be reported.
    TEST_ASSERT_TRUE(out.reason.find("too close") != std::string::npos);
}

static void test_a_score_below_the_threshold_is_never_auto_selected() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 79;  // one point short
    addDriver(reg, desc("almost", 10), &s);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_FALSE(out.autoSelected);
    TEST_ASSERT_EQUAL_size_t(1, out.candidates.size());
    TEST_ASSERT_TRUE(out.reason.find("below the threshold") != std::string::npos);
}

static void test_threshold_is_configurable() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 79;
    addDriver(reg, desc("almost", 10), &s);

    DiscoveryConfig c;
    c.minConfidence = 70;
    DiscoveryEngine e(reg, t);
    TEST_ASSERT_TRUE(e.run(DiscoveryMode::Quick, c).autoSelected);
}

static void test_inconsistent_probes_halve_the_score_and_block_selection() {
    // A device that identifies as something different on the second ask was never identified.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score          = 100;
    s.serial         = "SER-1";
    s.serialOnRepeat = "SER-2";
    addDriver(reg, desc("flaky", 10), &s);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_FALSE(out.autoSelected);
    TEST_ASSERT_EQUAL_INT(50, out.candidates[0].probe.confidenceScore);
    TEST_ASSERT_FALSE(out.candidates[0].consistent);
}

static void test_consistent_probes_are_recorded_as_evidence() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("solid", 10), &s);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_TRUE(out.candidates[0].consistent);
    TEST_ASSERT_EQUAL_INT(97, out.candidates[0].probe.confidenceScore);  // not halved
}

static void test_a_failed_checksum_blocks_selection() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score         = 97;
    s.checksumValid = false;
    addDriver(reg, desc("noisy", 10), &s);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_FALSE(out.autoSelected);
    TEST_ASSERT_TRUE(out.reason.find("checksum") != std::string::npos);
}

static void test_a_silent_bus_yields_no_candidates_and_a_useful_reason() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.responded = false;
    addDriver(reg, desc("absent", 10), &s);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_FALSE(out.autoSelected);
    TEST_ASSERT_EQUAL_size_t(0, out.candidates.size());
    TEST_ASSERT_TRUE(out.reason.find("wiring") != std::string::npos);
}

// --- safety --------------------------------------------------------------------------------

static void test_discovery_never_executes_a_command() {
    // The structural guarantee: discovery calls probe() and nothing else, so every forbidden
    // operation is unreachable rather than merely unwritten.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("any", 10), &s);

    DiscoveryEngine e(reg, t);
    e.run(DiscoveryMode::Extended);

    TEST_ASSERT_FALSE(s.writeAttempted);
}

static void test_drivers_opting_out_of_auto_detection_are_skipped() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script sim;
    sim.score = 100;
    addDriver(reg, desc("mock_like", -100, /*autoDetect=*/false), &sim);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    // A simulated device scoring 100 must not be discovered on a real bus.
    TEST_ASSERT_EQUAL_size_t(0, out.candidates.size());
    TEST_ASSERT_FALSE(out.autoSelected);
}

static void test_drivers_are_skipped_on_an_unsupported_transport() {
    DriverRegistry     reg;
    MockTransport      t;
    t.setType(TransportType::Tcp);
    FakeDriver::Script s;
    s.score = 97;
    auto d  = desc("serial_only", 10);
    d.supportedTransports = {TransportType::Rs485};
    addDriver(reg, d, &s);

    DiscoveryEngine e(reg, t);
    TEST_ASSERT_EQUAL_size_t(0, e.run(DiscoveryMode::Quick).candidates.size());
}

static void test_only_profiles_the_driver_recommends_are_tried() {
    // No brute-forcing baud rates on a live bus.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("one_profile", 10), &s);

    DiscoveryEngine e(reg, t);
    e.run(DiscoveryMode::Quick);

    TEST_ASSERT_EQUAL_UINT32(9600, t.profile().baudRate);
    TEST_ASSERT_TRUE(t.configureCalls <= 2);  // one profile, not a sweep
}

// --- the runner: handing a bus-bound job from the web task to rs485Task -----------------

static void test_a_request_is_not_run_by_the_requester() {
    // The whole point: probing takes the bus for seconds. Doing it inside an AsyncTCP callback
    // would block the web server and race the poll loop.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("any", 10), &s);
    DiscoveryRunner runner(reg, clockFn);

    TEST_ASSERT_TRUE(runner.request(false));
    TEST_ASSERT_EQUAL(DiscoveryStatus::Requested, runner.report().status);
    TEST_ASSERT_TRUE(runner.busy());
    // Nothing has touched the bus yet.
    TEST_ASSERT_EQUAL_UINT32(0, t.configureCalls);
}

static void test_the_bus_task_picks_the_request_up() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("any", 10), &s);
    DiscoveryRunner runner(reg, clockFn);

    runner.request(false);
    g_now += 500;
    TEST_ASSERT_TRUE(runner.runIfRequested(t));

    const auto r = runner.report();
    TEST_ASSERT_EQUAL(DiscoveryStatus::Done, r.status);
    TEST_ASSERT_TRUE(r.outcome.autoSelected);
    TEST_ASSERT_EQUAL_STRING("any", r.outcome.selectedDriverId.c_str());
    TEST_ASSERT_FALSE(runner.busy());
}

static void test_running_without_a_request_does_nothing() {
    DriverRegistry  reg;
    MockTransport   t;
    DiscoveryRunner runner(reg, clockFn);
    TEST_ASSERT_FALSE(runner.runIfRequested(t));
    TEST_ASSERT_EQUAL(DiscoveryStatus::Idle, runner.report().status);
}

static void test_a_second_request_is_refused_while_one_is_pending() {
    // The REST layer turns this into 409. Queueing would be worse: a second probe of the same
    // bus tells you nothing the first one will not.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    addDriver(reg, desc("any", 10), &s);
    DiscoveryRunner runner(reg, clockFn);

    TEST_ASSERT_TRUE(runner.request(false));
    TEST_ASSERT_FALSE(runner.request(false));
    TEST_ASSERT_FALSE(runner.request(true));

    runner.runIfRequested(t);
    // Once finished, a new run is allowed again.
    TEST_ASSERT_TRUE(runner.request(false));
}

static void test_a_new_run_discards_the_previous_result() {
    // A report half old and half new is worse than either: the bus may have changed.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("any", 10), &s);
    DiscoveryRunner runner(reg, clockFn);

    runner.request(false);
    runner.runIfRequested(t);
    TEST_ASSERT_EQUAL_size_t(1, runner.report().outcome.candidates.size());

    runner.request(false);
    TEST_ASSERT_EQUAL_size_t(0, runner.report().outcome.candidates.size());
    TEST_ASSERT_EQUAL(DiscoveryStatus::Requested, runner.report().status);
}

static void test_extended_mode_is_carried_through() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    addDriver(reg, desc("any", 10), &s);
    DiscoveryRunner runner(reg, clockFn);

    runner.request(true);
    TEST_ASSERT_EQUAL(DiscoveryMode::Extended, runner.report().mode);
    runner.request(false);  // refused while pending, so the mode must not change
    TEST_ASSERT_EQUAL(DiscoveryMode::Extended, runner.report().mode);
}

static void test_timings_are_recorded_for_the_wizard() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("any", 10), &s);
    DiscoveryRunner runner(reg, clockFn);

    runner.request(false);
    g_now += 200;
    runner.runIfRequested(t);

    const auto r = runner.report();
    TEST_ASSERT_EQUAL_UINT64(1000, r.requestedMs);
    TEST_ASSERT_EQUAL_UINT64(1200, r.startedMs);
    TEST_ASSERT_TRUE(r.finishedMs >= r.startedMs);
}

static void test_the_runner_never_executes_a_command() {
    // Same structural guarantee as the engine, now through the app layer.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("any", 10), &s);
    DiscoveryRunner runner(reg, clockFn);

    runner.request(true);
    runner.runIfRequested(t);
    TEST_ASSERT_FALSE(s.writeAttempted);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_request_is_not_run_by_the_requester);
    RUN_TEST(test_the_bus_task_picks_the_request_up);
    RUN_TEST(test_running_without_a_request_does_nothing);
    RUN_TEST(test_a_second_request_is_refused_while_one_is_pending);
    RUN_TEST(test_a_new_run_discards_the_previous_result);
    RUN_TEST(test_extended_mode_is_carried_through);
    RUN_TEST(test_timings_are_recorded_for_the_wizard);
    RUN_TEST(test_the_runner_never_executes_a_command);
    RUN_TEST(test_single_convincing_match_is_auto_selected);
    RUN_TEST(test_clear_winner_over_a_weak_match_is_auto_selected);
    RUN_TEST(test_two_close_candidates_are_never_auto_selected);
    RUN_TEST(test_a_score_below_the_threshold_is_never_auto_selected);
    RUN_TEST(test_threshold_is_configurable);
    RUN_TEST(test_inconsistent_probes_halve_the_score_and_block_selection);
    RUN_TEST(test_consistent_probes_are_recorded_as_evidence);
    RUN_TEST(test_a_failed_checksum_blocks_selection);
    RUN_TEST(test_a_silent_bus_yields_no_candidates_and_a_useful_reason);
    RUN_TEST(test_discovery_never_executes_a_command);
    RUN_TEST(test_drivers_opting_out_of_auto_detection_are_skipped);
    RUN_TEST(test_drivers_are_skipped_on_an_unsupported_transport);
    RUN_TEST(test_only_profiles_the_driver_recommends_are_tried);
    return UNITY_END();
}
