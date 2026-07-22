// SPDX-License-Identifier: MIT
//
// Pure formatting of a log-line time prefix. No Arduino / ESP-IDF: the interesting decision --
// "do we have a real wall clock yet, or only uptime?" -- is host-testable, and the SNTP wiring
// that feeds it lives in network/time_manager.

#pragma once

#include <cstddef>
#include <ctime>
#include <cstdint>

namespace heliograph::log {

/// Writes a time prefix into `buf` and returns its length (0 if it did not fit).
///
/// When `synced` is true the prefix is local wall-clock time "YYYY-MM-DD HH:MM:SS" (the caller
/// is responsible for having set TZ). When false -- before the first NTP sync -- it is the
/// uptime as "+<seconds>.<tenths>s", never a fabricated 1970 date presented as if it were real.
size_t formatLogTimestamp(char* buf, size_t n, bool synced, time_t epoch, uint64_t uptimeMs);

/// Formats `epoch` as local "YYYY-MM-DD HH:MM:SS" (TZ must be set by the caller). Returns
/// the length written, 0 when it does not fit. Used for the human-readable clock fields in
/// the status/diagnostics APIs; the caller decides synced-ness and must pass null instead
/// of calling this with an unsynced (1970-ish) epoch.
size_t formatIsoLocalTime(char* buf, size_t n, time_t epoch);

}  // namespace heliograph::log
