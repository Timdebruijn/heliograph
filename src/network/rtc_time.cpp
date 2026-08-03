// SPDX-License-Identifier: MIT

#include "rtc_time.h"

namespace heliograph::rtc {
namespace {

constexpr uint8_t kOsFlag = 0x80;  // seconds bit 7: oscillator stopped, time unreliable

uint8_t toBcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }

/// BCD with the nibbles actually checked. The naive version decodes 0x1A as 20 -- a perfectly
/// plausible minute out of a byte that is not BCD at all -- so one corrupted I2C bit yields a
/// wrong time that passes every range check that follows.
bool fromBcdChecked(uint8_t v, int& out) {
    const int hi = v >> 4;
    const int lo = v & 0x0F;
    if (hi > 9 || lo > 9) {
        return false;
    }
    out = hi * 10 + lo;
    return true;
}

}  // namespace

int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int     yoe = static_cast<int>(y - era * 400);
    const int     doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int     doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civilFromDays(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int     doe = static_cast<int>(z - era * 146097);
    const int     yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int     doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int     mp  = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = static_cast<int>(yoe + era * 400) + (m <= 2);
}

bool decodeRegisters(const Registers& regs, time_t& out) {
    if ((regs[0] & kOsFlag) != 0) {
        return false;  // oscillator stopped since the last write: time is untrustworthy
    }
    int sec = 0, min = 0, hour = 0, day = 0, mon = 0, yy = 0;
    if (!fromBcdChecked(regs[0] & 0x7F, sec) || !fromBcdChecked(regs[1] & 0x7F, min) ||
        !fromBcdChecked(regs[2] & 0x3F, hour) || !fromBcdChecked(regs[3] & 0x3F, day) ||
        !fromBcdChecked(regs[5] & 0x1F, mon) || !fromBcdChecked(regs[6], yy)) {
        return false;  // not valid BCD: the read is garbled, whatever it happens to decode to
    }
    if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 || mon < 1 || mon > 12) {
        return false;
    }
    const int year = 2000 + yy;
    // The year had no bound at all, and it is the field where a wrong value does the most
    // damage. TimeManager::synced() only guards the LOW side, so an implausibly HIGH year reads
    // as "the clock is set": it lands in every log line and payload, gets written back into the
    // chip on the next sync, and on a bridge that never reaches NTP it stays there forever.
    if (year < kEarliestPlausibleYear || year > kLatestPlausibleYear) {
        return false;
    }
    // Day against the actual month, not just against 31. February 31st passes every check
    // above and silently becomes March 3rd -- a plausible date that is not the one on the chip,
    // which is the exact class of bug this file was split out to close. Round-tripping through
    // the civil-date conversion is the cheapest way to ask "does this date exist".
    int ry = 0, rm = 0, rd = 0;
    const int64_t days = daysFromCivil(year, mon, day);
    civilFromDays(days, ry, rm, rd);
    if (ry != year || rm != mon || rd != day) {
        return false;
    }
    out = static_cast<time_t>(days * 86400 + hour * 3600 + min * 60 + sec);
    return true;
}

void encodeRegisters(time_t utc, Registers& regs) {
    const int64_t days = static_cast<int64_t>(utc) / 86400;
    int32_t       rem  = static_cast<int32_t>(static_cast<int64_t>(utc) - days * 86400);
    const int     hour = rem / 3600;
    rem %= 3600;
    int y = 0, m = 0, d = 0;
    civilFromDays(days, y, m, d);

    regs[0] = toBcd(rem % 60);  // seconds; writing this register also clears the OS flag
    regs[1] = toBcd(rem / 60);
    regs[2] = toBcd(hour);
    regs[3] = toBcd(d);
    // The weekday belongs here, not in the caller. Leaving it to the ESP32-only file put the
    // one bit of arithmetic that is not host-tested back inside the block this unit exists to
    // get it out of -- and deleting the caller's line as redundant would then store every write
    // as Sunday with nothing to notice.
    regs[4] = static_cast<uint8_t>((days + 4) % 7);  // 1970-01-01 was a Thursday
    regs[5] = toBcd(m);
    // No year guard here on purpose: past 2099 toBcd() emits a non-BCD byte, which the next
    // boot's decodeRegisters() refuses. The clock is then lost rather than silently wrong, and
    // that is the direction to fail in.
    regs[6] = toBcd(y - 2000);
}

}  // namespace heliograph::rtc
