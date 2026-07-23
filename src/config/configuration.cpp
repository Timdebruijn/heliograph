// SPDX-License-Identifier: MIT

#include "configuration.h"

#include "relays/drm.h"

#include <ArduinoJson.h>

#include <cstring>
#include <string>

namespace heliograph {
namespace {

bool finish(const JsonDocument& doc, std::string& out, size_t maxBytes) {
    if (doc.overflowed()) {
        return false;
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

/// Applies a string field if present. Returns false only on a type error.
bool patchString(JsonVariantConst v, std::string& target, const char* field, ConfigError& error) {
    if (v.isNull()) {
        return true;  // absent: leave alone
    }
    if (!v.is<const char*>()) {
        error = {field, "expected a string"};
        return false;
    }
    target = v.as<const char*>();
    return true;
}

/// True if the key is present, whether or not its value is null.
///
/// ArduinoJson v7 deprecates containsKey() and offers is<T>() instead, but is<T>() cannot
/// tell an absent key from an explicit null -- and that distinction is exactly what password
/// semantics rest on. Iterating the object is cheap here (these objects have <15 keys).
bool hasKey(JsonObjectConst obj, const char* key) {
    for (JsonPairConst kv : obj) {
        if (std::strcmp(kv.key().c_str(), key) == 0) {
            return true;
        }
    }
    return false;
}

/// Password semantics: absent leaves it, a string sets it, explicit null clears it.
bool patchSecret(JsonObjectConst obj, const char* key, std::string& target, const char* field,
                 ConfigError& error) {
    if (!hasKey(obj, key)) {
        return true;
    }
    JsonVariantConst v = obj[key];
    if (v.isNull()) {
        target.clear();
        return true;
    }
    if (!v.is<const char*>()) {
        error = {field, "expected a string or null"};
        return false;
    }
    target = v.as<const char*>();
    return true;
}

bool patchBool(JsonVariantConst v, bool& target, const char* field, ConfigError& error) {
    if (v.isNull()) {
        return true;
    }
    if (!v.is<bool>()) {
        error = {field, "expected a boolean"};
        return false;
    }
    target = v.as<bool>();
    return true;
}

template <typename T>
bool patchNumber(JsonVariantConst v, T& target, const char* field, ConfigError& error) {
    if (v.isNull()) {
        return true;
    }
    if (!v.is<long long>()) {
        error = {field, "expected an integer"};
        return false;
    }
    target = static_cast<T>(v.as<long long>());
    return true;
}

}  // namespace

const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "error";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Info:  return "info";
        case LogLevel::Debug: return "debug";
        case LogLevel::Trace: return "trace";
    }
    return "info";
}

bool parseLogLevel(const std::string& name, LogLevel& out) {
    if (name == "error") { out = LogLevel::Error; return true; }
    if (name == "warn")  { out = LogLevel::Warn;  return true; }
    if (name == "info")  { out = LogLevel::Info;  return true; }
    if (name == "debug") { out = LogLevel::Debug; return true; }
    if (name == "trace") { out = LogLevel::Trace; return true; }
    return false;
}

namespace {

/// Length limits. Without these a long string is only rejected at save time, where it
/// surfaces as an opaque HTTP 500 ("could not persist") instead of a 400 naming the field.
/// The SSID and PSK bounds are the 802.11 / WPA2 limits, not arbitrary choices.
bool checkLength(const std::string& value, size_t max, const char* field, ConfigError& error) {
    if (value.size() > max) {
        error = {field, "must be at most " + std::to_string(max) + " characters"};
        return false;
    }
    return true;
}

}  // namespace

bool validate(const Configuration& config, ConfigError& error) {
    if (!checkLength(config.bridgeName, 64, "bridge_name", error)) return false;
    if (!checkLength(config.wifi.ssid, 32, "wifi.ssid", error)) return false;          // 802.11
    if (!checkLength(config.wifi.password, 64, "wifi.password", error)) return false;  // WPA2 PSK
    if (!checkLength(config.wifi.hostname, 32, "wifi.hostname", error)) return false;
    if (!checkLength(config.mqtt.host, 128, "mqtt.host", error)) return false;
    if (!checkLength(config.mqtt.username, 64, "mqtt.username", error)) return false;
    if (!checkLength(config.mqtt.password, 128, "mqtt.password", error)) return false;
    if (!checkLength(config.mqtt.baseTopic, 64, "mqtt.base_topic", error)) return false;
    if (!checkLength(config.mqtt.discoveryPrefix, 64, "mqtt.discovery_prefix", error)) return false;
    if (!checkLength(config.driver.id, 64, "driver.id", error)) return false;
    if (!checkLength(config.security.adminUsername, 32, "security.admin_username", error)) return false;
    if (!checkLength(config.security.adminPassword, 64, "security.admin_password", error)) return false;
    // Driver options are free-form, so bound them too rather than trust the driver.
    for (const auto& [key, value] : config.driver.options) {
        if (!checkLength(key, 32, "driver.options", error)) return false;
        if (!checkLength(value, 128, "driver.options", error)) return false;
    }

    if (config.polling.intervalSeconds < 1 || config.polling.intervalSeconds > 3600) {
        error = {"polling.interval_seconds", "must be between 1 and 3600"};
        return false;
    }
    if (config.mqtt.enabled && config.mqtt.host.empty()) {
        error = {"mqtt.host", "required when mqtt is enabled"};
        return false;
    }
    if (config.mqtt.port == 0) {
        error = {"mqtt.port", "must be between 1 and 65535"};
        return false;
    }
    if (config.mqtt.qos > 2) {
        error = {"mqtt.qos", "must be 0, 1 or 2"};
        return false;
    }
    if (config.mqtt.baseTopic.empty()) {
        error = {"mqtt.base_topic", "must not be empty"};
        return false;
    }
    if (config.modbus.port == 0) {
        error = {"modbus.port", "must be between 1 and 65535"};
        return false;
    }
    // 0 is the Modbus broadcast address and 248-255 are reserved; neither addresses a device.
    if (config.modbus.unitId == 0 || config.modbus.unitId > 247) {
        error = {"modbus.unit_id", "must be between 1 and 247"};
        return false;
    }
    if (config.modbus.diagnosticsUnitId == 0) {
        error = {"modbus.diagnostics_unit_id", "must not be 0"};
        return false;
    }
    if (config.modbus.diagnosticsUnitId == config.modbus.unitId) {
        error = {"modbus.diagnostics_unit_id", "must differ from modbus.unit_id"};
        return false;
    }
    // The MVP has no writable driver; allowing this would advertise something untrue.
    if (config.modbus.writeEnabled) {
        error = {"modbus.write_enabled", "no writable driver exists in this build"};
        return false;
    }
    // driver.id may be empty: that means "pick the highest-priority driver compiled in".
    // Whether a non-empty id exists is the registry's business, not this validator's -- it
    // has no way to know which drivers were built into this firmware.
    if (config.relays.roles.size() > 8) {
        error = {"relays.roles", "at most 8 entries"};
        return false;
    }
    for (const auto& role : config.relays.roles) {
        if (!drm::isValidRole(role)) {
            error = {"relays.roles", "each entry must be 'none' or 'drm0'..'drm8'"};
            return false;
        }
    }
    if (config.security.adminUsername.empty()) {
        error = {"security.admin_username", "must not be empty"};
        return false;
    }
    if (config.wifi.hostname.empty()) {
        error = {"wifi.hostname", "must not be empty"};
        return false;
    }
    // The hostname is promised as http://<hostname>.local and sent as the DHCP client name,
    // so it must be a valid DNS label (RFC 1123): letters, digits and hyphens, not starting
    // or ending with a hyphen. Anything else is accepted by NVS but silently breaks both.
    for (const char ch : config.wifi.hostname) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '-';
        if (!ok) {
            error = {"wifi.hostname", "only letters, digits and hyphens are allowed"};
            return false;
        }
    }
    if (config.wifi.hostname.front() == '-' || config.wifi.hostname.back() == '-') {
        error = {"wifi.hostname", "must not start or end with a hyphen"};
        return false;
    }
    if (!checkLength(config.ntp.server, 64, "ntp.server", error)) return false;
    if (!checkLength(config.ntp.timezone, 48, "ntp.timezone", error)) return false;
    if (!checkLength(config.ntp.timezoneName, 48, "ntp.timezone_name", error)) return false;
    // A POSIX TZ is always needed to stamp logs in local time; a default is provided, so empty
    // is a mistake rather than a choice.
    if (config.ntp.timezone.empty()) {
        error = {"ntp.timezone", "must not be empty"};
        return false;
    }
    // With DHCP off there is no other source for the server, so it must be set. With DHCP on an
    // empty server is fine: the network supplies one, and a wrong-network case just leaves the
    // clock unsynced rather than refusing to boot.
    if (config.ntp.enabled && !config.ntp.useDhcp && config.ntp.server.empty()) {
        error = {"ntp.server", "required when ntp is enabled and dhcp is off"};
        return false;
    }
    return true;
}

bool serializeConfig(const Configuration& config, std::string& out, size_t maxBytes,
                     const bool* rebootRequired) {
    JsonDocument doc;
    doc["version"]     = config.version;
    doc["bridge_name"] = config.bridgeName;

    JsonObject wifi   = doc["wifi"].to<JsonObject>();
    wifi["ssid"]      = config.wifi.ssid;
    wifi["hostname"]  = config.wifi.hostname;
    // Not the password, not a mask of it. Only whether one is stored.
    wifi["password_set"] = !config.wifi.password.empty();

    JsonObject mqtt        = doc["mqtt"].to<JsonObject>();
    mqtt["enabled"]        = config.mqtt.enabled;
    mqtt["host"]           = config.mqtt.host;
    mqtt["port"]           = config.mqtt.port;
    // The username is half of a credential pair -- omitted like the password, with only a
    // *_set flag. It is not needed to identify the broker (host/topic do that) and handing
    // an unauthenticated LAN reader half the login is a leak worth closing.
    mqtt["username_set"]   = !config.mqtt.username.empty();
    mqtt["password_set"]   = !config.mqtt.password.empty();
    mqtt["base_topic"]     = config.mqtt.baseTopic;
    mqtt["discovery_prefix"]  = config.mqtt.discoveryPrefix;
    mqtt["discovery_enabled"] = config.mqtt.discoveryEnabled;
    mqtt["qos"]               = config.mqtt.qos;

    JsonObject modbus              = doc["modbus"].to<JsonObject>();
    modbus["enabled"]              = config.modbus.enabled;
    modbus["port"]                 = config.modbus.port;
    modbus["unit_id"]              = config.modbus.unitId;
    modbus["diagnostics_unit_id"]  = config.modbus.diagnosticsUnitId;
    modbus["write_enabled"]        = config.modbus.writeEnabled;

    doc["polling"]["interval_seconds"] = config.polling.intervalSeconds;

    doc["relays"]["enabled"] = config.relays.enabled;
    JsonArray relayRoles     = doc["relays"]["roles"].to<JsonArray>();
    for (const auto& role : config.relays.roles) {
        relayRoles.add(role);
    }

    JsonObject driver           = doc["driver"].to<JsonObject>();
    driver["id"]                = config.driver.id;
    driver["auto_detect"]       = config.driver.autoDetect;
    JsonObject options = driver["options"].to<JsonObject>();
    for (const auto& [key, value] : config.driver.options) {
        options[key] = value;
    }


    JsonObject ntp       = doc["ntp"].to<JsonObject>();
    ntp["enabled"]       = config.ntp.enabled;
    ntp["use_dhcp"]      = config.ntp.useDhcp;
    ntp["server"]        = config.ntp.server;  // not a secret
    ntp["timezone"]      = config.ntp.timezone;
    ntp["timezone_name"] = config.ntp.timezoneName;

    JsonObject security         = doc["security"].to<JsonObject>();
    security["admin_username"]  = config.security.adminUsername;
    security["password_set"]    = !config.security.adminPassword.empty();
    security["read_only_mode"]  = config.security.readOnlyMode;

    doc["logging"]["level"] = logLevelName(config.logLevel);

    // PATCH response only: tells a non-UI client whether the change it just made is waiting
    // on a restart. Absent from GET (nothing was changed there).
    if (rebootRequired != nullptr) {
        doc["reboot_required"] = *rebootRequired;
    }
    return finish(doc, out, maxBytes);
}

bool configChangeRequiresReboot(const Configuration& a, const Configuration& b) {
    // Everything the firmware reads exactly once -- WiFi.begin at setup(), the MQTT client
    // and Modbus server built in startOutputs(), the poll interval baked into PollPolicy, the
    // driver created at setup(), and the clock configured by TimeManager::begin(). Changing
    // any of these via PATCH updates NVS but not the running object, so it needs a restart.
    //
    // Deliberately NOT here (applied live by ctx.applyConfig): bridge_name (read fresh on
    // every status), relays.* (gates re-applied immediately), security.* (read per request),
    // logging.level (setLevel called from applyConfig). timezone_name and write_enabled carry
    // no runtime effect. This set mirrors RESTART_NEEDED in the web UI.
    return a.wifi.ssid != b.wifi.ssid || a.wifi.password != b.wifi.password ||
           a.wifi.hostname != b.wifi.hostname || a.mqtt.enabled != b.mqtt.enabled ||
           a.mqtt.host != b.mqtt.host || a.mqtt.port != b.mqtt.port ||
           a.mqtt.username != b.mqtt.username || a.mqtt.password != b.mqtt.password ||
           a.mqtt.baseTopic != b.mqtt.baseTopic ||
           a.mqtt.discoveryPrefix != b.mqtt.discoveryPrefix ||
           a.mqtt.discoveryEnabled != b.mqtt.discoveryEnabled || a.mqtt.qos != b.mqtt.qos ||
           a.modbus.enabled != b.modbus.enabled || a.modbus.port != b.modbus.port ||
           a.modbus.unitId != b.modbus.unitId ||
           a.modbus.diagnosticsUnitId != b.modbus.diagnosticsUnitId ||
           a.polling.intervalSeconds != b.polling.intervalSeconds ||
           a.driver.id != b.driver.id || a.driver.options != b.driver.options ||
           a.ntp.enabled != b.ntp.enabled || a.ntp.useDhcp != b.ntp.useDhcp ||
           a.ntp.server != b.ntp.server || a.ntp.timezone != b.ntp.timezone;
}

bool applyConfigPatch(const std::string& json, Configuration& config, ConfigError& error) {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        error = {"", "body is not valid JSON"};
        return false;
    }
    if (!doc.is<JsonObject>()) {
        error = {"", "body must be a JSON object"};
        return false;
    }

    // Merge into a copy: validation runs on the result, so a rejected patch never leaves a
    // half-applied configuration behind.
    Configuration merged = config;

    if (!patchString(doc["bridge_name"], merged.bridgeName, "bridge_name", error)) return false;

    if (JsonObjectConst wifi = doc["wifi"]; !wifi.isNull()) {
        if (!patchString(wifi["ssid"], merged.wifi.ssid, "wifi.ssid", error)) return false;
        if (!patchString(wifi["hostname"], merged.wifi.hostname, "wifi.hostname", error)) return false;
        if (!patchSecret(wifi, "password", merged.wifi.password, "wifi.password", error)) return false;
    }

    if (JsonObjectConst mqtt = doc["mqtt"]; !mqtt.isNull()) {
        if (!patchBool(mqtt["enabled"], merged.mqtt.enabled, "mqtt.enabled", error)) return false;
        if (!patchString(mqtt["host"], merged.mqtt.host, "mqtt.host", error)) return false;
        if (!patchNumber(mqtt["port"], merged.mqtt.port, "mqtt.port", error)) return false;
        if (!patchString(mqtt["username"], merged.mqtt.username, "mqtt.username", error)) return false;
        if (!patchSecret(mqtt, "password", merged.mqtt.password, "mqtt.password", error)) return false;
        if (!patchString(mqtt["base_topic"], merged.mqtt.baseTopic, "mqtt.base_topic", error)) return false;
        if (!patchString(mqtt["discovery_prefix"], merged.mqtt.discoveryPrefix, "mqtt.discovery_prefix", error)) return false;
        if (!patchBool(mqtt["discovery_enabled"], merged.mqtt.discoveryEnabled, "mqtt.discovery_enabled", error)) return false;
        if (!patchNumber(mqtt["qos"], merged.mqtt.qos, "mqtt.qos", error)) return false;
    }

    if (JsonObjectConst modbus = doc["modbus"]; !modbus.isNull()) {
        if (!patchBool(modbus["enabled"], merged.modbus.enabled, "modbus.enabled", error)) return false;
        if (!patchNumber(modbus["port"], merged.modbus.port, "modbus.port", error)) return false;
        if (!patchNumber(modbus["unit_id"], merged.modbus.unitId, "modbus.unit_id", error)) return false;
        if (!patchNumber(modbus["diagnostics_unit_id"], merged.modbus.diagnosticsUnitId, "modbus.diagnostics_unit_id", error)) return false;
        if (!patchBool(modbus["write_enabled"], merged.modbus.writeEnabled, "modbus.write_enabled", error)) return false;
    }

    if (JsonObjectConst polling = doc["polling"]; !polling.isNull()) {
        if (!patchNumber(polling["interval_seconds"], merged.polling.intervalSeconds, "polling.interval_seconds", error)) return false;
    }

    if (JsonObjectConst driver = doc["driver"]; !driver.isNull()) {
        if (!patchString(driver["id"], merged.driver.id, "driver.id", error)) return false;
        if (!patchBool(driver["auto_detect"], merged.driver.autoDetect, "driver.auto_detect", error)) return false;
        if (JsonObjectConst options = driver["options"]; !options.isNull()) {
            for (JsonPairConst kv : options) {
                if (!kv.value().is<const char*>()) {
                    error = {std::string("driver.options.") + kv.key().c_str(),
                             "expected a string"};
                    return false;
                }
                merged.driver.options[kv.key().c_str()] = kv.value().as<const char*>();
            }
        }
    }


    if (JsonObjectConst relays = doc["relays"]; !relays.isNull()) {
        if (!patchBool(relays["enabled"], merged.relays.enabled, "relays.enabled", error)) return false;
        if (JsonArrayConst roles = relays["roles"]; !roles.isNull()) {
            merged.relays.roles.clear();
            for (JsonVariantConst v : roles) {
                if (!v.is<const char*>()) {
                    error = {"relays.roles", "each entry must be a string"};
                    return false;
                }
                merged.relays.roles.emplace_back(v.as<const char*>());
            }
        }
    }

    if (JsonObjectConst ntp = doc["ntp"]; !ntp.isNull()) {
        if (!patchBool(ntp["enabled"], merged.ntp.enabled, "ntp.enabled", error)) return false;
        if (!patchBool(ntp["use_dhcp"], merged.ntp.useDhcp, "ntp.use_dhcp", error)) return false;
        if (!patchString(ntp["server"], merged.ntp.server, "ntp.server", error)) return false;
        if (!patchString(ntp["timezone"], merged.ntp.timezone, "ntp.timezone", error)) return false;
        if (!patchString(ntp["timezone_name"], merged.ntp.timezoneName, "ntp.timezone_name", error)) return false;
    }

    if (JsonObjectConst security = doc["security"]; !security.isNull()) {
        if (!patchString(security["admin_username"], merged.security.adminUsername, "security.admin_username", error)) return false;
        if (!patchSecret(security, "admin_password", merged.security.adminPassword, "security.admin_password", error)) return false;
        if (!patchBool(security["read_only_mode"], merged.security.readOnlyMode, "security.read_only_mode", error)) return false;
    }

    if (JsonObjectConst logging = doc["logging"]; !logging.isNull()) {
        if (JsonVariantConst level = logging["level"]; !level.isNull()) {
            if (!level.is<const char*>() || !parseLogLevel(level.as<const char*>(), merged.logLevel)) {
                error = {"logging.level", "must be error, warn, info, debug or trace"};
                return false;
            }
        }
    }

    if (!validate(merged, error)) {
        return false;
    }
    config = merged;
    return true;
}

}  // namespace heliograph
