// SPDX-License-Identifier: MIT
//
// SunSpec decoding. The offsets came from the official model definitions, but the rules that
// actually keep a reading honest are the scale factor and the not-implemented sentinels --
// get either wrong and the bridge publishes a confident number that is simply false. Those
// are what this file pins hardest.

#include <unity.h>

#include <vector>

#include "drivers/sunspec/sunspec_parser.h"

using namespace heliograph::sunspec;

void setUp() {}
void tearDown() {}

namespace {

/// A model 103 block with every point not-implemented, so each test can set exactly the ones
/// it cares about and nothing leaks in from a neighbouring offset.
std::vector<uint16_t> blankInverter() {
    std::vector<uint16_t> b(inverter::kMinRegisters + 1, kNotImplementedU16);
    b[0] = kModelInverterThreePhase;
    b[1] = 50;
    // Signed points carry the int16 sentinel, NOT the uint16 one: 0xFFFF in an int16 point
    // is simply -1, a perfectly valid reading. Getting this wrong in the fixture is the same
    // mistake a device firmware can make, and it is why the two sentinels are kept distinct.
    b[inverter::kW]      = kNotImplementedS16;
    b[inverter::kDCW]    = kNotImplementedS16;
    b[inverter::kTmpCab] = kNotImplementedS16;
    // Scale factors have their own sentinel.
    b[inverter::kA_SF]   = kNotImplementedS16;
    b[inverter::kV_SF]   = kNotImplementedS16;
    b[inverter::kW_SF]   = kNotImplementedS16;
    b[inverter::kHz_SF]  = kNotImplementedS16;
    b[inverter::kWH_SF]  = kNotImplementedS16;
    b[inverter::kDCW_SF] = kNotImplementedS16;
    b[inverter::kTmp_SF] = kNotImplementedS16;
    return b;
}

uint16_t sf(int exponent) { return static_cast<uint16_t>(static_cast<int16_t>(exponent)); }

}  // namespace

static void test_model_ids() {
    TEST_ASSERT_TRUE(isInverterModel(101));
    TEST_ASSERT_TRUE(isInverterModel(102));
    TEST_ASSERT_TRUE(isInverterModel(103));
    TEST_ASSERT_FALSE(isInverterModel(1));    // common model
    TEST_ASSERT_FALSE(isInverterModel(120));  // nameplate
}

// The whole point of a scale factor: the same raw register means different things.
static void test_scale_factor_is_applied_with_its_sign() {
    auto b = blankInverter();
    b[inverter::kW]    = 1234;
    b[inverter::kW_SF] = sf(0);
    InverterReadings r;
    TEST_ASSERT_TRUE(decodeInverter(b.data(), b.size(), r));
    TEST_ASSERT_TRUE(r.hasAcPower);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 1234.0, r.acPowerW);

    b[inverter::kW_SF] = sf(-1);  // x0.1
    TEST_ASSERT_TRUE(decodeInverter(b.data(), b.size(), r));
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 123.4, r.acPowerW);

    b[inverter::kW_SF] = sf(2);  // x100
    TEST_ASSERT_TRUE(decodeInverter(b.data(), b.size(), r));
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 123400.0, r.acPowerW);
}

// AC power is int16: a device exporting nothing and importing must not read as +64 kW.
static void test_signed_points_decode_negative() {
    auto b = blankInverter();
    b[inverter::kW]    = static_cast<uint16_t>(static_cast<int16_t>(-250));
    b[inverter::kW_SF] = sf(0);
    InverterReadings r;
    TEST_ASSERT_TRUE(decodeInverter(b.data(), b.size(), r));
    TEST_ASSERT_DOUBLE_WITHIN(0.001, -250.0, r.acPowerW);
}

// An unusable scale factor makes the value ABSENT, never unscaled. Publishing the raw number
// would be wrong by an unknown power of ten while looking perfectly plausible.
static void test_a_missing_scale_factor_drops_the_value() {
    auto b = blankInverter();
    b[inverter::kW]    = 1234;
    b[inverter::kW_SF] = kNotImplementedS16;
    InverterReadings r;
    TEST_ASSERT_TRUE(decodeInverter(b.data(), b.size(), r));
    TEST_ASSERT_FALSE(r.hasAcPower);
}

static void test_not_implemented_values_are_absent_not_zero() {
    auto b = blankInverter();
    // Real scale factors, but the values themselves are not implemented.
    b[inverter::kW_SF]  = sf(0);
    b[inverter::kV_SF]  = sf(-1);
    b[inverter::kHz_SF] = sf(-2);
    InverterReadings r;
    TEST_ASSERT_TRUE(decodeInverter(b.data(), b.size(), r));
    TEST_ASSERT_FALSE(r.hasAcPower);     // int16 sentinel
    TEST_ASSERT_FALSE(r.hasAcVoltage);   // uint16 sentinel
    TEST_ASSERT_FALSE(r.hasFrequency);
    TEST_ASSERT_FALSE(r.hasState);
}

static void test_an_absurd_scale_factor_is_refused() {
    auto b = blankInverter();
    b[inverter::kW]    = 100;
    b[inverter::kW_SF] = sf(30);  // outside the documented -10..10
    InverterReadings r;
    TEST_ASSERT_TRUE(decodeInverter(b.data(), b.size(), r));
    TEST_ASSERT_FALSE(r.hasAcPower);
}

// Lifetime energy is a 32-bit accumulator in watt-hours; the canonical channel is kWh.
static void test_lifetime_energy_is_converted_to_kwh() {
    auto b = blankInverter();
    b[inverter::kWH]     = 0x0021;  // 0x0021E240 = 2 220 608 Wh
    b[inverter::kWH + 1] = 0xE240;
    b[inverter::kWH_SF]  = sf(0);
    InverterReadings r;
    TEST_ASSERT_TRUE(decodeInverter(b.data(), b.size(), r));
    TEST_ASSERT_TRUE(r.hasEnergyTotal);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 2220.608, r.energyTotalKwh);
}

// A brand-new inverter really has produced zero, so an accumulator of 0 is a reading.
static void test_a_zero_accumulator_is_a_real_reading() {
    auto b = blankInverter();
    b[inverter::kWH]     = 0;
    b[inverter::kWH + 1] = 0;
    b[inverter::kWH_SF]  = sf(0);
    InverterReadings r;
    TEST_ASSERT_TRUE(decodeInverter(b.data(), b.size(), r));
    TEST_ASSERT_TRUE(r.hasEnergyTotal);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 0.0, r.energyTotalKwh);
}

// A short read must never be decoded from whatever happened to be in the buffer.
static void test_a_truncated_block_is_refused() {
    auto             b = blankInverter();
    InverterReadings r;
    TEST_ASSERT_FALSE(decodeInverter(b.data(), inverter::kMinRegisters - 1, r));
    TEST_ASSERT_FALSE(decodeInverter(nullptr, 100, r));
}

static void test_common_model_strings() {
    std::vector<uint16_t> b(common::kMinRegisters, 0);
    b[0] = kModelCommon;
    b[1] = 66;
    // "Acme" then NUL padding, big-endian register pairs.
    b[common::kMn]     = ('A' << 8) | 'c';
    b[common::kMn + 1] = ('m' << 8) | 'e';
    b[common::kSN]     = ('1' << 8) | '2';
    b[common::kSN + 1] = ('3' << 8) | '\0';

    CommonIdentity id;
    TEST_ASSERT_TRUE(decodeCommon(b.data(), b.size(), id));
    TEST_ASSERT_EQUAL_STRING("Acme", id.manufacturer.c_str());
    TEST_ASSERT_EQUAL_STRING("123", id.serial.c_str());
}

static void test_common_model_too_short_is_refused() {
    std::vector<uint16_t> b(10, 0);
    CommonIdentity        id;
    TEST_ASSERT_FALSE(decodeCommon(b.data(), b.size(), id));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_model_ids);
    RUN_TEST(test_scale_factor_is_applied_with_its_sign);
    RUN_TEST(test_signed_points_decode_negative);
    RUN_TEST(test_a_missing_scale_factor_drops_the_value);
    RUN_TEST(test_not_implemented_values_are_absent_not_zero);
    RUN_TEST(test_an_absurd_scale_factor_is_refused);
    RUN_TEST(test_lifetime_energy_is_converted_to_kwh);
    RUN_TEST(test_a_zero_accumulator_is_a_real_reading);
    RUN_TEST(test_a_truncated_block_is_refused);
    RUN_TEST(test_common_model_strings);
    RUN_TEST(test_common_model_too_short_is_refused);
    return UNITY_END();
}
