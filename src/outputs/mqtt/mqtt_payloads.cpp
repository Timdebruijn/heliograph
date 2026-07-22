// SPDX-License-Identifier: MIT

#include "mqtt_payloads.h"

#include <ArduinoJson.h>

#include "device/command.h"  // commandTypeName
#include "outputs/json_util.h"

namespace heliograph::mqtt {
namespace {

using json_util::addOptional;
using json_util::finish;

}  // namespace

const char* capabilityName(InverterCapability capability) {
    switch (capability) {
        case InverterCapability::ReadAcPower:              return "read_ac_power";
        case InverterCapability::ReadAcVoltage:            return "read_ac_voltage";
        case InverterCapability::ReadAcCurrent:            return "read_ac_current";
        case InverterCapability::ReadGridFrequency:        return "read_grid_frequency";
        case InverterCapability::ReadDcPower:              return "read_dc_power";
        case InverterCapability::ReadDcVoltage:            return "read_dc_voltage";
        case InverterCapability::ReadDcCurrent:            return "read_dc_current";
        case InverterCapability::ReadEnergyToday:          return "read_energy_today";
        case InverterCapability::ReadEnergyTotal:          return "read_energy_total";
        case InverterCapability::ReadTemperature:          return "read_temperature";
        case InverterCapability::ReadOperatingHours:       return "read_operating_hours";
        case InverterCapability::ReadStatus:               return "read_status";
        case InverterCapability::ReadErrors:               return "read_errors";
        case InverterCapability::ReadMultiplePhases:       return "read_multiple_phases";
        case InverterCapability::ReadMultipleMppts:        return "read_multiple_mppts";
        case InverterCapability::ReadBatteryState:         return "read_battery_state";
        case InverterCapability::SetActivePowerLimit:      return "set_active_power_limit";
        case InverterCapability::SetExportLimit:           return "set_export_limit";
        case InverterCapability::StartStop:                return "start_stop";
        case InverterCapability::SetReactivePower:         return "set_reactive_power";
        case InverterCapability::SetBatteryChargeLimit:    return "set_battery_charge_limit";
        case InverterCapability::SetBatteryDischargeLimit: return "set_battery_discharge_limit";
        case InverterCapability::SetBatteryOperatingMode:  return "set_battery_operating_mode";
        case InverterCapability::SetMinimumSoc:            return "set_minimum_soc";
        case InverterCapability::SetMaximumSoc:            return "set_maximum_soc";
        case InverterCapability::SynchronizeTime:          return "synchronize_time";
        case InverterCapability::_Count:                   break;
    }
    return "unknown";
}

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
        JsonObject entry = measurements[m.id].to<JsonObject>();
        const bool usable = m.valid && !m.stale;
        if (usable) {
            entry["value"] = m.value;
        } else {
            // null, never 0. Home Assistant maps this to "unknown" and keeps it out of the
            // statistics; a zero would be recorded as a genuine reading.
            entry["value"] = nullptr;
        }
        entry["unit"]  = unitSymbol(m.unit);
        entry["valid"] = m.valid;
        entry["stale"] = m.stale;
        if (m.derived) {
            entry["derived"] = true;
        }
    }

    const bool statusUsable = state.dataValid && !state.dataStale;
    if (statusUsable) {
        doc["status_code"] = state.statusCode;
        // Absent-as-null, mirroring the REST payload: a driver with no status text for its
        // protocol must not surface as an empty string in Home Assistant.
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
        // A protocol with no error code field must not report 0: that asserts "no fault".
        doc["error_code"] = nullptr;
    }

    return finish(doc, out, maxBytes);
}

bool buildDiagnosticsPayload(const DiagnosticsSnapshot& d, const BridgeInfo& bridge,
                             std::string& out, size_t maxBytes) {
    JsonDocument doc;
    doc["uptime_seconds"]            = bridge.uptimeSeconds;
    doc["firmware_version"]          = bridge.firmwareVersion;
    doc["free_heap_bytes"]           = bridge.freeHeapBytes;
    doc["minimum_free_heap_bytes"]   = bridge.minFreeHeapBytes;
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
            read.add(capabilityName(static_cast<InverterCapability>(i)));
        }
    }
    JsonArray write = doc["write"].to<JsonArray>();
    for (size_t i = 0; i < kCapabilityCount; ++i) {
        if (capabilities.write.test(i)) {
            write.add(capabilityName(static_cast<InverterCapability>(i)));
        }
    }

    JsonObject numeric = doc["numeric"].to<JsonObject>();
    for (size_t i = 0; i < kCommandTypeCount; ++i) {
        const NumericCapability& cap = capabilities.numeric[i];
        if (!cap.supported) {
            continue;
        }
        const auto type  = static_cast<InverterCommandType>(i);
        JsonObject entry = numeric[commandTypeName(type)].to<JsonObject>();
        entry["writable"] = cap.writable;
        entry["minimum"]  = cap.minimum;
        entry["maximum"]  = cap.maximum;
        entry["step"]     = cap.step;
        entry["unit"]     = unitSymbol(cap.unit);
    }
    return finish(doc, out, maxBytes);
}

}  // namespace heliograph::mqtt
