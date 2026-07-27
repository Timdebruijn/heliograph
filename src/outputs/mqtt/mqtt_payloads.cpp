// SPDX-License-Identifier: MIT

#include "mqtt_payloads.h"

#include <ArduinoJson.h>

#include <optional>

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
    // Re-taken rather than reusing `root` from above: `root` was captured before the whole
    // measurements object was built, and whether a JsonObject handle survives the document
    // growing is an ArduinoJson internal nobody should have to be sure about to read this.
    // Re-taking costs nothing and matches what the REST builder does.
    writeDeviceStatus(doc.as<JsonObject>(), state);

    return finish(doc, out, maxBytes);
}

bool buildDiagnosticsPayload(const DiagnosticsSnapshot& d, const BridgeInfo& bridge,
                             std::string& out, size_t maxBytes) {
    JsonDocument doc;
    // No board name and no mqtt_connected: the board is already the Home Assistant device
    // model, and a payload that arrived over MQTT cannot report anything but true. See
    // json_util::writeDiagnostics for why those two are the caller's choice.
    json_util::writeDiagnostics(doc.to<JsonObject>(), d, bridge, {}, std::nullopt);
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
