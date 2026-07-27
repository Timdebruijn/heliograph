// SPDX-License-Identifier: MIT
// Configuration backup: round-trip, redaction, the merge rule for absent credentials, the
// refusal of files this firmware must not apply, and the restore preview.

#include <unity.h>

#include <ArduinoJson.h>

#include <cstring>
#include <string>
#include <vector>

#include "config/config_backup.h"
#include "config/configuration_store.h"

using namespace heliograph;

void setUp() {}
void tearDown() {}

/// A configuration with every section moved off its defaults, so a field that fails to survive
/// the round trip shows up as a difference rather than coincidentally matching the default.
static Configuration populated() {
    Configuration c;
    c.bridgeName             = "Shed bridge";
    c.wifi.ssid              = "HomeNet";
    c.wifi.password          = "wifi-secret";
    c.wifi.hostname          = "shed";
    c.mqtt.enabled           = true;
    c.mqtt.host              = "192.168.20.10";
    c.mqtt.port              = 8883;
    c.mqtt.username          = "bridge";
    c.mqtt.password          = "mqtt-secret";
    c.mqtt.baseTopic         = "solar";
    c.mqtt.discoveryPrefix   = "ha";
    c.mqtt.discoveryEnabled  = false;
    c.mqtt.qos               = 1;
    c.modbus.enabled         = false;
    c.modbus.port            = 1502;
    c.modbus.unitId          = 7;
    c.modbus.diagnosticsUnitId = 240;
    c.modbus.maxClients      = 8;
    c.modbus.idleTimeoutSeconds = 0;
    c.polling.intervalSeconds = 30;
    c.driver.id              = "mock";
    c.driver.autoDetect      = true;
    c.driver.options["unit_id"] = "3";
    c.additionalDevices.push_back({"mock", false, {{"unit_id", "4"}}});
    c.relays.enabled         = true;
    c.relays.roles           = {"drm0", "none"};
    c.ntp.enabled            = false;
    c.ntp.useDhcp            = false;
    c.ntp.server             = "192.168.20.1";
    c.ntp.timezone           = "UTC0";
    c.ntp.timezoneName       = "UTC";
    c.serial.enabled         = true;
    c.serial.profile.baudRate = 19200;
    c.serial.profile.parity   = SerialParity::Even;
    c.security.adminUsername = "operator";
    c.security.adminPassword = "admin-secret";
    c.security.readOnlyMode  = false;
    c.logLevel               = LogLevel::Debug;
    return c;
}

static BackupContents parseOrFail(const std::string& json) {
    BackupContents contents;
    std::string    detail;
    const auto     result = parseConfigBackup(json, contents, detail);
    TEST_ASSERT_EQUAL_STRING("ok", backupResultName(result));
    return contents;
}

// --------------------------------------------------------------------------------------
// Round trip
// --------------------------------------------------------------------------------------

static void test_round_trip_with_secrets_is_lossless() {
    const Configuration original = populated();
    std::string         file;
    TEST_ASSERT_TRUE(buildConfigBackup(original, {true, "0.14.0", "2026-07-26T12:00:00Z"}, file));

    Configuration restored;  // defaults: nothing to inherit, so every field must come from the file
    applyBackup(parseOrFail(file), restored);

    TEST_ASSERT_EQUAL_STRING(original.bridgeName.c_str(), restored.bridgeName.c_str());
    TEST_ASSERT_EQUAL_STRING(original.wifi.ssid.c_str(), restored.wifi.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING(original.wifi.password.c_str(), restored.wifi.password.c_str());
    TEST_ASSERT_EQUAL_STRING(original.wifi.hostname.c_str(), restored.wifi.hostname.c_str());
    TEST_ASSERT_EQUAL_STRING(original.mqtt.host.c_str(), restored.mqtt.host.c_str());
    TEST_ASSERT_EQUAL_UINT16(original.mqtt.port, restored.mqtt.port);
    TEST_ASSERT_EQUAL_STRING(original.mqtt.username.c_str(), restored.mqtt.username.c_str());
    TEST_ASSERT_EQUAL_STRING(original.mqtt.password.c_str(), restored.mqtt.password.c_str());
    TEST_ASSERT_EQUAL_STRING(original.mqtt.baseTopic.c_str(), restored.mqtt.baseTopic.c_str());
    TEST_ASSERT_FALSE(restored.mqtt.discoveryEnabled);
    TEST_ASSERT_EQUAL_UINT8(1, restored.mqtt.qos);
    TEST_ASSERT_FALSE(restored.modbus.enabled);
    TEST_ASSERT_EQUAL_UINT16(1502, restored.modbus.port);
    TEST_ASSERT_EQUAL_UINT8(7, restored.modbus.unitId);
    TEST_ASSERT_EQUAL_UINT8(240, restored.modbus.diagnosticsUnitId);
    TEST_ASSERT_EQUAL_UINT8(8, restored.modbus.maxClients);
    // 0 round-trips as 0 rather than falling back to the default: it is the "never" setting,
    // and a restore that quietly reinstated a 20 s timeout would drop long-lived clients on a
    // bridge that was deliberately configured not to.
    TEST_ASSERT_EQUAL_UINT32(0, restored.modbus.idleTimeoutSeconds);
    TEST_ASSERT_EQUAL_UINT32(30, restored.polling.intervalSeconds);
    TEST_ASSERT_TRUE(original.driver == restored.driver);
    TEST_ASSERT_EQUAL_size_t(1, restored.additionalDevices.size());
    TEST_ASSERT_EQUAL_STRING("4", restored.additionalDevices[0].options.at("unit_id").c_str());
    TEST_ASSERT_TRUE(restored.relays.enabled);
    TEST_ASSERT_EQUAL_size_t(2, restored.relays.roles.size());
    TEST_ASSERT_EQUAL_STRING("drm0", restored.relays.roles[0].c_str());
    TEST_ASSERT_FALSE(restored.ntp.enabled);
    TEST_ASSERT_EQUAL_STRING("192.168.20.1", restored.ntp.server.c_str());
    TEST_ASSERT_EQUAL_STRING("UTC0", restored.ntp.timezone.c_str());
    TEST_ASSERT_TRUE(restored.serial.enabled);
    TEST_ASSERT_EQUAL_UINT32(19200, restored.serial.profile.baudRate);
    TEST_ASSERT_EQUAL(SerialParity::Even, restored.serial.profile.parity);
    TEST_ASSERT_EQUAL_STRING("operator", restored.security.adminUsername.c_str());
    TEST_ASSERT_EQUAL_STRING("admin-secret", restored.security.adminPassword.c_str());
    TEST_ASSERT_FALSE(restored.security.readOnlyMode);
    TEST_ASSERT_EQUAL(LogLevel::Debug, restored.logLevel);
}

/// The envelope's own metadata survives, because the restore preview shows it to the operator
/// before they commit -- "which bridge and which firmware wrote this" is half of deciding
/// whether to apply a file at all.
static void test_envelope_metadata_is_reported_back() {
    std::string file;
    TEST_ASSERT_TRUE(buildConfigBackup(populated(), {false, "0.14.0", "2026-07-26T12:00:00Z"},
                                       file));
    const BackupContents contents = parseOrFail(file);
    TEST_ASSERT_EQUAL_UINT16(kBackupFormatVersion, contents.formatVersion);
    TEST_ASSERT_EQUAL_STRING("0.14.0", contents.firmwareVersion.c_str());
    TEST_ASSERT_EQUAL_STRING("2026-07-26T12:00:00Z", contents.exportedAt.c_str());
    TEST_ASSERT_FALSE(contents.includesSecrets);
}

/// A field added to Configuration must reach the backup without anyone editing config_backup.
/// Asserting on the shared builder is how that stays true: both documents come from
/// serializeConfigForStorage, so the only way to add a field to one is to add it to both.
static void test_backup_carries_every_stored_key() {
    std::string stored;
    TEST_ASSERT_TRUE(serializeConfigForStorage(populated(), stored));
    JsonDocument storedDoc;
    TEST_ASSERT_TRUE(deserializeJson(storedDoc, stored) == DeserializationError::Ok);

    std::string file;
    TEST_ASSERT_TRUE(buildConfigBackup(populated(), {true, "", ""}, file));
    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(deserializeJson(backupDoc, file) == DeserializationError::Ok);

    for (JsonPairConst kv : storedDoc.as<JsonObjectConst>()) {
        TEST_ASSERT_FALSE_MESSAGE(backupDoc["config"][kv.key()].isNull(), kv.key().c_str());
    }
}

// --------------------------------------------------------------------------------------
// Secrets
// --------------------------------------------------------------------------------------

static void test_redacted_backup_contains_no_password_anywhere() {
    std::string file;
    TEST_ASSERT_TRUE(buildConfigBackup(populated(), {false, "", ""}, file));
    TEST_ASSERT_NULL(std::strstr(file.c_str(), "wifi-secret"));
    TEST_ASSERT_NULL(std::strstr(file.c_str(), "mqtt-secret"));
    TEST_ASSERT_NULL(std::strstr(file.c_str(), "admin-secret"));
    // Not merely absent as a value: the keys are gone, so nothing round-trips an empty string
    // back in as a deliberate "clear this password".
    TEST_ASSERT_NULL(std::strstr(file.c_str(), "\"password\""));
    TEST_ASSERT_NULL(std::strstr(file.c_str(), "admin_password"));
    // The non-secret half of each credential pair is NOT a password and stays: a restore that
    // silently dropped the MQTT username would fail to connect for a reason nothing explains.
    TEST_ASSERT_NOT_NULL(std::strstr(file.c_str(), "bridge"));
    TEST_ASSERT_NOT_NULL(std::strstr(file.c_str(), "operator"));
}

/// The rule that makes a redacted backup usable rather than merely safe.
static void test_absent_password_keeps_the_one_the_bridge_already_has() {
    std::string file;
    TEST_ASSERT_TRUE(buildConfigBackup(populated(), {false, "", ""}, file));

    Configuration target;
    target.wifi.password          = "existing-wifi";
    target.mqtt.password          = "existing-mqtt";
    target.security.adminPassword = "existing-admin";
    applyBackup(parseOrFail(file), target);

    TEST_ASSERT_EQUAL_STRING("existing-wifi", target.wifi.password.c_str());
    TEST_ASSERT_EQUAL_STRING("existing-mqtt", target.mqtt.password.c_str());
    TEST_ASSERT_EQUAL_STRING("existing-admin", target.security.adminPassword.c_str());
    // ...while everything that is not a credential really was replaced.
    TEST_ASSERT_EQUAL_STRING("Shed bridge", target.bridgeName.c_str());
    TEST_ASSERT_EQUAL_STRING("HomeNet", target.wifi.ssid.c_str());
}

/// A backup WITH secrets overwrites, including onto a bridge that already has different ones.
/// This is the "clone a bridge" path and it has to be complete.
static void test_carried_password_replaces_the_existing_one() {
    std::string file;
    TEST_ASSERT_TRUE(buildConfigBackup(populated(), {true, "", ""}, file));

    Configuration target;
    target.wifi.password          = "existing-wifi";
    target.security.adminPassword = "existing-admin";
    applyBackup(parseOrFail(file), target);

    TEST_ASSERT_EQUAL_STRING("wifi-secret", target.wifi.password.c_str());
    TEST_ASSERT_EQUAL_STRING("admin-secret", target.security.adminPassword.c_str());
}

/// Present-but-empty is a deliberate empty credential (an open WiFi network), not a redaction.
/// Treating it as "keep" would silently ignore the operator's edit.
static void test_an_explicitly_empty_password_is_applied() {
    Configuration open  = populated();
    open.wifi.password  = "";
    std::string file;
    TEST_ASSERT_TRUE(buildConfigBackup(open, {true, "", ""}, file));

    const BackupContents contents = parseOrFail(file);
    TEST_ASSERT_TRUE(contents.hasWifiPassword);

    Configuration target;
    target.wifi.password = "existing-wifi";
    applyBackup(contents, target);
    TEST_ASSERT_EQUAL_STRING("", target.wifi.password.c_str());
}

// --------------------------------------------------------------------------------------
// Refusals
// --------------------------------------------------------------------------------------

static void test_garbage_is_not_json() {
    BackupContents contents;
    std::string    detail;
    TEST_ASSERT_EQUAL(BackupResult::NotJson,
                      parseConfigBackup("this is not json at all", contents, detail));
    TEST_ASSERT_FALSE(detail.empty());
}

/// Valid JSON that is simply the wrong file -- by far the likeliest mistake, so it is told
/// apart from a version problem and says so.
static void test_a_foreign_json_file_is_refused_as_not_a_backup() {
    BackupContents contents;
    std::string    detail;
    TEST_ASSERT_EQUAL(BackupResult::NotABackup,
                      parseConfigBackup(R"({"name":"something else","value":3})", contents,
                                        detail));
    TEST_ASSERT_NOT_NULL(std::strstr(detail.c_str(), "not a Heliograph"));
}

static void test_a_newer_envelope_is_refused_not_guessed_at() {
    std::string file;
    TEST_ASSERT_TRUE(buildConfigBackup(populated(), {true, "", ""}, file));
    JsonDocument doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, file) == DeserializationError::Ok);
    doc["format_version"] = kBackupFormatVersion + 1;
    std::string tampered;
    serializeJson(doc, tampered);

    BackupContents contents;
    std::string    detail;
    TEST_ASSERT_EQUAL(BackupResult::FutureFormat,
                      parseConfigBackup(tampered, contents, detail));
    TEST_ASSERT_NOT_NULL(std::strstr(detail.c_str(), "newer firmware"));
}

/// The envelope is one version, the configuration inside is another. A file this firmware
/// understands the shape of can still hold a configuration it must not reinterpret.
static void test_a_newer_inner_config_is_refused_separately() {
    std::string file;
    TEST_ASSERT_TRUE(buildConfigBackup(populated(), {true, "", ""}, file));
    JsonDocument doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, file) == DeserializationError::Ok);
    doc["config"]["version"] = kConfigVersion + 1;
    std::string tampered;
    serializeJson(doc, tampered);

    BackupContents contents;
    std::string    detail;
    TEST_ASSERT_EQUAL(BackupResult::FutureConfig,
                      parseConfigBackup(tampered, contents, detail));
}

static void test_an_envelope_with_no_configuration_is_refused() {
    BackupContents contents;
    std::string    detail;
    const std::string file =
        std::string(R"({"format":")") + kBackupFormat + R"(","format_version":1})";
    TEST_ASSERT_EQUAL(BackupResult::NotABackup, parseConfigBackup(file, contents, detail));
}

/// A configuration this firmware would refuse to LOAD must not arrive through the restore
/// door either -- otherwise the backup path is a way around validate().
static void test_a_configuration_that_fails_validation_is_refused() {
    std::string file;
    TEST_ASSERT_TRUE(buildConfigBackup(populated(), {true, "", ""}, file));
    JsonDocument doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, file) == DeserializationError::Ok);
    doc["config"]["polling"]["interval_seconds"] = 0;  // below the validated minimum
    std::string tampered;
    serializeJson(doc, tampered);

    BackupContents contents;
    std::string    detail;
    TEST_ASSERT_EQUAL(BackupResult::Corrupt, parseConfigBackup(tampered, contents, detail));
}

static void test_an_oversized_file_is_refused_before_parsing() {
    BackupContents contents;
    std::string    detail;
    TEST_ASSERT_EQUAL(BackupResult::NotJson,
                      parseConfigBackup(std::string(kMaxBackupBytes + 1, 'x'), contents, detail));
}

// --------------------------------------------------------------------------------------
// Preview
// --------------------------------------------------------------------------------------

static const ConfigDiffEntry* find(const std::vector<ConfigDiffEntry>& diff,
                                   const std::string& field) {
    for (const auto& e : diff) {
        if (e.field == field) return &e;
    }
    return nullptr;
}

static void test_identical_configurations_have_an_empty_diff() {
    std::vector<ConfigDiffEntry> diff;
    TEST_ASSERT_TRUE(diffConfigurations(populated(), populated(), diff));
    TEST_ASSERT_EQUAL_size_t(0, diff.size());
}

static void test_diff_names_the_field_and_both_values() {
    Configuration after = populated();
    after.mqtt.host     = "broker.local";
    after.polling.intervalSeconds = 60;

    std::vector<ConfigDiffEntry> diff;
    TEST_ASSERT_TRUE(diffConfigurations(populated(), after, diff));

    const ConfigDiffEntry* host = find(diff, "mqtt.host");
    TEST_ASSERT_NOT_NULL(host);
    TEST_ASSERT_EQUAL_STRING("192.168.20.10", host->before.c_str());
    TEST_ASSERT_EQUAL_STRING("broker.local", host->after.c_str());

    const ConfigDiffEntry* interval = find(diff, "polling.interval_seconds");
    TEST_ASSERT_NOT_NULL(interval);
    TEST_ASSERT_EQUAL_STRING("30", interval->before.c_str());
    TEST_ASSERT_EQUAL_STRING("60", interval->after.c_str());
}

/// The preview is rendered in a browser. A password value reaching it would put the credential
/// on a page that is one screenshot away from a support thread.
static void test_diff_never_renders_a_password_value() {
    Configuration after           = populated();
    after.wifi.password           = "a-different-secret";
    after.security.adminPassword  = "";

    std::vector<ConfigDiffEntry> diff;
    TEST_ASSERT_TRUE(diffConfigurations(populated(), after, diff));
    for (const auto& e : diff) {
        TEST_ASSERT_NULL(std::strstr(e.before.c_str(), "secret"));
        TEST_ASSERT_NULL(std::strstr(e.after.c_str(), "secret"));
    }
    // Changing one secret for another is not a reportable change -- both sides are "(set)" and
    // there is nothing truthful to show. Clearing one is.
    TEST_ASSERT_NULL(find(diff, "wifi.password"));
    const ConfigDiffEntry* admin = find(diff, "security.admin_password");
    TEST_ASSERT_NOT_NULL(admin);
    TEST_ASSERT_EQUAL_STRING("(set)", admin->before.c_str());
    TEST_ASSERT_EQUAL_STRING("(not set)", admin->after.c_str());
}

static void test_diff_reports_a_changed_list_as_one_entry() {
    Configuration after  = populated();
    after.relays.roles   = {"drm1", "drm2"};

    std::vector<ConfigDiffEntry> diff;
    TEST_ASSERT_TRUE(diffConfigurations(populated(), after, diff));
    const ConfigDiffEntry* roles = find(diff, "relays.roles");
    TEST_ASSERT_NOT_NULL(roles);
    TEST_ASSERT_EQUAL_STRING("[\"drm0\",\"none\"]", roles->before.c_str());
    TEST_ASSERT_EQUAL_STRING("[\"drm1\",\"drm2\"]", roles->after.c_str());
}

/// A restore that REMOVES a device is a change the operator needs to see. Walking only the
/// incoming side would miss it entirely.
/// The removal sweep, exercised for real. Every other field exists on both sides -- both
/// documents come from the same serialiser -- so driver options are the ONLY place a key can
/// genuinely disappear, which happens whenever the driver changes. Walking the incoming side
/// alone would show nothing at all here.
static void test_diff_reports_a_driver_option_the_restore_drops() {
    Configuration after = populated();
    after.driver.options.erase("unit_id");

    std::vector<ConfigDiffEntry> diff;
    TEST_ASSERT_TRUE(diffConfigurations(populated(), after, diff));
    const ConfigDiffEntry* dropped = find(diff, "driver.options.unit_id");
    TEST_ASSERT_NOT_NULL(dropped);
    TEST_ASSERT_EQUAL_STRING("3", dropped->before.c_str());
    TEST_ASSERT_EQUAL_STRING("(absent)", dropped->after.c_str());
}

static void test_diff_reports_a_removed_extra_device() {
    Configuration after = populated();
    after.additionalDevices.clear();

    std::vector<ConfigDiffEntry> diff;
    TEST_ASSERT_TRUE(diffConfigurations(populated(), after, diff));
    TEST_ASSERT_NOT_NULL(find(diff, "additional_devices"));
}

/// What the preview actually previews: the merged result, not the file. With a redacted backup
/// the credentials must show as unchanged, because that is what applying it will do.
static void test_preview_of_a_redacted_restore_shows_no_credential_change() {
    Configuration current          = populated();
    current.mqtt.host              = "old.broker";
    std::string file;
    TEST_ASSERT_TRUE(buildConfigBackup(populated(), {false, "", ""}, file));

    Configuration merged = current;
    applyBackup(parseOrFail(file), merged);

    std::vector<ConfigDiffEntry> diff;
    TEST_ASSERT_TRUE(diffConfigurations(current, merged, diff));
    TEST_ASSERT_NULL(find(diff, "wifi.password"));
    TEST_ASSERT_NULL(find(diff, "security.admin_password"));
    TEST_ASSERT_NOT_NULL(find(diff, "mqtt.host"));
}

// --------------------------------------------------------------------------------------
// Timestamp
// --------------------------------------------------------------------------------------

static void test_timestamp_is_iso_utc() {
    TEST_ASSERT_EQUAL_STRING("2026-07-26T12:00:00Z", isoUtcTimestamp(1785067200).c_str());
}

/// An unsynced bridge has an epoch near zero. Stamping the file "1970-01-01" would put a date
/// on it that reads as real; absent is the honest answer.
static void test_an_unsynced_clock_produces_no_timestamp() {
    TEST_ASSERT_TRUE(isoUtcTimestamp(0).empty());
    TEST_ASSERT_TRUE(isoUtcTimestamp(1000).empty());
}

/// A backup carries the addressing, so restoring one taken on another network would hand this
/// bridge an address that cannot work here -- and the preview is what stands between the
/// operator and that. It covers the new fields for free because they go through writeCommon,
/// which is exactly the kind of "free" that stops being true when someone adds a field
/// somewhere else. Pinned so it cannot quietly stop covering them.
static void test_the_restore_preview_shows_an_address_change() {
    Configuration before;
    before.wifi.ssid = "thuis";
    Configuration after = before;
    after.wifi.ip       = "192.168.1.50";
    after.wifi.gateway  = "192.168.1.1";
    after.wifi.subnet   = "255.255.255.0";

    std::vector<ConfigDiffEntry> diff;
    TEST_ASSERT_TRUE(diffConfigurations(before, after, diff));
    TEST_ASSERT_NOT_NULL_MESSAGE(find(diff, "wifi.ip"), "an address change must be previewed");
    TEST_ASSERT_NOT_NULL(find(diff, "wifi.gateway"));
    TEST_ASSERT_NOT_NULL(find(diff, "wifi.subnet"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_the_restore_preview_shows_an_address_change);
    RUN_TEST(test_round_trip_with_secrets_is_lossless);
    RUN_TEST(test_envelope_metadata_is_reported_back);
    RUN_TEST(test_backup_carries_every_stored_key);
    RUN_TEST(test_redacted_backup_contains_no_password_anywhere);
    RUN_TEST(test_absent_password_keeps_the_one_the_bridge_already_has);
    RUN_TEST(test_carried_password_replaces_the_existing_one);
    RUN_TEST(test_an_explicitly_empty_password_is_applied);
    RUN_TEST(test_garbage_is_not_json);
    RUN_TEST(test_a_foreign_json_file_is_refused_as_not_a_backup);
    RUN_TEST(test_a_newer_envelope_is_refused_not_guessed_at);
    RUN_TEST(test_a_newer_inner_config_is_refused_separately);
    RUN_TEST(test_an_envelope_with_no_configuration_is_refused);
    RUN_TEST(test_a_configuration_that_fails_validation_is_refused);
    RUN_TEST(test_an_oversized_file_is_refused_before_parsing);
    RUN_TEST(test_identical_configurations_have_an_empty_diff);
    RUN_TEST(test_diff_names_the_field_and_both_values);
    RUN_TEST(test_diff_never_renders_a_password_value);
    RUN_TEST(test_diff_reports_a_changed_list_as_one_entry);
    RUN_TEST(test_diff_reports_a_driver_option_the_restore_drops);
    RUN_TEST(test_diff_reports_a_removed_extra_device);
    RUN_TEST(test_preview_of_a_redacted_restore_shows_no_credential_change);
    RUN_TEST(test_timestamp_is_iso_utc);
    RUN_TEST(test_an_unsynced_clock_produces_no_timestamp);
    return UNITY_END();
}
