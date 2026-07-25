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
    // Deliberately not the factory "admin": a leak of the default would be invisible in a body
    // that legitimately contains the word "admin" in prose or in a driver id.
    c.security.adminUsername   = "beheerder";
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
    // The admin username is half of the login, and this endpoint needs no credentials at all.
    // Serving it reduced guessing a login with no brute-force protection to guessing only the
    // password -- while mqtt.username, in the same serialiser, was already
    // omitted for exactly that reason.
    TEST_ASSERT_TRUE(json.find("beheerder") == std::string::npos);
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
    // The MQTT username is credential material: reported as a flag, never as the value.
    TEST_ASSERT_TRUE(doc["mqtt"]["username_set"].as<bool>());
    TEST_ASSERT_TRUE(doc["mqtt"]["username"].isNull());
    TEST_ASSERT_TRUE(json.find("solar") == std::string::npos);
    // No admin_username. This one is the actual regression guard: it fails against the old
    // serialiser.
    TEST_ASSERT_TRUE(doc["security"]["admin_username"].isNull());
    // And no *_set flag for it either, because validate() refuses an empty one so the flag
    // could only ever be true. Note this asserts nothing about the change -- the flag never
    // existed on either side. It is a forward guard against someone adding one later.
    TEST_ASSERT_TRUE(doc["security"]["username_set"].isNull());
    // Non-secret fields are readable, which is what makes the UI usable.
    TEST_ASSERT_EQUAL_STRING("thuisnetwerk", doc["wifi"]["ssid"]);
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", doc["mqtt"]["host"]);

    c.wifi.password.clear();
    c.mqtt.password.clear();
    c.mqtt.username.clear();
    c.security.adminPassword.clear();
    serializeConfig(c, json);
    doc = parse(json);
    TEST_ASSERT_FALSE(doc["wifi"]["password_set"].as<bool>());
    TEST_ASSERT_FALSE(doc["mqtt"]["password_set"].as<bool>());
    TEST_ASSERT_FALSE(doc["mqtt"]["username_set"].as<bool>());
    TEST_ASSERT_FALSE(doc["security"]["password_set"].as<bool>());
}

// --- serial override -------------------------------------------------------------------

// Discovery sweeps every profile a driver advertises and reports the one that answered. Saving
// the driver alone meant the next boot configured the driver's FIRST profile instead, so a
// device that had just been positively identified went silent. The override is what carries the
// answer across the reboot.
static void test_a_serial_override_round_trips_through_a_patch() {
    Configuration c;
    ConfigError   e;
    TEST_ASSERT_FALSE(c.serial.enabled);  // off unless something asks for it

    TEST_ASSERT_TRUE(applyConfigPatch(
        R"({"serial":{"override":true,"baud_rate":4800,"parity":"even","data_bits":8,)"
        R"("stop_bits":2}})", c, e));
    TEST_ASSERT_TRUE(c.serial.enabled);
    TEST_ASSERT_EQUAL_UINT32(4800, c.serial.profile.baudRate);
    TEST_ASSERT_EQUAL(SerialParity::Even, c.serial.profile.parity);
    TEST_ASSERT_EQUAL_UINT8(2, c.serial.profile.stopBits);

    std::string json;
    TEST_ASSERT_TRUE(serializeConfig(c, json));
    auto doc = parse(json);
    TEST_ASSERT_TRUE(doc["serial"]["override"].as<bool>());
    TEST_ASSERT_EQUAL_UINT32(4800, doc["serial"]["baud_rate"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING("even", doc["serial"]["parity"]);
}

// A typo must not quietly become "none": that configures a line the user did not ask for, and
// the symptom is a silent bus, which points the diagnosis straight at the cabling instead.
static void test_an_unknown_parity_is_refused_rather_than_defaulted() {
    Configuration c;
    c.serial.profile.parity = SerialParity::Odd;
    ConfigError e;
    TEST_ASSERT_FALSE(applyConfigPatch(R"({"serial":{"parity":"evne"}})", c, e));
    TEST_ASSERT_EQUAL_STRING("serial.parity", e.field.c_str());
    TEST_ASSERT_EQUAL(SerialParity::Odd, c.serial.profile.parity);  // untouched
}

static void test_serial_bounds_are_checked_only_when_the_override_is_on() {
    Configuration c;
    ConfigError   e;
    // Off: nothing configures the line from these fields, so a stale value must not block a
    // save of something unrelated.
    c.serial.profile.baudRate = 300;
    TEST_ASSERT_TRUE(validate(c, e));

    c.serial.enabled = true;
    TEST_ASSERT_FALSE(validate(c, e));
    TEST_ASSERT_EQUAL_STRING("serial.baud_rate", e.field.c_str());

    c.serial.profile.baudRate = 4800;
    c.serial.profile.dataBits = 9;
    TEST_ASSERT_FALSE(validate(c, e));
    TEST_ASSERT_EQUAL_STRING("serial.data_bits", e.field.c_str());

    c.serial.profile.dataBits = 8;
    TEST_ASSERT_TRUE(validate(c, e));

    // 7-bit framing is refused rather than coerced. The transport maps a profile onto the
    // ESP32's SERIAL_* constants and only handles the 8-bit ones; anything else falls through
    // to SERIAL_8N1, losing the data bits AND the parity while configure() still reports
    // success. That was unreachable while only drivers picked the line -- this override is what
    // opened it to the API, and a silent coercion there means a dead bus plus a log line
    // stating the setting that was not applied.
    c.serial.profile.dataBits = 7;
    TEST_ASSERT_FALSE(validate(c, e));
    TEST_ASSERT_EQUAL_STRING("serial.data_bits", e.field.c_str());
}

// An override is derived from one driver's profile sweep. Carried across to another driver it
// forces line settings never measured against it, and the symptom is a silent bus right after
// the restart the Driver card asked for -- with the cause in a different card.
static void test_switching_driver_drops_a_line_override_it_did_not_ask_for() {
    Configuration c;
    c.driver.id               = "eversolar_legacy";
    c.serial.enabled          = true;
    c.serial.profile.baudRate = 115200;
    ConfigError e;

    TEST_ASSERT_TRUE(applyConfigPatch(R"({"driver":{"id":"sunspec"}})", c, e));
    TEST_ASSERT_FALSE(c.serial.enabled);
    // The numbers stay put; only the flag drops, so turning it back on in Settings does not
    // mean retyping what discovery measured.
    TEST_ASSERT_EQUAL_UINT32(115200, c.serial.profile.baudRate);
}

// ...but not when the caller supplied `serial` in the same request. That is exactly what the
// wizard does -- driver and line together -- and clearing what the caller just asked for is the
// mistake the driver-option orphan rule had to be redesigned to avoid.
static void test_a_patch_that_sets_both_keeps_the_line_it_asked_for() {
    Configuration c;
    c.driver.id = "eversolar_legacy";
    ConfigError e;

    TEST_ASSERT_TRUE(applyConfigPatch(
        R"({"driver":{"id":"growatt_modbus"},"serial":{"override":true,"baud_rate":115200}})", c,
        e));
    TEST_ASSERT_TRUE(c.serial.enabled);
    TEST_ASSERT_EQUAL_UINT32(115200, c.serial.profile.baudRate);
}

// And an unchanged driver never triggers it: a save of something unrelated must not quietly
// undo a working line override.
static void test_an_unrelated_save_leaves_the_line_override_alone() {
    Configuration c;
    c.driver.id      = "growatt_modbus";
    c.serial.enabled = true;
    ConfigError e;

    TEST_ASSERT_TRUE(applyConfigPatch(R"({"bridge_name":"Zolder"})", c, e));
    TEST_ASSERT_TRUE(c.serial.enabled);
}

// The line is configured once, in setup(), immediately after the driver's begin(). Nothing
// reconfigures a live UART, so claiming the change is already in force would leave someone
// watching a bus still running at the old rate.
static void test_changing_the_serial_override_requires_a_reboot() {
    const Configuration base;
    auto                on = base;
    on.serial.enabled      = true;
    TEST_ASSERT_TRUE(configChangeRequiresReboot(base, on));

    auto faster = on;
    faster.serial.profile.baudRate = 19200;
    TEST_ASSERT_TRUE(configChangeRequiresReboot(on, faster));

    // While the override is off the stored numbers configure nothing, so editing them is not a
    // reason to make someone restart a working bridge.
    auto idle = base;
    idle.serial.profile.baudRate = 19200;
    TEST_ASSERT_FALSE(configChangeRequiresReboot(base, idle));
}

// --- several devices on one bus ----------------------------------------------------------

static void test_additional_devices_round_trip_through_a_patch() {
    Configuration c;
    ConfigError   e;
    TEST_ASSERT_TRUE(c.additionalDevices.empty());  // invisible on a single-inverter install

    TEST_ASSERT_TRUE(applyConfigPatch(
        R"({"additional_devices":[{"driver_id":"growatt_modbus","options":{"unit_id":"2"}},)"
        R"({"driver_id":"growatt_modbus","options":{"unit_id":"3"}}]})", c, e));
    TEST_ASSERT_EQUAL_UINT32(2, c.additionalDevices.size());
    TEST_ASSERT_EQUAL_STRING("growatt_modbus", c.additionalDevices[1].id.c_str());
    TEST_ASSERT_EQUAL_STRING("3", c.additionalDevices[1].options["unit_id"].c_str());

    std::string json;
    TEST_ASSERT_TRUE(serializeConfig(c, json));
    auto doc = parse(json);
    TEST_ASSERT_EQUAL_UINT32(2, doc["additional_devices"].size());
    TEST_ASSERT_EQUAL_STRING("2", doc["additional_devices"][0]["options"]["unit_id"]);
}

// Whole-list replacement, not a merge: there is no stable key to merge on, and an index the
// caller believes is element 2 may not be after someone else's edit.
static void test_sending_the_list_replaces_it_and_omitting_it_does_not() {
    Configuration c;
    ConfigError   e;
    applyConfigPatch(R"({"additional_devices":[{"driver_id":"sunspec"}]})", c, e);
    TEST_ASSERT_EQUAL_UINT32(1, c.additionalDevices.size());

    applyConfigPatch(R"({"bridge_name":"Zolder"})", c, e);
    TEST_ASSERT_EQUAL_UINT32(1, c.additionalDevices.size());  // untouched

    TEST_ASSERT_TRUE(applyConfigPatch(R"({"additional_devices":[]})", c, e));
    TEST_ASSERT_EQUAL_UINT32(0, c.additionalDevices.size());  // explicitly cleared
}

static void test_an_extra_device_must_name_a_driver() {
    Configuration c;
    ConfigError   e;
    // Empty is legal for `driver` -- it means "pick the highest-priority one". For an extra
    // device it is a poll slot that can never be filled, so the whole patch is refused and the
    // config is left untouched (applyConfigPatch validates the merged copy before adopting it).
    TEST_ASSERT_FALSE(applyConfigPatch(R"({"additional_devices":[{"options":{"unit_id":"2"}}]})",
                                       c, e));
    TEST_ASSERT_EQUAL_STRING("additional_devices[0].driver_id", e.field.c_str());
    TEST_ASSERT_TRUE(c.additionalDevices.empty());
}

static void test_the_device_count_is_bounded() {
    Configuration c;
    ConfigError   e;
    for (size_t i = 0; i < kMaxDevices - 1; ++i) {
        c.additionalDevices.push_back(DriverSettings{"growatt_modbus", false, {}});
    }
    TEST_ASSERT_TRUE(validate(c, e));  // driver + kMaxDevices-1 == the cap

    c.additionalDevices.push_back(DriverSettings{"growatt_modbus", false, {}});
    TEST_ASSERT_FALSE(validate(c, e));
    TEST_ASSERT_EQUAL_STRING("additional_devices", e.field.c_str());
}

// The drivers and their poll contexts are built once, in setup(). Adding an inverter changes
// nothing until the next boot, and saying otherwise would leave someone waiting for a device
// that is never going to appear.
static void test_adding_a_device_requires_a_reboot() {
    const Configuration base;
    auto                more = base;
    more.additionalDevices.push_back(DriverSettings{"sunspec", false, {}});
    TEST_ASSERT_TRUE(configChangeRequiresReboot(base, more));

    auto retuned = more;
    retuned.additionalDevices[0].options["unit_id"] = "4";
    TEST_ASSERT_TRUE(configChangeRequiresReboot(more, retuned));
}

// The blocker this design turned on. Three identical inverters on one bus differ only in their
// address, and no driver in this build has a serial number at the moment setup() keys the state
// store -- Growatt never reads one, EverSolar and SunSpec learn theirs on the first poll. So all
// three resolved to the bare driver id, DeviceManager handed them the SAME store, and they
// overwrote each other into one set of Home Assistant entities.
static void test_devices_on_one_bus_get_distinct_ids_without_a_serial() {
    DeviceIdentity a;
    a.driverId    = "growatt_modbus";
    a.instanceKey = "1";
    DeviceIdentity b = a;
    b.instanceKey    = "2";

    TEST_ASSERT_EQUAL_STRING("growatt_modbus-1", a.deviceId().c_str());
    TEST_ASSERT_TRUE(a.deviceId() != b.deviceId());
}

// A real serial still wins: it identifies the physical unit however it happens to be addressed,
// so re-addressing an inverter must not make it look like a new device.
static void test_a_serial_number_outranks_the_address() {
    DeviceIdentity id;
    id.driverId     = "eversolar_legacy";
    id.instanceKey  = "10";
    id.serialNumber = "XH300060115506193600V610";
    TEST_ASSERT_EQUAL_STRING("eversolar_legacy-XH300060115506193600V610", id.deviceId().c_str());
}

// One device and no address option: unchanged from before multi-device existed, which is what
// keeps every existing install's REST path and MQTT topic exactly where it was.
static void test_a_lone_device_without_either_keeps_the_bare_driver_id() {
    DeviceIdentity id;
    id.driverId = "eversolar_legacy";
    TEST_ASSERT_EQUAL_STRING("eversolar_legacy", id.deviceId().c_str());
}

// add() is idempotent by contract -- re-adding a known id hands back the existing store. That is
// right for a caller re-registering the same device and exactly wrong for two CONFIGURED devices
// sharing an id, so the boot loop has to ask contains() first. This pins the contract the loop
// depends on; an earlier version treated a null return as the collision signal and it never came.
static void test_re_adding_an_id_returns_the_same_store_rather_than_failing() {
    DeviceManager devices;
    StateStore*   first = devices.add("growatt_modbus-1");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_PTR(first, devices.add("growatt_modbus-1"));
    TEST_ASSERT_EQUAL_UINT32(1, devices.size());
    TEST_ASSERT_TRUE(devices.contains("growatt_modbus-1"));
    TEST_ASSERT_FALSE(devices.contains("growatt_modbus-2"));
}

static void test_the_device_manager_refuses_past_its_cap() {
    DeviceManager devices;
    for (size_t i = 0; i < kMaxDevices; ++i) {
        TEST_ASSERT_NOT_NULL(devices.add("d" + std::to_string(i)));
    }
    TEST_ASSERT_NULL(devices.add("one-too-many"));
    TEST_ASSERT_EQUAL_UINT32(kMaxDevices, devices.size());
}

// The settings page stops offering "Add a device" at this number, so it is a contract between
// the firmware and the page, not a decoration. Nothing guarded it: renaming or dropping the
// field would have degraded the UI to its hardcoded fallback with every test still green.
static void test_the_status_payload_publishes_the_device_cap() {
    Rig         r;
    const auto  state = r.poll();
    std::string json;
    TEST_ASSERT_TRUE(rest::buildStatusPayload(state, "eversolar_legacy", makeBridge(),
                                              r.diagnostics.snapshot(),
                                              &eversolar::descriptor(), g_now, json));
    TEST_ASSERT_EQUAL_UINT32(kMaxDevices, parse(json)["bridge"]["max_devices"].as<uint32_t>());
}

// A configured device that is not polling is the failure every mistake on the settings page
// ends in, and until this it existed only as one warn line in a ring buffer: the device list
// showed the ones that worked and the dashboard showed one.
static void test_the_status_payload_reports_devices_that_did_not_start() {
    Rig        r;
    const auto state  = r.poll();
    BridgeInfo bridge = makeBridge();
    bridge.devicesConfigured = 3;
    bridge.deviceProblems    = {"'growatt_modbus' shares the address of a device already added"};

    std::string json;
    TEST_ASSERT_TRUE(rest::buildStatusPayload(state, "eversolar_legacy", bridge,
                                              r.diagnostics.snapshot(),
                                              &eversolar::descriptor(), g_now, json));
    auto doc = parse(json);
    TEST_ASSERT_EQUAL_UINT32(3, doc["bridge"]["devices_configured"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(1, doc["bridge"]["device_problems"].size());
}

// Emitted empty rather than omitted: "no problems" and "this firmware cannot report problems"
// must not look identical to a client.
static void test_the_problem_list_is_always_present() {
    Rig         r;
    const auto  state = r.poll();
    std::string json;
    TEST_ASSERT_TRUE(rest::buildStatusPayload(state, "eversolar_legacy", makeBridge(),
                                              r.diagnostics.snapshot(),
                                              &eversolar::descriptor(), g_now, json));
    auto doc = parse(json);
    TEST_ASSERT_FALSE(doc["bridge"]["device_problems"].isNull());
    TEST_ASSERT_EQUAL_UINT32(0, doc["bridge"]["device_problems"].size());
}

static void test_reboot_required_flag_is_patch_only() {
    auto        c = configWithSecrets();
    std::string json;
    // GET path: no flag emitted.
    serializeConfig(c, json);
    TEST_ASSERT_TRUE(parse(json)["reboot_required"].isNull());
    // PATCH path: the caller's computed value is echoed verbatim.
    bool needed = true;
    serializeConfig(c, json, 4096, &needed);
    TEST_ASSERT_TRUE(parse(json)["reboot_required"].as<bool>());
    needed = false;
    serializeConfig(c, json, 4096, &needed);
    TEST_ASSERT_FALSE(parse(json)["reboot_required"].as<bool>());
}

static void test_reboot_required_only_for_boot_time_settings() {
    const Configuration base;

    // No change -> no reboot.
    TEST_ASSERT_FALSE(configChangeRequiresReboot(base, base));

    // Each boot-time domain forces a reboot.
    auto wifi = base;   wifi.wifi.ssid = "other";
    TEST_ASSERT_TRUE(configChangeRequiresReboot(base, wifi));
    auto mqttHost = base; mqttHost.mqtt.host = "10.0.0.9";
    TEST_ASSERT_TRUE(configChangeRequiresReboot(base, mqttHost));
    auto mqttUser = base; mqttUser.mqtt.username = "u";
    TEST_ASSERT_TRUE(configChangeRequiresReboot(base, mqttUser));
    auto qos = base;    qos.mqtt.qos = 1;  // editable only via the API, still boot-only
    TEST_ASSERT_TRUE(configChangeRequiresReboot(base, qos));
    auto diag = base;   diag.modbus.diagnosticsUnitId = 200;
    TEST_ASSERT_TRUE(configChangeRequiresReboot(base, diag));
    auto poll = base;   poll.polling.intervalSeconds = 30;
    TEST_ASSERT_TRUE(configChangeRequiresReboot(base, poll));
    auto drv = base;    drv.driver.options["layout"] = "dual";
    TEST_ASSERT_TRUE(configChangeRequiresReboot(base, drv));
    auto ntp = base;    ntp.ntp.timezone = "UTC0";
    TEST_ASSERT_TRUE(configChangeRequiresReboot(base, ntp));

    // Live-applied settings never demand one.
    auto name = base;   name.bridgeName = "Zonnebrug";
    TEST_ASSERT_FALSE(configChangeRequiresReboot(base, name));
    auto sec = base;    sec.security.readOnlyMode = !base.security.readOnlyMode;
    TEST_ASSERT_FALSE(configChangeRequiresReboot(base, sec));
    auto log = base;    log.logLevel = LogLevel::Debug;
    TEST_ASSERT_FALSE(configChangeRequiresReboot(base, log));
    auto rel = base;    rel.relays.enabled = !base.relays.enabled;
    TEST_ASSERT_FALSE(configChangeRequiresReboot(base, rel));
    // timezone_name is display metadata with no runtime effect.
    auto tzn = base;    tzn.ntp.timezoneName = "Europe/Berlin";
    TEST_ASSERT_FALSE(configChangeRequiresReboot(base, tzn));
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

// The settings page renders its Read-only checkbox from this field and diffs the saved value
// against it, so both halves are a contract, not an implementation detail. If GET stopped
// emitting it the box would render from an absent value; if PATCH stopped accepting it the
// switch would be unreachable from the UI again -- which is the hole this pair closes.
static void test_read_only_mode_survives_a_get_patch_round_trip() {
    auto        c = configWithSecrets();
    std::string json;
    TEST_ASSERT_TRUE(serializeConfig(c, json));
    TEST_ASSERT_TRUE(parse(json)["security"]["read_only_mode"].as<bool>());

    ConfigError e;
    TEST_ASSERT_TRUE(applyConfigPatch(R"({"security":{"read_only_mode":false}})", c, e));
    TEST_ASSERT_FALSE(c.security.readOnlyMode);
    TEST_ASSERT_TRUE(serializeConfig(c, json));
    TEST_ASSERT_FALSE(parse(json)["security"]["read_only_mode"].as<bool>());

    // And back on: the kill switch must be re-armable over the same path it was opened.
    TEST_ASSERT_TRUE(applyConfigPatch(R"({"security":{"read_only_mode":true}})", c, e));
    TEST_ASSERT_TRUE(c.security.readOnlyMode);
}

static void test_read_only_mode_is_untouched_by_an_unrelated_patch() {
    // The UI omits unchanged fields; a patch that never mentions the gate must leave it alone.
    auto c = configWithSecrets();
    c.security.readOnlyMode = false;
    ConfigError e;
    TEST_ASSERT_TRUE(applyConfigPatch(R"({"logging":{"level":"debug"}})", c, e));
    TEST_ASSERT_FALSE(c.security.readOnlyMode);

    c.security.readOnlyMode = true;
    TEST_ASSERT_TRUE(applyConfigPatch(R"({"security":{"admin_username":"tim"}})", c, e));
    TEST_ASSERT_TRUE(c.security.readOnlyMode);
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

// A value too large for the field used to be cast first and validated afterwards, so it wrapped
// into something validate() was perfectly happy with and the stored config was not the one that
// was asked for -- with no error anywhere. Whether a bad value got caught depended on where the
// wrap happened to land: 65537 became 1, a privileged port, while 70000 became 4464 and passed
// too (review, 2026-07-25).
static void test_a_value_too_large_for_the_field_is_refused_not_wrapped() {
    Configuration c;
    ConfigError   e;
    const uint16_t before = c.mqtt.port;

    TEST_ASSERT_FALSE(applyConfigPatch(R"({"mqtt":{"port":65537}})", c, e));
    TEST_ASSERT_EQUAL_STRING("mqtt.port", e.field.c_str());
    TEST_ASSERT_EQUAL_UINT16(before, c.mqtt.port);  // and nothing was half-applied

    TEST_ASSERT_FALSE(applyConfigPatch(R"({"mqtt":{"port":70000}})", c, e));
    TEST_ASSERT_FALSE(applyConfigPatch(R"({"mqtt":{"port":-1}})", c, e));
    TEST_ASSERT_FALSE(applyConfigPatch(R"({"modbus":{"unit_id":259}})", c, e));
    TEST_ASSERT_FALSE(applyConfigPatch(R"({"polling":{"interval_seconds":4294967306}})", c, e));

    // The boundary itself still works.
    TEST_ASSERT_TRUE(applyConfigPatch(R"({"mqtt":{"port":65535}})", c, e));
    TEST_ASSERT_EQUAL_UINT16(65535, c.mqtt.port);
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

// A stand-in registry: driver "alpha" declares {layout}, "beta" declares {address}. Returning
// nullptr for anything else models a typo'd driver id, which is the case the orphan cleanup
// must refuse to act on.
static const DriverDescriptor* fakeLookup(const std::string& id) {
    static const DriverDescriptor alpha = [] {
        DriverDescriptor d;
        d.id = "alpha";
        // Two options, one of them an enum with a default: a single-option driver could not tell
        // "keeps the others" from "replaces the map", and the enum is what the value-healing
        // rule below is judged against.
        d.options = {DriverOption{"layout", "Layout", "", "auto", {"auto", "single", "dual"}},
                     DriverOption{"note", "Note", "", "", {}}};
        return d;
    }();
    static const DriverDescriptor beta = [] {
        DriverDescriptor d;
        d.id      = "beta";
        d.options = {DriverOption{"address", "Address", "", "", {}}};
        return d;
    }();
    if (id == "alpha") return &alpha;
    if (id == "beta") return &beta;
    return nullptr;
}

// Options are scoped to the driver that declares them, and the merge has no delete, so an
// option left by a previous driver used to survive forever. Harmless until the REST layer
// started validating options against the descriptor (0.12.0), after which the orphan failed
// every later PATCH with "unknown option" and switching drivers became impossible short of a
// factory reset. Regression found in review 2026-07-25.
static void test_an_option_orphaned_by_a_driver_change_is_dropped() {
    Configuration c;
    ConfigError   e;
    c.driver.id                = "alpha";
    c.driver.options["layout"] = "dual";

    TEST_ASSERT_TRUE(applyConfigPatch(
        R"({"driver":{"id":"beta","options":{"address":"10"}}})", c, e, fakeLookup));

    TEST_ASSERT_EQUAL_STRING("beta", c.driver.id.c_str());
    TEST_ASSERT_EQUAL_STRING("10", c.driver.options["address"].c_str());
    TEST_ASSERT_TRUE(c.driver.options.find("layout") == c.driver.options.end());
}

// An already-stuck config must heal on any patch, not only on a driver change -- otherwise
// every bridge orphaned by 0.12.0 stays locked out.
static void test_an_existing_orphan_is_dropped_even_without_a_driver_change() {
    Configuration c;
    ConfigError   e;
    c.driver.id                 = "beta";
    c.driver.options["address"] = "10";
    c.driver.options["layout"]  = "dual";  // orphan from a previous driver

    TEST_ASSERT_TRUE(applyConfigPatch(R"({"bridge_name":"iets anders"})", c, e, fakeLookup));

    TEST_ASSERT_EQUAL_STRING("10", c.driver.options["address"].c_str());
    TEST_ASSERT_TRUE(c.driver.options.find("layout") == c.driver.options.end());
}

// A key THIS patch supplied is never dropped, even when the driver does not declare it: a
// typo'd option must be reported by validateDriverOptions, not silently swallowed here.
static void test_an_unknown_option_supplied_by_the_patch_survives_to_be_reported() {
    Configuration c;
    ConfigError   e;
    c.driver.id = "beta";

    TEST_ASSERT_TRUE(applyConfigPatch(
        R"({"driver":{"options":{"addres":"10"}}})", c, e, fakeLookup));  // typo
    TEST_ASSERT_EQUAL_STRING("10", c.driver.options["addres"].c_str());
}

// Nothing is dropped for a driver id that resolves to nothing. A typo'd DRIVER id would
// otherwise orphan every option at once and make the mistake unrecoverable; correcting the id
// must bring the configuration back.
static void test_a_typo_d_driver_id_destroys_no_options() {
    Configuration c;
    ConfigError   e;
    c.driver.id                = "alpha";
    c.driver.options["layout"] = "dual";

    TEST_ASSERT_TRUE(applyConfigPatch(R"({"driver":{"id":"alfa"}})", c, e, fakeLookup));
    TEST_ASSERT_EQUAL_STRING("dual", c.driver.options["layout"].c_str());

    // ...and correcting it restores a working configuration.
    TEST_ASSERT_TRUE(applyConfigPatch(R"({"driver":{"id":"alpha"}})", c, e, fakeLookup));
    TEST_ASSERT_EQUAL_STRING("dual", c.driver.options["layout"].c_str());
}

// A declared key can hold a value the driver no longer accepts without anyone editing it -- a
// firmware update that renames or drops a choice is enough, and the growatt `profile` option
// takes its allowed values from the generated profile list. validateDriverOptions then refuses
// every later PATCH, including ones touching nothing driver-related: the same lockout as an
// orphan, so it heals the same way, back to the declared default.
static void test_a_value_the_driver_no_longer_accepts_is_healed_to_the_default() {
    Configuration c;
    ConfigError   e;
    c.driver.id                = "alpha";
    c.driver.options["layout"] = "triple";  // no longer in allowedValues

    TEST_ASSERT_TRUE(applyConfigPatch(R"({"bridge_name":"iets anders"})", c, e, fakeLookup));
    TEST_ASSERT_EQUAL_STRING("auto", c.driver.options["layout"].c_str());
}

// A value this patch genuinely asserts is left alone, so the REST layer reports the mistake
// rather than this code silently correcting it.
static void test_a_bad_value_supplied_by_the_patch_is_left_for_the_validator() {
    Configuration c;
    ConfigError   e;
    c.driver.id = "alpha";

    TEST_ASSERT_TRUE(applyConfigPatch(R"({"driver":{"options":{"layout":"triple"}}})", c, e,
                                      fakeLookup));
    TEST_ASSERT_EQUAL_STRING("triple", c.driver.options["layout"].c_str());
}

// Echoing back what was just read is the obvious read-modify-write script, and it asserts
// nothing. Treating it as an assertion would let a broken stored value pin itself forever.
static void test_echoing_a_stored_bad_value_still_heals() {
    Configuration c;
    ConfigError   e;
    c.driver.id                = "alpha";
    c.driver.options["layout"] = "triple";
    c.driver.options["stale"]  = "x";  // orphan, echoed back too

    TEST_ASSERT_TRUE(applyConfigPatch(
        R"({"driver":{"options":{"layout":"triple","stale":"x"}}})", c, e, fakeLookup));

    TEST_ASSERT_EQUAL_STRING("auto", c.driver.options["layout"].c_str());
    TEST_ASSERT_TRUE(c.driver.options.find("stale") == c.driver.options.end());
}

// An empty id means "let the firmware pick", so there is no descriptor to judge against and
// nothing may be dropped.
static void test_an_empty_driver_id_keeps_the_options() {
    Configuration c;
    ConfigError   e;
    c.driver.id                = "alpha";
    c.driver.options["layout"] = "dual";

    TEST_ASSERT_TRUE(applyConfigPatch(R"({"driver":{"id":""}})", c, e, fakeLookup));
    TEST_ASSERT_EQUAL_STRING("dual", c.driver.options["layout"].c_str());
}

// Within one driver a partial patch must still MERGE, or setting one option would erase the
// others.
static void test_patching_one_option_keeps_the_others_on_the_same_driver() {
    Configuration c;
    ConfigError   e;
    c.driver.id                = "alpha";
    c.driver.options["layout"] = "dual";
    c.driver.options["note"]   = "keep me";

    TEST_ASSERT_TRUE(applyConfigPatch(R"({"driver":{"options":{"layout":"single"}}})", c, e,
                                      fakeLookup));
    TEST_ASSERT_EQUAL_STRING("single", c.driver.options["layout"].c_str());
    TEST_ASSERT_EQUAL_STRING("keep me", c.driver.options["note"].c_str());

    // Without a lookup nothing is ever dropped -- the pre-existing behaviour is untouched.
    Configuration d;
    ConfigError   e2;
    d.driver.id                = "alpha";
    d.driver.options["stale"]  = "x";
    TEST_ASSERT_TRUE(applyConfigPatch(R"({"driver":{"id":"beta"}})", d, e2));
    TEST_ASSERT_EQUAL_STRING("x", d.driver.options["stale"].c_str());
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

// The provision response echoes an operator-supplied hostname that the setup page drops into
// innerHTML. Validation restricts it to [A-Za-z0-9-] today, so the quotes and backslash below
// cannot currently reach this builder -- which is exactly why the escaping is pinned here
// rather than left to that validation: the builder must stay safe on its own terms if the
// character rules are ever relaxed. Before this test the payload was spliced together by hand.
static void test_provision_payload_escapes_the_hostname() {
    std::string json;
    TEST_ASSERT_TRUE(rest::buildProvisionPayload("he\"llo\\bridge", json));
    auto doc = parse(json);  // would fail outright on a broken splice
    TEST_ASSERT_EQUAL_STRING("saved", doc["status"]);
    TEST_ASSERT_TRUE(doc["rebooting"].as<bool>());
    // Round-trips byte-exact: escaped on the wire, original after parsing.
    TEST_ASSERT_EQUAL_STRING("he\"llo\\bridge", doc["hostname"]);

    std::string plain;
    TEST_ASSERT_TRUE(rest::buildProvisionPayload("heliograph-5af4e4", plain));
    TEST_ASSERT_EQUAL_STRING("heliograph-5af4e4", parse(plain)["hostname"]);
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
    TEST_ASSERT_EQUAL_STRING("Waveshare ESP32-S3-RS485-CAN", doc["board"]);
}

static void test_diagnostics_report_stack_marks_and_fragmentation() {
    Rig r;
    r.diagnostics.recordRs485StackFree(2500);
    r.diagnostics.recordLoopStackFree(4100);
    auto bridge              = makeBridge();
    bridge.maxAllocHeapBytes = 65536;
    std::string json;
    TEST_ASSERT_TRUE(rest::buildDiagnosticsPayload(r.diagnostics.snapshot(), bridge, json));
    auto doc = parse(json);
    TEST_ASSERT_EQUAL_UINT32(2500, doc["rs485_stack_free_bytes"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(4100, doc["loop_stack_free_bytes"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(65536, doc["max_alloc_heap_bytes"].as<uint32_t>());
}

static void test_stack_marks_are_null_before_the_first_sample() {
    // 0 would read as an exhausted stack to any alerting rule; before the tasks have
    // sampled themselves the honest answer is "unknown".
    std::string json;
    TEST_ASSERT_TRUE(rest::buildDiagnosticsPayload(DiagnosticsSnapshot{}, makeBridge(), json));
    TEST_ASSERT_TRUE(json.find("rs485_stack_free_bytes") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("loop_stack_free_bytes") != std::string::npos);
    auto doc = parse(json);
    TEST_ASSERT_TRUE(doc["rs485_stack_free_bytes"].isNull());
    TEST_ASSERT_TRUE(doc["loop_stack_free_bytes"].isNull());
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

static void test_prometheus_stack_and_fragmentation_gauges() {
    Rig        r;
    const auto state = r.poll();
    // Unsampled stack marks: the gauges are omitted entirely, like the RSSI gauge.
    auto text = prometheus::buildMetrics(state, makeBridge(), r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(text.find("rs485_stack_free_bytes") == std::string::npos);
    TEST_ASSERT_TRUE(text.find("loop_stack_free_bytes") == std::string::npos);

    r.diagnostics.recordRs485StackFree(2500);
    r.diagnostics.recordLoopStackFree(4100);
    auto bridge              = makeBridge();
    bridge.maxAllocHeapBytes = 65536;
    text = prometheus::buildMetrics(state, bridge, r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(text.find("heliograph_rs485_stack_free_bytes 2500\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_loop_stack_free_bytes 4100\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_max_alloc_heap_bytes 65536\n") != std::string::npos);
}

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

// A board without relays must not export relay series at all -- the same absent-not-zero rule
// the measurements follow. A permanent "heliograph_relay_energised 0" on a monitoring-only
// bridge would invite a dashboard panel for hardware that is not there.
static void test_prometheus_omits_relays_on_a_board_without_them() {
    Rig r;
    DeviceContext ctx(r.driver, r.store, r.diagnostics, clockFn);
    ctx.pollOnce();
    BridgeInfo b = makeBridge();  // relayCount stays 0
    const auto text = prometheus::buildMetrics(*r.store.snapshot(), b, r.diagnostics.snapshot());

    TEST_ASSERT_TRUE(text.find("heliograph_relay_energised") == std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_relays_enabled") == std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_drm_mode") == std::string::npos);
}

static void test_prometheus_exports_relay_and_drm_state() {
    Rig r;
    DeviceContext ctx(r.driver, r.store, r.diagnostics, clockFn);
    ctx.pollOnce();
    BridgeInfo b   = makeBridge();
    b.relayCount   = 3;
    b.relaysEnabled = true;
    b.relayMask    = 0b010;  // only relay index 1 energised
    b.relayRoles   = {"drm0", "drm5", "none"};
    const auto text = prometheus::buildMetrics(*r.store.snapshot(), b, r.diagnostics.snapshot());

    TEST_ASSERT_TRUE(text.find("heliograph_relays_enabled 1\n") != std::string::npos);
    // Labels carry the SAME 0-based index as the MQTT topic and the REST route.
    TEST_ASSERT_TRUE(text.find("heliograph_relay_energised{relay=\"0\"} 0\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_relay_energised{relay=\"1\"} 1\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_relay_energised{relay=\"2\"} 0\n") != std::string::npos);
    // Exactly relay 1 is on, and relay 1 carries the drm5 role, so that is the active mode.
    TEST_ASSERT_TRUE(text.find("heliograph_drm_mode{mode=\"drm5\"} 1\n") != std::string::npos);
}

// A metric family with labels carries exactly ONE HELP and ONE TYPE line, however many series
// it has. Repeating them per series is a parse error in strict Prometheus parsers, and it is
// the natural mistake to make when a later edit moves the header inside the loop -- at which
// point scraping breaks in the field while every other test here still passes.
static void test_prometheus_labelled_family_declares_help_once() {
    Rig r;
    DeviceContext ctx(r.driver, r.store, r.diagnostics, clockFn);
    ctx.pollOnce();
    BridgeInfo b = makeBridge();
    b.relayCount = 6;
    b.relayMask  = 0b101010;
    const auto text = prometheus::buildMetrics(*r.store.snapshot(), b, r.diagnostics.snapshot());

    auto count = [&text](const std::string& needle) {
        size_t n = 0;
        for (size_t at = text.find(needle); at != std::string::npos;
             at        = text.find(needle, at + needle.size())) {
            ++n;
        }
        return n;
    };
    TEST_ASSERT_EQUAL_size_t(1, count("# HELP heliograph_relay_energised"));
    TEST_ASSERT_EQUAL_size_t(1, count("# TYPE heliograph_relay_energised"));
    TEST_ASSERT_EQUAL_size_t(6, count("heliograph_relay_energised{relay="));
}

// Relays present but no DRM roles configured: the switches are reportable, the mode is not.
// Inventing "normal" would claim a curtailment model the operator never set up.
static void test_prometheus_omits_drm_mode_without_roles() {
    Rig r;
    DeviceContext ctx(r.driver, r.store, r.diagnostics, clockFn);
    ctx.pollOnce();
    BridgeInfo b = makeBridge();
    b.relayCount = 2;
    b.relayRoles = {"none", "none"};
    const auto text = prometheus::buildMetrics(*r.store.snapshot(), b, r.diagnostics.snapshot());

    TEST_ASSERT_TRUE(text.find("heliograph_relay_energised{relay=\"0\"}") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_drm_mode") == std::string::npos);
}

// A clock that has never synced must not export a 1970 timestamp: any "last sync was long ago"
// rule would then fire forever on a device that simply has no clock yet.
static void test_prometheus_omits_ntp_timestamp_until_synced() {
    Rig r;
    DeviceContext ctx(r.driver, r.store, r.diagnostics, clockFn);
    ctx.pollOnce();

    BridgeInfo unsynced = makeBridge();
    const auto before =
        prometheus::buildMetrics(*r.store.snapshot(), unsynced, r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(before.find("heliograph_time_synced 0\n") != std::string::npos);
    TEST_ASSERT_TRUE(before.find("heliograph_ntp_last_sync_timestamp_seconds") ==
                     std::string::npos);

    BridgeInfo synced      = makeBridge();
    synced.timeSynced      = true;
    synced.lastNtpSyncEpoch = 1753000000;
    const auto after =
        prometheus::buildMetrics(*r.store.snapshot(), synced, r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(after.find("heliograph_time_synced 1\n") != std::string::npos);
    TEST_ASSERT_TRUE(after.find("heliograph_ntp_last_sync_timestamp_seconds 1753000000\n") !=
                     std::string::npos);
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
    RUN_TEST(test_a_serial_override_round_trips_through_a_patch);
    RUN_TEST(test_an_unknown_parity_is_refused_rather_than_defaulted);
    RUN_TEST(test_serial_bounds_are_checked_only_when_the_override_is_on);
    RUN_TEST(test_switching_driver_drops_a_line_override_it_did_not_ask_for);
    RUN_TEST(test_a_patch_that_sets_both_keeps_the_line_it_asked_for);
    RUN_TEST(test_an_unrelated_save_leaves_the_line_override_alone);
    RUN_TEST(test_changing_the_serial_override_requires_a_reboot);
    RUN_TEST(test_additional_devices_round_trip_through_a_patch);
    RUN_TEST(test_sending_the_list_replaces_it_and_omitting_it_does_not);
    RUN_TEST(test_an_extra_device_must_name_a_driver);
    RUN_TEST(test_the_device_count_is_bounded);
    RUN_TEST(test_adding_a_device_requires_a_reboot);
    RUN_TEST(test_devices_on_one_bus_get_distinct_ids_without_a_serial);
    RUN_TEST(test_a_serial_number_outranks_the_address);
    RUN_TEST(test_a_lone_device_without_either_keeps_the_bare_driver_id);
    RUN_TEST(test_re_adding_an_id_returns_the_same_store_rather_than_failing);
    RUN_TEST(test_the_device_manager_refuses_past_its_cap);
    RUN_TEST(test_the_status_payload_publishes_the_device_cap);
    RUN_TEST(test_the_status_payload_reports_devices_that_did_not_start);
    RUN_TEST(test_the_problem_list_is_always_present);
    RUN_TEST(test_reboot_required_flag_is_patch_only);
    RUN_TEST(test_reboot_required_only_for_boot_time_settings);
    RUN_TEST(test_patch_leaves_absent_fields_alone);
    RUN_TEST(test_patch_sets_a_password);
    RUN_TEST(test_read_only_mode_survives_a_get_patch_round_trip);
    RUN_TEST(test_read_only_mode_is_untouched_by_an_unrelated_patch);
    RUN_TEST(test_explicit_null_clears_a_password);
    RUN_TEST(test_a_rejected_patch_changes_nothing);
    RUN_TEST(test_invalid_json_is_refused);
    RUN_TEST(test_out_of_range_values_are_refused);
    RUN_TEST(test_modbus_write_cannot_be_enabled);
    RUN_TEST(test_diagnostics_unit_id_must_differ_from_the_inverter);
    RUN_TEST(test_a_value_too_large_for_the_field_is_refused_not_wrapped);
    RUN_TEST(test_driver_options_are_opaque_to_the_config_model);
    RUN_TEST(test_an_option_orphaned_by_a_driver_change_is_dropped);
    RUN_TEST(test_an_existing_orphan_is_dropped_even_without_a_driver_change);
    RUN_TEST(test_an_unknown_option_supplied_by_the_patch_survives_to_be_reported);
    RUN_TEST(test_a_value_the_driver_no_longer_accepts_is_healed_to_the_default);
    RUN_TEST(test_a_bad_value_supplied_by_the_patch_is_left_for_the_validator);
    RUN_TEST(test_echoing_a_stored_bad_value_still_heals);
    RUN_TEST(test_a_typo_d_driver_id_destroys_no_options);
    RUN_TEST(test_an_empty_driver_id_keeps_the_options);
    RUN_TEST(test_patching_one_option_keeps_the_others_on_the_same_driver);
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
    RUN_TEST(test_provision_payload_escapes_the_hostname);
    RUN_TEST(test_devices_payload);
    RUN_TEST(test_measurements_payload_omits_unsupported);
    RUN_TEST(test_capabilities_payload);
    RUN_TEST(test_drivers_payload_drives_the_wizard);
    RUN_TEST(test_diagnostics_payload_has_no_secrets);
    RUN_TEST(test_diagnostics_report_stack_marks_and_fragmentation);
    RUN_TEST(test_stack_marks_are_null_before_the_first_sample);
    RUN_TEST(test_oversized_response_is_refused);
    RUN_TEST(test_prometheus_stack_and_fragmentation_gauges);
    RUN_TEST(test_prometheus_exports_current_readings);
    RUN_TEST(test_prometheus_omits_unknown_rather_than_exporting_zero);
    RUN_TEST(test_prometheus_omits_relays_on_a_board_without_them);
    RUN_TEST(test_prometheus_exports_relay_and_drm_state);
    RUN_TEST(test_prometheus_labelled_family_declares_help_once);
    RUN_TEST(test_prometheus_omits_drm_mode_without_roles);
    RUN_TEST(test_prometheus_omits_ntp_timestamp_until_synced);
    RUN_TEST(test_prometheus_has_no_high_cardinality_labels);
    RUN_TEST(test_prometheus_naming_conventions);
    RUN_TEST(test_prometheus_build_info_carries_the_version);
    RUN_TEST(test_the_mock_hybrid_also_exports);
    RUN_TEST(test_status_payload_reports_clock_sync_state);
    return UNITY_END();
}
