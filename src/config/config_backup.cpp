// SPDX-License-Identifier: MIT

#include "config_backup.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

#include "configuration_store.h"

namespace heliograph {

// The relationship between the two bounds, pinned here rather than in config_backup.h so that
// header keeps its light include set -- rest_payloads.h pulls it in, and reaching for
// kMaxStoredConfigBytes there would drag configuration_store.h's <map>, <mutex> and the MQTT
// announced-devices header along behind it.
static_assert(kMaxBackupBytes > kMaxStoredConfigBytes,
              "a backup must have room for the largest configuration plus its envelope");

namespace {

/// Keys whose value must never be rendered or exported. A rule, not a list: a credential
/// added to the config model later is covered without anyone having to remember this file.
bool isSecretKey(const char* key) {
    if (key == nullptr) return false;
    const size_t n = std::strlen(key);
    if (std::strcmp(key, "password") == 0) return true;
    const char* suffix = "_password";
    const size_t m     = std::strlen(suffix);
    return n > m && std::strcmp(key + n - m, suffix) == 0;
}

/// The storage document, as a parsed tree. Goes through serializeConfigForStorage rather than
/// rebuilding the field list, which is the whole point -- see the header. The extra
/// serialise/parse round trip costs a millisecond on a path a human triggers by hand.
bool storageDocument(const Configuration& config, JsonDocument& out) {
    std::string blob;
    if (!serializeConfigForStorage(config, blob)) {
        return false;
    }
    return deserializeJson(out, blob) == DeserializationError::Ok;
}

/// Removes every secret-named key, at any depth. Recursive because the config document is
/// nested and a top-level sweep would miss wifi.password.
void stripSecrets(JsonVariant node) {
    if (node.is<JsonObject>()) {
        JsonObject           obj = node.as<JsonObject>();
        std::vector<std::string> doomed;
        for (JsonPair kv : obj) {
            if (isSecretKey(kv.key().c_str())) {
                // Collected first: removing while iterating invalidates the iterator.
                doomed.emplace_back(kv.key().c_str());
            } else {
                stripSecrets(kv.value());
            }
        }
        for (const auto& key : doomed) {
            obj.remove(key);
        }
    } else if (node.is<JsonArray>()) {
        for (JsonVariant v : node.as<JsonArray>()) {
            stripSecrets(v);
        }
    }
}

/// Renders a leaf for the diff. Bool as true/false rather than 1/0 -- this is read by a person
/// deciding whether a restore does what they expect.
std::string renderValue(JsonVariantConst v) {
    if (v.isNull()) return "(absent)";
    if (v.is<bool>()) return v.as<bool>() ? "true" : "false";
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        return (s == nullptr || s[0] == '\0') ? "(empty)" : std::string(s);
    }
    if (v.is<JsonArrayConst>() || v.is<JsonObjectConst>()) {
        std::string compact;
        serializeJson(v, compact);
        // Bounded: additional_devices with eight entries would otherwise dominate the table.
        if (compact.size() > 120) {
            compact.resize(117);
            compact += "...";
        }
        return compact;
    }
    std::string number;
    serializeJson(v, number);
    return number;
}

void walkDiff(JsonObjectConst before, JsonObjectConst after, const std::string& prefix,
              std::vector<ConfigDiffEntry>& out) {
    // Driven by `after`, then swept for keys only `before` had. Iterating one side alone would
    // miss a setting the restore REMOVES, which is a change like any other.
    for (JsonPairConst kv : after) {
        const std::string path = prefix.empty() ? kv.key().c_str()
                                                : prefix + "." + kv.key().c_str();
        JsonVariantConst  old  = before[kv.key()];
        if (isSecretKey(kv.key().c_str())) {
            const auto flag = [](JsonVariantConst v) {
                if (!v.is<const char*>()) return "(not set)";
                const char* s = v.as<const char*>();
                return (s != nullptr && s[0] != '\0') ? "(set)" : "(not set)";
            };
            if (std::strcmp(flag(old), flag(kv.value())) != 0) {
                out.push_back({path, flag(old), flag(kv.value())});
            }
            continue;
        }
        if (kv.value().is<JsonObjectConst>() && old.is<JsonObjectConst>()) {
            walkDiff(old.as<JsonObjectConst>(), kv.value().as<JsonObjectConst>(), path, out);
            continue;
        }
        const std::string a = renderValue(old);
        const std::string b = renderValue(kv.value());
        if (a != b) {
            out.push_back({path, a, b});
        }
    }
    for (JsonPairConst kv : before) {
        if (!after[kv.key()].isNull()) continue;
        const std::string path = prefix.empty() ? kv.key().c_str()
                                                : prefix + "." + kv.key().c_str();
        if (isSecretKey(kv.key().c_str())) {
            out.push_back({path, "(set)", "(not set)"});
        } else {
            out.push_back({path, renderValue(kv.value()), "(absent)"});
        }
    }
}

}  // namespace

const char* backupResultName(BackupResult result) {
    switch (result) {
        case BackupResult::Ok:           return "ok";
        case BackupResult::NotJson:      return "not_json";
        case BackupResult::NotABackup:   return "not_a_backup";
        case BackupResult::FutureFormat: return "future_format";
        case BackupResult::FutureConfig: return "future_config";
        case BackupResult::Corrupt:      return "corrupt";
    }
    return "unknown";
}

std::string isoUtcTimestamp(time_t epoch) {
    // 2020-01-01. Below this the clock has not been set, and stamping a file "1970-01-01"
    // would put a plausible-looking date on a backup whose age is actually unknown.
    if (epoch < 1577836800) {
        return {};
    }
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &epoch);
#else
    gmtime_r(&epoch, &tm);
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        return {};
    }
    return buf;
}

bool buildConfigBackup(const Configuration& config, const BackupOptions& options,
                       std::string& out) {
    JsonDocument inner;
    if (!storageDocument(config, inner)) {
        return false;
    }
    if (!options.includeSecrets) {
        stripSecrets(inner.as<JsonVariant>());
    }

    JsonDocument doc;
    doc["format"]         = kBackupFormat;
    doc["format_version"] = kBackupFormatVersion;
    if (!options.firmwareVersion.empty()) doc["firmware_version"] = options.firmwareVersion;
    if (!options.exportedAt.empty()) doc["exported_at"] = options.exportedAt;
    // Recorded so the restore preview can say "this file carries no passwords" before the
    // operator commits, rather than leaving them to discover it from the diff.
    doc["includes_secrets"] = options.includeSecrets;
    // A name for the human sorting through a downloads folder. Not read back on restore.
    doc["bridge_name"]      = config.bridgeName;
    doc["config"]           = inner;

    if (doc.overflowed()) {
        return false;
    }
    // Pretty-printed on purpose. This file is meant to be opened, read and occasionally
    // hand-edited; the compact form the device stores is not.
    out.resize(measureJsonPretty(doc) + 1);
    out.resize(serializeJsonPretty(doc, out.data(), out.size()));
    return true;
}

BackupResult parseConfigBackup(const std::string& json, BackupContents& out,
                               std::string& detail) {
    detail.clear();
    if (json.size() > kMaxBackupBytes) {
        detail = "the file is larger than a configuration backup can be";
        return BackupResult::NotJson;
    }
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok || !doc.is<JsonObject>()) {
        detail = "this is not a JSON document";
        return BackupResult::NotJson;
    }
    if (!doc["format"].is<const char*>() ||
        std::strcmp(doc["format"].as<const char*>(), kBackupFormat) != 0) {
        detail = "this is a JSON file, but not a Heliograph configuration backup";
        return BackupResult::NotABackup;
    }
    out.formatVersion = doc["format_version"].is<uint16_t>()
                            ? doc["format_version"].as<uint16_t>()
                            : 0;
    if (out.formatVersion == 0) {
        detail = "the backup does not say which format version it is";
        return BackupResult::NotABackup;
    }
    if (out.formatVersion > kBackupFormatVersion) {
        detail = "the backup was written by a newer firmware (format version " +
                 std::to_string(out.formatVersion) + "; this firmware understands " +
                 std::to_string(kBackupFormatVersion) + ")";
        return BackupResult::FutureFormat;
    }
    JsonObjectConst inner = doc["config"];
    if (inner.isNull()) {
        detail = "the backup carries no configuration";
        return BackupResult::NotABackup;
    }

    if (doc["firmware_version"].is<const char*>())
        out.firmwareVersion = doc["firmware_version"].as<const char*>();
    if (doc["exported_at"].is<const char*>())
        out.exportedAt = doc["exported_at"].as<const char*>();
    out.includesSecrets = doc["includes_secrets"].as<bool>();

    // Which credentials the document ACTUALLY carries, read before the configuration is parsed:
    // once parsed, an omitted password and a stored empty one are the same empty string, and
    // the merge has to tell them apart. A present-but-empty password counts as carried -- that
    // is a deliberately empty credential, and refusing to apply it would silently ignore it.
    out.hasWifiPassword  = inner["wifi"]["password"].is<const char*>();
    out.hasMqttPassword  = inner["mqtt"]["password"].is<const char*>();
    out.hasAdminPassword = inner["security"]["admin_password"].is<const char*>();

    // Back through the store's own reader: it owns the version check, the migration chain and
    // the validation, and a second implementation of any of those is a second place to be
    // wrong about someone's configuration.
    std::string blob;
    serializeJson(inner, blob);
    Configuration parsed;
    switch (deserializeConfigFromStorage(blob, parsed)) {
        case LoadResult::Ok:
        case LoadResult::Migrated:
            out.config = parsed;
            return BackupResult::Ok;
        case LoadResult::FutureVersion:
            detail = "the configuration inside was written by a newer firmware than this one";
            return BackupResult::FutureConfig;
        case LoadResult::NotFound:
        case LoadResult::Corrupt:
            detail = "the configuration inside could not be read, or is not one this firmware "
                     "would accept";
            return BackupResult::Corrupt;
    }
    detail = "the configuration inside could not be read";
    return BackupResult::Corrupt;
}

void applyBackup(const BackupContents& backup, Configuration& target) {
    // Snapshot the credentials first: `target` is about to be overwritten wholesale, and the
    // redacted case needs the values that are on the bridge right now.
    const std::string wifiPassword  = target.wifi.password;
    const std::string mqttPassword  = target.mqtt.password;
    const std::string adminPassword = target.security.adminPassword;

    target = backup.config;

    if (!backup.hasWifiPassword) target.wifi.password = wifiPassword;
    if (!backup.hasMqttPassword) target.mqtt.password = mqttPassword;
    if (!backup.hasAdminPassword) target.security.adminPassword = adminPassword;
}

bool diffConfigurations(const Configuration& before, const Configuration& after,
                        std::vector<ConfigDiffEntry>& out) {
    out.clear();
    JsonDocument a;
    JsonDocument b;
    if (!storageDocument(before, a) || !storageDocument(after, b)) {
        return false;
    }
    walkDiff(a.as<JsonObjectConst>(), b.as<JsonObjectConst>(), "", out);
    return true;
}

}  // namespace heliograph
