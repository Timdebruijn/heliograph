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
void regs(Registers& out, uint8_t s, uint8_t mi, uint8_t h, uint8_t d, uint8_t mo, uint8_t y) {
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
    Registers r{};
    regs(r, 0x30, 0x45, 0x12, 0x25, 0x07, 0x26);  // 2026-07-25 12:45:30 UTC
    time_t t = 0;
    TEST_ASSERT_TRUE(decodeRegisters(r, t));
    // 2026-07-25T12:45:30Z
    TEST_ASSERT_EQUAL_INT64(1784983530, static_cast<int64_t>(t));
}

static void test_a_round_trip_survives() {
    Registers r{};
    const time_t original = 1784983530;
    encodeRegisters(original, r);
    time_t back = 0;
    TEST_ASSERT_TRUE(decodeRegisters(r, back));
    TEST_ASSERT_EQUAL_INT64(original, static_cast<int64_t>(back));
}

static void test_the_oscillator_stopped_flag_refuses_the_read() {
    Registers r{};
    regs(r, 0x30 | 0x80, 0x45, 0x12, 0x25, 0x07, 0x26);  // OS bit set
    time_t t = 0;
    TEST_ASSERT_FALSE(decodeRegisters(r, t));
}

// The finding this file exists for. fromBcd(0xFF) is 165, so the year came out as 2165 -- and
// TimeManager::synced() only guards the low side, so that reads as "the clock is set". It then
// stamps every log line, every payload, and gets written back into the chip on the next sync.
static void test_a_garbled_year_is_refused_rather_than_becoming_2165() {
    Registers r{};
    regs(r, 0x30, 0x45, 0x12, 0x25, 0x07, 0xFF);
    time_t t = 0;
    TEST_ASSERT_FALSE(decodeRegisters(r, t));
}

// ...and a merely legal-but-implausible year, which is not garbled BCD at all.
static void test_a_year_outside_the_plausible_range_is_refused() {
    Registers r{};
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
    Registers r{};
    regs(r, 0x30, 0x1A, 0x12, 0x25, 0x07, 0x26);  // minutes = 0x1A
    time_t t = 0;
    TEST_ASSERT_FALSE(decodeRegisters(r, t));

    regs(r, 0x30, 0x45, 0x12, 0x25, 0x0A, 0x26);  // month nibble > 9
    TEST_ASSERT_FALSE(decodeRegisters(r, t));
}

static void test_out_of_range_fields_are_refused() {
    Registers r{};
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
// February 31st passes every field-range check and silently becomes March 3rd. A plausible
// date that is not the one on the chip is exactly what this file exists to refuse.
static void test_a_date_that_does_not_exist_is_refused() {
    Registers r{};
    time_t  t = 0;
    regs(r, 0x00, 0x00, 0x12, 0x31, 0x02, 0x26);  // 31 February
    TEST_ASSERT_FALSE(decodeRegisters(r, t));
    regs(r, 0x00, 0x00, 0x12, 0x31, 0x04, 0x26);  // 31 April
    TEST_ASSERT_FALSE(decodeRegisters(r, t));
    regs(r, 0x00, 0x00, 0x12, 0x29, 0x02, 0x26);  // 29 February in a non-leap year
    TEST_ASSERT_FALSE(decodeRegisters(r, t));
    // ...and the real ones still pass.
    regs(r, 0x00, 0x00, 0x12, 0x30, 0x04, 0x26);
    TEST_ASSERT_TRUE(decodeRegisters(r, t));
}

// The weekday lives in encodeRegisters so it is covered here. Left to the ESP32-only caller it
// was the one piece of arithmetic no test could see, in the file split out precisely to stop
// that -- and deleting the caller's line as redundant would have stored every write as Sunday.
static void test_the_weekday_register_is_filled() {
    Registers r{};
    encodeRegisters(1784983530, r);  // 2026-07-25 was a Saturday
    TEST_ASSERT_EQUAL_UINT8(6, r[4]);
    encodeRegisters(1835395200, r);  // 2028-02-29 was a Tuesday
    TEST_ASSERT_EQUAL_UINT8(2, r[4]);
}

static void test_a_leap_day_is_handled() {
    Registers r{};
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
    RUN_TEST(test_a_date_that_does_not_exist_is_refused);
    RUN_TEST(test_the_weekday_register_is_filled);
    RUN_TEST(test_a_leap_day_is_handled);
    return UNITY_END();
}
