// SPDX-License-Identifier: MIT

#include "home_assistant_discovery.h"

#include <ArduinoJson.h>

#include <cctype>
#include <cstdio>

#include "device/command.h"
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
/// Escapes a label for embedding in a single-quoted Jinja string literal.
///
/// The labels come from our own profile TOMLs rather than from user input, so this is not a
/// security boundary -- it is a correctness one. A label containing an apostrophe ("Grid & Gen's
/// mode") would terminate the literal early, and Home Assistant would accept the resulting
/// template, render an entity, and silently send nothing when it was used. A quietly inert
/// control is worse than a rejected one.
std::string escapeSingleQuotes(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        if (c == '\\' || c == '\'') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

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
                    bool isBridgeEntity, const std::string& uniqueBase,
                    const std::string& label = {}) {
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
    // Per device, not per bridge: with one identifier for all of them Home Assistant merges
    // three physical inverters into a single device and their entities fight over the same
    // names. uniqueBase is the bridge id for device 1, so an existing install's device block
    // is byte-identical to before.
    device["identifiers"].to<JsonArray>().add(uniqueBase + "_inverter");
    // Model, not manufacturer: the manufacturer is already its own field, and repeating it here
    // produced names like "Heliograph - Heliograph open-source project". The model is what
    // distinguishes one inverter from the next, which is the whole point of the name.
    const std::string& descriptor = !identity.model.empty()          ? identity.model
                                    : !identity.manufacturer.empty() ? identity.manufacturer
                                                                     : kUnknownInverterName;
    // A label replaces the derived name outright, bridge prefix and all. Somebody who types
    // "Schuur" wants to read "Schuur" in Home Assistant, not "Heliograph - TL3000-20 #17", and
    // prefixing it would only put the bridge's name back in front of the one thing they chose.
    //
    // Only the NAME. identifiers, unique_id and the retained config topic are all derived from
    // uniqueBase and are untouched, which is what makes renaming safe: Home Assistant matches
    // the device it already has, updates its name, and every entity keeps its history. (An
    // operator who renamed the device inside HA keeps their own name -- HA treats that as
    // name_by_user and does not let discovery overrule it.)
    std::string name = !label.empty() ? label
                       : bridge.name.empty()
                           ? descriptor
                           : bridge.name + " - " + descriptor;
    // Three identical inverters on one bus produce three identical model strings and no serial
    // number, so without this Home Assistant lists three devices called exactly the same thing
    // and twelve entities each called "AC Power". The address is the only thing that tells them
    // apart, and the driver already put it in the identity.
    //
    // Only for devices 2..N: the primary keeps the name it has always had, like its topics and
    // its unique ids. Same reasoning, same test.
    //
    // And never when a label is set: the label already tells the devices apart -- that is what
    // it is for -- so appending "#17" to "Schuur" would put the address back into the one name
    // the operator chose to be free of it.
    const bool primary = uniqueBase == bridge.bridgeId;
    if (label.empty() && !primary && !identity.instanceKey.empty()) {
        name += " #" + identity.instanceKey;
    }
    device["name"] = name;
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
    // Command entities (below) are derived from the write bitset and, for a numeric one, its
    // supported/writable flags and published bounds. Folded in here for the same reason the
    // measurement loop above is: every shipping driver has this bitset fixed for the life of
    // the firmware today, so this never actually changes anything yet -- but the moment a
    // profile-declared write row is marked verified for real hardware, a stale or missing
    // command entity would otherwise sit there until the next reconnect, the same class of bug
    // the relay-roles fingerprint exists to prevent.
    //
    // Mirrors buildDiscoveryEntities' own gate term for term -- write bitset, then the enum
    // skip, then numeric supported/writable -- because a fingerprint that reacts to LESS than
    // the condition it is meant to track is worse than no fingerprint at all: it looks like
    // protection while silently missing the one flip that matters most. An earlier version of
    // this loop hashed the bounds unconditionally whenever the write bit was set, never
    // checking cap.supported/cap.writable at all -- so a NumericCapability pre-populated with
    // real bounds but supported=false produced the SAME hash before and after supported later
    // flipped true, even though that is exactly the "verified" transition the comment above
    // claims to cover (review, 2026-07-30).
    for (size_t i = 0; i < kCommandTypeCount; ++i) {
        const auto type       = static_cast<InverterCommandType>(i);
        const auto capability = requiredCapability(type);
        if (capability == InverterCapability::_Count ||
            !state.capabilities.write.test(static_cast<size_t>(capability))) {
            continue;
        }
        if (commandTakesEnumValue(type)) {
            const EnumCapability& cap = state.capabilities.enums[i];
            if (!cap.supported || !cap.writable || cap.optionCount == 0) {
                continue;  // no entity built, so no trace here -- same rule as the numeric case
            }
            sig = fnv1aAppend(sig, commandTypeName(type));
            // The LABELS and their values, not just how many there are. A profile that renames a
            // mode or renumbers one produces a different select in Home Assistant, and a
            // fingerprint blind to that would leave the old dropdown in place -- offering an
            // option that now maps to a different register value, which is the worst shape of
            // this bug because it still appears to work.
            for (size_t o = 0; o < cap.optionCount; ++o) {
                if (cap.options[o].label != nullptr) {
                    sig = fnv1aAppend(sig, cap.options[o].label);
                }
                char buf[16];
                snprintf(buf, sizeof(buf), "=%d", cap.options[o].value);
                sig = fnv1aAppend(sig, buf);
            }
            continue;
        }
        if (commandTakesNumericValue(type)) {
            const NumericCapability& cap = state.capabilities.numeric[i];
            if (!cap.supported || !cap.writable) {
                // Exactly buildDiscoveryEntities' own refusal: a write bit with no usable
                // range publishes no entity, so it must leave no trace here either.
                continue;
            }
            sig = fnv1aAppend(sig, commandTypeName(type));
            char buf[64];
            snprintf(buf, sizeof(buf), "%.6g:%.6g:%.6g", cap.minimum, cap.maximum, cap.step);
            sig = fnv1aAppend(sig, buf);
        } else {
            sig = fnv1aAppend(sig, commandTypeName(type));
        }
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
                                                    const std::string& availabilityTopic,
                                                    const std::string& discoveryPrefix,
                                                    const std::string& uniqueBase) {
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

        e["unique_id"] = uniqueBase + "_" + slug;
        e["object_id"] = uniqueBase + "_" + slug;
        const bool noName = m.displayName == nullptr || m.displayName[0] == '\0';
        e["name"]      = noName ? humanise(m.id) : std::string(m.displayName);
        e["state_topic"] = topics.state();
        // Yields None for a null value, which Home Assistant records as "unknown" rather
        // than as a reading of 0.
        e["value_template"] =
            std::string("{{ value_json.measurements['") + m.id + "'].value }}";
        e["availability_topic"]  = availabilityTopic;
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
        addDeviceBlock(e, bridge, state.identity, /*isBridgeEntity=*/false, uniqueBase, state.label);

        DiscoveryEntity entity;
        entity.uniqueId    = e["unique_id"].as<std::string>();
        // The RETAINED config topic has to be per device too. With the bridge id in the node
        // name, device 2 published its config over device 1's and Home Assistant kept only
        // the last one -- the unique_id alone was not enough (caught by its own test).
        entity.configTopic = discoveryPrefix + "/sensor/" + uniqueBase + "/" + slug + "/config";
        if (serialise(doc, entity.payload)) {
            entities.push_back(std::move(entity));
        }
    }

    // Status text, only when the driver actually reports status.
    if (state.capabilities.has(InverterCapability::ReadStatus)) {
        JsonDocument doc;
        JsonObject   e = doc.to<JsonObject>();
        e["unique_id"]      = uniqueBase + "_status";
        e["object_id"]      = uniqueBase + "_status";
        e["name"]           = "Status";
        e["state_topic"]    = topics.state();
        e["value_template"] = "{{ value_json.status_text }}";
        e["availability_topic"] = availabilityTopic;
        e["icon"]           = "mdi:information-outline";
        addDeviceBlock(e, bridge, state.identity, false, uniqueBase, state.label);

        DiscoveryEntity entity;
        entity.uniqueId    = e["unique_id"].as<std::string>();
        entity.configTopic = discoveryPrefix + "/sensor/" + uniqueBase + "/status/config";
        if (serialise(doc, entity.payload)) {
            entities.push_back(std::move(entity));
        }
    }

    // Inverter liveness as a binary_sensor, so a dashboard can show it without a template.
    {
        JsonDocument doc;
        JsonObject   e = doc.to<JsonObject>();
        e["unique_id"]      = uniqueBase + "_inverter_online";
        e["object_id"]      = uniqueBase + "_inverter_online";
        e["name"]           = "Inverter Online";
        e["state_topic"]    = topics.state();
        e["value_template"] = "{{ 'ON' if value_json.inverter_online else 'OFF' }}";
        e["availability_topic"] = availabilityTopic;
        e["device_class"]   = "connectivity";
        e["entity_category"] = "diagnostic";
        addDeviceBlock(e, bridge, state.identity, false, uniqueBase, state.label);

        DiscoveryEntity entity;
        entity.uniqueId = e["unique_id"].as<std::string>();
        entity.configTopic =
            discoveryPrefix + "/binary_sensor/" + uniqueBase + "/inverter_online/config";
        if (serialise(doc, entity.payload)) {
            entities.push_back(std::move(entity));
        }
    }

    // Control entities: one per command type the active driver's capabilities.write bit
    // actually grants -- the SAME bitset CommandDispatcher::dispatch's gate 2 checks, so an
    // entity exists in Home Assistant exactly when the command behind it could reach a driver.
    // On every driver shipping today the bit is never set, so this loop produces nothing, the
    // same way the sensor loop above produces nothing for an unsupported measurement.
    for (size_t i = 0; i < kCommandTypeCount; ++i) {
        const auto type       = static_cast<InverterCommandType>(i);
        const auto capability = requiredCapability(type);
        if (capability == InverterCapability::_Count ||
            !state.capabilities.write.test(static_cast<size_t>(capability))) {
            continue;
        }

        const std::string name       = commandTypeName(type);
        const std::string jsonPrefix = std::string("{\"type\":\"") + name + "\"";

        JsonDocument doc;
        JsonObject   e = doc.to<JsonObject>();
        e["unique_id"] = uniqueBase + "_" + name;
        e["object_id"] = uniqueBase + "_" + name;
        e["name"]      = humanise(name);
        e["command_topic"]         = topics.commandSet();
        e["availability_topic"]    = availabilityTopic;
        e["payload_available"]     = kPayloadOnline;
        e["payload_not_available"] = kPayloadOffline;
        addDeviceBlock(e, bridge, state.identity, /*isBridgeEntity=*/false, uniqueBase,
                      state.label);

        std::string domain;
        if (commandTakesNumericValue(type)) {
            const NumericCapability& cap = state.capabilities.numeric[i];
            if (!cap.supported || !cap.writable) {
                // The write bit is set but no range was published. CommandDispatcher itself
                // refuses this at dispatch time -- see command.h's note on why a missing
                // bound is a refusal rather than a bypass -- so no entity either: nothing
                // that reached this driver could ever succeed.
                continue;
            }
            domain                 = "number";
            e["command_template"]  = jsonPrefix + ",\"value\":{{ value }}}";
            e["min"]                = cap.minimum;
            e["max"]                = cap.maximum;
            e["step"]               = cap.step > 0.0 ? cap.step : 1.0;
            const char* unit = unitSymbol(cap.unit);
            if (unit[0] != '\0') {
                e["unit_of_measurement"] = unit;
            }
            // Optimistic: unlike the relay switch, there is no readback topic for a setpoint --
            // no shipping driver reports one back as a measurement, and command.h's own
            // `execute()` doc note explains why the ack cannot arrive synchronously either.
            e["optimistic"] = true;
        } else if (commandTakesEnumValue(type)) {
            const EnumCapability& cap = state.capabilities.enums[i];
            if (!cap.supported || !cap.writable || cap.optionCount == 0) {
                // The write bit is set but no modes were published. Same refusal as the missing
                // numeric range above, and for a sharper reason: a select with no options is a
                // dropdown a user can open and not choose anything from.
                continue;
            }
            domain = "select";

            // Home Assistant sends the LABEL a user picked; the device wants its own mode
            // number. The template carries the mapping, so the translation happens once here
            // rather than in every automation the user writes. Built as a Jinja dict literal:
            //   {% set m = {'Forced': 2} %}{"type":"...","enum_value":{{ m[value] }}}
            std::string mapping = "{% set m = {";
            JsonArray   options = e["options"].to<JsonArray>();
            for (size_t o = 0; o < cap.optionCount; ++o) {
                const char* label = cap.options[o].label;
                if (label == nullptr) {
                    continue;
                }
                options.add(label);
                if (o > 0) {
                    mapping += ", ";
                }
                // Labels come from a profile TOML, so they are ours rather than user input --
                // but a stray apostrophe would still break the template silently, and the
                // resulting entity would look fine and do nothing. Escaped rather than trusted.
                mapping += "'" + escapeSingleQuotes(label) + "': " +
                           std::to_string(cap.options[o].value);
            }
            mapping += "} %}";
            e["command_template"] = mapping + jsonPrefix + ",\"enum_value\":{{ m[value] }}}";
            // Optimistic for the same reason the number entity is: no driver reports the active
            // mode back as a measurement yet, so there is no state topic to follow.
            e["optimistic"] = true;
        } else {
            // Neither numeric nor enum: Start, Stop, SynchronizeTime -- a bare press, no
            // payload to configure beyond the fixed command type.
            domain             = "button";
            e["payload_press"] = jsonPrefix + "}";
        }

        DiscoveryEntity entity;
        entity.uniqueId = e["unique_id"].as<std::string>();
        entity.configTopic =
            discoveryPrefix + "/" + domain + "/" + uniqueBase + "/" + name + "/config";
        if (serialise(doc, entity.payload)) {
            entities.push_back(std::move(entity));
        }
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
        addDeviceBlock(e, bridge, DeviceIdentity{}, /*isBridgeEntity=*/true, bridge.bridgeId);

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
        // Bridge-scoped on purpose: the relays are the BRIDGE's contacts, not any inverter's,
        // so they keep the bridge id even on a multi-device install.
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
        addDeviceBlock(e, bridge, DeviceIdentity{}, /*isBridgeEntity=*/true, bridge.bridgeId);

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
        addDeviceBlock(e, bridge, DeviceIdentity{}, /*isBridgeEntity=*/true, bridge.bridgeId);
        if (serialise(doc, entity.payload)) {
            entities.push_back(std::move(entity));
        }
    }
    return entities;
}

}  // namespace heliograph::mqtt
