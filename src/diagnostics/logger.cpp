// SPDX-License-Identifier: MIT

#include "logger.h"

#include <cstdio>

#include "log_buffer.h"

#if defined(ESP32)
#include <Arduino.h>
#endif

namespace heliograph::log {
namespace {

LogLevel    g_level     = LogLevel::Info;
TimestampFn g_timestamp = nullptr;

const char* tag(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "E";
        case LogLevel::Warn:  return "W";
        case LogLevel::Info:  return "I";
        case LogLevel::Debug: return "D";
        case LogLevel::Trace: return "T";
    }
    return "?";
}

void emit(LogLevel level, const char* fmt, va_list args) {
    if (!enabled(level)) {
        return;
    }
    char buf[256];  // bounded: a log line must never be able to exhaust the heap
    vsnprintf(buf, sizeof(buf), fmt, args);

    char         ts[32];
    const size_t tn = g_timestamp != nullptr ? g_timestamp(ts, sizeof(ts)) : 0;

    // Assembled once, then both printed and retained: the serial console and the REST log
    // must show the same line, or chasing a bug over the network means chasing a different
    // rendering of it.
    //
    // The explicit %s precisions keep that true under truncation too -- console and ring
    // clip at the same point -- and let GCC prove the line fits instead of warning
    // -Wformat-truncation on every firmware build. Budget: kLogLineChars (256) minus NUL
    // and the worst-case prefix -- a 31-char timestamp, the 1-char tag() level letter and
    // 4 separator chars -- leaves 219; without a timestamp, 251. The longest real message
    // (a traceHex dump, ~210 chars) fits either way, so nothing legitimate is ever clipped.
    static_assert(kLogLineChars == 256, "revisit the %s precisions below");
    char line[kLogLineChars];
    if (tn > 0) {
        snprintf(line, sizeof(line), "%s [%s] %.219s", ts, tag(level), buf);
    } else {
        snprintf(line, sizeof(line), "[%s] %.251s", tag(level), buf);
    }

#if defined(ESP32)
    Serial.println(line);
#else
    std::printf("%s\n", line);
#endif
    pushLine(line);
}

}  // namespace

void setLevel(LogLevel level) { g_level = level; }
LogLevel level() { return g_level; }

void setTimestampProvider(TimestampFn provider) { g_timestamp = provider; }

bool enabled(LogLevel l) {
    return static_cast<uint8_t>(l) <= static_cast<uint8_t>(g_level);
}

#define HELIOGRAPH_LOG_FN(name, lvl)          \
    void name(const char* fmt, ...) {          \
        va_list args;                          \
        va_start(args, fmt);                   \
        emit(lvl, fmt, args);                  \
        va_end(args);                          \
    }

HELIOGRAPH_LOG_FN(error, LogLevel::Error)
HELIOGRAPH_LOG_FN(warn, LogLevel::Warn)
HELIOGRAPH_LOG_FN(info, LogLevel::Info)
HELIOGRAPH_LOG_FN(debug, LogLevel::Debug)
HELIOGRAPH_LOG_FN(trace, LogLevel::Trace)

#undef HELIOGRAPH_LOG_FN

void traceHex(const char* prefix, const uint8_t* data, size_t len) {
    if (!enabled(LogLevel::Trace) || data == nullptr) {
        return;
    }
    // Bounded per line and overall: a corrupted length byte must not turn into a megabyte of
    // output that itself takes the device down.
    constexpr size_t kMaxBytes = 64;
    const size_t     n         = len < kMaxBytes ? len : kMaxBytes;
    char             line[3 * kMaxBytes + 1];
    size_t           pos = 0;
    for (size_t i = 0; i < n; ++i) {
        pos += static_cast<size_t>(snprintf(line + pos, sizeof(line) - pos, "%02X ", data[i]));
    }
    trace("%s %s%s", prefix, line, len > kMaxBytes ? "..." : "");
}

}  // namespace heliograph::log
