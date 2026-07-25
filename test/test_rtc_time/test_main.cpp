// SPDX-License-Identifier: MIT
//
// The PCF85063's seven BCD registers, decoded. This arithmetic used to live inside an
// `#if defined(ESP32)` block around <Wire.h> and so could not be tested at all -- which is how
// an unbounded year survived: a wrong I2C address fails loudly, a wrong decode does not.

#include <unity.h>

#include "network/rtc_time.h"

using namespace heliograph::rtc;

void setUp() {}
void tearDown() {}

namespace {

/// Registers in chip order: sec, min, hour, day, weekday, month, year-2000. All BCD.
void regs(uint8_t out[7], uint8_t s, uint8_t mi, uint8_t h, uint8_t d, uint8_t mo, uint8_t y) {
    out[0] = s;
    out[1] = mi;
    out[2] = h;
    out[3] = d;
    out[4] = 0;
    out[5] = mo;
    out[6] = y;
}

}  // namespace

static void test_a_normal_time_decodes() {
    uint8_t r[7];
    regs(r, 0x30, 0x45, 0x12, 0x25, 0x07, 0x26);  // 2026-07-25 12:45:30 UTC
    time_t t = 0;
    TEST_ASSERT_TRUE(decodeRegisters(r, t));
    // 2026-07-25T12:45:30Z
    TEST_ASSERT_EQUAL_INT64(1784983530, static_cast<int64_t>(t));
}

static void test_a_round_trip_survives() {
    uint8_t r[7];
    const time_t original = 1784983530;
    encodeRegisters(original, r);
    time_t back = 0;
    TEST_ASSERT_TRUE(decodeRegisters(r, back));
    TEST_ASSERT_EQUAL_INT64(original, static_cast<int64_t>(back));
}

static void test_the_oscillator_stopped_flag_refuses_the_read() {
    uint8_t r[7];
    regs(r, 0x30 | 0x80, 0x45, 0x12, 0x25, 0x07, 0x26);  // OS bit set
    time_t t = 0;
    TEST_ASSERT_FALSE(decodeRegisters(r, t));
}

// The finding this file exists for. fromBcd(0xFF) is 165, so the year came out as 2165 -- and
// TimeManager::synced() only guards the low side, so that reads as "the clock is set". It then
// stamps every log line, every payload, and gets written back into the chip on the next sync.
static void test_a_garbled_year_is_refused_rather_than_becoming_2165() {
    uint8_t r[7];
    regs(r, 0x30, 0x45, 0x12, 0x25, 0x07, 0xFF);
    time_t t = 0;
    TEST_ASSERT_FALSE(decodeRegisters(r, t));
}

// ...and a merely legal-but-implausible year, which is not garbled BCD at all.
static void test_a_year_outside_the_plausible_range_is_refused() {
    uint8_t r[7];
    regs(r, 0x30, 0x45, 0x12, 0x25, 0x07, 0x99);  // 2099 is the chip's ceiling: accepted
    time_t t = 0;
    TEST_ASSERT_TRUE(decodeRegisters(r, t));

    regs(r, 0x30, 0x45, 0x12, 0x25, 0x07, 0x00);  // 2000: a chip that lost power
    TEST_ASSERT_FALSE(decodeRegisters(r, t));
    regs(r, 0x30, 0x45, 0x12, 0x25, 0x07, 0x23);  // 2023, before this firmware existed
    TEST_ASSERT_FALSE(decodeRegisters(r, t));
}

// A byte that is not valid BCD can still decode to something in range. 0x1A -> 20 is a
// perfectly plausible minute, so the range checks alone would wave it through.
static void test_a_non_bcd_byte_is_refused_even_when_it_decodes_in_range() {
    uint8_t r[7];
    regs(r, 0x30, 0x1A, 0x12, 0x25, 0x07, 0x26);  // minutes = 0x1A
    time_t t = 0;
    TEST_ASSERT_FALSE(decodeRegisters(r, t));

    regs(r, 0x30, 0x45, 0x12, 0x25, 0x0A, 0x26);  // month nibble > 9
    TEST_ASSERT_FALSE(decodeRegisters(r, t));
}

static void test_out_of_range_fields_are_refused() {
    uint8_t r[7];
    time_t  t = 0;
    regs(r, 0x60, 0x45, 0x12, 0x25, 0x07, 0x26);  // 60 seconds
    TEST_ASSERT_FALSE(decodeRegisters(r, t));
    regs(r, 0x30, 0x45, 0x24, 0x25, 0x07, 0x26);  // hour 24
    TEST_ASSERT_FALSE(decodeRegisters(r, t));
    regs(r, 0x30, 0x45, 0x12, 0x00, 0x07, 0x26);  // day 0
    TEST_ASSERT_FALSE(decodeRegisters(r, t));
    regs(r, 0x30, 0x45, 0x12, 0x25, 0x13, 0x26);  // month 13
    TEST_ASSERT_FALSE(decodeRegisters(r, t));
}

// A leap day is the classic off-by-one in civil-date arithmetic, and this code deliberately
// avoids mktime()/timegm() so it owns that correctness itself.
static void test_a_leap_day_is_handled() {
    uint8_t r[7];
    regs(r, 0x00, 0x00, 0x00, 0x29, 0x02, 0x28);  // 2028-02-29T00:00:00Z
    time_t t = 0;
    TEST_ASSERT_TRUE(decodeRegisters(r, t));
    TEST_ASSERT_EQUAL_INT64(1835395200, static_cast<int64_t>(t));

    int y = 0, m = 0, d = 0;
    civilFromDays(daysFromCivil(2028, 2, 29), y, m, d);
    TEST_ASSERT_EQUAL_INT(2028, y);
    TEST_ASSERT_EQUAL_INT(2, m);
    TEST_ASSERT_EQUAL_INT(29, d);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_normal_time_decodes);
    RUN_TEST(test_a_round_trip_survives);
    RUN_TEST(test_the_oscillator_stopped_flag_refuses_the_read);
    RUN_TEST(test_a_garbled_year_is_refused_rather_than_becoming_2165);
    RUN_TEST(test_a_year_outside_the_plausible_range_is_refused);
    RUN_TEST(test_a_non_bcd_byte_is_refused_even_when_it_decodes_in_range);
    RUN_TEST(test_out_of_range_fields_are_refused);
    RUN_TEST(test_a_leap_day_is_handled);
    return UNITY_END();
}
