// SPDX-License-Identifier: MIT
// REST payloads, configuration redaction/validation and Prometheus output.

#include <unity.h>

#include <cmath>
#include <cstring>

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
                                              &eversolar::descriptor(), g_now, {}, json));
    TEST_ASSERT_EQUAL_UINT32(kMaxDevices, parse(json)["bridge"]["max_devices"].as<uint32_t>());
}

// --- the fleet summary the Dashboard and the header indicator are built from ---------------
//
// Everything below exists because the always-visible parts of the UI described the FIRST
// device as if it were the bridge: the header dot was device 1's `online`, and every tile came
// from device 1's measurements. On a bus of three, one dead inverter left a green indicator and
// a healthy-looking dashboard (#38).

/// A device whose readings are whatever the test says, so a fleet can be assembled without
/// three simulated inverters. Only the channels the summary reads are declared.
///
/// `stale` goes through markAllStale(), which is what the firmware actually does: it leaves
/// every measurement VALID and only sets the stale flag. Setting `dataStale` alone produced a
/// combination the state machine cannot reach -- fresh measurements under a stale device -- and
/// that is why the first version of these tests stayed green while a dead inverter's last
/// daylight reading was being summed into the Dashboard total (review, 2026-07-26).
static DeviceState fakeDevice(bool online, bool valid, bool stale, double watts,
                              double today, uint64_t lastPollMs) {
    DeviceState s;
    s.inverterOnline       = online;
    s.dataValid            = valid;
    s.dataStale            = stale;
    s.lastSuccessfulPollMs = lastPollMs;
    s.measurements.declare(measurement_id::kAcPowerTotal, MeasurementType::Power, Unit::Watt,
                           "AC Power");
    s.measurements.declare(measurement_id::kEnergyToday, MeasurementType::Energy,
                           Unit::KilowattHour, "Energy Today");
    if (valid) {
        s.measurements.set(measurement_id::kAcPowerTotal, watts, lastPollMs);
        s.measurements.set(measurement_id::kEnergyToday, today, lastPollMs);
    }
    if (stale) {
        s.measurements.markAllStale();
    }
    return s;
}

static JsonDocument statusOf(const std::vector<rest::DeviceSummary>& fleet) {
    Rig         r;
    const auto  state = r.poll();
    std::string json;
    TEST_ASSERT_TRUE(rest::buildStatusPayload(state, "eversolar_legacy", makeBridge(),
                                              r.diagnostics.snapshot(),
                                              &eversolar::descriptor(), g_now, fleet, json));
    return parse(json);
}

static void test_the_status_payload_totals_every_polled_device() {
    const auto a = rest::summariseDevice(fakeDevice(true, true, false, 1200.0, 5.0, g_now - 2000),
                                         "growatt_modbus-1", g_now);
    const auto b = rest::summariseDevice(fakeDevice(true, true, false, 800.0, 3.5, g_now - 1000),
                                         "growatt_modbus-2", g_now);
    const auto doc = statusOf({a, b});

    TEST_ASSERT_EQUAL_UINT32(2, doc["devices"].size());
    TEST_ASSERT_EQUAL_STRING("growatt_modbus-2", doc["devices"][1]["id"]);
    TEST_ASSERT_EQUAL_DOUBLE(2000.0, doc["totals"]["ac_power_w"].as<double>());
    TEST_ASSERT_EQUAL_DOUBLE(8.5, doc["totals"]["energy_today_kwh"].as<double>());
    TEST_ASSERT_EQUAL_UINT32(2, doc["totals"]["devices_answering"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(2, doc["totals"]["devices_polled"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(2, doc["totals"]["ac_power_devices"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(2, doc["devices"][0]["last_successful_poll_seconds_ago"].as<uint32_t>());
}

// The reason the count travels with every total: a sum over two of three inverters is
// indistinguishable from a sum over three that had a bad afternoon.
static void test_a_device_that_reports_nothing_does_not_count_towards_a_total() {
    const auto live = rest::summariseDevice(
        fakeDevice(true, true, false, 1200.0, 5.0, g_now - 2000), "growatt_modbus-1", g_now);
    // Started, never returned a byte: every channel declared, none valid.
    const auto dead = rest::summariseDevice(fakeDevice(false, false, false, 0.0, 0.0, 0),
                                            "growatt_modbus-2", g_now);
    const auto doc = statusOf({live, dead});

    // 1200, not 1200 + a fabricated 0.
    TEST_ASSERT_EQUAL_DOUBLE(1200.0, doc["totals"]["ac_power_w"].as<double>());
    TEST_ASSERT_EQUAL_UINT32(1, doc["totals"]["ac_power_devices"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(2, doc["totals"]["devices_polled"].as<uint32_t>());
    // The header dot keys off this: one of two answering must never read as healthy.
    TEST_ASSERT_EQUAL_UINT32(1, doc["totals"]["devices_answering"].as<uint32_t>());
    TEST_ASSERT_TRUE(doc["devices"][1]["ac_power_w"].isNull());
    TEST_ASSERT_TRUE(doc["devices"][1]["last_successful_poll_seconds_ago"].isNull());
}

// A stale reading leaves the total, and the row says when the device last answered instead.
//
// This is the state every inverter is in all night, and it is the one that has to be right:
// markAllStale() keeps `valid` true, so a summary that only checked validity kept summing
// yesterday's watts. The bridge reported production at 03:00 and labelled it "3 inverters"
// in the same neutral grey as a healthy afternoon.
static void test_a_stale_reading_leaves_the_total_and_the_device_is_not_answering() {
    const auto stale = rest::summariseDevice(
        fakeDevice(true, true, true, 900.0, 4.0, g_now - 60000), "growatt_modbus-1", g_now);
    const auto doc = statusOf({stale});

    TEST_ASSERT_TRUE(doc["totals"]["ac_power_w"].isNull());
    TEST_ASSERT_TRUE(doc["totals"]["energy_today_kwh"].isNull());
    // Zero of one reporting, which is what turns the tile's sub-label red.
    TEST_ASSERT_EQUAL_UINT32(0, doc["totals"]["ac_power_devices"].as<uint32_t>());
    TEST_ASSERT_TRUE(doc["devices"][0]["ac_power_w"].isNull());
    TEST_ASSERT_EQUAL_UINT32(0, doc["totals"]["devices_answering"].as<uint32_t>());
    TEST_ASSERT_TRUE(doc["devices"][0]["data_stale"].as<bool>());
    TEST_ASSERT_EQUAL_UINT32(60, doc["devices"][0]["last_successful_poll_seconds_ago"].as<uint32_t>());
}

// The same thing again, but reached the way the firmware reaches it: a real poll followed by
// enough failures to take the device offline. No fixture can lie about the state machine here,
// and it is the state machine -- markAllStale() leaving `valid` set -- that made the earlier
// version of this summary report watts at three in the morning.
static void test_the_night_state_reports_no_production() {
    Rig             r;
    const auto      polled = r.poll();
    const StalenessPolicy policy;
    DeviceState     s = polled;
    TEST_ASSERT_TRUE(s.measurements.isValid(measurement_id::kAcPowerTotal));

    for (uint32_t i = 0; i <= policy.failuresBeforeOffline; ++i) {
        s.recordPollFailure(g_now + (i + 1) * 10000, policy);
    }
    TEST_ASSERT_FALSE(s.inverterOnline);
    // The trap in one assertion: still valid, and stale.
    const Measurement* m = s.measurements.find(measurement_id::kAcPowerTotal);
    TEST_ASSERT_TRUE(m != nullptr && m->valid && m->stale);

    const auto doc = statusOf({rest::summariseDevice(s, "growatt_modbus-1", g_now + 120000)});
    TEST_ASSERT_TRUE(doc["totals"]["ac_power_w"].isNull());
    TEST_ASSERT_EQUAL_UINT32(0, doc["totals"]["ac_power_devices"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(0, doc["totals"]["devices_answering"].as<uint32_t>());
}

// A bridge with nothing polling still answers /status -- it is the only payload that says how
// many devices were configured and why each is missing. A total over no devices is unknown.
static void test_totals_over_no_devices_are_null_not_zero() {
    const auto doc = statusOf({});
    TEST_ASSERT_EQUAL_UINT32(0, doc["devices"].size());
    TEST_ASSERT_TRUE(doc["totals"]["ac_power_w"].isNull());
    TEST_ASSERT_TRUE(doc["totals"]["energy_today_kwh"].isNull());
    TEST_ASSERT_TRUE(doc["totals"]["energy_total_kwh"].isNull());
    TEST_ASSERT_EQUAL_UINT32(0, doc["totals"]["devices_polled"].as<uint32_t>());
}

// The bound is 8 KB and a full bus is eight devices. The fleet rows are new weight in the one
// payload the page fetches every second; if they push it over, the whole UI goes dark.
static void test_a_full_bus_of_summaries_still_fits() {
    std::vector<rest::DeviceSummary> fleet;
    for (size_t i = 0; i < kMaxDevices; ++i) {
        fleet.push_back(rest::summariseDevice(
            fakeDevice(true, true, false, 1234.5, 6.78, g_now - 1000),
            "growatt_modbus-" + std::to_string(i + 1), g_now));
    }
    Rig         r;
    const auto  state = r.poll();
    std::string json;
    TEST_ASSERT_TRUE(rest::buildStatusPayload(state, "growatt_modbus-1", makeBridge(),
                                              r.diagnostics.snapshot(),
                                              &eversolar::descriptor(), g_now, fleet, json));
    TEST_ASSERT_TRUE(json.size() < rest::kMaxResponseBytes);
    TEST_ASSERT_EQUAL_UINT32(kMaxDevices, parse(json)["devices"].size());
}

// A configured device that is not polling is the failure every mistake on the settings page
// ends in, and until this it existed only as one warn line in a ring buffer: the device list
// showed the ones that worked and the dashboard showed one.
static void test_the_status_payload_reports_devices_that_did_not_start() {
    Rig        r;
    const auto state  = r.poll();
    BridgeInfo bridge = makeBridge();
    bridge.devicesConfigured = 3;
    bridge.devicesStarted    = 2;
    bridge.deviceProblems    = {"device 3 ('growatt_modbus') resolves to growatt_modbus-2, "
                                "which another configured device already uses"};

    std::string json;
    TEST_ASSERT_TRUE(rest::buildStatusPayload(state, "eversolar_legacy", bridge,
                                              r.diagnostics.snapshot(),
                                              &eversolar::descriptor(), g_now, {}, json));
    auto doc = parse(json);
    TEST_ASSERT_EQUAL_UINT32(3, doc["bridge"]["devices_configured"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(2, doc["bridge"]["devices_started"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(1, doc["bridge"]["device_problems"].size());
    // The string has to name the configuration ROW, not just the driver: three inverters on
    // one bus share a driver id, so "'growatt_modbus' could not be started" identifies none
    // of them. This asserts the shape the boot loop produces.
    TEST_ASSERT_TRUE(std::string(doc["bridge"]["device_problems"][0]).find("device 3") !=
                     std::string::npos);
}

// Emitted empty rather than omitted: "no problems" and "this firmware cannot report problems"
// must not look identical to a client.
static void test_the_problem_list_is_always_present() {
    Rig         r;
    const auto  state = r.poll();
    std::string json;
    TEST_ASSERT_TRUE(rest::buildStatusPayload(state, "eversolar_legacy", makeBridge(),
                                              r.diagnostics.snapshot(),
                                              &eversolar::descriptor(), g_now, {}, json));
    auto doc = parse(json);
    TEST_ASSERT_FALSE(doc["bridge"]["device_problems"].isNull());
    TEST_ASSERT_EQUAL_UINT32(0, doc["bridge"]["device_problems"].size());
}

// Every device carries when it last answered, not only the first. Without it "started" and
// "answering" were indistinguishable for devices 2..N in the UI and in the API both -- and an
// inverter with A and B swapped starts perfectly and never returns a byte.
static void test_a_device_payload_says_when_it_last_answered() {
    Rig        r;
    const auto state = r.poll();

    std::string json;
    TEST_ASSERT_TRUE(rest::buildDevicePayload(state, "growatt_modbus-2",
                                              &eversolar::descriptor(), g_now + 45000, json));
    auto doc = parse(json);
    TEST_ASSERT_EQUAL_UINT32(45, doc["last_successful_poll_seconds_ago"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(0, doc["consecutive_poll_failures"].as<uint32_t>());

    // Never answered is its own state, not "0 seconds ago" -- that is a bus fault, and it must
    // not read as a device that replied a moment ago.
    DeviceState fresh;
    TEST_ASSERT_TRUE(rest::buildDevicePayload(fresh, "growatt_modbus-3", nullptr, g_now, json));
    TEST_ASSERT_TRUE(parse(json)["last_successful_poll_seconds_ago"].isNull());
}

static const DriverDescriptor& boundedUnitIdDescriptor() {
    static const DriverDescriptor d = [] {
        DriverDescriptor x;
        x.id      = "growatt_modbus";
        x.options = {DriverOption{"unit_id", "Modbus unit id", "", "1", {}, 1, 247}};
        return x;
    }();
    return d;
}

// The lockout this repo has had to remove twice, reopened by a new option shape. A numeric
// option only gained bounds in this release, so a value that was legal when it was stored is
// refused now -- and the REST gate validates the MERGED map, so it made every later PATCH fail,
// including one touching nothing driver-related. Healed on the same terms as the other two
// shapes: only a value this request did NOT assert.
static void test_a_stored_out_of_range_option_is_healed_not_fatal() {
    Configuration c;
    c.driver.id                  = "growatt_modbus";
    c.driver.options["unit_id"]  = "300";  // legal before the bounds existed
    ConfigError e;

    // A locally declared descriptor rather than a real driver's: this suite runs on env:native,
    // where the drivers are compiled out, and the behaviour under test is the config layer's.
    const auto lookup = [](const std::string& id) -> const DriverDescriptor* {
        return id == "growatt_modbus" ? &boundedUnitIdDescriptor() : nullptr;
    };
    // A patch that touches nothing driver-related must still go through.
    TEST_ASSERT_TRUE(applyConfigPatch(R"({"logging":{"level":"debug"}})", c, e, lookup));
    TEST_ASSERT_EQUAL_STRING("1", c.driver.options["unit_id"].c_str());  // back to the default
}

// ...but a bad value the caller just supplied is reported, not silently rewritten. Swallowing
// it would tell someone their typo had been accepted.
static void test_an_asserted_out_of_range_option_is_left_for_the_caller() {
    Configuration c;
    c.driver.id = "growatt_modbus";
    ConfigError e;
    const auto  lookup = [](const std::string& id) -> const DriverDescriptor* {
        return id == "growatt_modbus" ? &boundedUnitIdDescriptor() : nullptr;
    };
    TEST_ASSERT_TRUE(applyConfigPatch(R"({"driver":{"options":{"unit_id":"300"}}})", c, e, lookup));
    TEST_ASSERT_EQUAL_STRING("300", c.driver.options["unit_id"].c_str());
    DriverOptionError optionError;
    TEST_ASSERT_FALSE(validateDriverOptions(boundedUnitIdDescriptor(), c.driver.options,
                                           optionError));
}

// The extra devices had no healing at all, so one stored value their driver stopped accepting
// made the whole configuration unsaveable with nothing naming the row.
static void test_a_stored_out_of_range_extra_device_option_is_healed() {
    Configuration c;
    c.driver.id = "growatt_modbus";
    c.additionalDevices.push_back(DriverSettings{"growatt_modbus", false, {{"unit_id", "300"}}});
    ConfigError e;
    const auto  lookup = [](const std::string& id) -> const DriverDescriptor* {
        return id == "growatt_modbus" ? &boundedUnitIdDescriptor() : nullptr;
    };
    TEST_ASSERT_TRUE(applyConfigPatch(R"({"logging":{"level":"debug"}})", c, e, lookup));
    TEST_ASSERT_EQUAL_STRING("1", c.additionalDevices[0].options["unit_id"].c_str());
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
// --- capture payload ------------------------------------------------------------------------

static CaptureReport captureWithOneFrame() {
    CaptureReport report;
    report.status            = CaptureStatus::Done;
    report.profile.baudRate  = 19200;
    report.config.durationMs = 30000;
    report.config.maxFrames  = 64;
    report.idleGapMs         = 2;
    report.startedMs         = 1000;
    report.finishedMs        = 31000;
    report.totalBytes        = 4;
    report.modbusFrames      = 1;
    diag::CapturedFrame frame;
    frame.offsetMs       = 120;
    frame.gapBeforeMs    = 33;
    frame.bytes          = {0x01, 0x03, 0xC0, 0x0B};
    frame.modbusCrcValid = true;
    report.frames.push_back(frame);
    return report;
}

/// Hex uppercase and space-separated, matching every protocol document and decoder example in
/// docs/ -- so a captured frame can be held up against one by eye.
static void test_capture_payload_renders_hex_the_way_the_docs_do() {
    std::string body;
    TEST_ASSERT_TRUE(rest::buildCapturePayload(captureWithOneFrame(), 31000, body));
    JsonDocument doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, body) == DeserializationError::Ok);
    TEST_ASSERT_EQUAL_STRING("01 03 C0 0B", doc["frames"][0]["hex"]);
    TEST_ASSERT_EQUAL_INT(4, doc["frames"][0]["length"].as<int>());
    TEST_ASSERT_EQUAL_INT(120, doc["frames"][0]["offset_ms"].as<int>());
    TEST_ASSERT_EQUAL_INT(33, doc["frames"][0]["gap_before_ms"].as<int>());
    TEST_ASSERT_TRUE(doc["frames"][0]["modbus_crc_ok"].as<bool>());
}

/// The line is echoed in full because on an unidentified device it is a guess, and every
/// number in the report is only meaningful against the guess that produced it.
static void test_capture_payload_echoes_the_line_it_listened_at() {
    std::string body;
    TEST_ASSERT_TRUE(rest::buildCapturePayload(captureWithOneFrame(), 31000, body));
    JsonDocument doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, body) == DeserializationError::Ok);
    TEST_ASSERT_EQUAL_STRING("done", doc["status"]);
    TEST_ASSERT_EQUAL_INT(19200, doc["line"]["baud_rate"].as<int>());
    TEST_ASSERT_EQUAL_STRING("none", doc["line"]["parity"]);
    TEST_ASSERT_EQUAL_INT(2, doc["line"]["idle_gap_ms"].as<int>());
    TEST_ASSERT_EQUAL_INT(30, doc["requested_seconds"].as<int>());
    TEST_ASSERT_EQUAL_INT(30000, doc["elapsed_ms"].as<int>());
    TEST_ASSERT_EQUAL_INT(1, doc["summary"]["modbus_crc_ok"].as<int>());
    TEST_ASSERT_EQUAL_INT(4, doc["summary"]["bytes"].as<int>());
    TEST_ASSERT_FALSE(doc["summary"]["truncated"].as<bool>());
}

/// A failure has to carry its reason. "failed" alone leaves the operator unable to tell a busy
/// bus from line settings the UART refused.
static void test_a_failed_capture_carries_its_reason() {
    CaptureReport report;
    report.status = CaptureStatus::Failed;
    report.error  = "the RS485 bus was busy";
    std::string body;
    TEST_ASSERT_TRUE(rest::buildCapturePayload(report, 0, body));
    JsonDocument doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, body) == DeserializationError::Ok);
    TEST_ASSERT_EQUAL_STRING("failed", doc["status"]);
    TEST_ASSERT_EQUAL_STRING("the RS485 bus was busy", doc["error"]);
}

/// A capture filled to its byte ceiling must still serialise, because that is the case a
/// contributor reaches on a busy bus -- and a 500 at the moment the artefact matters is exactly
/// the failure this bound exists to prevent.
///
/// The ceiling is maxTotalBytes, deliberately, and not maxFrames x maxFrameBytes: the product
/// of two independently chosen limits is not a bound on anything. 256 x 256 renders to about
/// 220 KB of hex, which no board here can hold. Asserting on the reachable worst case rather
/// than an unreachable one is the whole point.
static void test_a_capture_filled_to_its_byte_ceiling_fits_in_the_response() {
    const diag::CaptureConfig limits;  // the defaults the REST layer cannot exceed
    CaptureReport             report;
    report.status = CaptureStatus::Done;
    size_t written = 0;
    while (written < limits.maxTotalBytes) {
        const size_t n = std::min(limits.maxFrameBytes, limits.maxTotalBytes - written);
        diag::CapturedFrame frame;
        frame.offsetMs    = 4294967295u;  // widest rendering of every numeric field
        frame.gapBeforeMs = 4294967295u;
        frame.bytes.assign(n, 0xA5);
        report.frames.push_back(frame);
        written += n;
    }
    report.totalBytes = static_cast<uint32_t>(written);

    std::string body;
    TEST_ASSERT_TRUE(rest::buildCapturePayload(report, 0, body));
    TEST_ASSERT_TRUE(body.size() <= rest::kMaxCaptureResponseBytes);
}

// --- restore preview ----------------------------------------------------------------------

/// The preview is the last thing between a wrong file and an applied configuration, so it has
/// to carry the three facts the decision rests on: what the file is, what changes, and whether
/// there is a way back.
static void test_restore_preview_carries_the_file_and_the_changes() {
    BackupContents backup;
    backup.formatVersion   = kBackupFormatVersion;
    backup.firmwareVersion = "0.13.2";
    backup.exportedAt      = "2026-07-26T12:00:00Z";
    backup.includesSecrets = false;
    const std::vector<ConfigDiffEntry> diff{{"mqtt.host", "old.broker", "new.broker"},
                                            {"polling.interval_seconds", "10", "30"}};

    std::string body;
    TEST_ASSERT_TRUE(rest::buildRestorePreviewPayload(backup, diff, true, true, body));
    JsonDocument doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, body) == DeserializationError::Ok);
    TEST_ASSERT_EQUAL_STRING("0.13.2", doc["backup"]["firmware_version"]);
    TEST_ASSERT_EQUAL_STRING("2026-07-26T12:00:00Z", doc["backup"]["exported_at"]);
    TEST_ASSERT_FALSE(doc["backup"]["includes_secrets"].as<bool>());
    TEST_ASSERT_EQUAL_INT(2, doc["change_count"].as<int>());
    TEST_ASSERT_TRUE(doc["reboot_required"].as<bool>());
    TEST_ASSERT_TRUE(doc["rollback_exists"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("mqtt.host", doc["changes"][0]["field"]);
    TEST_ASSERT_EQUAL_STRING("old.broker", doc["changes"][0]["before"]);
    TEST_ASSERT_EQUAL_STRING("new.broker", doc["changes"][0]["after"]);
}

/// An undated backup renders as undated. Emitting "" would put an empty cell in a date column,
/// which reads as a bug rather than as "the bridge that wrote this had no clock".
static void test_restore_preview_omits_an_absent_export_date() {
    BackupContents backup;
    backup.formatVersion = kBackupFormatVersion;
    std::string body;
    TEST_ASSERT_TRUE(rest::buildRestorePreviewPayload(backup, {}, false, false, body));
    JsonDocument doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, body) == DeserializationError::Ok);
    TEST_ASSERT_TRUE(doc["backup"]["exported_at"].isNull());
    TEST_ASSERT_TRUE(doc["backup"]["firmware_version"].isNull());
    TEST_ASSERT_EQUAL_INT(0, doc["change_count"].as<int>());
    TEST_ASSERT_FALSE(doc["rollback_exists"].as<bool>());
}

/// The preview is rendered in a browser and is exactly the sort of screen that ends up in a
/// support thread. diffConfigurations already redacts; this asserts the payload does not
/// reintroduce a value on its way out.
static void test_restore_preview_of_a_real_backup_leaks_no_password() {
    Configuration before;
    before.wifi.ssid              = "HomeNet";
    before.wifi.password          = "before-secret";
    before.security.adminPassword = "before-admin";

    Configuration after           = before;
    after.wifi.password           = "after-secret";
    after.security.adminPassword  = "";
    after.mqtt.host               = "broker.local";

    std::vector<ConfigDiffEntry> diff;
    TEST_ASSERT_TRUE(diffConfigurations(before, after, diff));

    BackupContents backup;
    backup.formatVersion = kBackupFormatVersion;
    std::string body;
    TEST_ASSERT_TRUE(rest::buildRestorePreviewPayload(backup, diff, false, true, body));
    TEST_ASSERT_NULL(std::strstr(body.c_str(), "before-secret"));
    TEST_ASSERT_NULL(std::strstr(body.c_str(), "after-secret"));
    TEST_ASSERT_NULL(std::strstr(body.c_str(), "before-admin"));
    TEST_ASSERT_NOT_NULL(std::strstr(body.c_str(), "(not set)"));
}

/// rollback_stored is reported separately from status on purpose: the restore succeeded AND
/// there is no undo is a real combination on a full flash, and one field cannot say both.
static void test_restore_result_reports_a_missing_undo_separately() {
    std::string body;
    TEST_ASSERT_TRUE(rest::buildRestoreResultPayload(7, true, false, body));
    JsonDocument doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, body) == DeserializationError::Ok);
    TEST_ASSERT_EQUAL_STRING("restored", doc["status"]);
    TEST_ASSERT_EQUAL_INT(7, doc["changed_fields"].as<int>());
    TEST_ASSERT_TRUE(doc["reboot_required"].as<bool>());
    TEST_ASSERT_FALSE(doc["rollback_stored"].as<bool>());
}

static void test_status_payload_reports_clock_sync_state() {
    Rig        r;
    const auto state = r.poll();

    BridgeInfo unsynced = makeBridge();
    unsynced.timeSynced = false;
    std::string json;
    TEST_ASSERT_TRUE(rest::buildStatusPayload(state, "eversolar_legacy", unsynced,
                                              r.diagnostics.snapshot(),
                                              &eversolar::descriptor(), g_now, {}, json));
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
                                              &eversolar::descriptor(), g_now, {}, json));
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
                                              &eversolar::descriptor(), g_now, {}, json));
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
                                              DiagnosticsSnapshot{}, nullptr, g_now, {}, json));
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
                             r.diagnostics.snapshot(), &eversolar::descriptor(), g_now, {}, json);
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

// The three heap figures are MALLOC_CAP_INTERNAL and exclude PSRAM, so without these two an
// 8 MB board reported ~300 KB of RAM and a board whose PSRAM never trained looked identical
// to one where it worked (audit, 2026-07-26).
static void test_psram_is_reported_when_the_board_has_it() {
    Rig  r;
    auto bridge            = makeBridge();
    bridge.psramSizeBytes  = 8 * 1024 * 1024;
    bridge.psramFreeBytes  = 7 * 1024 * 1024;
    std::string json;
    TEST_ASSERT_TRUE(rest::buildDiagnosticsPayload(r.diagnostics.snapshot(), bridge, json));
    auto doc = parse(json);
    TEST_ASSERT_EQUAL_UINT32(8388608, doc["psram_size_bytes"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(7340032, doc["psram_free_bytes"].as<uint32_t>());
}

// Null, not 0. Zero free is a real reading on a board that HAS PSRAM and has exhausted it;
// collapsing that onto "no PSRAM fitted" would hide the more alarming of the two.
static void test_psram_is_null_on_a_board_without_it() {
    Rig  r;
    auto bridge           = makeBridge();
    bridge.psramSizeBytes = 0;  // Relay-6CH: ESP32-S3-WROOM-1U-N8, no PSRAM
    bridge.psramFreeBytes = 0;
    std::string json;
    TEST_ASSERT_TRUE(rest::buildDiagnosticsPayload(r.diagnostics.snapshot(), bridge, json));
    auto doc = parse(json);
    TEST_ASSERT_TRUE(doc["psram_size_bytes"].isNull());
    TEST_ASSERT_TRUE(doc["psram_free_bytes"].isNull());
}

// The dump has been written to flash on every panic since the OTA partition layout was
// designed; nothing read it until 2026-07-26. These pin the reporting, not the reading -- the
// esp_core_dump_* calls are ESP32-only and the summary reaches the builder as a plain struct.
// Until this counter existed, publish() returning 0 -- link down, or the client's outbox out
// of memory -- was discarded at eleven of its twelve call sites, so a message that never left
// looked exactly like one that did. A wedged client reports connected the whole time, which is
// why mqtt_connected alone was not enough.
static void test_mqtt_publish_failures_are_counted_and_published() {
    Rig r;
    r.diagnostics.recordMqttPublishFailure();
    r.diagnostics.recordMqttPublishFailure();
    r.diagnostics.recordMqttPublishFailure();

    std::string json;
    TEST_ASSERT_TRUE(rest::buildDiagnosticsPayload(r.diagnostics.snapshot(), makeBridge(), json));
    auto doc = parse(json);
    TEST_ASSERT_EQUAL_UINT32(3, doc["mqtt_publish_failure_total"].as<uint32_t>());
}

static void test_coredump_is_reported_when_one_is_stored() {
    Rig  r;
    auto bridge            = makeBridge();
    bridge.coredumpPresent = true;
    bridge.coredumpTask    = "rs485";
    bridge.coredumpPc      = 0x42011AF0;
    std::string json;
    TEST_ASSERT_TRUE(rest::buildDiagnosticsPayload(r.diagnostics.snapshot(), bridge, json));
    auto doc = parse(json);
    TEST_ASSERT_TRUE(doc["coredump_present"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("rs485", doc["coredump_task"].as<const char*>());
    TEST_ASSERT_EQUAL_UINT32(0x42011AF0u, doc["coredump_pc"].as<uint32_t>());
}

// Task "" at PC 0 is not a fact about anything, and coredump_present already carries the whole
// message. Null keeps a dashboard from rendering a crash that did not happen.
static void test_coredump_details_are_null_when_none_is_stored() {
    Rig  r;
    auto bridge            = makeBridge();
    bridge.coredumpPresent = false;
    std::string json;
    TEST_ASSERT_TRUE(rest::buildDiagnosticsPayload(r.diagnostics.snapshot(), bridge, json));
    auto doc = parse(json);
    TEST_ASSERT_FALSE(doc["coredump_present"].as<bool>());
    TEST_ASSERT_TRUE(doc["coredump_task"].isNull());
    TEST_ASSERT_TRUE(doc["coredump_pc"].isNull());
}

// A dump whose summary could not be parsed still says present -- the ELF is retrievable with
// the host tool even when the on-device summariser cannot read it.
static void test_a_nameless_coredump_still_reports_present() {
    Rig  r;
    auto bridge            = makeBridge();
    bridge.coredumpPresent = true;
    bridge.coredumpTask    = "";
    std::string json;
    TEST_ASSERT_TRUE(rest::buildDiagnosticsPayload(r.diagnostics.snapshot(), bridge, json));
    auto doc = parse(json);
    TEST_ASSERT_TRUE(doc["coredump_present"].as<bool>());
    TEST_ASSERT_TRUE(doc["coredump_task"].isNull());
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
                                               r.diagnostics.snapshot(), nullptr, g_now, {}, json, 50));
    TEST_ASSERT_EQUAL_STRING("untouched", json.c_str());
}

// --- Prometheus ------------------------------------------------------------------------------

/// One device, the way every test below used to call buildMetrics directly. The id is what
/// lands in the `device` label, so the assertions read the same shape a scraper sees.
static std::string metricsOf(const DeviceState& state, const BridgeInfo& bridge,
                             const DiagnosticsSnapshot& diagnostics,
                             const char* id = "eversolar_legacy") {
    return prometheus::buildMetrics({{id, &state}}, bridge, diagnostics);
}

static void test_prometheus_stack_and_fragmentation_gauges() {
    Rig        r;
    const auto state = r.poll();
    // Unsampled stack marks: the gauges are omitted entirely, like the RSSI gauge.
    auto text = metricsOf(state, makeBridge(), r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(text.find("rs485_stack_free_bytes") == std::string::npos);
    TEST_ASSERT_TRUE(text.find("loop_stack_free_bytes") == std::string::npos);

    r.diagnostics.recordRs485StackFree(2500);
    r.diagnostics.recordLoopStackFree(4100);
    auto bridge              = makeBridge();
    bridge.maxAllocHeapBytes = 65536;
    text = metricsOf(state, bridge, r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(text.find("heliograph_rs485_stack_free_bytes 2500\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_loop_stack_free_bytes 4100\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_max_alloc_heap_bytes 65536\n") != std::string::npos);
}

// Omitted rather than zero on a board with no PSRAM, like the RSSI and stack gauges: a flat 0
// would read as exhaustion to an alerting rule.
// 0 is a fact here ("no crash stored"), not a missing sample, so unlike the PSRAM gauges this
// one is always emitted -- it is the series an alert rule watches for going to 1.
// A counter, so 0 is emitted from the start: a rate() rule needs the series to exist before
// the first failure, not to appear at the moment things go wrong.
static void test_prometheus_exports_the_publish_failure_counter() {
    Rig        r;
    const auto state = r.poll();
    auto       text  = metricsOf(state, makeBridge(), r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(text.find("heliograph_mqtt_publish_failures_total 0\n") != std::string::npos);

    r.diagnostics.recordMqttPublishFailure();
    text = metricsOf(state, makeBridge(), r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(text.find("heliograph_mqtt_publish_failures_total 1\n") != std::string::npos);
}

static void test_prometheus_always_reports_the_coredump_flag() {
    Rig        r;
    const auto state = r.poll();

    auto clean = makeBridge();
    clean.coredumpPresent = false;
    auto text = metricsOf(state, clean, r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(text.find("heliograph_coredump_present 0\n") != std::string::npos);

    auto crashed = makeBridge();
    crashed.coredumpPresent = true;
    text = metricsOf(state, crashed, r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(text.find("heliograph_coredump_present 1\n") != std::string::npos);
}

static void test_prometheus_reports_psram_only_when_present() {
    Rig        r;
    const auto state = r.poll();

    auto without = makeBridge();
    without.psramSizeBytes = 0;
    auto text = metricsOf(state, without, r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(text.find("heliograph_psram_size_bytes") == std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_psram_free_bytes") == std::string::npos);

    auto with_ = makeBridge();
    with_.psramSizeBytes = 8388608;
    with_.psramFreeBytes = 7340032;
    text = metricsOf(state, with_, r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(text.find("heliograph_psram_size_bytes 8388608\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_psram_free_bytes 7340032\n") != std::string::npos);
}

static void test_prometheus_exports_current_readings() {
    Rig        r;
    const auto state = r.poll();
    const auto text  = metricsOf(state, makeBridge(), r.diagnostics.snapshot());

    TEST_ASSERT_TRUE(text.find("heliograph_inverter_online{device=\"eversolar_legacy\"} 1\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_ac_power_watts{device=\"eversolar_legacy\"} 1842.000\n") != std::string::npos);
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
        metricsOf(*r.store.snapshot(), makeBridge(), r.diagnostics.snapshot());

    TEST_ASSERT_TRUE(text.find("heliograph_inverter_ac_power_watts") == std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_online{device=\"eversolar_legacy\"} 0\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_data_stale{device=\"eversolar_legacy\"} 1\n") != std::string::npos);
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
    const auto text = metricsOf(*r.store.snapshot(), b, r.diagnostics.snapshot());

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
    const auto text = metricsOf(*r.store.snapshot(), b, r.diagnostics.snapshot());

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
    const auto text = metricsOf(*r.store.snapshot(), b, r.diagnostics.snapshot());

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
    const auto text = metricsOf(*r.store.snapshot(), b, r.diagnostics.snapshot());

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
        metricsOf(*r.store.snapshot(), unsynced, r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(before.find("heliograph_time_synced 0\n") != std::string::npos);
    TEST_ASSERT_TRUE(before.find("heliograph_ntp_last_sync_timestamp_seconds") ==
                     std::string::npos);

    BridgeInfo synced      = makeBridge();
    synced.timeSynced      = true;
    synced.lastNtpSyncEpoch = 1753000000;
    const auto after =
        metricsOf(*r.store.snapshot(), synced, r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(after.find("heliograph_time_synced 1\n") != std::string::npos);
    TEST_ASSERT_TRUE(after.find("heliograph_ntp_last_sync_timestamp_seconds 1753000000\n") !=
                     std::string::npos);
}

static void test_prometheus_has_no_high_cardinality_labels() {
    Rig        r;
    const auto state = r.poll();
    const auto text  = metricsOf(state, makeBridge(), r.diagnostics.snapshot());

    // The serial number must never become a label: cardinality explodes across a fleet.
    TEST_ASSERT_TRUE(text.find(fx::kExpectedSerial) == std::string::npos);
}

static void test_prometheus_naming_conventions() {
    Rig        r;
    const auto state = r.poll();
    const auto text  = metricsOf(state, makeBridge(), r.diagnostics.snapshot());

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

// --- Prometheus with more than one inverter ---------------------------------------------
//
// Every inverter series carries a `device` label, the first one included. The alternative --
// leaving device 1 unlabelled for back-compatibility, the way MQTT keeps its bridge-scoped
// topics -- would have made `sum by (device)` produce a blank label forever (#36).

static void test_every_inverter_series_carries_its_device_label() {
    Rig r1;
    Rig r2;
    const auto a = r1.poll();
    const auto b = r2.poll();
    const auto text =
        prometheus::buildMetrics({{"growatt_modbus-1", &a}, {"growatt_modbus-2", &b}},
                                 makeBridge(), r1.diagnostics.snapshot());

    TEST_ASSERT_TRUE(text.find("heliograph_inverter_ac_power_watts{device=\"growatt_modbus-1\"} "
                               "1842.000\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_ac_power_watts{device=\"growatt_modbus-2\"} "
                               "1842.000\n") != std::string::npos);
    // Bridge-wide series stay unlabelled: the counters live in one Diagnostics for the bus.
    TEST_ASSERT_TRUE(text.find("heliograph_uptime_seconds 86400\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_uptime_seconds{") == std::string::npos);
}

// HELP and TYPE appear once per metric family, whatever the device count. Repeating them per
// series is a parse error in strict parsers -- the relay family already had a test for this,
// and turning the inverter gauges into families made it apply to them too.
static void test_help_and_type_appear_once_per_family_across_devices() {
    Rig r1;
    Rig r2;
    const auto a = r1.poll();
    const auto b = r2.poll();
    const auto text =
        prometheus::buildMetrics({{"growatt_modbus-1", &a}, {"growatt_modbus-2", &b}},
                                 makeBridge(), r1.diagnostics.snapshot());

    const std::string help = "# HELP heliograph_inverter_ac_power_watts";
    size_t            count = 0;
    for (size_t at = text.find(help); at != std::string::npos; at = text.find(help, at + 1)) {
        ++count;
    }
    TEST_ASSERT_EQUAL_UINT32(1, count);
}

// A family no device reports is absent entirely -- not a bare HELP/TYPE header with no series
// under it, which is what a lazily-written header exists to prevent.
static void test_a_family_no_device_reports_has_no_header() {
    Rig r;
    DeviceContext ctx(r.driver, r.store, r.diagnostics, clockFn);
    ctx.pollOnce();
    r.device.offline = true;
    for (int i = 0; i < 10; ++i) {
        g_now += 10000;
        ctx.pollOnce();
    }
    const auto text = metricsOf(*r.store.snapshot(), makeBridge(), r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(text.find("# HELP heliograph_inverter_ac_power_watts") == std::string::npos);
    TEST_ASSERT_TRUE(text.find("# TYPE heliograph_inverter_ac_power_watts") == std::string::npos);
}

// --- Modbus TCP unit ids --------------------------------------------------------------------
//
// One unit id per inverter, consecutively from modbus.unit_id: the Modbus specification's Unit
// Identifier is exactly the field for addressing a device behind a gateway, so a client needs
// no vendor-specific arithmetic. Device 1 stays where it has always been.

static modbus::ModbusServerConfig serverConfig(uint8_t base, uint8_t devices,
                                               uint8_t diagnostics = 250) {
    modbus::ModbusServerConfig cfg;
    cfg.inverterUnitId    = base;
    cfg.diagnosticsUnitId = diagnostics;
    cfg.deviceCount       = devices;
    return cfg;
}

static void test_each_device_gets_the_next_unit_id() {
    modbus::ModbusTcpServer server(serverConfig(1, 3));
    TEST_ASSERT_EQUAL_UINT8(3, server.servedDevices());
    TEST_ASSERT_EQUAL_UINT8(1, server.unitIdFor(0));
    TEST_ASSERT_EQUAL_UINT8(2, server.unitIdFor(1));
    TEST_ASSERT_EQUAL_UINT8(3, server.unitIdFor(2));
    TEST_ASSERT_EQUAL_UINT8(0, server.unitIdFor(3));  // not served
}

// A single-inverter bridge is served at exactly the unit id it always was, whatever the base.
static void test_one_device_is_unchanged_at_the_configured_unit_id() {
    modbus::ModbusTcpServer server(serverConfig(7, 1));
    TEST_ASSERT_EQUAL_UINT8(1, server.servedDevices());
    TEST_ASSERT_EQUAL_UINT8(7, server.unitIdFor(0));
}

// The run has to be unbroken: a client derives device N's unit id by adding, so skipping a
// taken id and carrying on would leave a hole nothing can express. It stops instead, and
// main() logs how many were dropped.
static void test_the_run_stops_at_the_diagnostics_unit() {
    modbus::ModbusTcpServer server(serverConfig(8, 4, 10));
    TEST_ASSERT_EQUAL_UINT8(2, server.servedDevices());  // 8, 9; 10 is the diagnostics unit
    TEST_ASSERT_EQUAL_UINT8(0, server.unitIdFor(2));
}

// One map per served device -- not one per possible device. 900 registers is 1.8 KB apiece, so
// reserving eight on a one-inverter bridge would be 14 KB of heap for nothing.
// The routing itself, not just the arithmetic. Until readUnit() existed, "unit base+i returns
// device i" lived in a worker inside #if defined(ESP32) -- so the one thing this feature is
// about could only ever be checked by pointing a real Modbus client at real hardware (review).
static void test_each_unit_id_reads_its_own_devices_registers() {
    modbus::ModbusTcpServer server(serverConfig(1, 3));
    Rig                     r;
    DeviceState             a = r.poll();
    DeviceState             b = a;
    DeviceState             c = a;
    // Distinct AC power per device, so a wrong map is visible rather than plausible.
    b.measurements.set(measurement_id::kAcPowerTotal, 100.0, g_now);
    c.measurements.set(measurement_id::kAcPowerTotal, 200.0, g_now);
    server.refresh({&a, &b, &c}, makeBridge(), r.diagnostics.snapshot(), g_now);

    const auto watts = [&server](uint8_t unit) {
        uint16_t raw[2] = {0, 0};
        TEST_ASSERT_TRUE(server.readUnit(unit, modbus::reg::kAcPowerTotal, 2, raw));
        const uint32_t bits = (static_cast<uint32_t>(raw[0]) << 16) | raw[1];
        float          f    = 0.0f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    };
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 1842.0f, watts(1));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 100.0f, watts(2));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 200.0f, watts(3));
    // The diagnostics unit reads device 1, as it always has.
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 1842.0f, watts(250));
}

// A client with no unit-id field of its own sends 0 or 255. Both read device 1, so a
// configuration copied from a single-inverter example works instead of erroring.
static void test_unit_zero_and_255_read_the_first_device() {
    modbus::ModbusTcpServer server(serverConfig(1, 2));
    TEST_ASSERT_EQUAL_UINT32(0, server.mapIndexFor(0));
    TEST_ASSERT_EQUAL_UINT32(0, server.mapIndexFor(255));
    TEST_ASSERT_EQUAL_UINT32(0, server.mapIndexFor(250));  // diagnostics, unchanged
    TEST_ASSERT_EQUAL_UINT32(1, server.mapIndexFor(2));
}

// A unit below the base has no map at all rather than falling back to device 1 -- silently
// serving inverter 1 to a client that asked for something else is the failure to avoid.
static void test_a_unit_below_the_base_has_no_map() {
    modbus::ModbusTcpServer server(serverConfig(10, 2));
    TEST_ASSERT_EQUAL_UINT32(modbus::ModbusTcpServer::kNoMap, server.mapIndexFor(9));
    uint16_t raw[2] = {0, 0};
    TEST_ASSERT_FALSE(server.readUnit(9, modbus::reg::kAcPowerTotal, 2, raw));
    TEST_ASSERT_FALSE(server.readUnit(12, modbus::reg::kAcPowerTotal, 2, raw));  // past the run
}

// A configured device that did not start keeps its unit id, and that unit reports nothing
// rather than the next inverter's readings. This is the whole reason the ids are keyed on the
// configuration: skipping it would move every later inverter down one.
static void test_a_slot_with_no_device_answers_with_no_readings() {
    modbus::ModbusTcpServer server(serverConfig(1, 3));
    Rig                     r;
    const DeviceState       a = r.poll();
    DeviceState             c = a;
    c.measurements.set(measurement_id::kAcPowerTotal, 200.0, g_now);
    // Device 2 did not start.
    server.refresh({&a, nullptr, &c}, makeBridge(), r.diagnostics.snapshot(), g_now);

    uint16_t raw[2] = {0, 0};
    TEST_ASSERT_TRUE(server.readUnit(2, modbus::reg::kAcPowerTotal, 2, raw));
    const uint32_t bits = (static_cast<uint32_t>(raw[0]) << 16) | raw[1];
    float          f    = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    TEST_ASSERT_TRUE_MESSAGE(std::isnan(f), "an unstarted slot must not serve a reading");
    TEST_ASSERT_TRUE(server.readUnit(3, modbus::reg::kAcPowerTotal, 2, raw));
    const uint32_t bits3 = (static_cast<uint32_t>(raw[0]) << 16) | raw[1];
    std::memcpy(&f, &bits3, sizeof(f));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 200.0f, f);  // device 3 did NOT move down to unit 2
}

static void test_maps_are_allocated_per_served_device() {
    modbus::ModbusTcpServer three(serverConfig(1, 3));
    TEST_ASSERT_EQUAL_UINT32(3, three.registerMapCount());
    modbus::ModbusTcpServer one(serverConfig(1, 1));
    TEST_ASSERT_EQUAL_UINT32(1, one.registerMapCount());
}

// ...but never zero. The diagnostics unit reads map 0, and a bridge with nothing polling is
// exactly when someone scrapes it for uptime and heap. Before the per-device maps a single map
// always existed, so refusing that read would be a quiet regression.
static void test_the_diagnostics_unit_keeps_a_map_when_no_inverter_started() {
    modbus::ModbusTcpServer server(serverConfig(1, 0));
    TEST_ASSERT_EQUAL_UINT8(0, server.servedDevices());
    TEST_ASSERT_EQUAL_UINT32(1, server.registerMapCount());
}

// Re-applying a configuration replaces the maps rather than accumulating them.
static void test_reconfiguring_does_not_accumulate_maps() {
    modbus::ModbusTcpServer server(serverConfig(1, 3));
    TEST_ASSERT_TRUE(server.setConfig(serverConfig(1, 2)));
    TEST_ASSERT_EQUAL_UINT32(2, server.registerMapCount());
    TEST_ASSERT_EQUAL_UINT8(2, server.servedDevices());
}

static void test_the_run_stops_at_the_last_valid_slave_address() {
    // 247 is the last valid Modbus slave address; 248 and up are reserved.
    modbus::ModbusTcpServer server(serverConfig(246, 4, 10));
    TEST_ASSERT_EQUAL_UINT8(2, server.servedDevices());  // 246, 247
}

static void test_prometheus_build_info_carries_the_version() {
    Rig        r;
    const auto state = r.poll();
    const auto text  = metricsOf(state, makeBridge(), r.diagnostics.snapshot());
    TEST_ASSERT_TRUE(text.find("heliograph_build_info{device=\"eversolar_legacy\",version=\"0.1.0\",driver=\"eversolar_legacy\"") !=
                     std::string::npos);
}

static void test_the_mock_hybrid_also_exports() {
    mock::MockDriver driver(clockFn, mock::MockOptions{});
    StateStore       store;
    Diagnostics      diag;
    g_now = 12ULL * 60 * 60 * 1000;
    DeviceContext ctx(driver, store, diag, clockFn);
    ctx.pollOnce();

    const auto text = metricsOf(*store.snapshot(), makeBridge(), diag.snapshot());
    TEST_ASSERT_TRUE(text.find("heliograph_inverter_ac_power_watts") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("heliograph_build_info{device=\"eversolar_legacy\",version=\"0.1.0\",driver=\"mock_inverter\"") !=
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
    RUN_TEST(test_the_status_payload_totals_every_polled_device);
    RUN_TEST(test_a_device_that_reports_nothing_does_not_count_towards_a_total);
    RUN_TEST(test_a_stale_reading_leaves_the_total_and_the_device_is_not_answering);
    RUN_TEST(test_the_night_state_reports_no_production);
    RUN_TEST(test_totals_over_no_devices_are_null_not_zero);
    RUN_TEST(test_a_full_bus_of_summaries_still_fits);
    RUN_TEST(test_the_status_payload_reports_devices_that_did_not_start);
    RUN_TEST(test_the_problem_list_is_always_present);
    RUN_TEST(test_a_device_payload_says_when_it_last_answered);
    RUN_TEST(test_a_stored_out_of_range_option_is_healed_not_fatal);
    RUN_TEST(test_an_asserted_out_of_range_option_is_left_for_the_caller);
    RUN_TEST(test_a_stored_out_of_range_extra_device_option_is_healed);
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
    RUN_TEST(test_drivers_payload_drives_the_wizard);
    RUN_TEST(test_diagnostics_payload_has_no_secrets);
    RUN_TEST(test_diagnostics_report_stack_marks_and_fragmentation);
    RUN_TEST(test_psram_is_reported_when_the_board_has_it);
    RUN_TEST(test_psram_is_null_on_a_board_without_it);
    RUN_TEST(test_prometheus_reports_psram_only_when_present);
    RUN_TEST(test_mqtt_publish_failures_are_counted_and_published);
    RUN_TEST(test_prometheus_exports_the_publish_failure_counter);
    RUN_TEST(test_coredump_is_reported_when_one_is_stored);
    RUN_TEST(test_coredump_details_are_null_when_none_is_stored);
    RUN_TEST(test_a_nameless_coredump_still_reports_present);
    RUN_TEST(test_prometheus_always_reports_the_coredump_flag);
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
    RUN_TEST(test_every_inverter_series_carries_its_device_label);
    RUN_TEST(test_help_and_type_appear_once_per_family_across_devices);
    RUN_TEST(test_a_family_no_device_reports_has_no_header);
    RUN_TEST(test_each_device_gets_the_next_unit_id);
    RUN_TEST(test_one_device_is_unchanged_at_the_configured_unit_id);
    RUN_TEST(test_the_run_stops_at_the_diagnostics_unit);
    RUN_TEST(test_the_run_stops_at_the_last_valid_slave_address);
    RUN_TEST(test_each_unit_id_reads_its_own_devices_registers);
    RUN_TEST(test_unit_zero_and_255_read_the_first_device);
    RUN_TEST(test_a_unit_below_the_base_has_no_map);
    RUN_TEST(test_a_slot_with_no_device_answers_with_no_readings);
    RUN_TEST(test_maps_are_allocated_per_served_device);
    RUN_TEST(test_the_diagnostics_unit_keeps_a_map_when_no_inverter_started);
    RUN_TEST(test_reconfiguring_does_not_accumulate_maps);
    RUN_TEST(test_prometheus_build_info_carries_the_version);
    RUN_TEST(test_the_mock_hybrid_also_exports);
    RUN_TEST(test_status_payload_reports_clock_sync_state);
    RUN_TEST(test_capture_payload_renders_hex_the_way_the_docs_do);
    RUN_TEST(test_capture_payload_echoes_the_line_it_listened_at);
    RUN_TEST(test_a_failed_capture_carries_its_reason);
    RUN_TEST(test_a_capture_filled_to_its_byte_ceiling_fits_in_the_response);
    RUN_TEST(test_restore_preview_carries_the_file_and_the_changes);
    RUN_TEST(test_restore_preview_omits_an_absent_export_date);
    RUN_TEST(test_restore_preview_of_a_real_backup_leaks_no_password);
    RUN_TEST(test_restore_result_reports_a_missing_undo_separately);
    return UNITY_END();
}
