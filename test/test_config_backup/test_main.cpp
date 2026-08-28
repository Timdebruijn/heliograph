// SPDX-License-Identifier: MIT
// Configuration backup: round-trip, redaction, the merge rule for absent credentials, the
// refusal of files this firmware must not apply, and the restore preview.

#include <unity.h>

#include "outputs/rest/rest_payloads.h"

#include <algorithm>

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
    c.driver.label           = "Schuur";
    c.driver.autoDetect      = true;
    c.driver.options["unit_id"] = "3";
    c.additionalDevices.push_back({"mock", false, {{"unit_id", "4"}}, "Balkon"});
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
    // Named explicitly rather than left to operator==: a label that failed to round-trip would
    // come back empty, every surface would silently fall back to the id, and the operator would
    // find their inverters renamed to serial numbers after a restore.
    TEST_ASSERT_EQUAL_STRING("Schuur", restored.driver.label.c_str());
    TEST_ASSERT_EQUAL_STRING("Balkon", restored.additionalDevices[0].label.c_str());
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

    // VALID JSON, and too big. The fixture used to be a wall of 'x', which deserializeJson
    // rejects on its own -- so deleting the size ceiling entirely left this green and the
    // test could not tell "refused for being oversized" from "refused for not being JSON".
    // The ceiling exists to stop a large well-formed document reaching the parser at all, on
    // a device where that memory is not available to lose.
    std::string big = R"({"format_version":1,"configuration":{"note":")";
    big.append(kMaxBackupBytes, 'a');
    big += R"("}})";
    TEST_ASSERT_TRUE_MESSAGE(big.size() > kMaxBackupBytes, "the fixture must exceed the ceiling");
    TEST_ASSERT_EQUAL(BackupResult::NotJson, parseConfigBackup(big, contents, detail));
    TEST_ASSERT_TRUE_MESSAGE(detail.find("larger") != std::string::npos,
                             "and must say it was the SIZE, not the syntax");

    // The original case still holds: not-JSON is also refused, for its own reason.
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

/// The preview is capped at kMaxRestorePreviewBytes and answers 500 when it does not fit, so
/// the cost of expanding arrays into one row per field is a budget question, not a taste one.
///
/// Measured rather than reasoned about: everything different AND the device list full comes to
/// 67 rows and 5000 bytes, under a third of the 16 KB bound. Before arrays were expanded the
/// same restore was a few hundred bytes smaller, so this change spends real budget -- just not
/// much of it.
///
/// The assertion is the BOUND, not the byte count. Pinning 5000 would fail on any harmless
/// wording change; what must not happen is a preview that grows until the operator gets an
/// error instead of the diff they are about to apply.
static void test_the_worst_case_preview_still_fits_its_bound() {
    Configuration before;                  // defaults: every field differs
    Configuration after = populated();
    after.additionalDevices.clear();
    for (int i = 0; i < 7; ++i) {          // kMaxDevices is 8, one of them the primary
        DriverSettings d;
        d.id      = "modbus_profile";
        d.options = {{"unit_id", std::to_string(i + 2)}, {"profile", "mic_tl_x"}};
        after.additionalDevices.push_back(d);
    }

    std::vector<ConfigDiffEntry> diff;
    TEST_ASSERT_TRUE(diffConfigurations(before, after, diff));
    TEST_ASSERT_TRUE_MESSAGE(diff.size() > 40, "the fixture must actually be a large diff");

    BackupContents bc;
    bc.formatVersion   = 1;
    bc.firmwareVersion = "0.26.3";
    bc.exportedAt      = "2026-08-03T19:36:04Z";
    std::string out;
    TEST_ASSERT_TRUE_MESSAGE(
        heliograph::rest::buildRestorePreviewPayload(bc, diff, true, true, out),
        "the worst-case preview must not exceed kMaxRestorePreviewBytes");
    TEST_ASSERT_TRUE_MESSAGE(out.size() < heliograph::rest::kMaxRestorePreviewBytes,
                             "and must fit with room to spare, not exactly");
}

static void test_diff_reports_a_changed_list_position_by_position() {
    // Was "as one entry", and reported ["drm0","none"] -> ["drm1","drm2"] on a single row. That
    // is readable for two short strings and stops being readable the moment the elements are
    // objects: one added device rendered as a hundred characters of JSON on the row somebody is
    // meant to read before applying a restore.
    //
    // Position is what an anonymous array can honestly report. Both roles changed here, so both
    // are listed; only role 0 changing would list only role 0.
    Configuration after = populated();
    after.relays.roles  = {"drm1", "drm2"};

    std::vector<ConfigDiffEntry> diff;
    TEST_ASSERT_TRUE(diffConfigurations(populated(), after, diff));
    TEST_ASSERT_NULL_MESSAGE(find(diff, "relays.roles"), "the whole-array row must be gone");

    const ConfigDiffEntry* first = find(diff, "relays.roles[0]");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_STRING("drm0", first->before.c_str());
    TEST_ASSERT_EQUAL_STRING("drm1", first->after.c_str());

    const ConfigDiffEntry* second = find(diff, "relays.roles[1]");
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_EQUAL_STRING("none", second->before.c_str());
    TEST_ASSERT_EQUAL_STRING("drm2", second->after.c_str());
}

static void test_an_unchanged_list_position_is_not_reported() {
    // The point of walking positions is that only what moved shows up. Reporting every index of
    // a list because one of them changed would be the same wall of text in a different shape.
    Configuration after = populated();
    after.relays.roles  = {"drm0", "drm2"};  // index 0 unchanged

    std::vector<ConfigDiffEntry> diff;
    TEST_ASSERT_TRUE(diffConfigurations(populated(), after, diff));
    TEST_ASSERT_NULL(find(diff, "relays.roles[0]"));
    TEST_ASSERT_NOT_NULL(find(diff, "relays.roles[1]"));
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

static void test_diff_reports_a_removed_extra_device_field_by_field() {
    // The case that prompted this: removing a device and previewing the backup that restores it
    // showed one row of serialised JSON. Field by field says WHICH device and on which unit,
    // which is the question being asked before pressing apply.
    Configuration after = populated();
    after.additionalDevices.clear();

    std::vector<ConfigDiffEntry> diff;
    TEST_ASSERT_TRUE(diffConfigurations(populated(), after, diff));
    TEST_ASSERT_NULL_MESSAGE(find(diff, "additional_devices"), "no blob row");

    const ConfigDiffEntry* driver = find(diff, "additional_devices[0].driver_id");
    TEST_ASSERT_NOT_NULL(driver);
    TEST_ASSERT_EQUAL_STRING("(absent)", driver->after.c_str());
    TEST_ASSERT_TRUE_MESSAGE(!driver->before.empty() && driver->before != "(absent)",
                             "the removed device must name itself");
}

static void test_an_added_device_is_reported_field_by_field() {
    // The direction the screenshot showed: restoring a backup onto a bridge the device was
    // deleted from. Nested options must descend too -- unit_id is the field that says which
    // device on the bus this is.
    Configuration before = populated();
    before.additionalDevices.clear();

    std::vector<ConfigDiffEntry> diff;
    TEST_ASSERT_TRUE(diffConfigurations(before, populated(), diff));
    TEST_ASSERT_NULL_MESSAGE(find(diff, "additional_devices"), "no blob row");

    const ConfigDiffEntry* driver = find(diff, "additional_devices[0].driver_id");
    TEST_ASSERT_NOT_NULL(driver);
    TEST_ASSERT_EQUAL_STRING("(absent)", driver->before.c_str());
    TEST_ASSERT_TRUE(!driver->after.empty() && driver->after != "(absent)");

    // Whatever options the fixture device carries, they must arrive as their own rows rather
    // than as a nested blob inside the element.
    const bool nestedOption = std::any_of(diff.begin(), diff.end(), [](const ConfigDiffEntry& e) {
        return e.field.rfind("additional_devices[0].options.", 0) == 0;
    });
    TEST_ASSERT_TRUE_MESSAGE(nestedOption, "options must descend, not render as one value");
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
    RUN_TEST(test_the_worst_case_preview_still_fits_its_bound);
    RUN_TEST(test_diff_reports_a_changed_list_position_by_position);
    RUN_TEST(test_an_unchanged_list_position_is_not_reported);
    RUN_TEST(test_diff_reports_a_driver_option_the_restore_drops);
    RUN_TEST(test_diff_reports_a_removed_extra_device_field_by_field);
    RUN_TEST(test_an_added_device_is_reported_field_by_field);
    RUN_TEST(test_preview_of_a_redacted_restore_shows_no_credential_change);
    RUN_TEST(test_timestamp_is_iso_utc);
    RUN_TEST(test_an_unsynced_clock_produces_no_timestamp);
    return UNITY_END();
}
