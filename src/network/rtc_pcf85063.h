// SPDX-License-Identifier: MIT
//
// PCF85063 battery-backed RTC, present on some boards (board::kHasRtc).
//
// Purpose: the system clock is valid from the first second of boot instead of only after
// NTP. Log lines carry wall-clock timestamps immediately -- which is exactly what you
// want when investigating why a device did NOT reach the network. The RTC stores UTC;
// local rendering stays the job of TZ, as everywhere else.
//
// The chip's OS (oscillator stop) flag is the honesty mechanism: after a first boot or
// an empty backup supply the time is marked untrustworthy, and readUtc() refuses it
// rather than restoring a plausible-looking wrong clock.

#pragma once

#include <ctime>

namespace heliograph::rtc {

/// True when the board declares an RTC and the chip answers on I2C. Safe to call on
/// boards without one (returns false, touches nothing).
bool begin();

/// Reads UTC. False when absent, unreadable, or the oscillator-stop flag marks the
/// stored time untrustworthy.
bool readUtc(time_t& out);

/// Writes UTC and clears the oscillator-stop flag. Called after every NTP sync.
bool writeUtc(time_t t);

}  // namespace heliograph::rtc
