// SPDX-License-Identifier: MIT
//
// Shared JSON serialisation helpers for the output adapters. Both the MQTT and REST payload
// builders had a byte-for-byte copy of these; one definition now.

#pragma once

#include <ArduinoJson.h>

#include <string>

#include "device/bridge_info.h"
#include "diagnostics/log_timestamp.h"

namespace heliograph::json_util {

/// Serialises and enforces the size ceiling. The document is built first and measured after:
/// ArduinoJson v7 grows elastically, so the bound is applied here rather than by pre-sizing.
/// Returns false (leaving `out` untouched) on overflow rather than truncating.
inline bool finish(const JsonDocument& doc, std::string& out, size_t maxBytes) {
    if (doc.overflowed()) {
        return false;  // allocation failed under memory pressure
    }
    const size_t needed = measureJson(doc);
    if (needed > maxBytes) {
        return false;
    }
    std::string buffer;
    buffer.resize(needed + 1);
    buffer.resize(serializeJson(doc, buffer.data(), buffer.size()));
    out = std::move(buffer);
    return true;
}

/// Omit rather than emit "": absent means "this protocol does not report it".
inline void addOptional(JsonObject obj, const char* key, const std::string& value) {
    if (!value.empty()) {
        obj[key] = value;
    }
}

/// Wall-clock feedback, shared by the status and diagnostics payloads. Honesty rule:
/// before the first NTP sync there is no time, so `time` and `ntp_last_sync` are null --
/// never a formatted 1970 date presented as if it were real.
inline void addClockFields(JsonObject obj, const BridgeInfo& bridge) {
    obj["time_synced"] = bridge.timeSynced;
    if (!bridge.timeSynced) {
        obj["time"]          = nullptr;
        obj["ntp_last_sync"] = nullptr;
        return;
    }
    char buf[24];
    if (log::formatIsoLocalTime(buf, sizeof(buf), static_cast<time_t>(bridge.currentEpoch)) > 0) {
        obj["time"] = buf;  // ArduinoJson copies the string
    } else {
        obj["time"] = nullptr;
    }
    if (bridge.lastNtpSyncEpoch > 0 &&
        log::formatIsoLocalTime(buf, sizeof(buf), static_cast<time_t>(bridge.lastNtpSyncEpoch)) > 0) {
        obj["ntp_last_sync"] = buf;
    } else {
        obj["ntp_last_sync"] = nullptr;
    }
    // The server the clock actually came from, and whether DHCP (option 42) supplied it
    // or the configured fallback answered. Unknown stays null -- never a guessed name.
    if (!bridge.ntpServer.empty()) {
        obj["ntp_server"]        = bridge.ntpServer;
        obj["ntp_server_source"] = bridge.ntpFromDhcp ? "dhcp" : "configured";
    } else {
        obj["ntp_server"]        = nullptr;
        obj["ntp_server_source"] = nullptr;
    }
}

}  // namespace heliograph::json_util
