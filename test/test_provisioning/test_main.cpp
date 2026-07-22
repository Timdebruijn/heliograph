// SPDX-License-Identifier: MIT
// Configuration persistence, migration, provisioning state machine and OTA validation.

#include <unity.h>

#include <string>

#include "config/configuration_store.h"
#include "network/provisioning_policy.h"
#include "ota/ota_manager.h"

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

/// Exactly the body the settings form sends when only the Modbus checkbox is cleared: every
/// section present, passwords omitted (blank field means "keep").
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
    TEST_ASSERT_EQUAL(LoadResult::Ok, store.load(c));
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

    ConfigError e;
    TEST_ASSERT_TRUE_MESSAGE(validate(c, e), "a maximal config must still be valid");
    std::string blob;
    TEST_ASSERT_TRUE(serializeConfigForStorage(c, blob));
    TEST_ASSERT_TRUE(blob.size() < 3900);

    std::string typical;
    TEST_ASSERT_TRUE(serializeConfigForStorage(provisionedConfig(), typical));
    TEST_ASSERT_TRUE(typical.size() < 1000);
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
    const uint8_t a[6] = {0x24, 0x6F, 0x28, 0x11, 0xA1, 0xB2};
    const uint8_t b[6] = {0x24, 0x6F, 0x28, 0x11, 0xC3, 0xD4};
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
    RUN_TEST(test_config_under_the_legacy_namespace_is_adopted);
    RUN_TEST(test_primary_config_wins_over_legacy);
    RUN_TEST(test_ntp_settings_round_trip_through_storage);
    RUN_TEST(test_ntp_has_defaults_that_work_out_of_the_box);
    RUN_TEST(test_ntp_patch_applies_from_the_settings_form);
    RUN_TEST(test_ntp_without_dhcp_needs_a_server);
    RUN_TEST(test_ntp_timezone_must_not_be_empty);
    RUN_TEST(test_secrets_survive_the_round_trip);
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
    RUN_TEST(test_ota_rejects_a_non_firmware_upload_before_writing);
    RUN_TEST(test_ota_accepts_a_firmware_upload);
    RUN_TEST(test_ota_refuses_a_second_concurrent_upload);
    RUN_TEST(test_turning_modbus_off_leaves_mqtt_running_after_a_restart);
    RUN_TEST(test_ota_refuses_more_data_than_announced);
    RUN_TEST(test_ota_write_without_begin_is_refused);
    RUN_TEST(test_ota_end_without_any_data_is_refused);
    return UNITY_END();
}
