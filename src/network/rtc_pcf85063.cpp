// SPDX-License-Identifier: MIT

#include "rtc_pcf85063.h"

#if defined(ESP32)

#include <Wire.h>

#include "boards/board.h"
#include "diagnostics/logger.h"

namespace heliograph::rtc {
namespace {

// PCF85063A register map (datasheet rev. 8, section 7).
constexpr uint8_t kRegControl1 = 0x00;
constexpr uint8_t kRegSeconds  = 0x04;  // bit 7 = OS: oscillator stopped, time unreliable
constexpr uint8_t kOsFlag      = 0x80;

bool    g_present = false;

uint8_t toBcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
int     fromBcd(uint8_t v) { return ((v >> 4) * 10) + (v & 0x0F); }

// Civil <-> epoch without mktime: mktime interprets in the process TZ, and newlib has no
// timegm. Howard Hinnant's days_from_civil algorithm; exact for the Gregorian calendar.
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

bool readRegs(uint8_t reg, uint8_t* buf, size_t n) {
    Wire.beginTransmission(board::kRtcI2cAddress);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom(static_cast<int>(board::kRtcI2cAddress), static_cast<int>(n)) !=
        static_cast<int>(n)) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        buf[i] = static_cast<uint8_t>(Wire.read());
    }
    return true;
}

}  // namespace

bool begin() {
    if constexpr (!board::kHasRtc) {
        return false;
    }
    Wire.begin(board::kRtcSda, board::kRtcScl);
    uint8_t ctrl = 0;
    g_present    = readRegs(kRegControl1, &ctrl, 1);
    if (!g_present) {
        log::warn("rtc: board declares a PCF85063 but it did not answer on I2C");
    }
    return g_present;
}

bool readUtc(time_t& out) {
    if (!g_present) {
        return false;
    }
    uint8_t r[7];
    if (!readRegs(kRegSeconds, r, sizeof(r))) {
        return false;
    }
    if ((r[0] & kOsFlag) != 0) {
        return false;  // oscillator stopped since the last write: time is untrustworthy
    }
    const int sec  = fromBcd(r[0] & 0x7F);
    const int min  = fromBcd(r[1] & 0x7F);
    const int hour = fromBcd(r[2] & 0x3F);
    const int day  = fromBcd(r[3] & 0x3F);
    const int mon  = fromBcd(r[5] & 0x1F);
    const int year = 2000 + fromBcd(r[6]);
    if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 || mon < 1 || mon > 12) {
        return false;  // a garbled BCD read must not become a plausible wrong clock
    }
    out = static_cast<time_t>(daysFromCivil(year, mon, day) * 86400 + hour * 3600 +
                              min * 60 + sec);
    return true;
}

bool writeUtc(time_t t) {
    if (!g_present || t <= 0) {
        return false;
    }
    int           y, m, d;
    const int64_t days = t / 86400;
    int           rem  = static_cast<int>(t % 86400);
    civilFromDays(days, y, m, d);
    const int hour = rem / 3600;
    rem %= 3600;
    const uint8_t weekday = static_cast<uint8_t>((days + 4) % 7);  // 1970-01-01 = Thursday

    Wire.beginTransmission(board::kRtcI2cAddress);
    Wire.write(kRegSeconds);
    Wire.write(toBcd(rem % 60));  // seconds; writing this register clears the OS flag
    Wire.write(toBcd(rem / 60));  // minutes
    Wire.write(toBcd(hour));
    Wire.write(toBcd(d));
    Wire.write(weekday);
    Wire.write(toBcd(m));
    Wire.write(toBcd(y - 2000));
    return Wire.endTransmission() == 0;
}

}  // namespace heliograph::rtc

#else  // !ESP32

namespace heliograph::rtc {
bool begin() { return false; }
bool readUtc(time_t&) { return false; }
bool writeUtc(time_t) { return false; }
}  // namespace heliograph::rtc

#endif
