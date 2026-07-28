// SPDX-License-Identifier: MIT
//
// Shared JSON serialisation helpers for the output adapters. Both the MQTT and REST payload
// builders had a byte-for-byte copy of these; one definition now.

#pragma once

#include <ArduinoJson.h>

#include <optional>
#include <string>

#include "json_limits.h"

#include "device/bridge_info.h"
#include "device/capability.h"
#include "device/command.h"
#include "device/device_state.h"
#include "diagnostics/coredump.h"
#include "diagnostics/diagnostics.h"
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

/// Everything both diagnostics payloads report, written once.
///
/// They were a copy of each other -- same fields, same order, same null-not-zero rules, right
/// down to a word-for-word comment. That is the failure mode this header exists to prevent: a
/// field added to one output and silently missing from the other, with nothing failing to say
/// so. It escaped the earlier consolidation because the two builders differ in TWO fields, and
/// two differences were enough to make them look like separate functions.
///
/// Those two differences are deliberate and are preserved exactly, which is why they are
/// parameters rather than something this function decides:
///   - `boardName` empty omits `board`. MQTT already carries the board as the Home Assistant
///     device model, so repeating it on the diagnostics topic says nothing new.
///   - `mqttConnected` unset omits `mqtt_connected`. On a payload that arrived over MQTT the
///     field can only ever say true; it is a fact worth reporting over REST and a tautology
///     over MQTT.
///
/// Field order is preserved for both callers, so no consumer sees a changed document.
inline void writeDiagnostics(JsonObject doc, const DiagnosticsSnapshot& d,
                             const BridgeInfo& bridge, const std::string& boardName,
                             std::optional<bool> mqttConnected) {
    doc["uptime_seconds"]   = bridge.uptimeSeconds;
    doc["firmware_version"] = bridge.firmwareVersion;
    addOptional(doc, "board", boardName);
    doc["free_heap_bytes"]         = bridge.freeHeapBytes;
    doc["minimum_free_heap_bytes"] = bridge.minFreeHeapBytes;
    doc["max_alloc_heap_bytes"]    = bridge.maxAllocHeapBytes;

    // Absent, not zero, when the board has none -- and 0 is a legitimate reading for
    // psram_free_bytes on a board that HAS PSRAM and has exhausted it, so the two must not
    // collapse onto the same value. Reported at all because the three heap figures above are
    // MALLOC_CAP_INTERNAL and say nothing about it (audit, 2026-07-26).
    if (bridge.psramSizeBytes > 0) {
        doc["psram_size_bytes"] = bridge.psramSizeBytes;
        doc["psram_free_bytes"] = bridge.psramFreeBytes;
    } else {
        doc["psram_size_bytes"] = nullptr;
        doc["psram_free_bytes"] = nullptr;
    }
    doc["reset_reason"]    = bridge.resetReason;
    doc["ota_image_state"] = bridge.otaImageState;

    // Absent, not zero, when no dump is stored: task "" at PC 0 is not a fact about anything,
    // and `coredump_present` false already carries the whole message.
    doc["coredump_present"] = bridge.coredumpPresent;
    if (bridge.coredumpPresent && !bridge.coredumpTask.empty()) {
        doc["coredump_task"] = bridge.coredumpTask;  // std::string: copied into the document
    } else {
        doc["coredump_task"] = nullptr;
    }
    if (bridge.coredumpPresent) {
        doc["coredump_pc"] = bridge.coredumpPc;
    } else {
        doc["coredump_pc"] = nullptr;
    }
    // Why it faulted, and what it was reaching for. Shared with MQTT on purpose: two short
    // fields are enough for a Home Assistant notification to say "the bridge crashed on a null
    // dereference" rather than "the bridge crashed". The BACKTRACE is deliberately not here --
    // sixteen addresses republished every diagnostics interval, never changing, for something
    // nobody reads in Home Assistant. It lives on the REST payload, which is fetched on purpose.
    //
    // Null, not 0, when the dump carries no extra info: 0 is a real cause (IllegalInstruction)
    // and a real address, so neither can double as "unknown".
    if (bridge.coredumpPresent && bridge.coredumpCauseKnown) {
        const char* name       = diag::exceptionCauseName(bridge.coredumpCause);
        doc["coredump_cause"]  = bridge.coredumpCause;
        // The number stays alongside the name: a cause this table does not know is still worth
        // reporting, and a reader with the Xtensa manual can look it up.
        if (name != nullptr) {
            doc["coredump_cause_name"] = name;
        } else {
            doc["coredump_cause_name"] = nullptr;
        }
        doc["coredump_fault_address"] = bridge.coredumpFaultAddress;
    } else {
        doc["coredump_cause"]         = nullptr;
        doc["coredump_cause_name"]    = nullptr;
        doc["coredump_fault_address"] = nullptr;
    }
    doc["wifi_connected"] = bridge.wifiConnected;
    // Only meaningful while associated; 0 dBm would look like an excellent signal.
    if (bridge.wifiConnected) {
        doc["wifi_rssi_dbm"] = bridge.wifiRssiDbm;
    } else {
        doc["wifi_rssi_dbm"] = nullptr;
    }
    if (mqttConnected.has_value()) {
        doc["mqtt_connected"] = *mqttConnected;
    }
    addClockFields(doc, bridge);

    doc["poll_success_total"]              = d.pollSuccessTotal;
    doc["poll_failure_total"]              = d.pollFailureTotal;
    doc["consecutive_poll_failures"]       = d.consecutivePollFailures;
    doc["checksum_error_total"]            = d.checksumErrorTotal;
    doc["rs485_timeout_total"]             = d.rs485TimeoutTotal;
    doc["invalid_frame_total"]             = d.invalidFrameTotal;
    doc["wifi_reconnect_total"]            = d.wifiReconnectTotal;
    doc["mqtt_reconnect_total"]            = d.mqttReconnectTotal;
    doc["modbus_client_connections_total"] = d.modbusClientConnections;
    doc["rest_requests_total"]             = d.restRequestTotal;
    doc["mqtt_publish_failure_total"]      = d.mqttPublishFailureTotal;
    doc["last_successful_poll_ms"]         = d.lastSuccessfulPollMs;
    // Null until the first sample, not 0: a monitoring rule on "stack headroom == 0" must not
    // fire during the first seconds after boot.
    if (d.rs485StackFreeBytes > 0) {
        doc["rs485_stack_free_bytes"] = d.rs485StackFreeBytes;
    } else {
        doc["rs485_stack_free_bytes"] = nullptr;
    }
    if (d.loopStackFreeBytes > 0) {
        doc["loop_stack_free_bytes"] = d.loopStackFreeBytes;
    } else {
        doc["loop_stack_free_bytes"] = nullptr;
    }
    // Set from pollResultName() and friends only. Never carries payload bytes or config.
    doc["last_error"] = d.lastError;
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
