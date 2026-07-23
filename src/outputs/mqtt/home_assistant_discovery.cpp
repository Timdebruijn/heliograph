// SPDX-License-Identifier: MIT

#include "home_assistant_discovery.h"

#include <ArduinoJson.h>

#include <cctype>

#include "relays/drm.h"

namespace heliograph::mqtt {
namespace {

/// Used when a driver reports neither a model nor a manufacturer.
const std::string kUnknownInverterName = "Inverter";

/// MeasurementType -> Home Assistant device_class. The entire brand-agnostic mapping.
const char* deviceClassFor(MeasurementType type) {
    switch (type) {
        case MeasurementType::Power:          return "power";
        case MeasurementType::Voltage:        return "voltage";
        case MeasurementType::Current:        return "current";
        case MeasurementType::Frequency:      return "frequency";
        case MeasurementType::Temperature:    return "temperature";
        case MeasurementType::Energy:         return "energy";
        case MeasurementType::Duration:       return "duration";
        case MeasurementType::SignalStrength: return "signal_strength";
        case MeasurementType::Ratio:          return "battery";  // SoC-style percentages
        case MeasurementType::Generic:        break;
    }
    return nullptr;
}

/// Cumulative counters get total_increasing so Home Assistant's energy dashboard treats them
/// as meters; everything else is an instantaneous measurement.
const char* stateClassFor(MeasurementType type) {
    switch (type) {
        case MeasurementType::Energy:
        case MeasurementType::Duration:
            return "total_increasing";
        case MeasurementType::Power:
        case MeasurementType::Voltage:
        case MeasurementType::Current:
        case MeasurementType::Frequency:
        case MeasurementType::Temperature:
        case MeasurementType::Ratio:
        case MeasurementType::SignalStrength:
            return "measurement";
        case MeasurementType::Generic:
            break;
    }
    return nullptr;
}

/// Display precision, in decimals, or -1 for "let Home Assistant decide".
///
/// Home Assistant assigns a per-device-class default to most sensors -- power 0, voltage 0,
/// current 2, energy 2, temperature 1 -- but NOT to the battery device_class, so a raw
/// 74.54152672 % went straight to the dashboard while every other tile was already rounded.
/// This closes that gap and, by stating the intent for every type, stops the appearance from
/// depending on which Home Assistant version happens to be running.
///
/// This is display only. Home Assistant keeps the full-resolution value for history and the
/// energy dashboard; nothing here rounds what the bridge actually measured.
int displayPrecisionFor(MeasurementType type) {
    switch (type) {
        case MeasurementType::Power:          return 0;
        case MeasurementType::Frequency:      return 0;
        case MeasurementType::Voltage:        return 0;
        case MeasurementType::SignalStrength: return 0;
        case MeasurementType::Ratio:          return 0;  // battery SoC and the like: whole %
        case MeasurementType::Current:        return 2;
        case MeasurementType::Energy:         return 2;
        case MeasurementType::Temperature:    return 1;
        case MeasurementType::Duration:       return 0;
        case MeasurementType::Generic:        break;
    }
    return -1;
}

/// Turns "ac.phase_l1.voltage" into "AC Phase L1 Voltage" when the driver gave no name.
std::string humanise(const std::string& id) {
    std::string out;
    bool        upper = true;
    for (const char c : id) {
        if (c == '.' || c == '_') {
            out.push_back(' ');
            upper = true;
            continue;
        }
        out.push_back(upper ? static_cast<char>(::toupper(c)) : c);
        upper = false;
    }
    return out;
}

void addDeviceBlock(JsonObject entity, const BridgeInfo& bridge, const DeviceIdentity& identity,
                    bool isBridgeEntity) {
    JsonObject device = entity["device"].to<JsonObject>();
    if (isBridgeEntity) {
        device["identifiers"].to<JsonArray>().add(bridge.bridgeId);
        device["name"]         = bridge.name;
        device["manufacturer"] = "Heliograph open-source project";
        device["model"]        = bridge.boardName;
        device["sw_version"]   = bridge.firmwareVersion;
        return;
    }

    // The inverter is modelled as its own device behind the bridge, so that a second inverter
    // later simply appears alongside it rather than merging into one confused device.
    device["identifiers"].to<JsonArray>().add(bridge.bridgeId + "_inverter");
    // Model, not manufacturer: the manufacturer is already its own field, and repeating it here
    // produced names like "Heliograph - Heliograph open-source project". The model is what
    // distinguishes one inverter from the next, which is the whole point of the name.
    const std::string& descriptor = !identity.model.empty()          ? identity.model
                                    : !identity.manufacturer.empty() ? identity.manufacturer
                                                                     : kUnknownInverterName;
    device["name"] = bridge.name.empty() ? descriptor : bridge.name + " - " + descriptor;
    if (!identity.manufacturer.empty()) {
        device["manufacturer"] = identity.manufacturer;
    }
    if (!identity.model.empty()) {
        device["model"] = identity.model;
    }
    if (!identity.serialNumber.empty()) {
        device["serial_number"] = identity.serialNumber;
    }
    if (!identity.firmwareVersion.empty()) {
        device["sw_version"] = identity.firmwareVersion;
    }
    device["via_device"] = bridge.bridgeId;
}

bool serialise(const JsonDocument& doc, std::string& out) {
    if (doc.overflowed()) {
        return false;
    }
    const size_t needed = measureJson(doc);
    std::string  buffer;
    buffer.resize(needed + 1);
    buffer.resize(serializeJson(doc, buffer.data(), buffer.size()));
    out = std::move(buffer);
    return true;
}

}  // namespace

std::string sanitizeId(const std::string& measurementId) {
    std::string out;
    out.reserve(measurementId.size());
    for (const char c : measurementId) {
        out.push_back((c == '.' || c == '-') ? '_' : c);
    }
    return out;
}

namespace {

// FNV-1a, 64-bit. The '\n' fed between items keeps the hash order- and boundary-sensitive:
// {"ab","c"} and {"a","bc"} must not collide by construction.
constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime  = 1099511628211ull;

uint64_t fnv1aAppend(uint64_t hash, const char* s) {
    for (; *s != '\0'; ++s) {
        hash ^= static_cast<uint8_t>(*s);
        hash *= kFnvPrime;
    }
    hash ^= static_cast<uint8_t>('\n');
    hash *= kFnvPrime;
    return hash;
}

}  // namespace

uint64_t discoverySignature(const DeviceState& state) {
    // Only supported channels count: unsupported ones produce no entity (see below), so a
    // change in them must not trigger a republish.
    uint64_t sig = kFnvOffset;
    for (const auto& m : state.measurements.all()) {
        if (!m.supported) {
            continue;
        }
        sig = fnv1aAppend(sig, m.id);
    }
    return sig;
}

uint64_t stringListFingerprint(const std::vector<std::string>& items) {
    uint64_t sig = kFnvOffset;
    for (const auto& item : items) {
        sig = fnv1aAppend(sig, item.c_str());
    }
    return sig;
}

std::vector<DiscoveryEntity> buildDiscoveryEntities(const DeviceState& state,
                                                    const BridgeInfo&  bridge,
                                                    const MqttTopics&  topics,
                                                    const std::string& discoveryPrefix) {
    std::vector<DiscoveryEntity> entities;

    for (const auto& m : state.measurements.all()) {
        // Unsupported channels produce no entity. Not a disabled entity, not an entity
        // reporting zero: nothing. Home Assistant should only ever show what exists.
        if (!m.supported) {
            continue;
        }

        const std::string slug = sanitizeId(m.id);
        JsonDocument      doc;
        JsonObject        e = doc.to<JsonObject>();

        e["unique_id"] = bridge.bridgeId + "_" + slug;
        e["object_id"] = bridge.bridgeId + "_" + slug;
        const bool noName = m.displayName == nullptr || m.displayName[0] == '\0';
        e["name"]      = noName ? humanise(m.id) : std::string(m.displayName);
        e["state_topic"] = topics.state();
        // Yields None for a null value, which Home Assistant records as "unknown" rather
        // than as a reading of 0.
        e["value_template"] =
            std::string("{{ value_json.measurements['") + m.id + "'].value }}";
        e["availability_topic"]  = topics.availability();
        e["payload_available"]   = kPayloadOnline;
        e["payload_not_available"] = kPayloadOffline;

        if (const char* dc = deviceClassFor(m.type)) {
            e["device_class"] = dc;
        }
        if (const char* sc = stateClassFor(m.type)) {
            e["state_class"] = sc;
        }
        const char* unit = unitSymbol(m.unit);
        if (unit[0] != '\0') {
            e["unit_of_measurement"] = unit;
        }
        if (const int precision = displayPrecisionFor(m.type); precision >= 0) {
            e["suggested_display_precision"] = precision;
        }
        addDeviceBlock(e, bridge, state.identity, /*isBridgeEntity=*/false);

        DiscoveryEntity entity;
        entity.uniqueId    = e["unique_id"].as<std::string>();
        entity.configTopic = discoveryPrefix + "/sensor/" + bridge.bridgeId + "/" + slug + "/config";
        if (serialise(doc, entity.payload)) {
            entities.push_back(std::move(entity));
        }
    }

    // Status text, only when the driver actually reports status.
    if (state.capabilities.has(InverterCapability::ReadStatus)) {
        JsonDocument doc;
        JsonObject   e = doc.to<JsonObject>();
        e["unique_id"]      = bridge.bridgeId + "_status";
        e["object_id"]      = bridge.bridgeId + "_status";
        e["name"]           = "Status";
        e["state_topic"]    = topics.state();
        e["value_template"] = "{{ value_json.status_text }}";
        e["availability_topic"] = topics.availability();
        e["icon"]           = "mdi:information-outline";
        addDeviceBlock(e, bridge, state.identity, false);

        DiscoveryEntity entity;
        entity.uniqueId    = e["unique_id"].as<std::string>();
        entity.configTopic = discoveryPrefix + "/sensor/" + bridge.bridgeId + "/status/config";
        if (serialise(doc, entity.payload)) {
            entities.push_back(std::move(entity));
        }
    }

    // Inverter liveness as a binary_sensor, so a dashboard can show it without a template.
    {
        JsonDocument doc;
        JsonObject   e = doc.to<JsonObject>();
        e["unique_id"]      = bridge.bridgeId + "_inverter_online";
        e["object_id"]      = bridge.bridgeId + "_inverter_online";
        e["name"]           = "Inverter Online";
        e["state_topic"]    = topics.state();
        e["value_template"] = "{{ 'ON' if value_json.inverter_online else 'OFF' }}";
        e["availability_topic"] = topics.availability();
        e["device_class"]   = "connectivity";
        e["entity_category"] = "diagnostic";
        addDeviceBlock(e, bridge, state.identity, false);

        DiscoveryEntity entity;
        entity.uniqueId = e["unique_id"].as<std::string>();
        entity.configTopic =
            discoveryPrefix + "/binary_sensor/" + bridge.bridgeId + "/inverter_online/config";
        if (serialise(doc, entity.payload)) {
            entities.push_back(std::move(entity));
        }
    }

    // No control entities. Not an omission: the loop that would create them is gated on the
    // write bitset, which is empty for every driver in this build.
    if (!state.capabilities.isReadOnly()) {
        // Deliberately not implemented yet -- the MVP has no writable driver, so writing this
        // now would mean shipping untestable code that can move an inverter. Phase: future.
    }

    return entities;
}

std::vector<DiscoveryEntity> buildBridgeDiagnosticEntities(const BridgeInfo&  bridge,
                                                           const MqttTopics&  topics,
                                                           const std::string& discoveryPrefix) {
    struct Spec {
        const char* slug;
        const char* name;
        const char* jsonKey;
        const char* deviceClass;
        const char* stateClass;
        const char* unit;
    };
    static const Spec kSpecs[] = {
        {"wifi_rssi", "WiFi Signal", "wifi_rssi_dbm", "signal_strength", "measurement", "dBm"},
        {"uptime", "Uptime", "uptime_seconds", "duration", "total_increasing", "s"},
        {"free_heap", "Free Heap", "free_heap_bytes", "data_size", "measurement", "B"},
        {"poll_success", "Polls Succeeded", "poll_success_total", nullptr, "total_increasing", nullptr},
        {"poll_failure", "Polls Failed", "poll_failure_total", nullptr, "total_increasing", nullptr},
        {"checksum_errors", "Checksum Errors", "checksum_error_total", nullptr, "total_increasing", nullptr},
        {"rs485_timeouts", "RS485 Timeouts", "rs485_timeout_total", nullptr, "total_increasing", nullptr},
    };

    std::vector<DiscoveryEntity> entities;
    for (const auto& spec : kSpecs) {
        JsonDocument doc;
        JsonObject   e = doc.to<JsonObject>();
        e["unique_id"]      = bridge.bridgeId + "_" + spec.slug;
        e["object_id"]      = bridge.bridgeId + "_" + spec.slug;
        e["name"]           = spec.name;
        e["state_topic"]    = topics.diagnostics();
        e["value_template"] = std::string("{{ value_json.") + spec.jsonKey + " }}";
        e["availability_topic"] = topics.availability();
        e["entity_category"]    = "diagnostic";
        if (spec.deviceClass) {
            e["device_class"] = spec.deviceClass;
        }
        if (spec.stateClass) {
            e["state_class"] = spec.stateClass;
        }
        if (spec.unit) {
            e["unit_of_measurement"] = spec.unit;
        }
        addDeviceBlock(e, bridge, DeviceIdentity{}, /*isBridgeEntity=*/true);

        DiscoveryEntity entity;
        entity.uniqueId = e["unique_id"].as<std::string>();
        entity.configTopic =
            discoveryPrefix + "/sensor/" + bridge.bridgeId + "/" + spec.slug + "/config";
        if (serialise(doc, entity.payload)) {
            entities.push_back(std::move(entity));
        }
    }
    return entities;
}

std::vector<DiscoveryEntity> buildRelayEntities(const BridgeInfo&  bridge,
                                                const MqttTopics&  topics,
                                                const std::string& discoveryPrefix) {
    // Switches are announced only when the relays can actually act (board has them AND the
    // config flag is on). For a board that HAS relays but keeps them disabled, empty
    // retained payloads are published instead, so previously announced switches disappear
    // from Home Assistant rather than lingering as controls that silently reject.
    std::vector<DiscoveryEntity> entities;
    for (uint8_t i = 0; i < bridge.relayCount; ++i) {
        const std::string slug = "relay_" + std::to_string(i + 1);
        DiscoveryEntity   entity;
        entity.configTopic =
            discoveryPrefix + "/switch/" + bridge.bridgeId + "/" + slug + "/config";
        if (!bridge.relaysEnabled) {
            entity.uniqueId = bridge.bridgeId + "_" + slug;
            entity.payload.clear();  // empty retained payload = HA removes the entity
            entities.push_back(std::move(entity));
            continue;
        }
        JsonDocument doc;
        JsonObject   e = doc.to<JsonObject>();
        e["unique_id"] = bridge.bridgeId + "_" + slug;
        e["object_id"] = bridge.bridgeId + "_" + slug;
        // The configured DRM role lands in the entity name, so the HA UI says what the
        // contact MEANS ("Relay 1 (DRM0)") instead of only where it is.
        std::string name = "Relay " + std::to_string(i + 1);
        if (i < bridge.relayRoles.size() && bridge.relayRoles[i] != "none" &&
            !bridge.relayRoles[i].empty()) {
            std::string role = bridge.relayRoles[i];
            for (auto& ch : role) {
                ch = static_cast<char>(toupper(ch));
            }
            name += " (" + role + ")";
        }
        e["name"]          = name;
        e["command_topic"] = topics.relaySet(i);
        e["state_topic"]   = topics.relayState(i);
        e["payload_on"]    = "ON";
        e["payload_off"]   = "OFF";
        // Not optimistic: the switch shows the ACK'd state, so a command refused by the
        // gates (read-only mode flipped back on, rate limit) visibly snaps back in HA.
        e["optimistic"]         = false;
        e["availability_topic"] = topics.availability();
        addDeviceBlock(e, bridge, DeviceIdentity{}, /*isBridgeEntity=*/true);

        entity.uniqueId = e["unique_id"].as<std::string>();
        if (serialise(doc, entity.payload)) {
            entities.push_back(std::move(entity));
        }
    }

    // DRM mode select: one entity for "which demand-response mode is active", derived
    // entirely from the configured roles. Removal payload when disabled or role-less, so
    // reconfiguring never leaves a stale select behind.
    if (bridge.relayCount > 0) {
        std::vector<std::string> roles = bridge.relayRoles;
        roles.resize(bridge.relayCount, "none");
        const auto options = drm::optionsFor(roles);

        DiscoveryEntity entity;
        entity.uniqueId    = bridge.bridgeId + "_drm_mode";
        entity.configTopic = discoveryPrefix + "/select/" + bridge.bridgeId + "/drm_mode/config";
        if (!bridge.relaysEnabled || options.empty()) {
            entity.payload.clear();
            entities.push_back(std::move(entity));
            return entities;
        }
        JsonDocument doc;
        JsonObject   e = doc.to<JsonObject>();
        e["unique_id"]     = entity.uniqueId;
        e["object_id"]     = entity.uniqueId;
        e["name"]          = "DRM Mode";
        e["command_topic"] = topics.drmSet();
        e["state_topic"]   = topics.drmState();
        JsonArray opts     = e["options"].to<JsonArray>();
        for (const auto& o : options) {
            opts.add(o);
        }
        // "custom" is reportable state (hand-toggled switch combinations) but never a
        // command; HA requires the state to be one of the options, so it is listed.
        opts.add(drm::kModeCustom);
        e["availability_topic"] = topics.availability();
        addDeviceBlock(e, bridge, DeviceIdentity{}, /*isBridgeEntity=*/true);
        if (serialise(doc, entity.payload)) {
            entities.push_back(std::move(entity));
        }
    }
    return entities;
}

}  // namespace heliograph::mqtt
