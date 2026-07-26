// SPDX-License-Identifier: MIT

#include "mqtt_payloads.h"

#include <ArduinoJson.h>

#include "device/command.h"  // commandTypeName
#include "outputs/json_util.h"

namespace heliograph::mqtt {
namespace {

using json_util::addOptional;
using json_util::finish;
using json_util::writeDeviceStatus;
using json_util::writeMeasurement;

}  // namespace

bool buildStatePayload(const DeviceState& state, std::string& out, size_t maxBytes) {
    JsonDocument doc;

    doc["bridge_online"]   = state.bridgeOnline;
    doc["inverter_online"] = state.inverterOnline;
    doc["data_valid"]      = state.dataValid;
    doc["data_stale"]      = state.dataStale;

    JsonObject root = doc.as<JsonObject>();
    addOptional(root, "driver_id", state.identity.driverId);
    addOptional(root, "manufacturer", state.identity.manufacturer);
    addOptional(root, "model", state.identity.model);
    addOptional(root, "serial_number", state.identity.serialNumber);
    doc["last_successful_poll_ms"] = state.lastSuccessfulPollMs;

    JsonObject measurements = doc["measurements"].to<JsonObject>();
    for (const auto& m : state.measurements.all()) {
        if (!m.supported) {
            continue;  // the driver never provides this; do not mention it at all
        }
        writeMeasurement(measurements[m.id].to<JsonObject>(), m);
    }
    writeDeviceStatus(root, state);

    return finish(doc, out, maxBytes);
}

bool buildDiagnosticsPayload(const DiagnosticsSnapshot& d, const BridgeInfo& bridge,
                             std::string& out, size_t maxBytes) {
    JsonDocument doc;
    doc["uptime_seconds"]            = bridge.uptimeSeconds;
    doc["firmware_version"]          = bridge.firmwareVersion;
    doc["free_heap_bytes"]           = bridge.freeHeapBytes;
    doc["minimum_free_heap_bytes"]   = bridge.minFreeHeapBytes;
    doc["max_alloc_heap_bytes"]      = bridge.maxAllocHeapBytes;
    doc["reset_reason"]              = bridge.resetReason;
    doc["ota_image_state"]           = bridge.otaImageState;
    doc["wifi_connected"]            = bridge.wifiConnected;
    // Only meaningful while associated; 0 dBm would look like an excellent signal.
    if (bridge.wifiConnected) {
        doc["wifi_rssi_dbm"] = bridge.wifiRssiDbm;
    } else {
        doc["wifi_rssi_dbm"] = nullptr;
    }
    json_util::addClockFields(doc.as<JsonObject>(), bridge);
    doc["poll_success_total"]        = d.pollSuccessTotal;
    doc["poll_failure_total"]        = d.pollFailureTotal;
    doc["consecutive_poll_failures"] = d.consecutivePollFailures;
    doc["checksum_error_total"]      = d.checksumErrorTotal;
    doc["rs485_timeout_total"]       = d.rs485TimeoutTotal;
    doc["invalid_frame_total"]       = d.invalidFrameTotal;
    doc["wifi_reconnect_total"]      = d.wifiReconnectTotal;
    doc["mqtt_reconnect_total"]      = d.mqttReconnectTotal;
    doc["modbus_client_connections_total"] = d.modbusClientConnections;
    doc["rest_requests_total"]       = d.restRequestTotal;
    doc["last_successful_poll_ms"]   = d.lastSuccessfulPollMs;
    // Null until the first sample, not 0: same reasoning as the RSSI field above.
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
    return finish(doc, out, maxBytes);
}

bool buildIdentityPayload(const DeviceIdentity& identity, std::string& out, size_t maxBytes) {
    JsonDocument doc;
    JsonObject   root = doc.to<JsonObject>();
    addOptional(root, "manufacturer", identity.manufacturer);
    addOptional(root, "model", identity.model);
    addOptional(root, "serial_number", identity.serialNumber);
    addOptional(root, "firmware_version", identity.firmwareVersion);
    addOptional(root, "hardware_version", identity.hardwareVersion);
    addOptional(root, "protocol_name", identity.protocolName);
    addOptional(root, "protocol_version", identity.protocolVersion);
    addOptional(root, "driver_id", identity.driverId);
    root["device_id"] = identity.deviceId();
    return finish(doc, out, maxBytes);
}

}  // namespace heliograph::mqtt
