// SPDX-License-Identifier: MIT
// Profile-driven Modbus driver: pure register->canonical decoding, and the poll path against a
// scripted SPH. The register map itself is unvalidated hardware-wise; these tests pin the
// DECODING (scaling, 16/32-bit, sign, undeclared-when-unread) so a wrong value on the bench
// means a wrong table row, not a wrong decoder.

#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "device/device_state.h"
#include "drivers/modbus_profile/modbus_profile_driver.h"
#include "drivers/modbus_profile/profile_tables.h"
#include "protocols/modbus/modbus_rtu.h"
#include "support/mock_transport.h"

using namespace heliograph;
using namespace heliograph::profile;
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

// A biased register: the device stores temperature offset so it never goes negative on the wire,
// reporting 1000 for 0 degrees. Without an offset the choice was publishing 100 C or publishing
// no temperature at all, and temperature is not an optional channel -- it is the one that says
// an inverter is derating in a hot loft.
static void test_a_biased_register_decodes_through_its_offset() {
    static const RegBlock kBlock[] = {{RegSpace::Input, 0, 8}};
    static const RegisterMapping kMappings[] = {
        {measurement_id::kTemperature, MeasurementType::Temperature, Unit::Celsius, "Temperature",
         RegSpace::Input, 5, 1, 0.1, false, -100.0},
    };
    DeviceProfile p = *findProfile("mic_tl_x");
    p.blocks        = kBlock;
    p.blockCount    = 1;
    p.mappings      = kMappings;
    p.mappingCount  = 1;

    BlockData blocks[1];
    blocks[0] = {RegSpace::Input, 0, 8, {}};

    setReg(blocks[0], 5, 1000);  // the biased zero
    MeasurementSet m;
    applyProfile(p, blocks, 1, m, 1000);
    // WITHIN, not EQUAL: 1000 * 0.1 is 100.00000000000001 in binary floating point, so
    // subtracting the bias leaves ~5.6e-15 rather than a clean zero. That is the representation,
    // not the decode -- every output rounds it to 0.0 at one decimal. EQUAL_DOUBLE cannot express
    // this: Unity scales its tolerance by the EXPECTED value, so against 0.0 it demands exact
    // equality, which no scale-then-bias arithmetic can promise.
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, m.find(measurement_id::kTemperature)->value);

    setReg(blocks[0], 5, 1234);  // 23.4 C
    MeasurementSet warm;
    applyProfile(p, blocks, 1, warm, 1000);
    TEST_ASSERT_EQUAL_DOUBLE(23.4, warm.find(measurement_id::kTemperature)->value);

    // And below the bias is genuinely below zero, which is the whole reason the bias exists.
    setReg(blocks[0], 5, 950);
    MeasurementSet cold;
    applyProfile(p, blocks, 1, cold, 1000);
    TEST_ASSERT_EQUAL_DOUBLE(-5.0, cold.find(measurement_id::kTemperature)->value);
}

// A negative scale negates, which is how a device reporting the opposite sign convention to ours
// is corrected in data rather than in C++. Pinned because the docs claimed for a while that this
// was impossible while the decoder had always allowed it -- so the capability was real and
// undocumented, which is the state in which somebody eventually "fixes" it by adding an abs().
static void test_a_negative_scale_negates_the_reading() {
    static const RegBlock kBlock[] = {{RegSpace::Input, 0, 8}};
    static const RegisterMapping kMappings[] = {
        {measurement_id::kBatteryPower, MeasurementType::Power, Unit::Watt, "Battery Power",
         RegSpace::Input, 2, 1, -1.0, true, 0.0},
    };
    DeviceProfile p = *findProfile("mic_tl_x");
    p.blocks        = kBlock;
    p.blockCount    = 1;
    p.mappings      = kMappings;
    p.mappingCount  = 1;

    BlockData blocks[1];
    blocks[0] = {RegSpace::Input, 0, 8, {}};

    // Raw 500 in a "positive means discharging" device is 500 W leaving the battery, which is
    // -500 W in our convention (positive = charging).
    setReg(blocks[0], 2, 500);
    MeasurementSet m;
    applyProfile(p, blocks, 1, m, 1000);
    TEST_ASSERT_EQUAL_DOUBLE(-500.0, m.find(measurement_id::kBatteryPower)->value);

    // And the sign extension still happens first: raw -200 (charging, on that device) becomes
    // +200 W here rather than 65336.
    setReg(blocks[0], 2, 0xFF38);
    MeasurementSet charging;
    applyProfile(p, blocks, 1, charging, 1000);
    TEST_ASSERT_EQUAL_DOUBLE(200.0, charging.find(measurement_id::kBatteryPower)->value);
}

// A 32-bit value whose LOW word sits at the lower address. Nearly every Modbus inverter is the
// other way round, which is why high-word-first is the default -- but a vendor datasheet that
// specifies this order for a register it also recommends using is not something to decline to
// read. Getting it backwards is not a rounding error: the pair below decodes as ~34 MW.
static void test_a_low_word_first_pair_decodes_the_other_way_round() {
    static const RegBlock kBlock[] = {{RegSpace::Input, 0, 8}};
    static const RegisterMapping kLowFirst[] = {
        {measurement_id::kBatteryPower, MeasurementType::Power, Unit::Watt, "Battery Power",
         RegSpace::Input, 2, 2, 1.0, true, 0.0, /*lowWordFirst=*/true},
    };
    static const RegisterMapping kHighFirst[] = {
        {measurement_id::kBatteryPower, MeasurementType::Power, Unit::Watt, "Battery Power",
         RegSpace::Input, 2, 2, 1.0, true, 0.0, /*lowWordFirst=*/false},
    };
    DeviceProfile p = *findProfile("mic_tl_x");
    p.blocks        = kBlock;
    p.blockCount    = 1;
    p.mappingCount  = 1;

    BlockData blocks[1];
    blocks[0] = {RegSpace::Input, 0, 8, {}};
    // 2000 W as low-word-first: low half at register 2, high half at register 3.
    setReg(blocks[0], 2, 2000);
    setReg(blocks[0], 3, 0);

    p.mappings = kLowFirst;
    MeasurementSet low;
    applyProfile(p, blocks, 1, low, 1000);
    TEST_ASSERT_EQUAL_DOUBLE(2000.0, low.find(measurement_id::kBatteryPower)->value);

    // The same bytes read the default way round are the failure this flag exists to prevent.
    p.mappings = kHighFirst;
    MeasurementSet high;
    applyProfile(p, blocks, 1, high, 1000);
    TEST_ASSERT_EQUAL_DOUBLE(131072000.0, high.find(measurement_id::kBatteryPower)->value);
}

// Sign extension has to happen AFTER the words are put in the right order, or a negative value
// reassembles as a large positive one -- and on a battery register that is the difference
// between "charging at 2 kW" and "discharging at 4.29 gigawatts".
static void test_a_low_word_first_pair_still_sign_extends() {
    static const RegBlock kBlock[] = {{RegSpace::Input, 0, 8}};
    static const RegisterMapping kMappings[] = {
        {measurement_id::kBatteryPower, MeasurementType::Power, Unit::Watt, "Battery Power",
         RegSpace::Input, 2, 2, 1.0, true, 0.0, /*lowWordFirst=*/true},
    };
    DeviceProfile p = *findProfile("mic_tl_x");
    p.blocks        = kBlock;
    p.blockCount    = 1;
    p.mappings      = kMappings;
    p.mappingCount  = 1;

    BlockData blocks[1];
    blocks[0] = {RegSpace::Input, 0, 8, {}};
    // -2000 = 0xFFFFF830: low word 0xF830 at the lower address, high word 0xFFFF above it.
    setReg(blocks[0], 2, 0xF830);
    setReg(blocks[0], 3, 0xFFFF);

    MeasurementSet m;
    applyProfile(p, blocks, 1, m, 1000);
    TEST_ASSERT_EQUAL_DOUBLE(-2000.0, m.find(measurement_id::kBatteryPower)->value);
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
    ModbusProfileDriver driver(transport);

    DeviceState state;
    state.lastPollAttemptMs = 5000;
    TEST_ASSERT_EQUAL(PollResult::Ok, driver.poll(state));
    TEST_ASSERT_EQUAL_DOUBLE(87.0, state.measurements.find(measurement_id::kBatterySoc)->value);
    TEST_ASSERT_EQUAL_DOUBLE(51.2, state.measurements.find(measurement_id::kBatteryVoltage)->value);
    TEST_ASSERT_TRUE(state.capabilities.hasBattery);
}

static void test_silence_is_a_timeout() {
    MockTransport transport;  // no responder -> reads return 0
    ModbusProfileDriver driver(transport);

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

// Present, correctly addressed, refusing every range this profile asks for: a wrong profile or
// unit id, not a wire fault. This used to report InvalidFrame, which pointed the field
// diagnosis at the cabling and contradicted the bus counters -- an exception rightly moves
// none of them, so the poll failed every ten seconds while all three RS485 counters read zero.
// SunSpec has always called this NotRegistered; the two Modbus drivers now agree.
static void test_all_blocks_refused_is_not_registered() {
    MockTransport transport;
    transport.setResponder(alwaysException());
    ModbusProfileDriver driver(transport);

    DeviceState state;
    state.lastPollAttemptMs = 5000;
    TEST_ASSERT_EQUAL(PollResult::NotRegistered, driver.poll(state));
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
    ModbusProfileDriver driver(transport);

    DeviceState state;
    state.lastPollAttemptMs = 5000;
    TEST_ASSERT_EQUAL(PollResult::Ok, driver.poll(state));
    TEST_ASSERT_EQUAL_DOUBLE(55.0, state.measurements.find(measurement_id::kBatterySoc)->value);
}

static void test_a_refused_block_outranks_a_timeout_in_the_outcome() {
    // One block silent, another refused with an exception: the device is demonstrably alive,
    // so the poll says "present, nothing usable" rather than the misleading Timeout.
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
    ModbusProfileDriver driver(transport);
    DeviceState   state;
    state.lastPollAttemptMs = 5000;
    TEST_ASSERT_EQUAL(PollResult::NotRegistered, driver.poll(state));
}

// A bus with a bad ground or missing termination shows up as CRC failures, and that is the one
// symptom the alerting rules watch -- every night legitimately produces timeouts, so a rule
// built on those would drown. Until this was fixed the codec folded CRC into a generic protocol
// error and the driver reported InvalidFrame, making PollResult::ChecksumError structurally
// unreachable on a Modbus bus: the counter existed, the metric existed, the alert existed, and
// nothing could ever raise it (review, 2026-07-25).
static void test_a_corrupt_reply_is_reported_as_a_checksum_error() {
    MockTransport transport;
    transport.setResponder([](const std::vector<uint8_t>& req, std::vector<uint8_t>& reply) {
        if (req.size() < 8) return false;
        const uint16_t count = static_cast<uint16_t>((req[4] << 8) | req[5]);
        reply.push_back(req[0]);
        reply.push_back(req[1]);
        reply.push_back(static_cast<uint8_t>(count * 2));
        for (uint16_t i = 0; i < count * 2; ++i) {
            reply.push_back(0x11);
        }
        const uint16_t crc = modbus::crc16(reply.data(), reply.size());
        reply.push_back(static_cast<uint8_t>((crc & 0xFF) ^ 0xFF));  // wrecked
        reply.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
        return true;
    });
    ModbusProfileDriver driver(transport);
    DeviceState   state;
    state.lastPollAttemptMs = 5000;
    TEST_ASSERT_EQUAL(PollResult::ChecksumError, driver.poll(state));
}

// The degrading bus, which is the case the metric exists for and the one it used to miss
// entirely. One block comes back corrupt, the other decodes; the poll therefore succeeds --
// correctly, there IS usable data -- and for as long as the counter was derived from that
// verdict the checksum metric stayed at zero. A third of the frames on the floor and the
// dashboard said the wire was fine. The tally has to move on the transaction, not the poll.
static void test_a_corrupt_block_is_counted_even_when_the_poll_succeeds() {
    MockTransport transport;
    transport.setResponder([](const std::vector<uint8_t>& req, std::vector<uint8_t>& reply) {
        if (req.size() < 8) return false;
        const uint16_t start = static_cast<uint16_t>((req[2] << 8) | req[3]);
        const uint16_t count = static_cast<uint16_t>((req[4] << 8) | req[5]);
        reply.push_back(req[0]);
        reply.push_back(req[1]);
        reply.push_back(static_cast<uint8_t>(count * 2));
        for (uint16_t i = 0; i < count * 2; ++i) {
            reply.push_back(0x00);
        }
        const uint16_t crc = modbus::crc16(reply.data(), reply.size());
        // Only the storage block is wrecked. The base and probe blocks arrive clean, so the
        // poll has something to publish.
        const bool wreck = start == 1000;
        reply.push_back(static_cast<uint8_t>((crc & 0xFF) ^ (wreck ? 0xFF : 0x00)));
        reply.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
        return true;
    });
    ModbusProfileDriver driver(transport);
    DeviceState   state;
    state.lastPollAttemptMs = 5000;

    TEST_ASSERT_EQUAL(PollResult::Ok, driver.poll(state));
    TEST_ASSERT_EQUAL_UINT32(1, driver.busErrors().checksumErrors);
    TEST_ASSERT_EQUAL_UINT32(0, driver.busErrors().timeouts);

    // Cumulative, so a bus that keeps corrupting keeps climbing.
    TEST_ASSERT_EQUAL(PollResult::Ok, driver.poll(state));
    TEST_ASSERT_EQUAL_UINT32(2, driver.busErrors().checksumErrors);
}

// The probe block is the one this profile does not know the device has. The Modbus spec says an
// unknown range should be answered with an exception, but a device may simply say nothing --
// and unflagged, that silence would add a timeout to the metrics on EVERY poll of a perfectly
// wired inverter: ~360 an hour, forever. `probe = true` in the profile is what stops it.
static void test_a_silent_probe_block_moves_no_bus_counter() {
    MockTransport transport;
    transport.setResponder([](const std::vector<uint8_t>& req, std::vector<uint8_t>& reply) {
        if (req.size() < 8) return false;
        const uint16_t start = static_cast<uint16_t>((req[2] << 8) | req[3]);
        if (start >= 3000) {
            return false;  // the probe range: this unit does not implement it, and stays silent
        }
        const uint16_t count = static_cast<uint16_t>((req[4] << 8) | req[5]);
        reply.push_back(req[0]);
        reply.push_back(req[1]);
        reply.push_back(static_cast<uint8_t>(count * 2));
        for (uint16_t i = 0; i < count * 2; ++i) {
            reply.push_back(0x00);
        }
        const uint16_t crc = modbus::crc16(reply.data(), reply.size());
        reply.push_back(static_cast<uint8_t>(crc & 0xFF));
        reply.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
        return true;
    });
    ModbusProfileDriver driver(transport);
    DeviceState   state;
    state.lastPollAttemptMs = 5000;

    TEST_ASSERT_EQUAL(PollResult::Ok, driver.poll(state));
    TEST_ASSERT_EQUAL(PollResult::Ok, driver.poll(state));
    const auto errors = driver.busErrors();
    TEST_ASSERT_EQUAL_UINT32(0, errors.timeouts);
    TEST_ASSERT_EQUAL_UINT32(0, errors.checksumErrors);
    TEST_ASSERT_EQUAL_UINT32(0, errors.invalidFrames);
}

// ...and the flag is on the block the profile actually declares as a probe, not on any block
// that happens to be silent: a mapped block going quiet is still a real timeout.
static void test_a_silent_mapped_block_is_still_counted() {
    MockTransport transport;
    transport.setResponder([](const std::vector<uint8_t>& req, std::vector<uint8_t>& reply) {
        if (req.size() < 8) return false;
        const uint16_t start = static_cast<uint16_t>((req[2] << 8) | req[3]);
        if (start == 1000) {
            return false;  // a mapped block, silent
        }
        const uint16_t count = static_cast<uint16_t>((req[4] << 8) | req[5]);
        reply.push_back(req[0]);
        reply.push_back(req[1]);
        reply.push_back(static_cast<uint8_t>(count * 2));
        for (uint16_t i = 0; i < count * 2; ++i) {
            reply.push_back(0x00);
        }
        const uint16_t crc = modbus::crc16(reply.data(), reply.size());
        reply.push_back(static_cast<uint8_t>(crc & 0xFF));
        reply.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
        return true;
    });
    ModbusProfileDriver driver(transport);
    DeviceState   state;
    state.lastPollAttemptMs = 5000;

    TEST_ASSERT_EQUAL(PollResult::Ok, driver.poll(state));
    TEST_ASSERT_EQUAL_UINT32(1, driver.busErrors().timeouts);
}

// An exception reply is a healthy device declining a range -- the bring-up probe block does
// exactly this. Counting it as a bus error would put a permanent slope on the metric of every
// correctly wired installation.
static void test_a_refused_block_moves_no_bus_counter() {
    MockTransport transport;
    transport.setResponder(alwaysException());
    ModbusProfileDriver driver(transport);

    DeviceState state;
    state.lastPollAttemptMs = 5000;
    driver.poll(state);
    const auto errors = driver.busErrors();
    TEST_ASSERT_EQUAL_UINT32(0, errors.checksumErrors);
    TEST_ASSERT_EQUAL_UINT32(0, errors.timeouts);
    TEST_ASSERT_EQUAL_UINT32(0, errors.invalidFrames);
}

// ...while a frame that arrives intact but is not ours stays InvalidFrame. Counting a neighbour
// on the bus as line corruption would send someone to check a cable that is fine.
static void test_an_intact_reply_from_another_unit_is_not_a_checksum_error() {
    MockTransport transport;
    transport.setResponder([](const std::vector<uint8_t>& req, std::vector<uint8_t>& reply) {
        if (req.size() < 8) return false;
        reply.push_back(static_cast<uint8_t>(req[0] + 1));  // someone else's address
        reply.push_back(req[1]);
        reply.push_back(2);
        reply.push_back(0x00);
        reply.push_back(0x2A);
        const uint16_t crc = modbus::crc16(reply.data(), reply.size());
        reply.push_back(static_cast<uint8_t>(crc & 0xFF));
        reply.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
        return true;
    });
    ModbusProfileDriver driver(transport);
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
    ModbusProfileDriver driver(transport);

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
    ModbusProfileDriver driver(transport);
    InverterCommand cmd;
    cmd.type = InverterCommandType::SetBatteryOperatingMode;
    TEST_ASSERT_EQUAL(CommandResult::Unsupported, driver.execute(cmd));
}

// --- transport line configuration ---------------------------------------------------------

// begin() must configure the UART for this driver's protocol, exactly as the EverSolar
// driver does. Without it, a boot that goes straight into this driver (no discovery
// run first, which is every reboot after the driver is selected) polls an unconfigured
// UART and hears silence forever. Found in the 2026-07-21 discovery review.
static void test_begin_configures_the_serial_line() {
    MockTransport transport;
    ModbusProfileDriver driver(transport);

    TEST_ASSERT_TRUE(driver.begin(transport));
    TEST_ASSERT_EQUAL_UINT32(1, transport.configureCalls);
    // First recommended profile from the descriptor: the generic 9600 8N1 fallback.
    TEST_ASSERT_EQUAL_UINT32(9600, transport.profile().baudRate);
}

// A profile that declares its own [serial] settings wins over the descriptor's generic
// candidates: the profile knows what this family actually ships with.
static void test_a_profile_declared_serial_overrides_the_descriptor_default() {
    static const RegBlock       kOneBlock[] = {{RegSpace::Input, 0, 8}};
    static const DeviceProfile kCustom     = {
        "custom", "Custom (115200)", "Test", false, 1, 1,
        kOneBlock, 1, nullptr, 0,
        nullptr, 0,
        /*supportsRtu=*/true, /*supportsTcp=*/false, /*tcpPort=*/0,
        /*hasSerial=*/true,
        SerialProfile{115200, SerialParity::None, 8, 1, 1000, 3},
    };
    MockTransport  transport;
    ProfileOptions options;
    options.profile = &kCustom;
    ModbusProfileDriver driver(transport, options);

    TEST_ASSERT_TRUE(driver.begin(transport));
    TEST_ASSERT_EQUAL_UINT32(115200, transport.profile().baudRate);
}

// --- generated profile registry -----------------------------------------------------------

// The tables come out of tools/gen_profiles.py (profiles/*/*.toml). These pin the
// lookup contract the driver options rely on: a typo'd profile id must come back nullptr
// (loud fallback in optionsFrom), and the default must be the SPH profile.
static void test_the_profile_registry_finds_sph_and_rejects_unknown_ids() {
    const DeviceProfile* sph = findProfile("sph");
    TEST_ASSERT_NOT_NULL(sph);
    TEST_ASSERT_EQUAL_STRING("sph", sph->id);
    TEST_ASSERT_TRUE(sph->hasBattery);
    TEST_ASSERT_TRUE(sph->mappingCount > 0);
    TEST_ASSERT_TRUE(sph->blockCount > 0);

    TEST_ASSERT_NULL(findProfile("sph6000_typo"));
    TEST_ASSERT_NULL(findProfile(nullptr));
    TEST_ASSERT_EQUAL_PTR(sph, &defaultProfile());
}

// --- MIC TL-X profile -----------------------------------------------------------------------

// A second profile in the build is the moment the `profile` option stops being cosmetic: pick
// the wrong one and you get another family's map applied to your inverter. The descriptor now
// enumerates the ids so validateDriverOptions refuses a typo at configuration time, instead of
// optionsFrom() silently falling back to SPH.
static void test_the_profile_option_enumerates_every_compiled_profile() {
    // Deliberately no hardcoded count. This test pins that the option lists EVERY compiled
    // profile, which is a relationship, not a number -- asserting "2" made adding a profile fail
    // a test about the option list, which teaches the wrong lesson and invites bumping the
    // literal without checking what it guards.
    TEST_ASSERT_TRUE(profileCount() >= 2);

    const auto& allowed = descriptor().options[1].allowedValues;
    for (size_t i = 0; i < profileCount(); ++i) {
        const std::string id = profileAt(i).id;
        TEST_ASSERT_TRUE_MESSAGE(std::find(allowed.begin(), allowed.end(), id) != allowed.end(),
                                 "a compiled profile is missing from the option's allowed values");
    }

    // Out of range folds to the first entry rather than reading past the array.
    TEST_ASSERT_EQUAL_PTR(&profileAt(0), &profileAt(profileCount()));

    TEST_ASSERT_EQUAL_STRING("profile", descriptor().options[1].key.c_str());
    TEST_ASSERT_EQUAL_UINT32(profileCount() + 1, allowed.size());
    // "" must survive as an allowed value: it is the documented default meaning "use the
    // default profile", and validateDriverOptions checks values it finds, empty or not.
    TEST_ASSERT_TRUE(std::find(allowed.begin(), allowed.end(), "") != allowed.end());
    TEST_ASSERT_TRUE(std::find(allowed.begin(), allowed.end(), "mic_tl_x") != allowed.end());

    DriverOptions     values{{"profile", "mic_tlx"}};  // the plausible typo
    DriverOptionError err;
    TEST_ASSERT_FALSE(validateDriverOptions(descriptor(), values, err));
    TEST_ASSERT_EQUAL_STRING("profile", err.key.c_str());

    values["profile"] = "mic_tl_x";
    TEST_ASSERT_TRUE(validateDriverOptions(descriptor(), values, err));
    values["profile"] = "";
    TEST_ASSERT_TRUE(validateDriverOptions(descriptor(), values, err));
}

static void test_the_mic_profile_describes_a_single_phase_single_tracker_string_inverter() {
    const DeviceProfile* mic = findProfile("mic_tl_x");
    TEST_ASSERT_NOT_NULL(mic);
    TEST_ASSERT_FALSE(mic->hasBattery);
    TEST_ASSERT_EQUAL_UINT8(1, mic->phaseCount);
    TEST_ASSERT_EQUAL_UINT8(1, mic->mpptCount);
    // 9600 8N1 comes from the profile's own [serial] block, not the descriptor's candidates.
    TEST_ASSERT_TRUE(mic->hasSerial);
    TEST_ASSERT_EQUAL_UINT32(9600, mic->serial.baudRate);
    TEST_ASSERT_EQUAL(SerialParity::None, mic->serial.parity);
}

// Which register generation a MIC populates is NOT settled: the vendor CSV marks the 3000
// range "Use for TL-X and TL-XH", so it is not hybrid-only the way an earlier draft of the
// profile claimed. The profile therefore PROBES 3000 while MAPPING nothing from it -- the
// bring-up dump answers the question, and until it does, no published reading may come from
// a range nobody has confirmed this device speaks.
static void test_the_mic_profile_probes_3000_but_publishes_nothing_from_it() {
    const DeviceProfile* mic = findProfile("mic_tl_x");
    TEST_ASSERT_NOT_NULL(mic);

    bool probesTheOpenRange = false;
    for (size_t i = 0; i < mic->blockCount; ++i) {
        probesTheOpenRange = probesTheOpenRange || mic->blocks[i].start >= 3000;
    }
    TEST_ASSERT_TRUE(probesTheOpenRange);

    for (size_t i = 0; i < mic->mappingCount; ++i) {
        TEST_ASSERT_TRUE(mic->mappings[i].address < 3000);
    }
    // Same rule for the write row: writing into an unconfirmed generation is how you set a
    // register you did not mean to.
    for (size_t i = 0; i < mic->writeCount; ++i) {
        TEST_ASSERT_TRUE(mic->writes[i].address < 3000);
    }
}

// One frame of plausible mid-afternoon values, decoded through the real table. This pins the
// scaling decisions that differ from the rest of the map -- frequency is /100 where almost
// everything else is /10, and the runtime counter is in half-seconds.
static void test_the_mic_profile_decodes_a_realistic_frame() {
    BlockData blocks[2];
    blocks[0] = {RegSpace::Input, 0, 125, {}};
    blocks[1] = {RegSpace::Holding, 0, 45, {}};

    setReg(blocks[0], 1, 0);       // Ppv high
    setReg(blocks[0], 2, 12040);   // Ppv low   -> 1204.0 W
    setReg(blocks[0], 3, 3105);    // Vpv1      -> 310.5 V
    setReg(blocks[0], 4, 39);      // Ipv1      -> 3.9 A
    setReg(blocks[0], 35, 0);      // Pac high
    setReg(blocks[0], 36, 11880);  // Pac low   -> 1188.0 W
    setReg(blocks[0], 37, 5001);   // Fac       -> 50.01 Hz  (/100, not /10)
    setReg(blocks[0], 38, 2314);   // Vac1      -> 231.4 V
    setReg(blocks[0], 53, 0);      // E-today high
    setReg(blocks[0], 54, 87);     // E-today low -> 8.7 kWh
    setReg(blocks[0], 93, 412);    // Temp      -> 41.2 °C

    MeasurementSet m;
    applyProfile(*findProfile("mic_tl_x"), blocks, 2, m, 1000);

    const auto* pv = m.find(measurement_id::kDcPowerTotal);
    TEST_ASSERT_NOT_NULL(pv);
    TEST_ASSERT_EQUAL_DOUBLE(1204.0, pv->value);

    const auto* vpv = m.find(measurement_id::kDcMppt1Voltage);
    TEST_ASSERT_NOT_NULL(vpv);
    TEST_ASSERT_EQUAL_DOUBLE(310.5, vpv->value);

    const auto* ac = m.find(measurement_id::kAcPowerTotal);
    TEST_ASSERT_NOT_NULL(ac);
    TEST_ASSERT_EQUAL_DOUBLE(1188.0, ac->value);

    const auto* hz = m.find(measurement_id::kAcFrequency);
    TEST_ASSERT_NOT_NULL(hz);
    TEST_ASSERT_EQUAL_DOUBLE(50.01, hz->value);

    const auto* vac = m.find(measurement_id::kAcL1Voltage);
    TEST_ASSERT_NOT_NULL(vac);
    TEST_ASSERT_EQUAL_DOUBLE(231.4, vac->value);

    const auto* today = m.find(measurement_id::kEnergyToday);
    TEST_ASSERT_NOT_NULL(today);
    TEST_ASSERT_EQUAL_DOUBLE(8.7, today->value);

    const auto* temp = m.find(measurement_id::kTemperature);
    TEST_ASSERT_NOT_NULL(temp);
    TEST_ASSERT_EQUAL_DOUBLE(41.2, temp->value);

    // A string inverter has no battery and no second tracker. Neither may appear as a
    // confident zero -- an undeclared channel is the honest answer.
    TEST_ASSERT_NULL(m.find(measurement_id::kBatterySoc));
    TEST_ASSERT_NULL(m.find(measurement_id::kDcMppt2Voltage));
}

// Work time total counts half-seconds; 7200 of them is one hour. Its own test because the
// scale is a repeating fraction in the TOML and therefore the easiest row to get subtly wrong.
static void test_the_mic_profile_converts_half_seconds_to_hours() {
    BlockData blocks[2];
    blocks[0] = {RegSpace::Input, 0, 125, {}};
    blocks[1] = {RegSpace::Holding, 0, 45, {}};

    // 12 345 hours = 88 884 000 half-seconds = 0x054C_4320.
    setReg(blocks[0], 57, 0x054C);
    setReg(blocks[0], 58, 0x4320);

    MeasurementSet m;
    applyProfile(*findProfile("mic_tl_x"), blocks, 2, m, 1000);

    const auto* hours = m.find(measurement_id::kOperatingHours);
    TEST_ASSERT_NOT_NULL(hours);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 12345.0, hours->value);
}

// --- write path -------------------------------------------------------------------------
//
// The chain runs profile row -> capabilities() -> execute() -> FC06. Every case below turns on
// one link of it, because a driver that ADVERTISES a setpoint and then refuses it is worse than
// one that never offered.

namespace {

/// A profile carrying one VERIFIED write row, which no shipped profile has.
///
/// Built here rather than by flipping a real profile's flag: `verified = true` means somebody
/// wrote the register on hardware and confirmed the effect, and a test must not be able to
/// claim that on a device's behalf.
const WriteMapping kTestWrites[] = {
    {InverterCommandType::SetActivePowerLimitPercent, "Active Power Limit", RegSpace::Holding, 3,
     1, false, 1.0, 0.0, 100.0, 1.0, Unit::Percent, true},
};

/// The same row, needing FC16 -- which the Modbus client cannot send.
const WriteMapping kMultiWordWrites[] = {
    {InverterCommandType::SetActivePowerLimitPercent, "Active Power Limit", RegSpace::Holding, 3,
     2, false, 1.0, 0.0, 100.0, 1.0, Unit::Percent, true},
};

DeviceProfile profileWith(const WriteMapping* writes, size_t count) {
    DeviceProfile p = *findProfile("mic_tl_x");
    p.writes         = writes;
    p.writeCount     = count;
    return p;
}

/// Echoes a well-formed FC06 confirmation, which is what a device that accepted the write sends.
void echoWrites(MockTransport& t) {
    t.setResponder([](const std::vector<uint8_t>& req, std::vector<uint8_t>& reply) {
        if (req.size() < 6 || req[1] != 0x06) {
            return false;
        }
        reply.assign(req.begin(), req.begin() + 6);
        const uint16_t crc = modbus::crc16(reply.data(), reply.size());
        reply.push_back(static_cast<uint8_t>(crc & 0xFF));
        reply.push_back(static_cast<uint8_t>(crc >> 8));
        return true;
    });
}

/// A mode setpoint: selects from a list rather than moving along a range. The values are
/// deliberately NOT 0,1,2 -- a real EMS-mode register numbers its modes with gaps where a mode was
/// retired, and an implementation that sent the selection INDEX instead of the option's value
/// would pass a test built on consecutive numbering and enter the wrong mode on hardware.
const EnumOption kModeOptions[] = {
    {0, "Self-consumption"},
    {2, "Forced"},
    {3, "External EMS"},
};
const WriteMapping kModeWrites[] = {
    {InverterCommandType::SetBatteryOperatingMode, "EMS mode", RegSpace::Holding, 13049, 1, false,
     1.0, 0.0, 0.0, 0.0, Unit::None, true, kModeOptions, 3},
};

/// The same row with no modes declared, which must never become a control surface.
const WriteMapping kModeWritesNoOptions[] = {
    {InverterCommandType::SetBatteryOperatingMode, "EMS mode", RegSpace::Holding, 13049, 1, false,
     1.0, 0.0, 0.0, 0.0, Unit::None, true, nullptr, 0},
};

/// The same row with verified = false, which is how every shipped profile carries it.
const WriteMapping kUnverifiedWrites[] = {
    {InverterCommandType::SetActivePowerLimitPercent, "Active Power Limit", RegSpace::Holding, 3,
     1, false, 1.0, 0.0, 100.0, 1.0, Unit::Percent, false},
};

}  // namespace

// THE GATE THAT MATTERS MOST, and the one nothing was checking.
//
// A test existed asserting that the shipped row carries verified = false -- but that is a fact
// about the DATA. Nothing asserted the driver acts on it, so deleting the check in writeFor()
// left every test green while unverified research became a live control surface on somebody's
// inverter. Found by mutation-testing this change, not by reading it.
// --- the Deye/Sunsynk single-phase hybrid profile ------------------------------------------

namespace {
/// The three holding blocks the profile declares: 96-125, 150-192, 240-247.
void makeDeyeBlocks(BlockData blocks[3]) {
    blocks[0] = {RegSpace::Holding, 96, 30, {}};
    blocks[1] = {RegSpace::Holding, 150, 43, {}};
    blocks[2] = {RegSpace::Holding, 240, 8, {}};
}
}  // namespace

static void test_the_deye_profile_describes_a_single_phase_two_string_hybrid() {
    const DeviceProfile* p = findProfile("deye_sun_xk_sg");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("Deye", p->manufacturer);
    TEST_ASSERT_EQUAL_UINT8(1, p->phaseCount);
    // Two, not three: a third string exists in one source and not the other, and mppts is what
    // the profile actually maps rather than what the family can have.
    TEST_ASSERT_EQUAL_UINT8(2, p->mpptCount);
    TEST_ASSERT_TRUE(p->hasBattery);

    // Every block is holding. This family has no input registers at all, and reading function 04
    // against it would answer nothing on a bus that is wired correctly.
    TEST_ASSERT_EQUAL_UINT32(3, p->blockCount);
    for (size_t i = 0; i < p->blockCount; ++i) {
        TEST_ASSERT_EQUAL(RegSpace::Holding, p->blocks[i].space);
    }
}

static void test_the_deye_profile_decodes_a_realistic_frame() {
    BlockData blocks[3];
    makeDeyeBlocks(blocks);

    // Low word first: both sources read this family that way, so the low half is at 96.
    setReg(blocks[0], 96, 41230);  // E-total     -> 4123.0 kWh (low half)
    setReg(blocks[0], 97, 0);      //                           (high half)
    setReg(blocks[0], 108, 173);   // E-today     -> 17.3 kWh
    setReg(blocks[0], 109, 3210);  // PV1 V       -> 321.0 V
    setReg(blocks[0], 110, 42);    // PV1 I       -> 4.2 A
    setReg(blocks[0], 111, 2980);  // PV2 V       -> 298.0 V
    setReg(blocks[0], 112, 38);    // PV2 I       -> 3.8 A
    setReg(blocks[1], 150, 2331);  // Grid V      -> 233.1 V
    setReg(blocks[1], 164, 1450);  // Current     -> 14.5 A
    setReg(blocks[1], 175, 3320);  // AC power    -> 3320 W
    setReg(blocks[1], 182, 1284);  // Batt temp   -> 28.4 °C  (biased by 100)
    setReg(blocks[1], 183, 5320);  // Batt V      -> 53.2 V
    setReg(blocks[1], 184, 76);    // Batt SoC    -> 76 %
    setReg(blocks[1], 186, 1350);  // PV1 W
    setReg(blocks[1], 187, 1140);  // PV2 W

    MeasurementSet m;
    applyProfile(*findProfile("deye_sun_xk_sg"), blocks, 3, m, 1000);

    TEST_ASSERT_EQUAL_DOUBLE(4123.0, m.find(measurement_id::kEnergyTotal)->value);
    TEST_ASSERT_EQUAL_DOUBLE(17.3, m.find(measurement_id::kEnergyToday)->value);
    TEST_ASSERT_EQUAL_DOUBLE(321.0, m.find(measurement_id::kDcMppt1Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(4.2, m.find(measurement_id::kDcMppt1Current)->value);
    TEST_ASSERT_EQUAL_DOUBLE(1350.0, m.find(measurement_id::kDcMppt1Power)->value);
    TEST_ASSERT_EQUAL_DOUBLE(298.0, m.find(measurement_id::kDcMppt2Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(1140.0, m.find(measurement_id::kDcMppt2Power)->value);
    TEST_ASSERT_EQUAL_DOUBLE(233.1, m.find(measurement_id::kAcL1Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(14.5, m.find(measurement_id::kAcL1Current)->value);
    TEST_ASSERT_EQUAL_DOUBLE(3320.0, m.find(measurement_id::kAcPowerTotal)->value);
    TEST_ASSERT_EQUAL_DOUBLE(53.2, m.find(measurement_id::kBatteryVoltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(76.0, m.find(measurement_id::kBatterySoc)->value);

    // The biased register, which is the whole reason `offset` exists. Raw 1284 is 28.4 °C, not
    // 128.4 -- and a profile that dropped the offset would publish a battery permanently on fire.
    TEST_ASSERT_EQUAL_DOUBLE(28.4, m.find(measurement_id::kBatteryTemperature)->value);
}

// Below the bias is genuinely below zero: an outdoor battery on a winter morning.
static void test_the_deye_battery_temperature_survives_a_freezing_morning() {
    BlockData blocks[3];
    makeDeyeBlocks(blocks);
    setReg(blocks[1], 182, 964);  // -3.6 °C

    MeasurementSet m;
    applyProfile(*findProfile("deye_sun_xk_sg"), blocks, 3, m, 1000);
    TEST_ASSERT_EQUAL_DOUBLE(-3.6, m.find(measurement_id::kBatteryTemperature)->value);
}

// A hybrid charging its battery from the grid pushes AC power the other way. Declared s16 in the
// profile for exactly this: read unsigned, a 2 kW import would publish as roughly 63 megawatts.
static void test_the_deye_ac_power_goes_negative_while_importing() {
    BlockData blocks[3];
    makeDeyeBlocks(blocks);
    setReg(blocks[1], 175, 0xF830);  // -2000

    MeasurementSet m;
    applyProfile(*findProfile("deye_sun_xk_sg"), blocks, 3, m, 1000);
    TEST_ASSERT_EQUAL_DOUBLE(-2000.0, m.find(measurement_id::kAcPowerTotal)->value);
}

// The channels held back because the sources do not agree, or do not say. Pinned as ABSENT so
// that adding one later is a deliberate act with a bench reading behind it, rather than
// something that drifts in unnoticed.
static void test_the_deye_profile_publishes_nothing_it_could_not_source() {
    BlockData blocks[3];
    makeDeyeBlocks(blocks);
    // Plausible values in every register the profile deliberately leaves alone.
    setReg(blocks[1], 190, 1500);  // battery power  -- direction unstated by both sources
    setReg(blocks[1], 191, 2800);  // battery current -- same
    setReg(blocks[1], 192, 4999);  // frequency -- grid or load, sources disagree
    setReg(blocks[1], 169, 800);   // grid flow -- signed and bidirectional, cannot split

    MeasurementSet m;
    applyProfile(*findProfile("deye_sun_xk_sg"), blocks, 3, m, 1000);

    TEST_ASSERT_NULL(m.find(measurement_id::kBatteryPower));
    TEST_ASSERT_NULL(m.find(measurement_id::kBatteryCurrent));
    TEST_ASSERT_NULL(m.find(measurement_id::kAcFrequency));
    TEST_ASSERT_NULL(m.find(measurement_id::kGridImportPower));
    TEST_ASSERT_NULL(m.find(measurement_id::kGridExportPower));
    TEST_ASSERT_NULL(m.find(measurement_id::kTemperature));   // 90/91: sources disagree which
    TEST_ASSERT_NULL(m.find(measurement_id::kDcPowerTotal));  // no whole-array register
    TEST_ASSERT_NULL(m.find(measurement_id::kDcMppt3Power));  // third string: one source only
}

// The mode row is declared and doubly dormant, and the SECOND reason is the one a flipped
// `verified` flag would not fix: both sources write this family with FC16, which this firmware's
// write path refuses. Enabling it needs FC16 support, not a flag.
static void test_the_deye_mode_row_is_declared_and_refused_for_two_reasons() {
    const DeviceProfile* p = findProfile("deye_sun_xk_sg");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT32(1, p->writeCount);

    const WriteMapping& w = p->writes[0];
    TEST_ASSERT_EQUAL(InverterCommandType::SetBatteryOperatingMode, w.command);
    TEST_ASSERT_EQUAL_UINT16(244, w.address);
    TEST_ASSERT_FALSE(w.verified);
    TEST_ASSERT_TRUE(w.useWriteMultiple);
    TEST_ASSERT_EQUAL_UINT32(3, w.optionCount);
    TEST_ASSERT_EQUAL_STRING("Zero Export", w.options[2].label);
    TEST_ASSERT_EQUAL_INT32(2, w.options[2].value);

    // And dormant in effect: no capability, and the command refused.
    MockTransport  transport;
    ProfileOptions options;
    options.profile = p;
    ModbusProfileDriver driver(transport, options);
    TEST_ASSERT_FALSE(driver.capabilities().canWrite(InverterCapability::SetBatteryOperatingMode));

    InverterCommand cmd;
    cmd.type      = InverterCommandType::SetBatteryOperatingMode;
    cmd.enumValue = 2;
    TEST_ASSERT_EQUAL(CommandResult::Unsupported, driver.execute(cmd));
    TEST_ASSERT_TRUE(transport.writes.empty());
}

// --- the Solis RHI single-phase hybrid profile ---------------------------------------------

namespace {
/// The profile's four blocks: three input, one holding.
void makeSolisBlocks(BlockData blocks[4]) {
    blocks[0] = {RegSpace::Input, 33029, 30, {}};
    blocks[1] = {RegSpace::Input, 33073, 30, {}};
    blocks[2] = {RegSpace::Input, 33133, 40, {}};
    blocks[3] = {RegSpace::Holding, 43073, 40, {}};
}
}  // namespace

static void test_the_solis_profile_decodes_a_realistic_frame() {
    BlockData blocks[4];
    makeSolisBlocks(blocks);

    // 32-bit values are high word first. Source B lists them low-word-first in its own notation,
    // which is the one transcription mistake that would put every one of these out by 65536.
    setReg(blocks[0], 33029, 0);
    setReg(blocks[0], 33030, 8412);   // E-total   -> 8412 kWh
    setReg(blocks[0], 33035, 214);    // E-today   -> 21.4 kWh
    setReg(blocks[0], 33049, 3180);   // PV1 V     -> 318.0 V
    setReg(blocks[0], 33050, 51);     // PV1 I     -> 5.1 A
    setReg(blocks[0], 33051, 2960);   // PV2 V     -> 296.0 V
    setReg(blocks[0], 33052, 44);     // PV2 I     -> 4.4 A
    setReg(blocks[0], 33057, 0);
    setReg(blocks[0], 33058, 2920);   // PV power  -> 2920 W
    setReg(blocks[1], 33073, 2342);   // AC V      -> 234.2 V
    setReg(blocks[1], 33076, 118);    // AC I      -> 11.8 A
    setReg(blocks[1], 33079, 0);
    setReg(blocks[1], 33080, 2760);   // AC power  -> 2760 W
    setReg(blocks[1], 33093, 371);    // Temp      -> 37.1 °C
    setReg(blocks[1], 33094, 4998);   // Freq      -> 49.98 Hz
    setReg(blocks[2], 33133, 512);    // Batt V    -> 51.2 V
    setReg(blocks[2], 33139, 64);     // Batt SoC  -> 64 %
    setReg(blocks[2], 33161, 0);
    setReg(blocks[2], 33162, 1840);   // charged   -> 1840 kWh
    setReg(blocks[2], 33165, 0);
    setReg(blocks[2], 33166, 1610);   // discharged-> 1610 kWh

    MeasurementSet m;
    applyProfile(*findProfile("solis_rhi_hybrid"), blocks, 4, m, 1000);

    TEST_ASSERT_EQUAL_DOUBLE(8412.0, m.find(measurement_id::kEnergyTotal)->value);
    TEST_ASSERT_EQUAL_DOUBLE(21.4, m.find(measurement_id::kEnergyToday)->value);
    TEST_ASSERT_EQUAL_DOUBLE(318.0, m.find(measurement_id::kDcMppt1Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(5.1, m.find(measurement_id::kDcMppt1Current)->value);
    TEST_ASSERT_EQUAL_DOUBLE(296.0, m.find(measurement_id::kDcMppt2Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(2920.0, m.find(measurement_id::kDcPowerTotal)->value);
    TEST_ASSERT_EQUAL_DOUBLE(234.2, m.find(measurement_id::kAcL1Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(11.8, m.find(measurement_id::kAcL1Current)->value);
    TEST_ASSERT_EQUAL_DOUBLE(2760.0, m.find(measurement_id::kAcPowerTotal)->value);
    TEST_ASSERT_EQUAL_DOUBLE(37.1, m.find(measurement_id::kTemperature)->value);
    TEST_ASSERT_EQUAL_DOUBLE(49.98, m.find(measurement_id::kAcFrequency)->value);
    TEST_ASSERT_EQUAL_DOUBLE(51.2, m.find(measurement_id::kBatteryVoltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(64.0, m.find(measurement_id::kBatterySoc)->value);
    TEST_ASSERT_EQUAL_DOUBLE(1840.0, m.find(measurement_id::kBatteryEnergyCharged)->value);
    TEST_ASSERT_EQUAL_DOUBLE(1610.0, m.find(measurement_id::kBatteryEnergyDischarged)->value);
}

// Importing to charge the battery: AC power goes negative, and the s32 declaration is what keeps
// that from becoming a 4.3 gigawatt reading.
static void test_the_solis_ac_power_goes_negative_while_importing() {
    BlockData blocks[4];
    makeSolisBlocks(blocks);
    setReg(blocks[1], 33079, 0xFFFF);
    setReg(blocks[1], 33080, 0xF448);  // -3000

    MeasurementSet m;
    applyProfile(*findProfile("solis_rhi_hybrid"), blocks, 4, m, 1000);
    TEST_ASSERT_EQUAL_DOUBLE(-3000.0, m.find(measurement_id::kAcPowerTotal)->value);
}

// The channels the two sources would not agree on. Pinned absent so none can drift back in
// without somebody deciding to put it there.
static void test_the_solis_profile_publishes_nothing_the_sources_disputed() {
    BlockData blocks[4];
    makeSolisBlocks(blocks);
    setReg(blocks[2], 33134, 250);   // battery current -- signed, direction unstated
    setReg(blocks[2], 33135, 1);     // charge direction: the register source A trusts instead
    setReg(blocks[2], 33149, 0);
    setReg(blocks[2], 33150, 1500);  // battery power -- magnitude or signed, sources differ
    setReg(blocks[2], 33147, 900);   // house load -- no canonical id for it yet

    MeasurementSet m;
    applyProfile(*findProfile("solis_rhi_hybrid"), blocks, 4, m, 1000);

    TEST_ASSERT_NULL(m.find(measurement_id::kBatteryPower));
    TEST_ASSERT_NULL(m.find(measurement_id::kBatteryCurrent));
    TEST_ASSERT_NULL(m.find(measurement_id::kGridImportPower));
    TEST_ASSERT_NULL(m.find(measurement_id::kGridExportPower));
    TEST_ASSERT_NULL(m.find(measurement_id::kDcMppt3Voltage));
}

// Both setpoints are declared and both are dormant. The export limit is a plain FC06 row, so
// unlike the Deye mode row its only gate is `verified` -- which makes this the row that will go
// live first if a bench session ever confirms it, and the one most worth pinning as OFF.
static void test_the_solis_setpoints_are_declared_and_dormant() {
    const DeviceProfile* p = findProfile("solis_rhi_hybrid");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT32(2, p->writeCount);

    const WriteMapping* limit = nullptr;
    const WriteMapping* mode  = nullptr;
    for (size_t i = 0; i < p->writeCount; ++i) {
        if (p->writes[i].command == InverterCommandType::SetExportLimitWatts) {
            limit = &p->writes[i];
        }
        if (p->writes[i].command == InverterCommandType::SetBatteryOperatingMode) {
            mode = &p->writes[i];
        }
    }
    TEST_ASSERT_NOT_NULL(limit);
    TEST_ASSERT_NOT_NULL(mode);

    TEST_ASSERT_EQUAL_UINT16(43074, limit->address);
    TEST_ASSERT_EQUAL_DOUBLE(9900.0, limit->maximum);
    TEST_ASSERT_EQUAL_DOUBLE(100.0, limit->scale);  // raw is hundreds of watts
    TEST_ASSERT_FALSE(limit->verified);

    TEST_ASSERT_EQUAL_UINT16(43110, mode->address);
    TEST_ASSERT_EQUAL_UINT32(13, mode->optionCount);
    TEST_ASSERT_FALSE(mode->verified);
    // The vendor's own numbering, gaps and all -- 35 is "Self-Use", not option index 4.
    TEST_ASSERT_EQUAL_INT32(35, mode->options[4].value);
    TEST_ASSERT_EQUAL_STRING("Self-Use", mode->options[4].label);

    MockTransport  transport;
    ProfileOptions options;
    options.profile = p;
    ModbusProfileDriver driver(transport, options);
    const InverterCapabilities caps = driver.capabilities();
    TEST_ASSERT_FALSE(caps.canWrite(InverterCapability::SetExportLimit));
    TEST_ASSERT_FALSE(caps.canWrite(InverterCapability::SetBatteryOperatingMode));

    InverterCommand cmd;
    cmd.type         = InverterCommandType::SetExportLimitWatts;
    cmd.numericValue = 2500.0;
    TEST_ASSERT_EQUAL(CommandResult::Unsupported, driver.execute(cmd));
    TEST_ASSERT_TRUE(transport.writes.empty());
}

// --- the Sungrow SH residential hybrid profile ----------------------------------------------

namespace {
void makeSungrowBlocks(BlockData blocks[3]) {
    blocks[0] = {RegSpace::Input, 5000, 40, {}};
    blocks[1] = {RegSpace::Input, 5213, 32, {}};
    blocks[2] = {RegSpace::Input, 13000, 50, {}};
}
}  // namespace

static void test_the_sungrow_profile_decodes_a_realistic_frame() {
    BlockData blocks[3];
    makeSungrowBlocks(blocks);

    setReg(blocks[0], 5007, 415);    // temperature -> 41.5 °C
    setReg(blocks[0], 5010, 3450);   // MPPT1 V     -> 345.0 V
    setReg(blocks[0], 5011, 62);     // MPPT1 I     -> 6.2 A
    setReg(blocks[0], 5012, 3310);   // MPPT2 V     -> 331.0 V
    setReg(blocks[0], 5013, 58);     // MPPT2 I     -> 5.8 A
    // LOW WORD FIRST, like every 32-bit value on this family: the low half sits at the lower
    // address. Read the default way round these would decode as hundreds of megawatts.
    setReg(blocks[0], 5016, 4050);   // DC power    -> 4050 W (low half)
    setReg(blocks[0], 5017, 0);      //                        (high half)
    setReg(blocks[0], 5018, 2338);   // Phase A V   -> 233.8 V
    setReg(blocks[1], 5241, 5002);   // frequency   -> 50.02 Hz
    setReg(blocks[2], 13001, 187);   // E-today     -> 18.7 kWh
    setReg(blocks[2], 13002, 30150); // E-total     -> 3015.0 kWh (low half)
    setReg(blocks[2], 13003, 0);
    setReg(blocks[2], 13019, 5240);  // Batt V      -> 524.0 V
    setReg(blocks[2], 13022, 872);   // Batt SoC    -> 87.2 % (tenths!)
    setReg(blocks[2], 13024, 231);   // Batt temp   -> 23.1 °C
    setReg(blocks[2], 13030, 141);   // Phase A I   -> 14.1 A
    setReg(blocks[2], 13033, 3720);  // AC power    -> 3720 W (low half)
    setReg(blocks[2], 13034, 0);

    MeasurementSet m;
    applyProfile(*findProfile("sungrow_sh_hybrid"), blocks, 3, m, 1000);

    TEST_ASSERT_EQUAL_DOUBLE(41.5, m.find(measurement_id::kTemperature)->value);
    TEST_ASSERT_EQUAL_DOUBLE(345.0, m.find(measurement_id::kDcMppt1Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(6.2, m.find(measurement_id::kDcMppt1Current)->value);
    TEST_ASSERT_EQUAL_DOUBLE(331.0, m.find(measurement_id::kDcMppt2Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(4050.0, m.find(measurement_id::kDcPowerTotal)->value);
    TEST_ASSERT_EQUAL_DOUBLE(233.8, m.find(measurement_id::kAcL1Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(14.1, m.find(measurement_id::kAcL1Current)->value);
    TEST_ASSERT_EQUAL_DOUBLE(50.02, m.find(measurement_id::kAcFrequency)->value);
    TEST_ASSERT_EQUAL_DOUBLE(18.7, m.find(measurement_id::kEnergyToday)->value);
    TEST_ASSERT_EQUAL_DOUBLE(3015.0, m.find(measurement_id::kEnergyTotal)->value);
    TEST_ASSERT_EQUAL_DOUBLE(524.0, m.find(measurement_id::kBatteryVoltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(23.1, m.find(measurement_id::kBatteryTemperature)->value);
    TEST_ASSERT_EQUAL_DOUBLE(3720.0, m.find(measurement_id::kAcPowerTotal)->value);

    // Tenths of a percent in both sources. Read as whole percent this is a battery at 872%.
    TEST_ASSERT_EQUAL_DOUBLE(87.2, m.find(measurement_id::kBatterySoc)->value);
}

// The battery-power row is the one that needed two schema features at once, so it gets its own
// test: the pair is LOW word first, and the device's sign convention is the opposite of ours.
static void test_the_sungrow_battery_power_is_reordered_and_reoriented() {
    BlockData blocks[3];
    makeSungrowBlocks(blocks);

    // The device says -2000 W while CHARGING at 2 kW. Low word at the lower address.
    setReg(blocks[1], 5213, 0xF830);  // low half of -2000
    setReg(blocks[1], 5214, 0xFFFF);  // high half

    MeasurementSet m;
    applyProfile(*findProfile("sungrow_sh_hybrid"), blocks, 3, m, 1000);

    // Our convention is positive while charging, so the row's scale = -1 flips it.
    TEST_ASSERT_EQUAL_DOUBLE(2000.0, m.find(measurement_id::kBatteryPower)->value);

    // Discharging at 1.5 kW: the device reports +1500, we publish -1500.
    setReg(blocks[1], 5213, 1500);
    setReg(blocks[1], 5214, 0);
    MeasurementSet out;
    applyProfile(*findProfile("sungrow_sh_hybrid"), blocks, 3, out, 1000);
    TEST_ASSERT_EQUAL_DOUBLE(-1500.0, out.find(measurement_id::kBatteryPower)->value);
}

static void test_the_sungrow_setpoints_are_declared_and_dormant() {
    const DeviceProfile* p = findProfile("sungrow_sh_hybrid");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("Sungrow", p->manufacturer);
    TEST_ASSERT_EQUAL_UINT32(2, p->writeCount);

    for (size_t i = 0; i < p->writeCount; ++i) {
        TEST_ASSERT_FALSE(p->writes[i].verified);
    }

    const WriteMapping* mode = nullptr;
    for (size_t i = 0; i < p->writeCount; ++i) {
        if (p->writes[i].command == InverterCommandType::SetBatteryOperatingMode) {
            mode = &p->writes[i];
        }
    }
    TEST_ASSERT_NOT_NULL(mode);
    TEST_ASSERT_EQUAL_UINT16(13049, mode->address);
    TEST_ASSERT_EQUAL_UINT32(4, mode->optionCount);
    // 1 is absent from the vendor's numbering, and the gap is deliberate: option index 1 is
    // mode 2, not mode 1.
    TEST_ASSERT_EQUAL_INT32(2, mode->options[1].value);
    TEST_ASSERT_EQUAL_STRING("Forced mode", mode->options[1].label);

    MockTransport  transport;
    ProfileOptions options;
    options.profile = p;
    ModbusProfileDriver driver(transport, options);
    TEST_ASSERT_FALSE(driver.capabilities().canWrite(InverterCapability::SetExportLimit));
    TEST_ASSERT_FALSE(driver.capabilities().canWrite(InverterCapability::SetBatteryOperatingMode));
}

// --- the Huawei SUN2000 profile, and the sentinel that makes it honest ----------------------

namespace {
void makeHuaweiBlocks(BlockData blocks[4]) {
    blocks[0] = {RegSpace::Holding, 32016, 4, {}};
    blocks[1] = {RegSpace::Holding, 32064, 24, {}};
    blocks[2] = {RegSpace::Holding, 32106, 10, {}};
    blocks[3] = {RegSpace::Holding, 37760, 24, {}};
}
}  // namespace

static void test_the_huawei_profile_decodes_a_realistic_frame() {
    BlockData blocks[4];
    makeHuaweiBlocks(blocks);

    setReg(blocks[0], 32016, 3600);   // PV1 V   -> 360.0 V   (gain 10)
    setReg(blocks[0], 32017, 640);    // PV1 I   -> 6.40 A    (gain 100)
    setReg(blocks[0], 32018, 3480);   // PV2 V   -> 348.0 V
    setReg(blocks[1], 32064, 0);
    setReg(blocks[1], 32065, 4400);   // input   -> 4400 W
    setReg(blocks[1], 32069, 2331);   // Phase A -> 233.1 V
    setReg(blocks[1], 32072, 0);
    setReg(blocks[1], 32073, 18600);  // current -> 18.6 A    (gain 1000)
    setReg(blocks[1], 32080, 0);
    setReg(blocks[1], 32081, 4280);   // active  -> 4280 W
    setReg(blocks[1], 32085, 5001);   // freq    -> 50.01 Hz  (gain 100)
    setReg(blocks[1], 32087, 386);    // temp    -> 38.6 °C
    setReg(blocks[2], 32106, 0);
    setReg(blocks[2], 32107, 61200);  // total   -> 612.00 kWh (gain 100)
    setReg(blocks[2], 32114, 0);
    setReg(blocks[2], 32115, 2140);   // today   -> 21.40 kWh
    setReg(blocks[3], 37760, 763);    // SoC     -> 76.3 %    (gain 10)
    setReg(blocks[3], 37763, 3580);   // batt V  -> 358.0 V

    MeasurementSet m;
    applyProfile(*findProfile("huawei_sun2000"), blocks, 4, m, 1000);

    TEST_ASSERT_EQUAL_DOUBLE(360.0, m.find(measurement_id::kDcMppt1Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(6.4, m.find(measurement_id::kDcMppt1Current)->value);
    TEST_ASSERT_EQUAL_DOUBLE(348.0, m.find(measurement_id::kDcMppt2Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(4400.0, m.find(measurement_id::kDcPowerTotal)->value);
    TEST_ASSERT_EQUAL_DOUBLE(233.1, m.find(measurement_id::kAcL1Voltage)->value);
    // Milliamp resolution: read with the 0.01 that fits most vendors, this would be 0.186 A.
    TEST_ASSERT_EQUAL_DOUBLE(18.6, m.find(measurement_id::kAcL1Current)->value);
    TEST_ASSERT_EQUAL_DOUBLE(4280.0, m.find(measurement_id::kAcPowerTotal)->value);
    TEST_ASSERT_EQUAL_DOUBLE(50.01, m.find(measurement_id::kAcFrequency)->value);
    TEST_ASSERT_EQUAL_DOUBLE(38.6, m.find(measurement_id::kTemperature)->value);
    TEST_ASSERT_EQUAL_DOUBLE(612.0, m.find(measurement_id::kEnergyTotal)->value);
    TEST_ASSERT_EQUAL_DOUBLE(21.4, m.find(measurement_id::kEnergyToday)->value);
    TEST_ASSERT_EQUAL_DOUBLE(76.3, m.find(measurement_id::kBatterySoc)->value);
    TEST_ASSERT_EQUAL_DOUBLE(358.0, m.find(measurement_id::kBatteryVoltage)->value);
}

// A sleeping inverter with no battery. Every one of these registers holds its width's sentinel,
// and every one must come out ABSENT rather than as a number -- 3276.7 °C and a 6553.5 % battery
// are both the shape of a real reading, which is what makes them dangerous.
static void test_the_huawei_sentinels_leave_channels_absent_not_absurd() {
    BlockData blocks[4];
    makeHuaweiBlocks(blocks);

    setReg(blocks[0], 32016, 0x7FFF);  // PV1 voltage, s16
    setReg(blocks[0], 32017, 0x7FFF);
    setReg(blocks[1], 32087, 0x7FFF);  // temperature, s16
    setReg(blocks[1], 32069, 0xFFFF);  // phase voltage, u16
    setReg(blocks[1], 32085, 0xFFFF);  // frequency, u16
    setReg(blocks[1], 32064, 0x7FFF);  // input power, s32
    setReg(blocks[1], 32065, 0xFFFF);
    setReg(blocks[2], 32106, 0xFFFF);  // lifetime yield, u32
    setReg(blocks[2], 32107, 0xFFFF);
    setReg(blocks[3], 37760, 0xFFFF);  // battery SoC on an inverter with no battery
    setReg(blocks[3], 37763, 0xFFFF);

    MeasurementSet m;
    applyProfile(*findProfile("huawei_sun2000"), blocks, 4, m, 1000);

    TEST_ASSERT_NULL(m.find(measurement_id::kDcMppt1Voltage));
    TEST_ASSERT_NULL(m.find(measurement_id::kDcMppt1Current));
    TEST_ASSERT_NULL(m.find(measurement_id::kTemperature));
    TEST_ASSERT_NULL(m.find(measurement_id::kAcL1Voltage));
    TEST_ASSERT_NULL(m.find(measurement_id::kAcFrequency));
    TEST_ASSERT_NULL(m.find(measurement_id::kDcPowerTotal));
    TEST_ASSERT_NULL(m.find(measurement_id::kEnergyTotal));
    TEST_ASSERT_NULL(m.find(measurement_id::kBatterySoc));
    TEST_ASSERT_NULL(m.find(measurement_id::kBatteryVoltage));
}

// The sentinel must not swallow a legitimate reading that happens to sit next to it. 0x7FFE is
// one below the s16 sentinel and decodes normally; a guard written as ">=" would eat it.
static void test_the_huawei_sentinel_does_not_swallow_its_neighbour() {
    BlockData blocks[4];
    makeHuaweiBlocks(blocks);
    setReg(blocks[1], 32087, 0x7FFE);  // 32766 raw -> 3276.6 °C

    MeasurementSet m;
    applyProfile(*findProfile("huawei_sun2000"), blocks, 4, m, 1000);
    const auto* t = m.find(measurement_id::kTemperature);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_DOUBLE(3276.6, t->value);
}

// Battery power is held back here too, and for a third distinct reason -- see the profile.
static void test_the_huawei_profile_holds_back_battery_power() {
    BlockData blocks[4];
    makeHuaweiBlocks(blocks);
    setReg(blocks[3], 37765, 0);
    setReg(blocks[3], 37766, 2500);

    MeasurementSet m;
    applyProfile(*findProfile("huawei_sun2000"), blocks, 4, m, 1000);
    TEST_ASSERT_NULL(m.find(measurement_id::kBatteryPower));
}

static void test_the_huawei_setpoint_is_declared_and_dormant() {
    const DeviceProfile* p = findProfile("huawei_sun2000");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("Huawei", p->manufacturer);
    TEST_ASSERT_EQUAL_UINT32(1, p->writeCount);

    const WriteMapping& w = p->writes[0];
    TEST_ASSERT_EQUAL(InverterCommandType::SetActivePowerLimitPercent, w.command);
    TEST_ASSERT_EQUAL_UINT16(40125, w.address);
    TEST_ASSERT_EQUAL_UINT8(1, w.words);  // FC06 can serve it, once verified
    TEST_ASSERT_EQUAL_DOUBLE(0.1, w.scale);
    TEST_ASSERT_FALSE(w.verified);

    MockTransport  transport;
    ProfileOptions options;
    options.profile = p;
    ModbusProfileDriver driver(transport, options);
    TEST_ASSERT_FALSE(driver.capabilities().canWrite(InverterCapability::SetActivePowerLimit));
}

// --- the GoodWe ET hybrid profile -----------------------------------------------------------

namespace {
void makeGoodweBlocks(BlockData blocks[2]) {
    blocks[0] = {RegSpace::Holding, 35100, 125, {}};
    blocks[1] = {RegSpace::Holding, 37000, 24, {}};
}
}  // namespace

static void test_the_goodwe_profile_decodes_a_realistic_frame() {
    BlockData blocks[2];
    makeGoodweBlocks(blocks);

    setReg(blocks[0], 35103, 3120);   // PV1 V  -> 312.0 V
    setReg(blocks[0], 35104, 55);     // PV1 I  -> 5.5 A
    setReg(blocks[0], 35105, 0);
    setReg(blocks[0], 35106, 1716);   // PV1 W  -> 1716 W
    setReg(blocks[0], 35107, 2980);   // PV2 V  -> 298.0 V
    setReg(blocks[0], 35121, 2295);   // grid V -> 229.5 V
    setReg(blocks[0], 35122, 121);    // grid I -> 12.1 A
    setReg(blocks[0], 35123, 4999);   // freq   -> 49.99 Hz
    setReg(blocks[0], 35138, 2810);   // AC W   -> 2810 W
    setReg(blocks[0], 35176, 402);    // temp   -> 40.2 °C
    setReg(blocks[0], 35180, 3520);   // batt V -> 352.0 V
    setReg(blocks[0], 35191, 0);
    setReg(blocks[0], 35192, 52340);  // E-tot  -> 5234.0 kWh
    setReg(blocks[0], 35193, 0);
    setReg(blocks[0], 35194, 168);    // E-day  -> 16.8 kWh
    setReg(blocks[1], 37003, 254);    // batt T -> 25.4 °C
    setReg(blocks[1], 37007, 68);     // SoC    -> 68 %

    MeasurementSet m;
    applyProfile(*findProfile("goodwe_et_hybrid"), blocks, 2, m, 1000);

    TEST_ASSERT_EQUAL_DOUBLE(312.0, m.find(measurement_id::kDcMppt1Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(5.5, m.find(measurement_id::kDcMppt1Current)->value);
    TEST_ASSERT_EQUAL_DOUBLE(1716.0, m.find(measurement_id::kDcMppt1Power)->value);
    TEST_ASSERT_EQUAL_DOUBLE(298.0, m.find(measurement_id::kDcMppt2Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(229.5, m.find(measurement_id::kAcL1Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(12.1, m.find(measurement_id::kAcL1Current)->value);
    TEST_ASSERT_EQUAL_DOUBLE(49.99, m.find(measurement_id::kAcFrequency)->value);
    TEST_ASSERT_EQUAL_DOUBLE(2810.0, m.find(measurement_id::kAcPowerTotal)->value);
    TEST_ASSERT_EQUAL_DOUBLE(40.2, m.find(measurement_id::kTemperature)->value);
    TEST_ASSERT_EQUAL_DOUBLE(352.0, m.find(measurement_id::kBatteryVoltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(5234.0, m.find(measurement_id::kEnergyTotal)->value);
    TEST_ASSERT_EQUAL_DOUBLE(16.8, m.find(measurement_id::kEnergyToday)->value);
    TEST_ASSERT_EQUAL_DOUBLE(25.4, m.find(measurement_id::kBatteryTemperature)->value);
    TEST_ASSERT_EQUAL_DOUBLE(68.0, m.find(measurement_id::kBatterySoc)->value);
}

// The same sentinel convention as Huawei, on a different vendor -- which is the point of having
// put it in the schema rather than in one profile's driver.
static void test_the_goodwe_sentinels_leave_channels_absent() {
    BlockData blocks[2];
    makeGoodweBlocks(blocks);
    setReg(blocks[0], 35103, 0xFFFF);  // PV1 voltage
    setReg(blocks[0], 35176, 0x7FFF);  // radiator temperature
    setReg(blocks[0], 35191, 0xFFFF);  // lifetime energy, u32
    setReg(blocks[0], 35192, 0xFFFF);
    setReg(blocks[1], 37007, 0xFFFF);  // SoC on an inverter with no battery

    MeasurementSet m;
    applyProfile(*findProfile("goodwe_et_hybrid"), blocks, 2, m, 1000);

    TEST_ASSERT_NULL(m.find(measurement_id::kDcMppt1Voltage));
    TEST_ASSERT_NULL(m.find(measurement_id::kTemperature));
    TEST_ASSERT_NULL(m.find(measurement_id::kEnergyTotal));
    TEST_ASSERT_NULL(m.find(measurement_id::kBatterySoc));
}

// Read-only by sourcing, not by oversight: the public protocol document is titled "Read Only",
// so nothing is declared -- not even a dormant row.
static void test_the_goodwe_profile_declares_no_setpoints() {
    const DeviceProfile* p = findProfile("goodwe_et_hybrid");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("GoodWe", p->manufacturer);
    TEST_ASSERT_EQUAL_UINT32(0, p->writeCount);

    MockTransport  transport;
    ProfileOptions options;
    options.profile = p;
    ModbusProfileDriver driver(transport, options);
    TEST_ASSERT_TRUE(driver.capabilities().isReadOnly());
}

// Battery power and grid power are both held back, for reasons recorded in the profile.
static void test_the_goodwe_profile_holds_back_the_unsourced_channels() {
    BlockData blocks[2];
    makeGoodweBlocks(blocks);
    setReg(blocks[0], 35181, 90);    // battery current, signed, direction unstated
    setReg(blocks[0], 35182, 0);
    setReg(blocks[0], 35183, 3100);  // battery power, same
    setReg(blocks[0], 35140, 500);   // grid power: one signed bidirectional value

    MeasurementSet m;
    applyProfile(*findProfile("goodwe_et_hybrid"), blocks, 2, m, 1000);

    TEST_ASSERT_NULL(m.find(measurement_id::kBatteryPower));
    TEST_ASSERT_NULL(m.find(measurement_id::kBatteryCurrent));
    TEST_ASSERT_NULL(m.find(measurement_id::kGridImportPower));
    TEST_ASSERT_NULL(m.find(measurement_id::kGridExportPower));
}

// The MIC and MIN TL-X share one register layout and differ only in tracker count. That is the
// whole reason they are two profiles, so it is what this pins: the same frame decodes the same
// way on both, and only the MIN publishes a second string.
static void test_the_mic_and_min_profiles_share_a_layout_and_differ_in_strings() {
    const DeviceProfile* mic = findProfile("mic_tl_x");
    const DeviceProfile* min = findProfile("min_tl_x");
    TEST_ASSERT_NOT_NULL(mic);
    TEST_ASSERT_NOT_NULL(min);
    TEST_ASSERT_EQUAL_UINT8(1, mic->mpptCount);
    TEST_ASSERT_EQUAL_UINT8(2, min->mpptCount);

    BlockData blocks[2];
    blocks[0] = {RegSpace::Input, 0, 125, {}};
    blocks[1] = {RegSpace::Holding, 0, 89, {}};

    setReg(blocks[0], 3, 3105);    // PV1 V -> 310.5 V
    setReg(blocks[0], 35, 0);
    setReg(blocks[0], 36, 11880);  // AC    -> 1188.0 W
    setReg(blocks[0], 7, 2980);    // PV2 V -> 298.0 V, only the MIN maps this
    setReg(blocks[0], 9, 0);
    setReg(blocks[0], 10, 9400);   // PV2 W -> 940.0 W

    MeasurementSet asMic;
    applyProfile(*mic, blocks, 2, asMic, 1000);
    MeasurementSet asMin;
    applyProfile(*min, blocks, 2, asMin, 1000);

    // Shared rows decode identically -- same layout, same scaling.
    TEST_ASSERT_EQUAL_DOUBLE(310.5, asMic.find(measurement_id::kDcMppt1Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(310.5, asMin.find(measurement_id::kDcMppt1Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(1188.0, asMic.find(measurement_id::kAcPowerTotal)->value);
    TEST_ASSERT_EQUAL_DOUBLE(1188.0, asMin.find(measurement_id::kAcPowerTotal)->value);

    // The second string is the difference. On the MIC it stays ABSENT rather than publishing the
    // zero a one-tracker inverter would report there -- which is why widening the MIC profile
    // instead of adding this one would have been wrong.
    TEST_ASSERT_NULL(asMic.find(measurement_id::kDcMppt2Voltage));
    TEST_ASSERT_NULL(asMic.find(measurement_id::kDcMppt2Power));
    TEST_ASSERT_EQUAL_DOUBLE(298.0, asMin.find(measurement_id::kDcMppt2Voltage)->value);
    TEST_ASSERT_EQUAL_DOUBLE(940.0, asMin.find(measurement_id::kDcMppt2Power)->value);

    // And the MIN carries the same dormant curtailment row, on its own flag.
    TEST_ASSERT_EQUAL_UINT32(1, min->writeCount);
    TEST_ASSERT_EQUAL_UINT16(3, min->writes[0].address);
    TEST_ASSERT_FALSE(min->writes[0].verified);
}

// Word order is a per-FAMILY property, and this pins it per family rather than per row.
//
// This is the test that would have caught the bug it exists because of. The Sungrow profile
// originally declared low-word-first on its battery-power row alone, with a comment asserting
// that row was "the opposite of every other 32-bit value in this file" -- which was false: the
// source declares `swap: word` on all 21 of its 32-bit sensors. Six rows were therefore out by a
// factor of 65536, and the decode test did NOT catch it, because that test was written from the
// profile and so agreed with the mistake.
//
// A frame-decoding test can only confirm the profile matches itself. Asserting the CONVENTION
// independently is what catches a row that was missed.
static void test_the_declared_word_order_is_consistent_within_each_family() {
    struct Family {
        const char* id;
        bool        lowWordFirst;
        const char* why;
    };
    // Verified against each profile's own sources, mechanically, not by reading its comments.
    static const Family kFamilies[] = {
        {"sungrow_sh_hybrid", true, "all 21 of source B's 32-bit sensors carry swap: word"},
        {"deye_sun_xk_sg", true, "source A is little-endian; source B defaults low_word_first"},
        {"solis_rhi_hybrid", false, "source B lists [low, high], so high sits at the lower address"},
        {"huawei_sun2000", false, "the library decodes with a big-endian struct format"},
        {"goodwe_et_hybrid", false, "read_bytes4 is int.from_bytes(..., byteorder='big')"},
        {"mic_tl_x", false, "growatt-local assembles (value << 16) | next"},
        {"min_tl_x", false, "same map and same assembly as the MIC"},
    };

    for (const Family& f : kFamilies) {
        const DeviceProfile* p = findProfile(f.id);
        TEST_ASSERT_NOT_NULL_MESSAGE(p, f.id);
        size_t checked = 0;
        for (size_t i = 0; i < p->mappingCount; ++i) {
            const RegisterMapping& m = p->mappings[i];
            if (m.words != 2) {
                continue;  // word order is meaningless for a single register
            }
            ++checked;
            TEST_ASSERT_EQUAL_MESSAGE(f.lowWordFirst, m.lowWordFirst, f.why);
        }
        // A family with no 32-bit rows would pass this vacuously, so require at least one.
        TEST_ASSERT_TRUE_MESSAGE(checked > 0, f.id);
    }
}

static void test_an_unverified_row_is_neither_advertised_nor_executed() {
    MockTransport transport;
    echoWrites(transport);
    DeviceProfile profile = profileWith(kUnverifiedWrites, 1);
    ProfileOptions options;
    options.profile = &profile;
    ModbusProfileDriver driver(transport, options);

    // Held by VALUE. capabilities() returns by value, so binding a reference into the result
    // leaves it dangling the moment the full expression ends -- the assertion below then read
    // freed stack, which happens to still hold the right answer often enough to pass. Caught by
    // -Wdangling-gsl in the native build, where a second incremental `pio test` had been hiding
    // it (audit, 2026-07-29).
    const InverterCapabilities caps = driver.capabilities();
    TEST_ASSERT_FALSE(caps.canWrite(InverterCapability::SetActivePowerLimit));
    TEST_ASSERT_FALSE(
        caps.numeric[static_cast<size_t>(InverterCommandType::SetActivePowerLimitPercent)]
            .writable);

    InverterCommand cmd;
    cmd.type         = InverterCommandType::SetActivePowerLimitPercent;
    cmd.numericValue = 60.0;
    TEST_ASSERT_EQUAL(CommandResult::Unsupported, driver.execute(cmd));
    // Nothing on the bus. `verified` means a person confirmed the register on hardware, and
    // until then the row is documentation.
    TEST_ASSERT_EQUAL_UINT32(0, transport.writes.size());
}

static void test_a_verified_row_becomes_an_advertised_setpoint() {
    MockTransport  transport;
    DeviceProfile profile = profileWith(kTestWrites, 1);
    ProfileOptions options;
    options.profile = &profile;
    ModbusProfileDriver driver(transport, options);

    const auto caps = driver.capabilities();
    TEST_ASSERT_TRUE(caps.canWrite(InverterCapability::SetActivePowerLimit));
    const auto& n = caps.numeric[static_cast<size_t>(
        InverterCommandType::SetActivePowerLimitPercent)];
    TEST_ASSERT_TRUE(n.writable);
    // The bounds come from the row, so the dispatcher enforces what the register accepts rather
    // than a number typed twice.
    TEST_ASSERT_EQUAL_DOUBLE(0.0, n.minimum);
    TEST_ASSERT_EQUAL_DOUBLE(100.0, n.maximum);
}

static void test_a_verified_row_writes_the_register_it_names() {
    MockTransport transport;
    echoWrites(transport);
    DeviceProfile profile = profileWith(kTestWrites, 1);
    ProfileOptions options;
    options.unitId  = 2;
    options.profile = &profile;
    ModbusProfileDriver driver(transport, options);

    InverterCommand cmd;
    cmd.type         = InverterCommandType::SetActivePowerLimitPercent;
    cmd.numericValue = 60.0;
    TEST_ASSERT_EQUAL(CommandResult::Ok, driver.execute(cmd));

    // unit 2, FC06, register 3, value 60 -- the frame the profile row describes, not a frame
    // that happens to be accepted.
    TEST_ASSERT_EQUAL_UINT32(1, transport.writes.size());
    const auto& f = transport.writes.back();
    TEST_ASSERT_EQUAL_UINT8(0x02, f[0]);
    TEST_ASSERT_EQUAL_UINT8(0x06, f[1]);
    TEST_ASSERT_EQUAL_UINT16(3, (f[2] << 8) | f[3]);
    TEST_ASSERT_EQUAL_UINT16(60, (f[4] << 8) | f[5]);
}

// A mode row writes the option's OWN value, not the position it sits in.
static void test_a_mode_row_writes_the_declared_value_not_the_selection_index() {
    MockTransport transport;
    echoWrites(transport);
    DeviceProfile profile = profileWith(kModeWrites, 1);
    ProfileOptions options;
    options.unitId  = 1;
    options.profile = &profile;
    ModbusProfileDriver driver(transport, options);

    // "External EMS" is option index 2 and value 3. Sending 2 would select "Forced" -- on a real
    // device, the difference between following a house battery plan and handing control to
    // something that is not there.
    InverterCommand cmd;
    cmd.type      = InverterCommandType::SetBatteryOperatingMode;
    cmd.enumValue = 3;
    TEST_ASSERT_EQUAL(CommandResult::Ok, driver.execute(cmd));

    TEST_ASSERT_EQUAL_UINT32(1, transport.writes.size());
    const auto& f = transport.writes.back();
    TEST_ASSERT_EQUAL_UINT8(0x06, f[1]);
    TEST_ASSERT_EQUAL_UINT16(13049, (f[2] << 8) | f[3]);
    TEST_ASSERT_EQUAL_UINT16(3, (f[4] << 8) | f[5]);
}

static void test_a_mode_row_advertises_its_options_as_a_capability() {
    MockTransport transport;
    DeviceProfile profile = profileWith(kModeWrites, 1);
    ProfileOptions options;
    options.profile = &profile;
    ModbusProfileDriver driver(transport, options);

    const InverterCapabilities caps = driver.capabilities();
    TEST_ASSERT_TRUE(caps.canWrite(InverterCapability::SetBatteryOperatingMode));

    const EnumCapability& ec =
        caps.enums[static_cast<size_t>(InverterCommandType::SetBatteryOperatingMode)];
    TEST_ASSERT_TRUE(ec.supported);
    TEST_ASSERT_TRUE(ec.writable);
    TEST_ASSERT_EQUAL_UINT32(3, ec.optionCount);
    TEST_ASSERT_EQUAL_STRING("Self-consumption", ec.options[0].label);
    TEST_ASSERT_TRUE(ec.accepts(0));
    TEST_ASSERT_TRUE(ec.accepts(3));
    // The gaps are not modes. 1 sits between two declared values and is exactly the number an
    // off-by-one would produce.
    TEST_ASSERT_FALSE(ec.accepts(1));
    TEST_ASSERT_FALSE(ec.accepts(4));

    // A mode row publishes no numeric range -- there is nothing to interpolate -- so nothing
    // should read it as an unbounded numeric setpoint.
    TEST_ASSERT_FALSE(
        caps.numeric[static_cast<size_t>(InverterCommandType::SetBatteryOperatingMode)].writable);
}

static void test_a_mode_the_device_never_declared_never_reaches_the_bus() {
    MockTransport transport;
    echoWrites(transport);
    DeviceProfile profile = profileWith(kModeWrites, 1);
    ProfileOptions options;
    options.profile = &profile;
    ModbusProfileDriver driver(transport, options);

    InverterCommand cmd;
    cmd.type      = InverterCommandType::SetBatteryOperatingMode;
    cmd.enumValue = 1;  // between two declared values
    TEST_ASSERT_EQUAL(CommandResult::OutOfRange, driver.execute(cmd));
    TEST_ASSERT_TRUE(transport.writes.empty());

    // And a mode command with no selection at all is rejected rather than defaulted to the first
    // option, which would silently switch a battery to whatever happens to be listed first.
    InverterCommand empty;
    empty.type = InverterCommandType::SetBatteryOperatingMode;
    TEST_ASSERT_EQUAL(CommandResult::Rejected, driver.execute(empty));
    TEST_ASSERT_TRUE(transport.writes.empty());
}

// A mode row with no modes is documentation, not a control surface -- an empty dropdown in Home
// Assistant and a register nothing can range-check.
static void test_a_mode_row_without_options_is_not_a_control_surface() {
    MockTransport transport;
    echoWrites(transport);
    DeviceProfile profile = profileWith(kModeWritesNoOptions, 1);
    ProfileOptions options;
    options.profile = &profile;
    ModbusProfileDriver driver(transport, options);

    TEST_ASSERT_FALSE(driver.capabilities().canWrite(InverterCapability::SetBatteryOperatingMode));

    InverterCommand cmd;
    cmd.type      = InverterCommandType::SetBatteryOperatingMode;
    cmd.enumValue = 0;
    TEST_ASSERT_EQUAL(CommandResult::Unsupported, driver.execute(cmd));
    TEST_ASSERT_TRUE(transport.writes.empty());
}

static void test_a_value_outside_the_row_bounds_never_reaches_the_bus() {
    MockTransport transport;
    echoWrites(transport);
    DeviceProfile profile = profileWith(kTestWrites, 1);
    ProfileOptions options;
    options.profile = &profile;
    ModbusProfileDriver driver(transport, options);

    InverterCommand cmd;
    cmd.type         = InverterCommandType::SetActivePowerLimitPercent;
    cmd.numericValue = 150.0;
    TEST_ASSERT_EQUAL(CommandResult::OutOfRange, driver.execute(cmd));
    // Refused before the frame is built. An inverter asked for 150% is an inverter asked
    // something meaningless, and finding out from its exception reply is finding out too late.
    TEST_ASSERT_EQUAL_UINT32(0, transport.writes.size());
}

static void test_a_row_needing_write_multiple_is_refused_not_faked() {
    MockTransport  transport;
    DeviceProfile profile = profileWith(kMultiWordWrites, 1);
    ProfileOptions options;
    options.profile = &profile;
    ModbusProfileDriver driver(transport, options);

    // The client speaks FC06 only. Sending a single-register write for a two-register row would
    // put half a value in the device, which is worse than declining.
    TEST_ASSERT_FALSE(driver.capabilities().canWrite(InverterCapability::SetActivePowerLimit));
    InverterCommand cmd;
    cmd.type         = InverterCommandType::SetActivePowerLimitPercent;
    cmd.numericValue = 60.0;
    TEST_ASSERT_EQUAL(CommandResult::Unsupported, driver.execute(cmd));
    TEST_ASSERT_EQUAL_UINT32(0, transport.writes.size());
}

static void test_a_device_that_refuses_is_rejected_not_reported_as_a_fault() {
    MockTransport transport;
    transport.setResponder([](const std::vector<uint8_t>& req, std::vector<uint8_t>& reply) {
        if (req.size() < 2) {
            return false;
        }
        reply = {req[0], static_cast<uint8_t>(req[1] | 0x80), 0x02};  // illegal data address
        const uint16_t crc = modbus::crc16(reply.data(), reply.size());
        reply.push_back(static_cast<uint8_t>(crc & 0xFF));
        reply.push_back(static_cast<uint8_t>(crc >> 8));
        return true;
    });
    DeviceProfile profile = profileWith(kTestWrites, 1);
    ProfileOptions options;
    options.profile = &profile;
    ModbusProfileDriver driver(transport, options);

    InverterCommand cmd;
    cmd.type         = InverterCommandType::SetActivePowerLimitPercent;
    cmd.numericValue = 60.0;
    // The bus worked perfectly and the device said no. Reporting that as a driver fault sends
    // somebody to look at wiring.
    TEST_ASSERT_EQUAL(CommandResult::Rejected, driver.execute(cmd));
}

// The active-power-limit row is research, not a control surface: it stays dormant until a
// bench session sets verified = true AND the driver grows a write path (execute() still
// returns Unsupported). Both gates are asserted here so neither can be dropped unnoticed.
static void test_the_mic_power_limit_write_row_is_declared_but_dormant() {
    const DeviceProfile* mic = findProfile("mic_tl_x");
    TEST_ASSERT_NOT_NULL(mic);
    TEST_ASSERT_EQUAL_UINT32(1, mic->writeCount);

    const WriteMapping& w = mic->writes[0];
    TEST_ASSERT_EQUAL(InverterCommandType::SetActivePowerLimitPercent, w.command);
    TEST_ASSERT_EQUAL(RegSpace::Holding, w.space);
    TEST_ASSERT_EQUAL_UINT16(3, w.address);
    TEST_ASSERT_EQUAL_UINT8(1, w.words);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, w.minimum);
    // Deliberately 100, not 255: some revisions read 255 as "limit disabled", and a percentage
    // that silently means "off" needs explicit handling, not a widened bound.
    TEST_ASSERT_EQUAL_DOUBLE(100.0, w.maximum);
    TEST_ASSERT_FALSE(w.verified);

    // And dormant in effect, not just on paper. This used to assert descriptor().supportsWrite
    // was false, which stopped meaning anything once the driver grew a write path: that flag
    // answers "can this driver ever write", the honest answer to which is now yes. What the
    // shipped profile must guarantee is narrower and more useful -- a device configured with
    // THIS map advertises no setpoint and refuses the command.
    MockTransport transport;
    ProfileOptions options;
    options.profile = mic;
    ModbusProfileDriver driver(transport, options);

    const InverterCapabilities caps = driver.capabilities();
    TEST_ASSERT_FALSE(caps.canWrite(InverterCapability::SetActivePowerLimit));
    TEST_ASSERT_FALSE(
        caps.numeric[static_cast<size_t>(InverterCommandType::SetActivePowerLimitPercent)]
            .writable);

    InverterCommand command;
    command.type         = InverterCommandType::SetActivePowerLimitPercent;
    command.numericValue = 50.0;
    TEST_ASSERT_EQUAL(CommandResult::Unsupported, driver.execute(command));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_soc_is_decoded_as_a_plain_percent);
    RUN_TEST(test_voltage_scales_by_a_tenth);
    RUN_TEST(test_temperature_is_signed);
    RUN_TEST(test_battery_power_is_a_signed_32bit_pair);
    RUN_TEST(test_pv_power_is_an_unsigned_32bit_pair);
    RUN_TEST(test_a_biased_register_decodes_through_its_offset);
    RUN_TEST(test_a_negative_scale_negates_the_reading);
    RUN_TEST(test_a_low_word_first_pair_decodes_the_other_way_round);
    RUN_TEST(test_a_low_word_first_pair_still_sign_extends);
    RUN_TEST(test_a_register_in_an_unread_block_is_left_undeclared);
    RUN_TEST(test_find_register_reports_out_of_range);
    RUN_TEST(test_a_full_poll_decodes_measurements_over_the_bus);
    RUN_TEST(test_silence_is_a_timeout);
    RUN_TEST(test_all_blocks_refused_is_not_registered);
    RUN_TEST(test_one_refused_block_does_not_sink_the_poll);
    RUN_TEST(test_a_refused_block_outranks_a_timeout_in_the_outcome);
    RUN_TEST(test_a_corrupt_reply_is_reported_as_a_checksum_error);
    RUN_TEST(test_a_corrupt_block_is_counted_even_when_the_poll_succeeds);
    RUN_TEST(test_a_silent_probe_block_moves_no_bus_counter);
    RUN_TEST(test_a_silent_mapped_block_is_still_counted);
    RUN_TEST(test_a_refused_block_moves_no_bus_counter);
    RUN_TEST(test_an_intact_reply_from_another_unit_is_not_a_checksum_error);
    RUN_TEST(test_a_sustained_noise_trickle_hits_the_transaction_deadline);
    RUN_TEST(test_execute_is_unsupported_read_only);
    RUN_TEST(test_begin_configures_the_serial_line);
    RUN_TEST(test_a_profile_declared_serial_overrides_the_descriptor_default);
    RUN_TEST(test_the_profile_registry_finds_sph_and_rejects_unknown_ids);
    RUN_TEST(test_the_profile_option_enumerates_every_compiled_profile);
    RUN_TEST(test_the_mic_profile_describes_a_single_phase_single_tracker_string_inverter);
    RUN_TEST(test_the_mic_profile_probes_3000_but_publishes_nothing_from_it);
    RUN_TEST(test_the_mic_profile_decodes_a_realistic_frame);
    RUN_TEST(test_the_mic_profile_converts_half_seconds_to_hours);
    RUN_TEST(test_the_deye_profile_describes_a_single_phase_two_string_hybrid);
    RUN_TEST(test_the_deye_profile_decodes_a_realistic_frame);
    RUN_TEST(test_the_deye_battery_temperature_survives_a_freezing_morning);
    RUN_TEST(test_the_deye_ac_power_goes_negative_while_importing);
    RUN_TEST(test_the_deye_profile_publishes_nothing_it_could_not_source);
    RUN_TEST(test_the_deye_mode_row_is_declared_and_refused_for_two_reasons);
    RUN_TEST(test_the_solis_profile_decodes_a_realistic_frame);
    RUN_TEST(test_the_solis_ac_power_goes_negative_while_importing);
    RUN_TEST(test_the_solis_profile_publishes_nothing_the_sources_disputed);
    RUN_TEST(test_the_solis_setpoints_are_declared_and_dormant);
    RUN_TEST(test_the_sungrow_profile_decodes_a_realistic_frame);
    RUN_TEST(test_the_sungrow_battery_power_is_reordered_and_reoriented);
    RUN_TEST(test_the_sungrow_setpoints_are_declared_and_dormant);
    RUN_TEST(test_the_huawei_profile_decodes_a_realistic_frame);
    RUN_TEST(test_the_huawei_sentinels_leave_channels_absent_not_absurd);
    RUN_TEST(test_the_huawei_sentinel_does_not_swallow_its_neighbour);
    RUN_TEST(test_the_huawei_profile_holds_back_battery_power);
    RUN_TEST(test_the_huawei_setpoint_is_declared_and_dormant);
    RUN_TEST(test_the_goodwe_profile_decodes_a_realistic_frame);
    RUN_TEST(test_the_goodwe_sentinels_leave_channels_absent);
    RUN_TEST(test_the_goodwe_profile_declares_no_setpoints);
    RUN_TEST(test_the_goodwe_profile_holds_back_the_unsourced_channels);
    RUN_TEST(test_the_mic_and_min_profiles_share_a_layout_and_differ_in_strings);
    RUN_TEST(test_the_declared_word_order_is_consistent_within_each_family);
    RUN_TEST(test_an_unverified_row_is_neither_advertised_nor_executed);
    RUN_TEST(test_a_verified_row_becomes_an_advertised_setpoint);
    RUN_TEST(test_a_verified_row_writes_the_register_it_names);
    RUN_TEST(test_a_mode_row_writes_the_declared_value_not_the_selection_index);
    RUN_TEST(test_a_mode_row_advertises_its_options_as_a_capability);
    RUN_TEST(test_a_mode_the_device_never_declared_never_reaches_the_bus);
    RUN_TEST(test_a_mode_row_without_options_is_not_a_control_surface);
    RUN_TEST(test_a_value_outside_the_row_bounds_never_reaches_the_bus);
    RUN_TEST(test_a_row_needing_write_multiple_is_refused_not_faked);
    RUN_TEST(test_a_device_that_refuses_is_rejected_not_reported_as_a_fault);
    RUN_TEST(test_the_mic_power_limit_write_row_is_declared_but_dormant);
    return UNITY_END();
}
