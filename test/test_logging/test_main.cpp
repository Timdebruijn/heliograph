// SPDX-License-Identifier: MIT
// Log-line time prefix: wall-clock once synced, uptime before, never a fake 1970.

#include <unity.h>

#include <cstdlib>
#include <cstring>
#include <ctime>

#include <string>

#include "diagnostics/log_buffer.h"
#include "diagnostics/log_timestamp.h"

using heliograph::log::formatIsoLocalTime;
using heliograph::log::formatLogTimestamp;

namespace {

void setTz(const char* tz) {
    setenv("TZ", tz, 1);
    tzset();
}

}  // namespace

void setUp() {}
void tearDown() {}

// 2024-01-01 00:00:00 UTC.
constexpr time_t kNewYear2024Utc = 1704067200;
// 2024-07-01 12:00:00 UTC — summer, so a DST zone is one hour further off than in winter.
constexpr time_t kSummerNoonUtc = 1719835200;

static void test_synced_formats_local_wallclock() {
    setTz("UTC0");
    char buf[32];
    const size_t n = formatLogTimestamp(buf, sizeof(buf), /*synced=*/true, kNewYear2024Utc, 5000);
    TEST_ASSERT_EQUAL_STRING("2024-01-01 00:00:00", buf);
    TEST_ASSERT_EQUAL_UINT(19, n);
}

static void test_synced_honours_timezone_and_dst() {
    // Europe/Amsterdam: CET (+1) in winter, CEST (+2) in summer. The POSIX rule must apply DST,
    // otherwise the whole point of a configurable TZ is lost.
    setTz("CET-1CEST,M3.5.0,M10.5.0/3");
    char buf[32];
    formatLogTimestamp(buf, sizeof(buf), true, kSummerNoonUtc, 0);
    TEST_ASSERT_EQUAL_STRING("2024-07-01 14:00:00", buf);  // 12:00 UTC + 2h CEST

    formatLogTimestamp(buf, sizeof(buf), true, kNewYear2024Utc, 0);
    TEST_ASSERT_EQUAL_STRING("2024-01-01 01:00:00", buf);  // 00:00 UTC + 1h CET
}

static void test_unsynced_shows_uptime_in_tenths() {
    char buf[32];
    const size_t n = formatLogTimestamp(buf, sizeof(buf), /*synced=*/false, kNewYear2024Utc, 123456);
    TEST_ASSERT_EQUAL_STRING("+123.4s", buf);  // 123456 ms -> 123.4 s; epoch ignored when unsynced
    TEST_ASSERT_EQUAL_UINT(7, n);
}

static void test_unsynced_at_zero_uptime() {
    char buf[32];
    formatLogTimestamp(buf, sizeof(buf), false, 0, 0);
    TEST_ASSERT_EQUAL_STRING("+0.0s", buf);
}

static void test_too_small_buffer_returns_zero() {
    setTz("UTC0");
    char buf[8];  // cannot hold "2024-01-01 00:00:00"
    const size_t n = formatLogTimestamp(buf, sizeof(buf), true, kNewYear2024Utc, 0);
    TEST_ASSERT_EQUAL_UINT(0, n);
}

static void test_zero_length_buffer_is_safe() {
    char buf[1] = {'x'};
    const size_t n = formatLogTimestamp(buf, 0, true, kNewYear2024Utc, 0);
    TEST_ASSERT_EQUAL_UINT(0, n);
}

// --- log ring buffer ----------------------------------------------------------------------

static void test_lines_come_back_oldest_first() {
    heliograph::log::clearLines();
    heliograph::log::pushLine("first");
    heliograph::log::pushLine("second");
    heliograph::log::pushLine("third");

    const auto lines = heliograph::log::recentLines(10);
    TEST_ASSERT_EQUAL_size_t(3, lines.size());
    TEST_ASSERT_EQUAL_STRING("first", lines[0].c_str());
    TEST_ASSERT_EQUAL_STRING("third", lines[2].c_str());
}

static void test_limit_returns_the_most_recent_lines() {
    heliograph::log::clearLines();
    heliograph::log::pushLine("a");
    heliograph::log::pushLine("b");
    heliograph::log::pushLine("c");

    const auto lines = heliograph::log::recentLines(2);
    TEST_ASSERT_EQUAL_size_t(2, lines.size());
    TEST_ASSERT_EQUAL_STRING("b", lines[0].c_str());
    TEST_ASSERT_EQUAL_STRING("c", lines[1].c_str());
}

static void test_the_ring_overwrites_the_oldest_line() {
    heliograph::log::clearLines();
    for (size_t i = 0; i < heliograph::log::kLogBufferLines + 5; ++i) {
        heliograph::log::pushLine(std::to_string(i).c_str());
    }
    const auto lines = heliograph::log::recentLines(heliograph::log::kLogBufferLines);
    TEST_ASSERT_EQUAL_size_t(heliograph::log::kLogBufferLines, lines.size());
    // The first five are gone; the oldest survivor is line 5.
    TEST_ASSERT_EQUAL_STRING("5", lines[0].c_str());
    TEST_ASSERT_EQUAL_STRING(
        std::to_string(heliograph::log::kLogBufferLines + 4).c_str(),
        lines[lines.size() - 1].c_str());
}

static void test_total_counts_overwritten_lines_too() {
    // Without this a reader cannot tell a quiet bus from a buffer that wrapped.
    heliograph::log::clearLines();
    for (size_t i = 0; i < heliograph::log::kLogBufferLines + 3; ++i) {
        heliograph::log::pushLine("x");
    }
    TEST_ASSERT_EQUAL_UINT32(heliograph::log::kLogBufferLines + 3,
                             heliograph::log::totalLines());
}

static void test_an_overlong_line_is_truncated_not_overflowing() {
    heliograph::log::clearLines();
    const std::string huge(heliograph::log::kLogLineChars * 2, 'A');
    heliograph::log::pushLine(huge.c_str());

    const auto lines = heliograph::log::recentLines(1);
    TEST_ASSERT_EQUAL_size_t(1, lines.size());
    TEST_ASSERT_TRUE(lines[0].size() < heliograph::log::kLogLineChars);
    TEST_ASSERT_EQUAL_CHAR('A', lines[0][0]);
}

// The API's human-readable clock ("time" in status/diagnostics) uses the same local-time
// rules as the log prefix; this pins the format and the buffer-safety contract.
static void test_iso_local_time_formats_in_the_configured_zone() {
    setTz("CET-1CEST,M3.5.0,M10.5.0/3");
    char buf[32];
    const size_t n = formatIsoLocalTime(buf, sizeof(buf), kSummerNoonUtc);
    TEST_ASSERT_EQUAL_STRING("2024-07-01 14:00:00", buf);  // CEST = UTC+2
    TEST_ASSERT_EQUAL_UINT(19, n);

    char tiny[8];
    TEST_ASSERT_EQUAL_UINT(0, formatIsoLocalTime(tiny, sizeof(tiny), kSummerNoonUtc));
}

static void test_a_null_line_is_ignored() {
    heliograph::log::clearLines();
    heliograph::log::pushLine(nullptr);
    TEST_ASSERT_EQUAL_size_t(0, heliograph::log::recentLines(10).size());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_lines_come_back_oldest_first);
    RUN_TEST(test_limit_returns_the_most_recent_lines);
    RUN_TEST(test_the_ring_overwrites_the_oldest_line);
    RUN_TEST(test_total_counts_overwritten_lines_too);
    RUN_TEST(test_an_overlong_line_is_truncated_not_overflowing);
    RUN_TEST(test_a_null_line_is_ignored);
    RUN_TEST(test_iso_local_time_formats_in_the_configured_zone);
    RUN_TEST(test_synced_formats_local_wallclock);
    RUN_TEST(test_synced_honours_timezone_and_dst);
    RUN_TEST(test_unsynced_shows_uptime_in_tenths);
    RUN_TEST(test_unsynced_at_zero_uptime);
    RUN_TEST(test_too_small_buffer_returns_zero);
    RUN_TEST(test_zero_length_buffer_is_safe);
    return UNITY_END();
}
