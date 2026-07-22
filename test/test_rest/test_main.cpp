// SPDX-License-Identifier: MIT
// REST payloads, configuration redaction/validation and Prometheus output.

#include <unity.h>

#include <ArduinoJson.h>

#include <string>

#include "config/configuration.h"
#include "diagnostics/logger.h"
#include "outputs/modbus_tcp/modbus_tcp_server.h"
#include "device/device_context.h"
#include "drivers/driver_registry.h"
#include "drivers/eversolar_legacy/eversolar_driver.h"
#include "drivers/mock/mock_driver.h"
#include "outputs/prometheus/prometheus_metrics.h"
#include "outputs/rest/rest_payloads.h"
#include "state/state_store.h"
#include "support/fake_eversolar_device.h"
#include "fixtures/eversolar_frames.h"
#include "support/mock_transport.h"

using namespace heliograph;
using test::FakeEversolarDevice;
using test::MockTransport;
namespace fx = heliograph::fixtures;

static uint64_t g_now = 0;
static uint64_t clockFn() { return g_now; }

void setUp() { g_now = 100000; }
void tearDown() {}

static BridgeInfo makeBridge() {
    BridgeInfo b;
    b.bridgeId        = "heliograph-a1b2c3";
    b.bridgeOnline    = true;
    b.wifiConnected   = true;
    b.wifiRssiDbm     = -57;
    b.uptimeSeconds   = 86400;
    b.freeHeapBytes   = 180000;
    b.firmwareVersion = "0.1.0";
    return b;
}

struct Rig {
    MockTransport              transport;
    FakeEversolarDevice        device;
    eversolar::EversolarDriver driver{transport};
    StateStore                 store;
    Diagnostics                diagnostics;

    Rig() {
        device.installOn(transport);
        driver.begin(transport);
    }
    DeviceState poll() {
        DeviceContext ctx(driver, store, diagnostics, clockFn);
        ctx.pollOnce();
        return *store.snapshot();
    }
};

static JsonDocument parse(const std::string& json) {
    JsonDocument doc;
    TEST_ASSERT_EQUAL_MESSAGE(DeserializationError::Ok, deserializeJson(doc, json).code(),
                              "response is not valid JSON");
    return doc;
}

/// A configuration with every secret populated, so a leak has something to leak.
static Configuration configWithSecrets() {
    Configuration c;
    c.wifi.ssid                = "thuisnetwerk";
    c.wifi.password            = "SuperSecretWifiPassword123";
    c.mqtt.enabled             = true;
    c.mqtt.host                = "10.0.0.5";
    c.mqtt.username            = "solar";
    c.mqtt.password            = "SuperSecretMqttPassword456";
    c.security.adminPassword   = "SuperSecretAdminPassword789";
    return c;
}

// --- the thing that must never go wrong -----------------------------------------------------

static void test_config_response_contains_no_secret_anywhere() {
    const auto  c = configWithSecrets();
    std::string json;
    TEST_ASSERT_TRUE(serializeConfig(c, json));

    // Not masked. Absent. Searching the raw body is the point: a structural check could miss
    // a password that leaked into some other field.
    TEST_ASSERT_TRUE(json.find("SuperSecretWifiPassword123") == std::string::npos);
    TEST_ASSERT_TRUE(json.find("SuperSecretMqttPassword456") == std::string::npos);
    TEST_ASSERT_TRUE(json.find("SuperSecretAdminPassword789") == std::string::npos);
    // And no masked placeholder either: a client could round-trip "***" back in as a literal.
    TEST_ASSERT_TRUE(json.find("***") == std::string::npos);
}

static void test_config_reports_whether_a_secret_is_set() {
    auto        c = configWithSecrets();
    std::string json;
    serializeConfig(c, json);
    auto doc = parse(json);

    TEST_ASSERT_TRUE(doc["wifi"]["password_set"].as<bool>());
    TEST_ASSERT_TRUE(doc["mqtt"]["password_set"].as<bool>());
    TEST_ASSERT_TRUE(doc["security"]["password_set"].as<bool>());
    // Non-secret fields are readable, which is what makes the UI usable.
    TEST_ASSERT_EQUAL_STRING("thuisnetwerk", doc["wifi"]["ssid"]);
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", doc["mqtt"]["host"]);
    TEST_ASSERT_EQUAL_STRING("solar", doc["mqtt"]["username"]);

    c.wifi.password.clear();
    c.mqtt.password.clear();
    c.security.adminPassword.clear();
    serializeConfig(c, json);
    doc = parse(json);
    TEST_ASSERT_FALSE(doc["wifi"]["password_set"].as<bool>());
    TEST_ASSERT_FALSE(doc["mqtt"]["password_set"].as<bool>());
    TEST_ASSERT_FALSE(doc["security"]["password_set"].as<bool>());
}

// --- config patching -------------------------------------------------------------------------

static void test_patch_leaves_absent_fields_alone() {
    auto        c = configWithSecrets();
    ConfigError e;
    TEST_ASSERT_TRUE(applyConfigPatch(R"({"polling":{"interval_seconds":30}})", c, e));

    TEST_ASSERT_EQUAL_UINT32(30, c.polling.intervalSeconds);
    TEST_ASSERT_EQUAL_STRING("SuperSecretWifiPassword123", c.wifi.password.c_str());
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", c.mqtt.host.c_str());
}

static void test_patch_sets_a_password() {
    auto        c = configWithSecrets();
    ConfigError e;
    TEST_ASSERT_TRUE(applyConfigPatch(R"({"mqtt":{"password":"nieuw"}})", c, e));
    TEST_ASSERT_EQUAL_STRING("nieuw", c.mqtt.password.c_str());
}

static void test_explicit_null_clears_a_password() {
    auto        c = configWithSecrets();
    ConfigError e;
    TEST_ASSERT_TRUE(applyConfigPatch(R"({"mqtt":{"password":null}})", c, e));
    TEST_ASSERT_TRUE(c.mqtt.password.empty());
}

static void test_a_rejected_patch_changes_nothing() {
    // The merge-then-validate rule: a bad field must not leave earlier fields applied.
    auto        c = configWithSecrets();
    ConfigError e;
    const auto  before = c.polling.intervalSeconds;
    TEST_ASSERT_FALSE(applyConfigPatch(
        R"({"polling":{"interval_seconds":30},"modbus":{"unit_id":0}})", c, e));

    TEST_ASSERT_EQUAL_UINT32(before, c.polling.intervalSeconds);  // not 30
    TEST_ASSERT_EQUAL_STRING("modbus.unit_id", e.field.c_str());
}

static void test_invalid_json_is_refused() {
    Configuration c;
    ConfigError   e;
    TEST_ASSERT_FALSE(applyConfigPatch("{not json", c, e));
    TEST_ASSERT_FALSE(applyConfigPatch("[1,2,3]", c, e));
    TEST_ASSERT_FALSE(applyConfigPatch(R"({"polling":{"interval_seconds":"tien"}})", c, e));
}

static void test_out_of_range_values_are_refused() {
    Configuration c;
    ConfigError   e;
    TEST_ASSERT_FALSE(applyConfigPatch(R"({"polling":{"interval_seconds":0}})", c, e));
    TEST_ASSERT_FALSE(applyConfigPatch(R"({"polling":{"interval_seconds":99999}})", c, e));
    TEST_ASSERT_FALSE(applyConfigPatch(R"({"mqtt":{"qos":3}})", c, e));
    TEST_ASSERT_FALSE(applyConfigPatch(R"({"modbus":{"unit_id":248}})", c, e));
    TEST_ASSERT_FALSE(applyConfigPatch(R"({"logging":{"level":"chatty"}})", c, e));
}

static void test_modbus_write_cannot_be_enabled() {
    // No writable driver exists in this build; allowing the flag would advertise a lie.
    Configuration c;
    ConfigError   e;
    TEST_ASSERT_FALSE(applyConfigPatch(R"({"modbus":{"write_enabled":true}})", c, e));
    TEST_ASSERT_EQUAL_STRING("modbus.write_enabled", e.field.c_str());
}

static void test_diagnostics_unit_id_must_differ_from_the_inverter() {
    Configuration c;
    ConfigError   e;
    TEST_ASSERT_FALSE(applyConfigPatch(R"({"modbus":{"diagnostics_unit_id":1}})", c, e));
}

static void test_driver_options_are_opaque_to_the_config_model() {
    // The config model must never gain a manufacturer-specific field. Options are a string
    // map here; what the keys mean is the driver's business.
    Configuration c;
    ConfigError   e;
    TEST_ASSERT_TRUE(applyConfigPatch(R"({"driver":{"options":{"layout":"dual"}}})", c, e));
    TEST_ASSERT_EQUAL_STRING("dual", c.driver.options["layout"].c_str());

    std::string json;
    serializeConfig(c, json);
    TEST_ASSERT_EQUAL_STRING("dual", parse(json)["driver"]["options"]["layout"]);
}

static void test_driver_options_are_validated_against_the_driver() {
    // Validation lives with the driver that declares the option, not in the config validator
    // -- which has no way to know what a "layout" is.
    DriverRegistry reg;
    registerBuiltinDrivers(reg);
    const auto* d = reg.find("eversolar_legacy");
    TEST_ASSERT_NOT_NULL(d);

    DriverOptionError e;
    TEST_ASSERT_TRUE(validateDriverOptions(*d, {{"layout", "dual"}}, e));
    TEST_ASSERT_TRUE(validateDriverOptions(*d, {{"layout", "auto"}}, e));

    TEST_ASSERT_FALSE(validateDriverOptions(*d, {{"layout", "triple"}}, e));
    TEST_ASSERT_EQUAL_STRING("layout", e.key.c_str());

    // A silently ignored setting is worse than a refused one: the user believes it applied.
    TEST_ASSERT_FALSE(validateDriverOptions(*d, {{"laoyut", "dual"}}, e));
    TEST_ASSERT_TRUE(e.message.find("unknown option") != std::string::npos);
}

static void test_an_unset_option_falls_back_to_the_declared_default() {
    DriverRegistry reg;
    registerBuiltinDrivers(reg);
    const auto* d = reg.find("eversolar_legacy");
    TEST_ASSERT_EQUAL_STRING("auto", d->optionOr({}, "layout").c_str());
    TEST_ASSERT_EQUAL_STRING("dual", d->optionOr({{"layout", "dual"}}, "layout").c_str());
}

static void test_driver_options_reach_the_driver() {
    // End to end: a config string turns into the driver's own enum.
    TEST_ASSERT_EQUAL(eversolar::LayoutSelection::Auto, eversolar::optionsFrom({}).layout);
    TEST_ASSERT_EQUAL(eversolar::LayoutSelection::ForceDualString,
                      eversolar::optionsFrom({{"layout", "dual"}}).layout);
    TEST_ASSERT_EQUAL(eversolar::LayoutSelection::ForceSingleString,
                      eversolar::optionsFrom({{"layout", "single"}}).layout);
}

static void test_driver_id_defaults_to_empty_not_to_a_manufacturer() {
    // A default naming one manufacturer is that manufacturer leaking into the config model.
    // Empty means "the application picks the highest-priority driver compiled in".
    const Configuration c;
    ConfigError         e;
    TEST_ASSERT_TRUE(c.driver.id.empty());
    TEST_ASSERT_TRUE(validate(c, e));
}

static void test_defaults_are_valid_and_read_only() {
    Configuration c;
    ConfigError   e;
    TEST_ASSERT_TRUE(validate(c, e));
    TEST_ASSERT_TRUE(c.security.readOnlyMode);
    TEST_ASSERT_FALSE(c.modbus.writeEnabled);
    TEST_ASSERT_FALSE(c.provisioned());  // no credentials baked in
    TEST_ASSERT_TRUE(c.security.adminPassword.empty());
}

static void test_hostname_must_be_a_valid_dns_label() {
    // The hostname is promised as http://<hostname>.local and sent as the DHCP client name.
    // A space or punctuation used to be accepted and then silently broke both.
    Configuration c;
    ConfigError   e;
    c.wifi.hostname = "solar-bridge-2";
    TEST_ASSERT_TRUE(validate(c, e));
    c.wifi.hostname = "Mijn Bridge!";
    TEST_ASSERT_FALSE(validate(c, e));
    TEST_ASSERT_EQUAL_STRING("wifi.hostname", e.field.c_str());
    c.wifi.hostname = "solar.bridge";  // dots make it a hierarchy, not a label
    TEST_ASSERT_FALSE(validate(c, e));
    c.wifi.hostname = "-heliograph";
    TEST_ASSERT_FALSE(validate(c, e));
    c.wifi.hostname = "heliograph-";
    TEST_ASSERT_FALSE(validate(c, e));
}

// --- logging level ---------------------------------------------------------------------------

static void test_log_level_gates_output() {
    // This existed as a config field long before it did anything: validated, persisted and
    // rendered in the web form while no code read it. A setting that silently does nothing is
    // worse than a missing one, because the user believes it applied.
    log::setLevel(LogLevel::Info);
    TEST_ASSERT_TRUE(log::enabled(LogLevel::Error));
    TEST_ASSERT_TRUE(log::enabled(LogLevel::Warn));
    TEST_ASSERT_TRUE(log::enabled(LogLevel::Info));
    TEST_ASSERT_FALSE(log::enabled(LogLevel::Debug));
    TEST_ASSERT_FALSE(log::enabled(LogLevel::Trace));

    log::setLevel(LogLevel::Error);
    TEST_ASSERT_TRUE(log::enabled(LogLevel::Error));
    TEST_ASSERT_FALSE(log::enabled(LogLevel::Warn));

    log::setLevel(LogLevel::Trace);
    TEST_ASSERT_TRUE(log::enabled(LogLevel::Trace));
    TEST_ASSERT_TRUE(log::enabled(LogLevel::Error));
    log::setLevel(LogLevel::Info);
}

static void test_raw_frames_are_trace_only() {
    // The brief is explicit: raw RS485 frames only at TRACE. Anything else and a busy bus
    // floods the console at the default level.
    log::setLevel(LogLevel::Debug);
    TEST_ASSERT_FALSE(log::enabled(LogLevel::Trace));
    log::setLevel(LogLevel::Info);
}

// --- Modbus server configuration ----------------------------------------------------------

static void test_modbus_config_is_actually_applied() {
    // It was not: main.cpp built a default server and called begin() on it, so modbus.port and
    // modbus.unit_id in the configuration did nothing at all, ever, even across a reboot.
    modbus::ModbusServerConfig cfg;
    cfg.port              = 5020;
    cfg.inverterUnitId    = 7;
    cfg.diagnosticsUnitId = 200;

    modbus::ModbusTcpServer server;
    TEST_ASSERT_TRUE(server.setConfig(cfg));
    TEST_ASSERT_EQUAL_UINT16(5020, server.config().port);
    TEST_ASSERT_EQUAL_UINT8(7, server.config().inverterUnitId);
    TEST_ASSERT_EQUAL_UINT8(200, server.config().diagnosticsUnitId);
}

static void test_modbus_write_stays_off_whatever_the_config_says() {
    modbus::ModbusServerConfig cfg;
    cfg.writeEnabled = true;  // a caller could try
    modbus::ModbusTcpServer server;
    server.setConfig(cfg);
    // The struct carries it, but validate() refuses it and main.cpp hardcodes false. The
    // guarantee lives in those two places, not in a hopeful default.
    Configuration c;
    c.modbus.writeEnabled = true;
    ConfigError e;
    TEST_ASSERT_FALSE(validate(c, e));
}

// --- REST payloads ---------------------------------------------------------------------------

// NTP feedback: before sync the API must say so honestly (null, never a 1970 date dressed
// up as real); after sync it carries the local wall-clock and the last sync moment.
static void test_status_payload_reports_clock_sync_state() {
    Rig        r;
    const auto state = r.poll();

    BridgeInfo unsynced = makeBridge();
    unsynced.timeSynced = false;
    std::string json;
    TEST_ASSERT_TRUE(rest::buildStatusPayload(state, "eversolar_legacy", unsynced,
                                              r.diagnostics.snapshot(),
                                              &eversolar::descriptor(), g_now, json));
    auto doc = parse(json);
    TEST_ASSERT_FALSE(doc["bridge"]["time_synced"].as<bool>());
    TEST_ASSERT_TRUE(doc["bridge"]["time"].isNull());
    TEST_ASSERT_TRUE(doc["bridge"]["ntp_last_sync"].isNull());
    TEST_ASSERT_TRUE(doc["bridge"]["ntp_server"].isNull());
    TEST_ASSERT_TRUE(doc["bridge"]["ntp_server_source"].isNull());

    setenv("TZ", "UTC0", 1);
    tzset();
    BridgeInfo synced       = makeBridge();
    synced.timeSynced       = true;
    synced.currentEpoch     = 1704067200;  // 2024-01-01 00:00:00 UTC
    synced.lastNtpSyncEpoch = 1704067200 - 30;
    synced.ntpServer        = "192.168.2.1";  // DHCP option 42 hands out an IP
    synced.ntpFromDhcp      = true;
    TEST_ASSERT_TRUE(rest::buildStatusPayload(state, "eversolar_legacy", synced,
                                              r.diagnostics.snapshot(),
                                              &eversolar::descriptor(), g_now, json));
    doc = parse(json);
    TEST_ASSERT_TRUE(doc["bridge"]["time_synced"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("2024-01-01 00:00:00", doc["bridge"]["time"]);
    TEST_ASSERT_EQUAL_STRING("2023-12-31 23:59:30", doc["bridge"]["ntp_last_sync"]);
    // Which server answered, and where it came from -- DHCP option 42 vs the configured
    // fallback. An unknown source must be null, never a guessed server name.
    TEST_ASSERT_EQUAL_STRING("192.168.2.1", doc["bridge"]["ntp_server"]);
    TEST_ASSERT_EQUAL_STRING("dhcp", doc["bridge"]["ntp_server_source"]);
}

static void test_status_payload() {
    Rig        r;
    const auto state = r.poll();
    std::string json;
    TEST_ASSERT_TRUE(rest::buildStatusPayload(state, "eversolar_legacy", makeBridge(),
                                              r.diagnostics.snapshot(),
                                              &eversolar::descriptor(), g_now, json));
    auto doc = parse(json);

    TEST_ASSERT_EQUAL_STRING("heliograph-a1b2c3", doc["bridge"]["id"]);
    // The REGISTERED id, verbatim -- not identity.deviceId(). The store key is minted before
    // the bus hands over a serial number; recomputing the id afterwards sent the Device page
    // to /api/v1/devices/eversolar_legacy-XH.../capabilities, which 404s (live, 2026-07-19).
    TEST_ASSERT_EQUAL_STRING("eversolar_legacy", doc["device"]["id"]);
    TEST_ASSERT_EQUAL_STRING("0.1.0", doc["bridge"]["firmware_version"]);
    TEST_ASSERT_EQUAL_INT(-57, doc["bridge"]["wifi_rssi_dbm"].as<int>());
    TEST_ASSERT_EQUAL_STRING("eversolar_legacy", doc["device"]["driver_id"]);
    TEST_ASSERT_EQUAL_STRING("beta", doc["device"]["support_level"]);
    TEST_ASSERT_TRUE(doc["device"]["online"].as<bool>());
    TEST_ASSERT_DOUBLE_WITHIN(0.01, fx::expected::kAcPowerW,
                              doc["measurements"]["ac.power.total"]["value"].as<double>());
}

static void test_status_reports_unknown_before_the_first_poll() {
    // 0 seconds ago would read as "just polled".
    DeviceState state;
    std::string json;
    TEST_ASSERT_TRUE(rest::buildStatusPayload(state, "eversolar_legacy", makeBridge(),
                                              DiagnosticsSnapshot{}, nullptr, g_now, json));
    TEST_ASSERT_TRUE(parse(json)["device"]["last_successful_poll_seconds_ago"].isNull());
}

static void test_stale_status_is_null() {
    Rig r;
    DeviceContext ctx(r.driver, r.store, r.diagnostics, clockFn);
    ctx.pollOnce();
    r.device.offline = true;
    for (int i = 0; i < 10; ++i) {
        g_now += 10000;
        ctx.pollOnce();
    }
    std::string json;
    rest::buildStatusPayload(*r.store.snapshot(), "eversolar_legacy", makeBridge(),
                             r.diagnostics.snapshot(), &eversolar::descriptor(), g_now, json);
    auto doc = parse(json);

    TEST_ASSERT_TRUE(doc["measurements"]["ac.power.total"]["value"].isNull());
    TEST_ASSERT_TRUE(doc["status_code"].isNull());
    TEST_ASSERT_FALSE(doc["device"]["online"].as<bool>());
}

static void test_error_payload_is_uniform() {
    std::string json;
    TEST_ASSERT_TRUE(rest::buildErrorPayload(
        {404, "device_not_found", "No device with id 'foo'"}, "a3f1", json));
    auto doc = parse(json);
    TEST_ASSERT_EQUAL_STRING("device_not_found", doc["error"]["code"]);
    TEST_ASSERT_EQUAL_STRING("No device with id 'foo'", doc["error"]["message"]);
    TEST_ASSERT_EQUAL_STRING("a3f1", doc["error"]["request_id"]);
}

static void test_devices_payload() {
    std::string json;
    TEST_ASSERT_TRUE(rest::buildDevicesPayload({"eversolar_legacy-ABC123"}, json));
    auto doc = parse(json);
    TEST_ASSERT_EQUAL_size_t(1, doc["devices"].as<JsonArray>().size());
    TEST_ASSERT_EQUAL_STRING("eversolar_legacy-ABC123", doc["devices"][0]);
}

static void test_measurements_payload_omits_unsupported() {
    Rig        r;
    const auto state = r.poll();
    std::string json;
    TEST_ASSERT_TRUE(rest::buildMeasurementsPayload(state, json));
    auto doc = parse(json);

    TEST_ASSERT_TRUE(doc["measurements"]["ac.power.total"].is<JsonObject>());
    TEST_ASSERT_FALSE(doc["measurements"].as<JsonObject>()["battery.soc"].is<JsonObject>());
    TEST_ASSERT_TRUE(doc["measurements"]["dc.power.total"]["derived"].as<bool>());
}

static void test_capabilities_payload() {
    Rig        r;
    const auto state = r.poll();
    std::string json;
    TEST_ASSERT_TRUE(rest::buildCapabilitiesPayload(state.capabilities, json));
    auto doc = parse(json);
    TEST_ASSERT_TRUE(doc["read_only"].as<bool>());
    TEST_ASSERT_EQUAL_size_t(0, doc["write"].as<JsonArray>().size());
}

static void test_drivers_payload_drives_the_wizard() {
    DriverRegistry reg;
    registerBuiltinDrivers(reg);
    std::string json;
    TEST_ASSERT_TRUE(rest::buildDriversPayload(reg.availableDrivers(), json));
    auto doc = parse(json);

    bool foundEversolar = false;
    for (JsonObject d : doc["drivers"].as<JsonArray>()) {
        if (std::string(d["id"].as<const char*>()) == "eversolar_legacy") {
            foundEversolar = true;
            TEST_ASSERT_EQUAL_STRING("beta", d["support_level"]);
            TEST_ASSERT_FALSE(d["supports_write"].as<bool>());
            // The wizard shows which line settings will actually be tried.
            TEST_ASSERT_EQUAL_size_t(1, d["serial_profiles"].as<JsonArray>().size());
            TEST_ASSERT_EQUAL_UINT32(9600, d["serial_profiles"][0]["baud_rate"].as<uint32_t>());
            TEST_ASSERT_EQUAL_STRING("none", d["serial_profiles"][0]["parity"]);
        }
    }
    TEST_ASSERT_TRUE(foundEversolar);
}

static void test_diagnostics_payload_has_no_secrets() {
    Rig r;
    r.poll();
    std::string json;
    TEST_ASSERT_TRUE(rest::buildDiagnosticsPayload(r.diagnostics.snapshot(), makeBridge(), json));
    TEST_ASSERT_TRUE(json.find("password") == std::string::npos);
    auto doc = parse(json);
    TEST_ASSERT_EQUAL_UINT32(1, doc["poll_success_total"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING("Waveshare ESP32-S3-Relay-1CH", doc["board"]);
}

static void test_oversized_response_is_refused() {
    Rig        r;
    const auto state = r.poll();
    std::string json = "untouched";
    TEST_ASSERT_FALSE(rest::buildStatusPayload(state, "eversolar_legacy", makeBridge(),
                                               r.diagnostics.snapshot(), nullptr, g_now, json, 50));
    TEST_ASSERT_EQUAL_STRING("untouched", json.c_str());
}

// --- Prometheus ------------------------------------------------------------------------------

static void test_prometheus_exports_current_readings() {
    Rig        r;
    const auto state = r.poll();
    const auto text  = prometheus::buildMetrics(state, makeBridge(), r.diagnostics.snapshot());

    TEST_ASSERT_TRUE(text.find("heliograph_inverter_online 1\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_ac_power_watts 1842.000\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_poll_success_total 1\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_wifi_rssi_dbm -57\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_uptime_seconds 86400\n") != std::string::npos);
}

static void test_prometheus_omits_unknown_rather_than_exporting_zero() {
    // A missing sample is handled correctly by Prometheus; a 0 would be averaged into the
    // graph as a real reading.
    Rig r;
    DeviceContext ctx(r.driver, r.store, r.diagnostics, clockFn);
    ctx.pollOnce();
    r.device.offline = true;
    for (int i = 0; i < 10; ++i) {
        g_now += 10000;
        ctx.pollOnce();
    }
    const auto text =
        prometheus::buildMetrics(*r.store.snapshot(), makeBridge(), r.diagnostics.snapshot());

    TEST_ASSERT_TRUE(text.find("heliograph_inverter_ac_power_watts") == std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_online 0\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_data_stale 1\n") != std::string::npos);
    // Counters still work while the inverter is away.
    TEST_ASSERT_TRUE(text.find("heliograph_poll_failure_total 10\n") != std::string::npos);
}

static void test_prometheus_has_no_high_cardinality_labels() {
    Rig        r;
    const auto state = r.poll();
    const auto text  = prometheus::buildMetrics(state, makeBridge(), r.diagnostics.snapshot());

    // The serial number must never become a label: cardinality explodes across a fleet.
    TEST_ASSERT_TRUE(text.find(fx::kExpectedSerial) == std::string::npos);
}

static void test_prometheus_naming_conventions() {
    Rig        r;
    const auto state = r.poll();
    const auto text  = prometheus::buildMetrics(state, makeBridge(), r.diagnostics.snapshot());

    // Counters end in _total and are typed as counters; gauges are typed as gauges.
    TEST_ASSERT_TRUE(text.find("# TYPE heliograph_poll_success_total counter") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("# TYPE heliograph_inverter_ac_power_watts gauge") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("# HELP heliograph_uptime_seconds") != std::string::npos);
    // No uppercase in metric names.
    for (size_t i = 0; i < text.size(); ++i) {
        if (text.compare(i, 12, "heliograph_") == 0) {
            size_t j = i;
            while (j < text.size() && (isalnum(text[j]) || text[j] == '_')) {
                TEST_ASSERT_FALSE(isupper(text[j]));
                ++j;
            }
        }
    }
}

static void test_prometheus_build_info_carries_the_version() {
    Rig        r;
    const auto state = r.poll();
    const auto text  = prometheus::buildMetrics(state, makeBridge(), r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(text.find("heliograph_build_info{version=\"0.1.0\",driver=\"eversolar_legacy\"") !=
                     std::string::npos);
}

static void test_the_mock_hybrid_also_exports() {
    mock::MockDriver driver(clockFn, mock::MockOptions{});
    StateStore       store;
    Diagnostics      diag;
    g_now = 12ULL * 60 * 60 * 1000;
    DeviceContext ctx(driver, store, diag, clockFn);
    ctx.pollOnce();

    const auto text = prometheus::buildMetrics(*store.snapshot(), makeBridge(), diag.snapshot());
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_ac_power_watts") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_build_info{version=\"0.1.0\",driver=\"mock_inverter\"") !=
                     std::string::npos);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_config_response_contains_no_secret_anywhere);
    RUN_TEST(test_config_reports_whether_a_secret_is_set);
    RUN_TEST(test_patch_leaves_absent_fields_alone);
    RUN_TEST(test_patch_sets_a_password);
    RUN_TEST(test_explicit_null_clears_a_password);
    RUN_TEST(test_a_rejected_patch_changes_nothing);
    RUN_TEST(test_invalid_json_is_refused);
    RUN_TEST(test_out_of_range_values_are_refused);
    RUN_TEST(test_modbus_write_cannot_be_enabled);
    RUN_TEST(test_diagnostics_unit_id_must_differ_from_the_inverter);
    RUN_TEST(test_driver_options_are_opaque_to_the_config_model);
    RUN_TEST(test_driver_options_are_validated_against_the_driver);
    RUN_TEST(test_an_unset_option_falls_back_to_the_declared_default);
    RUN_TEST(test_driver_options_reach_the_driver);
    RUN_TEST(test_driver_id_defaults_to_empty_not_to_a_manufacturer);
    RUN_TEST(test_defaults_are_valid_and_read_only);
    RUN_TEST(test_hostname_must_be_a_valid_dns_label);
    RUN_TEST(test_log_level_gates_output);
    RUN_TEST(test_raw_frames_are_trace_only);
    RUN_TEST(test_modbus_config_is_actually_applied);
    RUN_TEST(test_modbus_write_stays_off_whatever_the_config_says);
    RUN_TEST(test_status_payload);
    RUN_TEST(test_status_reports_unknown_before_the_first_poll);
    RUN_TEST(test_stale_status_is_null);
    RUN_TEST(test_error_payload_is_uniform);
    RUN_TEST(test_devices_payload);
    RUN_TEST(test_measurements_payload_omits_unsupported);
    RUN_TEST(test_capabilities_payload);
    RUN_TEST(test_drivers_payload_drives_the_wizard);
    RUN_TEST(test_diagnostics_payload_has_no_secrets);
    RUN_TEST(test_oversized_response_is_refused);
    RUN_TEST(test_prometheus_exports_current_readings);
    RUN_TEST(test_prometheus_omits_unknown_rather_than_exporting_zero);
    RUN_TEST(test_prometheus_has_no_high_cardinality_labels);
    RUN_TEST(test_prometheus_naming_conventions);
    RUN_TEST(test_prometheus_build_info_carries_the_version);
    RUN_TEST(test_the_mock_hybrid_also_exports);
    RUN_TEST(test_status_payload_reports_clock_sync_state);
    return UNITY_END();
}
