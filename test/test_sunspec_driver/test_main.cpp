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

void setUp() {}
void tearDown() {}

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

static void test_a_silent_device_does_not_poll() {
    Rig r;
    buildTypical(r.device);
    r.arm();
    auto d = r.makeDriver();

    r.device.asleep = true;
    DeviceState state;
    TEST_ASSERT_NOT_EQUAL(PollResult::Ok, d.poll(state));
}

static void test_the_driver_is_read_only() {
    Rig r;
    buildTypical(r.device);
    r.arm();
    auto d = r.makeDriver();

    TEST_ASSERT_TRUE(d.capabilities().isReadOnly());
    InverterCommand cmd;
    TEST_ASSERT_EQUAL(CommandResult::Unsupported, d.execute(cmd));
}

int main(int, char**) {
    UNITY_BEGIN();
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
    RUN_TEST(test_a_silent_device_does_not_poll);
    RUN_TEST(test_the_driver_is_read_only);
    return UNITY_END();
}
