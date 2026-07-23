// SPDX-License-Identifier: MIT
// MQTT payloads, Home Assistant discovery and publish throttling.

#include <unity.h>

#include <ArduinoJson.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "device/device_context.h"
#include "drivers/eversolar_legacy/eversolar_driver.h"
#include "drivers/mock/mock_driver.h"
#include "outputs/mqtt/home_assistant_discovery.h"
#include "outputs/mqtt/mqtt_payloads.h"
#include "outputs/mqtt/mqtt_topics.h"
#include "outputs/mqtt/publish_policy.h"
#include "state/state_store.h"
#include "support/fake_eversolar_device.h"
#include "support/mock_transport.h"

using namespace heliograph;
using namespace heliograph::mqtt;
namespace fx = heliograph::fixtures;
using test::FakeEversolarDevice;
using test::MockTransport;

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

/// The real EverSolar driver against the simulated inverter.
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
    const auto   err = deserializeJson(doc, json);
    TEST_ASSERT_EQUAL_MESSAGE(DeserializationError::Ok, err.code(), "payload is not valid JSON");
    return doc;
}

static const DiscoveryEntity* findEntity(const std::vector<DiscoveryEntity>& v,
                                         const std::string&                  uniqueId) {
    for (const auto& e : v) {
        if (e.uniqueId == uniqueId) {
            return &e;
        }
    }
    return nullptr;
}

// --- topics -----------------------------------------------------------------------------

static void test_topics_are_built_consistently() {
    const MqttTopics t(kDefaultBaseTopic, "heliograph-a1b2c3");
    TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/availability", t.availability().c_str());
    TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/state", t.state().c_str());
    TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/diagnostics", t.diagnostics().c_str());
    TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/identity", t.identity().c_str());
    TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/capabilities", t.capabilities().c_str());
}

// --- state payload ------------------------------------------------------------------------

static void test_state_payload_is_valid_json_with_the_expected_values() {
    Rig r;
    const auto  state = r.poll();
    std::string json;
    TEST_ASSERT_TRUE(buildStatePayload(state, json));

    auto doc = parse(json);
    TEST_ASSERT_TRUE(doc["bridge_online"].as<bool>());
    TEST_ASSERT_TRUE(doc["inverter_online"].as<bool>());
    TEST_ASSERT_TRUE(doc["data_valid"].as<bool>());
    TEST_ASSERT_FALSE(doc["data_stale"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("eversolar_legacy", doc["driver_id"]);
    TEST_ASSERT_EQUAL_STRING("Ever-Solar", doc["manufacturer"]);
    TEST_ASSERT_EQUAL_STRING(fx::kExpectedSerial, doc["serial_number"]);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, fx::expected::kAcPowerW,
                              doc["measurements"]["ac.power.total"]["value"].as<double>());
    TEST_ASSERT_DOUBLE_WITHIN(0.01, fx::expected::kEnergyTotalKwh,
                              doc["measurements"]["energy.total"]["value"].as<double>());
    TEST_ASSERT_EQUAL_STRING("W", doc["measurements"]["ac.power.total"]["unit"]);
}

static void test_unsupported_measurements_are_absent_not_null() {
    // The TL3000-20 has no L2 and no battery. Those keys must not exist at all: a null would
    // imply "this device has it but we do not know the value".
    Rig r;
    const auto  state = r.poll();
    std::string json;
    buildStatePayload(state, json);
    auto doc = parse(json);

    TEST_ASSERT_TRUE(doc["measurements"]["ac.phase_l2.voltage"].isNull());
    TEST_ASSERT_FALSE(doc["measurements"].as<JsonObject>()["ac.phase_l2.voltage"].is<JsonObject>());
    TEST_ASSERT_FALSE(doc["measurements"].as<JsonObject>()["battery.soc"].is<JsonObject>());
}

static void test_an_unsupported_declared_channel_is_absent_from_the_payload() {
    // Distinct from "omitted entirely": the channel is in the schema but unreadable. Every
    // output must treat both the same way -- publish nothing.
    Rig  r;
    auto state = r.poll();
    state.measurements.declareUnsupported("battery.soc", MeasurementType::Ratio, Unit::Percent,
                                          "Battery SoC");
    std::string json;
    TEST_ASSERT_TRUE(buildStatePayload(state, json));
    TEST_ASSERT_TRUE(json.find("battery.soc") == std::string::npos);
}

static void test_no_discovery_entity_for_an_unsupported_declared_channel() {
    Rig  r;
    auto state = r.poll();
    state.measurements.declareUnsupported("battery.soc", MeasurementType::Ratio, Unit::Percent,
                                          "Battery SoC");
    const auto bridge = makeBridge();
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities = buildDiscoveryEntities(state, bridge, topics, kDefaultDiscoveryPrefix);

    TEST_ASSERT_NULL(findEntity(entities, "heliograph-a1b2c3_battery_soc"));
}

static void test_discovery_signature_changes_when_the_set_changes_at_equal_size() {
    // The re-announce decision in MqttOutput must not compare measurement counts: a driver
    // that swaps one channel for another at the same total would silently leave stale
    // entity definitions in Home Assistant.
    DeviceState a;
    a.measurements.declare("ac.power.total", MeasurementType::Power, Unit::Watt, "AC power");
    a.measurements.declare("energy.total", MeasurementType::Energy, Unit::KilowattHour,
                           "Total yield");

    DeviceState b;
    b.measurements.declare("ac.power.total", MeasurementType::Power, Unit::Watt, "AC power");
    b.measurements.declare("energy.today", MeasurementType::Energy, Unit::KilowattHour,
                           "Yield today");

    TEST_ASSERT_EQUAL_size_t(a.measurements.size(), b.measurements.size());
    TEST_ASSERT_TRUE(discoverySignature(a) != discoverySignature(b));
}

static void test_discovery_signature_ignores_unsupported_channels() {
    // Unsupported channels produce no discovery entity, so declaring one must not force a
    // discovery republish.
    DeviceState a;
    a.measurements.declare("ac.power.total", MeasurementType::Power, Unit::Watt, "AC power");

    DeviceState b;
    b.measurements.declare("ac.power.total", MeasurementType::Power, Unit::Watt, "AC power");
    b.measurements.declareUnsupported("battery.soc", MeasurementType::Ratio, Unit::Percent,
                                      "Battery SoC");

    TEST_ASSERT_TRUE(discoverySignature(a) == discoverySignature(b));
}

static void test_error_code_is_null_when_the_protocol_has_none() {
    Rig r;
    const auto  state = r.poll();
    std::string json;
    buildStatePayload(state, json);
    auto doc = parse(json);

    // Not 0: that would mean "no fault", which this protocol cannot tell us.
    TEST_ASSERT_TRUE(doc["error_code"].isNull());
    TEST_ASSERT_TRUE(json.find("\"error_code\":null") != std::string::npos);
}

static void test_status_text_is_not_invented() {
    // The payload carries whatever text the driver established. Code 1 earned a measured
    // meaning on 2026-07-19 (grid-tied production observed live, corroborated by the
    // ha-zeversolar-modbus calibration); the payload builder itself still never invents --
    // it publishes the driver's text verbatim next to the raw code.
    Rig r;
    const auto  state = r.poll();
    std::string json;
    buildStatePayload(state, json);
    auto doc = parse(json);

    TEST_ASSERT_EQUAL_INT(1, doc["status_code"].as<int>());
    TEST_ASSERT_EQUAL_STRING("Grid-connected (normal)", doc["status_text"]);
}

static void test_an_empty_status_text_is_null_not_an_empty_string() {
    // A driver whose protocol has no status text (the Modbus bring-up case) leaves it empty.
    // Publishing "" would render as a blank tile in every consumer; absent-as-null is the
    // same rule identity fields follow.
    DeviceState state;
    state.inverterOnline = true;
    state.dataValid      = true;
    state.dataStale      = false;
    state.statusCode     = 3;
    state.statusText     = "";

    std::string json;
    TEST_ASSERT_TRUE(buildStatePayload(state, json));
    const auto doc = parse(json);
    TEST_ASSERT_TRUE(doc["status_text"].isNull());
    TEST_ASSERT_EQUAL(3, doc["status_code"].as<int>());
}

static void test_stale_measurements_are_published_as_null() {
    // The night. The value survives internally but must not be presented as a reading.
    Rig r;
    DeviceContext ctx(r.driver, r.store, r.diagnostics, clockFn);
    ctx.pollOnce();
    r.device.offline = true;
    for (int i = 0; i < 10; ++i) {
        g_now += 10000;
        ctx.pollOnce();
    }

    std::string json;
    TEST_ASSERT_TRUE(buildStatePayload(*r.store.snapshot(), json));
    auto doc = parse(json);

    TEST_ASSERT_FALSE(doc["inverter_online"].as<bool>());
    TEST_ASSERT_TRUE(doc["measurements"]["ac.power.total"]["value"].isNull());
    TEST_ASSERT_TRUE(doc["measurements"]["ac.power.total"]["stale"].as<bool>());
    // The key must still exist -- the channel is supported, just not currently known.
    TEST_ASSERT_TRUE(doc["measurements"]["ac.power.total"].is<JsonObject>());
}

static void test_a_genuine_zero_is_published_as_zero() {
    Rig r;
    r.device.payload = FakeEversolarDevice::Payload::Night;
    const auto  state = r.poll();
    std::string json;
    buildStatePayload(state, json);
    auto doc = parse(json);

    TEST_ASSERT_FALSE(doc["measurements"]["ac.power.total"]["value"].isNull());
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, doc["measurements"]["ac.power.total"]["value"].as<double>());
}

static void test_derived_measurements_are_flagged() {
    Rig r;
    const auto  state = r.poll();
    std::string json;
    buildStatePayload(state, json);
    auto doc = parse(json);

    TEST_ASSERT_TRUE(doc["measurements"]["dc.power.total"]["derived"].as<bool>());
    TEST_ASSERT_TRUE(doc["measurements"]["ac.power.total"]["derived"].isNull());
}

static void test_oversized_payload_is_refused_rather_than_truncated() {
    // Truncated JSON is indistinguishable from corrupt data downstream.
    Rig r;
    const auto  state = r.poll();
    std::string json  = "untouched";
    TEST_ASSERT_FALSE(buildStatePayload(state, json, 50));
    TEST_ASSERT_EQUAL_STRING("untouched", json.c_str());
}

static void test_state_payload_stays_well_within_the_bound() {
    Rig r;
    const auto  state = r.poll();
    std::string json;
    TEST_ASSERT_TRUE(buildStatePayload(state, json));
    TEST_ASSERT_TRUE(json.size() < kMaxPayloadBytes);
}

static void test_the_mock_hybrid_payload_also_fits() {
    // Three phases, two MPPTs and a battery is the largest realistic payload.
    mock::MockDriver driver(clockFn, mock::MockOptions{});
    StateStore       store;
    Diagnostics      diag;
    g_now = 12ULL * 60 * 60 * 1000;
    DeviceContext ctx(driver, store, diag, clockFn);
    ctx.pollOnce();

    std::string json;
    TEST_ASSERT_TRUE(buildStatePayload(*store.snapshot(), json));
    TEST_ASSERT_TRUE(json.size() < kMaxPayloadBytes);
    auto doc = parse(json);
    TEST_ASSERT_FALSE(doc["measurements"]["battery.soc"]["value"].isNull());
}

// --- diagnostics / identity / capabilities -----------------------------------------------

static void test_diagnostics_payload() {
    Rig r;
    r.poll();
    std::string json;
    TEST_ASSERT_TRUE(buildDiagnosticsPayload(r.diagnostics.snapshot(), makeBridge(), json));
    auto doc = parse(json);

    TEST_ASSERT_EQUAL_UINT32(86400, doc["uptime_seconds"].as<uint32_t>());
    TEST_ASSERT_EQUAL_INT(-57, doc["wifi_rssi_dbm"].as<int>());
    TEST_ASSERT_EQUAL_UINT32(1, doc["poll_success_total"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING("0.1.0", doc["firmware_version"]);
}

static void test_rssi_is_null_when_wifi_is_down() {
    // 0 dBm would read as a perfect signal.
    auto bridge          = makeBridge();
    bridge.wifiConnected = false;
    std::string json;
    buildDiagnosticsPayload(DiagnosticsSnapshot{}, bridge, json);
    TEST_ASSERT_TRUE(parse(json)["wifi_rssi_dbm"].isNull());
}

static void test_diagnostics_report_stack_marks_and_fragmentation() {
    Rig r;
    r.diagnostics.recordRs485StackFree(2500);
    r.diagnostics.recordLoopStackFree(4100);
    auto bridge              = makeBridge();
    bridge.maxAllocHeapBytes = 65536;
    std::string json;
    TEST_ASSERT_TRUE(buildDiagnosticsPayload(r.diagnostics.snapshot(), bridge, json));
    auto doc = parse(json);
    TEST_ASSERT_EQUAL_UINT32(2500, doc["rs485_stack_free_bytes"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(4100, doc["loop_stack_free_bytes"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(65536, doc["max_alloc_heap_bytes"].as<uint32_t>());

    // Before the first sample the honest answer is "unknown", not an alarming 0.
    buildDiagnosticsPayload(DiagnosticsSnapshot{}, bridge, json);
    doc = parse(json);
    TEST_ASSERT_TRUE(doc["rs485_stack_free_bytes"].isNull());
    TEST_ASSERT_TRUE(doc["loop_stack_free_bytes"].isNull());
}

static void test_diagnostics_never_contain_a_secret() {
    // The last_error string is fed only from pollResultName() and friends.
    Rig r;
    DeviceContext ctx(r.driver, r.store, r.diagnostics, clockFn);
    ctx.pollOnce();
    r.device.offline = true;
    g_now += 10000;
    ctx.pollOnce();

    std::string json;
    buildDiagnosticsPayload(r.diagnostics.snapshot(), makeBridge(), json);
    TEST_ASSERT_EQUAL_STRING("poll failed: timeout", parse(json)["last_error"]);
    TEST_ASSERT_TRUE(json.find("password") == std::string::npos);
    TEST_ASSERT_TRUE(json.find("ssid") == std::string::npos);
}

static void test_identity_omits_unknown_fields() {
    Rig r;
    const auto  state = r.poll();
    std::string json;
    TEST_ASSERT_TRUE(buildIdentityPayload(state.identity, json));
    auto doc = parse(json);

    TEST_ASSERT_EQUAL_STRING("Ever-Solar", doc["manufacturer"]);
    // The protocol reports no firmware or hardware version, so the keys must be absent
    // rather than empty strings.
    TEST_ASSERT_FALSE(doc["firmware_version"].is<const char*>());
    TEST_ASSERT_FALSE(doc["hardware_version"].is<const char*>());
    TEST_ASSERT_EQUAL_STRING("eversolar_legacy", doc["driver_id"]);
}

static void test_capabilities_payload_reports_read_only() {
    Rig r;
    const auto  state = r.poll();
    std::string json;
    TEST_ASSERT_TRUE(buildCapabilitiesPayload(state.capabilities, json));
    auto doc = parse(json);

    TEST_ASSERT_TRUE(doc["read_only"].as<bool>());
    TEST_ASSERT_EQUAL_size_t(0, doc["write"].as<JsonArray>().size());
    TEST_ASSERT_TRUE(doc["read"].as<JsonArray>().size() > 5);
    TEST_ASSERT_EQUAL_INT(1, doc["phase_count"].as<int>());
    TEST_ASSERT_FALSE(doc["has_battery"].as<bool>());
}

static void test_writable_driver_lists_its_bounds() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver driver(clockFn, o);
    std::string      json;
    TEST_ASSERT_TRUE(buildCapabilitiesPayload(driver.capabilities(), json));
    auto doc = parse(json);

    TEST_ASSERT_FALSE(doc["read_only"].as<bool>());
    TEST_ASSERT_TRUE(doc["write"].as<JsonArray>().size() > 0);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 100.0,
                              doc["numeric"]["set_active_power_limit_percent"]["maximum"].as<double>());
}

// --- Home Assistant discovery --------------------------------------------------------------

static void test_discovery_creates_an_entity_per_supported_measurement() {
    Rig        r;
    const auto state  = r.poll();
    const auto bridge = makeBridge();
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities = buildDiscoveryEntities(state, bridge, topics, kDefaultDiscoveryPrefix);

    TEST_ASSERT_NOT_NULL(findEntity(entities, "heliograph-a1b2c3_ac_power_total"));
    TEST_ASSERT_NOT_NULL(findEntity(entities, "heliograph-a1b2c3_energy_total"));
    TEST_ASSERT_NOT_NULL(findEntity(entities, "heliograph-a1b2c3_inverter_temperature"));
    // Nothing this driver does not support.
    TEST_ASSERT_NULL(findEntity(entities, "heliograph-a1b2c3_battery_soc"));
    TEST_ASSERT_NULL(findEntity(entities, "heliograph-a1b2c3_ac_phase_l2_voltage"));
}

static void test_discovery_metadata_matches_the_measurement_type() {
    Rig        r;
    const auto state  = r.poll();
    const auto bridge = makeBridge();
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities = buildDiscoveryEntities(state, bridge, topics, kDefaultDiscoveryPrefix);

    auto power = parse(findEntity(entities, "heliograph-a1b2c3_ac_power_total")->payload);
    TEST_ASSERT_EQUAL_STRING("power", power["device_class"]);
    TEST_ASSERT_EQUAL_STRING("measurement", power["state_class"]);
    TEST_ASSERT_EQUAL_STRING("W", power["unit_of_measurement"]);

    // Energy must be total_increasing or it never reaches the energy dashboard.
    auto energy = parse(findEntity(entities, "heliograph-a1b2c3_energy_total")->payload);
    TEST_ASSERT_EQUAL_STRING("energy", energy["device_class"]);
    TEST_ASSERT_EQUAL_STRING("total_increasing", energy["state_class"]);
    TEST_ASSERT_EQUAL_STRING("kWh", energy["unit_of_measurement"]);

    auto temp = parse(findEntity(entities, "heliograph-a1b2c3_inverter_temperature")->payload);
    TEST_ASSERT_EQUAL_STRING("temperature", temp["device_class"]);
    TEST_ASSERT_EQUAL_STRING("°C", temp["unit_of_measurement"]);

    auto freq = parse(findEntity(entities, "heliograph-a1b2c3_ac_frequency")->payload);
    TEST_ASSERT_EQUAL_STRING("frequency", freq["device_class"]);
    TEST_ASSERT_EQUAL_STRING("Hz", freq["unit_of_measurement"]);
}

static void test_value_template_reads_the_right_key() {
    Rig        r;
    const auto state  = r.poll();
    const auto bridge = makeBridge();
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities = buildDiscoveryEntities(state, bridge, topics, kDefaultDiscoveryPrefix);
    auto doc = parse(findEntity(entities, "heliograph-a1b2c3_ac_power_total")->payload);

    TEST_ASSERT_EQUAL_STRING("{{ value_json.measurements['ac.power.total'].value }}",
                             doc["value_template"]);
    TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/state", doc["state_topic"]);
}

static void test_availability_tracks_the_bridge_not_the_inverter() {
    // If it tracked the inverter, every night would blank the entities instead of recording
    // an honest "unknown", and the history would be full of gaps.
    Rig        r;
    const auto state  = r.poll();
    const auto bridge = makeBridge();
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities = buildDiscoveryEntities(state, bridge, topics, kDefaultDiscoveryPrefix);
    auto doc = parse(findEntity(entities, "heliograph-a1b2c3_ac_power_total")->payload);

    TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/availability", doc["availability_topic"]);
    TEST_ASSERT_EQUAL_STRING("online", doc["payload_available"]);
    TEST_ASSERT_EQUAL_STRING("offline", doc["payload_not_available"]);
}

static void test_inverter_is_a_separate_device_behind_the_bridge() {
    Rig        r;
    const auto state  = r.poll();
    const auto bridge = makeBridge();
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities = buildDiscoveryEntities(state, bridge, topics, kDefaultDiscoveryPrefix);
    auto doc = parse(findEntity(entities, "heliograph-a1b2c3_ac_power_total")->payload);

    TEST_ASSERT_EQUAL_STRING("heliograph-a1b2c3_inverter", doc["device"]["identifiers"][0]);
    TEST_ASSERT_EQUAL_STRING("heliograph-a1b2c3", doc["device"]["via_device"]);
    TEST_ASSERT_EQUAL_STRING("Ever-Solar", doc["device"]["manufacturer"]);
    TEST_ASSERT_EQUAL_STRING(fx::kExpectedSerial, doc["device"]["serial_number"]);
}

static void test_the_inverter_device_is_named_after_its_model_not_its_manufacturer() {
    Rig  r;
    const auto state = r.poll();
    auto bridge      = makeBridge();
    bridge.name      = "Heliograph";
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities = buildDiscoveryEntities(state, bridge, topics, kDefaultDiscoveryPrefix);
    auto doc = parse(findEntity(entities, "heliograph-a1b2c3_ac_power_total")->payload);

    // The manufacturer is already its own field. Repeating it in the name gave Home Assistant
    // "Heliograph - Heliograph open-source project" on real hardware.
    const std::string name = doc["device"]["name"].as<std::string>();
    TEST_ASSERT_TRUE(name.find(state.identity.model) != std::string::npos);
    TEST_ASSERT_TRUE(name.find("Heliograph - ") == 0);
    TEST_ASSERT_TRUE(name.find(state.identity.manufacturer) == std::string::npos);
}

static void test_an_inverter_without_a_model_still_gets_a_usable_name() {
    Rig  r;
    auto state = r.poll();
    state.identity.model.clear();
    auto bridge = makeBridge();
    bridge.name = "Heliograph";
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities = buildDiscoveryEntities(state, bridge, topics, kDefaultDiscoveryPrefix);
    auto doc = parse(findEntity(entities, "heliograph-a1b2c3_ac_power_total")->payload);

    // Falling back to the manufacturer is the one case where the repetition is acceptable:
    // a nameless device is worse than a redundant one.
    TEST_ASSERT_EQUAL_STRING("Heliograph - Ever-Solar", doc["device"]["name"]);
}

static void test_every_entity_on_a_device_has_a_distinct_display_name() {
    // The mock hybrid on purpose: three phases and two MPPTs is the layout that produces
    // repeated names, and it is the one the EverSolar single-phase unit can never exercise.
    mock::MockDriver driver(clockFn, mock::MockOptions{});
    StateStore       store;
    Diagnostics      diag;
    g_now = 12ULL * 60 * 60 * 1000;
    DeviceContext ctx(driver, store, diag, clockFn);
    ctx.pollOnce();

    const auto bridge = makeBridge();
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities =
        buildDiscoveryEntities(*store.snapshot(), bridge, topics, kDefaultDiscoveryPrefix);

    // Home Assistant derives the entity id from the display name and silently disambiguates
    // duplicates with _2/_3 -- a suffix that carries no meaning and reorders itself whenever a
    // channel drops out. Distinctness here is what keeps entity ids stable.
    std::map<std::string, std::vector<std::string>> namesPerDevice;
    for (const auto& e : entities) {
        auto doc = parse(e.payload);
        namesPerDevice[doc["device"]["identifiers"][0].as<std::string>()].push_back(
            doc["name"].as<std::string>());
    }
    for (auto& [device, names] : namesPerDevice) {
        std::vector<std::string> unique = names;
        std::sort(unique.begin(), unique.end());
        TEST_ASSERT_TRUE(std::adjacent_find(unique.begin(), unique.end()) == unique.end());
    }
}

static void test_no_control_entities_for_a_read_only_driver() {
    Rig        r;
    const auto state  = r.poll();
    const auto bridge = makeBridge();
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities = buildDiscoveryEntities(state, bridge, topics, kDefaultDiscoveryPrefix);

    // Nothing that can be commanded: no number, switch or select component anywhere.
    for (const auto& e : entities) {
        TEST_ASSERT_TRUE(e.configTopic.find("/number/") == std::string::npos);
        TEST_ASSERT_TRUE(e.configTopic.find("/switch/") == std::string::npos);
        TEST_ASSERT_TRUE(e.configTopic.find("/select/") == std::string::npos);
        TEST_ASSERT_TRUE(e.payload.find("command_topic") == std::string::npos);
    }
}

static void test_config_topics_are_well_formed() {
    Rig        r;
    const auto state  = r.poll();
    const auto bridge = makeBridge();
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities = buildDiscoveryEntities(state, bridge, topics, kDefaultDiscoveryPrefix);

    const auto* e = findEntity(entities, "heliograph-a1b2c3_ac_power_total");
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/heliograph-a1b2c3/ac_power_total/config",
                             e->configTopic.c_str());
    // Dots are illegal in a discovery topic segment.
    for (const auto& entity : entities) {
        TEST_ASSERT_TRUE(entity.configTopic.find('.') == std::string::npos);
        TEST_ASSERT_TRUE(entity.uniqueId.find('.') == std::string::npos);
    }
}

static void test_the_mock_hybrid_gets_battery_and_phase_entities_for_free() {
    // The architectural claim, on the discovery side: no code here knows about batteries or
    // three-phase devices, yet both appear.
    mock::MockDriver driver(clockFn, mock::MockOptions{});
    StateStore       store;
    Diagnostics      diag;
    g_now = 12ULL * 60 * 60 * 1000;
    DeviceContext ctx(driver, store, diag, clockFn);
    ctx.pollOnce();

    const auto bridge = makeBridge();
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities =
        buildDiscoveryEntities(*store.snapshot(), bridge, topics, kDefaultDiscoveryPrefix);

    TEST_ASSERT_NOT_NULL(findEntity(entities, "heliograph-a1b2c3_battery_soc"));
    TEST_ASSERT_NOT_NULL(findEntity(entities, "heliograph-a1b2c3_ac_phase_l3_voltage"));
    TEST_ASSERT_NOT_NULL(findEntity(entities, "heliograph-a1b2c3_dc_mppt_2_power"));

    auto soc = parse(findEntity(entities, "heliograph-a1b2c3_battery_soc")->payload);
    TEST_ASSERT_EQUAL_STRING("battery", soc["device_class"]);
    TEST_ASSERT_EQUAL_STRING("%", soc["unit_of_measurement"]);
    // Battery SoC is where Home Assistant has no default precision, so a raw 74.54152672 %
    // reached the dashboard. The bridge states the intent: whole percent.
    TEST_ASSERT_TRUE(soc["suggested_display_precision"].is<int>());
    TEST_ASSERT_EQUAL_INT(0, soc["suggested_display_precision"].as<int>());

    // Current keeps two decimals; a hint of 0 is a real value distinct from "no hint", so
    // check a non-zero case too rather than let 0 pass by default.
    auto current = parse(findEntity(entities, "heliograph-a1b2c3_dc_mppt_1_current")->payload);
    TEST_ASSERT_EQUAL_INT(2, current["suggested_display_precision"].as<int>());
}

static void test_bridge_diagnostic_entities() {
    const auto bridge = makeBridge();
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities = buildBridgeDiagnosticEntities(bridge, topics, kDefaultDiscoveryPrefix);

    const auto* rssi = findEntity(entities, "heliograph-a1b2c3_wifi_rssi");
    TEST_ASSERT_NOT_NULL(rssi);
    auto doc = parse(rssi->payload);
    TEST_ASSERT_EQUAL_STRING("signal_strength", doc["device_class"]);
    TEST_ASSERT_EQUAL_STRING("dBm", doc["unit_of_measurement"]);
    TEST_ASSERT_EQUAL_STRING("diagnostic", doc["entity_category"]);
    TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/diagnostics", doc["state_topic"]);
    // These belong to the bridge device, not the inverter.
    TEST_ASSERT_EQUAL_STRING("heliograph-a1b2c3", doc["device"]["identifiers"][0]);
    TEST_ASSERT_EQUAL_STRING("Waveshare ESP32-S3-RS485-CAN", doc["device"]["model"]);
}

static void test_relay_entities_follow_count_and_enabled() {
    auto bridge = makeBridge();
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);

    // No relay hardware: nothing at all -- not even removal payloads.
    bridge.relayCount = 0;
    TEST_ASSERT_TRUE(buildRelayEntities(bridge, topics, kDefaultDiscoveryPrefix).empty());

    // Relays present but the feature disabled: EMPTY retained payloads on the config
    // topics (switches AND the select), so previously announced entities disappear.
    bridge.relayCount    = 2;
    bridge.relaysEnabled = false;
    auto removed = buildRelayEntities(bridge, topics, kDefaultDiscoveryPrefix);
    TEST_ASSERT_EQUAL_UINT32(3, removed.size());
    TEST_ASSERT_TRUE(removed[0].payload.empty());
    TEST_ASSERT_TRUE(removed[2].payload.empty());
    TEST_ASSERT_EQUAL_STRING("homeassistant/switch/heliograph-a1b2c3/relay_1/config",
                             removed[0].configTopic.c_str());
    TEST_ASSERT_EQUAL_STRING("homeassistant/select/heliograph-a1b2c3/drm_mode/config",
                             removed[2].configTopic.c_str());

    // Enabled without roles: real switches, plain names, and a select REMOVAL (no roles =
    // no modes to offer).
    bridge.relaysEnabled = true;
    bridge.relayMask     = 0b01;
    auto entities = buildRelayEntities(bridge, topics, kDefaultDiscoveryPrefix);
    TEST_ASSERT_EQUAL_UINT32(3, entities.size());
    auto doc = parse(entities[0].payload);
    TEST_ASSERT_EQUAL_STRING("Relay 1", doc["name"]);
    TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/relay/0/set", doc["command_topic"]);
    TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/relay/0/state", doc["state_topic"]);
    TEST_ASSERT_FALSE(doc["optimistic"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("heliograph-a1b2c3", doc["device"]["identifiers"][0]);
    TEST_ASSERT_TRUE(entities[2].payload.empty());
}

static void test_drm_select_and_role_names_follow_the_roles() {
    auto bridge          = makeBridge();
    bridge.relayCount    = 2;
    bridge.relaysEnabled = true;
    bridge.relayRoles    = {"drm0", "none"};
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);

    const auto entities = buildRelayEntities(bridge, topics, kDefaultDiscoveryPrefix);
    TEST_ASSERT_EQUAL_UINT32(3, entities.size());

    // The role lands in the switch name; the role-less relay keeps the plain name.
    auto sw = parse(entities[0].payload);
    TEST_ASSERT_EQUAL_STRING("Relay 1 (DRM0)", sw["name"]);
    TEST_ASSERT_EQUAL_STRING("Relay 2", parse(entities[1].payload)["name"]);

    // The select offers normal + the configured role, plus "custom" as reportable state.
    auto sel = parse(entities[2].payload);
    TEST_ASSERT_EQUAL_STRING("DRM Mode", sel["name"]);
    TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/drm/set", sel["command_topic"]);
    TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/drm/state", sel["state_topic"]);
    TEST_ASSERT_EQUAL_UINT32(3, sel["options"].size());
    TEST_ASSERT_EQUAL_STRING("normal", sel["options"][0]);
    TEST_ASSERT_EQUAL_STRING("drm0", sel["options"][1]);
    TEST_ASSERT_EQUAL_STRING("custom", sel["options"][2]);
}

static void test_sanitize_id() {
    TEST_ASSERT_EQUAL_STRING("ac_power_total", sanitizeId("ac.power.total").c_str());
    TEST_ASSERT_EQUAL_STRING("dc_mppt_1_voltage", sanitizeId("dc.mppt_1.voltage").c_str());
}

// --- publish throttling ---------------------------------------------------------------------

static void test_first_state_is_always_published() {
    Rig             r;
    const auto      state = r.poll();
    PublishThrottle t;
    TEST_ASSERT_TRUE(t.shouldPublish(state, g_now));
}

static void test_an_unchanged_state_is_not_republished() {
    Rig             r;
    const auto      state = r.poll();
    PublishThrottle t;
    t.recordPublished(state, g_now);
    g_now += 5000;
    TEST_ASSERT_FALSE(t.shouldPublish(state, g_now));
}

static void test_a_change_within_the_deadband_is_ignored() {
    Rig        r;
    auto       state = r.poll();
    PublishThrottle t;
    t.recordPublished(state, g_now);

    // 2 W of drift is noise on a 1842 W reading; the default deadband is 5 W.
    state.measurements.set(measurement_id::kAcPowerTotal, fx::expected::kAcPowerW + 2.0, g_now);
    g_now += 5000;
    TEST_ASSERT_FALSE(t.shouldPublish(state, g_now));
}

static void test_a_change_beyond_the_deadband_publishes() {
    Rig             r;
    auto            state = r.poll();
    PublishThrottle t;
    t.recordPublished(state, g_now);

    state.measurements.set(measurement_id::kAcPowerTotal, fx::expected::kAcPowerW + 50.0, g_now);
    g_now += 5000;
    TEST_ASSERT_TRUE(t.shouldPublish(state, g_now));
}

static void test_energy_publishes_on_any_change() {
    // Energy is a meter reading; a deadband would lose kWh.
    Rig             r;
    auto            state = r.poll();
    PublishThrottle t;
    t.recordPublished(state, g_now);

    state.measurements.set(measurement_id::kEnergyTotal, fx::expected::kEnergyTotalKwh + 0.1, g_now);
    g_now += 1000;
    TEST_ASSERT_TRUE(t.shouldPublish(state, g_now));
}

static void test_going_offline_publishes_immediately() {
    Rig             r;
    auto            state = r.poll();
    PublishThrottle t;
    t.recordPublished(state, g_now);

    state.inverterOnline = false;
    g_now += 100;  // well inside every deadband and the force interval
    TEST_ASSERT_TRUE(t.shouldPublish(state, g_now));
}

static void test_a_status_change_publishes_immediately() {
    Rig             r;
    auto            state = r.poll();
    PublishThrottle t;
    t.recordPublished(state, g_now);

    state.statusCode = 3;
    g_now += 100;
    TEST_ASSERT_TRUE(t.shouldPublish(state, g_now));
}

static void test_forced_refresh_after_the_interval() {
    Rig             r;
    const auto      state = r.poll();
    PublishPolicy   p;
    PublishThrottle t(p);
    t.recordPublished(state, g_now);

    g_now += p.forceIntervalMs - 1;
    TEST_ASSERT_FALSE(t.shouldPublish(state, g_now));
    g_now += 2;
    TEST_ASSERT_TRUE(t.shouldPublish(state, g_now));
}

static void test_a_new_channel_publishes() {
    // e.g. a second MPPT appears once a dual-string payload arrives.
    Rig             r;
    auto            state = r.poll();
    PublishThrottle t;
    t.recordPublished(state, g_now);

    state.measurements.declare(measurement_id::kDcMppt2Voltage, MeasurementType::Voltage,
                               Unit::Volt, "PV2 Voltage");
    g_now += 100;
    TEST_ASSERT_TRUE(t.shouldPublish(state, g_now));
}

static void test_reset_forces_the_next_publish() {
    // On reconnect the broker may have lost our retained messages.
    Rig             r;
    const auto      state = r.poll();
    PublishThrottle t;
    t.recordPublished(state, g_now);
    TEST_ASSERT_FALSE(t.shouldPublish(state, g_now + 1000));
    t.reset();
    TEST_ASSERT_TRUE(t.shouldPublish(state, g_now + 1000));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_topics_are_built_consistently);
    RUN_TEST(test_state_payload_is_valid_json_with_the_expected_values);
    RUN_TEST(test_unsupported_measurements_are_absent_not_null);
    RUN_TEST(test_an_unsupported_declared_channel_is_absent_from_the_payload);
    RUN_TEST(test_no_discovery_entity_for_an_unsupported_declared_channel);
    RUN_TEST(test_discovery_signature_changes_when_the_set_changes_at_equal_size);
    RUN_TEST(test_discovery_signature_ignores_unsupported_channels);
    RUN_TEST(test_error_code_is_null_when_the_protocol_has_none);
    RUN_TEST(test_status_text_is_not_invented);
    RUN_TEST(test_an_empty_status_text_is_null_not_an_empty_string);
    RUN_TEST(test_stale_measurements_are_published_as_null);
    RUN_TEST(test_a_genuine_zero_is_published_as_zero);
    RUN_TEST(test_derived_measurements_are_flagged);
    RUN_TEST(test_oversized_payload_is_refused_rather_than_truncated);
    RUN_TEST(test_state_payload_stays_well_within_the_bound);
    RUN_TEST(test_the_mock_hybrid_payload_also_fits);
    RUN_TEST(test_diagnostics_payload);
    RUN_TEST(test_rssi_is_null_when_wifi_is_down);
    RUN_TEST(test_diagnostics_report_stack_marks_and_fragmentation);
    RUN_TEST(test_diagnostics_never_contain_a_secret);
    RUN_TEST(test_identity_omits_unknown_fields);
    RUN_TEST(test_capabilities_payload_reports_read_only);
    RUN_TEST(test_writable_driver_lists_its_bounds);
    RUN_TEST(test_discovery_creates_an_entity_per_supported_measurement);
    RUN_TEST(test_discovery_metadata_matches_the_measurement_type);
    RUN_TEST(test_value_template_reads_the_right_key);
    RUN_TEST(test_availability_tracks_the_bridge_not_the_inverter);
    RUN_TEST(test_inverter_is_a_separate_device_behind_the_bridge);
    RUN_TEST(test_the_inverter_device_is_named_after_its_model_not_its_manufacturer);
    RUN_TEST(test_an_inverter_without_a_model_still_gets_a_usable_name);
    RUN_TEST(test_every_entity_on_a_device_has_a_distinct_display_name);
    RUN_TEST(test_no_control_entities_for_a_read_only_driver);
    RUN_TEST(test_config_topics_are_well_formed);
    RUN_TEST(test_the_mock_hybrid_gets_battery_and_phase_entities_for_free);
    RUN_TEST(test_bridge_diagnostic_entities);
    RUN_TEST(test_relay_entities_follow_count_and_enabled);
    RUN_TEST(test_drm_select_and_role_names_follow_the_roles);
    RUN_TEST(test_sanitize_id);
    RUN_TEST(test_first_state_is_always_published);
    RUN_TEST(test_an_unchanged_state_is_not_republished);
    RUN_TEST(test_a_change_within_the_deadband_is_ignored);
    RUN_TEST(test_a_change_beyond_the_deadband_publishes);
    RUN_TEST(test_energy_publishes_on_any_change);
    RUN_TEST(test_going_offline_publishes_immediately);
    RUN_TEST(test_a_status_change_publishes_immediately);
    RUN_TEST(test_forced_refresh_after_the_interval);
    RUN_TEST(test_a_new_channel_publishes);
    RUN_TEST(test_reset_forces_the_next_publish);
    return UNITY_END();
}
