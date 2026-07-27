// SPDX-License-Identifier: MIT
//
// The SunSpec driver against a simulated device. The decoding is pinned in
// test_sunspec_parser; what this file exercises is the part that talks to a bus and therefore
// meets devices that misbehave -- a chain with no terminator, one that never ends, a
// non-standard base address, and one carrying no model this driver can read.

#include <unity.h>

#include "drivers/sunspec/sunspec_driver.h"
#include "state/state_store.h"
#include "support/fake_sunspec_device.h"
#include "support/mock_transport.h"

using namespace heliograph;
using heliograph::test::FakeSunspecDevice;
using heliograph::test::MockTransport;


namespace {

/// Wires a fake device to a transport and a driver, so each test says only what it varies.
struct Rig {
    MockTransport         transport;
    FakeSunspecDevice     device;
    sunspec::SunspecOptions options;

    void arm() {
        transport.setResponder([this](const std::vector<uint8_t>& req, std::vector<uint8_t>& rep) {
            return device.respond(req, rep);
        });
    }
    sunspec::SunspecDriver makeDriver() {
        sunspec::SunspecDriver d(options);
        d.begin(transport);
        return d;
    }
};

/// A plain, healthy device: common model, a three-phase inverter, terminator.
void buildTypical(FakeSunspecDevice& dev, double watts = 1500.0) {
    uint16_t at = dev.placeMarker();
    at = dev.addModel(at, sunspec::kModelCommon,
                      FakeSunspecDevice::commonPayload("Acme Solar", "AS-5000", "SN12345"));

    auto block = FakeSunspecDevice::blankInverterPayload();
    block[sunspec::inverter::kW]    = static_cast<uint16_t>(static_cast<int16_t>(watts / 10));
    block[sunspec::inverter::kW_SF] = static_cast<uint16_t>(static_cast<int16_t>(1));  // x10
    block[sunspec::inverter::kPhVphA] = 2301;
    block[sunspec::inverter::kV_SF]   = static_cast<uint16_t>(static_cast<int16_t>(-1));
    block[sunspec::inverter::kHz]     = 5001;
    block[sunspec::inverter::kHz_SF]  = static_cast<uint16_t>(static_cast<int16_t>(-2));
    at = dev.addModel(at, sunspec::kModelInverterThreePhase, FakeSunspecDevice::asPayload(block));

    dev.terminate(at);
}

/// The same healthy device, plus a model 123 it actually implements.
///
/// `sf` is the exponent the device publishes for the limit, which is what decides both the
/// resolution offered and how a percentage is encoded on the wire.
uint16_t buildWithControls(FakeSunspecDevice& dev, int sf = -1, uint16_t limitRaw = 1000,
                           uint16_t enabled = sunspec::controls::kDisabled) {
    uint16_t at = dev.placeMarker();
    at = dev.addModel(at, sunspec::kModelCommon,
                      FakeSunspecDevice::commonPayload("Acme Solar", "AS-5000", "SN12345"));
    at = dev.addModel(at, sunspec::kModelInverterThreePhase,
                      FakeSunspecDevice::asPayload(FakeSunspecDevice::blankInverterPayload()));

    const uint16_t controlsAt = at;
    auto block = FakeSunspecDevice::blankControlsPayload();
    block[sunspec::controls::kWMaxLimPct]    = limitRaw;
    block[sunspec::controls::kWMaxLim_Ena]   = enabled;
    block[sunspec::controls::kConn]          = sunspec::controls::kConnect;
    block[sunspec::controls::kWMaxLimPct_SF] = static_cast<uint16_t>(static_cast<int16_t>(sf));
    at = dev.addModel(at, sunspec::kModelControls, FakeSunspecDevice::asPayload(block));

    dev.terminate(at);
    return controlsAt;  ///< so a test can assert the ADDRESS a control write landed on
}

}  // namespace

static void test_probe_identifies_the_device_from_the_common_model() {
    Rig r;
    buildTypical(r.device);
    r.arm();
    auto       d      = r.makeDriver();
    const auto result = d.probe();

    TEST_ASSERT_TRUE(result.responded);
    TEST_ASSERT_EQUAL_STRING("Acme Solar", result.detectedManufacturer.c_str());
    TEST_ASSERT_EQUAL_STRING("AS-5000", result.detectedModel.c_str());
    TEST_ASSERT_EQUAL_STRING("SN12345", result.serialNumber.c_str());
    // Marker plus a readable inverter model is as unambiguous as this bus gets.
    TEST_ASSERT_TRUE(result.confidenceScore >= 90);
}

static void test_the_whole_chain_is_mapped_not_just_the_usable_model() {
    Rig      r;
    uint16_t at = r.device.placeMarker();
    at = r.device.addModel(at, sunspec::kModelCommon,
                           FakeSunspecDevice::commonPayload("Acme", "X", "1"));
    at = r.device.addModel(at, sunspec::kModelInverterThreePhase,
                           FakeSunspecDevice::asPayload(FakeSunspecDevice::blankInverterPayload()));
    at = r.device.addModel(at, 120, std::vector<uint16_t>(26, 0));   // nameplate
    at = r.device.addModel(at, 802, std::vector<uint16_t>(62, 0));   // battery
    r.device.terminate(at);
    r.arm();

    auto d = r.makeDriver();
    d.probe();

    // Everything the device offered, in order -- including the two models this driver cannot
    // read. That inventory is the point: it is what an unsupported-device report is built on.
    TEST_ASSERT_EQUAL_UINT32(4, d.chain().size());
    TEST_ASSERT_EQUAL_UINT16(sunspec::kModelCommon, d.chain()[0].modelId);
    TEST_ASSERT_EQUAL_UINT16(sunspec::kModelInverterThreePhase, d.chain()[1].modelId);
    TEST_ASSERT_EQUAL_UINT16(120, d.chain()[2].modelId);
    TEST_ASSERT_EQUAL_UINT16(802, d.chain()[3].modelId);
}

static void test_poll_publishes_scaled_readings() {
    Rig r;
    buildTypical(r.device, 1500.0);
    r.arm();
    auto d = r.makeDriver();

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(state));

    const auto* p = state.measurements.find(measurement_id::kAcPowerTotal);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(p->valid);
    TEST_ASSERT_DOUBLE_WITHIN(1.0, 1500.0, p->value);

    const auto* v = state.measurements.find(measurement_id::kAcL1Voltage);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 230.1, v->value);

    const auto* hz = state.measurements.find(measurement_id::kAcFrequency);
    TEST_ASSERT_NOT_NULL(hz);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 50.01, hz->value);
}

// Points the device does not implement must not turn up as zero readings.
static void test_unimplemented_points_are_not_published() {
    Rig r;
    buildTypical(r.device);
    r.arm();
    auto        d = r.makeDriver();
    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(state));

    // The fixture never filled temperature or lifetime energy.
    const auto* t = state.measurements.find(measurement_id::kTemperature);
    TEST_ASSERT_TRUE(t == nullptr || !t->valid);
    const auto* e = state.measurements.find(measurement_id::kEnergyTotal);
    TEST_ASSERT_TRUE(e == nullptr || !e->valid);
}

static void test_no_marker_means_no_device() {
    Rig r;
    // A bus that answers, but with something that is not SunSpec.
    r.device.registers[40000] = 0x1234;
    r.device.registers[40001] = 0x5678;
    r.arm();
    auto       d = r.makeDriver();
    const auto result = d.probe();

    TEST_ASSERT_FALSE(result.responded);
    TEST_ASSERT_EQUAL_UINT32(0, d.chain().size());
}

// Several real devices simply stop answering instead of serving 0xFFFF. That is not a fault,
// and whatever was mapped before the silence must survive.
static void test_a_chain_without_a_terminator_keeps_what_was_mapped() {
    Rig r;
    r.device.serveTerminator = false;
    buildTypical(r.device);
    r.arm();

    auto       d      = r.makeDriver();
    const auto result = d.probe();

    TEST_ASSERT_TRUE(result.responded);
    TEST_ASSERT_EQUAL_UINT32(2, d.chain().size());
    TEST_ASSERT_TRUE(result.confidenceScore >= 90);  // the inverter model was still found

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(state));
}

// A device whose chain never ends must be bounded, not followed forever.
static void test_an_endless_chain_is_bounded() {
    Rig      r;
    uint16_t at = r.device.placeMarker();
    for (size_t i = 0; i < sunspec::kMaxChainEntries + 10; ++i) {
        at = r.device.addModel(at, 120, std::vector<uint16_t>(4, 0));
    }
    // No terminator on purpose: the ceiling is the only thing that stops this.
    r.arm();

    auto d = r.makeDriver();
    d.probe();
    TEST_ASSERT_EQUAL_UINT32(sunspec::kMaxChainEntries, d.chain().size());
}

// The base address is an option because vendors differ; 50000 is the other common choice.
static void test_a_non_default_base_address_is_honoured() {
    Rig r;
    r.device.baseAddress = 50000;
    r.options.baseAddress = 50000;
    buildTypical(r.device);
    r.arm();

    auto d = r.makeDriver();
    TEST_ASSERT_TRUE(d.probe().responded);

    // And the same device is invisible when the driver looks in the usual place.
    Rig wrong;
    wrong.device.baseAddress = 50000;
    buildTypical(wrong.device);
    wrong.arm();  // options.baseAddress stays at the 40000 default
    auto blind = wrong.makeDriver();
    TEST_ASSERT_FALSE(blind.probe().responded);
}

// Marker present, but nothing this driver can read. Worth reporting, not worth claiming.
static void test_a_chain_without_an_inverter_model_is_reported_honestly() {
    Rig      r;
    uint16_t at = r.device.placeMarker();
    at = r.device.addModel(at, sunspec::kModelCommon,
                           FakeSunspecDevice::commonPayload("Acme", "Battery", "9"));
    at = r.device.addModel(at, 802, std::vector<uint16_t>(62, 0));
    r.device.terminate(at);
    r.arm();

    auto       d      = r.makeDriver();
    const auto result = d.probe();

    TEST_ASSERT_TRUE(result.responded);           // it IS a SunSpec device
    TEST_ASSERT_TRUE(result.confidenceScore < 90);  // just not one we can read
    TEST_ASSERT_EQUAL_STRING("Acme", result.detectedManufacturer.c_str());

    DeviceState state;
    TEST_ASSERT_NOT_EQUAL(PollResult::Ok, d.poll(state));
}

// begin() must configure the line. Without it, a bridge that boots straight into this driver
// -- every reboot once it is the selected driver -- polls an unconfigured UART and hears
// nothing forever, while a discovery run masks the bug by configuring the transport itself.
// The sibling Modbus driver carries a comment about exactly this; the trap is easy to re-enter.
static void test_begin_configures_the_serial_line() {
    Rig r;
    buildTypical(r.device);
    r.arm();
    auto d = r.makeDriver();  // calls begin()

    TEST_ASSERT_TRUE(r.transport.configureCalls > 0);
}

// A driver chosen by hand is never probed, so identity has to come from the chain walk. Without
// it the device stays nameless in the UI and in Home Assistant for the whole session, even
// though the information was one read away.
static void test_identity_is_available_without_probing() {
    Rig r;
    buildTypical(r.device);
    r.arm();
    auto d = r.makeDriver();

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(state));  // poll only, never probe()

    TEST_ASSERT_EQUAL_STRING("Acme Solar", d.identity().manufacturer.c_str());
    TEST_ASSERT_EQUAL_STRING("SN12345", d.identity().serialNumber.c_str());
}

// A device with no readable model must not be reported as a CORRUPTED one. InvalidFrame feeds
// the counter that means "bytes arrived damaged", which drives the alert telling someone to go
// check their ground and termination -- and nothing is wrong with this bus.
static void test_an_unreadable_device_is_not_reported_as_line_corruption() {
    Rig      r;
    uint16_t at = r.device.placeMarker();
    at = r.device.addModel(at, sunspec::kModelCommon,
                           FakeSunspecDevice::commonPayload("Acme", "Battery", "9"));
    at = r.device.addModel(at, 802, std::vector<uint16_t>(62, 0));
    r.device.terminate(at);
    r.arm();

    auto        d = r.makeDriver();
    DeviceState state;
    const auto  result = d.poll(state);

    TEST_ASSERT_NOT_EQUAL(PollResult::Ok, result);
    TEST_ASSERT_NOT_EQUAL(PollResult::InvalidFrame, result);
    TEST_ASSERT_NOT_EQUAL(PollResult::ChecksumError, result);
}

// A perfectly healthy Modbus device that simply is not SunSpec used to be reported as Timeout,
// which accused the wiring of a fault that does not exist. It answers promptly; the marker is
// just not there.
static void test_a_device_without_the_marker_is_not_reported_as_a_timeout() {
    Rig r;
    r.device.registers[r.device.baseAddress]     = 0x1234;  // something, but not "SunS"
    r.device.registers[r.device.baseAddress + 1] = 0x5678;
    r.arm();

    auto        d = r.makeDriver();
    DeviceState state;
    const auto  result = d.poll(state);

    TEST_ASSERT_EQUAL(PollResult::NotRegistered, result);
}

// The one symptom that indicts the cable. Everything used to collapse into Timeout, so a bus
// with a bad ground was invisible in the counter the alerting rules key on -- and pointed the
// field diagnosis at "the inverter is asleep" instead (review, 2026-07-25).
static void test_a_corrupt_reply_is_reported_as_a_checksum_error() {
    Rig r;
    buildTypical(r.device);
    r.device.corruptCrc = true;
    r.arm();

    auto        d = r.makeDriver();
    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::ChecksumError, d.poll(state));
}

// The same symptom, but appearing only once the chain is already mapped -- so the failure has
// to travel back out of readModel rather than out of the opening marker read. Corrupting
// everything from the first byte, as the test above does, never exercises that path.
static void test_corruption_that_starts_mid_chain_is_still_a_checksum_error() {
    Rig r;
    buildTypical(r.device);
    r.device.corruptCrc    = true;
    r.device.intactReplies = 5;  // marker, chain headers and the identity read survive
    r.arm();

    auto        d = r.makeDriver();
    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::ChecksumError, d.poll(state));
    // Self-evidencing: a fully mapped chain proves the walk completed, so the failure can only
    // have come out of readModel. Without this the test would silently pass while exercising
    // the marker read, which is what the sibling test above already covers.
    TEST_ASSERT_EQUAL_UINT32(2, d.chain().size());
}

// The marker is there, so something SunSpec-shaped is on the bus, but the very first model
// header is refused. Nothing gets mapped. The reason the walk stopped used to be discarded
// here, so the caller's default won and this reported Timeout -- accusing the wiring of a
// fault on a device that is demonstrably answering (review, 2026-07-25).
static void test_a_marker_with_no_readable_models_is_not_reported_as_a_timeout() {
    Rig r;
    r.device.placeMarker();  // marker only; every later register earns an exception
    r.arm();

    auto        d = r.makeDriver();
    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::NotRegistered, d.poll(state));
}

// A device advertising an inverter model shorter than this driver can decode. Every read
// succeeds with a valid CRC, so the tally inside read() sees nothing wrong -- the failure is in
// the decode, and it has to be counted there. It was not, and because walked_ is not reset on
// this path the driver loops on the same entry forever: every poll fails, and all three RS485
// counters read zero for the entire uptime. That is the reading an operator is most likely to
// act on wrongly, since zero bus errors says "the cable is fine" about a bridge that publishes
// nothing at all.
static void test_an_undecodable_model_counts_an_invalid_frame() {
    Rig      r;
    uint16_t at = r.device.placeMarker();
    at = r.device.addModel(at, sunspec::kModelCommon,
                           FakeSunspecDevice::commonPayload("Acme Solar", "AS-5000", "SN12345"));
    // Declares itself an inverter, then carries far too few registers to be one.
    at = r.device.addModel(at, sunspec::kModelInverterThreePhase, std::vector<uint16_t>(10, 0));
    r.device.terminate(at);
    r.arm();
    auto d = r.makeDriver();

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::InvalidFrame, d.poll(state));
    TEST_ASSERT_EQUAL_UINT32(1, d.busErrors().invalidFrames);
    // Not a wire fault: nothing arrived corrupted and nothing went unanswered.
    TEST_ASSERT_EQUAL_UINT32(0, d.busErrors().checksumErrors);
    TEST_ASSERT_EQUAL_UINT32(0, d.busErrors().timeouts);

    // ...and it keeps counting, because the driver keeps retrying the same chain entry.
    TEST_ASSERT_EQUAL(PollResult::InvalidFrame, d.poll(state));
    TEST_ASSERT_EQUAL_UINT32(2, d.busErrors().invalidFrames);
}

static void test_a_silent_device_does_not_poll() {
    Rig r;
    buildTypical(r.device);
    r.arm();
    auto d = r.makeDriver();

    r.device.asleep = true;
    DeviceState state;
    TEST_ASSERT_NOT_EQUAL(PollResult::Ok, d.poll(state));
}

/// A device that publishes no model 123 must not merely refuse a write -- the capability has to
/// be ABSENT, so nothing upstream ever offers a control that cannot exist.
static void test_a_device_without_model_123_offers_no_controls() {
    Rig r;
    buildTypical(r.device);
    r.arm();
    auto d = r.makeDriver();

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(state));

    TEST_ASSERT_TRUE(d.capabilities().isReadOnly());
    TEST_ASSERT_FALSE(d.capabilities().canWrite(InverterCapability::SetActivePowerLimit));
    InverterCommand cmd;
    cmd.type         = InverterCommandType::SetActivePowerLimitPercent;
    cmd.numericValue = 50.0;
    TEST_ASSERT_EQUAL(CommandResult::Unsupported, d.execute(cmd));
    TEST_ASSERT_EQUAL_UINT32(0, r.device.writes);
}

/// The capability is not a property of the driver but of the device in front of it, and its
/// bounds come from the scale factor that device published -- not from a constant here.
static void test_model_123_grants_a_power_limit_bounded_by_the_devices_scale_factor() {
    Rig r;
    buildWithControls(r.device, /*sf=*/-1);
    r.arm();
    auto d = r.makeDriver();

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(state));

    const auto caps = d.capabilities();
    TEST_ASSERT_FALSE(caps.isReadOnly());
    TEST_ASSERT_TRUE(caps.canWrite(InverterCapability::SetActivePowerLimit));

    const auto& n =
        caps.numeric[static_cast<size_t>(InverterCommandType::SetActivePowerLimitPercent)];
    TEST_ASSERT_TRUE(n.supported);
    TEST_ASSERT_TRUE(n.writable);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, n.minimum);
    TEST_ASSERT_EQUAL_DOUBLE(100.0, n.maximum);
    // sf = -1 buys one decimal. A control surface told otherwise would accept 12.34% and the
    // device would round it, silently.
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.1, n.step);
    TEST_ASSERT_EQUAL(Unit::Percent, n.unit);
}

/// Before the block has been read there is no scale factor, and therefore no honest bounds to
/// advertise. A capability offered at that point is a control with invented limits.
static void test_the_capability_is_absent_until_the_controls_block_has_been_read() {
    Rig r;
    buildWithControls(r.device);
    r.arm();
    auto d = r.makeDriver();

    TEST_ASSERT_TRUE(d.capabilities().isReadOnly());
    InverterCommand cmd;
    cmd.type         = InverterCommandType::SetActivePowerLimitPercent;
    cmd.numericValue = 50.0;
    TEST_ASSERT_EQUAL(CommandResult::Unsupported, d.execute(cmd));
}

/// The whole write path, on the wire: the right registers, the right values, in the right order.
static void test_setting_a_limit_writes_the_value_before_it_arms_the_limit() {
    Rig            r;
    const uint16_t controlsAt = buildWithControls(r.device, /*sf=*/-1);
    r.arm();
    auto d = r.makeDriver();

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(state));

    InverterCommand cmd;
    cmd.type         = InverterCommandType::SetActivePowerLimitPercent;
    cmd.numericValue = 62.5;
    TEST_ASSERT_EQUAL(CommandResult::Ok, d.execute(cmd));

    // Two writes, not one span: the registers between the value and the enable are ramp and
    // revert times nobody asked to change.
    TEST_ASSERT_EQUAL_UINT32(2, r.device.writes);
    // 62.5% at sf = -1 is 625 raw.
    TEST_ASSERT_EQUAL_UINT16(
        625, r.device.registers[static_cast<uint16_t>(controlsAt + sunspec::controls::kWMaxLimPct)]);
    // And the enable is the LAST thing written -- arming first would apply whatever limit the
    // register happened to hold until the second write landed.
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(controlsAt + sunspec::controls::kWMaxLim_Ena),
                             r.device.lastWriteAddress);
    TEST_ASSERT_EQUAL_UINT16(sunspec::controls::kEnabled, r.device.lastWriteValue);
}

/// Out of range is refused, not clamped, and nothing goes onto the bus. A caller that asked for
/// 150% has made a mistake; running at 100% instead hides it behind a successful command.
static void test_a_limit_outside_the_devices_range_never_reaches_the_bus() {
    Rig r;
    buildWithControls(r.device);
    r.arm();
    auto d = r.makeDriver();

    DeviceState state;
    d.poll(state);
    const uint32_t before = r.device.writes;

    InverterCommand cmd;
    cmd.type         = InverterCommandType::SetActivePowerLimitPercent;
    cmd.numericValue = 150.0;
    TEST_ASSERT_EQUAL(CommandResult::OutOfRange, d.execute(cmd));
    cmd.numericValue = -1.0;
    TEST_ASSERT_EQUAL(CommandResult::OutOfRange, d.execute(cmd));
    TEST_ASSERT_EQUAL_UINT32(before, r.device.writes);
}

/// A device that answers with an exception has refused. Distinct from a bus failure: the write
/// arrived, so the same value will be refused again.
static void test_a_refused_write_is_reported_as_a_refusal() {
    Rig r;
    buildWithControls(r.device);
    r.arm();
    auto d = r.makeDriver();

    DeviceState state;
    d.poll(state);
    r.device.refuseWrites = true;

    InverterCommand cmd;
    cmd.type         = InverterCommandType::SetActivePowerLimitPercent;
    cmd.numericValue = 40.0;
    TEST_ASSERT_EQUAL(CommandResult::Rejected, d.execute(cmd));
}

/// The echo IS the confirmation. A device that answers with a well-formed frame carrying a
/// different value has not done what it was asked, and reporting Ok would leave everybody
/// believing in a limit that was never set.
static void test_a_write_whose_echo_disagrees_is_not_success() {
    Rig r;
    buildWithControls(r.device);
    r.arm();
    auto d = r.makeDriver();

    DeviceState state;
    d.poll(state);
    r.device.echoWrongValue = true;

    InverterCommand cmd;
    cmd.type         = InverterCommandType::SetActivePowerLimitPercent;
    cmd.numericValue = 40.0;
    TEST_ASSERT_EQUAL(CommandResult::DriverError, d.execute(cmd));
}

/// What is published is what the DEVICE holds, not what was last written -- the two differ
/// whenever someone else changed it or the inverter's own revert timer fired.
static void test_the_published_limit_comes_from_the_device_not_from_the_last_write() {
    Rig r;
    // 45.0% stored, and switched OFF: an inverter holding a limit it is not applying.
    buildWithControls(r.device, /*sf=*/-1, /*limitRaw=*/450, sunspec::controls::kDisabled);
    r.arm();
    auto d = r.makeDriver();

    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(state));

    const auto* limit = state.measurements.find(measurement_id::kActivePowerLimitPct);
    TEST_ASSERT_NOT_NULL(limit);
    TEST_ASSERT_TRUE(limit->valid);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 45.0, limit->value);

    // And the flag that stops that 45% being read as "the inverter is limited to 45%".
    const auto* on = state.measurements.find(measurement_id::kActivePowerLimitEnabled);
    TEST_ASSERT_NOT_NULL(on);
    TEST_ASSERT_TRUE(on->valid);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, on->value);
}

/// A device that advertises model 123 on the chain and then will not serve it must cost ONE
/// failed transaction, not one per poll forever.
///
/// The failure this prevents is quiet: the inverter read still succeeds, so every poll reports
/// Ok while a full response timeout is added to each cycle and each failure is tallied into the
/// bus error counters the alerting rules watch. A healthy bus is made to look like a degrading
/// one, by a control surface nobody is using.
static void test_a_controls_block_that_never_answers_is_given_up_on() {
    Rig      r;
    uint16_t at = r.device.placeMarker();
    at = r.device.addModel(at, sunspec::kModelInverterThreePhase,
                           FakeSunspecDevice::asPayload(FakeSunspecDevice::blankInverterPayload()));
    // Advertised on the chain at its full length, but the payload registers are absent, so the
    // device answers the read with "illegal data address" -- exactly what a slave does for a
    // model it lists and does not implement.
    const uint16_t controlsAt = at;
    at = r.device.addModel(at, sunspec::kModelControls,
                           FakeSunspecDevice::asPayload(FakeSunspecDevice::blankControlsPayload()));
    // From the PAYLOAD onwards only. The model id and its length have to stay, or the chain walk
    // stops at this block and never records it -- which would make this a test about a truncated
    // chain, and it would pass whether or not the retry is bounded.
    for (uint16_t i = 2; i < sunspec::controls::kMinRegisters; ++i) {
        r.device.registers.erase(static_cast<uint16_t>(controlsAt + i));
    }
    r.device.terminate(at);
    r.arm();

    auto        d = r.makeDriver();
    DeviceState first;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(first));
    const uint32_t afterFirst = r.device.reads;

    // The ABSOLUTE cost of a steady-state poll, not a comparison between two later polls:
    // consecutive polls cost the same whether the block is retried forever or given up on, so
    // comparing them proves nothing. One transaction is the inverter model. Two would be the
    // inverter model plus another doomed attempt at the controls.
    DeviceState second;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(second));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, r.device.reads - afterFirst,
                                     "the unreadable controls block is still being retried");

    DeviceState third;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(third));
    TEST_ASSERT_EQUAL_UINT32(2, r.device.reads - afterFirst);
    TEST_ASSERT_FALSE(d.capabilities().canWrite(InverterCapability::SetActivePowerLimit));
}

/// A device carrying model 123 with no usable scale factor can be read but not written: the
/// percentage cannot be encoded, so offering the control would be offering a guess.
static void test_a_controls_block_without_a_scale_factor_grants_no_limit() {
    Rig      r;
    uint16_t at = r.device.placeMarker();
    at = r.device.addModel(at, sunspec::kModelInverterThreePhase,
                           FakeSunspecDevice::asPayload(FakeSunspecDevice::blankInverterPayload()));
    // Every point at its sentinel, including WMaxLimPct_SF.
    at = r.device.addModel(at, sunspec::kModelControls,
                           FakeSunspecDevice::asPayload(FakeSunspecDevice::blankControlsPayload()));
    r.device.terminate(at);
    r.arm();

    auto        d = r.makeDriver();
    DeviceState state;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(state));

    TEST_ASSERT_FALSE(d.capabilities().canWrite(InverterCapability::SetActivePowerLimit));
    const auto* limit = state.measurements.find(measurement_id::kActivePowerLimitPct);
    // Declared, because the model is there -- but carrying no reading, which is the honest
    // combination for a point the device does not implement.
    TEST_ASSERT_NOT_NULL(limit);
    TEST_ASSERT_FALSE(limit->valid);
}

void run_sunspec_driver() {
    RUN_TEST(test_probe_identifies_the_device_from_the_common_model);
    RUN_TEST(test_the_whole_chain_is_mapped_not_just_the_usable_model);
    RUN_TEST(test_poll_publishes_scaled_readings);
    RUN_TEST(test_unimplemented_points_are_not_published);
    RUN_TEST(test_no_marker_means_no_device);
    RUN_TEST(test_a_chain_without_a_terminator_keeps_what_was_mapped);
    RUN_TEST(test_an_endless_chain_is_bounded);
    RUN_TEST(test_a_non_default_base_address_is_honoured);
    RUN_TEST(test_a_chain_without_an_inverter_model_is_reported_honestly);
    RUN_TEST(test_begin_configures_the_serial_line);
    RUN_TEST(test_identity_is_available_without_probing);
    RUN_TEST(test_an_unreadable_device_is_not_reported_as_line_corruption);
    RUN_TEST(test_a_device_without_the_marker_is_not_reported_as_a_timeout);
    RUN_TEST(test_a_corrupt_reply_is_reported_as_a_checksum_error);
    RUN_TEST(test_corruption_that_starts_mid_chain_is_still_a_checksum_error);
    RUN_TEST(test_a_marker_with_no_readable_models_is_not_reported_as_a_timeout);
    RUN_TEST(test_an_undecodable_model_counts_an_invalid_frame);
    RUN_TEST(test_a_silent_device_does_not_poll);
    RUN_TEST(test_a_device_without_model_123_offers_no_controls);
    RUN_TEST(test_model_123_grants_a_power_limit_bounded_by_the_devices_scale_factor);
    RUN_TEST(test_the_capability_is_absent_until_the_controls_block_has_been_read);
    RUN_TEST(test_setting_a_limit_writes_the_value_before_it_arms_the_limit);
    RUN_TEST(test_a_limit_outside_the_devices_range_never_reaches_the_bus);
    RUN_TEST(test_a_refused_write_is_reported_as_a_refusal);
    RUN_TEST(test_a_write_whose_echo_disagrees_is_not_success);
    RUN_TEST(test_the_published_limit_comes_from_the_device_not_from_the_last_write);
    RUN_TEST(test_a_controls_block_that_never_answers_is_given_up_on);
    RUN_TEST(test_a_controls_block_without_a_scale_factor_grants_no_limit);
}
