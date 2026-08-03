// SPDX-License-Identifier: MIT
// Configuration persistence, migration, provisioning state machine and OTA validation.

#include <unity.h>

#include <cstring>
#include <set>
#include <string>

#include <ArduinoJson.h>

#include "config/configuration_store.h"
#include "network/provisioning_policy.h"
#include "ota/ota_manager.h"
#include "ota/sha256.h"
#include "support/configured_device.h"

using namespace heliograph;
using heliograph::ota::looksLikeFirmware;

void setUp() {}
void tearDown() {}

static Configuration provisionedConfig() {
    Configuration c;
    c.bridgeName             = "Zolder";
    c.wifi.ssid              = "thuisnetwerk";
    c.wifi.password          = "GeheimWifiWachtwoord";
    c.mqtt.enabled           = true;
    c.mqtt.host              = "10.0.0.5";
    c.mqtt.password          = "GeheimMqttWachtwoord";
    c.security.adminPassword = "GeheimAdminWachtwoord";
    c.polling.intervalSeconds = 30;
    c.driver.id              = "eversolar_legacy";
    c.driver.options["layout"] = "dual";
    return c;
}

/// A full-section PATCH of the shape the settings form used to send when only the Modbus
/// checkbox was cleared. Kept verbatim as a back-compat case rather than updated: the current
/// form no longer sends admin_username unless it is typed, and always sends read_only_mode, so
/// this body is now an OLD client's -- which is exactly what makes it worth still accepting.
static void test_turning_modbus_off_leaves_mqtt_running_after_a_restart() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    auto               c = provisionedConfig();
    TEST_ASSERT_TRUE(store.save(c));

    const char* body = R"({"bridge_name":"Zolder",
        "wifi":{"ssid":"thuisnetwerk","hostname":"heliograph"},
        "mqtt":{"enabled":true,"host":"10.0.0.5","port":1883,"username":"tim",
                "base_topic":"heliograph","discovery_enabled":true},
        "modbus":{"enabled":false,"port":502,"unit_id":1},
        "polling":{"interval_seconds":30},
        "driver":{"id":"eversolar_legacy","options":{}},
        "rs485":{"baud_rate":9600,"parity":"none"},)"  // section removed 0.4.14; an old
        // client still sending it must not break the PATCH
        R"(
        "security":{"admin_username":"admin"},
        "logging":{"level":"info"}})";
    ConfigError e;
    TEST_ASSERT_TRUE(applyConfigPatch(body, c, e));
    TEST_ASSERT_TRUE(store.save(c));

    // Reboot: a fresh store reading the same backend is exactly what setup() does.
    ConfigurationStore reloaded(backend);
    Configuration      after;
    TEST_ASSERT_EQUAL(LoadResult::Ok, reloaded.load(after));

    TEST_ASSERT_FALSE(after.modbus.enabled);
    // The two outputs are independent. Nothing about switching one off may touch the other.
    TEST_ASSERT_TRUE(after.mqtt.enabled);
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", after.mqtt.host.c_str());
    TEST_ASSERT_EQUAL_STRING("GeheimMqttWachtwoord", after.mqtt.password.c_str());
    TEST_ASSERT_TRUE(after.mqtt.discoveryEnabled);
}

// --- round trip ---------------------------------------------------------------------------

static void test_first_boot_finds_nothing_and_keeps_defaults() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    Configuration      c;
    c.bridgeName = "untouched";

    TEST_ASSERT_EQUAL(LoadResult::NotFound, store.load(c));
    TEST_ASSERT_EQUAL_STRING("untouched", c.bridgeName.c_str());
    TEST_ASSERT_FALSE(c.provisioned());
}

static void test_save_then_load_round_trips_everything() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    const auto         original = provisionedConfig();
    TEST_ASSERT_TRUE(store.save(original));

    Configuration loaded;
    TEST_ASSERT_EQUAL(LoadResult::Ok, store.load(loaded));

    TEST_ASSERT_EQUAL_STRING("Zolder", loaded.bridgeName.c_str());
    TEST_ASSERT_EQUAL_STRING("thuisnetwerk", loaded.wifi.ssid.c_str());
    TEST_ASSERT_EQUAL_UINT32(30, loaded.polling.intervalSeconds);
    TEST_ASSERT_EQUAL_STRING("eversolar_legacy", loaded.driver.id.c_str());
    TEST_ASSERT_EQUAL_STRING("dual", loaded.driver.options.at("layout").c_str());
    TEST_ASSERT_TRUE(loaded.provisioned());
}

static void test_config_under_the_legacy_namespace_is_adopted() {
    // The 0.5.0 rename changed the NVS namespace and stranded every existing config under
    // the old name: an OTA'd device booted "unprovisioned" with its settings intact in
    // flash (live, 2026-07-22). load() must adopt the legacy blob, persist it under the
    // new namespace, and leave the legacy copy alone so a 0.4.x rollback still finds it.
    MemoryBackend legacy;
    {
        ConfigurationStore old(legacy);
        TEST_ASSERT_TRUE(old.save(provisionedConfig()));
    }
    MemoryBackend      fresh;
    ConfigurationStore store(fresh, &legacy);

    Configuration loaded;
    TEST_ASSERT_EQUAL(LoadResult::Migrated, store.load(loaded));
    TEST_ASSERT_TRUE(loaded.provisioned());
    TEST_ASSERT_EQUAL_STRING("thuisnetwerk", loaded.wifi.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("GeheimMqttWachtwoord", loaded.mqtt.password.c_str());

    // Persisted under the new namespace: the next load no longer needs the legacy source.
    TEST_ASSERT_TRUE(fresh.contains(kStorageKeyConfig));
    ConfigurationStore rebooted(fresh);
    Configuration      after;
    TEST_ASSERT_EQUAL(LoadResult::Ok, rebooted.load(after));
    TEST_ASSERT_EQUAL_STRING("thuisnetwerk", after.wifi.ssid.c_str());

    // Legacy copy untouched (rollback safety).
    TEST_ASSERT_TRUE(legacy.contains(kStorageKeyConfig));
}

static void test_primary_config_wins_over_legacy() {
    // Once anything is stored under the new namespace, the legacy blob is history -- it
    // must never override newer settings on a later boot.
    MemoryBackend legacy;
    {
        ConfigurationStore old(legacy);
        auto               stale = provisionedConfig();
        stale.bridgeName         = "Oud";
        TEST_ASSERT_TRUE(old.save(stale));
    }
    MemoryBackend fresh;
    {
        ConfigurationStore current(fresh);
        auto               newer = provisionedConfig();
        newer.bridgeName         = "Nieuw";
        TEST_ASSERT_TRUE(current.save(newer));
    }
    ConfigurationStore store(fresh, &legacy);
    Configuration      loaded;
    TEST_ASSERT_EQUAL(LoadResult::Ok, store.load(loaded));
    TEST_ASSERT_EQUAL_STRING("Nieuw", loaded.bridgeName.c_str());
}

static void test_relays_enabled_defaults_off_and_round_trips() {
    // A relay board with factory settings must be inert: the flag defaults to false, and
    // only an explicit patch turns it on. It must survive storage like everything else.
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    auto               c = provisionedConfig();
    TEST_ASSERT_FALSE(c.relays.enabled);

    ConfigError e;
    TEST_ASSERT_TRUE(applyConfigPatch(R"({"relays":{"enabled":true}})", c, e));
    TEST_ASSERT_TRUE(c.relays.enabled);
    TEST_ASSERT_TRUE(store.save(c));

    ConfigurationStore reloaded(backend);
    Configuration      after;
    TEST_ASSERT_EQUAL(LoadResult::Ok, reloaded.load(after));
    TEST_ASSERT_TRUE(after.relays.enabled);
}

static void test_relay_roles_validate_and_round_trip() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    auto               c = provisionedConfig();
    ConfigError        e;

    // A bad role never lands: the patch is refused as a whole.
    TEST_ASSERT_FALSE(applyConfigPatch(R"({"relays":{"roles":["drm9"]}})", c, e));
    TEST_ASSERT_EQUAL_STRING("relays.roles", e.field.c_str());
    TEST_ASSERT_TRUE(c.relays.roles.empty());

    TEST_ASSERT_TRUE(applyConfigPatch(R"({"relays":{"roles":["drm0","none"]}})", c, e));
    TEST_ASSERT_TRUE(store.save(c));

    ConfigurationStore reloaded(backend);
    Configuration      after;
    TEST_ASSERT_EQUAL(LoadResult::Ok, reloaded.load(after));
    TEST_ASSERT_EQUAL_UINT32(2, after.relays.roles.size());
    TEST_ASSERT_EQUAL_STRING("drm0", after.relays.roles[0].c_str());
    TEST_ASSERT_EQUAL_STRING("none", after.relays.roles[1].c_str());
}

// --- ntp ----------------------------------------------------------------------------------

static void test_ntp_settings_round_trip_through_storage() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    auto               c = provisionedConfig();
    c.ntp.enabled      = true;
    c.ntp.useDhcp      = false;
    c.ntp.server       = "192.168.1.1";
    c.ntp.timezone     = "UTC0";
    c.ntp.timezoneName = "UTC";
    TEST_ASSERT_TRUE(store.save(c));

    Configuration loaded;
    TEST_ASSERT_EQUAL(LoadResult::Ok, store.load(loaded));
    TEST_ASSERT_FALSE(loaded.ntp.useDhcp);
    TEST_ASSERT_EQUAL_STRING("192.168.1.1", loaded.ntp.server.c_str());
    TEST_ASSERT_EQUAL_STRING("UTC0", loaded.ntp.timezone.c_str());
    // The IANA label survives too: the dropdown must re-select the city the user picked, not
    // whichever city happens to share the same POSIX rules.
    TEST_ASSERT_EQUAL_STRING("UTC", loaded.ntp.timezoneName.c_str());
}

static void test_ntp_has_defaults_that_work_out_of_the_box() {
    Configuration c;  // fresh: no network configured yet
    TEST_ASSERT_TRUE(c.ntp.enabled);
    TEST_ASSERT_TRUE(c.ntp.useDhcp);
    TEST_ASSERT_FALSE(c.ntp.server.empty());    // a public default so the clock works anywhere
    TEST_ASSERT_FALSE(c.ntp.timezone.empty());  // logs need a zone from the very first line
    ConfigError e;
    TEST_ASSERT_TRUE(validate(c, e));
}

static void test_ntp_patch_applies_from_the_settings_form() {
    Configuration c;
    const char*   body = R"({"ntp":{"enabled":true,"use_dhcp":true,"server":"nl.pool.ntp.org",
                             "timezone":"GMT0BST,M3.5.0/1,M10.5.0","timezone_name":"Europe/London"}})";
    ConfigError   e;
    TEST_ASSERT_TRUE(applyConfigPatch(body, c, e));
    TEST_ASSERT_TRUE(c.ntp.useDhcp);
    TEST_ASSERT_EQUAL_STRING("nl.pool.ntp.org", c.ntp.server.c_str());
    TEST_ASSERT_EQUAL_STRING("GMT0BST,M3.5.0/1,M10.5.0", c.ntp.timezone.c_str());
    TEST_ASSERT_EQUAL_STRING("Europe/London", c.ntp.timezoneName.c_str());
}

static void test_ntp_without_dhcp_needs_a_server() {
    Configuration c;
    c.ntp.enabled = true;
    c.ntp.useDhcp = false;
    c.ntp.server.clear();
    ConfigError e;
    TEST_ASSERT_FALSE(validate(c, e));  // no DHCP and no server means no clock source at all
    // With DHCP on, an empty server is fine: the network supplies one, and a wrong network just
    // leaves the clock unsynced rather than refusing to boot.
    c.ntp.useDhcp = true;
    TEST_ASSERT_TRUE(validate(c, e));
}

static void test_ntp_timezone_must_not_be_empty() {
    Configuration c;
    c.ntp.timezone.clear();
    ConfigError e;
    TEST_ASSERT_FALSE(validate(c, e));
}

static void test_secrets_survive_the_round_trip() {
    // Storage is the one place passwords must persist. If they did not, every reboot would
    // silently drop the device off WiFi.
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    store.save(provisionedConfig());

    Configuration loaded;
    store.load(loaded);
    TEST_ASSERT_EQUAL_STRING("GeheimWifiWachtwoord", loaded.wifi.password.c_str());
    TEST_ASSERT_EQUAL_STRING("GeheimMqttWachtwoord", loaded.mqtt.password.c_str());
    TEST_ASSERT_EQUAL_STRING("GeheimAdminWachtwoord", loaded.security.adminPassword.c_str());
}

static void test_the_storage_serialiser_is_the_only_one_that_writes_secrets() {
    // Two functions, deliberately different names. serializeConfig() is what REST uses and
    // must never emit a password; serializeConfigForStorage() must.
    const auto  c = provisionedConfig();
    std::string forApi;
    std::string forStorage;
    TEST_ASSERT_TRUE(serializeConfig(c, forApi));
    TEST_ASSERT_TRUE(serializeConfigForStorage(c, forStorage));

    TEST_ASSERT_TRUE(forApi.find("GeheimWifiWachtwoord") == std::string::npos);
    TEST_ASSERT_TRUE(forStorage.find("GeheimWifiWachtwoord") != std::string::npos);
}

/// Collects every key PATH in a document ("mqtt.qos", "relays.roles"), so two documents can be
/// compared on shape rather than on the order or the values they happen to carry.
static void collectKeyPaths(JsonVariantConst v, const std::string& prefix,
                            std::set<std::string>& out) {
    if (!v.is<JsonObjectConst>()) {
        return;
    }
    for (JsonPairConst kv : v.as<JsonObjectConst>()) {
        const std::string path = prefix.empty() ? std::string(kv.key().c_str())
                                                : prefix + "." + kv.key().c_str();
        out.insert(path);
        collectKeyPaths(kv.value(), path, out);  // arrays stop here, which is what we want
    }
}

/// THE invariant behind config_sections::writeCommon.
///
/// The API document and the stored document are written by one shared function plus a short
/// per-caller tail. This asserts that the tail is the ONLY thing that differs: every key in one
/// must appear in the other, except the credential keys listed here by name.
///
/// It exists because the failure it guards is silent. Before the writers were merged, a setting
/// added to serializeConfig and forgotten in serializeConfigForStorage produced a settings page
/// that showed the field, accepted a change, reported success, and lost it on the next reboot --
/// nothing threw and nothing logged. Merging the writers makes that mistake hard; this makes it
/// impossible to land unnoticed, including if someone later adds a field to one tail instead of
/// to writeCommon().
static void test_the_two_config_documents_differ_only_in_their_credentials() {
    const auto  c = provisionedConfig();
    std::string forApi;
    std::string forStorage;
    TEST_ASSERT_TRUE(serializeConfig(c, forApi));
    TEST_ASSERT_TRUE(serializeConfigForStorage(c, forStorage));

    JsonDocument api;
    JsonDocument stored;
    TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(api, forApi).code());
    TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(stored, forStorage).code());

    std::set<std::string> apiKeys;
    std::set<std::string> storedKeys;
    collectKeyPaths(api.as<JsonVariantConst>(), "", apiKeys);
    collectKeyPaths(stored.as<JsonVariantConst>(), "", storedKeys);

    // The redaction, stated as data. Anything outside these two lists that appears in only one
    // document is drift, not design.
    const std::set<std::string> apiOnly{"wifi.password_set", "mqtt.username_set",
                                        "mqtt.password_set", "security.password_set"};
    const std::set<std::string> storedOnly{"wifi.password",          "mqtt.username",
                                           "mqtt.password",         "security.admin_username",
                                           "security.admin_password"};

    for (const auto& key : apiKeys) {
        if (apiOnly.count(key) != 0) continue;
        TEST_ASSERT_TRUE_MESSAGE(storedKeys.count(key) != 0,
                                 ("in the API document but not stored: " + key).c_str());
    }
    for (const auto& key : storedKeys) {
        if (storedOnly.count(key) != 0) continue;
        TEST_ASSERT_TRUE_MESSAGE(apiKeys.count(key) != 0,
                                 ("stored but not in the API document: " + key).c_str());
    }
    // And the redactions themselves are really present, so the lists above cannot silently rot
    // into a pair of unused constants that make the loops above vacuous.
    for (const auto& key : apiOnly) {
        TEST_ASSERT_TRUE_MESSAGE(apiKeys.count(key) != 0, key.c_str());
    }
    for (const auto& key : storedOnly) {
        TEST_ASSERT_TRUE_MESSAGE(storedKeys.count(key) != 0, key.c_str());
    }
}

static void test_factory_reset_wipes_credentials() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    store.save(provisionedConfig());
    TEST_ASSERT_TRUE(backend.contains(kStorageKeyConfig));

    TEST_ASSERT_TRUE(store.factoryReset());
    TEST_ASSERT_FALSE(backend.contains(kStorageKeyConfig));
    // Nothing recoverable is left behind in the blob.
    TEST_ASSERT_TRUE(backend.raw(kStorageKeyConfig).empty());

    Configuration loaded;
    TEST_ASSERT_EQUAL(LoadResult::NotFound, store.load(loaded));
}

static void test_a_failed_write_is_reported_not_swallowed() {
    // The REST handler turns this into a 500. A save that silently does nothing is how a user
    // discovers at reboot that their settings never existed.
    MemoryBackend backend;
    backend.writeFails = true;
    ConfigurationStore store(backend);
    TEST_ASSERT_FALSE(store.save(provisionedConfig()));
}

static void test_an_invalid_config_is_never_persisted() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    auto               c = provisionedConfig();
    c.polling.intervalSeconds = 0;  // out of range
    TEST_ASSERT_FALSE(store.save(c));
    TEST_ASSERT_FALSE(backend.contains(kStorageKeyConfig));
}

/// On by default, and it has to survive a reboot like anything else -- an operator who turned
/// the check off did so for a reason, and a bridge that quietly starts asking github.io again
/// after a power cut has ignored them.
static void test_the_update_check_defaults_on_and_round_trips() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    auto               c = provisionedConfig();
    TEST_ASSERT_TRUE(c.updates.checkEnabled);

    ConfigError e;
    TEST_ASSERT_TRUE(applyConfigPatch(R"({"updates":{"check_enabled":false}})", c, e));
    TEST_ASSERT_FALSE(c.updates.checkEnabled);
    TEST_ASSERT_TRUE(store.save(c));

    ConfigurationStore reloaded(backend);
    Configuration      after;
    TEST_ASSERT_EQUAL(LoadResult::Ok, reloaded.load(after));
    TEST_ASSERT_FALSE(after.updates.checkEnabled);
}

/// A config stored before this setting existed has no `updates` key at all. It must come back
/// as the default rather than as false, or every existing bridge silently opts out.
static void test_a_config_without_the_setting_keeps_the_default() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    store.save(provisionedConfig());
    // Strip the key, exactly as an older firmware's blob would look.
    std::string blob = backend.raw(kStorageKeyConfig);
    const size_t at  = blob.find(",\"updates\":{\"check_enabled\":true}");
    TEST_ASSERT_TRUE(at != std::string::npos);
    blob.erase(at, std::string(",\"updates\":{\"check_enabled\":true}").size());
    backend.write(kStorageKeyConfig, blob);

    Configuration after;
    TEST_ASSERT_EQUAL(LoadResult::Ok, store.load(after));
    TEST_ASSERT_TRUE(after.updates.checkEnabled);
}

// --- the rollback slot ------------------------------------------------------------------------

static void test_nothing_to_roll_back_to_on_a_fresh_board() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    TEST_ASSERT_FALSE(store.hasRollback());
    TEST_ASSERT_FALSE(store.stashRollback());
    Configuration out;
    TEST_ASSERT_EQUAL(LoadResult::NotFound, store.rollback(out));
}

static void test_stash_then_rollback_returns_the_earlier_configuration() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    auto               before = provisionedConfig();
    before.bridgeName         = "before restore";
    TEST_ASSERT_TRUE(store.save(before));

    TEST_ASSERT_TRUE(store.stashRollback());
    TEST_ASSERT_TRUE(store.hasRollback());

    auto after       = provisionedConfig();
    after.bridgeName = "after restore";
    TEST_ASSERT_TRUE(store.save(after));

    Configuration recovered;
    TEST_ASSERT_EQUAL(LoadResult::Ok, store.rollback(recovered));
    TEST_ASSERT_EQUAL_STRING("before restore", recovered.bridgeName.c_str());

    // ...and it is live, not merely returned: the next boot must find it too.
    Configuration reloaded;
    TEST_ASSERT_EQUAL(LoadResult::Ok, store.load(reloaded));
    TEST_ASSERT_EQUAL_STRING("before restore", reloaded.bridgeName.c_str());
}

/// The swap. Pressing undo twice returns to where you were, rather than doing nothing the
/// second time with no way to explain why.
static void test_rolling_back_twice_returns_to_the_restored_configuration() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    auto               before = provisionedConfig();
    before.bridgeName         = "before restore";
    store.save(before);
    store.stashRollback();
    auto after       = provisionedConfig();
    after.bridgeName = "after restore";
    store.save(after);

    Configuration first;
    TEST_ASSERT_EQUAL(LoadResult::Ok, store.rollback(first));
    TEST_ASSERT_EQUAL_STRING("before restore", first.bridgeName.c_str());

    Configuration second;
    TEST_ASSERT_EQUAL(LoadResult::Ok, store.rollback(second));
    TEST_ASSERT_EQUAL_STRING("after restore", second.bridgeName.c_str());
}

/// The safety net is allowed to fail. NVS here is 20 KB shared with the WiFi stack, so a
/// second copy of the configuration is the first thing that will not fit -- and the restore
/// must still be possible, with the caller told there is no undo.
static void test_a_rollback_slot_that_does_not_fit_is_reported_not_fatal() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    store.save(provisionedConfig());
    backend.writeFails = true;
    TEST_ASSERT_FALSE(store.stashRollback());
    backend.writeFails = false;
    TEST_ASSERT_FALSE(store.hasRollback());
}

/// A rollback copy this firmware can no longer read must leave the live configuration alone.
/// Whoever reaches for undo is already recovering from something; half-applying is worse than
/// refusing.
static void test_an_unreadable_rollback_leaves_the_live_config_untouched() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    auto               live = provisionedConfig();
    live.bridgeName         = "still running";
    store.save(live);
    backend.write(kStorageKeyRollback, "{not json");

    Configuration out;
    TEST_ASSERT_EQUAL(LoadResult::Corrupt, store.rollback(out));

    Configuration reloaded;
    TEST_ASSERT_EQUAL(LoadResult::Ok, store.load(reloaded));
    TEST_ASSERT_EQUAL_STRING("still running", reloaded.bridgeName.c_str());
}

static void test_factory_reset_takes_the_rollback_slot_with_it() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    store.save(provisionedConfig());
    TEST_ASSERT_TRUE(store.stashRollback());

    TEST_ASSERT_TRUE(store.factoryReset());
    TEST_ASSERT_FALSE(store.hasRollback());
    // Explicitly: the stashed copy holds the WiFi and admin passwords too, so leaving it
    // behind would make "erases everything, including passwords" untrue.
    TEST_ASSERT_TRUE(backend.raw(kStorageKeyRollback).empty());
}

// --- corruption and versions ----------------------------------------------------------------

static void test_corrupt_blob_falls_back_to_defaults() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    backend.write(kStorageKeyConfig, "{this is not json");

    Configuration c;
    c.bridgeName = "untouched";
    TEST_ASSERT_EQUAL(LoadResult::Corrupt, store.load(c));
    // Never half-populated: a partially parsed config is worse than defaults.
    TEST_ASSERT_EQUAL_STRING("untouched", c.bridgeName.c_str());
}

static void test_blob_without_a_version_is_corrupt() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    backend.write(kStorageKeyConfig, R"({"bridge_name":"x"})");

    Configuration c;
    TEST_ASSERT_EQUAL(LoadResult::Corrupt, store.load(c));
}

static void test_a_newer_version_is_refused_not_guessed_at() {
    // After a downgrade the flash holds a config from newer firmware. Reinterpreting fields
    // we do not understand produces a plausible-looking wrong configuration, which is worse
    // than starting fresh.
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    backend.write(kStorageKeyConfig, R"({"version":99,"bridge_name":"from the future"})");

    Configuration c;
    c.bridgeName = "untouched";
    TEST_ASSERT_EQUAL(LoadResult::FutureVersion, store.load(c));
    TEST_ASSERT_EQUAL_STRING("untouched", c.bridgeName.c_str());
}

// The rollback-safety complement (review 2026-07-21): a FutureVersion config must not cost
// the device its network identity. After a rollback the OLDER binary reads a NEWER config;
// refusing the whole document put the device in the setup AP at exactly the moment the
// rollback safety net fired -- worse off than before the update. Credentials (WiFi + admin)
// have been stable since version 1 and are salvaged; feature settings stay at defaults.
static void test_a_newer_version_still_keeps_the_network_identity() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    backend.write(kStorageKeyConfig,
                  R"({"version":99,"bridge_name":"from the future",)"
                  R"("wifi":{"ssid":"thuis","password":"geheim","hostname":"heliograph"},)"
                  R"("security":{"admin_username":"admin","admin_password":"sterk"},)"
                  R"("mqtt":{"host":"broker.future"}})");

    Configuration c;
    TEST_ASSERT_EQUAL(LoadResult::FutureVersion, store.load(c));
    // Identity survives: the device reconnects and stays remotely reachable.
    TEST_ASSERT_EQUAL_STRING("thuis", c.wifi.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("geheim", c.wifi.password.c_str());
    TEST_ASSERT_EQUAL_STRING("heliograph", c.wifi.hostname.c_str());
    TEST_ASSERT_EQUAL_STRING("admin", c.security.adminUsername.c_str());
    TEST_ASSERT_EQUAL_STRING("sterk", c.security.adminPassword.c_str());
    TEST_ASSERT_TRUE(c.provisioned());
    // Feature settings are NOT reinterpreted: they fall back to defaults.
    TEST_ASSERT_EQUAL_STRING("", c.mqtt.host.c_str());
}

static void test_a_stored_config_that_no_longer_validates_is_corrupt() {
    // A range tightened in newer firmware. Running on values we would refuse to accept is how
    // a device ends up in a state nobody can reason about.
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    backend.write(kStorageKeyConfig,
                  R"({"version":1,"polling":{"interval_seconds":99999}})");

    Configuration c;
    TEST_ASSERT_EQUAL(LoadResult::Corrupt, store.load(c));
}

static void test_missing_fields_fall_back_to_defaults() {
    // Forward compatibility: an older blob simply lacks keys a newer firmware knows.
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    backend.write(kStorageKeyConfig, R"({"version":1,"wifi":{"ssid":"minimaal"}})");

    Configuration c;
    TEST_ASSERT_EQUAL(LoadResult::Migrated, store.load(c));
    TEST_ASSERT_EQUAL_STRING("minimaal", c.wifi.ssid.c_str());
    TEST_ASSERT_EQUAL_UINT32(10, c.polling.intervalSeconds);  // default
    TEST_ASSERT_TRUE(c.security.readOnlyMode);                // default, and it matters
}

static void test_the_stored_blob_fits_nvs_comfortably() {
    // NVS caps a string entry at 4000 bytes. With every field at its maximum length the blob
    // must still fit, or a legal configuration would be unsaveable.
    Configuration c;
    c.bridgeName             = std::string(64, 'x');
    c.wifi.ssid              = std::string(32, 'x');
    c.wifi.password          = std::string(64, 'x');
    c.wifi.hostname          = std::string(32, 'x');
    c.mqtt.host              = std::string(128, 'x');
    c.mqtt.username          = std::string(64, 'x');
    c.mqtt.password          = std::string(128, 'x');
    c.mqtt.baseTopic         = std::string(64, 'x');
    c.mqtt.discoveryPrefix   = std::string(64, 'x');
    c.driver.id              = std::string(64, 'x');
    c.security.adminUsername = std::string(32, 'x');
    c.security.adminPassword = std::string(64, 'x');
    c.driver.options["layout"] = std::string(128, 'x');

    c.wifi.ip      = "192.168.100.200";
    c.wifi.gateway = "192.168.100.254";
    c.wifi.subnet  = "255.255.255.0";
    c.wifi.dns1    = "192.168.100.253";
    c.wifi.dns2    = "192.168.100.252";

    ConfigError e;
    TEST_ASSERT_TRUE_MESSAGE(validate(c, e), "a maximal config must still be valid");
    std::string blob;
    TEST_ASSERT_TRUE(serializeConfigForStorage(c, blob));
    TEST_ASSERT_TRUE(blob.size() < kMaxStoredConfigBytes);

    // The property that actually matters: even everything-at-maximum leaves most of the entry
    // free, so the next field to be added is not a cliff. Measured 1822 of 3900 when static
    // addressing was added (0.16.0); the assertion is half the cap, so it has room to drift and
    // still fails before anything is at risk.
    TEST_ASSERT_TRUE_MESSAGE(blob.size() < kMaxStoredConfigBytes / 2,
                             std::to_string(blob.size()).c_str());

    std::string typical;
    TEST_ASSERT_TRUE(serializeConfigForStorage(provisionedConfig(), typical));
    // A canary, not a limit. Moved from 1000 to 1200 when the five addressing fields were added
    // -- they are emitted empty on a DHCP bridge, which costs ~55 bytes and took a typical blob
    // from 983 to 1038. Raised deliberately and with the numbers written down, because a
    // threshold quietly nudged whenever it trips stops being evidence of anything.
    TEST_ASSERT_TRUE_MESSAGE(typical.size() < 1200, std::to_string(typical.size()).c_str());
}

static Configuration staticConfig() {
    auto c         = provisionedConfig();
    c.wifi.ip      = "192.168.1.50";
    c.wifi.gateway = "192.168.1.1";
    c.wifi.subnet  = "255.255.255.0";
    c.wifi.dns1    = "192.168.1.1";
    return c;
}

/// Whatever the writer emits, the reader must take back — all of it.
///
/// Serialise, parse, serialise again: if the reader ignores a field the writer produced, the
/// second blob differs and this fails, naming nothing in particular but failing reliably. That
/// is the point — it needs no per-field maintenance, so it keeps working for fields nobody has
/// thought of yet.
///
/// Written because exactly this went wrong while adding static addressing: the five new keys
/// were added to writeCommon and forgotten in deserializeConfigFromStorage, so a bridge would
/// have saved its static address and come back on DHCP after a reboot. The writer-vs-writer
/// drift check could not see it — it compares the two documents to each other, and both were
/// correct. Nothing else in the suite covered the return trip in full.
static void test_everything_the_writer_emits_the_reader_takes_back() {
    auto c = staticConfig();
    c.additionalDevices.push_back([] {
        DriverSettings d;
        d.id                = "sunspec";
        d.options["unit_id"] = "2";
        return d;
    }());
    c.relays.roles          = {"drm0", "drm5"};
    c.relays.enabled        = true;
    c.serial.enabled        = true;
    c.serial.profile.parity = SerialParity::Even;
    c.updates.checkEnabled  = false;
    c.logLevel              = LogLevel::Debug;

    std::string first;
    TEST_ASSERT_TRUE(serializeConfigForStorage(c, first));

    Configuration back;
    TEST_ASSERT_EQUAL(LoadResult::Ok, deserializeConfigFromStorage(first, back));

    std::string second;
    TEST_ASSERT_TRUE(serializeConfigForStorage(back, second));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(first.c_str(), second.c_str(),
                                     "a field the writer emits is not read back");
}

// --- static addressing ------------------------------------------------------------------------
//
// Each of these refuses a configuration that would otherwise be stored, applied at the next
// boot, and leave a bridge unreachable at whatever height it is mounted. The PATCH handler is
// the last moment anyone is looking at it, so that is where they live.

static void test_a_sound_static_configuration_is_accepted() {
    ConfigError e;
    TEST_ASSERT_TRUE_MESSAGE(validate(staticConfig(), e), e.field.c_str());
}

static void test_an_empty_ip_means_dhcp_and_needs_nothing_else() {
    auto        c = provisionedConfig();
    ConfigError e;
    TEST_ASSERT_TRUE(validate(c, e));
    TEST_ASSERT_FALSE(c.wifi.staticIp());
}

/// A gateway left behind after clearing the address is a half-configuration that reads as if it
/// were in effect. Refused so the form cannot lie about what the bridge will do.
static void test_leftover_fields_without_an_ip_are_refused() {
    auto        c = provisionedConfig();
    ConfigError e;
    c.wifi.gateway = "192.168.1.1";
    TEST_ASSERT_FALSE(validate(c, e));
    TEST_ASSERT_EQUAL_STRING("wifi.gateway", e.field.c_str());
}

static void test_the_address_fields_must_parse() {
    ConfigError e;
    struct { const char* value; const char* field; } cases[] = {
        {"192.168.1", "wifi.ip"},
        {"010.0.0.5", "wifi.ip"},
        {"not-an-ip", "wifi.ip"},
    };
    for (const auto& tc : cases) {
        auto c    = staticConfig();
        c.wifi.ip = tc.value;
        TEST_ASSERT_FALSE_MESSAGE(validate(c, e), tc.value);
        TEST_ASSERT_EQUAL_STRING(tc.field, e.field.c_str());
    }
}

/// The mask rule is not style. WiFiSTAClass::config() reinterprets its arguments as the ESP8266
/// ordering when the mask's first octet is not 255 -- so an odd mask does not fail, it silently
/// configures a different gateway.
static void test_a_mask_the_arduino_core_would_reinterpret_is_refused() {
    ConfigError e;
    for (const char* mask : {"0.0.0.0", "255.0.255.0", "128.0.0.0", "255.255.255.255"}) {
        auto c        = staticConfig();
        c.wifi.subnet = mask;
        TEST_ASSERT_FALSE_MESSAGE(validate(c, e), mask);
        TEST_ASSERT_EQUAL_STRING("wifi.subnet", e.field.c_str());
    }
}

static void test_an_unreachable_gateway_is_refused() {
    auto        c  = staticConfig();
    ConfigError e;
    c.wifi.gateway = "10.0.0.1";  // not inside 192.168.1.0/24
    TEST_ASSERT_FALSE(validate(c, e));
    TEST_ASSERT_EQUAL_STRING("wifi.gateway", e.field.c_str());
}

static void test_the_network_and_broadcast_addresses_are_refused() {
    ConfigError e;
    auto        c = staticConfig();
    c.wifi.ip     = "192.168.1.0";
    TEST_ASSERT_FALSE(validate(c, e));
    TEST_ASSERT_EQUAL_STRING("wifi.ip", e.field.c_str());

    c         = staticConfig();
    c.wifi.ip = "192.168.1.255";
    TEST_ASSERT_FALSE(validate(c, e));
    TEST_ASSERT_EQUAL_STRING("wifi.ip", e.field.c_str());
}

static void test_taking_the_gateways_own_address_is_refused() {
    auto        c = staticConfig();
    ConfigError e;
    c.wifi.ip     = c.wifi.gateway;
    TEST_ASSERT_FALSE(validate(c, e));
    TEST_ASSERT_EQUAL_STRING("wifi.ip", e.field.c_str());
}

/// THE quiet one. With no DNS the stack resolves nothing: the bridge boots, answers on its
/// address, looks healthy -- and never syncs its clock, because pool.ntp.org is a name. Nothing
/// throws. This is the only moment anyone is looking.
static void test_a_static_address_with_no_dns_is_refused_while_a_name_is_configured() {
    ConfigError e;

    auto c      = staticConfig();
    c.wifi.dns1 = "";
    c.wifi.dns2 = "";
    c.ntp.enabled = true;
    c.ntp.server  = "pool.ntp.org";
    TEST_ASSERT_FALSE_MESSAGE(validate(c, e), "a named NTP server needs a resolver");
    TEST_ASSERT_EQUAL_STRING("wifi.dns1", e.field.c_str());

    // An MQTT broker named rather than numbered is the same problem.
    c            = staticConfig();
    c.wifi.dns1  = "";
    c.wifi.dns2  = "";
    c.ntp.server = "192.168.1.1";  // numbered, so NTP is fine
    c.mqtt.enabled = true;
    c.mqtt.host    = "homeassistant.local";
    TEST_ASSERT_FALSE_MESSAGE(validate(c, e), "a named broker needs a resolver");
    TEST_ASSERT_EQUAL_STRING("wifi.dns1", e.field.c_str());
}

/// ...and the complement: everything numbered needs no resolver, so demanding one would be a
/// rule that refuses a perfectly workable network.
static void test_no_dns_is_fine_when_nothing_is_configured_by_name() {
    auto c         = staticConfig();
    c.wifi.dns1    = "";
    c.wifi.dns2    = "";
    c.ntp.enabled  = true;
    c.ntp.server   = "192.168.1.1";
    c.mqtt.enabled = true;
    c.mqtt.host    = "192.168.1.10";
    ConfigError e;
    TEST_ASSERT_TRUE_MESSAGE(validate(c, e), e.field.c_str());

    // A disabled output does not count either -- its host is never resolved.
    c.mqtt.enabled = false;
    c.mqtt.host    = "homeassistant.local";
    TEST_ASSERT_TRUE_MESSAGE(validate(c, e), e.field.c_str());
}

/// The other quiet one, found reviewing the first. ntp.use_dhcp means "take the server from the
/// DHCP lease" -- and a static address has no lease. Index 0 is never filled, and with an empty
/// ntp.server index 1 is never set either, so NTP runs with no server at all: the clock never
/// syncs and every log line stays stamped from uptime. The DHCP path may leave the server empty
/// because the lease covers it; that reasoning does not survive a static address.
static void test_a_static_address_needs_an_ntp_server_even_with_use_dhcp_on() {
    auto c        = staticConfig();
    c.ntp.enabled = true;
    c.ntp.useDhcp = true;
    c.ntp.server.clear();
    ConfigError e;
    TEST_ASSERT_FALSE(validate(c, e));
    TEST_ASSERT_EQUAL_STRING("ntp.server", e.field.c_str());

    // Naming one is enough -- use_dhcp then merely means "prefer the lease if there ever is one".
    c.ntp.server = "192.168.1.1";
    TEST_ASSERT_TRUE_MESSAGE(validate(c, e), e.field.c_str());

    // And on DHCP the empty server stays legal, because the lease really does supply it.
    auto d        = provisionedConfig();
    d.ntp.enabled = true;
    d.ntp.useDhcp = true;
    d.ntp.server.clear();
    TEST_ASSERT_TRUE_MESSAGE(validate(d, e), e.field.c_str());
}

static void test_static_addressing_round_trips_through_storage_and_patch() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    TEST_ASSERT_TRUE(store.save(staticConfig()));

    Configuration loaded;
    TEST_ASSERT_EQUAL(LoadResult::Ok, store.load(loaded));
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", loaded.wifi.ip.c_str());
    TEST_ASSERT_EQUAL_STRING("255.255.255.0", loaded.wifi.subnet.c_str());
    TEST_ASSERT_TRUE(loaded.wifi.staticIp());

    // Back to DHCP: clearing every field is a legal patch, and must leave nothing behind.
    ConfigError e;
    TEST_ASSERT_TRUE(applyConfigPatch(
        R"({"wifi":{"ip":"","gateway":"","subnet":"","dns1":"","dns2":""}})", loaded, e));
    TEST_ASSERT_FALSE(loaded.wifi.staticIp());
    TEST_ASSERT_TRUE(validate(loaded, e));
    TEST_ASSERT_TRUE(loaded.wifi.gateway.empty());
}

/// The network is read once, between WiFi.mode() and WiFi.begin(). Changing it live would tear
/// down every listening socket underneath the request that asked for the change.
static void test_changing_the_address_requires_a_restart() {
    const auto before = staticConfig();
    auto       after  = before;
    after.wifi.ip     = "192.168.1.51";
    TEST_ASSERT_TRUE(configChangeRequiresReboot(before, after));

    after         = before;
    after.wifi.dns2 = "9.9.9.9";
    TEST_ASSERT_TRUE(configChangeRequiresReboot(before, after));

    after            = before;
    after.bridgeName = "Something else";
    TEST_ASSERT_FALSE(configChangeRequiresReboot(before, after));
}

static void test_overlong_strings_are_refused_at_the_boundary() {
    // A 400 naming the field, not an opaque 500 at save time.
    Configuration c;
    ConfigError   e;
    c.bridgeName = std::string(65, 'x');
    TEST_ASSERT_FALSE(validate(c, e));
    TEST_ASSERT_EQUAL_STRING("bridge_name", e.field.c_str());

    c = Configuration{};
    c.wifi.ssid = std::string(33, 'x');  // 802.11 caps an SSID at 32
    TEST_ASSERT_FALSE(validate(c, e));
    TEST_ASSERT_EQUAL_STRING("wifi.ssid", e.field.c_str());

    c = Configuration{};
    c.wifi.password = std::string(65, 'x');  // WPA2 PSK max
    TEST_ASSERT_FALSE(validate(c, e));
    TEST_ASSERT_EQUAL_STRING("wifi.password", e.field.c_str());

    c = Configuration{};
    c.driver.options["layout"] = std::string(129, 'x');
    TEST_ASSERT_FALSE(validate(c, e));
}

// --- provisioning state machine ---------------------------------------------------------------

static void test_no_credentials_means_portal() {
    const ProvisioningPolicy p;
    TEST_ASSERT_EQUAL(ProvisioningState::NeedsProvisioning,
                      decideState(p, /*hasCredentials=*/false, /*connected=*/false, 0));
}

static void test_a_few_failures_keep_trying_rather_than_open_a_portal() {
    // A router rebooting, or the bridge waking before the AP does, must not drop the device
    // off the network into a portal nobody is watching.
    const ProvisioningPolicy p;
    for (uint32_t i = 0; i < p.failuresBeforePortal; ++i) {
        TEST_ASSERT_EQUAL(ProvisioningState::Connecting, decideState(p, true, false, i));
    }
}

static void test_enough_failures_open_the_portal() {
    // With no reset button on this board, this is the only way out of a wrong password
    // short of reflashing over USB.
    const ProvisioningPolicy p;
    TEST_ASSERT_EQUAL(ProvisioningState::PortalAfterFailures,
                      decideState(p, true, false, p.failuresBeforePortal));
    TEST_ASSERT_EQUAL(ProvisioningState::PortalAfterFailures,
                      decideState(p, true, false, 1000));
}

static void test_connecting_always_beats_the_failure_history() {
    // A router that comes back must pull the device out of the portal by itself.
    const ProvisioningPolicy p;
    TEST_ASSERT_EQUAL(ProvisioningState::Connected, decideState(p, true, true, 9999));
}

static void test_the_portal_threshold_is_configurable() {
    ProvisioningPolicy p;
    p.failuresBeforePortal = 2;
    TEST_ASSERT_EQUAL(ProvisioningState::Connecting, decideState(p, true, false, 1));
    TEST_ASSERT_EQUAL(ProvisioningState::PortalAfterFailures, decideState(p, true, false, 2));
}

static void test_retry_backoff_is_bounded() {
    const ProvisioningPolicy p;
    TEST_ASSERT_EQUAL_UINT32(p.initialRetryMs, retryDelayMs(p, 1));
    TEST_ASSERT_EQUAL_UINT32(4000, retryDelayMs(p, 2));
    TEST_ASSERT_EQUAL_UINT32(8000, retryDelayMs(p, 3));
    // Capped: the AP may return at any moment and must be noticed within a minute.
    TEST_ASSERT_EQUAL_UINT32(p.maxRetryMs, retryDelayMs(p, 50));
    TEST_ASSERT_EQUAL_UINT32(p.maxRetryMs, retryDelayMs(p, 100000));
}

static void test_setup_ssid_is_stable_and_distinguishable() {
    const MacAddress a{0x24, 0x6F, 0x28, 0x11, 0xA1, 0xB2};
    const MacAddress b{0x24, 0x6F, 0x28, 0x11, 0xC3, 0xD4};
    TEST_ASSERT_EQUAL_STRING("Heliograph-Setup-A1B2", setupApSsid(a).c_str());
    // Two bridges being provisioned in one room must not present the same SSID.
    TEST_ASSERT_EQUAL_STRING("Heliograph-Setup-C3D4", setupApSsid(b).c_str());
}

// --- OTA ------------------------------------------------------------------------------------

static void test_firmware_magic_is_recognised() {
    const uint8_t good[] = {0xE9, 0x06, 0x02, 0x20};
    TEST_ASSERT_TRUE(looksLikeFirmware(good, sizeof(good)));
}

static void test_non_firmware_is_rejected() {
    // The realistic mistakes: a filesystem image, a zip, or an HTML error page a proxy
    // substituted for the download.
    const uint8_t html[] = {'<', '!', 'D', 'O'};
    const uint8_t zip[]  = {'P', 'K', 0x03, 0x04};
    TEST_ASSERT_FALSE(looksLikeFirmware(html, sizeof(html)));
    TEST_ASSERT_FALSE(looksLikeFirmware(zip, sizeof(zip)));
    TEST_ASSERT_FALSE(looksLikeFirmware(nullptr, 0));
    TEST_ASSERT_FALSE(looksLikeFirmware(html, 0));
}

// The whole point of the override: it has to survive the restart. Discovery runs, reports the
// profile the device answered at, the wizard saves it -- and then the bridge reboots. If NVS
// does not carry it, setup() configures the driver's own first profile and the inverter that
// was just positively identified goes quiet, with the wizard's own screenshot showing the
// right numbers.
static void test_a_serial_override_survives_a_restart() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    auto               c = provisionedConfig();
    c.serial.enabled                  = true;
    c.serial.profile.baudRate         = 4800;
    c.serial.profile.parity           = SerialParity::Even;
    c.serial.profile.stopBits         = 2;
    TEST_ASSERT_TRUE(store.save(c));

    Configuration loaded;
    TEST_ASSERT_EQUAL(LoadResult::Ok, store.load(loaded));
    TEST_ASSERT_TRUE(loaded.serial.enabled);
    TEST_ASSERT_EQUAL_UINT32(4800, loaded.serial.profile.baudRate);
    TEST_ASSERT_EQUAL(SerialParity::Even, loaded.serial.profile.parity);
    TEST_ASSERT_EQUAL_UINT8(2, loaded.serial.profile.stopBits);
}

// A blob written by a firmware from before this field existed -- written raw, because saving
// through the current code would emit the section and prove nothing. It must load as "the
// driver decides", which is exactly what those bridges are already doing; anything else would
// change the line under a working install on a firmware update.
static void test_a_config_without_a_serial_section_keeps_the_driver_in_charge() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    backend.write(kStorageKeyConfig,
                  R"({"version":1,"bridge_name":"Zolder","wifi":{"ssid":"thuisnetwerk"},)"
                  R"("driver":{"id":"eversolar_legacy"}})");

    Configuration loaded;
    TEST_ASSERT_EQUAL(LoadResult::Migrated, store.load(loaded));
    TEST_ASSERT_FALSE(loaded.serial.enabled);
    // ...and the defaults underneath are the driver-neutral ones, not zeroes that would
    // configure an impossible line if the flag were ever flipped by hand.
    TEST_ASSERT_EQUAL_UINT32(9600, loaded.serial.profile.baudRate);
    TEST_ASSERT_EQUAL_UINT8(8, loaded.serial.profile.dataBits);
}

// Three inverters on one bus is a configuration, so it has to survive the reboot that makes it
// take effect at all.
static void test_extra_devices_survive_a_restart() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    auto               c = provisionedConfig();
    c.additionalDevices.push_back(test::configuredDevice("modbus_profile", {{"unit_id", "2"}}));
    c.additionalDevices.push_back(test::configuredDevice("modbus_profile", {{"unit_id", "3"}}));
    TEST_ASSERT_TRUE(store.save(c));

    Configuration loaded;
    TEST_ASSERT_EQUAL(LoadResult::Ok, store.load(loaded));
    TEST_ASSERT_EQUAL_UINT32(2, loaded.additionalDevices.size());
    TEST_ASSERT_EQUAL_STRING("modbus_profile", loaded.additionalDevices[0].id.c_str());
    TEST_ASSERT_EQUAL_STRING("2", loaded.additionalDevices[0].options["unit_id"].c_str());
    TEST_ASSERT_EQUAL_STRING("3", loaded.additionalDevices[1].options["unit_id"].c_str());
}

// A blob written before this field existed -- raw, because saving through the current code
// would emit the section and prove nothing. Must load as one device, which is what those
// bridges are already doing.
static void test_a_config_without_the_device_list_stays_single_device() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    backend.write(kStorageKeyConfig,
                  R"({"version":1,"bridge_name":"Zolder","wifi":{"ssid":"thuisnetwerk"},)"
                  R"("driver":{"id":"eversolar_legacy"}})");

    Configuration loaded;
    TEST_ASSERT_EQUAL(LoadResult::Migrated, store.load(loaded));
    TEST_ASSERT_TRUE(loaded.additionalDevices.empty());
    TEST_ASSERT_EQUAL_STRING("eversolar_legacy", loaded.driver.id.c_str());
}

// A stored entry with no driver id is dropped rather than loaded: validate() would refuse the
// whole configuration for it, and refusing to boot over one corrupted list entry is worse than
// polling one inverter fewer.
static void test_a_nameless_stored_device_is_dropped_not_fatal() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    backend.write(kStorageKeyConfig,
                  R"({"version":1,"wifi":{"ssid":"thuisnetwerk"},)"
                  R"("driver":{"id":"eversolar_legacy"},)"
                  R"("additional_devices":[{"options":{"unit_id":"2"}},{"driver_id":"sunspec"}]})");

    Configuration loaded;
    TEST_ASSERT_EQUAL(LoadResult::Migrated, store.load(loaded));
    TEST_ASSERT_EQUAL_UINT32(1, loaded.additionalDevices.size());
    TEST_ASSERT_EQUAL_STRING("sunspec", loaded.additionalDevices[0].id.c_str());
    ConfigError e;
    TEST_ASSERT_TRUE(validate(loaded, e));  // and what survived is saveable
}

// Config version 2: the table-driven Modbus driver's id stopped naming one vendor.
//
// Without this migration a bridge that had been polling inverters for months would come up with
// a stored driver id matching no compiled-in driver -- no inverter at all, and nothing on the
// dashboard to say why. Both places an id is stored have to move, which is the half of this that
// is easy to miss: migrating only the primary driver leaves a three-inverter bus reporting one
// inverter, and a dashboard that still shows data looks like a working bridge.
static void test_the_renamed_profile_driver_id_is_migrated_everywhere() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    backend.write(kStorageKeyConfig,
                  R"({"version":1,"wifi":{"ssid":"thuisnetwerk"},)"
                  R"("driver":{"id":"growatt_modbus","options":{"profile":"mic_tl_x"}},)"
                  R"("additional_devices":[{"driver_id":"growatt_modbus","options":{"unit_id":"2"}},)"
                  R"({"driver_id":"eversolar_legacy","options":{}}]})");

    Configuration loaded;
    TEST_ASSERT_EQUAL(LoadResult::Migrated, store.load(loaded));
    TEST_ASSERT_EQUAL_STRING("modbus_profile", loaded.driver.id.c_str());
    TEST_ASSERT_EQUAL_UINT32(2, loaded.additionalDevices.size());
    TEST_ASSERT_EQUAL_STRING("modbus_profile", loaded.additionalDevices[0].id.c_str());
    // Untouched: the rename applies to exactly one id, and a migration that rewrote every
    // driver id would be a far worse bug than the one it fixes.
    TEST_ASSERT_EQUAL_STRING("eversolar_legacy", loaded.additionalDevices[1].id.c_str());
    // The profile selection rides along. It is the register map, so losing it would silently
    // fall back to the default profile -- another vendor's map, reporting plausible wrong values.
    TEST_ASSERT_EQUAL_STRING("mic_tl_x", loaded.driver.options.at("profile").c_str());
    TEST_ASSERT_EQUAL_UINT16(kConfigVersion, loaded.version);
}

// Which device ids were announced to the broker is the one fact nothing else survives a reboot
// knowing -- and without it a removed device's retained Home Assistant entities can never be
// cleared. Kept out of Configuration on purpose: it is not a user setting, must not appear in
// GET /config, and must not take part in the reboot-required diff.
/// The prefixes are recorded WITH the ids, because every topic MqttOutput builds comes from the
/// current configuration: once base_topic or discovery_prefix changes there is nothing left that
/// knows where the previous tree was. Recording it after the fact is impossible, which is why
/// this lands before anything that uses it.
static void test_the_announcement_records_the_tree_it_went_to() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);

    mqtt::AnnouncementRecord record;
    record.baseTopic       = "solarbridge";
    record.discoveryPrefix = "homeassistant";
    record.devices         = {{"eversolar_legacy-16", true}, {"modbus_profile-2", false}};
    TEST_ASSERT_TRUE(store.setAnnouncement(record));

    ConfigurationStore reloaded(backend);
    const auto         back = reloaded.announcement();
    TEST_ASSERT_TRUE(back.prefixesKnown());
    TEST_ASSERT_EQUAL_STRING("solarbridge", back.baseTopic.c_str());
    TEST_ASSERT_EQUAL_STRING("homeassistant", back.discoveryPrefix.c_str());
    TEST_ASSERT_EQUAL_UINT32(2, back.devices.size());
    TEST_ASSERT_TRUE(back.devices[0].primary);
    TEST_ASSERT_FALSE(back.devices[1].primary);
    TEST_ASSERT_EQUAL_STRING("modbus_profile-2", back.devices[1].id.c_str());
}

/// Both older shapes still read. Their ids survive; their prefixes are UNKNOWN rather than
/// assumed to be the defaults -- a record that never said where its topics went must not be
/// read as saying they were somewhere in particular, or a later cleanup would aim at the tree
/// that is live right now.
static void test_older_announcement_records_still_read_with_unknown_prefixes() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);

    // Oldest: bare strings, before the primary flag existed.
    backend.write(kStorageKeyAnnounced, R"(["eversolar_legacy-16","modbus_profile-2"])");
    auto bare = store.announcement();
    TEST_ASSERT_FALSE(bare.prefixesKnown());
    TEST_ASSERT_EQUAL_UINT32(2, bare.devices.size());
    TEST_ASSERT_FALSE_MESSAGE(bare.devices[0].primary, "a bare string always meant non-primary");

    // Second: objects with the flag, still no tree.
    backend.write(kStorageKeyAnnounced,
                  R"([{"id":"eversolar_legacy-16","primary":true}])");
    auto flagged = store.announcement();
    TEST_ASSERT_FALSE(flagged.prefixesKnown());
    TEST_ASSERT_EQUAL_UINT32(1, flagged.devices.size());
    TEST_ASSERT_TRUE(flagged.devices[0].primary);

    // Unreadable bookkeeping is forgotten, never fatal.
    backend.write(kStorageKeyAnnounced, "{not json");
    TEST_ASSERT_EQUAL_UINT32(0, store.announcement().devices.size());
}

static void test_announced_devices_round_trip_and_start_empty() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    TEST_ASSERT_TRUE(store.announcement().devices.empty());

    mqtt::AnnouncementRecord announced;
    announced.baseTopic       = "heliograph";
    announced.discoveryPrefix = "homeassistant";
    announced.devices         = {{"modbus_profile-1", true}, {"modbus_profile-2", false}};
    TEST_ASSERT_TRUE(store.setAnnouncement(announced));
    const auto back = store.announcement().devices;
    TEST_ASSERT_EQUAL_UINT32(2, back.size());
    TEST_ASSERT_EQUAL_STRING("modbus_profile-2", back[1].id.c_str());
    // Which tree a device was announced on is half the fact: without it, a device promoted into
    // the `driver` slot keeps its id, is never seen as removed, and leaves its whole per-device
    // entity set behind forever.
    TEST_ASSERT_TRUE(back[0].primary);
    TEST_ASSERT_FALSE(back[1].primary);

    // Removing the last device must be storable as such, not indistinguishable from "never set".
    TEST_ASSERT_TRUE(store.setAnnouncement({}));
    TEST_ASSERT_TRUE(store.announcement().devices.empty());
}

// The key first shipped as a plain array of ids. Reading one back as non-primary is not a
// courtesy: it is what those entries meant, so a bridge flashed with a build from between the
// two shapes still clears its per-device topics instead of starting from nothing.
static void test_announced_bookkeeping_reads_the_flat_id_list() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    backend.write(kStorageKeyAnnounced, R"(["eversolar-1","modbus_profile-2"])");

    const auto back = store.announcement().devices;
    TEST_ASSERT_EQUAL_UINT32(2, back.size());
    TEST_ASSERT_EQUAL_STRING("eversolar-1", back[0].id.c_str());
    TEST_ASSERT_FALSE(back[0].primary);
    TEST_ASSERT_FALSE(back[1].primary);
}

// Unreadable bookkeeping is forgotten, never fatal: it lives in its own key precisely so it
// cannot take a working configuration down with it.
static void test_corrupt_announced_bookkeeping_is_not_fatal() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    backend.write(kStorageKeyAnnounced, "{not json");
    TEST_ASSERT_TRUE(store.announcement().devices.empty());

    auto c = provisionedConfig();
    TEST_ASSERT_TRUE(store.save(c));
    Configuration loaded;
    TEST_ASSERT_EQUAL(LoadResult::Ok, store.load(loaded));
}

// A factory reset erases the whole namespace, so the bookkeeping goes with it -- a bridge that
// has been wiped must not then try to clear topics for devices from a previous life.
static void test_a_factory_reset_forgets_what_was_announced() {
    MemoryBackend      backend;
    ConfigurationStore store(backend);
    store.setAnnouncement({"heliograph", "homeassistant", {{"modbus_profile-1", true}}});
    TEST_ASSERT_TRUE(store.factoryReset());
    TEST_ASSERT_TRUE(store.announcement().devices.empty());
}

static void test_ota_rejects_a_non_firmware_upload_before_writing() {
    ota::OtaManager ota;
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.begin(1024));

    const uint8_t html[] = {'<', '!', 'D', 'O', 'C'};
    TEST_ASSERT_EQUAL(ota::OtaResult::NotFirmware, ota.write(html, sizeof(html)));
    // Aborted, and nothing counted as written.
    TEST_ASSERT_FALSE(ota.running());
    TEST_ASSERT_EQUAL_size_t(0, ota.written());
    TEST_ASSERT_TRUE(ota.lastError().find("0xE9") != std::string::npos);
}

static void test_ota_accepts_a_firmware_upload() {
    ota::OtaManager ota;
    const uint8_t   image[] = {0xE9, 0x01, 0x02, 0x03, 0x04, 0x05};
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.begin(sizeof(image)));
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.write(image, sizeof(image)));
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.end());
    TEST_ASSERT_EQUAL_size_t(sizeof(image), ota.written());
    TEST_ASSERT_FALSE(ota.running());
}

static void test_a_matching_hash_is_accepted() {
    ota::OtaManager ota;
    const uint8_t   image[] = {0xE9, 0x01, 0x02, 0x03, 0x04, 0x05};
    // Computed here rather than pasted, then fed back as the expectation: what this asserts is
    // that the accept path works end to end, with the wrong-hash case below carrying the real
    // weight.
    ota::Sha256 h;
    h.update(image, sizeof(image));
    const std::string digest = h.finishHex();

    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.begin(sizeof(image), digest));
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.write(image, sizeof(image)));
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.end());
    TEST_ASSERT_EQUAL_STRING(digest.c_str(), ota.writtenSha256().c_str());
}

/// The case the whole feature turns on: every byte arrived, the flash was happy, and the image
/// is not the one that was promised. It must not reach end().
static void test_a_wrong_hash_is_refused() {
    ota::OtaManager ota;
    const uint8_t   image[] = {0xE9, 0x01, 0x02, 0x03, 0x04, 0x05};
    const std::string wrong(64, 'a');

    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.begin(sizeof(image), wrong));
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.write(image, sizeof(image)));
    TEST_ASSERT_EQUAL(ota::OtaResult::HashMismatch, ota.end());
    TEST_ASSERT_FALSE(ota.running());
    // Both digests named, so a corrupted download can be told from the wrong file entirely.
    TEST_ASSERT_NOT_NULL(std::strstr(ota.lastError().c_str(), ota.writtenSha256().c_str()));
    TEST_ASSERT_NOT_NULL(std::strstr(ota.lastError().c_str(), wrong.c_str()));
}

/// One flipped bit in the middle of a multi-chunk upload -- what a mangled LAN transfer looks
/// like, as opposed to a wholesale substitution.
static void test_a_single_corrupted_chunk_is_caught() {
    const uint8_t good[] = {0xE9, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    ota::Sha256   h;
    h.update(good, sizeof(good));
    const std::string expected = h.finishHex();

    uint8_t corrupted[sizeof(good)];
    std::memcpy(corrupted, good, sizeof(good));
    corrupted[4] ^= 0x01;

    ota::OtaManager ota;
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.begin(sizeof(corrupted), expected));
    // Chunked, because that is how a body arrives and a hash that only worked on one big
    // buffer would pass every test and fail on a real upload.
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.write(corrupted, 3));
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.write(corrupted + 3, 5));
    TEST_ASSERT_EQUAL(ota::OtaResult::HashMismatch, ota.end());
}

/// A hand-picked file from the settings page has nothing to compare against, and must keep
/// working exactly as before.
static void test_no_expected_hash_means_no_check() {
    ota::OtaManager ota;
    const uint8_t   image[] = {0xE9, 0x01, 0x02, 0x03};
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.begin(sizeof(image)));
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.write(image, sizeof(image)));
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.end());
    // Still reported, so the settings page can show what it just flashed.
    TEST_ASSERT_EQUAL_size_t(64, ota.writtenSha256().size());
}

/// A malformed expectation is a refusal, not a comparison that happens to fail. An empty-ish
/// or truncated hash field must never mean "close enough".
static void test_a_malformed_expected_hash_refuses_the_image() {
    ota::OtaManager ota;
    const uint8_t   image[] = {0xE9, 0x01, 0x02, 0x03};
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.begin(sizeof(image), "not-a-digest"));
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.write(image, sizeof(image)));
    TEST_ASSERT_EQUAL(ota::OtaResult::HashMismatch, ota.end());
}

/// begin() has to clear the previous run's state. Without the reset a second upload would hash
/// the first one's bytes as well and fail for a reason nothing could explain.
static void test_a_second_upload_starts_from_a_clean_hash() {
    ota::OtaManager ota;
    const uint8_t   first[]  = {0xE9, 0xAA, 0xBB};
    const uint8_t   second[] = {0xE9, 0x01, 0x02, 0x03};

    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.begin(sizeof(first)));
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.write(first, sizeof(first)));
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.end());

    ota::Sha256 h;
    h.update(second, sizeof(second));
    const std::string expected = h.finishHex();
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.begin(sizeof(second), expected));
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.write(second, sizeof(second)));
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.end());
}

static void test_ota_refuses_a_second_concurrent_upload() {
    ota::OtaManager ota;
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.begin(1024));
    TEST_ASSERT_EQUAL(ota::OtaResult::AlreadyRunning, ota.begin(1024));
}

static void test_ota_refuses_more_data_than_announced() {
    ota::OtaManager ota;
    const uint8_t   image[] = {0xE9, 0x01, 0x02, 0x03};
    TEST_ASSERT_EQUAL(ota::OtaResult::Ok, ota.begin(2));
    TEST_ASSERT_EQUAL(ota::OtaResult::TooLarge, ota.write(image, sizeof(image)));
    TEST_ASSERT_FALSE(ota.running());
}

static void test_ota_write_without_begin_is_refused() {
    ota::OtaManager ota;
    const uint8_t   image[] = {0xE9, 0x01};
    TEST_ASSERT_EQUAL(ota::OtaResult::NotFinished, ota.write(image, sizeof(image)));
    TEST_ASSERT_EQUAL(ota::OtaResult::NotFinished, ota.end());
}

static void test_ota_end_without_any_data_is_refused() {
    // An empty POST must not mark an empty partition bootable.
    ota::OtaManager ota;
    ota.begin(1024);
    TEST_ASSERT_EQUAL(ota::OtaResult::NotFirmware, ota.end());
}

static void test_boot_is_confirmed_only_when_networked_and_settled() {
    using ota::kHealthyBootThresholdMs;
    using ota::shouldConfirmHealthyBoot;
    // Healthy: networked and past the threshold, not yet confirmed.
    TEST_ASSERT_TRUE(shouldConfirmHealthyBoot(true, kHealthyBootThresholdMs, false, false));
    TEST_ASSERT_TRUE(shouldConfirmHealthyBoot(true, kHealthyBootThresholdMs + 5000, false, false));
    // Already confirmed -> never again (latched by the caller).
    TEST_ASSERT_FALSE(shouldConfirmHealthyBoot(true, kHealthyBootThresholdMs + 5000, true, true));
    // No network -> unrecoverable image, allow rollback (do not confirm).
    TEST_ASSERT_FALSE(shouldConfirmHealthyBoot(false, kHealthyBootThresholdMs + 5000, false, false));
    // Too soon after boot -> not settled yet.
    TEST_ASSERT_FALSE(shouldConfirmHealthyBoot(true, kHealthyBootThresholdMs - 1, false, false));
    // Late WiFi (slow router) is fine: confirmation just waits until it connects.
    TEST_ASSERT_FALSE(shouldConfirmHealthyBoot(false, kHealthyBootThresholdMs + 120000, false, false));
    TEST_ASSERT_TRUE(shouldConfirmHealthyBoot(true, kHealthyBootThresholdMs + 120000, false, false));
}

// The bounded offline fallback (review 2026-07-21): a healthy image behind a long router
// outage must not be silently rolled back by the next power blip. It confirms without WiFi
// once it has BOTH been up for the long threshold AND proven it does its actual job (a
// successful inverter poll). Either alone is not enough.
static void test_a_working_bridge_confirms_eventually_even_without_wifi() {
    using ota::kHealthyBootThresholdMs;
    using ota::kOfflineConfirmThresholdMs;
    using ota::shouldConfirmHealthyBoot;
    // Long-up AND polling successfully, no WiFi -> confirm (the fallback).
    TEST_ASSERT_TRUE(shouldConfirmHealthyBoot(false, kOfflineConfirmThresholdMs, false, true));
    // Long-up but never polled anything: superficially alive is not healthy -> roll back.
    TEST_ASSERT_FALSE(shouldConfirmHealthyBoot(false, kOfflineConfirmThresholdMs, false, false));
    // Polling fine but not up long enough: the fast path stays WiFi-gated.
    TEST_ASSERT_FALSE(
        shouldConfirmHealthyBoot(false, kOfflineConfirmThresholdMs - 1, false, true));
    // The fallback threshold is deliberately much longer than the fast path.
    TEST_ASSERT_TRUE(kOfflineConfirmThresholdMs >= 10 * kHealthyBootThresholdMs);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_boot_is_confirmed_only_when_networked_and_settled);
    RUN_TEST(test_a_working_bridge_confirms_eventually_even_without_wifi);
    RUN_TEST(test_first_boot_finds_nothing_and_keeps_defaults);
    RUN_TEST(test_save_then_load_round_trips_everything);
    RUN_TEST(test_relays_enabled_defaults_off_and_round_trips);
    RUN_TEST(test_relay_roles_validate_and_round_trip);
    RUN_TEST(test_config_under_the_legacy_namespace_is_adopted);
    RUN_TEST(test_primary_config_wins_over_legacy);
    RUN_TEST(test_ntp_settings_round_trip_through_storage);
    RUN_TEST(test_ntp_has_defaults_that_work_out_of_the_box);
    RUN_TEST(test_ntp_patch_applies_from_the_settings_form);
    RUN_TEST(test_ntp_without_dhcp_needs_a_server);
    RUN_TEST(test_ntp_timezone_must_not_be_empty);
    RUN_TEST(test_secrets_survive_the_round_trip);
    RUN_TEST(test_the_two_config_documents_differ_only_in_their_credentials);
    RUN_TEST(test_the_storage_serialiser_is_the_only_one_that_writes_secrets);
    RUN_TEST(test_factory_reset_wipes_credentials);
    RUN_TEST(test_a_failed_write_is_reported_not_swallowed);
    RUN_TEST(test_an_invalid_config_is_never_persisted);
    RUN_TEST(test_corrupt_blob_falls_back_to_defaults);
    RUN_TEST(test_blob_without_a_version_is_corrupt);
    RUN_TEST(test_a_newer_version_is_refused_not_guessed_at);
    RUN_TEST(test_a_newer_version_still_keeps_the_network_identity);
    RUN_TEST(test_a_stored_config_that_no_longer_validates_is_corrupt);
    RUN_TEST(test_missing_fields_fall_back_to_defaults);
    RUN_TEST(test_the_stored_blob_fits_nvs_comfortably);
    RUN_TEST(test_everything_the_writer_emits_the_reader_takes_back);
    RUN_TEST(test_a_sound_static_configuration_is_accepted);
    RUN_TEST(test_an_empty_ip_means_dhcp_and_needs_nothing_else);
    RUN_TEST(test_leftover_fields_without_an_ip_are_refused);
    RUN_TEST(test_the_address_fields_must_parse);
    RUN_TEST(test_a_mask_the_arduino_core_would_reinterpret_is_refused);
    RUN_TEST(test_an_unreachable_gateway_is_refused);
    RUN_TEST(test_the_network_and_broadcast_addresses_are_refused);
    RUN_TEST(test_taking_the_gateways_own_address_is_refused);
    RUN_TEST(test_a_static_address_with_no_dns_is_refused_while_a_name_is_configured);
    RUN_TEST(test_no_dns_is_fine_when_nothing_is_configured_by_name);
    RUN_TEST(test_a_static_address_needs_an_ntp_server_even_with_use_dhcp_on);
    RUN_TEST(test_static_addressing_round_trips_through_storage_and_patch);
    RUN_TEST(test_changing_the_address_requires_a_restart);
    RUN_TEST(test_overlong_strings_are_refused_at_the_boundary);
    RUN_TEST(test_no_credentials_means_portal);
    RUN_TEST(test_a_few_failures_keep_trying_rather_than_open_a_portal);
    RUN_TEST(test_enough_failures_open_the_portal);
    RUN_TEST(test_connecting_always_beats_the_failure_history);
    RUN_TEST(test_the_portal_threshold_is_configurable);
    RUN_TEST(test_retry_backoff_is_bounded);
    RUN_TEST(test_setup_ssid_is_stable_and_distinguishable);
    RUN_TEST(test_firmware_magic_is_recognised);
    RUN_TEST(test_non_firmware_is_rejected);
    RUN_TEST(test_a_serial_override_survives_a_restart);
    RUN_TEST(test_a_config_without_a_serial_section_keeps_the_driver_in_charge);
    RUN_TEST(test_extra_devices_survive_a_restart);
    RUN_TEST(test_a_config_without_the_device_list_stays_single_device);
    RUN_TEST(test_a_nameless_stored_device_is_dropped_not_fatal);
    RUN_TEST(test_the_renamed_profile_driver_id_is_migrated_everywhere);
    RUN_TEST(test_the_announcement_records_the_tree_it_went_to);
    RUN_TEST(test_older_announcement_records_still_read_with_unknown_prefixes);
    RUN_TEST(test_announced_devices_round_trip_and_start_empty);
    RUN_TEST(test_announced_bookkeeping_reads_the_flat_id_list);
    RUN_TEST(test_corrupt_announced_bookkeeping_is_not_fatal);
    RUN_TEST(test_a_factory_reset_forgets_what_was_announced);
    RUN_TEST(test_ota_rejects_a_non_firmware_upload_before_writing);
    RUN_TEST(test_ota_accepts_a_firmware_upload);
    RUN_TEST(test_ota_refuses_a_second_concurrent_upload);
    RUN_TEST(test_a_matching_hash_is_accepted);
    RUN_TEST(test_a_wrong_hash_is_refused);
    RUN_TEST(test_a_single_corrupted_chunk_is_caught);
    RUN_TEST(test_no_expected_hash_means_no_check);
    RUN_TEST(test_a_malformed_expected_hash_refuses_the_image);
    RUN_TEST(test_a_second_upload_starts_from_a_clean_hash);
    RUN_TEST(test_turning_modbus_off_leaves_mqtt_running_after_a_restart);
    RUN_TEST(test_ota_refuses_more_data_than_announced);
    RUN_TEST(test_ota_write_without_begin_is_refused);
    RUN_TEST(test_ota_end_without_any_data_is_refused);
    RUN_TEST(test_the_update_check_defaults_on_and_round_trips);
    RUN_TEST(test_a_config_without_the_setting_keeps_the_default);
    RUN_TEST(test_nothing_to_roll_back_to_on_a_fresh_board);
    RUN_TEST(test_stash_then_rollback_returns_the_earlier_configuration);
    RUN_TEST(test_rolling_back_twice_returns_to_the_restored_configuration);
    RUN_TEST(test_a_rollback_slot_that_does_not_fit_is_reported_not_fatal);
    RUN_TEST(test_an_unreadable_rollback_leaves_the_live_config_untouched);
    RUN_TEST(test_factory_reset_takes_the_rollback_slot_with_it);
    return UNITY_END();
}
