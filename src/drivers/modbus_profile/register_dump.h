// SPDX-License-Identifier: MIT
//
// The line-rendering half of the TRACE register dump, with no logger and no level gate.
//
// Split out of modbus_profile_driver.cpp for the same reason rtc_time.cpp was split out of
// rtc_pcf85063.cpp: the interesting part could not be reached from a test. traceBlock() lives in
// an anonymous namespace behind `if (!log::enabled(Trace))`, so nothing could call it, and its
// buffer was a fixed 128 bytes against a worst case of 67 -- which meant the truncation guard it
// carries was unreachable BY CONSTRUCTION. A test that cannot reach the branch it is aiming at
// proves the branch compiles, nothing more.
//
// Making the buffer a parameter is what changes that. At sizeof(line) == 128 the guard is dead
// code; at 20 it fires on the first register. Same code path, reachable.

#pragma once

#include <cstddef>
#include <cstdint>

#include "drivers/modbus_profile/profile_tables.h"

namespace heliograph::profile {

/// Renders one dump line into `out`: the prefix `MODBUS unit <id> <in|hold> <reg>:` followed by
/// up to `count` registers as ` XXXX`, uppercase and zero-padded to four digits.
///
/// `start` is the ABSOLUTE register number of values[0] -- the caller advances both together, so
/// a line always names the register it shows. That is the whole point of the dump: the numbers
/// are read against an inverter's own display during bring-up.
///
/// Writes at most `outSize - 1` characters plus a NUL and never reads past `values[count - 1]`.
/// Returns the number of registers actually rendered, which is below `count` only when the
/// buffer ran out. `out` is left a valid NUL-terminated string in every case, including
/// outSize == 0 being refused outright and a formatting failure part-way through.
size_t formatRegisterLine(char* out, size_t outSize, uint8_t unitId, RegSpace space,
                          uint16_t start, const uint16_t* values, size_t count);

}  // namespace heliograph::profile
