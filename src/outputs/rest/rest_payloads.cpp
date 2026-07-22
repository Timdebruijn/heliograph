// SPDX-License-Identifier: MIT

#include "rest_payloads.h"

#include <ArduinoJson.h>

#include "outputs/json_util.h"
#include "outputs/mqtt/mqtt_payloads.h"  // capabilityName, shared vocabulary

namespace heliograph::rest {
namespace {

using json_util::addOptional;
using json_util::finish;

/// One measurement, with the same null-not-zero rule the MQTT payload uses.
void writeMeasurement(JsonObject entry, const Measurement& m) {
    const bool usable = m.valid && !m.stale;
    if (usable) {
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

}  // namespace

bool buildErrorPayload(const ApiError& error, const std::string& requestId, std::string& out) {
    JsonDocument doc;
    JsonObject   e = doc["error"].to<JsonObject>();
    e["code"]      = error.code;
    e["message"]   = error.message;
    if (!requestId.empty()) {
        e["request_id"] = requestId;
    }
    return finish(doc, out, 512);
}

bool buildStatusPayload(const DeviceState& state, const std::string& deviceId,
                        const BridgeInfo& bridge, const DiagnosticsSnapshot& diagnostics,
                        const DriverDescriptor* driver, uint64_t nowMs, std::string& out,
                        size_t maxBytes) {
    JsonDocument doc;

    JsonObject b            = doc["bridge"].to<JsonObject>();
    b["id"]                 = bridge.bridgeId;
    b["name"]               = bridge.name;
    b["firmware_version"]   = bridge.firmwareVersion;
    b["uptime_seconds"]     = bridge.uptimeSeconds;
    b["wifi_connected"]     = bridge.wifiConnected;
    if (bridge.wifiConnected) {
        b["wifi_rssi_dbm"] = bridge.wifiRssiDbm;
    } else {
        b["wifi_rssi_dbm"] = nullptr;  // 0 dBm would read as a perfect signal
    }
    b["mqtt_connected"]    = bridge.mqttConnected;
    b["modbus_listening"]  = bridge.modbusListening;
    b["modbus_clients"]    = bridge.modbusClients;
    json_util::addClockFields(b, bridge);
    b["free_heap_bytes"]   = bridge.freeHeapBytes;

    // Only on boards that have relays: absent, not an empty list, per the house rule that
    // hardware which does not exist is never reported as a zero-ish value.
    if (bridge.relayCount > 0) {
        b["relays_enabled"] = bridge.relaysEnabled;
        JsonArray relays    = b["relays"].to<JsonArray>();
        for (uint8_t i = 0; i < bridge.relayCount; ++i) {
            relays.add(((bridge.relayMask >> i) & 1) != 0);
        }
    }

    JsonObject d = doc["device"].to<JsonObject>();
    d["id"]      = deviceId;  // the registered id, not identity.deviceId() -- see the header
    addOptional(d, "driver_id", state.identity.driverId);
    if (driver != nullptr) {
        d["support_level"] = supportLevelName(driver->supportLevel);
    }
    addOptional(d, "manufacturer", state.identity.manufacturer);
    addOptional(d, "model", state.identity.model);
    addOptional(d, "serial_number", state.identity.serialNumber);
    d["online"]     = state.inverterOnline;
    d["data_valid"] = state.dataValid;
    d["data_stale"] = state.dataStale;
    d["consecutive_poll_failures"] = state.consecutiveFailures;
    if (state.lastSuccessfulPollMs == 0) {
        // Never polled successfully. 0 would read as "just now".
        d["last_successful_poll_seconds_ago"] = nullptr;
    } else {
        d["last_successful_poll_seconds_ago"] =
            static_cast<uint32_t>((nowMs - state.lastSuccessfulPollMs) / 1000);
    }

    JsonObject measurements = doc["measurements"].to<JsonObject>();
    for (const auto& m : state.measurements.all()) {
        if (!m.supported) {
            continue;
        }
        writeMeasurement(measurements[m.id].to<JsonObject>(), m);
    }

    const bool statusUsable = state.dataValid && !state.dataStale;
    if (statusUsable) {
        doc["status_code"] = state.statusCode;
        // Empty means the driver has no text for this protocol -- same rule as identity
        // fields: absent-as-null, never an empty string the UI would render as a blank tile.
        if (state.statusText.empty()) {
            doc["status_text"] = nullptr;
        } else {
            doc["status_text"] = state.statusText;
        }
    } else {
        doc["status_code"] = nullptr;
        doc["status_text"] = nullptr;
    }
    if (state.errorCodeSupported && statusUsable) {
        doc["error_code"] = state.errorCode;
    } else {
        doc["error_code"] = nullptr;
    }

    doc["poll_success_total"] = diagnostics.pollSuccessTotal;
    doc["poll_failure_total"] = diagnostics.pollFailureTotal;
    return finish(doc, out, maxBytes);
}

bool buildDevicesPayload(const std::vector<std::string>& deviceIds, std::string& out,
                         size_t maxBytes) {
    JsonDocument doc;
    JsonArray    arr = doc["devices"].to<JsonArray>();
    for (const auto& id : deviceIds) {
        arr.add(id);
    }
    return finish(doc, out, maxBytes);
}

bool buildDevicePayload(const DeviceState& state, const std::string& deviceId,
                        const DriverDescriptor* driver, std::string& out, size_t maxBytes) {
    JsonDocument doc;
    doc["id"] = deviceId;

    JsonObject identity = doc["identity"].to<JsonObject>();
    addOptional(identity, "manufacturer", state.identity.manufacturer);
    addOptional(identity, "model", state.identity.model);
    addOptional(identity, "serial_number", state.identity.serialNumber);
    addOptional(identity, "firmware_version", state.identity.firmwareVersion);
    addOptional(identity, "hardware_version", state.identity.hardwareVersion);
    addOptional(identity, "protocol_name", state.identity.protocolName);
    addOptional(identity, "driver_id", state.identity.driverId);

    if (driver != nullptr) {
        JsonObject drv     = doc["driver"].to<JsonObject>();
        drv["id"]          = driver->id;
        drv["display_name"] = driver->displayName;
        drv["protocol"]    = driver->protocol;
        drv["support_level"] = supportLevelName(driver->supportLevel);
        drv["supports_write"] = driver->supportsWrite;
    }

    doc["online"]     = state.inverterOnline;
    doc["data_valid"] = state.dataValid;
    doc["data_stale"] = state.dataStale;
    return finish(doc, out, maxBytes);
}

bool buildMeasurementsPayload(const DeviceState& state, std::string& out, size_t maxBytes) {
    JsonDocument doc;
    JsonObject   measurements = doc["measurements"].to<JsonObject>();
    for (const auto& m : state.measurements.all()) {
        if (!m.supported) {
            continue;
        }
        JsonObject entry = measurements[m.id].to<JsonObject>();
        entry["display_name"] = m.displayName;
        writeMeasurement(entry, m);
        entry["timestamp_ms"] = m.timestampMs;
    }
    return finish(doc, out, maxBytes);
}

bool buildCapabilitiesPayload(const InverterCapabilities& capabilities, std::string& out,
                              size_t maxBytes) {
    JsonDocument doc;
    doc["read_only"]   = capabilities.isReadOnly();
    doc["phase_count"] = capabilities.phaseCount;
    doc["mppt_count"]  = capabilities.mpptCount;
    doc["has_battery"] = capabilities.hasBattery;

    JsonArray read = doc["read"].to<JsonArray>();
    for (size_t i = 0; i < kCapabilityCount; ++i) {
        if (capabilities.read.test(i)) {
            read.add(mqtt::capabilityName(static_cast<InverterCapability>(i)));
        }
    }
    JsonArray write = doc["write"].to<JsonArray>();
    for (size_t i = 0; i < kCapabilityCount; ++i) {
        if (capabilities.write.test(i)) {
            write.add(mqtt::capabilityName(static_cast<InverterCapability>(i)));
        }
    }
    // Numeric bounds, matching the MQTT capabilities payload -- so a REST/web client can render
    // a writable driver's min/max/step for bring-up, not just discover the command exists.
    JsonObject numeric = doc["numeric"].to<JsonObject>();
    for (size_t i = 0; i < kCommandTypeCount; ++i) {
        const NumericCapability& cap = capabilities.numeric[i];
        if (!cap.supported) {
            continue;
        }
        JsonObject entry  = numeric[commandTypeName(static_cast<InverterCommandType>(i))].to<JsonObject>();
        entry["writable"] = cap.writable;
        entry["minimum"]  = cap.minimum;
        entry["maximum"]  = cap.maximum;
        entry["step"]     = cap.step;
        entry["unit"]     = unitSymbol(cap.unit);
    }
    return finish(doc, out, maxBytes);
}

bool buildDiagnosticsPayload(const DiagnosticsSnapshot& d, const BridgeInfo& bridge,
                             std::string& out, size_t maxBytes) {
    JsonDocument doc;
    doc["uptime_seconds"]          = bridge.uptimeSeconds;
    doc["firmware_version"]        = bridge.firmwareVersion;
    doc["board"]                   = bridge.boardName;
    doc["free_heap_bytes"]         = bridge.freeHeapBytes;
    doc["minimum_free_heap_bytes"] = bridge.minFreeHeapBytes;
    doc["reset_reason"]            = bridge.resetReason;
    doc["ota_image_state"]         = bridge.otaImageState;
    doc["wifi_connected"]          = bridge.wifiConnected;
    if (bridge.wifiConnected) {
        doc["wifi_rssi_dbm"] = bridge.wifiRssiDbm;
    } else {
        doc["wifi_rssi_dbm"] = nullptr;
    }
    doc["mqtt_connected"]                  = bridge.mqttConnected;
    json_util::addClockFields(doc.as<JsonObject>(), bridge);
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
    doc["last_successful_poll_ms"]         = d.lastSuccessfulPollMs;
    doc["last_error"]                      = d.lastError;
    return finish(doc, out, maxBytes);
}

bool buildDiscoveryPayload(const DiscoveryReport& report, uint64_t nowMs, std::string& out,
                           size_t maxBytes) {
    JsonDocument doc;
    doc["status"] = discoveryStatusName(report.status);
    doc["mode"]   = report.mode == DiscoveryMode::Extended ? "extended" : "quick";
    doc["busy"]   = report.status == DiscoveryStatus::Requested ||
                  report.status == DiscoveryStatus::Running;

    if (report.startedMs > 0) {
        const uint64_t until =
            report.finishedMs > 0 ? report.finishedMs : (nowMs > report.startedMs ? nowMs : report.startedMs);
        doc["elapsed_ms"] = until - report.startedMs;
    }
    if (!report.error.empty()) {
        doc["error"] = report.error;
    }

    if (report.status == DiscoveryStatus::Done) {
        doc["auto_selected"] = report.outcome.autoSelected;
        // Always present, even when nothing was chosen: the wizard shows this verbatim so the
        // user can judge an ambiguous result themselves rather than be told "failed".
        doc["reason"] = report.outcome.reason;
        if (report.outcome.autoSelected) {
            doc["selected_driver_id"] = report.outcome.selectedDriverId;
        } else {
            doc["selected_driver_id"] = nullptr;
        }

        JsonArray candidates = doc["candidates"].to<JsonArray>();
        for (const auto& c : report.outcome.candidates) {
            JsonObject e            = candidates.add<JsonObject>();
            e["driver_id"]          = c.descriptor.id;
            e["display_name"]       = c.descriptor.displayName;
            e["manufacturer"]       = c.descriptor.manufacturer;
            e["protocol"]           = c.descriptor.protocol;
            e["support_level"]      = supportLevelName(c.descriptor.supportLevel);
            e["confidence"]         = c.probe.confidenceScore;
            e["consistent"]         = c.consistent;
            e["responded"]          = c.probe.responded;
            e["checksum_valid"]     = c.probe.checksumValid;
            addOptional(e, "detected_manufacturer", c.probe.detectedManufacturer);
            addOptional(e, "detected_model", c.probe.detectedModel);
            addOptional(e, "serial_number", c.probe.serialNumber);
            addOptional(e, "firmware_version", c.probe.firmwareVersion);
            if (!c.descriptor.recommendedSerialProfiles.empty()) {
                const auto& p        = c.descriptor.recommendedSerialProfiles.front();
                JsonObject  profile  = e["serial_profile"].to<JsonObject>();
                profile["baud_rate"] = p.baudRate;
                profile["parity"]    = parityName(p.parity);
                profile["data_bits"] = p.dataBits;
                profile["stop_bits"] = p.stopBits;
                profile["response_timeout_ms"] = p.responseTimeoutMs;
            }
            JsonArray evidence = e["evidence"].to<JsonArray>();
            for (const auto& line : c.probe.evidence) {
                evidence.add(line);
            }
        }
    }
    return finish(doc, out, maxBytes);
}

bool buildDriversPayload(const std::vector<DriverDescriptor>& drivers, std::string& out,
                         size_t maxBytes) {
    JsonDocument doc;
    JsonArray    arr = doc["drivers"].to<JsonArray>();
    for (const auto& d : drivers) {
        JsonObject e         = arr.add<JsonObject>();
        e["id"]              = d.id;
        e["display_name"]    = d.displayName;
        e["manufacturer"]    = d.manufacturer;
        e["protocol"]        = d.protocol;
        e["support_level"]   = supportLevelName(d.supportLevel);
        e["supports_read"]   = d.supportsRead;
        e["supports_write"]  = d.supportsWrite;
        e["auto_detection"]  = d.supportsAutoDetection;
        JsonArray profiles   = e["serial_profiles"].to<JsonArray>();
        for (const auto& p : d.recommendedSerialProfiles) {
            JsonObject pr = profiles.add<JsonObject>();
            pr["baud_rate"] = p.baudRate;
            pr["parity"]    = parityName(p.parity);
            pr["data_bits"] = p.dataBits;
            pr["stop_bits"] = p.stopBits;
        }
        // Driver-declared settings. The web form renders these generically, so a new driver's
        // options show up in the UI without a line of frontend work.
        JsonArray options = e["options"].to<JsonArray>();
        for (const auto& o : d.options) {
            JsonObject oo        = options.add<JsonObject>();
            oo["key"]            = o.key;
            oo["display_name"]   = o.displayName;
            oo["description"]    = o.description;
            oo["default_value"]  = o.defaultValue;
            JsonArray allowed    = oo["allowed_values"].to<JsonArray>();
            for (const auto& v : o.allowedValues) {
                allowed.add(v);
            }
        }
    }
    return finish(doc, out, maxBytes);
}

bool buildLogsPayload(const std::vector<std::string>& lines, uint32_t totalLines,
                      const std::string& level, std::string& out, size_t maxBytes) {
    JsonDocument doc;
    // `total` versus the returned count is how a reader spots that the ring wrapped and lines
    // were lost, rather than assuming a quiet bus.
    doc["total"]    = totalLines;
    doc["returned"] = lines.size();
    doc["level"]    = level;
    JsonArray arr   = doc["lines"].to<JsonArray>();
    for (const auto& l : lines) {
        arr.add(l);
    }
    return finish(doc, out, maxBytes);
}

}  // namespace heliograph::rest
