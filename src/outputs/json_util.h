// SPDX-License-Identifier: MIT
//
// Shared JSON serialisation helpers for the output adapters. Both the MQTT and REST payload
// builders had a byte-for-byte copy of these; one definition now.

#pragma once

#include <ArduinoJson.h>

#include <string>

#include "json_limits.h"

#include "device/bridge_info.h"
#include "device/capability.h"
#include "device/command.h"
#include "device/device_state.h"
#include "diagnostics/log_timestamp.h"

namespace heliograph::json_util {

/// Re-exported so the payload builders keep saying `finish(...)`. The definition lives in
/// json_limits.h because config/ needs the same rule for the NVS blob.
using json_limits::finish;

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

/// One measurement entry: `{"value": …, "unit": …, "valid": …, "stale": …[, "derived": true]}`.
///
/// null, never 0, for anything not currently usable. Home Assistant maps null to "unknown" and
/// keeps it out of the statistics; a zero is recorded as a genuine reading, so a dead inverter
/// would drag an energy average down all night. The rule is identical in MQTT and REST because
/// it is the same promise to the same consumer -- it lived in both files until this became one.
inline void writeMeasurement(JsonObject entry, const Measurement& m) {
    if (m.valid && !m.stale) {
        entry["value"] = m.value;
    } else {
        entry["value"] = nullptr;
    }
    entry["unit"]  = unitSymbol(m.unit);
    entry["valid"] = m.valid;
    entry["stale"] = m.stale;
    if (m.derived) {
        entry["derived"] = true;
    }
}

/// The `status_code` / `status_text` / `error_code` triplet, under the same absent-as-null rule.
///
/// A protocol with no error-code field must not report 0: that reads as "no fault". A driver
/// with no status text must not report "": the UI renders that as a blank tile rather than as
/// "this protocol does not say".
inline void writeDeviceStatus(JsonObject obj, const DeviceState& state) {
    const bool usable = state.dataValid && !state.dataStale;
    if (state.statusCodeSupported && usable) {
        obj["status_code"] = state.statusCode;
        if (state.statusText.empty()) {
            obj["status_text"] = nullptr;
        } else {
            obj["status_text"] = state.statusText;
        }
    } else {
        obj["status_code"] = nullptr;
        obj["status_text"] = nullptr;
    }
    if (state.errorCodeSupported && usable) {
        obj["error_code"] = state.errorCode;
    } else {
        obj["error_code"] = nullptr;
    }
}

/// The capabilities document, byte-for-byte the same over MQTT and REST.
///
/// `maxBytes` has no default on purpose: the two outputs cap their payloads differently
/// (mqtt::kMaxPayloadBytes vs rest::kMaxResponseBytes) and that difference is deliberate, so
/// each caller states its own ceiling rather than inheriting whichever one was written first.
inline bool buildCapabilitiesPayload(const InverterCapabilities& capabilities, std::string& out,
                                     size_t maxBytes) {
    JsonDocument doc;
    doc["read_only"]   = capabilities.isReadOnly();
    doc["phase_count"] = capabilities.phaseCount;
    doc["mppt_count"]  = capabilities.mpptCount;
    doc["has_battery"] = capabilities.hasBattery;

    JsonArray read = doc["read"].to<JsonArray>();
    for (size_t i = 0; i < kCapabilityCount; ++i) {
        if (capabilities.read.test(i)) {
            read.add(capabilityName(static_cast<InverterCapability>(i)));
        }
    }
    JsonArray write = doc["write"].to<JsonArray>();
    for (size_t i = 0; i < kCapabilityCount; ++i) {
        if (capabilities.write.test(i)) {
            write.add(capabilityName(static_cast<InverterCapability>(i)));
        }
    }
    // Numeric bounds, so a client can render a writable driver's min/max/step for bring-up
    // rather than only discover that the command exists.
    JsonObject numeric = doc["numeric"].to<JsonObject>();
    for (size_t i = 0; i < kCommandTypeCount; ++i) {
        const NumericCapability& cap = capabilities.numeric[i];
        if (!cap.supported) {
            continue;
        }
        JsonObject entry = numeric[commandTypeName(static_cast<InverterCommandType>(i))]
                               .to<JsonObject>();
        entry["writable"] = cap.writable;
        entry["minimum"]  = cap.minimum;
        entry["maximum"]  = cap.maximum;
        entry["step"]     = cap.step;
        entry["unit"]     = unitSymbol(cap.unit);
    }
    return finish(doc, out, maxBytes);
}

}  // namespace heliograph::json_util
