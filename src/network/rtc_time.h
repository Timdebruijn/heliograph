// SPDX-License-Identifier: MIT
//
// The pure half of the PCF85063 driver: turning its seven BCD registers into an epoch, and back.
//
// Split out of rtc_pcf85063.cpp because that file is `#if defined(ESP32)` around <Wire.h>, so
// none of this arithmetic could be tested on the host -- and it is the part where a mistake is
// silent. A wrong I2C address fails loudly; a wrong decode produces a plausible time that
// propagates into every log line, every payload and back into the chip on the next NTP sync.

#pragma once

#include <cstdint>
#include <ctime>

namespace heliograph::rtc {

/// The RTC counts a two-digit year from 2000, so 2000-2099 is everything it can express. The
/// lower bound is deliberately "before this firmware existed": a chip that has lost power reads
/// back something from the last century or from 2000, and that must fail rather than become the
/// system clock.
inline constexpr int kEarliestPlausibleYear = 2024;
inline constexpr int kLatestPlausibleYear   = 2099;

/// Days since the Unix epoch for a civil date. Howard Hinnant's algorithm, exact for the
/// Gregorian calendar. Used instead of mktime(), which interprets in the process timezone, and
/// instead of timegm(), which newlib does not have.
int64_t daysFromCivil(int y, int m, int d);
void    civilFromDays(int64_t z, int& y, int& m, int& d);

/// Decodes the seven registers starting at Seconds into a UTC epoch.
///
/// Returns false -- meaning "no usable time" -- when the oscillator-stopped flag is set, when
/// any byte is not valid BCD, or when a field is out of range. Every one of those is a reading
/// that would otherwise become a confident wrong clock:
///
///   - Plain BCD decoding turns 0x1A into 20, a perfectly plausible minute from a byte that is
///     not BCD at all, so a single flipped I2C bit passes every range check downstream.
///   - The year had no bound, and 0xFF decodes to 165 -> the year 2165. Because
///     TimeManager::synced() only guards the low side, that reads as "the clock is set", and on
///     a bridge that never reaches NTP it stays that way forever.
bool decodeRegisters(const uint8_t regs[7], time_t& out);

/// The inverse, for the write-back after an NTP sync. Fills seven registers starting at Seconds.
void encodeRegisters(time_t utc, uint8_t regs[7]);

}  // namespace heliograph::rtc
