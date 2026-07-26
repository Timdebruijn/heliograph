// SPDX-License-Identifier: MIT
// Passive bus capture: idle-gap framing, the checksum verdicts that tell an operator whether
// their baud guess was right, the memory bounds, and the runner's handover to the bus task.

#include <unity.h>

#include <cstring>
#include <vector>

#include "app/capture_runner.h"
#include "diagnostics/frame_capture.h"
#include "protocols/modbus/modbus_rtu.h"
#include "protocols/pmu/pmu_protocol.h"
#include "support/mock_transport.h"

using namespace heliograph;
using heliograph::diag::CaptureConfig;
using heliograph::diag::FrameCapture;
using heliograph::test::MockTransport;

void setUp() {}
void tearDown() {}

static SerialProfile profileAt(uint32_t baud) {
    SerialProfile p;
    p.baudRate = baud;
    return p;
}

/// A well-formed Modbus RTU read request, CRC and all.
static std::vector<uint8_t> modbusFrame(uint8_t unit) {
    std::vector<uint8_t> f{unit, 0x03, 0x00, 0x00, 0x00, 0x02};
    const uint16_t       crc = modbus::crc16(f.data(), f.size());
    f.push_back(static_cast<uint8_t>(crc & 0xFF));
    f.push_back(static_cast<uint8_t>(crc >> 8));
    return f;
}

static void feed(FrameCapture& c, const std::vector<uint8_t>& bytes, uint64_t at) {
    c.feed(bytes.data(), bytes.size(), at);
}

// --------------------------------------------------------------------------------------
// The idle gap
// --------------------------------------------------------------------------------------

/// 3.5 characters at 9600 8N1 is 10 bits x 3.5 / 9600 = 3.65 ms, rounded up.
static void test_idle_gap_follows_the_baud_rate() {
    TEST_ASSERT_EQUAL_UINT32(4, diag::idleGapMsFor(profileAt(9600)));
    TEST_ASSERT_EQUAL_UINT32(2, diag::idleGapMsFor(profileAt(19200)));
}

/// Above 19200 the true gap is a fraction of a millisecond -- finer than anything polling a
/// UART can see. The floor states the resolution the reader actually has instead of claiming
/// one it does not; the header and the UI both say what that costs.
static void test_the_gap_is_floored_at_the_readers_resolution() {
    TEST_ASSERT_EQUAL_UINT32(2, diag::idleGapMsFor(profileAt(38400)));
    TEST_ASSERT_EQUAL_UINT32(2, diag::idleGapMsFor(profileAt(115200)));
}

/// Parity adds a bit to every character, so it lengthens the gap. Small, but it is the
/// difference between cutting frames correctly and merging them on a marginal line.
static void test_parity_lengthens_the_gap() {
    SerialProfile even = profileAt(9600);
    even.parity        = SerialParity::Even;
    TEST_ASSERT_TRUE(diag::idleGapMsFor(even) >= diag::idleGapMsFor(profileAt(9600)));
}

/// A zero baud rate cannot happen through the API, which bounds it -- but dividing by it
/// would be a crash rather than a bad number, so it is handled rather than assumed away.
static void test_a_zero_baud_rate_does_not_divide_by_zero() {
    TEST_ASSERT_EQUAL_UINT32(2, diag::idleGapMsFor(profileAt(0)));
}

// --------------------------------------------------------------------------------------
// Framing
// --------------------------------------------------------------------------------------

static void test_silence_between_bursts_cuts_them_into_frames() {
    CaptureConfig config;
    config.idleGapMs = 4;
    FrameCapture capture(config, profileAt(9600));
    capture.begin(1000);

    feed(capture, {0x01, 0x03, 0x04}, 1000);
    feed(capture, {0x05, 0x06}, 1001);       // 1 ms later: same frame
    capture.feed(nullptr, 0, 1010);          // 9 ms of silence: cuts it
    feed(capture, {0x02, 0x03}, 1010);
    capture.finish(1020);

    TEST_ASSERT_EQUAL_size_t(2, capture.frames().size());
    TEST_ASSERT_EQUAL_size_t(5, capture.frames()[0].bytes.size());
    TEST_ASSERT_EQUAL_size_t(2, capture.frames()[1].bytes.size());
    TEST_ASSERT_EQUAL_UINT32(0, capture.frames()[0].offsetMs);
    TEST_ASSERT_EQUAL_UINT32(10, capture.frames()[1].offsetMs);
    TEST_ASSERT_EQUAL_UINT32(9, capture.frames()[1].gapBeforeMs);
    TEST_ASSERT_EQUAL_UINT32(7, capture.totalBytes());
}

/// The zero-length feed is not a nicety: it is the only thing that advances time while the
/// line is quiet. Without it the last burst before a silence would never be closed, and a
/// reader that only fed real bytes would emit one record per traffic burst.
static void test_a_trailing_frame_is_closed_by_finish() {
    CaptureConfig config;
    config.idleGapMs = 4;
    FrameCapture capture(config, profileAt(9600));
    capture.begin(0);
    feed(capture, {0xAA, 0x55}, 1);
    TEST_ASSERT_EQUAL_size_t(0, capture.frames().size());  // still open
    capture.finish(2);
    TEST_ASSERT_EQUAL_size_t(1, capture.frames().size());
}

/// A record at its byte bound is cut and a new one started rather than dropping the overflow.
/// On a fast line, where the gap is unresolvable, this is the only thing keeping the entire
/// capture from becoming one enormous record.
static void test_an_overlong_burst_is_split_rather_than_truncated() {
    CaptureConfig config;
    config.idleGapMs     = 4;
    config.maxFrameBytes = 8;
    FrameCapture capture(config, profileAt(115200));
    capture.begin(0);
    feed(capture, std::vector<uint8_t>(20, 0x5A), 0);
    capture.finish(1);

    TEST_ASSERT_EQUAL_size_t(3, capture.frames().size());
    TEST_ASSERT_EQUAL_size_t(8, capture.frames()[0].bytes.size());
    TEST_ASSERT_EQUAL_size_t(8, capture.frames()[1].bytes.size());
    TEST_ASSERT_EQUAL_size_t(4, capture.frames()[2].bytes.size());
    TEST_ASSERT_EQUAL_UINT32(20, capture.totalBytes());
    // A cut the protocol did not make says so: no silence preceded it.
    TEST_ASSERT_EQUAL_UINT32(0, capture.frames()[1].gapBeforeMs);
}

// --------------------------------------------------------------------------------------
// Checksum verdicts -- the "is my baud rate right" signal
// --------------------------------------------------------------------------------------

static void test_a_real_modbus_frame_is_recognised() {
    CaptureConfig config;
    config.idleGapMs = 4;
    FrameCapture capture(config, profileAt(9600));
    capture.begin(0);
    feed(capture, modbusFrame(1), 0);
    capture.finish(1);

    TEST_ASSERT_EQUAL_size_t(1, capture.frames().size());
    TEST_ASSERT_TRUE(capture.frames()[0].modbusCrcValid);
    TEST_ASSERT_EQUAL_UINT32(1, capture.modbusFrames());
}

/// The whole point of the verdict. A capture at the wrong baud produces plenty of bytes and no
/// valid checksums, and that count is what tells the operator to try another rate rather than
/// concluding the device is mute.
static void test_a_corrupted_frame_reports_a_failed_crc() {
    CaptureConfig config;
    config.idleGapMs = 4;
    FrameCapture capture(config, profileAt(9600));
    capture.begin(0);
    auto bad = modbusFrame(1);
    bad[3] ^= 0xFF;  // one flipped bit in the payload, CRC now wrong
    feed(capture, bad, 0);
    capture.finish(1);

    TEST_ASSERT_FALSE(capture.frames()[0].modbusCrcValid);
    TEST_ASSERT_EQUAL_UINT32(0, capture.modbusFrames());
}

static void test_an_aa55_frame_is_recognised_as_its_own_family() {
    CaptureConfig config;
    config.idleGapMs = 4;
    FrameCapture capture(config, profileAt(9600));
    capture.begin(0);

    uint8_t buf[64];
    size_t  n = 0;
    TEST_ASSERT_EQUAL(pmu::BuildResult::Ok,
                      pmu::buildRequest(pmu::cmd::kOfflineQuery, pmu::kBroadcastAddress, nullptr,
                                        0, buf, sizeof(buf), n));
    TEST_ASSERT_TRUE(n > 0);
    feed(capture, std::vector<uint8_t>(buf, buf + n), 0);
    capture.finish(1);

    TEST_ASSERT_TRUE(capture.frames()[0].pmuFrameValid);
    TEST_ASSERT_EQUAL_UINT32(1, capture.pmuFrames());
}

/// Too short to carry a CRC at all. "No checksum to check" must not read as "checksum failed",
/// and must certainly not read past the front of the buffer looking for one.
static void test_a_two_byte_record_is_not_claimed_either_way() {
    CaptureConfig config;
    config.idleGapMs = 4;
    FrameCapture capture(config, profileAt(9600));
    capture.begin(0);
    feed(capture, {0x01, 0x02}, 0);
    capture.finish(1);

    TEST_ASSERT_FALSE(capture.frames()[0].modbusCrcValid);
    TEST_ASSERT_FALSE(capture.frames()[0].pmuFrameValid);
}

// --------------------------------------------------------------------------------------
// Bounds
// --------------------------------------------------------------------------------------

/// Full means STOPPED, not "oldest dropped". For a handshake -- the case this feature exists
/// for -- the interesting part is the registration dance at the beginning, and a ring buffer
/// would reliably discard exactly that.
static void test_filling_up_keeps_the_first_frames_and_says_so() {
    CaptureConfig config;
    config.idleGapMs = 4;
    config.maxFrames = 2;
    FrameCapture capture(config, profileAt(9600));
    capture.begin(0);
    feed(capture, {0x01}, 0);
    feed(capture, {0x02}, 100);
    feed(capture, {0x03}, 200);
    feed(capture, {0x04}, 300);
    capture.finish(400);

    TEST_ASSERT_EQUAL_size_t(2, capture.frames().size());
    TEST_ASSERT_EQUAL_UINT8(0x01, capture.frames()[0].bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02, capture.frames()[1].bytes[0]);
    TEST_ASSERT_TRUE(capture.truncated());
    TEST_ASSERT_TRUE(capture.done(1));
}

/// The bound that actually holds. maxFrames and maxFrameBytes are a product, and a product of
/// two independently chosen limits bounds nothing -- 256 records of 256 bytes renders to about
/// 220 KB of JSON hex, which no board here can hold. This caps the payload directly.
static void test_the_byte_ceiling_stops_the_capture() {
    CaptureConfig config;
    config.idleGapMs     = 4;
    config.maxTotalBytes = 10;
    config.maxFrames     = 100;
    config.maxFrameBytes = 100;
    FrameCapture capture(config, profileAt(9600));
    capture.begin(0);
    feed(capture, std::vector<uint8_t>(50, 0x11), 0);
    capture.finish(1);

    TEST_ASSERT_EQUAL_UINT32(10, capture.totalBytes());
    TEST_ASSERT_TRUE(capture.truncated());
    TEST_ASSERT_TRUE(capture.done(1));
}

static void test_the_window_ends_the_capture() {
    CaptureConfig config;
    config.durationMs = 50;
    FrameCapture capture(config, profileAt(9600));
    capture.begin(1000);
    TEST_ASSERT_FALSE(capture.done(1049));
    TEST_ASSERT_TRUE(capture.done(1050));
    TEST_ASSERT_FALSE(capture.truncated());  // time ran out, it did not fill up
}

// --------------------------------------------------------------------------------------
// The runner: handover, bus ownership, failure paths
// --------------------------------------------------------------------------------------

static uint64_t g_clock = 0;

static CaptureConfig shortWindow() {
    CaptureConfig config;
    config.durationMs = 20;
    config.idleGapMs  = 4;
    return config;
}

static void test_nothing_runs_until_something_is_requested() {
    CaptureRunner runner([] { return g_clock; });
    MockTransport transport;
    TEST_ASSERT_FALSE(runner.busy());
    TEST_ASSERT_FALSE(runner.runIfRequested(transport));
    TEST_ASSERT_EQUAL(CaptureStatus::Idle, runner.report().status);
}

/// Bytes have to arrive DURING the window, not before it: runIfRequested flushes the input
/// first, precisely so traffic that predates the capture cannot be timestamped as its first
/// frame. injectNoise() lands in the buffer that flush clears, so this drives the line through
/// the noise pattern instead -- one byte per read, which is what a live bus looks like to this
/// loop.
static void test_a_requested_capture_reads_the_line_and_reports() {
    CaptureRunner runner([] { return g_clock; });
    MockTransport transport;
    transport.msPerRead    = 1;  // the simulated clock only moves when the loop reads
    transport.infiniteNoise = true;
    transport.noisePattern  = modbusFrame(3);

    TEST_ASSERT_TRUE(runner.request(shortWindow(), profileAt(9600)));
    TEST_ASSERT_TRUE(runner.busy());
    TEST_ASSERT_EQUAL(CaptureStatus::Requested, runner.report().status);

    TEST_ASSERT_TRUE(runner.runIfRequested(transport));
    const auto report = runner.report();
    TEST_ASSERT_EQUAL(CaptureStatus::Done, report.status);
    // A byte every millisecond never reaches the 4 ms gap, so it is one continuous record --
    // which is exactly what a fast line looks like, and what the framing note warns about.
    TEST_ASSERT_EQUAL_size_t(1, report.frames.size());
    TEST_ASSERT_TRUE(report.totalBytes >= 15);
    TEST_ASSERT_EQUAL_UINT32(report.totalBytes, report.frames[0].bytes.size());
    TEST_ASSERT_EQUAL_UINT32(4, report.idleGapMs);
    TEST_ASSERT_FALSE(runner.busy());
}

/// Traffic already buffered when the capture starts belongs to whatever the driver was doing.
/// Recording it would put a frame at offset 0 that the capture never witnessed.
static void test_traffic_from_before_the_window_is_not_recorded() {
    CaptureRunner runner([] { return g_clock; });
    MockTransport transport;
    transport.msPerRead = 1;
    transport.injectNoise(modbusFrame(3));  // arrived before the capture was asked for

    runner.request(shortWindow(), profileAt(9600));
    runner.runIfRequested(transport);
    TEST_ASSERT_EQUAL_UINT32(0, runner.report().totalBytes);
}

/// The bus is taken for the whole window and the line is put on the requested settings --
/// which for an unidentified device is exactly the point, since the driver's guess is what
/// failed. The buffer is flushed first so traffic that predates the capture cannot be
/// timestamped as its first frame.
static void test_the_capture_owns_the_bus_and_sets_the_line() {
    CaptureRunner runner([] { return g_clock; });
    MockTransport transport;
    transport.msPerRead = 1;

    runner.request(shortWindow(), profileAt(19200));
    runner.runIfRequested(transport);

    TEST_ASSERT_EQUAL_INT(1, transport.lockCalls);
    TEST_ASSERT_EQUAL_INT(1, transport.unlockCalls);
    TEST_ASSERT_FALSE(transport.locked);
    TEST_ASSERT_EQUAL_INT(1, transport.configureCalls);
    TEST_ASSERT_EQUAL_INT(1, transport.flushCalls);
    // Passive throughout: a capture that transmitted would be recording its own voice, and on
    // an unknown protocol a stray write is the one thing that could actually disturb a device.
    TEST_ASSERT_EQUAL_size_t(0, transport.writes.size());
}

static void test_a_second_request_is_refused_while_one_is_pending() {
    CaptureRunner runner([] { return g_clock; });
    TEST_ASSERT_TRUE(runner.request(shortWindow(), profileAt(9600)));
    TEST_ASSERT_FALSE(runner.request(shortWindow(), profileAt(19200)));
}

/// A capture cannot wait for the bus: it would hold the task while the window ran down and
/// then report an empty result as though the line were silent.
static void test_a_busy_bus_fails_the_capture_with_a_reason() {
    CaptureRunner runner([] { return g_clock; });
    MockTransport transport;
    transport.lockFails = true;

    runner.request(shortWindow(), profileAt(9600));
    TEST_ASSERT_TRUE(runner.runIfRequested(transport));
    const auto report = runner.report();
    TEST_ASSERT_EQUAL(CaptureStatus::Failed, report.status);
    TEST_ASSERT_FALSE(report.error.empty());
    TEST_ASSERT_FALSE(runner.busy());  // and a retry is possible
    TEST_ASSERT_EQUAL_INT(0, transport.configureCalls);
    // The caller keys its recovery off this. Restoring a line that was never touched means
    // calling begin() on every driver, which on the AA55 family is a registration handshake --
    // real traffic, on a bus that has just reported itself busy.
    TEST_ASSERT_FALSE(report.lineReconfigured);
}

/// The other failure. Here the UART may well have been half-reconfigured before it refused, so
/// the driver's settings DO have to go back on -- the opposite answer to the case above, which
/// is why one boolean carries it rather than the status.
static void test_failing_line_settings_still_ask_for_the_line_back() {
    CaptureRunner runner([] { return g_clock; });
    MockTransport transport;
    transport.configureSucceeds = false;

    runner.request(shortWindow(), profileAt(9600));
    runner.runIfRequested(transport);
    TEST_ASSERT_EQUAL(CaptureStatus::Failed, runner.report().status);
    TEST_ASSERT_TRUE(runner.report().lineReconfigured);
}

static void test_a_completed_capture_asks_for_the_line_back() {
    CaptureRunner runner([] { return g_clock; });
    MockTransport transport;
    transport.msPerRead = 1;

    runner.request(shortWindow(), profileAt(19200));
    runner.runIfRequested(transport);
    TEST_ASSERT_EQUAL(CaptureStatus::Done, runner.report().status);
    TEST_ASSERT_TRUE(runner.report().lineReconfigured);
}

static void test_line_settings_that_will_not_apply_fail_rather_than_record_nothing() {
    CaptureRunner runner([] { return g_clock; });
    MockTransport transport;
    transport.configureSucceeds = false;

    runner.request(shortWindow(), profileAt(9600));
    TEST_ASSERT_TRUE(runner.runIfRequested(transport));
    TEST_ASSERT_EQUAL(CaptureStatus::Failed, runner.report().status);
    // The bus is handed back even on the failure path.
    TEST_ASSERT_FALSE(transport.locked);
}

/// The watchdog feed. A window is tens of seconds inside ONE rs485Task iteration, so without a
/// per-iteration callback the task watchdog resets the board mid-capture.
static void test_the_tick_callback_fires_throughout_the_window() {
    CaptureRunner runner([] { return g_clock; });
    MockTransport transport;
    transport.msPerRead = 1;

    int ticks = 0;
    runner.request(shortWindow(), profileAt(9600));
    runner.runIfRequested(transport, [&ticks] { ++ticks; });
    // 20 ms of window at 1 ms per read: many feeds, not one.
    TEST_ASSERT_TRUE(ticks >= 10);
}

/// A new run replaces the old report rather than appending. Two runs at different line
/// settings in one report would make the checksum counts meaningless -- their whole value is
/// that they describe a single guess at the line.
static void test_a_new_capture_discards_the_previous_result() {
    CaptureRunner runner([] { return g_clock; });
    MockTransport transport;
    transport.msPerRead     = 1;
    transport.infiniteNoise = true;
    transport.noisePattern  = modbusFrame(3);
    runner.request(shortWindow(), profileAt(9600));
    runner.runIfRequested(transport);
    TEST_ASSERT_EQUAL_size_t(1, runner.report().frames.size());

    runner.request(shortWindow(), profileAt(19200));
    TEST_ASSERT_EQUAL_size_t(0, runner.report().frames.size());
    TEST_ASSERT_EQUAL_UINT32(0, runner.report().totalBytes);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_idle_gap_follows_the_baud_rate);
    RUN_TEST(test_the_gap_is_floored_at_the_readers_resolution);
    RUN_TEST(test_parity_lengthens_the_gap);
    RUN_TEST(test_a_zero_baud_rate_does_not_divide_by_zero);
    RUN_TEST(test_silence_between_bursts_cuts_them_into_frames);
    RUN_TEST(test_a_trailing_frame_is_closed_by_finish);
    RUN_TEST(test_an_overlong_burst_is_split_rather_than_truncated);
    RUN_TEST(test_a_real_modbus_frame_is_recognised);
    RUN_TEST(test_a_corrupted_frame_reports_a_failed_crc);
    RUN_TEST(test_an_aa55_frame_is_recognised_as_its_own_family);
    RUN_TEST(test_a_two_byte_record_is_not_claimed_either_way);
    RUN_TEST(test_filling_up_keeps_the_first_frames_and_says_so);
    RUN_TEST(test_the_byte_ceiling_stops_the_capture);
    RUN_TEST(test_the_window_ends_the_capture);
    RUN_TEST(test_nothing_runs_until_something_is_requested);
    RUN_TEST(test_a_requested_capture_reads_the_line_and_reports);
    RUN_TEST(test_traffic_from_before_the_window_is_not_recorded);
    RUN_TEST(test_the_capture_owns_the_bus_and_sets_the_line);
    RUN_TEST(test_a_second_request_is_refused_while_one_is_pending);
    RUN_TEST(test_a_busy_bus_fails_the_capture_with_a_reason);
    RUN_TEST(test_failing_line_settings_still_ask_for_the_line_back);
    RUN_TEST(test_a_completed_capture_asks_for_the_line_back);
    RUN_TEST(test_line_settings_that_will_not_apply_fail_rather_than_record_nothing);
    RUN_TEST(test_the_tick_callback_fires_throughout_the_window);
    RUN_TEST(test_a_new_capture_discards_the_previous_result);
    return UNITY_END();
}
