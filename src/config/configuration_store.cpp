// SPDX-License-Identifier: MIT

#include "configuration_store.h"

#include <ArduinoJson.h>

#include <cstring>

namespace heliograph {
namespace {

/// Applies migrations in sequence. Each step upgrades exactly one version, so the chain works
/// for a device that skipped several firmware releases.
///
/// There is only one version so far, so this is a scaffold -- but a tested one. Writing it at
/// v1 costs nothing; writing it at v3, after two silent config wipes, costs a lot.
bool migrate(JsonDocument& doc, uint16_t from) {
    uint16_t version = from;

    // while (version == 1) { ...move fields...; version = 2; }

    if (version != kConfigVersion) {
        return false;
    }
    doc["version"] = kConfigVersion;
    return true;
}

}  // namespace

const char* loadResultName(LoadResult result) {
    switch (result) {
        case LoadResult::Ok:            return "ok";
        case LoadResult::NotFound:      return "not_found";
        case LoadResult::Corrupt:       return "corrupt";
        case LoadResult::FutureVersion: return "future_version";
        case LoadResult::Migrated:      return "migrated";
    }
    return "unknown";
}

bool MemoryBackend::read(const std::string& key, std::string& value) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return false;
    }
    value = it->second;
    return true;
}

bool MemoryBackend::write(const std::string& key, const std::string& value) {
    if (writeFails) {
        return false;
    }
    values_[key] = value;
    return true;
}

bool MemoryBackend::erase() {
    values_.clear();
    return true;
}

bool MemoryBackend::contains(const std::string& key) const {
    return values_.find(key) != values_.end();
}

std::string MemoryBackend::raw(const std::string& key) const {
    const auto it = values_.find(key);
    return it == values_.end() ? std::string{} : it->second;
}

bool serializeConfigForStorage(const Configuration& config, std::string& out) {
    JsonDocument doc;
    doc["version"]     = kConfigVersion;
    doc["bridge_name"] = config.bridgeName;

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["ssid"]     = config.wifi.ssid;
    wifi["password"] = config.wifi.password;  // storage only, never a response body
    wifi["hostname"] = config.wifi.hostname;

    JsonObject mqtt = doc["mqtt"].to<JsonObject>();
    mqtt["enabled"]           = config.mqtt.enabled;
    mqtt["host"]              = config.mqtt.host;
    mqtt["port"]              = config.mqtt.port;
    mqtt["username"]          = config.mqtt.username;
    mqtt["password"]          = config.mqtt.password;
    mqtt["base_topic"]        = config.mqtt.baseTopic;
    mqtt["discovery_prefix"]  = config.mqtt.discoveryPrefix;
    mqtt["discovery_enabled"] = config.mqtt.discoveryEnabled;
    mqtt["qos"]               = config.mqtt.qos;

    JsonObject modbus = doc["modbus"].to<JsonObject>();
    modbus["enabled"]             = config.modbus.enabled;
    modbus["port"]                = config.modbus.port;
    modbus["unit_id"]             = config.modbus.unitId;
    modbus["diagnostics_unit_id"] = config.modbus.diagnosticsUnitId;
    modbus["write_enabled"]       = config.modbus.writeEnabled;

    doc["polling"]["interval_seconds"] = config.polling.intervalSeconds;
    doc["relays"]["enabled"]           = config.relays.enabled;

    JsonObject driver     = doc["driver"].to<JsonObject>();
    driver["id"]          = config.driver.id;
    driver["auto_detect"] = config.driver.autoDetect;
    JsonObject options    = driver["options"].to<JsonObject>();
    for (const auto& [key, value] : config.driver.options) {
        options[key] = value;
    }


    JsonObject ntp       = doc["ntp"].to<JsonObject>();
    ntp["enabled"]       = config.ntp.enabled;
    ntp["use_dhcp"]      = config.ntp.useDhcp;
    ntp["server"]        = config.ntp.server;
    ntp["timezone"]      = config.ntp.timezone;
    ntp["timezone_name"] = config.ntp.timezoneName;

    JsonObject security        = doc["security"].to<JsonObject>();
    security["admin_username"] = config.security.adminUsername;
    security["admin_password"] = config.security.adminPassword;
    security["read_only_mode"] = config.security.readOnlyMode;

    doc["logging"]["level"] = logLevelName(config.logLevel);

    if (doc.overflowed()) {
        return false;
    }
    // NVS caps a string entry at 4000 bytes. Refuse rather than write a truncated blob that
    // would fail to parse on the next boot and look like corruption.
    const size_t needed = measureJson(doc);
    if (needed > 3900) {
        return false;
    }
    out.resize(needed + 1);
    out.resize(serializeJson(doc, out.data(), out.size()));
    return true;
}

namespace {

/// Copies the network identity -- WiFi credentials, hostname and the admin account -- out
/// of a config document this firmware otherwise refuses (FutureVersion, failed validation).
/// These five fields have existed unchanged since version 1 and are individually
/// length-checked here; everything else in the refused document stays untouched, so the
/// caller keeps defaults for all feature settings.
void salvageIdentity(const JsonDocument& doc, Configuration& out) {
    const auto take = [](JsonVariantConst v, std::string& into, size_t maxLen) {
        if (v.is<const char*>()) {
            const char* s = v.as<const char*>();
            if (s != nullptr && s[0] != '\0' && std::strlen(s) <= maxLen) {
                into = s;
            }
        }
    };
    JsonObjectConst wifi = doc["wifi"];
    if (!wifi.isNull()) {
        take(wifi["ssid"], out.wifi.ssid, 32);       // 802.11 SSID limit
        take(wifi["password"], out.wifi.password, 64);
        take(wifi["hostname"], out.wifi.hostname, 63);  // DNS label limit
    }
    JsonObjectConst security = doc["security"];
    if (!security.isNull()) {
        take(security["admin_username"], out.security.adminUsername, 64);
        take(security["admin_password"], out.security.adminPassword, 64);
    }
}

}  // namespace

LoadResult deserializeConfigFromStorage(const std::string& json, Configuration& out) {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok || !doc.is<JsonObject>()) {
        return LoadResult::Corrupt;
    }

    const uint16_t stored = doc["version"].is<uint16_t>() ? doc["version"].as<uint16_t>() : 0;
    if (stored == 0) {
        return LoadResult::Corrupt;  // no version: not something we wrote
    }
    if (stored > kConfigVersion) {
        // Written by newer firmware -- the OTA-rollback direction. Reinterpreting feature
        // fields we do not understand is worse than defaults, because the result looks
        // plausible. But the network identity (WiFi + admin credentials, stable since
        // version 1) is salvaged into the caller's config: without it, the rollback safety
        // net dropped the device into the setup AP at a customer's house -- unreachable at
        // exactly the moment the net fired (review, 2026-07-21).
        salvageIdentity(doc, out);
        return LoadResult::FutureVersion;
    }

    const bool needsMigration = stored < kConfigVersion;
    if (needsMigration && !migrate(doc, stored)) {
        return LoadResult::Corrupt;
    }

    Configuration parsed;
    parsed.version = kConfigVersion;
    if (doc["bridge_name"].is<const char*>()) parsed.bridgeName = doc["bridge_name"].as<const char*>();

    if (JsonObjectConst wifi = doc["wifi"]; !wifi.isNull()) {
        if (wifi["ssid"].is<const char*>())     parsed.wifi.ssid     = wifi["ssid"].as<const char*>();
        if (wifi["password"].is<const char*>()) parsed.wifi.password = wifi["password"].as<const char*>();
        if (wifi["hostname"].is<const char*>()) parsed.wifi.hostname = wifi["hostname"].as<const char*>();
    }
    if (JsonObjectConst mqtt = doc["mqtt"]; !mqtt.isNull()) {
        if (mqtt["enabled"].is<bool>())            parsed.mqtt.enabled = mqtt["enabled"].as<bool>();
        if (mqtt["host"].is<const char*>())        parsed.mqtt.host = mqtt["host"].as<const char*>();
        if (mqtt["port"].is<uint16_t>())           parsed.mqtt.port = mqtt["port"].as<uint16_t>();
        if (mqtt["username"].is<const char*>())    parsed.mqtt.username = mqtt["username"].as<const char*>();
        if (mqtt["password"].is<const char*>())    parsed.mqtt.password = mqtt["password"].as<const char*>();
        if (mqtt["base_topic"].is<const char*>())  parsed.mqtt.baseTopic = mqtt["base_topic"].as<const char*>();
        if (mqtt["discovery_prefix"].is<const char*>()) parsed.mqtt.discoveryPrefix = mqtt["discovery_prefix"].as<const char*>();
        if (mqtt["discovery_enabled"].is<bool>())  parsed.mqtt.discoveryEnabled = mqtt["discovery_enabled"].as<bool>();
        if (mqtt["qos"].is<uint8_t>())             parsed.mqtt.qos = mqtt["qos"].as<uint8_t>();
    }
    if (JsonObjectConst modbus = doc["modbus"]; !modbus.isNull()) {
        if (modbus["enabled"].is<bool>())   parsed.modbus.enabled = modbus["enabled"].as<bool>();
        if (modbus["port"].is<uint16_t>())  parsed.modbus.port = modbus["port"].as<uint16_t>();
        if (modbus["unit_id"].is<uint8_t>()) parsed.modbus.unitId = modbus["unit_id"].as<uint8_t>();
        if (modbus["diagnostics_unit_id"].is<uint8_t>()) parsed.modbus.diagnosticsUnitId = modbus["diagnostics_unit_id"].as<uint8_t>();
        if (modbus["write_enabled"].is<bool>()) parsed.modbus.writeEnabled = modbus["write_enabled"].as<bool>();
    }
    if (JsonObjectConst polling = doc["polling"]; !polling.isNull()) {
        if (polling["interval_seconds"].is<uint32_t>()) parsed.polling.intervalSeconds = polling["interval_seconds"].as<uint32_t>();
    }
    if (JsonObjectConst relays = doc["relays"]; !relays.isNull()) {
        if (relays["enabled"].is<bool>()) parsed.relays.enabled = relays["enabled"].as<bool>();
    }
    if (JsonObjectConst driver = doc["driver"]; !driver.isNull()) {
        if (driver["id"].is<const char*>())    parsed.driver.id = driver["id"].as<const char*>();
        if (driver["auto_detect"].is<bool>())  parsed.driver.autoDetect = driver["auto_detect"].as<bool>();
        if (JsonObjectConst options = driver["options"]; !options.isNull()) {
            for (JsonPairConst kv : options) {
                if (kv.value().is<const char*>()) {
                    parsed.driver.options[kv.key().c_str()] = kv.value().as<const char*>();
                }
            }
        }
    }
    if (JsonObjectConst ntp = doc["ntp"]; !ntp.isNull()) {
        if (ntp["enabled"].is<bool>())         parsed.ntp.enabled  = ntp["enabled"].as<bool>();
        if (ntp["use_dhcp"].is<bool>())        parsed.ntp.useDhcp  = ntp["use_dhcp"].as<bool>();
        if (ntp["server"].is<const char*>())   parsed.ntp.server   = ntp["server"].as<const char*>();
        if (ntp["timezone"].is<const char*>()) parsed.ntp.timezone = ntp["timezone"].as<const char*>();
        if (ntp["timezone_name"].is<const char*>()) parsed.ntp.timezoneName = ntp["timezone_name"].as<const char*>();
    }
    if (JsonObjectConst security = doc["security"]; !security.isNull()) {
        if (security["admin_username"].is<const char*>()) parsed.security.adminUsername = security["admin_username"].as<const char*>();
        if (security["admin_password"].is<const char*>()) parsed.security.adminPassword = security["admin_password"].as<const char*>();
        if (security["read_only_mode"].is<bool>()) parsed.security.readOnlyMode = security["read_only_mode"].as<bool>();
    }
    if (JsonObjectConst logging = doc["logging"]; !logging.isNull()) {
        if (logging["level"].is<const char*>()) {
            parseLogLevel(logging["level"].as<const char*>(), parsed.logLevel);
        }
    }

    // A stored config that no longer validates (a range tightened in a newer firmware) is
    // treated as corrupt rather than loaded: running on values we have declared invalid is
    // exactly how a device ends up in a state nobody can reason about. The network identity
    // is still salvaged -- same reasoning as the FutureVersion path above.
    ConfigError error;
    if (!validate(parsed, error)) {
        salvageIdentity(doc, out);
        return LoadResult::Corrupt;
    }

    out = parsed;
    return needsMigration ? LoadResult::Migrated : LoadResult::Ok;
}

ConfigurationStore::ConfigurationStore(KeyValueBackend& backend, KeyValueBackend* legacy)
    : backend_(backend), legacy_(legacy) {}

LoadResult ConfigurationStore::load(Configuration& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string                 blob;
    bool migratedFromLegacy = false;
    if (!backend_.read(kStorageKeyConfig, blob) || blob.empty()) {
        // Rename migration: the primary namespace is empty, but a pre-rename image may have
        // left the configuration under the old one. Adopt it -- otherwise an OTA from 0.4.x
        // boots "unprovisioned" with the config sitting intact in flash. The legacy copy is
        // deliberately not erased, so a bootloader rollback to 0.4.x still finds it.
        if (legacy_ == nullptr || !legacy_->read(kStorageKeyConfig, blob) || blob.empty()) {
            return LoadResult::NotFound;
        }
        migratedFromLegacy = true;
    }
    Configuration parsed;
    auto          result = deserializeConfigFromStorage(blob, parsed);
    if (result == LoadResult::Ok || result == LoadResult::Migrated) {
        out = parsed;
        if (migratedFromLegacy) {
            // Persist under the new namespace so the next boot reads it directly. A failed
            // write is not fatal: the legacy copy remains, and this path simply runs again.
            backend_.write(kStorageKeyConfig, blob);
            result = LoadResult::Migrated;
        }
    } else if (result == LoadResult::FutureVersion || result == LoadResult::Corrupt) {
        // The refused document may still have yielded a salvaged network identity (WiFi +
        // admin credentials -- see salvageIdentity). Carry exactly those five fields over
        // so a rollback keeps the device reachable; everything else stays at defaults.
        out.wifi.ssid                = parsed.wifi.ssid;
        out.wifi.password            = parsed.wifi.password;
        out.wifi.hostname            = parsed.wifi.hostname.empty() ? out.wifi.hostname
                                                                    : parsed.wifi.hostname;
        out.security.adminUsername   = parsed.security.adminUsername.empty()
                                           ? out.security.adminUsername
                                           : parsed.security.adminUsername;
        out.security.adminPassword   = parsed.security.adminPassword;
    }
    return result;
}

bool ConfigurationStore::save(const Configuration& config) {
    ConfigError error;
    if (!validate(config, error)) {
        return false;  // never persist something we would refuse to load
    }
    std::string blob;
    if (!serializeConfigForStorage(config, blob)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return backend_.write(kStorageKeyConfig, blob);
}

bool ConfigurationStore::factoryReset() {
    std::lock_guard<std::mutex> lock(mutex_);
    return backend_.erase();
}

}  // namespace heliograph
