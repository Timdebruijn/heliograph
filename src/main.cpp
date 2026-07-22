// SPDX-License-Identifier: MIT
//
// Heliograph — firmware entry point.
//
// Boot order matters and is deliberate:
//   1. serial;
//   2. configuration from NVS (nothing else can be decided without it);
//   3. WiFi, or the setup portal when unprovisioned;
//   4. driver + poll task -- started even without a network, because RS485 does not need one;
//   5. outputs, only once there is a network to serve them on.
//
// There are no credentials in this file and none in the image. An unprovisioned device puts
// up Heliograph-Setup-XXXX and waits.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "app/discovery_runner.h"
#include "boards/board.h"
#include "config/configuration.h"
#include "config/configuration_store.h"
#include "config/nvs_backend.h"
#include "device/device_context.h"
#include "diagnostics/diagnostics.h"
#include "diagnostics/log_timestamp.h"
#include "diagnostics/logger.h"
#include "ota/ota_manager.h"
#include "drivers/driver_registry.h"
#include "network/rtc_pcf85063.h"
#include "network/time_manager.h"
#include "network/wifi_manager.h"
#include "outputs/modbus_tcp/modbus_tcp_server.h"
#include "outputs/mqtt/mqtt_output.h"
#include "outputs/rest/rest_api.h"
#include "state/state_store.h"
#include "transport/rs485_transport.h"

using namespace heliograph;

namespace {

// The build stamp (adjacent string-literal concatenation, resolved at compile time) makes each
// image identifiable over the API. Without it two different builds both reported "0.1.0" and a
// flash could not be told from the previous one -- exactly what bit the post-flash check on
// 2026-07-21. Bumped to 0.2.0 for the Fase 9 review batch.
constexpr const char* kFirmwareVersion = "0.6.0 (" __DATE__ " " __TIME__ ")";

Rs485Transport     g_transport;
DriverRegistry     g_registry;
DeviceManager      g_devices;
Diagnostics        g_diagnostics;
NvsBackend         g_nvs;
NvsBackend         g_nvsLegacy{kLegacyStorageNamespace};  // pre-rename config, read-only
ConfigurationStore g_store{g_nvs, &g_nvsLegacy};
Configuration      g_config;
WifiManager        g_wifi;
TimeManager        g_time;
modbus::ModbusTcpServer           g_modbus;
std::unique_ptr<mqtt::MqttOutput> g_mqtt;
std::unique_ptr<rest::RestApi>    g_rest;

std::unique_ptr<InverterDriver> g_driver;
std::unique_ptr<DeviceContext>  g_context;
StateStore*                     g_state = nullptr;

bool g_outputsStarted = false;

/// Guards g_config against the one cross-task hazard it has: the AsyncTCP task replacing
/// the whole object (config PATCH / provision, via ctx.applyConfig) while loop()/rs485Task
/// read its std::string members. Readers on the AsyncTCP task itself need no lock -- they
/// are serialized with the writer by the library's single event task.
std::mutex g_configMutex;

/// When to reboot, or 0. See requestReboot below.
// Atomic, not just volatile: written by the AsyncTCP task, read by the Arduino loop task, and
// a 64-bit load/store is not a single instruction on this 32-bit MCU -- volatile alone allows
// a torn read. Consistent with g_manualPollRequested.
std::atomic<uint64_t> g_rebootAtMs{0};
/// Set by the REST poll action, consumed by rs485Task: the task owns the bus, the web thread
/// only ever asks. See the note at ctx.requestPoll.
std::atomic<bool> g_manualPollRequested{false};

uint64_t nowMs() { return static_cast<uint64_t>(millis()); }

/// Owns discovery runs. The web handler requests; rs485Task runs, because it owns the bus.
DiscoveryRunner g_discovery{g_registry, nowMs};

/// The configured driver, or the highest-priority one compiled in. No manufacturer name here.
std::string selectedDriverId() {
    if (!g_config.driver.id.empty() && g_registry.contains(g_config.driver.id)) {
        return g_config.driver.id;
    }
    const auto available = g_registry.availableDrivers();
    return available.empty() ? std::string{} : available.front().id;
}

BridgeInfo bridgeInfo() {
    BridgeInfo info;
    info.boardName        = board::kName;
    info.bridgeId         = g_wifi.bridgeId();
    {
        // bridgeInfo() runs on loop() and rs485Task; the AsyncTCP task can be replacing
        // g_config concurrently (see g_configMutex).
        std::lock_guard<std::mutex> lock(g_configMutex);
        info.name = g_config.bridgeName;
    }
    info.bridgeOnline     = true;
    info.uptimeSeconds    = static_cast<uint32_t>(millis() / 1000);
    info.freeHeapBytes    = ESP.getFreeHeap();
    info.minFreeHeapBytes = ESP.getMinFreeHeap();
    info.resetReason      = static_cast<uint16_t>(esp_reset_reason());
    info.wifiConnected    = g_wifi.connected();
    info.wifiRssiDbm      = g_wifi.rssi();
    info.mqttConnected    = g_mqtt && g_mqtt->connected();
    info.modbusListening  = g_modbus.running();
    info.modbusClients    = g_modbus.activeClients();
    info.firmwareVersion  = kFirmwareVersion;
    info.timeSynced       = g_time.synced();
    info.currentEpoch     = static_cast<int64_t>(time(nullptr));
    info.lastNtpSyncEpoch = static_cast<int64_t>(g_time.lastSyncEpoch());
    const auto ntpSource  = g_time.syncSource();
    info.ntpServer        = ntpSource.server;
    info.ntpFromDhcp      = ntpSource.fromDhcp;
    info.otaImageState    = ota::imageStateName();
    return info;
}

std::string scanNetworksJson() {
    const int n = WiFi.scanNetworks();

    // One entry per SSID, strongest BSSID wins. A multi-AP network (UniFi and friends)
    // returns every access point separately, which showed the same name three times in the
    // picker -- pointless, since joining is by SSID and the firmware picks the strongest
    // BSSID itself at connect time (WIFI_CONNECT_AP_BY_SIGNAL). Hidden networks (empty
    // SSID) are skipped: an unnameable entry cannot be chosen from a list anyway.
    struct Network {
        String  ssid;
        int32_t rssi;
        bool    open;
    };
    std::vector<Network> unique;
    for (int i = 0; i < n; ++i) {
        const String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) {
            continue;
        }
        bool merged = false;
        for (auto& u : unique) {
            if (u.ssid == ssid) {
                if (WiFi.RSSI(i) > u.rssi) {
                    u.rssi = WiFi.RSSI(i);
                    u.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
                }
                merged = true;
                break;
            }
        }
        if (!merged) {
            unique.push_back({ssid, WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
        }
    }

    std::string out   = "{\"networks\":[";
    size_t      count = 0;
    for (const auto& net : unique) {
        if (count >= 20) {
            break;
        }
        if (count++ > 0) {
            out += ',';
        }
        // The SSID lands in JSON. Escape it rather than trust an access point's name -- it is
        // attacker-controlled data by definition.
        std::string escaped;
        for (size_t j = 0; j < net.ssid.length(); ++j) {
            const char c = net.ssid[j];
            if (c == '"' || c == '\\') {
                escaped.push_back('\\');
            }
            if (static_cast<unsigned char>(c) >= 0x20) {
                escaped.push_back(c);
            }
        }
        out += "{\"ssid\":\"" + escaped + "\",\"rssi\":" + std::to_string(net.rssi) +
               ",\"open\":" + (net.open ? "true" : "false") + "}";
    }
    out += "]}";
    WiFi.scanDelete();
    return out;
}

void startOutputs() {
    if (g_outputsStarted || !g_wifi.connected()) {
        return;
    }
    g_outputsStarted = true;

    // Snapshot under the lock, then configure everything from the copy: this runs on the
    // loop task and reads many string members, any of which the AsyncTCP task could be
    // replacing (see g_configMutex). One copy at startup beats fine-grained locking below.
    Configuration configSnapshot;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        configSnapshot = g_config;
    }

    // Time first: SNTP needs the network (and the DHCP lease that may carry the NTP server), and
    // starting it here means every log line below already gets a wall-clock stamp once it syncs.
    g_time.begin(configSnapshot);

    if (configSnapshot.modbus.enabled) {
        modbus::ModbusServerConfig cfg;
        cfg.port              = configSnapshot.modbus.port;
        cfg.inverterUnitId    = configSnapshot.modbus.unitId;
        cfg.diagnosticsUnitId = configSnapshot.modbus.diagnosticsUnitId;
        // Never from configuration: no driver in this build can write, so offering the switch
        // would advertise something untrue. validate() rejects it too.
        cfg.writeEnabled = false;
        g_modbus.setConfig(cfg);
        log::info("modbus: %s on :%u (unit %u)", g_modbus.begin() ? "listening" : "failed to start",
                  cfg.port, cfg.inverterUnitId);
    }

    if (configSnapshot.mqtt.enabled && !configSnapshot.mqtt.host.empty()) {
        mqtt::MqttConfig cfg;
        cfg.enabled          = true;
        cfg.host             = configSnapshot.mqtt.host;
        cfg.port             = configSnapshot.mqtt.port;
        cfg.username         = configSnapshot.mqtt.username;
        cfg.password         = configSnapshot.mqtt.password;
        cfg.baseTopic        = configSnapshot.mqtt.baseTopic;
        cfg.discoveryPrefix  = configSnapshot.mqtt.discoveryPrefix;
        cfg.discoveryEnabled = configSnapshot.mqtt.discoveryEnabled;
        cfg.qos              = configSnapshot.mqtt.qos;
        g_mqtt               = std::make_unique<mqtt::MqttOutput>(cfg);
        g_mqtt->setDiagnostics(&g_diagnostics);
        g_mqtt->begin(bridgeInfo());
        // The host is not a secret; the password must never reach a log.
        log::info("mqtt: broker %s:%u", cfg.host.c_str(), cfg.port);
    }

    log::info("web: http://%s/", g_wifi.ipAddress().c_str());
}

void startRestApi() {
    rest::RestContext ctx;
    ctx.devices     = &g_devices;
    ctx.diagnostics = &g_diagnostics;
    ctx.registry    = &g_registry;
    ctx.config      = &g_config;
    // The one sanctioned write path to g_config after boot. The AsyncTCP task publishes a
    // whole new Configuration here while loop()/rs485Task read string members via
    // bridgeInfo(); without the lock that is a use-after-free waiting on a settings save
    // landing mid-poll (review, 2026-07-21).
    ctx.applyConfig = [](const Configuration& c) {
        std::lock_guard<std::mutex> lock(g_configMutex);
        g_config = c;
    };
    ctx.bridgeInfo  = bridgeInfo;
    ctx.clock       = nowMs;
    ctx.saveConfig  = [](const Configuration& c) { return g_store.save(c); };
    // Request only -- the poll itself runs on rs485Task, exactly like discovery. Running
    // pollOnce() here executed a seconds-long bus transaction inside an AsyncTCP callback and
    // raced the periodic poll for the bus lock: with the mock (no bus) it always won, against
    // the real inverter the wizard's test poll lost and got 409 "bus_busy". Found live during
    // Phase 3 (2026-07-19), same seam class as the deferred reboot above.
    ctx.requestPoll = [] {
        if (!g_context) {
            return false;
        }
        g_manualPollRequested = true;
        return true;
    };
    ctx.requestReboot = [] {
        // Do NOT restart here. This runs inside an AsyncTCP callback and the response has only
        // been *queued*, not sent -- restarting drops the connection before it goes out, and
        // the browser reports a network error for a request that actually succeeded. Worse, a
        // blocking delay() in this callback stalls the whole web server.
        //
        // Hand it to loop() instead, with enough time for the socket to flush.
        g_rebootAtMs = static_cast<uint64_t>(millis()) + 1500;
    };
    ctx.requestDiscovery    = [](bool extended) { return g_discovery.request(extended); };
    ctx.discoveryReport     = [] { return g_discovery.report(); };
    ctx.requestFactoryReset = [] { return g_store.factoryReset(); };
    ctx.portalActive        = [] { return g_wifi.portalActive(); };
    ctx.scanNetworks        = scanNetworksJson;

    g_rest = std::make_unique<rest::RestApi>(ctx);
    g_rest->begin();
}

void rs485Task(void* /*arg*/) {
    for (;;) {
        // One feed per iteration; the 120 s budget covers the longest legitimate iteration
        // (an extended discovery run). See the watchdog setup in setup().
        esp_task_wdt_reset();
        // Discovery first, and instead of polling this cycle: probing re-registers every
        // inverter on the bus, so the two must never interleave. This task owns the bus, which
        // is why the web handler only ever *requests* a run.
        if (g_discovery.runIfRequested(g_transport)) {
            Serial.printf("[discovery] %s\n", g_discovery.report().outcome.reason.c_str());
            // The probe left the bus re-registered; make the driver pick that up rather than
            // poll a stale address.
            if (g_driver) {
                g_driver->begin(g_transport);
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        // A requested manual poll (wizard test poll, REST action) runs now rather than at the
        // next interval; exchange() clears the flag so one request is one poll.
        if (g_context &&
            (g_manualPollRequested.exchange(false) || g_context->due(nowMs()))) {
            const PollResult result = g_context->pollOnce();
            if (result != PollResult::Ok) {
                // Bounded: one line per attempt, no payload, no growth over time.
                log::warn("poll: %s", pollResultName(result));
            }
        }
        if (g_state) {
            const auto snapshot = g_state->snapshot();
            const auto bridge   = bridgeInfo();
            const auto diag     = g_diagnostics.snapshot();
            g_modbus.refresh(*snapshot, bridge, diag, nowMs());
            if (g_mqtt) {
                g_mqtt->loop(*snapshot, bridge, diag, nowMs());
            }
            if (g_rest) {
                g_rest->notifyState(*snapshot, nowMs());
            }
        }
        // Never a long delay: an unreachable inverter must not stall the task or the watchdog.
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

}  // namespace

void setup() {
    // No GPIO forcing here. Earlier revisions drove GPIO47 low first thing, believing this
    // board was the Relay-1CH and that pin its relay. The real board (RS485-CAN) has no
    // relay, and the safest state for a pin with no known function is untouched hi-Z.
    Serial.begin(115200);
    const uint32_t serialDeadline = millis() + 2000;
    while (!Serial && millis() < serialDeadline) {
        delay(10);
    }

    Serial.printf("\nHeliograph %s\nboard: %s\nreset reason: %d\n", kFirmwareVersion,
                  board::kName, static_cast<int>(esp_reset_reason()));

    const auto loaded = g_store.load(g_config);
    // Apply the stored level before anything else logs, or the first lines ignore it.
    log::setLevel(g_config.logLevel);
    log::info("config: %s (log level %s)", loadResultName(loaded), logLevelName(g_config.logLevel));
    if (loaded == LoadResult::Corrupt || loaded == LoadResult::FutureVersion) {
        // Defaults, so the device lands in the setup portal. Better than running on values we
        // could not parse or do not understand.
        Serial.println("[config] falling back to defaults; the setup portal will start");
    }

    // RTC restore, before anything else that logs at length: with a battery-backed
    // PCF85063 (board::kHasRtc) the system clock is valid from here on, so every log line
    // below carries a wall-clock timestamp even when the network never comes up -- which
    // is exactly the boot you end up debugging. TZ first, so the timestamps render local.
    setenv("TZ", g_config.ntp.timezone.c_str(), 1);
    tzset();
    if (rtc::begin()) {
        time_t stored = 0;
        if (rtc::readUtc(stored)) {
            const timeval tv{stored, 0};
            settimeofday(&tv, nullptr);
            char buf[24];
            log::formatIsoLocalTime(buf, sizeof(buf), stored);
            log::info("rtc: clock restored: %s (awaiting ntp for drift correction)", buf);
        } else {
            log::warn("rtc: present but time not set (first boot or empty backup supply)");
        }
    }

    // A factory-fresh device gets a UNIQUE default hostname (heliograph-a1b2c3, from the
    // MAC) instead of the shared "heliograph": two bridges on one LAN -- configuring a
    // second unit at home before installing it elsewhere is the normal way to deploy one --
    // otherwise fight over the same mDNS name. Provisioned devices keep whatever hostname
    // they were given; this only upgrades the never-configured default.
    if (!g_config.provisioned() && g_config.wifi.hostname == "heliograph") {
        g_config.wifi.hostname = g_wifi.bridgeId();
    }

    registerBuiltinDrivers(g_registry);
    g_wifi.setDiagnostics(&g_diagnostics);
    // lwip only harvests the DHCP-offered NTP server (option 42) if this is armed when the
    // lease arrives -- but the arming call is dispatched via the tcpip thread, so it must
    // run AFTER the network stack exists and BEFORE the lease: exactly the hook's moment.
    // Calling it here directly, before WiFi, aborted the boot (0.4.4, 2026-07-21).
    g_wifi.setNetworkStackReadyHook([] { g_time.prepareDhcp(g_config); });
    g_wifi.begin(g_config);
    if (!g_config.provisioned()) {
        Serial.printf("[wifi] not provisioned; setup AP '%s' is up\n", g_wifi.apSsid().c_str());
    }

    // The driver starts regardless of the network: RS485 does not need WiFi, and a bridge
    // sitting in the setup portal should already be polling so the first page load has real
    // data on it.
    const std::string driverId = selectedDriverId();
    // Pass the configured driver options through: a unit_id or profile set in the web UI
    // must reach the driver, not silently fall back to factory defaults (2026-07-21 review).
    g_driver = driverId.empty() ? nullptr
                                : g_registry.create(driverId, g_transport,
                                                    g_config.driver.options);
    if (g_driver && g_driver->begin(g_transport)) {
        Serial.printf("[driver] %s (%s)\n", g_driver->descriptor().id.c_str(),
                      supportLevelName(g_driver->descriptor().supportLevel));
        const DeviceId id = g_driver->identity().deviceId();
        g_state           = g_devices.add(id);
        if (g_state) {
            PollPolicy policy;
            policy.intervalMs = g_config.polling.intervalSeconds * 1000;
            g_context =
                std::make_unique<DeviceContext>(*g_driver, *g_state, g_diagnostics, nowMs, policy);
        }
    } else {
        Serial.printf("[driver] '%s' unavailable\n", driverId.c_str());
    }

    // The web server runs on the portal AP too -- that is how setup happens at all.
    startRestApi();

    // Pinned to core 1: WiFi and lwIP live on core 0, so network load cannot disturb RS485
    // timing. See docs/architecture.md.
    //
    // 8192, not 4096: this task runs the whole driver chain, and the deepest real path --
    // poll -> registerDevice -> transact (two ~300 B frame buffers) -> traceHex -> emit ->
    // newlib vsnprintf (~1.3 KB by itself) -- blew the 4 KB canary within seconds of the
    // first contact with a real inverter (boot loop, 2026-07-19). The mock never came close:
    // its poll is pure arithmetic and never touches the transport or the hexdump path.
    TaskHandle_t rs485Handle = nullptr;
    xTaskCreatePinnedToCore(rs485Task, "rs485", 8192, nullptr, 5, &rs485Handle, 1);

    // Watchdog coverage for both application tasks. Without this, only the idle tasks were
    // watched: a HANG (as opposed to a crash) in loop() or rs485Task ran forever with no
    // recovery -- and a hang after the OTA image was confirmed healthy is permanent, because
    // a never-resetting device also never rolls back (review, 2026-07-21). 120 s, not the
    // 5 s default: an extended discovery scan legitimately runs many back-to-back 3 s
    // transactions between feeds, and the purpose here is catching forever-hangs, not
    // latency policing.
    esp_task_wdt_config_t wdtConfig = {
        .timeout_ms    = 120000,
        .idle_core_mask = 1 << 0,  // keep the idle-task coverage the sdkconfig had
        .trigger_panic = true,     // panic -> reset -> (unconfirmed image) -> rollback
    };
    esp_task_wdt_reconfigure(&wdtConfig);
    enableLoopWDT();
    if (rs485Handle != nullptr) {
        esp_task_wdt_add(rs485Handle);
    }
}

void loop() {
    if (g_rebootAtMs != 0 && nowMs() >= g_rebootAtMs) {
        Serial.println("[sys] rebooting");
        Serial.flush();
        ESP.restart();
    }

    // Confirm a freshly-flashed image to the bootloader once it has run healthily, so it is not
    // rolled back on the next reboot. Latched: the check runs until it fires once. Gated on
    // network health, not on an inverter poll -- the inverter is gone every night.
    static bool bootConfirmed = false;
    if (ota::shouldConfirmHealthyBoot(g_wifi.connected(), nowMs(), bootConfirmed,
                                      g_diagnostics.pollSuccessTotal() > 0)) {
        ota::confirmHealthyBoot();
        bootConfirmed = true;
        log::info("ota: image confirmed healthy; rollback cancelled");
    }

    g_wifi.loop(nowMs());
    startOutputs();  // no-op until there is a network, and only ever runs once

    // After every NTP sync, put the corrected time into the battery-backed RTC (when the
    // board has one), so the next boot restores an accurate clock. Runs on the loop task,
    // not in the SNTP callback: I2C from lwip's thread is asking for trouble.
    static time_t lastRtcSync = 0;
    const time_t  ntpSync     = g_time.lastSyncEpoch();
    if (ntpSync != 0 && ntpSync != lastRtcSync) {
        lastRtcSync = ntpSync;
        if (rtc::writeUtc(time(nullptr))) {
            log::debug("rtc: updated from ntp");
        }
    }

    // modbus_client_connections_total was a dead counter: recordModbusClient() existed but
    // nothing ever called it, so the API reported 0 forever (found by the Fase 9 multi-client
    // test, 2026-07-22: three live clients, counter stayed 0). eModbus exposes no connect
    // hook, so count rising edges of the active-client count instead. Sampled per loop pass;
    // a connection shorter than one pass can be missed, which is fine for a trend counter.
    {
        static uint16_t prevModbusClients = 0;
        const uint16_t  nowClients        = g_modbus.activeClients();
        for (uint16_t i = prevModbusClients; i < nowClients; ++i) {
            g_diagnostics.recordModbusClient();
        }
        prevModbusClients = nowClients;
    }

    static uint32_t lastReport = 0;
    if (millis() - lastReport > 10000) {
        lastReport = millis();
        if (g_state) {
            const auto  s = g_state->snapshot();
            const auto* p = s->measurements.find(measurement_id::kAcPowerTotal);
            // Through the logger, not a raw print: the heartbeat then carries the same
            // wall-clock stamp as everything else, which is what makes an unattended capture
            // legible after the fact.
            log::info("state: wifi=%s inverter=%d valid=%d stale=%d power=%s heap=%lu",
                      provisioningStateName(g_wifi.state()), s->inverterOnline, s->dataValid,
                      s->dataStale, (p && p->valid) ? String(p->value, 1).c_str() : "unknown",
                      static_cast<unsigned long>(ESP.getFreeHeap()));
        }
    }
    delay(100);
}
