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
#include <esp_timer.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "app/capture_runner.h"
#include "app/discovery_runner.h"
#include "boards/board.h"
#include "config/configuration.h"
#include "config/configuration_store.h"
#include "config/nvs_backend.h"
#include "device/device_context.h"
#include "diagnostics/coredump.h"
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
#include "outputs/rest/rest_payloads.h"
#include "relays/drm.h"
#include "relays/relay_controller.h"
#include "status/boot_button.h"
#include "status/status_led.h"
#include "state/state_store.h"
#include "transport/rs485_transport.h"

using namespace heliograph;

namespace {

// Single source of truth for the firmware version. The three numbers feed the Modbus
// diagnostic registers (820-822); the string -- built from the same numbers plus a
// compile-time build stamp -- feeds the REST/MQTT API and the boot banner. Deriving the
// string from the numbers means they can never drift, which they had: bridgeInfo() only ever
// set the string, so the Modbus registers reported the 0.1.0 struct default for every release.
//
// Keep in lockstep with the git tag: the release workflow builds from the tag, so a stale
// value ships a firmware that misreports its own version -- exactly what bit the post-flash
// check on 2026-07-21. 0.9.0 covered the stability + observability + config-transparency
// batch; 0.10.0 added the BOOT-hold factory reset and the Relay-6CH status LED, both verified
// on hardware; 0.10.1 was defect-fixes from the full-codebase review; 0.11.0 added the generic
// SunSpec Modbus driver and the shared Modbus transaction it runs on; 0.12.0 added a second
// vendor register-map profile and made driver options validated rather than free-form; 0.13.0
// polls several inverters on one bus and carries every one of them into every output; 0.13.1
// stops the boot log opening in UTC after a warm reset; 0.13.2 is the cleanup sweep -- every
// warning our own code emitted, the dead symbols, the duplication that had one point of truth
// to move to, and a traceHex() that printed stack bytes when asked to dump zero of them;
// 0.14.0 makes the configuration something you can carry off the board (backup, previewed
// restore, and an undo), records a raw bus for a device no driver can name, and gives the
// settings page the grouping and the spacing it never had; 0.15.0 tells you in the dashboard
// when a newer release exists and installs it in one click -- checked in the browser, never by
// the bridge, and verified against the release checksum before the boot partition flips -- and
// lets the mock driver be a whole simulated fleet instead of one inverter; 0.15.1 changes
// nothing a user can see -- it collapses five duplications onto one point of truth each (the
// config document that was written twice, the JSON size bound that existed three times, the
// rate limiter, the digest-to-hex renderer, the relay safety gates), names the NVS cap that had
// been a bare 3900, and adds two guards so the classes of rot it cleaned up cannot come back:
// a layering rule against comments citing line numbers in our own files, and a test asserting
// the two config documents differ only in their credentials; 0.15.2 makes the admin password
// box on the Logs tab fillable again -- the 5 s refresh re-opened the prompt while it was being
// typed into and blanked the field, so the only way in was to sign in on Settings first (found
// on hardware) -- and stops a late 401 from discarding credentials another request had just had
// accepted; 0.16.0 lets the bridge take a static address instead of only a DHCP lease -- with
// the validation doing the real work, because a wrong address does not fail loudly (the WiFi
// association still succeeds, the bridge simply becomes unreachable), so every mistake visible
// from the settings form is refused before it is stored, including the two whose symptom is an
// absence rather than an error: no DNS while something is configured by name, and no NTP server
// on a network that has no lease to supply one. It also records which MQTT topic tree was
// announced, so a bridge whose base topic or discovery prefix changes can at least say what it
// left behind, and splits Backup and System out of a settings page that had grown to nine
// sections; 0.17.0 makes a bridge with several inverters legible. The per-inverter strip on the
// Dashboard carried watts and nothing else, so the only comparison available was who is
// producing more right now -- it now also shows energy today, AC voltage, temperature and, for
// hybrids, battery state of charge and whether the battery is charging or discharging and by
// how much, said in words rather than as a signed number. Only columns some inverter can
// actually fill appear, because a column of em dashes reads as broken rather than as not
// applicable. The ten-second log heartbeat stops describing the first device as though it were
// the bridge -- it reported one inverter's power and printed a bool under a name that read as a
// count, so four inverters looked exactly like one -- and now gives the fleet, naming every
// inverter that is not answering, why, and how long ago it last replied. Underneath: the two
// PMU drivers stop carrying the same hundred-line transaction loop twice, and AsyncTCP and
// ESPAsyncWebServer move up for two use-after-free fixes on the teardown path the dashboard's
// live updates run over.
#define HELIOGRAPH_VERSION_MAJOR 0
#define HELIOGRAPH_VERSION_MINOR 17
#define HELIOGRAPH_VERSION_PATCH 0
#define HELIOGRAPH_STRINGIFY_(x) #x
#define HELIOGRAPH_STRINGIFY(x) HELIOGRAPH_STRINGIFY_(x)
constexpr uint16_t kFirmwareMajor = HELIOGRAPH_VERSION_MAJOR;
constexpr uint16_t kFirmwareMinor = HELIOGRAPH_VERSION_MINOR;
constexpr uint16_t kFirmwarePatch = HELIOGRAPH_VERSION_PATCH;
constexpr const char* kFirmwareVersion =
    HELIOGRAPH_STRINGIFY(HELIOGRAPH_VERSION_MAJOR) "." HELIOGRAPH_STRINGIFY(HELIOGRAPH_VERSION_MINOR)
    "." HELIOGRAPH_STRINGIFY(HELIOGRAPH_VERSION_PATCH) " (" __DATE__ " " __TIME__ ")";

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

/// One entry per configured device, in poll order. Element 0 is the `driver` section; the rest
/// come from `additional_devices`. Held as parallel owning vectors rather than one struct so
/// the existing single-device call sites (g_driver / g_context / g_state below) keep meaning
/// exactly what they meant: the FIRST device.
std::vector<std::unique_ptr<InverterDriver>> g_drivers;
std::vector<std::unique_ptr<DeviceContext>>  g_contexts;

/// The first device, or nullptr. What is left that still speaks about "the" inverter: the
/// boot-confirm check, the REST /status device block, and the "is anything polling at all"
/// guard in the poll loop. Naming them rather than indexing at each call site keeps the
/// remaining single-device assumptions countable -- every use of g_driver/g_context/g_state is
/// a place that has not been taught about the others. The outputs no longer belong on that
/// list: MQTT, REST, Modbus TCP and Prometheus all carry every device.
InverterDriver* g_driver  = nullptr;
DeviceContext*  g_context = nullptr;
StateStore*     g_state   = nullptr;

/// Round-robin cursor for the poll loop, so a device whose backoff has expired does not always
/// lose to the one before it in the list.
size_t g_pollCursor = 0;

/// Device ids, index-aligned with g_contexts, so a log line can name the inverter it means.
std::vector<DeviceId> g_deviceIds;

/// The id of the CONFIGURED first device, if it started. Empty otherwise -- and empty is the
/// point: it is what stops a boot where device 1 failed from handing its MQTT topics, its Home
/// Assistant entities and its recorder history to whichever device did start.
DeviceId g_primaryDeviceId;

/// Whether the announced-device reconciliation is done for this boot. Once per session, and
/// only once the broker is actually connected: publishing the clears into a disconnected client
/// would drop them silently and then record the new list as if they had gone out. A refused
/// publish leaves this false so the next pass retries, bounded by the counter below.
bool     g_announcedReconciled = false;
unsigned g_announcedAttempts   = 0;

/// How many devices the configuration asked for, whether or not they started. Drives the status
/// LED: a configured device that failed to start is a fault to show, not an absence to ignore.
size_t g_devicesConfigured = 0;

/// One line per configured device that is not being polled, in configuration order. Filled at
/// boot alongside the log lines, so the same facts reach a screen instead of only a ring buffer.
std::vector<std::string> g_deviceProblems;

/// The started device at each CONFIGURED position, empty where one did not start.
///
/// This is what the Modbus unit ids are keyed on, and it exists because g_deviceIds is
/// compacted: a device that fails to start is simply absent from it, so keying unit ids on that
/// list would silently move every later inverter down one unit id. A client reading unit 2
/// would get inverter 3's watts with no way to notice, and unit ids are a wire contract nobody
/// re-derives after bring-up. Keyed on the configuration instead, an unstarted device's unit id
/// answers "offline, no data" -- which is the truth, and the same thing the Devices tab shows.
std::vector<DeviceId> g_configSlotIds;

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

/// esp_timer, NOT millis(): millis() is uint32 and wraps every 49.7 days, and casting the
/// wrapped value to uint64 does not un-wrap it. Every `now < deadline` comparison downstream
/// (MQTT reconnect back-off, WiFi retry schedule, poll due-time, relay rate limiter) would
/// see time jump backwards once per wrap and could stall until the next one — on a device
/// that is up for months, that is a scheduled outage. esp_timer_get_time() is a true 64-bit
/// microsecond counter: monotonic for ~292k years. Same fix in Rs485Transport::nowMs() and
/// the log-timestamp provider.
uint64_t nowMs() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

/// The crash dump the previous boot left behind, read ONCE in setup().
///
/// Not per request: reading it verifies a checksum over the whole stored image. It also cannot
/// change while we run -- a new dump is only written by a panic, and a panic does not come back
/// here -- so a cached copy is not merely an optimisation, it is the accurate model. Cleared in
/// place when the erase action succeeds, which is the one thing that CAN change it.
diag::CoredumpSummary g_coredump;

/// The bridge's DRM relays (empty on boards without them). Commands arrive on two tasks
/// (REST via AsyncTCP, MQTT via the client's task); g_relayMutex serialises them and the
/// state reads in bridgeInfo(). The controller itself stays lock-free and host-testable.
RelayController g_relays{nowMs};
std::mutex      g_relayMutex;

/// BOOT-hold factory reset and the status LED, on boards that carry them (board::kHasBootButton
/// / kHasStatusLed). Both are sampled from loop() only, so no locking: g_bootPressed and
/// g_statusLedColor are atomics purely so bridgeInfo() (loop + rs485Task) can read them for the
/// REST payload. 5 s hold, long enough that a factory reset is never one accidental brush.
status::HoldDetector        g_bootHold{5000};
std::atomic<bool>           g_bootPressed{false};
std::atomic<status::LedColor> g_statusLedColor{status::LedColor::Off};

/// Owns discovery runs. The web handler requests; rs485Task runs, because it owns the bus.
DiscoveryRunner g_discovery{g_registry, nowMs};

/// Owns passive bus captures, on the same terms and for the same reason.
CaptureRunner g_capture{nowMs};

/// The configured driver, or the highest-priority one compiled in. No manufacturer name here.
std::string selectedDriverId() {
    if (!g_config.driver.id.empty() && g_registry.contains(g_config.driver.id)) {
        return g_config.driver.id;
    }
    const auto available = g_registry.availableDrivers();
    return available.empty() ? std::string{} : available.front().id;
}

/// Puts the stored line-settings override back on the UART, and says what the line is either
/// way.
///
/// Called after EVERY begin(): at boot, and again after a discovery run, because begin()
/// unconditionally configures the driver's own first profile. Missing the second call meant
/// running discovery from the web UI on a healthy bridge silently reset the line and the
/// inverter went quiet until the next power cycle -- the same failure the override exists to
/// prevent, on the one path that reconfigures the line at runtime (review, 2026-07-25).
void applySerialOverride() {
    if (!g_config.serial.enabled) {
        // Logged even when nothing is overridden. Most bridges follow their driver, and without
        // this line an owner has no way to learn what the bus is actually running at -- which
        // matters the day a firmware update changes a driver's recommendation under them.
        if (g_driver) {
            Serial.println("[serial] line follows the driver's own profile");
        }
        return;
    }
    const auto& p = g_config.serial.profile;
    if (g_transport.configure(p)) {
        Serial.printf("[serial] override: %u baud, %u data bits, %s parity, %u stop\n",
                      static_cast<unsigned>(p.baudRate), static_cast<unsigned>(p.dataBits),
                      parityName(p.parity), static_cast<unsigned>(p.stopBits));
    } else {
        // configure() opens the UART before the direction-pin setup that is the only thing
        // that can fail, so on this path the line IS at the override's speed and framing --
        // what is missing is half-duplex direction control. Saying "the line is still at the
        // driver's setting" would have sent the reader to the wrong hypothesis entirely.
        Serial.printf("[serial] override applied at %u baud, but RS485 direction control "
                      "failed to configure; transmissions may collide\n",
                      static_cast<unsigned>(p.baudRate));
    }
}

BridgeInfo bridgeInfo() {
    BridgeInfo info;
    info.boardName        = board::kName;
    info.boardId          = board::kId;
    info.bridgeId         = g_wifi.bridgeId();
    {
        // bridgeInfo() runs on loop() and rs485Task; the AsyncTCP task can be replacing
        // g_config concurrently (see g_configMutex).
        std::lock_guard<std::mutex> lock(g_configMutex);
        info.name = g_config.bridgeName;
    }
    info.bridgeOnline     = true;
    info.uptimeSeconds    = static_cast<uint32_t>(nowMs() / 1000);  // good for 136 years
    info.freeHeapBytes    = ESP.getFreeHeap();
    info.minFreeHeapBytes = ESP.getMinFreeHeap();
    info.maxAllocHeapBytes = ESP.getMaxAllocHeap();
    // Separate from the three above, which are MALLOC_CAP_INTERNAL. Both accessors are guarded
    // by psramFound() inside the core, so a board without PSRAM reports 0 rather than failing.
    info.psramSizeBytes    = ESP.getPsramSize();
    info.psramFreeBytes    = ESP.getFreePsram();
    info.resetReason      = static_cast<uint16_t>(esp_reset_reason());
    info.wifiConnected    = g_wifi.connected();
    info.wifiRssiDbm      = g_wifi.rssi();
    info.ipAddress        = g_wifi.ipAddress();
    info.staticIp         = g_config.wifi.staticIp();
    info.mqttConnected    = g_mqtt && g_mqtt->connected();
    info.modbusListening  = g_modbus.running();
    info.modbusClients    = g_modbus.activeClients();
    info.firmwareVersion  = kFirmwareVersion;
    info.firmwareMajor    = kFirmwareMajor;
    info.firmwareMinor    = kFirmwareMinor;
    info.firmwarePatch    = kFirmwarePatch;
    info.timeSynced       = g_time.synced();
    info.currentEpoch     = static_cast<int64_t>(time(nullptr));
    info.lastNtpSyncEpoch = static_cast<int64_t>(g_time.lastSyncEpoch());
    const auto ntpSource  = g_time.syncSource();
    info.ntpServer        = ntpSource.server;
    info.ntpFromDhcp      = ntpSource.fromDhcp;
    info.otaImageState    = ota::imageStateName();
    info.coredumpPresent  = g_coredump.present;
    info.coredumpTask     = g_coredump.taskName;
    info.coredumpPc       = g_coredump.programCounter;
    if (g_relays.count() > 0) {
        {
            std::lock_guard<std::mutex> lock(g_relayMutex);
            info.relayCount    = g_relays.count();
            info.relaysEnabled = g_relays.enabled();
            for (uint8_t i = 0; i < g_relays.count(); ++i) {
                if (g_relays.energised(i)) {
                    info.relayMask |= static_cast<uint8_t>(1u << i);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            info.relayRoles = g_config.relays.roles;
        }
    }
    {
        // From the LIVE configuration, not the boot-time count. A device added on the settings
        // page takes effect at the next restart, so between save and reboot the boot count says
        // "1 configured, 1 polling" and the page cheerfully reports that everything is
        // accounted for -- while the configuration it just stored asks for two. Reading it here
        // makes the same screen say "polling 1 of 2", which is both true and the nudge to
        // restart (review, 2026-07-25).
        std::lock_guard<std::mutex> lock(g_configMutex);
        info.devicesConfigured =
            (g_config.driver.id.empty() ? 0 : 1) + g_config.additionalDevices.size();
    }
    // No lock: written once in setup(), before any task that reads them exists.
    info.devicesStarted = g_deviceIds.size();
    info.deviceProblems = g_deviceProblems;
    info.hasBootButton     = board::kHasBootButton;
    info.bootButtonPressed = g_bootPressed.load();
    info.hasStatusLed      = board::kHasStatusLed;
    if (board::kHasStatusLed) {
        info.statusLedColor = status::colorName(g_statusLedColor.load());
    }
    return info;
}

/// Applies a named DRM mode: the role's relays energised, everything else released.
/// The controller applies the pattern atomically behind its gates, charging ONE rate-limit
/// token for the whole mode switch. Charging per relay (the previous shape) made any role
/// spanning more relays than the burst impossible to assert, ever: the tail ONs always hit
/// the throttle and the rollback released the mode again.
CommandResult applyDrmMode(const std::string& mode) {
    std::vector<std::string> roles;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        roles = g_config.relays.roles;
    }
    roles.resize(g_relays.count(), "none");
    std::vector<bool> pattern;
    if (!drm::patternFor(roles, mode, pattern)) {
        return CommandResult::OutOfRange;
    }
    std::lock_guard<std::mutex> lock(g_relayMutex);
    return g_relays.applyPattern(pattern);
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
        const auto seen = std::find_if(unique.begin(), unique.end(),
                                       [&ssid](const Network& u) { return u.ssid == ssid; });
        if (seen == unique.end()) {
            unique.push_back({ssid, WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
        } else if (WiFi.RSSI(i) > seen->rssi) {
            // Same SSID on a second AP: keep the stronger one, so the list shows the radio the
            // bridge would actually associate with.
            seen->rssi = WiFi.RSSI(i);
            seen->open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
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

// --- Onboard indicators (BOOT-hold factory reset, status LED, buzzer) ----------------------
// All guarded by the board flags: on a board without them (the RS485-CAN, the 1CH) these are
// dead code the compiler drops, and no pin is touched. Sampled from loop() only.

void initOnboardIndicators() {
    if (board::kHasBootButton) {
        pinMode(board::kBootPin, INPUT_PULLUP);  // pressed reads LOW
    }
    if (board::kHasBuzzer) {
        pinMode(board::kBuzzerPin, OUTPUT);
        digitalWrite(board::kBuzzerPin, LOW);
    }
    if (board::kHasStatusLed) {
        rgbLedWrite(board::kStatusLedPin, 0, 0, 0);  // dark until the first health reading
    }
}

void beep(uint32_t ms) {
    if (!board::kHasBuzzer) {
        return;
    }
    // Active-high, transistor-driven. Blocking is fine: the only caller is the factory-reset
    // path, which reboots immediately afterwards.
    digitalWrite(board::kBuzzerPin, HIGH);
    delay(ms);
    digitalWrite(board::kBuzzerPin, LOW);
}

void driveStatusLed(const status::LedIndication& ind) {
    // Report the logical colour (steady, not the blink phase) so the REST payload reads
    // "red" throughout a factory-reset hold rather than flickering to "off".
    g_statusLedColor = ind.color;

    status::LedColor shown = ind.color;
    if (ind.blink && ((millis() / 300) % 2 == 0)) {
        shown = status::LedColor::Off;
    }
    // Only touch the RMT peripheral when the shown colour actually changes.
    static status::LedColor lastShown = status::LedColor::Off;
    static bool             everWrote = false;
    if (everWrote && shown == lastShown) {
        return;
    }
    everWrote = true;
    lastShown = shown;

    uint8_t r = 0, g = 0, b = 0;
    switch (shown) {
        case status::LedColor::Green: g = 40; break;
        case status::LedColor::Amber: r = 40; g = 18; break;  // warm amber, not yellow-green
        case status::LedColor::Red:   r = 40; break;
        case status::LedColor::Blue:  b = 40; break;
        case status::LedColor::Off:   break;
    }
    // Channel order: this WS2812 lights the RED element from rgbLedWrite's SECOND argument,
    // not the first -- a plain "green" (0,40,0) came out red on the first 6CH hardware run
    // (2026-07-23). So swap red and green here; blue is unaffected. rgbLedWrite's own GRB
    // timing conversion is fine, it is the element mapping on this board that is transposed.
    //
    // rgbLedWrite, not neopixelWrite: the latter is [[deprecated]] in Arduino core 3.x and is
    // now a thin forwarder to this one. Same signature, same behaviour.
    rgbLedWrite(board::kStatusLedPin, g, r, b);
}

/// One call per loop pass: sample BOOT, act on a completed hold, and refresh the LED.
void serviceOnboard() {
    bool holding = false;
    if (board::kHasBootButton) {
        const bool pressed = digitalRead(board::kBootPin) == LOW;
        g_bootPressed      = pressed;
        switch (g_bootHold.update(pressed, nowMs())) {
            case status::HoldDetector::Event::Holding:
                holding = true;
                break;
            case status::HoldDetector::Event::Triggered:
                log::warn("boot: BOOT held; factory reset requested");
                beep(400);  // audible confirmation before the wipe
                g_store.factoryReset();
                Serial.flush();
                ESP.restart();
                return;  // unreachable
            case status::HoldDetector::Event::Idle:
                break;
        }
    }
    if (board::kHasStatusLed) {
        status::LedInputs in;
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            in.provisioned   = g_config.provisioned();
            in.mqttEnabled   = g_config.mqtt.enabled;
            in.modbusEnabled = g_config.modbus.enabled;
        }
        in.factoryResetHolding = holding;
        in.wifiConnected       = g_wifi.connected();
        // "A device was configured", not "a device started". The old expression was
        // `g_driver != nullptr`, and the multi-device loop destroys the unique_ptr when begin()
        // fails -- so a bridge whose only configured driver refused to start went from RED to
        // GREEN, reporting "all healthy" while polling nothing (review, 2026-07-25).
        in.inverterExpected    = g_devicesConfigured > 0;
        in.mqttConnected       = g_mqtt && g_mqtt->connected();
        in.modbusListening      = g_modbus.running();
        // Worst-of across every polled device, not the first one. The LED is on the bridge, so
        // it reports the bridge: with three inverters on one bus it showed device 1 and stayed
        // green while the other two were dead -- the same defect as the web header (#38), on
        // the indicator someone standing at the bus is actually looking at. Its three states
        // are kept: red when any device is offline, amber when any is stale or invalid.
        in.inverterOnline = !g_deviceIds.empty();
        in.dataValid      = true;
        in.dataStale      = false;
        for (const auto& id : g_deviceIds) {
            if (StateHandle h = g_devices.state(id)) {
                in.inverterOnline = in.inverterOnline && h->inverterOnline;
                in.dataValid      = in.dataValid && h->dataValid;
                in.dataStale      = in.dataStale || h->dataStale;
            }
        }
        driveStatusLed(status::decide(in));
    }
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
        const modbus::ModbusServerConfig cfg = modbus::serverConfigFrom(
            configSnapshot.modbus, static_cast<uint8_t>(g_devicesConfigured));
        g_modbus.setConfig(cfg);
        const bool listening = g_modbus.begin();
        if (g_modbus.servedDevices() <= 1) {
            log::info("modbus: %s on :%u (unit %u)", listening ? "listening" : "failed to start",
                      cfg.port, cfg.inverterUnitId);
        } else {
            log::info("modbus: %s on :%u (units %u-%u, one per inverter)",
                      listening ? "listening" : "failed to start", cfg.port, cfg.inverterUnitId,
                      g_modbus.unitIdFor(g_modbus.servedDevices() - 1));
        }
        // Which unit is which inverter, once, at boot. Without it the only way to find out is
        // to read modbus.unit_id, fetch /api/v1/devices and count positions -- and on a bus of
        // identical inverters the per-device lines above are indistinguishable (review).
        for (size_t i = 0; i < g_configSlotIds.size() && i < g_modbus.servedDevices(); ++i) {
            log::info("modbus: unit %u -> %s", static_cast<unsigned>(g_modbus.unitIdFor(i)),
                      g_configSlotIds[i].empty() ? "(configured device did not start)"
                                                 : g_configSlotIds[i].c_str());
        }
        if (g_modbus.servedDevices() < g_devicesConfigured) {
            // Which of the two limits was hit, because the fix differs: lowering the base is no
            // help at all when the diagnostics unit is sitting inside the run (validate() only
            // stops it equalling unit_id, not landing a few above it).
            const int firstUnserved = cfg.inverterUnitId + g_modbus.servedDevices();
            log::warn("modbus: only %u of %u configured devices are reachable over Modbus TCP -- "
                      "unit %d is %s. %s",
                      static_cast<unsigned>(g_modbus.servedDevices()),
                      static_cast<unsigned>(g_devicesConfigured), firstUnserved,
                      firstUnserved == cfg.diagnosticsUnitId ? "the diagnostics unit"
                                                             : "past 247, the last valid address",
                      firstUnserved == cfg.diagnosticsUnitId
                          ? "Move modbus.diagnostics_unit_id out of the range."
                          : "Lower modbus.unit_id.");
        }
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
        if (g_relays.count() > 0) {
            g_mqtt->setRelayCommandHandler([](uint8_t index, bool on) {
                std::lock_guard<std::mutex> lock(g_relayMutex);
                return g_relays.set(index, on);
            });
            g_mqtt->setDrmCommandHandler(
                [](const std::string& mode) { return applyDrmMode(mode) == CommandResult::Ok; });
        }
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
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            g_config = c;
        }
        // Log level is live, not boot-only: the logger is a global whose level is a plain
        // setter. Without this a level change reported "Saved and applied" in the UI but did
        // nothing until a reboot -- and configChangeRequiresReboot() correctly omits it only
        // because this line makes the claim true.
        log::setLevel(c.logLevel);
        // The relay gates follow the config immediately -- no restart. Closing EITHER
        // gate also releases every relay: with the gate closed, no command -- not even
        // OFF -- would get through, so an energised contact would otherwise stay frozen
        // with DRM asserted and no way to release it. The failsafe direction (contacts
        // open, inverter runs) is the only state a closed gate may leave behind.
        {
            std::lock_guard<std::mutex> lock(g_relayMutex);
            g_relays.setReadOnlyMode(c.security.readOnlyMode);
            g_relays.setEnabled(c.relays.enabled);
            if (!c.relays.enabled || c.security.readOnlyMode) {
                g_relays.allOff();
            }
        }
    };
    ctx.bridgeInfo  = bridgeInfo;
    ctx.clock       = nowMs;
    // Configuration slot -> unit id, which is the mapping only this file knows.
    ctx.modbusUnitIdFor = [](const DeviceId& id) -> uint8_t {
        for (size_t i = 0; i < g_configSlotIds.size(); ++i) {
            if (g_configSlotIds[i] == id) {
                return g_modbus.running() ? g_modbus.unitIdFor(i) : 0;
            }
        }
        return 0;
    };
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
        g_rebootAtMs = nowMs() + 1500;
    };
    // Symmetric with requestCapture below, and it has to be: rs485Task checks discovery FIRST,
    // so a discovery accepted while a capture was merely pending would jump the queue and the
    // capture would then record the tail of the probe run -- exactly what the guard on the
    // other side exists to prevent. Guarding one direction only left that hole open.
    ctx.requestDiscovery = [](bool extended) {
        if (g_capture.busy()) {
            return false;
        }
        return g_discovery.request(extended);
    };
    ctx.discoveryReport     = [] { return g_discovery.report(); };
    // Refused while discovery is pending or running as well as while another capture is: both
    // want exclusive use of the same bus.
    ctx.requestCapture = [](const diag::CaptureConfig& config, const SerialProfile& profile) {
        if (g_discovery.busy()) {
            return false;
        }
        return g_capture.request(config, profile);
    };
    ctx.captureReport = [] { return g_capture.report(); };
    ctx.requestFactoryReset = [] { return g_store.factoryReset(); };
    // The undo behind a configuration restore. Straight through to the store, which owns both
    // the stash and the swap; the REST layer only decides when.
    ctx.stashRollback  = [] { return g_store.stashRollback(); };
    ctx.hasRollback    = [] { return g_store.hasRollback(); };
    ctx.rollbackConfig = [](Configuration& out) {
        const auto result = g_store.rollback(out);
        return result == LoadResult::Ok || result == LoadResult::Migrated;
    };
    // Clears a dump the operator has dealt with. Without it every later diagnostics read keeps
    // reporting the same old crash, and "is this new?" becomes unanswerable. The cached copy is
    // updated in the same breath so the next payload agrees with the flash.
    ctx.clearCoredump = [] {
        if (!diag::eraseCoredump()) {
            return false;
        }
        g_coredump = {};
        return true;
    };
    ctx.portalActive        = [] { return g_wifi.portalActive(); };
    ctx.scanNetworks        = scanNetworksJson;
    if (g_relays.count() > 0) {
        // Behind the same mutex as the MQTT path: REST commands arrive on the AsyncTCP
        // task, MQTT commands on the MQTT task.
        ctx.setRelay = [](uint8_t index, bool on) {
            std::lock_guard<std::mutex> lock(g_relayMutex);
            return g_relays.set(index, on);
        };
        ctx.setDrmMode = applyDrmMode;
    }

    g_rest = std::make_unique<rest::RestApi>(ctx);
    g_rest->begin();
}

/// Clears the retained topics of devices that are no longer where we announced them.
///
/// Retained discovery configs outlive the device that published them, and because availability
/// is bridge-scoped an orphaned entity does not go unavailable -- it reports ONLINE forever with
/// its last value, and anything summing the inverters keeps counting it. What we announced last
/// time is the one fact nothing else survives a reboot knowing.
void reconcileAnnouncedDevices(const BridgeInfo& bridge) {
    // Only when every configured device actually started. g_deviceIds holds the devices that
    // STARTED; one skipped for a duplicate id, a driver that is not compiled in or a full slot
    // table is still configured, and tearing its Home Assistant entities down over a typo that
    // gets corrected in a minute is worse than leaving them one boot longer. Nothing here can
    // know the id of a device that never got a driver -- the id comes from the driver -- so the
    // honest move is to defer the whole reconciliation and leave the bookkeeping untouched
    // (review, 2026-07-26). Zero configured devices lands here too, and never reaches this
    // function at all: with nothing started there is no state and MQTT does not run.
    if (g_deviceIds.size() != g_devicesConfigured) {
        log::warn("MQTT: not clearing retained device topics this boot -- %u of %u configured "
                  "devices started; fix them and reboot",
                  static_cast<unsigned>(g_deviceIds.size()),
                  static_cast<unsigned>(g_devicesConfigured));
        g_announcedReconciled = true;
        return;
    }

    const auto previous = g_store.announcement();
    // Say it, once, if the tree moved. Nothing is cleared automatically: the retained payloads
    // under an old base topic are litter, but the discovery configs under an old prefix are the
    // live Home Assistant entities if only the base topic changed -- deviceUniqueBase() does not
    // include it, so those configs are OVERWRITTEN in place rather than orphaned. An automatic
    // sweep that gets that distinction wrong deletes a working device from someone's dashboard
    // while nobody is watching, which is why no comparable project does this in firmware either
    // (ESPHome ships `esphome clean-mqtt`, Tasmota points at its Device Manager -- both host-side
    // tools a human runs). What the bridge CAN do that an external script cannot is know where
    // its own topics used to be, so it names them. See docs/mqtt.md and issue #41.
    if (previous.prefixesKnown()) {
        if (previous.baseTopic != g_config.mqtt.baseTopic) {
            log::warn("MQTT: base topic changed %s -> %s; retained payloads under "
                      "%s/%s/... are no longer updated (docs/mqtt.md explains how to clear them)",
                      previous.baseTopic.c_str(), g_config.mqtt.baseTopic.c_str(),
                      previous.baseTopic.c_str(), bridge.bridgeId.c_str());
        }
        if (previous.discoveryPrefix != g_config.mqtt.discoveryPrefix) {
            log::warn("MQTT: discovery prefix changed %s -> %s; Home Assistant will keep the old "
                      "entities until the retained configs under %s/ are cleared",
                      previous.discoveryPrefix.c_str(), g_config.mqtt.discoveryPrefix.c_str(),
                      previous.discoveryPrefix.c_str());
        }
    }

    mqtt::AnnouncementRecord record;
    record.baseTopic       = g_config.mqtt.baseTopic;
    record.discoveryPrefix = g_config.mqtt.discoveryPrefix;
    record.devices.reserve(g_deviceIds.size());
    for (const auto& id : g_deviceIds) {
        record.devices.push_back({id, id == g_primaryDeviceId});
    }

    bool allCleared = true;
    for (const auto& id :
         mqtt::devicesToForget(previous.devices, g_deviceIds, g_primaryDeviceId)) {
        if (!g_mqtt->forgetDevice(id, bridge)) {
            allCleared = false;
            // Kept in the record even though it is gone from the configuration: dropping it
            // here would be the last time anything knew it existed, and its entities would then
            // be orphaned for good.
            record.devices.push_back({id, false});
        }
    }
    if (!allCleared && ++g_announcedAttempts < 5) {
        return;  // link hiccup or a full outbox; try again next pass
    }
    if (!allCleared) {
        log::warn("MQTT: could not clear every removed device's topics; will retry next boot");
    }
    if (!g_store.setAnnouncement(record)) {
        log::warn("MQTT: could not record which devices were announced; removing one later will "
                  "leave its Home Assistant entities behind");
    }
    g_announcedReconciled = true;
}

/// Puts every driver back on the bus and the line back where it belongs.
///
/// Needed after anything that took the bus away from the poll loop and left it changed.
/// Discovery re-registers every inverter and resets the line to the driver's own first
/// profile; a capture leaves the line on whatever the operator asked to listen at. Both end
/// with a bus the driver no longer agrees with, so both end here.
void restoreDriverLine() {
    for (auto& d : g_drivers) {
        d->begin(g_transport);
    }
    if (!g_drivers.empty()) {
        // begin() has just put the line back on the driver's own first profile, exactly as it
        // does at boot. Without this, running discovery from the web UI on a healthy bridge
        // silently undid the stored override and the inverter went quiet until the next power
        // cycle -- the same failure that override exists to prevent, on the one path that
        // reconfigures the line at runtime.
        applySerialOverride();
    }
}

void rs485Task(void* /*arg*/) {
    for (;;) {
        // One feed per iteration. The 120 s budget covers a normal iteration; an extended
        // discovery run no longer fits inside one feed and provides its own, per probe -- see
        // the call below and the watchdog setup in setup().
        esp_task_wdt_reset();
        // Own stack headroom into diagnostics: the 8192 sizing rests on one measured crash
        // (2026-07-19); this keeps creeping growth visible in the API instead of leaving
        // the canary as the only witness. ESP-IDF returns BYTES (StackType_t is uint8_t
        // on xtensa); the scan only walks the unused region -- microseconds.
        g_diagnostics.recordRs485StackFree(
            static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)));
        // Discovery first, and instead of polling this cycle: probing re-registers every
        // inverter on the bus, so the two must never interleave. This task owns the bus, which
        // is why the web handler only ever *requests* a run.
        // The watchdog is fed per PROBE, not once per iteration: an extended run now sweeps
        // eight addresses per driver per profile, and on a silent bus every one of those is a
        // response timeout. The one feed above covered a run that was a handful of probes long.
        if (g_discovery.runIfRequested(g_transport, [] { esp_task_wdt_reset(); })) {
            Serial.printf("[discovery] %s\n", g_discovery.report().outcome.reason.c_str());
            // The probe left the bus re-registered and the line on the driver's default; make
            // every device pick that up rather than poll a stale address.
            restoreDriverLine();
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        // A capture, on exactly the same terms: instead of this cycle's poll, not alongside it.
        // That is the whole concurrency answer -- there is no iteration in which the bus is
        // being listened to AND polled, so the two cannot interleave by construction.
        //
        // The watchdog is fed per read iteration, not once here: a window is tens of seconds
        // inside this single iteration, which the 120 s budget above would not survive on its
        // own at the maximum window length.
        if (g_capture.runIfRequested(g_transport, [] { esp_task_wdt_reset(); })) {
            const auto report = g_capture.report();
            log::info("capture: %u frames, %u bytes, %u with a valid Modbus CRC",
                      static_cast<unsigned>(report.frames.size()),
                      static_cast<unsigned>(report.totalBytes),
                      static_cast<unsigned>(report.modbusFrames));
            // Only when the line was actually changed. The capture listens at settings the
            // operator chose, which for an unidentified device is precisely NOT the driver's,
            // so leaving them in place would silence a working inverter until the next reboot.
            // But a run that could not take the bus never got that far, and re-running begin()
            // on every driver is a registration handshake on the AA55 family -- real traffic,
            // on a bus that just said it was busy.
            if (report.lineReconfigured) {
                restoreDriverLine();
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        // At most ONE device per iteration, round-robin. Not a loop over all of them: the
        // watchdog is fed once per iteration, and a bus of eight silent devices would spend
        // eight transaction deadlines back to back before the outputs below ever ran. One per
        // pass keeps the loop's timing independent of how many inverters are chained, and the
        // cursor keeps a device whose backoff has expired from always losing to the one before
        // it in the list.
        //
        // The manual-poll request is honoured by whichever device comes up first. exchange()
        // clears the flag, so one request is still one poll.
        if (!g_contexts.empty()) {
            // A manual poll always goes to device 1, never to whichever device the cursor
            // happens to sit on. The wizard's test poll and POST /actions/poll are both read
            // back through /api/v1/status, which serves the FIRST device -- so sending the poll
            // anywhere else made the button report that nothing had happened.
            if (g_manualPollRequested.exchange(false)) {
                const PollResult r = g_contexts.front()->pollOnce();
                if (r != PollResult::Ok) {
                    log::warn("manual poll of %s: %s", g_deviceIds.front().c_str(),
                              pollResultName(r));
                }
            }
            for (size_t i = 0; i < g_contexts.size(); ++i) {
                const size_t index = (g_pollCursor + i) % g_contexts.size();
                DeviceContext& ctx = *g_contexts[index];
                if (!ctx.due(nowMs())) {
                    continue;
                }
                const PollResult r = ctx.pollOnce();
                if (r != PollResult::Ok) {
                    // Bounded: one line per attempt, no payload, no growth over time. The device
                    // ID rather than a position: a position among the STARTED devices is not the
                    // position in the config, so "device 2" would name the wrong inverter as
                    // soon as one failed to start -- and it carries the address, which is what
                    // someone standing at the bus actually needs.
                    log::warn("poll %s: %s", g_deviceIds[index].c_str(), pollResultName(r));
                }
                g_pollCursor = (index + 1) % g_contexts.size();
                break;
            }
        }
        // Every output takes every device: MQTT/Home Assistant by topic subtree, REST by device
        // id, Modbus TCP by unit id, Prometheus by a `device` label. docs/architecture.md has
        // the table of what each one did about backwards compatibility for device 1.
        if (g_state) {
            const auto bridge = bridgeInfo();
            const auto diag   = g_diagnostics.snapshot();
            // Snapshots held for the whole block: MqttOutput::DeviceView keeps raw pointers
            // into them, so the shared_ptrs have to outlive the loop() call.
            std::vector<StateHandle>               held;
            std::vector<mqtt::MqttOutput::DeviceView> views;
            held.reserve(g_deviceIds.size());
            views.reserve(g_deviceIds.size());
            for (const auto& id : g_deviceIds) {
                if (StateHandle h = g_devices.state(id)) {
                    held.push_back(std::move(h));
                    // Primary is the CONFIGURED first device, not the first that happened to
                    // start. When it fails to start no device is primary and nobody inherits
                    // its topics, its Home Assistant entities or its history.
                    views.push_back({id, held.back().get(), id == g_primaryDeviceId});
                }
            }
            const auto first = g_state->snapshot();
            // Every device, one Modbus unit id each: configuration slot i is served at
            // inverterUnitId + i (#36).
            //
            // Keyed on g_configSlotIds -- the CONFIGURED positions -- not on g_deviceIds, which
            // is compacted. A device that failed to start is absent from the latter, so keying
            // on it moved every later inverter down a unit id: a client reading unit 2 would
            // get inverter 3's watts with no way to notice, and a unit id is a wire contract
            // nobody re-derives after bring-up. A slot with no device is passed as null;
            // refresh() leaves its map at the constructed sentinels, so that unit answers
            // offline with no readings rather than someone else's.
            std::vector<const DeviceState*> modbusDevices;
            modbusDevices.reserve(g_configSlotIds.size());
            for (const auto& slotId : g_configSlotIds) {
                const auto it = std::find_if(views.begin(), views.end(),
                                             [&slotId](const mqtt::MqttOutput::DeviceView& v) {
                                                 return !slotId.empty() && v.id == slotId;
                                             });
                modbusDevices.push_back(it == views.end() ? nullptr : it->state);
            }
            g_modbus.refresh(modbusDevices, bridge, diag, nowMs());
            if (g_mqtt) {
                // Once, on the first connected pass -- before loop() below, so the clears are
                // queued ahead of this boot's own discovery announcements.
                if (!g_announcedReconciled && g_mqtt->connected()) {
                    reconcileAnnouncedDevices(bridge);
                }
                g_mqtt->loop(views, bridge, diag, nowMs());
            }
            if (g_rest) {
                g_rest->notifyState(*first, nowMs());
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
    // Headless operation is the normal state: no USB host for months. HWCDC's default TX
    // timeout is 100 ms PER WRITE once the 256-byte ring fills with nobody draining it, and
    // the logger runs on rs485Task too -- every log line would then stall polling for up to
    // that timeout. Zero means drop-when-full: the REST log ring keeps every line anyway.
    Serial.setTxTimeoutMs(0);

    Serial.printf("\nHeliograph %s\nboard: %s\nreset reason: %d\n", kFirmwareVersion,
                  board::kName, static_cast<int>(esp_reset_reason()));

    // From the very first log line: uptime stamps until the clock is valid, wall-clock the
    // moment it is -- which on RTC boards is seconds from now, long before any network.
    TimeManager::installLogTimestamps();

    const auto loaded = g_store.load(g_config);
    // Apply the stored level before anything else logs, or the first lines ignore it.
    log::setLevel(g_config.logLevel);

    // TZ before the FIRST log:: call, not merely before the RTC restore. formatLogTimestamp
    // renders through localtime_r, so a stamped line written while TZ is unset comes out in
    // UTC with nothing saying so. That is not hypothetical: a warm reset (OTA reboot, the
    // reboot action) keeps the system clock across the restart, so the clock is already valid
    // here and the config line below shipped an hour or two in the past on every OTA -- the
    // exact boot whose log gets read. Seen on 0.13.0, 2026-07-26. A cold start hid it: the
    // clock is not valid yet at this point, so the line carries an uptime stamp instead.
    //
    // This is as early as it can go. The zone comes out of the stored configuration, so the
    // load above has to have happened -- which is fine, because nothing on that path logs.
    setenv("TZ", g_config.ntp.timezone.c_str(), 1);
    tzset();

    log::info("config: %s (log level %s)", loadResultName(loaded), logLevelName(g_config.logLevel));

    // Once per boot, before anything else can panic on top of it. A dump here means the PREVIOUS
    // run crashed; saying so in the boot log is half the value, because that log is what gets
    // read after an unexplained reboot.
    g_coredump = diag::readCoredumpSummary();
    if (g_coredump.present) {
        log::warn("coredump: previous boot crashed in task '%s' at pc 0x%08lX "
                  "(clear it from Diagnostics once handled)",
                  g_coredump.taskName.empty() ? "?" : g_coredump.taskName.c_str(),
                  static_cast<unsigned long>(g_coredump.programCounter));
    }
    if (loaded == LoadResult::Corrupt || loaded == LoadResult::FutureVersion) {
        // Defaults, so the device lands in the setup portal. Better than running on values we
        // could not parse or do not understand.
        Serial.println("[config] falling back to defaults; the setup portal will start");
    }

    // RTC restore, before anything else that logs at length: with a battery-backed
    // PCF85063 (board::kHasRtc) the system clock is valid from here on, so every log line
    // below carries a wall-clock timestamp even when the network never comes up -- which
    // is exactly the boot you end up debugging.
    if (rtc::begin()) {
        time_t stored = 0;
        if (rtc::readUtc(stored)) {
            const timeval tv{stored, 0};
            settimeofday(&tv, nullptr);
            char         buf[24];
            const size_t n = log::formatIsoLocalTime(buf, sizeof(buf), stored);
            // formatIsoLocalTime leaves buf untouched when localtime_r rejects the epoch or
            // strftime does not fit, so printing it unconditionally would read uninitialised
            // stack. Same guard as the NTP-sync line in time_manager.cpp.
            log::info("rtc: clock restored: %s (awaiting ntp for drift correction)",
                      n > 0 ? buf : "?");
        } else {
            // Deliberately does not name a cause. readUtc() refuses for five distinct reasons
            // -- oscillator stopped, a byte that is not valid BCD, a field out of range, a
            // year outside 2024-2099, or a date that does not exist -- and only the first is
            // "flat backup cell". Naming that one sent someone to replace a battery over what
            // was actually a wiring or I2C fault (review, 2026-07-25).
            log::warn("rtc: present but gave no usable time; running without it until ntp "
                      "(flat backup cell, or a bad read -- check the I2C wiring if it persists)");
        }
    }

    // Relays, on boards that have them: pins to OUTPUT and everything de-energised before
    // anything else can fail. The gates start closed (read-only on, enabled off) and only
    // the config below opens them. Under HELIOGRAPH_MOCK_RELAYS the mock build exposes
    // virtual relays through the full MQTT/REST/HA stack without touching a single pin.
#if defined(HELIOGRAPH_MOCK_RELAYS)
    g_relays.begin(HELIOGRAPH_MOCK_RELAYS, [](uint8_t i, bool on) {
        log::info("relay[mock] %u -> %s", i + 1, on ? "ON" : "OFF");
    });
#else
    if (board::kRelayCount > 0) {
        for (int i = 0; i < board::kRelayCount; ++i) {
            pinMode(board::kRelayPins[i], OUTPUT);
        }
        g_relays.begin(static_cast<uint8_t>(board::kRelayCount), [](uint8_t i, bool on) {
            digitalWrite(board::kRelayPins[i],
                         (on == board::kRelayActiveHigh) ? HIGH : LOW);
        });
    }
#endif
    g_relays.setReadOnlyMode(g_config.security.readOnlyMode);
    g_relays.setEnabled(g_config.relays.enabled);

    // BOOT button, status LED and buzzer on boards that have them (6CH). No-op elsewhere.
    initOnboardIndicators();

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
    // Device 1 comes from `driver`, the rest from `additional_devices`, in that order. One list
    // so the poll loop has one thing to walk, and so a bring-up log reads in the same order the
    // settings page shows.
    struct Planned { std::string id; const DriverOptions* options; std::string label; };
    std::vector<Planned> planned;
    if (!driverId.empty()) {
        planned.push_back({driverId, &g_config.driver.options, g_config.driver.label});
    }
    for (const auto& d : g_config.additionalDevices) {
        planned.push_back({d.id, &d.options, d.label});
    }
    g_devicesConfigured = planned.size();
    // One slot per CONFIGURED device, filled in as each one starts. The Modbus unit ids are
    // keyed on this, not on the compacted g_deviceIds -- see the declaration.
    g_configSlotIds.assign(planned.size(), DeviceId{});
    if (planned.empty()) {
        log::warn("no driver configured and none compiled in; nothing will be polled");
    }

    for (size_t plannedIndex = 0; plannedIndex < planned.size(); ++plannedIndex) {
        const auto& p = planned[plannedIndex];
        // "Device 1" is the `driver` section; 2..N are additional_devices, which is how the
        // settings page numbers them. Naming only the driver id was useless on the very bus
        // this exists for: three inverters share one driver id, so all three failures read
        // identically (review, 2026-07-25).
        // The label goes in the PROBLEM text too. "device 2 could not be started" sends someone
        // to count rows on the settings page; "device 2 (Schuur) could not be started" sends
        // them to the shed. The row number stays, because that is what the settings page shows
        // and an unlabelled bridge still needs to be told which row.
        const std::string row = p.label.empty()
                                    ? "device " + std::to_string(plannedIndex + 1)
                                    : "device " + std::to_string(plannedIndex + 1) + " (" +
                                          p.label + ")";

        // Refused BEFORE create() and begin(), because for a driver that cannot share a bus,
        // begin() IS the damage. The AA55 handshake opens with a bus-wide RE_REGISTER
        // broadcast, so a second instance starting up tells the first, already-polling inverter
        // to forget its address -- and a check that ran afterwards would report the refusal
        // from the far side of the harm (#63).
        //
        // Enforced here rather than in config validation because this is where the registry is:
        // the answer is a property of the driver, not of the configuration file, and the config
        // layer deliberately knows nothing about drivers.
        const auto* descriptor = g_registry.find(p.id);
        if (descriptor != nullptr && !descriptor->supportsMultipleDevices) {
            bool alreadyPlanned = false;
            for (size_t earlier = 0; earlier < plannedIndex; ++earlier) {
                if (planned[earlier].id == p.id) {
                    alreadyPlanned = true;
                    break;
                }
            }
            if (alreadyPlanned) {
                log::warn("device '%s' skipped: this driver supports only one device per bridge",
                          p.id.c_str());
                g_deviceProblems.push_back(row + " ('" + p.id +
                                           "') was not started: this driver supports only one "
                                           "device per bridge");
                continue;
            }
        }

        auto driver = g_registry.create(p.id, g_transport, *p.options);
        if (!driver || !driver->begin(g_transport)) {
            // Named, and the loop continues: one unconfigurable device must not cost the others
            // their poll. A bus with three inverters where the second has a typo'd driver id
            // should still report the first and third.
            log::warn("device '%s' could not be started; the others still poll", p.id.c_str());
            g_deviceProblems.push_back(row + " ('" + p.id + "') could not be started");
            continue;
        }
        Serial.printf("[driver] %s (%s)\n", driver->descriptor().id.c_str(),
                      supportLevelName(driver->descriptor().supportLevel));
        const DeviceId id = driver->identity().deviceId();
        // Checked BEFORE add(), because add() is idempotent: re-adding a known id hands back
        // the existing store rather than refusing. That is right for a caller re-registering
        // the same device, and exactly wrong here -- two configured devices sharing an id would
        // have silently shared one store and overwritten each other's readings into one set of
        // Home Assistant entities. An earlier version of this loop treated a null return as the
        // collision signal; it never came (review, 2026-07-25).
        if (g_devices.contains(id)) {
            log::warn("device '%s' skipped: another configured device already resolves to id "
                      "'%s' -- give them different addresses", p.id.c_str(), id.c_str());
            g_deviceProblems.push_back(row + " ('" + p.id + "') resolves to " + id +
                                       ", which another configured device already uses; give "
                                       "them different addresses");
            continue;
        }
        StateStore* store = g_devices.add(id);
        if (store == nullptr) {
            log::warn("device '%s' skipped: no free device slot (max %u)", p.id.c_str(),
                      static_cast<unsigned>(kMaxDevices));
            g_deviceProblems.push_back(row + " ('" + p.id + "') has no free device slot");
            continue;
        }
        PollPolicy policy;
        policy.intervalMs = g_config.polling.intervalSeconds * 1000;
        g_contexts.push_back(std::make_unique<DeviceContext>(*driver, *store, g_diagnostics,
                                                             nowMs, policy, p.label));
        g_deviceIds.push_back(id);
        g_configSlotIds[plannedIndex] = id;
        // `planned` is in configuration order and `p` is the entry being started, so this is
        // true only for the first configured device -- never for a later one that starts first.
        if (&p == &planned.front()) {
            g_primaryDeviceId = id;
        }
        g_drivers.push_back(std::move(driver));
        if (g_driver == nullptr) {
            g_driver  = g_drivers.front().get();
            g_context = g_contexts.front().get();
            g_state   = store;
        }
    }
    if (!g_drivers.empty()) {
        // Once, after the LAST begin(): every begin() reconfigures the line to its own driver's
        // first profile, so applying the override per device would only be undone by the next
        // one. All devices share the bus, so there is one line to set, not one per device --
        // which also means that on a MIXED bus, with the override off, the line is left on the
        // last driver's profile. Drivers whose profiles differ need the override set explicitly.
        applySerialOverride();
        if (g_drivers.size() > 1) {
            Serial.printf("[driver] polling %u devices in turn on one bus\n",
                          static_cast<unsigned>(g_drivers.size()));
        }
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
    //
    // Side effect worth naming: esp_task_wdt_reconfigure applies to EVERY watched task,
    // including the core-0 idle task kept by idle_core_mask. So idle starvation on core 0,
    // which the IDF default would have caught in 5 s, now takes two minutes to trip. The API
    // has no per-task timeout, so the choice is this or no idle coverage at all; discovery
    // needs the headroom more than idle starvation needs the faster trip.
    //
    // Not watched at all: the AsyncTCP task behind the web server and the task espMqttClient
    // creates for itself. Registering a task whose blocking behaviour we do not control risks
    // panicking a healthy device, so their liveness is reported through diagnostics instead
    // (webLastServiceMs / mqttLastServiceMs) and left for a human or an alert to act on.
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
    serviceOnboard();  // BOOT-hold factory reset + status LED (no-op on boards without them)

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
        // Say when the ceiling is reached, because eModbus will not: past maxNoClients it
        // accepts the socket, closes it and logs nothing (ModbusServerTCPasync::onClientConnect).
        // From outside that is a client which connects and is never answered, and the only way
        // to find out why is to go and read eModbus's source -- which is exactly what the
        // 4-of-6 hardware result on 2026-07-27 cost (#71).
        //
        // This reports being AT the limit, not a refusal. Counting refusals would need a hook
        // eModbus does not offer. Being at the limit is the condition under which the next
        // connection is refused, which is the part worth knowing in advance.
        //
        // Edge-triggered: the limit stays reached for as long as the clients stay connected,
        // and a warning every loop pass would bury the log it belongs in.
        const uint16_t limit = g_modbus.config().maxClients;
        if (nowClients >= limit && prevModbusClients < limit) {
            log::warn("modbus: %u of %u client slots in use -- further connections are refused "
                      "until one frees. Raise modbus.max_clients if this is normal here.",
                      static_cast<unsigned>(nowClients), static_cast<unsigned>(limit));
        }
        prevModbusClients = nowClients;
    }

    static uint32_t lastReport = 0;
    if (millis() - lastReport > 10000) {
        lastReport = millis();
        // Same per-task self-report as rs485Task; see the note there.
        g_diagnostics.recordLoopStackFree(
            static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)));
        // THE WHOLE FLEET, not the first device (#74). This used to read g_state, which is
        // assigned once while starting devices and then holds device 1 forever: on a bus of
        // four, three could be dead and this line would report the healthy one every ten
        // seconds. It also printed `inverter=%d` from a bool, so "1" meant online and read as
        // a count -- identical output for one inverter and for eight.
        //
        // Summed by rest::totalsFor so the heartbeat and /api/v1/status cannot disagree about
        // what "answering" means.
        //
        // Through the logger, not a raw print: the heartbeat then carries the same wall-clock
        // stamp as everything else, which is what makes an unattended capture legible.
        {
            const uint64_t                   now = nowMs();
            std::vector<rest::DeviceSummary> fleet;
            fleet.reserve(g_deviceIds.size());
            for (const auto& id : g_deviceIds) {
                if (StateHandle h = g_devices.state(id)) {
                    fleet.push_back(rest::summariseDevice(*h, id, now));
                }
            }
            const auto t = rest::totalsFor(fleet);
            log::info("state: wifi=%s devices=%u/%u answering power=%s heap=%lu",
                      provisioningStateName(g_wifi.state()), t.answering,
                      static_cast<unsigned>(g_devicesConfigured),
                      t.acCount != 0 ? String(t.acPowerW, 1).c_str() : "unknown",
                      static_cast<unsigned long>(ESP.getFreeHeap()));
            // One line each for the devices that are NOT answering, and none at all when the
            // fleet is healthy. A capture from a working bridge stays one line per heartbeat;
            // a sick one names the inverter instead of leaving it to be deduced from a count.
            // Bounded by kMaxDevices, so this can never be more than eight lines.
            for (const auto& f : fleet) {
                if (f.online && f.dataValid && !f.dataStale) {
                    continue;
                }
                if (f.everPolled) {
                    // WHY, not just that. The old line carried valid= and stale= for the first
                    // device, and folding three causes into one phrase would have dropped what
                    // those flags were for. Offline before stale: a device that drops is marked
                    // offline AND stale by markAllStale(), and the offline is the cause.
                    const char* why = !f.online          ? "offline"
                                      : f.dataStale      ? "stale"
                                                         : "no valid reading";
                    log::info("state:   %s not answering (%s, last reply %us ago)",
                              rest::displayName(f).c_str(), why,
                              static_cast<unsigned>(f.lastPollSecondsAgo));
                } else {
                    // Never a byte since boot: a bus or addressing fault, not an inverter that
                    // went quiet, and the two need different things done about them.
                    log::info("state:   %s has never answered", rest::displayName(f).c_str());
                }
            }
            // A device that never STARTED is not in the fleet at all -- it has no driver, so it
            // has no id to print (see the reconciliation note above). Without this the count
            // would read "3/4" with nothing named, which is the one case a reader cannot tell
            // apart from a bug in the count. Said at boot too, but a capture taken hours later
            // has long since lost that line out of the ring.
            for (const auto& p : g_deviceProblems) {
                log::info("state:   %s", p.c_str());
            }
        }
    }
    delay(100);
}
