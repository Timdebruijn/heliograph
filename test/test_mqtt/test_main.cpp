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
#include "outputs/json_util.h"
#include "outputs/mqtt/announced_devices.h"
#include "outputs/mqtt/home_assistant_discovery.h"
#include "outputs/mqtt/mqtt_output.h"
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
    const auto entities = buildDiscoveryEntities(state, bridge, topics, topics.availability(),
                               kDefaultDiscoveryPrefix, bridge.bridgeId);

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
    state.statusCode          = 3;
    state.statusCodeSupported = true;
    state.statusText          = "";

    std::string json;
    TEST_ASSERT_TRUE(buildStatePayload(state, json));
    const auto doc = parse(json);
    TEST_ASSERT_TRUE(doc["status_text"].isNull());
    TEST_ASSERT_EQUAL(3, doc["status_code"].as<int>());
}

// A driver that never reads a status word leaves statusCode at 0 -- and 0 is a MEANING in most
// protocols ("waiting", "standby"), so publishing the default reports an inverter at full power
// as idle. Exactly the rule errorCodeSupported already carried, applied to its neighbour
// (review, 2026-07-25).
static void test_a_driver_without_a_status_word_publishes_no_status_code() {
    DeviceState state;
    state.inverterOnline = true;
    state.dataValid      = true;
    state.dataStale      = false;
    // statusCodeSupported left false, statusCode left at its 0 default.

    std::string json;
    TEST_ASSERT_TRUE(buildStatePayload(state, json));
    const auto doc = parse(json);
    TEST_ASSERT_TRUE(doc["status_code"].isNull());
    TEST_ASSERT_TRUE(json.find("\"status_code\":0") == std::string::npos);
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

/// The crash cause travels over MQTT; the backtrace deliberately does not.
///
/// Two short fields are what a Home Assistant notification needs to say "the bridge crashed on
/// a null dereference" rather than "the bridge crashed". Sixteen addresses that never change,
/// republished on every diagnostics interval, are payload weight for something nobody reads
/// there -- so the backtrace lives on the REST payload, which is fetched on purpose.
static void test_the_backtrace_stays_off_the_mqtt_payload() {
    Rig  r;
    auto bridge                 = makeBridge();
    bridge.coredumpPresent      = true;
    bridge.coredumpCause        = 29;  // EXCCAUSE_STORE_PROHIBITED
    bridge.coredumpFaultAddress = 0x0000000C;
    bridge.coredumpCauseKnown        = true;
    bridge.coredumpFaultAddressKnown = true;  // StoreProhibited genuinely has one
    bridge.coredumpBacktrace         = {0x42011AF0, 0x42010C34};

    std::string json;
    TEST_ASSERT_TRUE(buildDiagnosticsPayload(r.diagnostics.snapshot(), bridge, json));
    auto doc = parse(json);

    TEST_ASSERT_EQUAL_STRING("StoreProhibited", doc["coredump_cause_name"].as<const char*>());
    TEST_ASSERT_EQUAL_UINT32(0x0000000C, doc["coredump_fault_address"].as<uint32_t>());
    TEST_ASSERT_TRUE(doc["coredump_backtrace"].isNull());
}

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
    TEST_ASSERT_TRUE(json_util::buildCapabilitiesPayload(state.capabilities, json,
                                                     kMaxPayloadBytes));
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
    TEST_ASSERT_TRUE(json_util::buildCapabilitiesPayload(driver.capabilities(), json,
                                                     kMaxPayloadBytes));
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
    const auto entities = buildDiscoveryEntities(state, bridge, topics, topics.availability(),
                               kDefaultDiscoveryPrefix, bridge.bridgeId);

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
    const auto entities = buildDiscoveryEntities(state, bridge, topics, topics.availability(),
                               kDefaultDiscoveryPrefix, bridge.bridgeId);

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
    const auto entities = buildDiscoveryEntities(state, bridge, topics, topics.availability(),
                               kDefaultDiscoveryPrefix, bridge.bridgeId);
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
    const auto entities = buildDiscoveryEntities(state, bridge, topics, topics.availability(),
                               kDefaultDiscoveryPrefix, bridge.bridgeId);
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
    const auto entities = buildDiscoveryEntities(state, bridge, topics, topics.availability(),
                               kDefaultDiscoveryPrefix, bridge.bridgeId);
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
    const auto entities = buildDiscoveryEntities(state, bridge, topics, topics.availability(),
                               kDefaultDiscoveryPrefix, bridge.bridgeId);
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
    const auto entities = buildDiscoveryEntities(state, bridge, topics, topics.availability(),
                               kDefaultDiscoveryPrefix, bridge.bridgeId);
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
        buildDiscoveryEntities(*store.snapshot(), bridge, topics, topics.availability(),
                               kDefaultDiscoveryPrefix, bridge.bridgeId);

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
    const auto entities = buildDiscoveryEntities(state, bridge, topics, topics.availability(),
                               kDefaultDiscoveryPrefix, bridge.bridgeId);

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
    const auto entities = buildDiscoveryEntities(state, bridge, topics, topics.availability(),
                               kDefaultDiscoveryPrefix, bridge.bridgeId);

    const auto* e = findEntity(entities, "heliograph-a1b2c3_ac_power_total");
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/heliograph-a1b2c3/ac_power_total/config",
                             e->configTopic.c_str());
    // Dots are illegal in a discovery topic segment.
    for (const auto& entity : entities) {
        TEST_ASSERT_TRUE(entity.configTopic.find('.') == std::string::npos);
        TEST_ASSERT_TRUE(entity.uniqueId.find('.') == std::string::npos);
    }
}

static DeviceState pollWritableMockState() {
    mock::MockOptions o;
    o.writable = true;
    mock::MockDriver driver(clockFn, o);
    StateStore    store;
    Diagnostics   diag;
    DeviceContext ctx(driver, store, diag, clockFn);
    ctx.pollOnce();
    return *store.snapshot();
}

static void test_a_writable_numeric_command_gets_a_number_entity() {
    const auto state  = pollWritableMockState();
    const auto bridge = makeBridge();
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities = buildDiscoveryEntities(state, bridge, topics, topics.availability(),
                               kDefaultDiscoveryPrefix, bridge.bridgeId);

    const auto* e = findEntity(entities, "heliograph-a1b2c3_set_active_power_limit_percent");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->configTopic.find("/number/") != std::string::npos);

    auto doc = parse(e->payload);
    TEST_ASSERT_EQUAL_STRING(topics.commandSet().c_str(),
                             doc["command_topic"].as<std::string>().c_str());
    const std::string tmpl = doc["command_template"].as<std::string>();
    TEST_ASSERT_TRUE(tmpl.find("\"type\":\"set_active_power_limit_percent\"") !=
                     std::string::npos);
    TEST_ASSERT_TRUE(tmpl.find("{{ value }}") != std::string::npos);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, doc["min"].as<double>());
    TEST_ASSERT_EQUAL_DOUBLE(100.0, doc["max"].as<double>());
    TEST_ASSERT_EQUAL_DOUBLE(1.0, doc["step"].as<double>());
    TEST_ASSERT_EQUAL_STRING("%", doc["unit_of_measurement"]);
    // No readback exists for a setpoint -- no shipping driver reports one as a measurement --
    // so this can only ever be optimistic, unlike the relay switch.
    TEST_ASSERT_TRUE(doc["optimistic"].as<bool>());
}

static void test_start_and_stop_get_button_entities() {
    const auto state  = pollWritableMockState();
    const auto bridge = makeBridge();
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities = buildDiscoveryEntities(state, bridge, topics, topics.availability(),
                               kDefaultDiscoveryPrefix, bridge.bridgeId);

    const auto* start = findEntity(entities, "heliograph-a1b2c3_start");
    TEST_ASSERT_NOT_NULL(start);
    TEST_ASSERT_TRUE(start->configTopic.find("/button/") != std::string::npos);
    auto startDoc = parse(start->payload);
    TEST_ASSERT_EQUAL_STRING(topics.commandSet().c_str(),
                             startDoc["command_topic"].as<std::string>().c_str());
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"start\"}",
                             startDoc["payload_press"].as<std::string>().c_str());

    const auto* stop = findEntity(entities, "heliograph-a1b2c3_stop");
    TEST_ASSERT_NOT_NULL(stop);
    auto stopDoc = parse(stop->payload);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"stop\"}",
                             stopDoc["payload_press"].as<std::string>().c_str());
}

namespace {
/// A mode setpoint with a gap in its numbering, which is what real EMS-mode registers look like.
constexpr EnumOption kTestModes[] = {{0, "Self-consumption"}, {2, "Forced"}, {3, "External EMS"}};

DeviceState modeState(const EnumOption* options, size_t count) {
    DeviceState state;
    state.capabilities.addWrite(InverterCapability::SetBatteryOperatingMode);
    EnumCapability& e = state.capabilities
                            .enums[static_cast<size_t>(InverterCommandType::SetBatteryOperatingMode)];
    e.supported   = options != nullptr;
    e.writable    = options != nullptr;
    e.options     = options;
    e.optionCount = count;
    return state;
}
}  // namespace

// A granted mode write with no modes publishes nothing. Same refusal as a numeric write with no
// range: a select whose dropdown is empty is a control a user can open and not use.
static void test_an_enum_command_without_options_gets_no_entity() {
    const DeviceState state  = modeState(nullptr, 0);
    const auto        bridge = makeBridge();
    const MqttTopics  topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities = buildDiscoveryEntities(state, bridge, topics, topics.availability(),
                               kDefaultDiscoveryPrefix, bridge.bridgeId);

    TEST_ASSERT_NULL(findEntity(entities, "heliograph-a1b2c3_set_battery_operating_mode"));
}

// With modes declared it becomes a select, and the template translates the LABEL Home Assistant
// sends into the mode number the device wants.
static void test_a_mode_command_with_options_becomes_a_select() {
    const DeviceState state  = modeState(kTestModes, 3);
    const auto        bridge = makeBridge();
    const MqttTopics  topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities = buildDiscoveryEntities(state, bridge, topics, topics.availability(),
                               kDefaultDiscoveryPrefix, bridge.bridgeId);

    const auto* entity = findEntity(entities, "heliograph-a1b2c3_set_battery_operating_mode");
    TEST_ASSERT_NOT_NULL(entity);
    TEST_ASSERT_TRUE(entity->configTopic.find("/select/") != std::string::npos);

    JsonDocument doc;
    TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(doc, entity->payload).code());
    TEST_ASSERT_EQUAL_UINT32(3, doc["options"].size());
    TEST_ASSERT_EQUAL_STRING("Self-consumption", doc["options"][0].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("External EMS", doc["options"][2].as<const char*>());
    TEST_ASSERT_TRUE(doc["optimistic"].as<bool>());

    // The mapping must carry each label's OWN value. A template mapping labels to 0,1,2 would
    // look right in the dropdown and select the wrong mode on the device.
    const std::string tmpl = doc["command_template"].as<std::string>();
    TEST_ASSERT_TRUE(tmpl.find("'Forced': 2") != std::string::npos);
    TEST_ASSERT_TRUE(tmpl.find("'External EMS': 3") != std::string::npos);
    TEST_ASSERT_TRUE(tmpl.find("\"enum_value\":{{ m[value] }}") != std::string::npos);
}

// Renaming or renumbering a mode has to re-announce: otherwise the old dropdown survives, and an
// option a user picks now maps to a different register value than the one it was created for.
static void test_discovery_signature_changes_when_a_mode_is_renumbered() {
    static constexpr EnumOption kRenumbered[] = {
        {0, "Self-consumption"}, {5, "Forced"}, {3, "External EMS"}};
    static constexpr EnumOption kRenamed[] = {
        {0, "Self-consumption"}, {2, "Compulsory"}, {3, "External EMS"}};

    // uint64_t, matching what discoverySignature returns. Narrowing this to uint32_t threw away
    // the top half of the hash and then compared it against full 64-bit values, so the
    // assertions below were true whatever the signature did -- a test that could not fail,
    // guarding the one invariant most worth guarding.
    const uint64_t base = discoverySignature(modeState(kTestModes, 3));
    TEST_ASSERT_TRUE(base != discoverySignature(modeState(kRenumbered, 3)));
    TEST_ASSERT_TRUE(base != discoverySignature(modeState(kRenamed, 3)));
    // And dropping one is a change too, not merely a shorter list that hashes the same.
    TEST_ASSERT_TRUE(base != discoverySignature(modeState(kTestModes, 2)));
}

static void test_discovery_signature_changes_when_a_writable_bound_changes() {
    DeviceState a = pollWritableMockState();
    DeviceState b = a;
    b.capabilities.numeric[static_cast<size_t>(InverterCommandType::SetActivePowerLimitPercent)]
        .maximum = 50.0;

    TEST_ASSERT_TRUE(discoverySignature(a) != discoverySignature(b));
}

// The exact scenario a review caught the signature missing (2026-07-30): a NumericCapability
// can be pre-populated with real bounds ahead of hardware verification, with `supported` still
// false -- buildDiscoveryEntities correctly builds no entity for that. The day `supported`
// flips true with the SAME bounds (the verification event itself), the entity set changes from
// "none" to "one number entity", and the signature must change too, or MqttOutput never
// notices there is now something new to announce.
static void test_discovery_signature_changes_when_supported_flips_with_unchanged_bounds() {
    DeviceState a = pollWritableMockState();
    const size_t idx =
        static_cast<size_t>(InverterCommandType::SetActivePowerLimitPercent);
    a.capabilities.numeric[idx].supported = false;

    DeviceState b = a;
    b.capabilities.numeric[idx].supported = true;
    // Bounds deliberately IDENTICAL to a's -- only `supported` differs.

    TEST_ASSERT_TRUE(discoverySignature(a) != discoverySignature(b));
}

// The mirror image: writable=false must behave the same way, and a command with no numeric
// value at all (start/stop) must still be able to change the signature via just the write
// bitset, independent of the numeric-specific gate above.
static void test_discovery_signature_changes_when_writable_flips_with_unchanged_bounds() {
    DeviceState a = pollWritableMockState();
    const size_t idx =
        static_cast<size_t>(InverterCommandType::SetActivePowerLimitPercent);
    a.capabilities.numeric[idx].writable = false;

    DeviceState b = a;
    b.capabilities.numeric[idx].writable = true;

    TEST_ASSERT_TRUE(discoverySignature(a) != discoverySignature(b));
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
        buildDiscoveryEntities(*store.snapshot(), bridge, topics, topics.availability(),
                               kDefaultDiscoveryPrefix, bridge.bridgeId);

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

// --- several inverters on one bus ---------------------------------------------------------

// The back-compat contract, asserted against the functions that MAKE the decision rather than
// against the MqttTopics constructor. The previous version of this test compared
// MqttTopics(b, id) with MqttTopics(b, id, "") -- the same call via its default argument -- so
// it would have passed with the primary rule inverted. The rule itself lived inside MqttOutput,
// which is compiled only for ESP32 and therefore unreachable from any test (review).
static void test_the_primary_device_keeps_the_bridge_scoped_identity() {
    const std::string bridgeId = "heliograph-a1b2c3";
    TEST_ASSERT_EQUAL_STRING("", deviceTopicKey(true, "eversolar_legacy-10").c_str());
    TEST_ASSERT_EQUAL_STRING(bridgeId.c_str(),
                             deviceUniqueBase(true, bridgeId, "eversolar_legacy-10").c_str());

    const MqttTopics primary(kDefaultBaseTopic, bridgeId,
                             deviceTopicKey(true, "eversolar_legacy-10"));
    TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/state", primary.state().c_str());
    TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/availability",
                             primary.availability().c_str());
}

// ...and a NON-primary device gets its own everything. Both halves matter: this is the pair a
// wrong branch would swap.
static void test_a_non_primary_device_gets_its_own_identity() {
    const std::string bridgeId = "heliograph-a1b2c3";
    TEST_ASSERT_EQUAL_STRING("modbus_profile-2", deviceTopicKey(false, "modbus_profile-2").c_str());
    TEST_ASSERT_EQUAL_STRING("heliograph-a1b2c3_modbus_profile-2",
                             deviceUniqueBase(false, bridgeId, "modbus_profile-2").c_str());
}

static void test_further_devices_get_their_own_subtree() {
    const MqttTopics second(kDefaultBaseTopic, "heliograph-a1b2c3", "modbus_profile-2");
    TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/device/modbus_profile-2/state",
                             second.state().c_str());
    TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/device/modbus_profile-2/identity",
                             second.identity().c_str());
}

// Two inverters must not land on each other's entities. Before the unique base existed, every
// unique_id was built from the bridge id alone, so three identical inverters announced three
// times over the SAME entity -- and Home Assistant merged them into one.
// Every entity on devices 2..N announced an availability topic that NOTHING publishes -- the
// device's own, rather than the bridge's -- so Home Assistant held all of them at `unavailable`
// forever, with no retained value to recover from on restart. The measurements were on the
// broker and invisible in HA: the whole feature, silently not working (review).
static void test_every_device_tracks_the_bridge_availability_topic() {
    Rig               r;
    const DeviceState state  = r.poll();
    const BridgeInfo  bridge = makeBridge();

    const MqttTopics primary(kDefaultBaseTopic, bridge.bridgeId);
    const MqttTopics second(kDefaultBaseTopic, bridge.bridgeId, "modbus_profile-2");
    const auto entities = buildDiscoveryEntities(state, bridge, second, primary.availability(),
                                                 kDefaultDiscoveryPrefix,
                                                 bridge.bridgeId + "_modbus_profile-2");
    TEST_ASSERT_TRUE(!entities.empty());
    for (const auto& e : entities) {
        JsonDocument doc;
        TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(doc, e.payload).code());
        TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/availability",
                                 doc["availability_topic"]);
        // ...while the state it reads still comes from its OWN subtree.
        if (!doc["state_topic"].isNull()) {
            TEST_ASSERT_EQUAL_STRING("heliograph/heliograph-a1b2c3/device/modbus_profile-2/state",
                                     doc["state_topic"]);
        }
    }
}

// Three identical inverters report the same model and no serial, so without the address in the
// name Home Assistant lists three devices called exactly the same thing.
static void test_devices_beyond_the_first_carry_their_address_in_the_name() {
    Rig         r;
    DeviceState state          = r.poll();
    state.identity.instanceKey = "2";
    const BridgeInfo bridge    = makeBridge();

    const MqttTopics t1(kDefaultBaseTopic, bridge.bridgeId);
    const MqttTopics t2(kDefaultBaseTopic, bridge.bridgeId, "modbus_profile-2");
    const auto first  = buildDiscoveryEntities(state, bridge, t1, t1.availability(),
                                               kDefaultDiscoveryPrefix, bridge.bridgeId);
    const auto second = buildDiscoveryEntities(state, bridge, t2, t1.availability(),
                                               kDefaultDiscoveryPrefix,
                                               bridge.bridgeId + "_modbus_profile-2");

    JsonDocument a;
    JsonDocument b;
    deserializeJson(a, first.front().payload);
    deserializeJson(b, second.front().payload);
    const std::string nameA = a["device"]["name"].as<std::string>();
    const std::string nameB = b["device"]["name"].as<std::string>();
    TEST_ASSERT_TRUE(nameA != nameB);
    TEST_ASSERT_TRUE(nameB.find("#2") != std::string::npos);
    // The primary's name is untouched -- same reasoning as its topics.
    TEST_ASSERT_TRUE(nameA.find('#') == std::string::npos);
}

// A label becomes the Home Assistant device NAME and nothing else. This is the assertion the
// whole feature rests on: unique_id and the retained config topic stay derived from the id, so
// renaming an inverter updates the name of the device Home Assistant already has instead of
// creating a second one and stranding every entity's history behind the first (#76).
static void test_a_label_renames_the_ha_device_without_touching_its_unique_id() {
    Rig              r;
    DeviceState      state  = r.poll();
    const BridgeInfo bridge = makeBridge();
    const MqttTopics t(kDefaultBaseTopic, bridge.bridgeId);

    const auto unlabelled = buildDiscoveryEntities(state, bridge, t, t.availability(),
                                                   kDefaultDiscoveryPrefix, bridge.bridgeId);
    state.label           = "Schuur";
    const auto labelled   = buildDiscoveryEntities(state, bridge, t, t.availability(),
                                                   kDefaultDiscoveryPrefix, bridge.bridgeId);

    TEST_ASSERT_EQUAL_size_t(unlabelled.size(), labelled.size());
    for (size_t i = 0; i < labelled.size(); ++i) {
        // The two things a rename must never move.
        TEST_ASSERT_EQUAL_STRING(unlabelled[i].uniqueId.c_str(), labelled[i].uniqueId.c_str());
        TEST_ASSERT_EQUAL_STRING(unlabelled[i].configTopic.c_str(),
                                 labelled[i].configTopic.c_str());
    }

    JsonDocument doc;
    deserializeJson(doc, labelled.front().payload);
    // Bare, with no bridge prefix: somebody who types "Schuur" wants to read "Schuur".
    TEST_ASSERT_EQUAL_STRING("Schuur", doc["device"]["name"]);
    // Identifiers are keys too, and they are what Home Assistant matches an existing device on.
    JsonDocument before;
    deserializeJson(before, unlabelled.front().payload);
    TEST_ASSERT_EQUAL_STRING(before["device"]["identifiers"][0].as<const char*>(),
                             doc["device"]["identifiers"][0].as<const char*>());
    // State topics follow the id, not the name.
    if (!doc["state_topic"].isNull()) {
        TEST_ASSERT_EQUAL_STRING(before["state_topic"].as<const char*>(),
                                 doc["state_topic"].as<const char*>());
    }
}

// The address suffix exists to tell identical inverters apart. A label already does that, so
// appending "#2" to it would put the address back into the one name chosen to be free of it.
static void test_a_label_replaces_the_address_suffix_rather_than_carrying_it() {
    Rig         r;
    DeviceState state          = r.poll();
    state.identity.instanceKey = "2";
    state.label                = "Balkon";
    const BridgeInfo bridge    = makeBridge();
    const MqttTopics t(kDefaultBaseTopic, bridge.bridgeId, "modbus_profile-2");

    const auto entities = buildDiscoveryEntities(state, bridge, t, t.availability(),
                                                 kDefaultDiscoveryPrefix,
                                                 bridge.bridgeId + "_modbus_profile-2");
    JsonDocument doc;
    deserializeJson(doc, entities.front().payload);
    TEST_ASSERT_EQUAL_STRING("Balkon", doc["device"]["name"]);
}

static void test_two_devices_produce_distinct_unique_ids_and_ha_devices() {
    Rig               r;
    const DeviceState state  = r.poll();
    const BridgeInfo  bridge = makeBridge();

    const MqttTopics t1(kDefaultBaseTopic, bridge.bridgeId);
    const MqttTopics t2(kDefaultBaseTopic, bridge.bridgeId, "modbus_profile-2");
    const auto       first  = buildDiscoveryEntities(state, bridge, t1, t1.availability(),
                                                     kDefaultDiscoveryPrefix, bridge.bridgeId);
    const auto       second = buildDiscoveryEntities(state, bridge, t2, t1.availability(),
                                                     kDefaultDiscoveryPrefix,
                                                     bridge.bridgeId + "_modbus_profile-2");

    TEST_ASSERT_TRUE(!first.empty() && first.size() == second.size());
    for (const auto& e : first) {
        for (const auto& o : second) {
            TEST_ASSERT_TRUE(e.uniqueId != o.uniqueId);
            TEST_ASSERT_TRUE(e.configTopic != o.configTopic);
        }
    }
}


// The topics a removed device has to be cleared on are enumerated from the canonical ids,
// because by the time the clearing runs the device's measurement set is gone with it. If a
// canonical id were missing from that list its entity would stay in Home Assistant reporting
// online forever -- so this pins that every id a discovery entity can be built from is in it.
static void assertEveryAnnouncedSlugIsClearable(const DeviceState& state, const char* who) {
    const BridgeInfo bridge = makeBridge();
    const MqttTopics topics(kDefaultBaseTopic, bridge.bridgeId);
    const auto entities = buildDiscoveryEntities(state, bridge, topics, topics.availability(),
                                                 kDefaultDiscoveryPrefix, bridge.bridgeId);

    std::vector<std::string> clearable;
    for (const char* id : measurement_id::kAll) {
        clearable.push_back(sanitizeId(id));
    }
    clearable.push_back("status");
    clearable.push_back("inverter_online");

    TEST_ASSERT_TRUE(!entities.empty());
    for (const auto& e : entities) {
        // unique_id is "<base>_<slug>"; the slug is what the config topic is keyed on.
        const std::string slug = e.uniqueId.substr(bridge.bridgeId.size() + 1);
        TEST_ASSERT_TRUE_MESSAGE(
            std::find(clearable.begin(), clearable.end(), slug) != clearable.end(),
            (std::string(who) + " announced a slug that cannot be cleared: " + slug).c_str());
    }
}

static void test_every_announceable_measurement_is_clearable() {
    // Both drivers, because one driver's channel set proves nothing about the enumeration. The
    // EverSolar rig alone happened to be all-canonical, so the first version of this test passed
    // while the mock -- three phases and a battery, which is what it exists for -- announced
    // eight ids that no consumer could enumerate or clear (review, 2026-07-26).
    Rig r;
    assertEveryAnnouncedSlugIsClearable(r.poll(), "eversolar");

    mock::MockDriver driver(clockFn, mock::MockOptions{});
    StateStore       store;
    Diagnostics      diag;
    g_now = 12ULL * 60 * 60 * 1000;
    DeviceContext ctx(driver, store, diag, clockFn);
    ctx.pollOnce();
    assertEveryAnnouncedSlugIsClearable(*store.snapshot(), "mock");
}

// --- which devices have to be forgotten ---------------------------------------------------
//
// The decision, not the publishing: MqttOutput is ESP32-only, so this rule is the part that can
// be held to account. Its predecessor -- a `primary` flag computed at the call site from a
// variable that could never hold a removed device's id -- was dead code no test could reach.

static void test_a_removed_device_is_forgotten() {
    const auto gone = devicesToForget({{"eversolar-1", true}, {"modbus_profile-2", false}},
                                      {"eversolar-1"}, "eversolar-1");
    TEST_ASSERT_EQUAL_UINT32(1, gone.size());
    TEST_ASSERT_EQUAL_STRING("modbus_profile-2", gone[0].c_str());
}

static void test_a_re_addressed_device_is_forgotten_under_its_old_id() {
    // Bring-up reality: unit 3 turns out to sit at address 4. The address is part of the id.
    const auto gone = devicesToForget({{"modbus_profile-1", true}, {"modbus_profile-3", false}},
                                      {"modbus_profile-1", "modbus_profile-4"},
                                      "modbus_profile-1");
    TEST_ASSERT_EQUAL_UINT32(1, gone.size());
    TEST_ASSERT_EQUAL_STRING("modbus_profile-3", gone[0].c_str());
}

static void test_a_promoted_device_gives_up_its_device_scoped_tree() {
    // Device 1 deleted, device 2 moved into the `driver` slot. Its id never changed, so nothing
    // ever saw it as removed -- and its whole per-device entity set stayed in Home Assistant.
    const auto gone = devicesToForget({{"eversolar-1", true}, {"modbus_profile-2", false}},
                                      {"modbus_profile-2"}, "modbus_profile-2");
    TEST_ASSERT_EQUAL_UINT32(1, gone.size());
    TEST_ASSERT_EQUAL_STRING("modbus_profile-2", gone[0].c_str());
}

static void test_the_bridge_scoped_tree_is_never_cleared() {
    // The old primary is gone and a different device owns the bridge-scoped tree now. Clearing
    // what the old one published would delete the live primary's entities and their history --
    // handing them over is the back-compat contract, not a leak to be plugged.
    const auto gone = devicesToForget({{"eversolar-1", true}}, {"modbus_profile-1"},
                                      "modbus_profile-1");
    TEST_ASSERT_TRUE(gone.empty());

    // Same when nothing is primary this boot: still not ours to delete.
    TEST_ASSERT_TRUE(devicesToForget({{"eversolar-1", true}}, {}, "").empty());
}

static void test_an_unchanged_line_up_forgets_nothing() {
    const std::vector<AnnouncedDevice> announced{{"modbus_profile-1", true},
                                                 {"modbus_profile-2", false},
                                                 {"modbus_profile-3", false}};
    const std::vector<std::string>     current{"modbus_profile-1", "modbus_profile-2",
                                               "modbus_profile-3"};
    TEST_ASSERT_TRUE(devicesToForget(announced, current, "modbus_profile-1").empty());
    TEST_ASSERT_TRUE(devicesToForget({}, current, "modbus_profile-1").empty());
}


/// The memory guard in front of every publish (audit F5). espMqttClient has this policy
/// already but compares against max(internal, PSRAM), so on a board with 8 MB of idle PSRAM it
/// never fires while the allocation itself comes from internal SRAM. This predicate is that
/// same 16 KB intent, measured on the pool that actually pays.
///
/// This pins the PREDICATE, not the wiring around it. MqttOutput itself is ESP32-only (no
/// fake espMqttClient exists to run its loop() on the host), so the rule added alongside this
/// guard -- a refused publish must not be recorded as delivered, or a Home Assistant entity or
/// a relay ack can go missing until an unrelated reconnect or signature change retriggers it,
/// see loop()'s discovery/relay-ack commits in mqtt_output.cpp -- is verified by review and by
/// the compile/layering/build checks, not by a test that can inject a refusal and observe the
/// retry. Recorded here rather than left implicit (review, 2026-07-30).
static void test_publish_memory_guard() {
    // Comfortable: the figure a healthy 6CH reports for this exact call -- 90 100 B, measured
    // 2026-07-30 via max_alloc_heap_bytes, which main.cpp fills from ESP.getMaxAllocHeap().
    TEST_ASSERT_FALSE(refusePublishForMemory(90100));
    // The threshold is a floor, not a target: at exactly the floor there is still room.
    TEST_ASSERT_FALSE(refusePublishForMemory(kMinFreeBlockBytes));
    TEST_ASSERT_TRUE(refusePublishForMemory(kMinFreeBlockBytes - 1));
    // The case this exists for: PSRAM would report megabytes free, internal SRAM is nearly
    // gone. Only the internal figure reaches this function, so the answer is refuse.
    TEST_ASSERT_TRUE(refusePublishForMemory(4096));
    TEST_ASSERT_TRUE(refusePublishForMemory(0));
}


int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_every_announceable_measurement_is_clearable);
    RUN_TEST(test_a_removed_device_is_forgotten);
    RUN_TEST(test_a_re_addressed_device_is_forgotten_under_its_old_id);
    RUN_TEST(test_a_promoted_device_gives_up_its_device_scoped_tree);
    RUN_TEST(test_the_bridge_scoped_tree_is_never_cleared);
    RUN_TEST(test_an_unchanged_line_up_forgets_nothing);
    RUN_TEST(test_the_primary_device_keeps_the_bridge_scoped_identity);
    RUN_TEST(test_a_non_primary_device_gets_its_own_identity);
    RUN_TEST(test_every_device_tracks_the_bridge_availability_topic);
    RUN_TEST(test_devices_beyond_the_first_carry_their_address_in_the_name);
    RUN_TEST(test_further_devices_get_their_own_subtree);
    RUN_TEST(test_a_label_renames_the_ha_device_without_touching_its_unique_id);
    RUN_TEST(test_a_label_replaces_the_address_suffix_rather_than_carrying_it);
    RUN_TEST(test_two_devices_produce_distinct_unique_ids_and_ha_devices);
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
    RUN_TEST(test_a_driver_without_a_status_word_publishes_no_status_code);
    RUN_TEST(test_stale_measurements_are_published_as_null);
    RUN_TEST(test_a_genuine_zero_is_published_as_zero);
    RUN_TEST(test_derived_measurements_are_flagged);
    RUN_TEST(test_oversized_payload_is_refused_rather_than_truncated);
    RUN_TEST(test_state_payload_stays_well_within_the_bound);
    RUN_TEST(test_the_mock_hybrid_payload_also_fits);
    RUN_TEST(test_the_backtrace_stays_off_the_mqtt_payload);
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
    RUN_TEST(test_a_writable_numeric_command_gets_a_number_entity);
    RUN_TEST(test_start_and_stop_get_button_entities);
    RUN_TEST(test_an_enum_command_without_options_gets_no_entity);
    RUN_TEST(test_a_mode_command_with_options_becomes_a_select);
    RUN_TEST(test_discovery_signature_changes_when_a_mode_is_renumbered);
    RUN_TEST(test_discovery_signature_changes_when_a_writable_bound_changes);
    RUN_TEST(test_discovery_signature_changes_when_supported_flips_with_unchanged_bounds);
    RUN_TEST(test_discovery_signature_changes_when_writable_flips_with_unchanged_bounds);
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
    RUN_TEST(test_publish_memory_guard);
    return UNITY_END();
}
