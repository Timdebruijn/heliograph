// SPDX-License-Identifier: MIT

#include "configuration.h"

#include "json_limits.h"
#include "relays/drm.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <string>

namespace heliograph {
namespace {

using json_limits::finish;

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
    // Range-checked BEFORE the narrowing cast. Casting first silently wrapped a value into
    // something the later validate() was perfectly happy with, so the config that got stored was
    // not the one that was asked for and nothing said so: port 65537 became 1 -- a privileged
    // port -- and unit_id 259 became 3. Whether a bad value was caught depended entirely on
    // where the wrap happened to land (review, 2026-07-25).
    const long long raw = v.as<long long>();
    if (raw < static_cast<long long>(std::numeric_limits<T>::min()) ||
        raw > static_cast<long long>(std::numeric_limits<T>::max())) {
        // Deliberately says "out of range for this field" rather than naming numbers: the only
        // bounds available here are the C type's, and quoting those would be its own plausible
        // lie -- polling.interval_seconds would advertise 4294967295 where validate() allows
        // 3600. The narrower rule reports itself one step later, in its own words.
        error = {field, "value is out of range for this field"};
        return false;
    }
    target = static_cast<T>(raw);
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
    if (1 + config.additionalDevices.size() > kMaxDevices) {
        error = {"additional_devices",
                 "at most " + std::to_string(kMaxDevices) + " devices per bridge"};
        return false;
    }
    for (size_t i = 0; i < config.additionalDevices.size(); ++i) {
        const auto&       d     = config.additionalDevices[i];
        const std::string where = "additional_devices[" + std::to_string(i) + "]";
        // An empty id is fine for `driver` -- it means "let the application pick the
        // highest-priority driver". For an extra device it means nothing at all: there is no
        // second driver to pick, and an entry that names no driver would just be a poll slot
        // that can never be filled.
        if (d.id.empty()) {
            error = {where + ".driver_id", "must name a driver"};
            return false;
        }
        if (!checkLength(d.id, 64, (where + ".driver_id").c_str(), error)) return false;
        for (const auto& [key, value] : d.options) {
            if (!checkLength(key, 32, (where + ".options").c_str(), error)) return false;
            if (!checkLength(value, 128, (where + ".options").c_str(), error)) return false;
        }
    }

    if (config.serial.enabled) {
        // Bounds, not a whitelist of "known good" rates. RS485 devices in the field run at
        // 1200 and at 115200, and refusing an odd-but-real rate would be the firmware deciding
        // it knows the installation better than the installer does.
        const auto& p = config.serial.profile;
        if (p.baudRate < 1200 || p.baudRate > 115200) {
            error = {"serial.baud_rate", "must be between 1200 and 115200"};
            return false;
        }
        // 8 only. The transport maps a SerialProfile onto the ESP32's SERIAL_* constants and
        // handles the 8-bit combinations; anything else falls through to SERIAL_8N1, which
        // loses the data bits AND the parity while configure() still reports success. That
        // fallthrough was harmless while only drivers picked the line -- every driver ships
        // 8 bits -- and this override is what made it reachable from the API and the settings
        // form. Refused here rather than coerced silently, because the symptom of the coercion
        // is a dead bus and a log line stating the setting that was NOT applied.
        if (p.dataBits != 8) {
            error = {"serial.data_bits", "must be 8; 7-bit framing is not supported"};
            return false;
        }
        if (p.stopBits < 1 || p.stopBits > 2) {
            error = {"serial.stop_bits", "must be 1 or 2"};
            return false;
        }
    }
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


    // Always emitted, empty or not, so a client can tell "this firmware has no idea about
    // extra devices" from "this bridge has none configured".
    JsonArray extra = doc["additional_devices"].to<JsonArray>();
    for (const auto& d : config.additionalDevices) {
        JsonObject e   = extra.add<JsonObject>();
        e["driver_id"] = d.id;
        JsonObject o   = e["options"].to<JsonObject>();
        for (const auto& [key, value] : d.options) {
            o[key] = value;
        }
    }

    JsonObject ntp       = doc["ntp"].to<JsonObject>();
    ntp["enabled"]       = config.ntp.enabled;
    ntp["use_dhcp"]      = config.ntp.useDhcp;
    ntp["server"]        = config.ntp.server;  // not a secret
    ntp["timezone"]      = config.ntp.timezone;
    ntp["timezone_name"] = config.ntp.timezoneName;

    // Always emitted, enabled or not, so the settings page can show what the bridge will
    // actually do to the line rather than leaving the reader to infer it from an absent key.
    JsonObject serial   = doc["serial"].to<JsonObject>();
    serial["override"]  = config.serial.enabled;
    serial["baud_rate"] = config.serial.profile.baudRate;
    serial["parity"]    = parityName(config.serial.profile.parity);
    serial["data_bits"] = config.serial.profile.dataBits;
    serial["stop_bits"] = config.serial.profile.stopBits;

    JsonObject security = doc["security"].to<JsonObject>();
    // The admin username is omitted for the same reason mqtt.username is, in the mqtt block
    // above: it is half of a credential pair, this endpoint is unauthenticated, and Basic has no
    // brute-force protection. Serving it turned guessing the login into guessing only the
    // password. Unlike the MQTT one it needs no *_set flag -- validate() requires it to be
    // non-empty, so "is one set" is always yes and would say nothing.
    security["password_set"]   = !config.security.adminPassword.empty();
    security["read_only_mode"] = config.security.readOnlyMode;

    doc["updates"]["check_enabled"] = config.updates.checkEnabled;
    doc["logging"]["level"]         = logLevelName(config.logLevel);

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
    // logging.level (setLevel called from applyConfig), updates.check_enabled (the dashboard
    // reads it when it renders, and the check runs in the browser -- the firmware never acts on
    // it). timezone_name and write_enabled carry no runtime effect. This set mirrors
    // RESTART_NEEDED in the web UI.
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
           // The drivers and their poll contexts are built once, in setup(). Adding or
           // removing an inverter therefore changes nothing until the next boot.
           a.additionalDevices != b.additionalDevices ||
           a.ntp.enabled != b.ntp.enabled || a.ntp.useDhcp != b.ntp.useDhcp ||
           a.ntp.server != b.ntp.server || a.ntp.timezone != b.ntp.timezone ||
           // The line is configured once, in setup(), right after the driver's begin(). Nothing
           // reconfigures a live UART mid-poll, so a changed override only takes effect on the
           // next boot -- and saying otherwise would leave someone watching a bus that is still
           // running at the old rate while the page claims the new one is in force.
           a.serial.enabled != b.serial.enabled ||
           (b.serial.enabled && !(a.serial.profile == b.serial.profile));
}

bool applyConfigPatch(const std::string& json, Configuration& config, ConfigError& error,
                      const DriverLookupFn& lookupDriver) {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        error = {"", "body is not valid JSON"};
        return false;
    }
    if (!doc.is<JsonObject>()) {
        error = {"", "body must be a JSON object"};
        return false;
    }

    // Driver option keys this request itself supplied. Collected during the merge and consulted
    // by the orphan cleanup below, which must never drop what the caller just asked for.
    std::vector<std::string> suppliedOptionKeys;

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
                suppliedOptionKeys.emplace_back(kv.key().c_str());
            }
        }
    }
    // Heal driver options the resulting driver cannot accept, so that a stored value can never
    // make the configuration permanently unsaveable. Two shapes of that:
    //   - a key the driver does not declare at all -- an orphan from a previous driver, which
    //     the merge has no way to delete;
    //   - a declared key holding a value outside the driver's allowed set, which happens without
    //     anyone editing it when a firmware update renames or drops a choice. Not theoretical:
    //     a driver whose option values are generated from the shipped device profiles narrows
    //     that list whenever a profile is renamed or removed.
    // Either one makes the REST layer's validateDriverOptions refuse EVERY later PATCH, including
    // ones that touch nothing driver-related -- the lockout this batch exists to remove, which
    // shipped in 0.12.0 as the orphan case.
    //
    // Runs on every patch, not only one that touches `driver`: a bridge already carrying a bad
    // value must heal on the next save it makes, otherwise it stays stuck forever.
    //
    // Only values this request did NOT assert are touched. "Asserted" means the patch supplied
    // the key AND changed it -- a caller echoing back what it just read (the obvious
    // read-modify-write script) is asking for nothing and must not thereby pin a broken value,
    // while a genuine new value is left alone so the REST layer reports the mistake instead of
    // this code silently swallowing it. And nothing at all happens when the id resolves to no
    // driver: that would orphan every option at once, and a typo'd DRIVER id has to stay
    // recoverable by correcting it.
    //
    // An earlier attempt simply cleared the map whenever the id changed. Review showed that
    // silently wiped a working setup on a discovery-wizard click, on a typo, and on an empty id,
    // and never repaired an already-stuck config (2026-07-25).
    // Heals one option map against what its driver declares. `assertedKey` decides which
    // entries are off limits -- a value this request asked for is reported, never rewritten.
    const auto healOptions = [](const DriverDescriptor& target, DriverOptions& options,
                                const std::function<bool(const std::string&)>& assertedKey) {
        for (auto it = options.begin(); it != options.end();) {
            if (assertedKey(it->first)) {
                ++it;
                continue;
            }
            const DriverOption* declared = nullptr;
            for (const auto& o : target.options) {
                if (o.key == it->first) {
                    declared = &o;
                    break;
                }
            }
            if (declared == nullptr) {
                it = options.erase(it);
                continue;
            }
            if (!declared->allowedValues.empty() &&
                std::find(declared->allowedValues.begin(), declared->allowedValues.end(),
                          it->second) == declared->allowedValues.end()) {
                it->second = declared->defaultValue;
            }
            // The third shape, and the one this loop was missing. A numeric option only gained
            // bounds in this release, so a value that was perfectly legal before is refused
            // now -- and because the REST gate validates the MERGED map, that made every later
            // PATCH fail, including one that touches nothing driver-related. Exactly the
            // lockout the branches above exist to prevent, reopened for a new shape.
            if (declared->isNumeric()) {
                char*      end    = nullptr;
                const long parsed = std::strtol(it->second.c_str(), &end, 10);
                const bool number = !it->second.empty() && end != it->second.c_str() && *end == '\0';
                if (!number || parsed < declared->minValue || parsed > declared->maxValue) {
                    it->second = declared->defaultValue;
                }
            }
            ++it;
        }
    };

    if (const DriverDescriptor* target = lookupDriver ? lookupDriver(merged.driver.id) : nullptr) {
        healOptions(*target, merged.driver.options, [&](const std::string& key) {
            const bool supplied =
                std::find(suppliedOptionKeys.begin(), suppliedOptionKeys.end(), key) !=
                suppliedOptionKeys.end();
            const auto stored = config.driver.options.find(key);
            const auto now    = merged.driver.options.find(key);
            return supplied && (stored == config.driver.options.end() ||
                                (now != merged.driver.options.end() && stored->second != now->second));
        });
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

    const bool serialSupplied = !doc["serial"].isNull();
    bool extraDevicesSupplied = false;
    // Whole-list replacement, not a merge: the list has no stable keys to merge on, and
    // "patch element 2" would need an index the caller cannot know is still the same element.
    // Sending the array replaces it; omitting it leaves it alone, like every other section.
    if (JsonVariantConst extra = doc["additional_devices"]; !extra.isNull()) {
        if (!extra.is<JsonArrayConst>()) {
            error = {"additional_devices", "expected an array"};
            return false;
        }
        std::vector<DriverSettings> parsed;
        size_t                      index = 0;
        for (JsonVariantConst item : extra.as<JsonArrayConst>()) {
            const std::string where = "additional_devices[" + std::to_string(index++) + "]";
            if (!item.is<JsonObjectConst>()) {
                error = {where, "expected an object"};
                return false;
            }
            JsonObjectConst obj = item.as<JsonObjectConst>();
            DriverSettings  d;
            if (!patchString(obj["driver_id"], d.id, (where + ".driver_id").c_str(), error))
                return false;
            if (JsonObjectConst options = obj["options"]; !options.isNull()) {
                for (JsonPairConst kv : options) {
                    if (!kv.value().is<const char*>()) {
                        error = {where + ".options", "option values must be strings"};
                        return false;
                    }
                    d.options[kv.key().c_str()] = kv.value().as<const char*>();
                }
            }
            parsed.push_back(std::move(d));
        }
        merged.additionalDevices = std::move(parsed);
        extraDevicesSupplied     = true;
    }

    // The extra devices get the same healing, and they need it more: nothing healed them at
    // all before, so any stored value their driver stopped accepting made the whole
    // configuration unsaveable with no way to see which row was at fault. Nothing here is ever
    // "asserted" -- the list travels whole or not at all, and when it travels the caller's
    // values are validated by the REST layer rather than rewritten here.
    if (lookupDriver && !extraDevicesSupplied) {
        for (auto& dev : merged.additionalDevices) {
            if (const DriverDescriptor* d = lookupDriver(dev.id)) {
                healOptions(*d, dev.options, [](const std::string&) { return false; });
            }
        }
    }


    if (JsonObjectConst serial = doc["serial"]; !serial.isNull()) {
        if (!patchBool(serial["override"], merged.serial.enabled, "serial.override", error))
            return false;
        if (!patchNumber(serial["baud_rate"], merged.serial.profile.baudRate, "serial.baud_rate",
                         error))
            return false;
        if (!patchNumber(serial["data_bits"], merged.serial.profile.dataBits, "serial.data_bits",
                         error))
            return false;
        if (!patchNumber(serial["stop_bits"], merged.serial.profile.stopBits, "serial.stop_bits",
                         error))
            return false;
        if (JsonVariantConst parity = serial["parity"]; !parity.isNull()) {
            std::string name;
            if (!patchString(parity, name, "serial.parity", error)) {
                return false;
            }
            // Refused rather than defaulted: a typo'd parity that silently became "none" would
            // configure a line the user did not ask for and then blame the cable.
            if (!parseParity(name, merged.serial.profile.parity)) {
                error = {"serial.parity", "must be \"none\", \"even\" or \"odd\""};
                return false;
            }
        }
    }

    // A line override belongs to the driver it was derived from. The wizard pins one only when
    // the device answered at a profile that driver does not lead with, so carrying it across to
    // a DIFFERENT driver forces line settings that were never measured against it -- and every
    // driver in this build leads with 9600 8N1, so the carried value is wrong by construction.
    // The symptom is a silent bus right after a restart the Driver card asked for, with the
    // cause sitting in a different card further up the page.
    //
    // Only when this patch did not supply `serial` itself. The wizard sends driver AND serial
    // together, and clearing what the caller just asked for is the mistake the driver-option
    // rule above already had to be redesigned to avoid. Clearing degrades to "the driver
    // decides", which is where every healthy install already is, is visible in Settings, and
    // is re-derived by running discovery again -- so the recovery does not need a factory reset.
    if (!serialSupplied && merged.driver.id != config.driver.id && merged.serial.enabled) {
        merged.serial.enabled = false;
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

    if (JsonObjectConst updates = doc["updates"]; !updates.isNull()) {
        // Into `merged`, like every other section here, and through the same helper. Writing
        // to `config` directly looks equivalent and is not: the merged copy is assigned over
        // it once validation passes, so the value would be silently discarded.
        if (!patchBool(updates["check_enabled"], merged.updates.checkEnabled,
                       "updates.check_enabled", error))
            return false;
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
