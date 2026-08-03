// SPDX-License-Identifier: MIT
//
// The pure half of the PCF85063 driver: turning its seven BCD registers into an epoch, and back.
//
// Split out of rtc_pcf85063.cpp because that file is `#if defined(ESP32)` around <Wire.h>, so
// none of this arithmetic could be tested on the host -- and it is the part where a mistake is
// silent. A wrong I2C address fails loudly; a wrong decode produces a plausible time that
// propagates into every log line, every payload and back into the chip on the next NTP sync.

#pragma once

#include <array>
#include <cstdint>
#include <ctime>

namespace heliograph::rtc {

/// The chip's seven time registers, in chip order from Seconds: sec, min, hour, day, weekday,
/// month, year-2000. All BCD.
///
/// A named type rather than `uint8_t regs[7]` in each signature, because that form decays to a
/// pointer: the seven is a comment the compiler does not read, and decode and encode could drift
/// apart on it without anything failing to build. Here the length is checked at every call.
using Registers = std::array<uint8_t, 7>;

/// The RTC counts a two-digit year from 2000, so 2000-2099 is everything it can express. The
/// lower bound is deliberately "before this firmware existed", and it deliberately matches
/// TimeManager's kSaneEpoch so the two agree on what counts as a set clock. A chip that lost
/// power is already caught by its oscillator-stopped flag; this bound is for the cases that are
/// not -- a corrupted register that still decodes as valid BCD, or a chip written by something
/// else entirely.
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
bool decodeRegisters(const Registers& regs, time_t& out);

/// The inverse, for the write-back after an NTP sync. Fills seven registers starting at Seconds.
void encodeRegisters(time_t utc, Registers& regs);

}  // namespace heliograph::rtc
