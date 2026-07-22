// SPDX-License-Identifier: MIT
// Register conversion, NaN sentinels, and the promise that outputs are driver-agnostic.

#include <unity.h>

#include <cmath>
#include <cstring>

#include "device/device_context.h"
#include "drivers/eversolar_legacy/eversolar_driver.h"
#include "drivers/mock/mock_driver.h"
#include "outputs/modbus_tcp/register_map.h"
#include "state/state_store.h"
#include "support/fake_eversolar_device.h"
#include "support/mock_transport.h"

using namespace heliograph;
using namespace heliograph::modbus;
namespace fx = heliograph::fixtures;
using test::FakeEversolarDevice;
using test::MockTransport;

static uint64_t g_now = 0;
static uint64_t clockFn() { return g_now; }

void setUp() { g_now = 100000; }
void tearDown() {}

/// Decodes a float32 the way a Modbus client does: high word first.
static float decodeFloat(const RegisterMap& m, uint16_t addr) {
    const uint32_t bits =
        (static_cast<uint32_t>(m.at(addr)) << 16) | static_cast<uint32_t>(m.at(addr + 1));
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

static uint32_t decodeU32(const RegisterMap& m, uint16_t addr) {
    return (static_cast<uint32_t>(m.at(addr)) << 16) | static_cast<uint32_t>(m.at(addr + 1));
}

static std::string decodeString(const RegisterMap& m, uint16_t addr, size_t regs) {
    std::string s;
    for (size_t i = 0; i < regs; ++i) {
        const uint16_t w = m.at(static_cast<uint16_t>(addr + i));
        const char     a = static_cast<char>(w >> 8);
        const char     b = static_cast<char>(w & 0xFF);
        if (a == 0) return s;
        s.push_back(a);
        if (b == 0) return s;
        s.push_back(b);
    }
    return s;
}

/// Runs the real EverSolar driver against the simulated inverter and renders the result.
struct EversolarRig {
    MockTransport              transport;
    FakeEversolarDevice        device;
    eversolar::EversolarDriver driver{transport};
    StateStore                 store;
    Diagnostics                diagnostics;
    RegisterMap                map;
    BridgeInfo                 bridge;

    EversolarRig() {
        device.installOn(transport);
        driver.begin(transport);
        bridge.bridgeOnline  = true;
        bridge.wifiConnected = true;
        bridge.wifiRssiDbm   = -57;
        bridge.uptimeSeconds = 86400;
    }

    void pollAndRender() {
        DeviceContext ctx(driver, store, diagnostics, clockFn);
        ctx.pollOnce();
        map.update(*store.snapshot(), bridge, diagnostics.snapshot(), g_now);
    }
};

// --- schema and framing --------------------------------------------------------------------

static void test_schema_version_is_published() {
    RegisterMap m;
    TEST_ASSERT_EQUAL_UINT32(kSchemaVersion, decodeU32(m, reg::kSchemaVersionAddr));
}

static void test_an_unpopulated_map_reads_as_unknown_not_zero() {
    // A client polling before the first successful read must not see a device idling at 0 W.
    RegisterMap m;
    TEST_ASSERT_TRUE(std::isnan(decodeFloat(m, reg::kAcPowerTotal)));
    TEST_ASSERT_TRUE(std::isnan(decodeFloat(m, reg::kEnergyTotal)));
}

static void test_reads_beyond_the_map_are_refused() {
    // The server turns this into exception 0x02 rather than serving adjacent memory.
    RegisterMap m;
    uint16_t    buf[4] = {};
    TEST_ASSERT_FALSE(m.read(static_cast<uint16_t>(RegisterMap::size() - 1), 4, buf));
    TEST_ASSERT_FALSE(m.read(9999, 1, buf));
    TEST_ASSERT_FALSE(m.read(0, 0, buf));
    TEST_ASSERT_TRUE(m.read(static_cast<uint16_t>(RegisterMap::size() - 4), 4, buf));
}

static void test_read_returns_the_requested_window() {
    RegisterMap m;
    uint16_t    buf[2] = {};
    TEST_ASSERT_TRUE(m.read(reg::kSchemaVersionAddr, 2, buf));
    TEST_ASSERT_EQUAL_UINT16(0, buf[0]);
    TEST_ASSERT_EQUAL_UINT16(kSchemaVersion, buf[1]);
}

// --- word order -----------------------------------------------------------------------------

static void test_float32_is_high_word_first() {
    EversolarRig r;
    r.pollAndRender();

    // 1842.0f == 0x44E64000. High word first means 0x44E6 lands at the lower address.
    TEST_ASSERT_EQUAL_HEX16(0x44E6, r.map.at(reg::kAcPowerTotal));
    TEST_ASSERT_EQUAL_HEX16(0x4000, r.map.at(reg::kAcPowerTotal + 1));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1842.0f, decodeFloat(r.map, reg::kAcPowerTotal));
}

static void test_uint32_is_high_word_first() {
    EversolarRig r;
    r.pollAndRender();
    // 31204 == 0x000079E4
    TEST_ASSERT_EQUAL_HEX16(0x0000, r.map.at(reg::kOperatingHours));
    TEST_ASSERT_EQUAL_HEX16(0x79E4, r.map.at(reg::kOperatingHours + 1));
    TEST_ASSERT_EQUAL_UINT32(31204, decodeU32(r.map, reg::kOperatingHours));
}

static void test_negative_int32_round_trips() {
    EversolarRig r;
    r.pollAndRender();
    const int32_t rssi = static_cast<int32_t>(decodeU32(r.map, reg::kWifiRssi));
    TEST_ASSERT_EQUAL_INT32(-57, rssi);
}

static void test_rssi_is_a_sentinel_not_zero_when_wifi_is_down() {
    // 0 dBm reads as a perfect connection; the map must publish the unknown sentinel instead,
    // on both the RSSI register and its diagnostics mirror.
    EversolarRig r;
    r.bridge.wifiConnected = false;
    r.pollAndRender();
    TEST_ASSERT_EQUAL_HEX32(kInvalidU32, decodeU32(r.map, reg::kWifiRssi));
    uint16_t diagRssi = 0;
    TEST_ASSERT_TRUE(r.map.read(reg::kDiagWifiRssi, 1, &diagRssi));
    TEST_ASSERT_EQUAL_HEX16(kInvalidU16, diagRssi);
}

// --- EverSolar: a single-phase, no-battery device --------------------------------------------

static void test_eversolar_measurements_land_in_the_right_registers() {
    EversolarRig r;
    r.pollAndRender();

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1842.0f, decodeFloat(r.map, reg::kAcPowerTotal));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 233.4f, decodeFloat(r.map, reg::kAcL1Voltage));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.9f, decodeFloat(r.map, reg::kAcL1Current));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 49.98f, decodeFloat(r.map, reg::kAcFrequency));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 8.42f, decodeFloat(r.map, reg::kEnergyToday));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 18452.7f, decodeFloat(r.map, reg::kEnergyTotal));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 41.3f, decodeFloat(r.map, reg::kTemperature));
}

static void test_absent_phases_are_nan_not_zero() {
    // The TL3000-20 is single-phase. L2/L3 must read as unknown, not as 0 V.
    EversolarRig r;
    r.pollAndRender();

    const uint16_t l2 = reg::kPhaseBase + reg::kPhaseStride;
    const uint16_t l3 = reg::kPhaseBase + 2 * reg::kPhaseStride;
    TEST_ASSERT_TRUE(std::isnan(decodeFloat(r.map, l2 + reg::kPhaseVoltageOffset)));
    TEST_ASSERT_TRUE(std::isnan(decodeFloat(r.map, l3 + reg::kPhaseVoltageOffset)));
    TEST_ASSERT_FALSE(r.map.validityBit(ValidityBit::AcL2Voltage));
    TEST_ASSERT_FALSE(r.map.validityBit(ValidityBit::AcL3Voltage));
}

static void test_absent_battery_is_nan_not_zero() {
    // 0% SoC would read as a flat battery. There is no battery.
    EversolarRig r;
    r.pollAndRender();

    TEST_ASSERT_TRUE(std::isnan(decodeFloat(r.map, reg::kBatterySoc)));
    TEST_ASSERT_FALSE(r.map.validityBit(ValidityBit::BatterySoc));
    TEST_ASSERT_EQUAL_UINT16(0, r.map.at(reg::kBatteryPresent));
}

static void test_an_unsupported_declared_channel_is_nan() {
    // A channel present in the schema but unreadable must render exactly like one that was
    // never declared: NaN plus a cleared validity bit. Otherwise `supported` would be a flag
    // this file checks and nothing can ever set.
    EversolarRig r;
    DeviceContext ctx(r.driver, r.store, r.diagnostics, clockFn);
    ctx.pollOnce();

    DeviceState state = *r.store.snapshot();
    state.measurements.declareUnsupported("battery.soc", MeasurementType::Ratio, Unit::Percent,
                                          "Battery SoC");
    r.map.update(state, r.bridge, r.diagnostics.snapshot(), g_now);

    TEST_ASSERT_TRUE(std::isnan(decodeFloat(r.map, reg::kBatterySoc)));
    TEST_ASSERT_FALSE(r.map.validityBit(ValidityBit::BatterySoc));
}

static void test_unsupported_error_code_is_not_published_as_zero() {
    // The protocol has no error code field; 0 would assert "no fault".
    EversolarRig r;
    r.pollAndRender();

    TEST_ASSERT_EQUAL_HEX16(kInvalidU16, r.map.at(reg::kErrorCode));
    TEST_ASSERT_EQUAL_HEX16(kInvalidU16, r.map.at(reg::kErrorCodeMirror));
    TEST_ASSERT_FALSE(r.map.validityBit(ValidityBit::ErrorCode));
}

static void test_a_32bit_error_code_saturates_the_16bit_register() {
    // A 32-bit fault bitmask cannot fit this register. It must saturate to 0xFFFE ("fault
    // present, consult MQTT/REST") -- truncating would silently rename the fault, and
    // 0xFFFF must stay reserved for "not usable".
    EversolarRig r;
    DeviceContext ctx(r.driver, r.store, r.diagnostics, clockFn);
    ctx.pollOnce();

    DeviceState state         = *r.store.snapshot();
    state.errorCodeSupported  = true;
    state.errorCode           = 0x20000000u;  // bit 29: fits only in 32 bits
    r.map.update(state, r.bridge, r.diagnostics.snapshot(), g_now);
    TEST_ASSERT_EQUAL_HEX16(0xFFFE, r.map.at(reg::kErrorCode));
    TEST_ASSERT_EQUAL_HEX16(0xFFFE, r.map.at(reg::kErrorCodeMirror));
    TEST_ASSERT_TRUE(r.map.validityBit(ValidityBit::ErrorCode));

    // A code that fits passes through unchanged.
    state.errorCode = 0x1234u;
    r.map.update(state, r.bridge, r.diagnostics.snapshot(), g_now);
    TEST_ASSERT_EQUAL_HEX16(0x1234, r.map.at(reg::kErrorCode));
}

static void test_second_mppt_is_absent_for_a_single_string_payload() {
    EversolarRig r;
    r.pollAndRender();

    const uint16_t mppt2 = reg::kMpptBase + reg::kMpptStride;
    TEST_ASSERT_TRUE(std::isnan(decodeFloat(r.map, mppt2 + reg::kMpptVoltageOffset)));
    TEST_ASSERT_EQUAL_UINT16(1, r.map.at(reg::kMpptCount));
}

static void test_second_mppt_appears_for_a_dual_string_payload() {
    EversolarRig r;
    r.device.payload = FakeEversolarDevice::Payload::DualString;
    r.pollAndRender();

    const uint16_t mppt2 = reg::kMpptBase + reg::kMpptStride;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 335.0f, decodeFloat(r.map, mppt2 + reg::kMpptVoltageOffset));
    TEST_ASSERT_EQUAL_UINT16(2, r.map.at(reg::kMpptCount));
    TEST_ASSERT_TRUE(r.map.validityBit(ValidityBit::DcMppt2Voltage));
}

static void test_identity_strings_are_readable() {
    EversolarRig r;
    r.pollAndRender();

    TEST_ASSERT_EQUAL_STRING("Ever-Solar", decodeString(r.map, reg::kManufacturer, 16).c_str());
    TEST_ASSERT_EQUAL_STRING(fx::kExpectedSerial, decodeString(r.map, reg::kSerialNumber, 16).c_str());
    TEST_ASSERT_EQUAL_STRING("eversolar_legacy", decodeString(r.map, reg::kDriverId, 8).c_str());
}

static void test_unknown_identity_string_is_all_zeros() {
    RegisterMap m;
    DeviceState s;
    BridgeInfo  b;
    m.update(s, b, DiagnosticsSnapshot{}, 0);
    TEST_ASSERT_EQUAL_STRING("", decodeString(m, reg::kSerialNumber, 16).c_str());
    TEST_ASSERT_EQUAL_UINT16(0, m.at(reg::kSerialNumber));
}

static void test_read_only_driver_is_advertised_as_such() {
    // A client can tell before trying that writes are pointless.
    EversolarRig r;
    r.pollAndRender();
    TEST_ASSERT_EQUAL_UINT16(1, r.map.at(reg::kDriverReadOnly));
    TEST_ASSERT_EQUAL_UINT16(0, r.map.at(reg::kCapabilitiesWrite));
    TEST_ASSERT_EQUAL_UINT16(0, r.map.at(reg::kCapabilitiesWrite + 3));
}

// --- the night ------------------------------------------------------------------------------

static void test_stale_data_is_published_as_unknown() {
    EversolarRig  r;
    DeviceContext ctx(r.driver, r.store, r.diagnostics, clockFn);
    ctx.pollOnce();
    r.map.update(*r.store.snapshot(), r.bridge, r.diagnostics.snapshot(), g_now);
    TEST_ASSERT_FALSE(std::isnan(decodeFloat(r.map, reg::kAcPowerTotal)));

    // Sunset.
    r.device.offline = true;
    for (int i = 0; i < 10; ++i) {
        g_now += 10000;
        ctx.pollOnce();
    }
    r.map.update(*r.store.snapshot(), r.bridge, r.diagnostics.snapshot(), g_now);

    // The last real value is retained in the state, but Modbus must not present it as
    // current: a stale reading is not a measurement.
    TEST_ASSERT_TRUE(std::isnan(decodeFloat(r.map, reg::kAcPowerTotal)));
    TEST_ASSERT_FALSE(r.map.validityBit(ValidityBit::AcPowerTotal));
    TEST_ASSERT_EQUAL_UINT16(0, r.map.at(reg::kInverterOnline));
    TEST_ASSERT_EQUAL_UINT16(1, r.map.at(reg::kDataStale));
    // The bridge itself is still perfectly healthy and still answering.
    TEST_ASSERT_EQUAL_UINT16(1, r.map.at(reg::kBridgeOnline));
}

static void test_a_genuine_zero_is_published_as_zero() {
    // Dawn: the inverter is awake and producing nothing. That is data.
    EversolarRig r;
    r.device.payload = FakeEversolarDevice::Payload::Night;
    r.pollAndRender();

    TEST_ASSERT_FALSE(std::isnan(decodeFloat(r.map, reg::kAcPowerTotal)));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, decodeFloat(r.map, reg::kAcPowerTotal));
    TEST_ASSERT_TRUE(r.map.validityBit(ValidityBit::AcPowerTotal));
    TEST_ASSERT_EQUAL_UINT16(1, r.map.at(reg::kInverterOnline));
}

static void test_seconds_since_poll_is_unknown_before_the_first_success() {
    RegisterMap m;
    DeviceState s;
    m.update(s, BridgeInfo{}, DiagnosticsSnapshot{}, 500000);
    TEST_ASSERT_EQUAL_UINT32(kInvalidU32, decodeU32(m, reg::kSecondsSincePoll));
}

static void test_seconds_since_poll_counts_up() {
    EversolarRig  r;
    DeviceContext ctx(r.driver, r.store, r.diagnostics, clockFn);
    ctx.pollOnce();
    g_now += 45000;
    r.map.update(*r.store.snapshot(), r.bridge, r.diagnostics.snapshot(), g_now);
    TEST_ASSERT_EQUAL_UINT32(45, decodeU32(r.map, reg::kSecondsSincePoll));
}

static void test_counters_are_published() {
    EversolarRig  r;
    DeviceContext ctx(r.driver, r.store, r.diagnostics, clockFn);
    ctx.pollOnce();
    r.device.offline = true;
    g_now += 10000;
    ctx.pollOnce();
    r.map.update(*r.store.snapshot(), r.bridge, r.diagnostics.snapshot(), g_now);

    TEST_ASSERT_EQUAL_UINT32(1, decodeU32(r.map, reg::kPollSuccessTotal));
    TEST_ASSERT_EQUAL_UINT32(1, decodeU32(r.map, reg::kPollFailureTotal));
    TEST_ASSERT_EQUAL_UINT32(1, decodeU32(r.map, reg::kRs485Timeouts));
    TEST_ASSERT_EQUAL_UINT32(86400, decodeU32(r.map, reg::kBridgeUptime));
}

// --- the architectural claim ------------------------------------------------------------------

static void test_the_mock_hybrid_populates_the_same_map_with_no_output_changes() {
    // The whole point of the mock driver. A three-phase hybrid with two MPPTs and a battery
    // renders through exactly the code the single-phase EverSolar uses. If any of this had
    // been hardcoded around one device, this test could not pass.
    mock::MockOptions o;
    mock::MockDriver  driver(clockFn, o);
    StateStore        store;
    Diagnostics       diag;
    RegisterMap       map;
    BridgeInfo        bridge;
    bridge.bridgeOnline = true;

    g_now = 12ULL * 60 * 60 * 1000;  // midday on the simulated curve
    DeviceContext ctx(driver, store, diag, clockFn);
    ctx.pollOnce();
    map.update(*store.snapshot(), bridge, diag.snapshot(), g_now);

    // Three phases, all real.
    for (uint16_t i = 0; i < 3; ++i) {
        const uint16_t base = static_cast<uint16_t>(reg::kPhaseBase + i * reg::kPhaseStride);
        TEST_ASSERT_FALSE(std::isnan(decodeFloat(map, base + reg::kPhaseVoltageOffset)));
        TEST_ASSERT_FALSE(std::isnan(decodeFloat(map, base + reg::kPhasePowerOffset)));
    }
    TEST_ASSERT_EQUAL_UINT16(3, map.at(reg::kPhaseCount));

    // Two MPPTs.
    const uint16_t mppt2 = reg::kMpptBase + reg::kMpptStride;
    TEST_ASSERT_FALSE(std::isnan(decodeFloat(map, mppt2 + reg::kMpptVoltageOffset)));
    TEST_ASSERT_EQUAL_UINT16(2, map.at(reg::kMpptCount));

    // A battery, in the block EverSolar leaves entirely as NaN.
    TEST_ASSERT_FALSE(std::isnan(decodeFloat(map, reg::kBatterySoc)));
    TEST_ASSERT_TRUE(map.validityBit(ValidityBit::BatterySoc));
    TEST_ASSERT_EQUAL_UINT16(1, map.at(reg::kBatteryPresent));

    // And it does report an error code, unlike EverSolar.
    TEST_ASSERT_TRUE(map.validityBit(ValidityBit::ErrorCode));
    TEST_ASSERT_EQUAL_UINT16(0, map.at(reg::kErrorCode));
}

static void test_a_writable_driver_flips_the_read_only_register() {
    // No output code changes: the register follows the capabilities.
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver driver(clockFn, o);
    StateStore       store;
    Diagnostics      diag;
    RegisterMap      map;
    DeviceContext    ctx(driver, store, diag, clockFn);
    ctx.pollOnce();
    map.update(*store.snapshot(), BridgeInfo{}, diag.snapshot(), g_now);

    TEST_ASSERT_EQUAL_UINT16(0, map.at(reg::kDriverReadOnly));
    const bool anyWriteBit = map.at(reg::kCapabilitiesWrite) || map.at(reg::kCapabilitiesWrite + 1) ||
                             map.at(reg::kCapabilitiesWrite + 2) ||
                             map.at(reg::kCapabilitiesWrite + 3);
    TEST_ASSERT_TRUE(anyWriteBit);
}

static void test_validity_bitmap_and_nan_always_agree() {
    // Two ways of saying "unknown" for clients that cannot handle NaN. They must never
    // disagree, or one of them is a trap.
    EversolarRig r;
    r.pollAndRender();

    struct Pair {
        ValidityBit bit;
        uint16_t    addr;
    };
    const Pair pairs[] = {
        {ValidityBit::AcPowerTotal, reg::kAcPowerTotal},
        {ValidityBit::AcL1Voltage, reg::kAcL1Voltage},
        {ValidityBit::AcL2Voltage, reg::kPhaseBase + reg::kPhaseStride},
        {ValidityBit::BatterySoc, reg::kBatterySoc},
        {ValidityBit::GridImportPower, reg::kGridImportPower},
        {ValidityBit::EnergyTotal, reg::kEnergyTotal},
        {ValidityBit::DcMppt2Voltage, reg::kMpptBase + reg::kMpptStride},
    };
    for (const auto& p : pairs) {
        const bool nan = std::isnan(decodeFloat(r.map, p.addr));
        TEST_ASSERT_EQUAL_MESSAGE(r.map.validityBit(p.bit), !nan,
                                  "validity bit disagrees with NaN sentinel");
    }
}

static void test_validity_bits_fit_the_reserved_space() {
    // 8 registers = 128 bits at reg::kValidityBitmap.
    TEST_ASSERT_TRUE(static_cast<size_t>(ValidityBit::_Count) <= 128);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_schema_version_is_published);
    RUN_TEST(test_an_unpopulated_map_reads_as_unknown_not_zero);
    RUN_TEST(test_reads_beyond_the_map_are_refused);
    RUN_TEST(test_read_returns_the_requested_window);
    RUN_TEST(test_float32_is_high_word_first);
    RUN_TEST(test_uint32_is_high_word_first);
    RUN_TEST(test_negative_int32_round_trips);
    RUN_TEST(test_rssi_is_a_sentinel_not_zero_when_wifi_is_down);
    RUN_TEST(test_eversolar_measurements_land_in_the_right_registers);
    RUN_TEST(test_absent_phases_are_nan_not_zero);
    RUN_TEST(test_absent_battery_is_nan_not_zero);
    RUN_TEST(test_an_unsupported_declared_channel_is_nan);
    RUN_TEST(test_unsupported_error_code_is_not_published_as_zero);
    RUN_TEST(test_second_mppt_is_absent_for_a_single_string_payload);
    RUN_TEST(test_second_mppt_appears_for_a_dual_string_payload);
    RUN_TEST(test_identity_strings_are_readable);
    RUN_TEST(test_unknown_identity_string_is_all_zeros);
    RUN_TEST(test_read_only_driver_is_advertised_as_such);
    RUN_TEST(test_stale_data_is_published_as_unknown);
    RUN_TEST(test_a_genuine_zero_is_published_as_zero);
    RUN_TEST(test_seconds_since_poll_is_unknown_before_the_first_success);
    RUN_TEST(test_seconds_since_poll_counts_up);
    RUN_TEST(test_counters_are_published);
    RUN_TEST(test_the_mock_hybrid_populates_the_same_map_with_no_output_changes);
    RUN_TEST(test_a_writable_driver_flips_the_read_only_register);
    RUN_TEST(test_a_32bit_error_code_saturates_the_16bit_register);
    RUN_TEST(test_validity_bitmap_and_nan_always_agree);
    RUN_TEST(test_validity_bits_fit_the_reserved_space);
    return UNITY_END();
}
