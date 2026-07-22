// SPDX-License-Identifier: MIT
//
// Minimal level-gated logging.
//
// Exists because configuration carried a `logging.level` field that was validated, persisted
// and shown in the web form -- and used by nothing at all. A setting that silently does
// nothing is worse than a missing one: the user believes it applied.
//
// The level gate is pure and host-tested. Emission is a thin Serial wrapper.

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>

#include "config/configuration.h"

namespace heliograph::log {

void     setLevel(LogLevel level);
LogLevel level();

/// Fills a short time-prefix into `buf`, returning its length (0 = none). Installed by the time
/// manager once SNTP is wired; until then, or on the host, there is no provider and lines carry
/// no timestamp. Kept as a plain function pointer so the logger gains no dependency on the clock
/// or on Arduino -- the provider reaches back to those, not the other way around.
using TimestampFn = size_t (*)(char* buf, size_t n);
void setTimestampProvider(TimestampFn provider);

/// True when a message at `level` would be emitted. Call before building an expensive
/// message -- formatting a hex dump you then discard is the usual way TRACE support ends up
/// costing something even when it is off.
bool enabled(LogLevel level);

void error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void info(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void debug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/// Hex dump for raw protocol frames. TRACE only, per the brief: raw RS485 frames must never
/// appear at a lower level.
void traceHex(const char* prefix, const uint8_t* data, size_t len);

}  // namespace heliograph::log
