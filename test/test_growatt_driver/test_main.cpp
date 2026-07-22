// SPDX-License-Identifier: MIT
// Growatt driver: pure register->canonical decoding, and the Modbus poll path against a
// scripted SPH. The register map itself is unvalidated hardware-wise; these tests pin the
// DECODING (scaling, 16/32-bit, sign, undeclared-when-unread) so a wrong value on the bench
// means a wrong table row, not a wrong decoder.

#include <unity.h>

#include <cstdint>
#include <vector>

#include "device/device_state.h"
#include "drivers/growatt_modbus/growatt_driver.h"
#include "drivers/growatt_modbus/growatt_registers.h"
#include "protocols/modbus/modbus_rtu.h"
#include "support/mock_transport.h"

using namespace heliograph;
using namespace heliograph::growatt;
using heliograph::test::MockTransport;

void setUp() {}
void tearDown() {}

// --- helpers ------------------------------------------------------------------------------

static void setReg(BlockData& b, uint16_t addr, uint16_t value) {
    b.values[addr - b.start] = value;
}

// Two Input blocks matching the SPH profile: base 0-124 and storage 1000-1044.
static void makeSphBlocks(BlockData blocks[2]) {
    blocks[0] = {RegSpace::Input, 0, 125, {}};
    blocks[1] = {RegSpace::Input, 1000, 45, {}};
}

// --- pure decoding ------------------------------------------------------------------------

static void test_soc_is_decoded_as_a_plain_percent() {
    BlockData blocks[2];
    makeSphBlocks(blocks);
    setReg(blocks[1], 1014, 87);

    MeasurementSet m;
    applyProfile(*findProfile("sph"), blocks, 2, m, 1000);

    const auto* soc = m.find(measurement_id::kBatterySoc);
    TEST_ASSERT_NOT_NULL(soc);
    TEST_ASSERT_TRUE(soc->valid);
    TEST_ASSERT_EQUAL_DOUBLE(87.0, soc->value);
}

static void test_voltage_scales_by_a_tenth() {
    BlockData blocks[2];
    makeSphBlocks(blocks);
    setReg(blocks[1], 1013, 512);  // 51.2 V

    MeasurementSet m;
    applyProfile(*findProfile("sph"), blocks, 2, m, 1000);
    TEST_ASSERT_EQUAL_DOUBLE(51.2, m.find(measurement_id::kBatteryVoltage)->value);
}

static void test_temperature_is_signed() {
    BlockData blocks[2];
    makeSphBlocks(blocks);
    setReg(blocks[1], 1040, 0xFFF6);  // -10 raw -> -1.0 C

    MeasurementSet m;
    applyProfile(*findProfile("sph"), blocks, 2, m, 1000);
    TEST_ASSERT_EQUAL_DOUBLE(-1.0, m.find(measurement_id::kBatteryTemperature)->value);
}

static void test_battery_power_is_a_signed_32bit_pair() {
    BlockData blocks[2];
    makeSphBlocks(blocks);
    // -5000 raw over two registers, high word first -> -500.0 W (discharging, per the source
    // label; sign convention itself is confirmed on hardware, decoding is what this pins).
    setReg(blocks[1], 1009, 0xFFFF);
    setReg(blocks[1], 1010, 0xEC78);

    MeasurementSet m;
    applyProfile(*findProfile("sph"), blocks, 2, m, 1000);
    TEST_ASSERT_EQUAL_DOUBLE(-500.0, m.find(measurement_id::kBatteryPower)->value);
}

static void test_pv_power_is_an_unsigned_32bit_pair() {
    BlockData blocks[2];
    makeSphBlocks(blocks);
    setReg(blocks[0], 116, 0x0000);
    setReg(blocks[0], 117, 0x2EE0);  // 12000 -> 1200.0 W

    MeasurementSet m;
    applyProfile(*findProfile("sph"), blocks, 2, m, 1000);
    TEST_ASSERT_EQUAL_DOUBLE(1200.0, m.find(measurement_id::kDcPowerTotal)->value);
}

static void test_a_register_in_an_unread_block_is_left_undeclared() {
    // Only the base block present; battery registers live in a block we did not pass.
    BlockData blocks[1];
    blocks[0] = {RegSpace::Input, 0, 125, {}};

    MeasurementSet m;
    applyProfile(*findProfile("sph"), blocks, 1, m, 1000);
    // SoC lives at 1014, outside the single block -> never declared, never a fabricated zero.
    TEST_ASSERT_NULL(m.find(measurement_id::kBatterySoc));
}

static void test_find_register_reports_out_of_range() {
    BlockData blocks[1];
    blocks[0] = {RegSpace::Input, 0, 10, {}};
    uint16_t out = 0xAA;
    TEST_ASSERT_FALSE(findRegister(blocks, 1, RegSpace::Input, 50, out));
    TEST_ASSERT_FALSE(findRegister(blocks, 1, RegSpace::Holding, 5, out));  // wrong space
    setReg(blocks[0], 5, 0x1234);
    TEST_ASSERT_TRUE(findRegister(blocks, 1, RegSpace::Input, 5, out));
    TEST_ASSERT_EQUAL_HEX16(0x1234, out);
}

// --- poll over a scripted Modbus device ---------------------------------------------------

// Answers any read (fn 0x04/0x03) with values from a per-address function.
static heliograph::test::Responder sphResponder() {
    return [](const std::vector<uint8_t>& req, std::vector<uint8_t>& reply) {
        if (req.size() < 8) {
            return false;
        }
        const uint8_t  unit  = req[0];
        const uint8_t  fn    = req[1];
        const uint16_t start = static_cast<uint16_t>((req[2] << 8) | req[3]);
        const uint16_t count = static_cast<uint16_t>((req[4] << 8) | req[5]);

        reply.push_back(unit);
        reply.push_back(fn);
        reply.push_back(static_cast<uint8_t>(count * 2));
        for (uint16_t i = 0; i < count; ++i) {
            const uint16_t addr = start + i;
            uint16_t       v    = 0;
            if (addr == 1014) v = 87;
            else if (addr == 1013) v = 512;
            reply.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            reply.push_back(static_cast<uint8_t>(v & 0xFF));
        }
        const uint16_t crc = modbus::crc16(reply.data(), reply.size());
        reply.push_back(static_cast<uint8_t>(crc & 0xFF));
        reply.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
        return true;
    };
}

static void test_a_full_poll_decodes_measurements_over_the_bus() {
    MockTransport transport;
    transport.setResponder(sphResponder());
    GrowattDriver driver(transport);

    DeviceState state;
    state.lastPollAttemptMs = 5000;
    TEST_ASSERT_EQUAL(PollResult::Ok, driver.poll(state));
    TEST_ASSERT_EQUAL_DOUBLE(87.0, state.measurements.find(measurement_id::kBatterySoc)->value);
    TEST_ASSERT_EQUAL_DOUBLE(51.2, state.measurements.find(measurement_id::kBatteryVoltage)->value);
    TEST_ASSERT_TRUE(state.capabilities.hasBattery);
}

static void test_silence_is_a_timeout() {
    MockTransport transport;  // no responder -> reads return 0
    GrowattDriver driver(transport);

    DeviceState state;
    state.lastPollAttemptMs = 5000;
    TEST_ASSERT_EQUAL(PollResult::Timeout, driver.poll(state));
}

// Every block refused -> the device is present but nothing is readable.
static heliograph::test::Responder alwaysException() {
    return [](const std::vector<uint8_t>& req, std::vector<uint8_t>& reply) {
        if (req.size() < 2) return false;
        reply.push_back(req[0]);
        reply.push_back(static_cast<uint8_t>(req[1] | modbus::kExceptionFlag));
        reply.push_back(0x02);  // illegal data address
        const uint16_t crc = modbus::crc16(reply.data(), reply.size());
        reply.push_back(static_cast<uint8_t>(crc & 0xFF));
        reply.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
        return true;
    };
}

static void test_all_blocks_refused_is_an_invalid_frame() {
    MockTransport transport;
    transport.setResponder(alwaysException());
    GrowattDriver driver(transport);

    DeviceState state;
    state.lastPollAttemptMs = 5000;
    TEST_ASSERT_EQUAL(PollResult::InvalidFrame, driver.poll(state));
}

static void test_one_refused_block_does_not_sink_the_poll() {
    // The bring-up case: the device speaks one generation and refuses the probe block of the
    // other. The poll must still succeed on the block that answered.
    MockTransport transport;
    transport.setResponder([](const std::vector<uint8_t>& req, std::vector<uint8_t>& reply) {
        if (req.size() < 8) return false;
        const uint16_t start = static_cast<uint16_t>((req[2] << 8) | req[3]);
        const uint16_t count = static_cast<uint16_t>((req[4] << 8) | req[5]);
        if (start >= 3000) {  // refuse the 3000-series probe, answer everything else
            reply.push_back(req[0]);
            reply.push_back(static_cast<uint8_t>(req[1] | modbus::kExceptionFlag));
            reply.push_back(0x02);
        } else {
            reply.push_back(req[0]);
            reply.push_back(req[1]);
            reply.push_back(static_cast<uint8_t>(count * 2));
            for (uint16_t i = 0; i < count; ++i) {
                const uint16_t addr = start + i;
                uint16_t       v    = addr == 1014 ? 55 : 0;
                reply.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
                reply.push_back(static_cast<uint8_t>(v & 0xFF));
            }
        }
        const uint16_t crc = modbus::crc16(reply.data(), reply.size());
        reply.push_back(static_cast<uint8_t>(crc & 0xFF));
        reply.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
        return true;
    });
    GrowattDriver driver(transport);

    DeviceState state;
    state.lastPollAttemptMs = 5000;
    TEST_ASSERT_EQUAL(PollResult::Ok, driver.poll(state));
    TEST_ASSERT_EQUAL_DOUBLE(55.0, state.measurements.find(measurement_id::kBatterySoc)->value);
}

static void test_a_refused_block_outranks_a_timeout_in_the_outcome() {
    // One block silent, another refused with an exception: the device is demonstrably alive,
    // so the poll is InvalidFrame (present, nothing usable), not the misleading Timeout.
    MockTransport transport;
    transport.setResponder([](const std::vector<uint8_t>& req, std::vector<uint8_t>& reply) {
        if (req.size() < 8) return false;
        const uint16_t start = static_cast<uint16_t>((req[2] << 8) | req[3]);
        if (start < 1000) {
            return false;  // base block: silence -> timeout
        }
        reply.push_back(req[0]);
        reply.push_back(static_cast<uint8_t>(req[1] | modbus::kExceptionFlag));
        reply.push_back(0x02);
        const uint16_t crc = modbus::crc16(reply.data(), reply.size());
        reply.push_back(static_cast<uint8_t>(crc & 0xFF));
        reply.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
        return true;
    });
    GrowattDriver driver(transport);
    DeviceState   state;
    state.lastPollAttemptMs = 5000;
    TEST_ASSERT_EQUAL(PollResult::InvalidFrame, driver.poll(state));
}

static void test_a_sustained_noise_trickle_hits_the_transaction_deadline() {
    // Same bound as the eversolar driver: a line that never completes a frame must not hold
    // the bus indefinitely (review, 2026-07-20).
    MockTransport transport;
    transport.infiniteNoise = true;
    transport.msPerRead     = 100;
    // unit, function (bit7 clear), byte-count 250 -> the parser stays "incomplete" forever
    // (frame length 255 never arrives byte-by-byte), so only the deadline can end the loop.
    transport.noisePattern = {0x01, 0x04, 0xFA};
    GrowattDriver driver(transport);

    DeviceState state;
    state.lastPollAttemptMs = 5000;
    const PollResult r = driver.poll(state);
    // No block ever reads -> the poll fails, but bounded, not hung.
    TEST_ASSERT_TRUE(r == PollResult::Timeout || r == PollResult::InvalidFrame);
    TEST_ASSERT_TRUE(transport.nowMs() >= 2000);
    TEST_ASSERT_TRUE(transport.nowMs() < 20000);
}

static void test_execute_is_unsupported_read_only() {
    MockTransport transport;
    GrowattDriver driver(transport);
    InverterCommand cmd;
    cmd.type = InverterCommandType::SetBatteryOperatingMode;
    TEST_ASSERT_EQUAL(CommandResult::Unsupported, driver.execute(cmd));
}

// --- transport line configuration ---------------------------------------------------------

// begin() must configure the UART for this driver's protocol, exactly as the EverSolar
// driver does. Without it, a boot that goes straight into the Growatt driver (no discovery
// run first, which is every reboot after the driver is selected) polls an unconfigured
// UART and hears silence forever. Found in the 2026-07-21 discovery review.
static void test_begin_configures_the_serial_line() {
    MockTransport transport;
    GrowattDriver driver(transport);

    TEST_ASSERT_TRUE(driver.begin(transport));
    TEST_ASSERT_EQUAL_UINT32(1, transport.configureCalls);
    // First recommended profile from the descriptor: the Growatt factory default, 9600 8N1.
    TEST_ASSERT_EQUAL_UINT32(9600, transport.profile().baudRate);
}

// A profile that declares its own [serial] settings wins over the descriptor's generic
// candidates: the profile knows what this family actually ships with.
static void test_a_profile_declared_serial_overrides_the_descriptor_default() {
    static const RegBlock       kOneBlock[] = {{RegSpace::Input, 0, 8}};
    static const GrowattProfile kCustom     = {
        "custom", "Custom (115200)", false, 1, 1,
        kOneBlock, 1, nullptr, 0,
        nullptr, 0,
        /*supportsRtu=*/true, /*supportsTcp=*/false, /*tcpPort=*/0,
        /*hasSerial=*/true,
        SerialProfile{115200, SerialParity::None, 8, 1, 1000, 3},
    };
    MockTransport  transport;
    GrowattOptions options;
    options.profile = &kCustom;
    GrowattDriver driver(transport, options);

    TEST_ASSERT_TRUE(driver.begin(transport));
    TEST_ASSERT_EQUAL_UINT32(115200, transport.profile().baudRate);
}

// --- generated profile registry -----------------------------------------------------------

// The tables come out of tools/gen_profiles.py (profiles/growatt/*.toml). These pin the
// lookup contract the driver options rely on: a typo'd profile id must come back nullptr
// (loud fallback in optionsFrom), and the default must be the SPH profile.
static void test_the_profile_registry_finds_sph_and_rejects_unknown_ids() {
    const GrowattProfile* sph = findProfile("sph");
    TEST_ASSERT_NOT_NULL(sph);
    TEST_ASSERT_EQUAL_STRING("sph", sph->id);
    TEST_ASSERT_TRUE(sph->hasBattery);
    TEST_ASSERT_TRUE(sph->mappingCount > 0);
    TEST_ASSERT_TRUE(sph->blockCount > 0);

    TEST_ASSERT_NULL(findProfile("sph6000_typo"));
    TEST_ASSERT_NULL(findProfile(nullptr));
    TEST_ASSERT_EQUAL_PTR(sph, &defaultProfile());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_soc_is_decoded_as_a_plain_percent);
    RUN_TEST(test_voltage_scales_by_a_tenth);
    RUN_TEST(test_temperature_is_signed);
    RUN_TEST(test_battery_power_is_a_signed_32bit_pair);
    RUN_TEST(test_pv_power_is_an_unsigned_32bit_pair);
    RUN_TEST(test_a_register_in_an_unread_block_is_left_undeclared);
    RUN_TEST(test_find_register_reports_out_of_range);
    RUN_TEST(test_a_full_poll_decodes_measurements_over_the_bus);
    RUN_TEST(test_silence_is_a_timeout);
    RUN_TEST(test_all_blocks_refused_is_an_invalid_frame);
    RUN_TEST(test_one_refused_block_does_not_sink_the_poll);
    RUN_TEST(test_a_refused_block_outranks_a_timeout_in_the_outcome);
    RUN_TEST(test_a_sustained_noise_trickle_hits_the_transaction_deadline);
    RUN_TEST(test_execute_is_unsupported_read_only);
    RUN_TEST(test_begin_configures_the_serial_line);
    RUN_TEST(test_a_profile_declared_serial_overrides_the_descriptor_default);
    RUN_TEST(test_the_profile_registry_finds_sph_and_rejects_unknown_ids);
    return UNITY_END();
}
