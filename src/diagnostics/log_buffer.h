// SPDX-License-Identifier: MIT
//
// In-memory ring of recent log lines, readable over REST.
//
// Exists because diagnosing a live bus meant attaching a USB cable -- and on a device powered
// from that same port, attaching the cable power-cycles it, destroying the very state being
// investigated (2026-07-20). Logs the device already produces are kept in RAM so they can be
// fetched over the network instead.
//
// Fixed storage, never the heap: a log buffer that grows under load takes the device down
// exactly when the load is worth logging. Oldest lines are overwritten.
//
// Same rule as every other output: this holds whatever the logger emitted, and the logger
// never emits a secret. Nothing here may become the exception.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace heliograph::log {

/// Lines retained. A failed poll cycle (registration handshake plus the data query, TX and RX
/// each) runs to roughly a dozen lines at TRACE, so this holds several cycles.
inline constexpr size_t kLogBufferLines = 64;
/// Long enough for a 64-byte TRACE hex dump plus prefix, tag and timestamp. Longer lines are
/// truncated rather than split: a partial frame dump still identifies the frame.
inline constexpr size_t kLogLineChars = 256;

/// Appends a finished line. Thread-safe; called from every task that logs.
void pushLine(const char* line);

/// The most recent lines, oldest first, capped at `max`.
std::vector<std::string> recentLines(size_t max);

/// Lines produced since boot, including those already overwritten. The gap between this and
/// the returned count tells a reader whether they are seeing everything.
uint32_t totalLines();

void clearLines();

}  // namespace heliograph::log
