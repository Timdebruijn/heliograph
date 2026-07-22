// SPDX-License-Identifier: MIT
// Full poll cycle against a simulated inverter: no hardware, and it works at night.

#include <unity.h>

#include <vector>

#include "device/device_context.h"
#include "diagnostics/diagnostics.h"
#include "drivers/eversolar_legacy/eversolar_driver.h"
#include "fixtures/eversolar_frames.h"
#include "state/state_store.h"
#include "support/fake_eversolar_device.h"
#include "support/mock_transport.h"

using namespace heliograph;
using namespace heliograph::eversolar;
namespace fx = heliograph::fixtures;
using test::FakeEversolarDevice;
using test::MockTransport;
using Payload = FakeEversolarDevice::Payload;

static uint64_t g_now = 0;
static uint64_t clockFn() { return g_now; }

void setUp() { g_now = 1000; }
void tearDown() {}

/// A driver wired to a simulated inverter. Tests describe the device, not the byte script.
struct Rig {
    MockTransport       transport;
    FakeEversolarDevice device;
    EversolarDriver     driver{transport};

    Rig() { device.installOn(transport); }
    bool begin() { return driver.begin(transport); }
    PollResult poll(DeviceState& s) { return driver.poll(s); }
};

// --- begin ----------------------------------------------------------------------------------

static void test_begin_configures_the_only_known_profile() {
    Rig r;
    TEST_ASSERT_TRUE(r.begin());

    // 9600 8N1 is hardcoded in the reference; the driver must not invent alternatives.
    TEST_ASSERT_EQUAL_UINT32(9600, r.transport.profile().baudRate);
    TEST_ASSERT_EQUAL(SerialParity::None, r.transport.profile().parity);
    TEST_ASSERT_EQUAL_UINT8(8, r.transport.profile().dataBits);
    TEST_ASSERT_EQUAL_UINT8(1, r.transport.profile().stopBits);
}

static void test_begin_broadcasts_re_register() {
    Rig r;
    r.begin();

    TEST_ASSERT_EQUAL_UINT32(3, r.device.reRegisterCount);
    TEST_ASSERT_EQUAL_size_t(3, r.transport.writes.size());
    for (const auto& w : r.transport.writes) {
        TEST_ASSERT_EQUAL_size_t(fx::kReqReRegisterLen, w.size());
        TEST_ASSERT_EQUAL_UINT8_ARRAY(fx::kReqReRegister, w.data(), w.size());
    }
}

static void test_begin_fails_when_the_line_cannot_be_configured() {
    Rig r;
    r.transport.configureSucceeds = false;
    TEST_ASSERT_FALSE(r.begin());
}

static void test_begin_declares_no_capabilities_it_cannot_deliver() {
    Rig r;
    r.begin();
    const auto caps = r.driver.capabilities();

    TEST_ASSERT_TRUE(caps.isReadOnly());
    TEST_ASSERT_TRUE(caps.has(InverterCapability::ReadAcPower));
    TEST_ASSERT_TRUE(caps.has(InverterCapability::ReadEnergyTotal));
    // The protocol has no error code field and no battery, so claiming these would be a lie.
    TEST_ASSERT_FALSE(caps.has(InverterCapability::ReadErrors));
    TEST_ASSERT_FALSE(caps.has(InverterCapability::ReadBatteryState));
    TEST_ASSERT_FALSE(caps.has(InverterCapability::ReadMultiplePhases));
    TEST_ASSERT_EQUAL_UINT8(1, caps.phaseCount);
    TEST_ASSERT_FALSE(caps.hasBattery);
}

// --- registration ------------------------------------------------------------------------------

static void test_poll_registers_before_reading() {
    Rig r;
    r.begin();

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, r.poll(state));
    TEST_ASSERT_TRUE(r.driver.registered());
    TEST_ASSERT_EQUAL_STRING(fx::kExpectedSerial, state.identity.serialNumber.c_str());
    TEST_ASSERT_EQUAL_STRING("Ever-Solar", state.identity.manufacturer.c_str());
    TEST_ASSERT_EQUAL_STRING("eversolar_legacy", state.identity.driverId.c_str());
}

static void test_registration_succeeds_against_the_captured_hardware_frame() {
    // Byte for byte the offline-query response a real TL3000-20 sent on 2026-07-19. It answers
    // from source 00 00 -- it has no bus address yet -- where the constructed fixtures guessed
    // 00 10. Rejecting this frame is exactly the bug found on first hardware contact.
    Rig r;
    r.begin();
    r.device.useCapturedOfflineQuery = true;

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, r.poll(state));
    TEST_ASSERT_TRUE(r.driver.registered());
    TEST_ASSERT_EQUAL_STRING("XH30006011550619", state.identity.serialNumber.c_str());
    // The bare model, not the whole composite id string: that string as a "model" made every
    // Home Assistant device and entity name unreadable.
    TEST_ASSERT_EQUAL_STRING("TL3000-20", state.identity.model.c_str());
    // Status 1 has a measured meaning (our capture + the ha-zeversolar-modbus calibration:
    // grid-connected, normal). Anything else must stay honestly unknown.
    TEST_ASSERT_EQUAL_UINT16(1, state.statusCode);
    TEST_ASSERT_EQUAL_STRING("Grid-connected (normal)", state.statusText.c_str());
}

static void test_one_missed_reply_does_not_drop_the_registration() {
    // Seen live (2026-07-19 12:48): one lost QUERY_NORMAL_INFO reply made the driver forget
    // its registration and fall back to the offline query -- which a still-registered
    // inverter ignores by design. One dropped frame on a real bus must cost one poll,
    // not the whole session.
    Rig r;
    r.begin();
    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, r.poll(state));  // registers and reads

    r.device.silentNormalInfoPolls = 1;
    TEST_ASSERT_EQUAL(PollResult::Timeout, r.poll(state));
    TEST_ASSERT_TRUE(r.driver.registered());  // still registered after a single miss

    const size_t writesBefore = r.transport.writes.size();
    TEST_ASSERT_EQUAL(PollResult::Ok, r.poll(state));  // next poll reads again, directly
    // Exactly one write happened: the normal-info query. No offline query, no re-register.
    TEST_ASSERT_EQUAL_size_t(writesBefore + 1, r.transport.writes.size());
    TEST_ASSERT_EQUAL_UINT8(0x11, r.transport.writes.back()[kOffsetControl]);
}

static void test_registration_recovers_a_registered_but_forgotten_inverter() {
    // The stuck state from the live wizard run: the inverter holds an address (assigned by a
    // probe or an earlier session) while the driver starts fresh. The offline query alone
    // stays silent forever; only a RE_REGISTER broadcast makes the inverter forget and
    // answer again. The driver must fall back to that by itself.
    Rig r;
    r.begin();
    r.device.registered = true;  // inverter side remembers; driver side knows nothing

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, r.poll(state));
    TEST_ASSERT_TRUE(r.driver.registered());
}

static void test_nothing_written_to_the_bus_is_a_write_command() {
    Rig r;
    r.begin();
    DeviceState state;
    r.poll(state);
    r.driver.probe();

    // Control codes 0x12 (WRITE) and 0x13 (EXECUTE) must never appear. This covers the
    // registration handshake and the probe, which are the only things discovery may run.
    TEST_ASSERT_TRUE(r.transport.writes.size() > 4);
    for (const auto& w : r.transport.writes) {
        TEST_ASSERT_TRUE(w.size() > kOffsetControl);
        TEST_ASSERT_NOT_EQUAL(0x12, w[kOffsetControl]);
        TEST_ASSERT_NOT_EQUAL(0x13, w[kOffsetControl]);
    }
}

static void test_poll_reports_not_registered_when_nobody_answers() {
    Rig r;
    r.begin();
    r.device.offline = true;

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::NotRegistered, r.poll(state));
    TEST_ASSERT_FALSE(r.driver.registered());
}

static void test_registration_refused_without_ack() {
    Rig r;
    r.begin();
    r.device.refuseRegistration = true;

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::NotRegistered, r.poll(state));
    TEST_ASSERT_FALSE(r.driver.registered());
}

// --- measurements ------------------------------------------------------------------------------

static void test_poll_fills_the_canonical_model() {
    Rig r;
    r.begin();

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, r.poll(state));

    const auto& m = state.measurements;
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, fx::expected::kAcPowerW,
                              m.find(measurement_id::kAcPowerTotal)->value);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, fx::expected::kEnergyTotalKwh,
                              m.find(measurement_id::kEnergyTotal)->value);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, fx::expected::kEnergyTodayKwh,
                              m.find(measurement_id::kEnergyToday)->value);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, fx::expected::kTemperatureC,
                              m.find(measurement_id::kTemperature)->value);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, fx::expected::kAcVoltage,
                              m.find(measurement_id::kAcL1Voltage)->value);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, fx::expected::kFrequencyHz,
                              m.find(measurement_id::kAcFrequency)->value);
}

static void test_dc_power_is_marked_derived() {
    Rig r;
    r.begin();
    DeviceState state;
    r.poll(state);

    // The inverter does not report DC power; it is V x I. Consumers must be able to tell.
    const auto* dc = state.measurements.find(measurement_id::kDcPowerTotal);
    TEST_ASSERT_TRUE(dc->derived);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, fx::expected::kPvVoltage1 * fx::expected::kPvCurrent1,
                              dc->value);
    TEST_ASSERT_FALSE(state.measurements.find(measurement_id::kAcPowerTotal)->derived);
}

static void test_single_string_declares_no_second_mppt() {
    Rig r;
    r.begin();
    DeviceState state;
    r.poll(state);

    // Not "present but zero": absent. Otherwise MPPT2 would read 0 V forever.
    TEST_ASSERT_NULL(state.measurements.find(measurement_id::kDcMppt2Voltage));
    TEST_ASSERT_EQUAL_UINT8(1, state.capabilities.mpptCount);
    TEST_ASSERT_FALSE(state.capabilities.has(InverterCapability::ReadMultipleMppts));
}

static void test_dual_string_declares_the_second_mppt() {
    Rig r;
    r.begin();
    r.device.payload = Payload::DualString;

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, r.poll(state));

    TEST_ASSERT_DOUBLE_WITHIN(1e-6, fx::expected::kPvVoltage2,
                              state.measurements.find(measurement_id::kDcMppt2Voltage)->value);
    TEST_ASSERT_EQUAL_UINT8(2, state.capabilities.mpptCount);
    TEST_ASSERT_TRUE(state.capabilities.has(InverterCapability::ReadMultipleMppts));
}

static void test_status_text_does_not_invent_a_meaning() {
    Rig r;
    r.begin();
    DeviceState state;
    r.poll(state);

    // Code 1 is the exception with a *measured* meaning (our 2026-07-19 capture during
    // grid-tied production + the ha-zeversolar-modbus calibration agree). The rest of the
    // OP_MODE space stays undocumented, and naming any other code would be a fabrication --
    // covered by test_unknown_status_codes_stay_unknown below.
    TEST_ASSERT_EQUAL_UINT16(1, state.statusCode);
    TEST_ASSERT_EQUAL_STRING("Grid-connected (normal)", state.statusText.c_str());
    TEST_ASSERT_FALSE(state.errorCodeSupported);
}

static void test_op_mode_zero_is_standby() {
    // OP_MODE 0's meaning is now OBSERVED, four independent events in the HA history
    // (2026-07-19/20/21 dusk, 2026-07-22 dawn): the inverter answers with code 0 for a few
    // minutes around shutdown and startup, while producing nothing, then flips to/from
    // code 1. That is standby, not an unknown.
    Rig r;
    r.begin();
    r.device.payload = Payload::Night;
    DeviceState state;
    r.poll(state);

    TEST_ASSERT_EQUAL_UINT16(0, state.statusCode);
    TEST_ASSERT_EQUAL_STRING("Standby (not feeding)", state.statusText.c_str());
}

static void test_unobserved_status_codes_stay_unknown() {
    // Any code never seen on hardware (2..n) keeps its honest "Unknown (n)" presentation;
    // the mapping function itself is pure and pinned here.
    TEST_ASSERT_EQUAL_STRING("Standby (not feeding)", eversolar::opModeText(0));
    TEST_ASSERT_EQUAL_STRING("Grid-connected (normal)", eversolar::opModeText(1));
    TEST_ASSERT_EQUAL_STRING("", eversolar::opModeText(2));
    TEST_ASSERT_EQUAL_STRING("", eversolar::opModeText(999));
}

// --- failure handling ---------------------------------------------------------------------------

static void test_corrupt_reply_does_not_touch_state() {
    Rig r;
    r.begin();
    DeviceState state;
    r.poll(state);
    const double good = state.measurements.find(measurement_id::kAcPowerTotal)->value;

    r.device.payload = Payload::BadChecksum;
    TEST_ASSERT_EQUAL(PollResult::ChecksumError, r.poll(state));

    // The previous reading survives untouched; a corrupt frame yields no data at all.
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, good,
                              state.measurements.find(measurement_id::kAcPowerTotal)->value);
    TEST_ASSERT_TRUE(r.driver.checksumErrors() > 0);
}

static void test_reply_of_wrong_length_is_an_invalid_frame() {
    Rig r;
    r.begin();
    r.device.payload = Payload::BadLength;

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::InvalidFrame, r.poll(state));
    TEST_ASSERT_NULL(state.measurements.find(measurement_id::kAcPowerTotal));
}

static void test_a_run_of_timeouts_keeps_the_registration_and_never_broadcasts() {
    // Superseded contract: the driver used to drop the registration after three timeouts and
    // re-register with a RE_REGISTER broadcast. On real hardware that looped through sunrise
    // (2026-07-21): the inverter flickers on and off at its start threshold, and every
    // measurement query then landed right after a fresh registration, which it ignored. The
    // reference logger keeps the registration and polls patiently; so do we now.
    Rig r;
    r.begin();
    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, r.poll(state));
    TEST_ASSERT_TRUE(r.driver.registered());
    const uint32_t broadcastsAfterConnect = r.device.reRegisterCount;

    // Registered, answering id, but withholding measurements -- the dawn state.
    r.device.withholdNormalInfo = true;
    for (int i = 0; i < 8; ++i) {
        TEST_ASSERT_EQUAL(PollResult::Timeout, r.poll(state));
        TEST_ASSERT_TRUE(r.driver.registered());  // never dropped
    }
    // And crucially: not one RE_REGISTER broadcast the whole time.
    TEST_ASSERT_EQUAL_UINT32(broadcastsAfterConnect, r.device.reRegisterCount);

    // When it finally starts producing, the very next poll reads -- no re-registration dance.
    r.device.withholdNormalInfo = false;
    TEST_ASSERT_EQUAL(PollResult::Ok, r.poll(state));
    TEST_ASSERT_EQUAL_UINT32(broadcastsAfterConnect, r.device.reRegisterCount);
}

static void test_a_sustained_noise_trickle_hits_the_transaction_deadline() {
    // A line that never falls silent and never forms a valid frame must not hold the bus
    // forever. Without the deadline this loops until the watchdog reboots (review, 2026-07-20).
    MockTransport transport;
    transport.infiniteNoise = true;   // read() always returns a byte, never a frame
    transport.msPerRead     = 100;    // each read advances the sim clock 100 ms
    EversolarDriver driver(transport);
    driver.begin(transport);

    DeviceState state;
    // Registration's first offline query runs straight into the trickle and must give up.
    const PollResult r = driver.poll(state);
    TEST_ASSERT_EQUAL(PollResult::NotRegistered, r);
    // The transaction bounded itself in wall-clock terms: the sim clock advanced to roughly
    // the deadline rather than running away.
    TEST_ASSERT_TRUE(transport.nowMs() >= 2000);
    TEST_ASSERT_TRUE(transport.nowMs() < 10000);
}

static void test_an_inverter_that_returns_addressless_is_recovered_without_a_broadcast() {
    // The other half of the dawn flicker: the inverter drops out completely (loses its
    // volatile address) and comes back address-less. It answers the offline query again, so
    // the non-disruptive recovery probe picks it up -- no broadcast needed.
    Rig r;
    r.begin();
    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, r.poll(state));
    const uint32_t broadcastsAfterConnect = r.device.reRegisterCount;

    // Drops out: address forgotten. A few polls miss.
    r.device.offline = true;
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL(PollResult::Timeout, r.poll(state));
    }
    // Comes back, still address-less (offline cleared, registered stayed false).
    r.device.offline = false;
    // Within a couple of polls the recovery probe re-registers it and reads, with no broadcast.
    PollResult last = PollResult::Timeout;
    for (int i = 0; i < 3; ++i) {
        last = r.poll(state);
        if (last == PollResult::Ok) break;
    }
    TEST_ASSERT_EQUAL(PollResult::Ok, last);
    TEST_ASSERT_EQUAL_UINT32(broadcastsAfterConnect, r.device.reRegisterCount);
}

static void test_echo_of_our_own_request_is_skipped() {
    // On a half-duplex bus the UART can hand back what we just sent. It parses cleanly and is
    // addressed to the inverter, not to us -- the driver must step over it, not fail.
    Rig r;
    r.begin();
    r.device.echoRequests = true;

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, r.poll(state));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, fx::expected::kAcPowerW,
                              state.measurements.find(measurement_id::kAcPowerTotal)->value);
}

static void test_leading_line_noise_is_resynchronised_past() {
    Rig r;
    r.begin();
    r.device.prependNoise = true;

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, r.poll(state));
}

static void test_reply_split_across_reads_is_reassembled() {
    Rig r;
    r.begin();
    r.transport.chunkSize = 3;  // dribble it out, as a 9600 baud line does

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, r.poll(state));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, fx::expected::kAcPowerW,
                              state.measurements.find(measurement_id::kAcPowerTotal)->value);
}

static void test_bus_is_locked_and_released() {
    Rig r;
    r.begin();
    DeviceState state;
    r.poll(state);

    TEST_ASSERT_TRUE(r.transport.lockCalls > 0);
    TEST_ASSERT_EQUAL_UINT32(r.transport.lockCalls, r.transport.unlockCalls);  // no leaked lock
    TEST_ASSERT_FALSE(r.transport.locked);
}

static void test_poll_fails_cleanly_when_the_bus_is_busy() {
    Rig r;
    r.begin();
    r.transport.lockFails = true;

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::NotRegistered, r.poll(state));
}

static void test_input_is_flushed_before_each_request() {
    // Stops a late reply to the previous request being read as the answer to this one.
    Rig r;
    r.begin();
    const uint32_t before = r.transport.flushCalls;

    DeviceState state;
    r.poll(state);
    TEST_ASSERT_TRUE(r.transport.flushCalls > before);
}

// --- commands ------------------------------------------------------------------------------------

static void test_every_command_is_unsupported() {
    Rig r;
    r.begin();

    for (size_t i = 0; i < kCommandTypeCount; ++i) {
        InverterCommand c;
        c.type         = static_cast<InverterCommandType>(i);
        c.numericValue = 50.0;
        TEST_ASSERT_EQUAL(CommandResult::Unsupported, r.driver.execute(c));
    }
}

static void test_execute_never_writes_to_the_bus() {
    Rig r;
    r.begin();
    const size_t before = r.transport.writes.size();

    InverterCommand c;
    c.type         = InverterCommandType::SetActivePowerLimitPercent;
    c.numericValue = 50.0;
    r.driver.execute(c);

    TEST_ASSERT_EQUAL_size_t(before, r.transport.writes.size());
}

// --- probe ------------------------------------------------------------------------------------

static void test_probe_scores_a_full_handshake_highly() {
    Rig r;
    r.begin();

    const auto res = r.driver.probe();
    TEST_ASSERT_TRUE(res.responded);
    TEST_ASSERT_TRUE(res.checksumValid);
    TEST_ASSERT_EQUAL_INT(100, res.confidenceScore);  // 40 + 25 + 20 + 10 + 5
    TEST_ASSERT_EQUAL_STRING(fx::kExpectedSerial, res.serialNumber.c_str());
    TEST_ASSERT_EQUAL_STRING("Ever-Solar", res.detectedManufacturer.c_str());
    TEST_ASSERT_TRUE(res.evidence.size() >= 4);
}

static void test_probe_on_a_silent_bus_scores_zero() {
    Rig r;
    r.begin();
    r.device.offline = true;

    const auto res = r.driver.probe();
    TEST_ASSERT_FALSE(res.responded);
    TEST_ASSERT_EQUAL_INT(0, res.confidenceScore);
}

static void test_probe_stops_short_when_registration_is_refused() {
    Rig r;
    r.begin();
    r.device.refuseRegistration = true;

    const auto res = r.driver.probe();
    TEST_ASSERT_TRUE(res.responded);               // something is out there
    TEST_ASSERT_EQUAL_INT(65, res.confidenceScore);  // 40 + 25, no ack
    TEST_ASSERT_TRUE(res.confidenceScore < 80);      // below the auto-select threshold
}

static void test_probe_is_repeatable() {
    // Discovery probes twice and compares; a driver whose probe is not idempotent would make
    // that check meaningless.
    Rig r;
    r.begin();

    const auto a = r.driver.probe();
    const auto b = r.driver.probe();
    TEST_ASSERT_EQUAL_INT(a.confidenceScore, b.confidenceScore);
    TEST_ASSERT_EQUAL_STRING(a.serialNumber.c_str(), b.serialNumber.c_str());
}

// --- full cycle through DeviceContext -------------------------------------------------------------

static void test_night_cycle_never_publishes_a_fake_zero() {
    Rig r;
    r.begin();

    StateStore    store;
    Diagnostics   diag;
    DeviceContext ctx(r.driver, store, diag, clockFn);

    TEST_ASSERT_EQUAL(PollResult::Ok, ctx.pollOnce());
    auto snap = store.snapshot();
    TEST_ASSERT_TRUE(snap->inverterOnline);
    TEST_ASSERT_TRUE(snap->dataValid);

    // Sunset: the inverter stops answering entirely.
    r.device.offline = true;
    for (int i = 0; i < 10; ++i) {
        g_now += 10000;
        ctx.pollOnce();
    }

    snap = store.snapshot();
    TEST_ASSERT_FALSE(snap->inverterOnline);
    TEST_ASSERT_FALSE(snap->dataValid);
    TEST_ASSERT_TRUE(snap->dataStale);
    TEST_ASSERT_TRUE(snap->bridgeOnline);  // the bridge itself is fine and still serving

    // The last real reading is retained and flagged, not overwritten with zero.
    const auto* p = snap->measurements.find(measurement_id::kAcPowerTotal);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, fx::expected::kAcPowerW, p->value);
    TEST_ASSERT_TRUE(p->stale);
}

static void test_sunrise_recovers_without_intervention() {
    Rig r;
    r.begin();

    StateStore    store;
    Diagnostics   diag;
    DeviceContext ctx(r.driver, store, diag, clockFn);
    ctx.pollOnce();

    r.device.offline = true;
    for (int i = 0; i < 15; ++i) {
        g_now += 60000;
        ctx.pollOnce();
    }
    TEST_ASSERT_FALSE(store.snapshot()->inverterOnline);

    // Dawn: the inverter powers up and registers again from scratch.
    r.device.offline = false;
    r.device.payload = Payload::Night;  // awake, but producing nothing yet
    g_now += 60000;
    TEST_ASSERT_EQUAL(PollResult::Ok, ctx.pollOnce());

    const auto snap = store.snapshot();
    TEST_ASSERT_TRUE(snap->inverterOnline);
    TEST_ASSERT_TRUE(snap->dataValid);
    TEST_ASSERT_FALSE(snap->dataStale);
    TEST_ASSERT_EQUAL_UINT32(0, snap->consecutiveFailures);

    // 0 W at dawn is a real measurement and must read as exactly zero, not as "unknown".
    const auto* p = snap->measurements.find(measurement_id::kAcPowerTotal);
    TEST_ASSERT_TRUE(p->valid);
    TEST_ASSERT_FALSE(p->stale);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, p->value);
    // Lifetime energy keeps its value across the outage.
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, fx::expected::kEnergyTotalKwh,
                              snap->measurements.find(measurement_id::kEnergyTotal)->value);
}

static void test_a_single_dropped_frame_does_not_disturb_anything() {
    Rig r;
    r.begin();

    StateStore    store;
    Diagnostics   diag;
    DeviceContext ctx(r.driver, store, diag, clockFn);
    ctx.pollOnce();

    r.device.payload = Payload::BadChecksum;
    g_now += 10000;
    ctx.pollOnce();

    const auto snap = store.snapshot();
    TEST_ASSERT_TRUE(snap->inverterOnline);
    TEST_ASSERT_TRUE(snap->dataValid);
    TEST_ASSERT_FALSE(snap->dataStale);  // one bad frame is not a reason to doubt the data
    TEST_ASSERT_EQUAL_UINT32(1, snap->consecutiveFailures);
}

static void test_backoff_is_bounded() {
    Rig r;
    r.begin();
    r.device.offline = true;

    StateStore    store;
    Diagnostics   diag;
    PollPolicy    policy;
    DeviceContext ctx(r.driver, store, diag, clockFn, policy);

    TEST_ASSERT_EQUAL_UINT32(policy.intervalMs, ctx.nextDelayMs());
    for (int i = 0; i < 30; ++i) {
        g_now += 60000;
        ctx.pollOnce();
    }
    // Bounded: an inverter that is merely asleep must be picked up promptly at sunrise.
    TEST_ASSERT_EQUAL_UINT32(policy.maxBackoffMs, ctx.nextDelayMs());
}

static void test_diagnostics_track_the_cycle_without_leaking_payload() {
    Rig r;
    r.begin();

    StateStore    store;
    Diagnostics   diag;
    DeviceContext ctx(r.driver, store, diag, clockFn);
    ctx.pollOnce();

    r.device.offline = true;
    g_now += 10000;
    ctx.pollOnce();

    const auto s = diag.snapshot();
    TEST_ASSERT_EQUAL_UINT32(1, s.pollSuccessTotal);
    TEST_ASSERT_EQUAL_UINT32(1, s.pollFailureTotal);
    TEST_ASSERT_EQUAL_UINT32(1, s.consecutivePollFailures);
    // Already registered when it went quiet, so the failure is a timeout on the measurement
    // request rather than a failure to register.
    TEST_ASSERT_EQUAL_UINT32(1, s.rs485TimeoutTotal);
    // The error string is published over MQTT and REST, so it carries the outcome only --
    // never payload bytes, never anything from the configuration.
    TEST_ASSERT_EQUAL_STRING("poll failed: timeout", s.lastError.c_str());
}

static void test_snapshots_are_immutable_and_independent() {
    Rig r;
    r.begin();

    StateStore    store;
    Diagnostics   diag;
    DeviceContext ctx(r.driver, store, diag, clockFn);
    ctx.pollOnce();

    // A reader holding an old snapshot must keep seeing what it saw, however much the poll
    // loop moves on. This is what lets a slow REST client not block RS485.
    const auto held = store.snapshot();
    const double before = held->measurements.find(measurement_id::kAcPowerTotal)->value;

    r.device.payload = Payload::Night;
    g_now += 10000;
    ctx.pollOnce();

    TEST_ASSERT_DOUBLE_WITHIN(1e-6, before,
                              held->measurements.find(measurement_id::kAcPowerTotal)->value);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0,
                              store.snapshot()->measurements.find(measurement_id::kAcPowerTotal)->value);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_begin_configures_the_only_known_profile);
    RUN_TEST(test_begin_broadcasts_re_register);
    RUN_TEST(test_begin_fails_when_the_line_cannot_be_configured);
    RUN_TEST(test_begin_declares_no_capabilities_it_cannot_deliver);
    RUN_TEST(test_poll_registers_before_reading);
    RUN_TEST(test_nothing_written_to_the_bus_is_a_write_command);
    RUN_TEST(test_poll_reports_not_registered_when_nobody_answers);
    RUN_TEST(test_registration_refused_without_ack);
    RUN_TEST(test_registration_succeeds_against_the_captured_hardware_frame);
    RUN_TEST(test_one_missed_reply_does_not_drop_the_registration);
    RUN_TEST(test_registration_recovers_a_registered_but_forgotten_inverter);
    RUN_TEST(test_poll_fills_the_canonical_model);
    RUN_TEST(test_dc_power_is_marked_derived);
    RUN_TEST(test_single_string_declares_no_second_mppt);
    RUN_TEST(test_dual_string_declares_the_second_mppt);
    RUN_TEST(test_status_text_does_not_invent_a_meaning);
    RUN_TEST(test_op_mode_zero_is_standby);
    RUN_TEST(test_unobserved_status_codes_stay_unknown);
    RUN_TEST(test_corrupt_reply_does_not_touch_state);
    RUN_TEST(test_reply_of_wrong_length_is_an_invalid_frame);
    RUN_TEST(test_a_run_of_timeouts_keeps_the_registration_and_never_broadcasts);
    RUN_TEST(test_an_inverter_that_returns_addressless_is_recovered_without_a_broadcast);
    RUN_TEST(test_a_sustained_noise_trickle_hits_the_transaction_deadline);
    RUN_TEST(test_echo_of_our_own_request_is_skipped);
    RUN_TEST(test_leading_line_noise_is_resynchronised_past);
    RUN_TEST(test_reply_split_across_reads_is_reassembled);
    RUN_TEST(test_bus_is_locked_and_released);
    RUN_TEST(test_poll_fails_cleanly_when_the_bus_is_busy);
    RUN_TEST(test_input_is_flushed_before_each_request);
    RUN_TEST(test_every_command_is_unsupported);
    RUN_TEST(test_execute_never_writes_to_the_bus);
    RUN_TEST(test_probe_scores_a_full_handshake_highly);
    RUN_TEST(test_probe_on_a_silent_bus_scores_zero);
    RUN_TEST(test_probe_stops_short_when_registration_is_refused);
    RUN_TEST(test_probe_is_repeatable);
    RUN_TEST(test_night_cycle_never_publishes_a_fake_zero);
    RUN_TEST(test_sunrise_recovers_without_intervention);
    RUN_TEST(test_a_single_dropped_frame_does_not_disturb_anything);
    RUN_TEST(test_backoff_is_bounded);
    RUN_TEST(test_diagnostics_track_the_cycle_without_leaking_payload);
    RUN_TEST(test_snapshots_are_immutable_and_independent);
    return UNITY_END();
}
