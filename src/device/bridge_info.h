// SPDX-License-Identifier: MIT
//
// Facts about the bridge itself, as opposed to the inverter it talks to. Every output adapter
// needs these, so they do not belong to any one of them.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace heliograph {

struct BridgeInfo {
    /// Stable id derived from the MAC, e.g. "heliograph-a1b2c3". Used in MQTT topics and as
    /// the Home Assistant device identifier, so it must not change across reboots.
    std::string bridgeId = "heliograph";
    std::string name     = "Heliograph";

    bool     bridgeOnline     = false;
    uint32_t uptimeSeconds    = 0;
    uint32_t freeHeapBytes    = 0;
    uint32_t minFreeHeapBytes = 0;
    /// Largest single allocatable block. THE fragmentation signal: free heap can look
    /// healthy while no allocation of consequence fits anymore, which is exactly the
    /// failure mode a months-uptime device grows into.
    uint32_t maxAllocHeapBytes = 0;
    /// External PSRAM, reported separately because the three figures above do NOT include it.
    ///
    /// ESP.getFreeHeap(), getMinFreeHeap() and getMaxAllocHeap() are all
    /// heap_caps_*(MALLOC_CAP_INTERNAL) in Arduino core 3.x -- internal SRAM only. So on a
    /// board with 8 MB of PSRAM every heap number this struct carried described about 300 KB
    /// of it, and a board where PSRAM failed to train looked exactly like one where it worked.
    /// The RS485-CAN and Relay-1CH have 8 MB; the Relay-6CH is an N8 with none (audit,
    /// 2026-07-26).
    ///
    /// `psramSizeBytes == 0` means this board has no PSRAM, or it did not initialise -- the two
    /// are indistinguishable from software and both are worth seeing. Outputs report absent
    /// rather than zero, per the house rule.
    uint32_t psramSizeBytes    = 0;
    uint32_t psramFreeBytes    = 0;
    uint16_t resetReason       = 0;

    bool    wifiConnected  = false;
    int16_t wifiRssiDbm    = 0;
    bool    mqttConnected  = false;
    bool    modbusListening = false;
    uint16_t modbusClients  = 0;

    uint16_t    firmwareMajor   = 0;
    uint16_t    firmwareMinor   = 1;
    uint16_t    firmwarePatch   = 0;
    std::string firmwareVersion = "0.1.0";

    /// Wall-clock state. `timeSynced` is the honesty gate: while false, the epochs below are
    /// 1970-ish garbage and every output must publish null, never a formatted fake date.
    bool    timeSynced       = false;
    int64_t currentEpoch     = 0;  ///< time(nullptr) at snapshot
    int64_t lastNtpSyncEpoch = 0;  ///< wall-clock moment of the last SNTP sync; 0 = never
    /// The NTP server that actually answered (name or IP as text), empty when that cannot
    /// be determined -- outputs then publish null rather than guessing.
    std::string ntpServer;
    bool        ntpFromDhcp = false;  ///< true: DHCP option 42 supplied it; false: configured

    /// The running image's otadata state ("pending_verify" until the healthy-boot
    /// confirmation, then "valid"). Makes the rollback window observable in diagnostics.
    std::string otaImageState = "unknown";

    /// The crash dump waiting in the `coredump` partition, read once at boot.
    ///
    /// Same kind of fact as otaImageState above: a one-shot read of what the bootloader left
    /// behind, carried here so the outputs never call an ESP-IDF function themselves. Reading
    /// it verifies a checksum over the whole stored image, so it happens once in setup(), not
    /// per request.
    ///
    /// `coredumpPresent` false is the normal state, and the state after an erase; the other
    /// two are meaningless then and every output reports them absent rather than as task ""
    /// at PC 0.
    bool        coredumpPresent = false;
    std::string coredumpTask;
    uint32_t    coredumpPc      = 0;

    /// The board this firmware is running on. Reported to Home Assistant as the bridge
    /// device's model.
    /// Set by main from board::kName; the default only serves host tests, which have no
    /// board header.
    std::string boardName = "Waveshare ESP32-S3-RS485-CAN";

    /// Bridge-local relays (DRM curtailment contacts on relay boards). Count 0 = the board
    /// has none, and every output omits the topic/field entirely -- absent, not zero, per
    /// the house rule. `relayMask` bit i = relay i energised; `relaysEnabled` mirrors the
    /// config flag so outputs can announce switches only when they can actually act.
    uint8_t relayCount    = 0;
    uint8_t relayMask     = 0;
    bool    relaysEnabled = false;
    /// Per-relay DRM role from the configuration (index-aligned; may be shorter than
    /// relayCount, missing = "none"). Drives switch names and the DRM mode select.
    std::vector<std::string> relayRoles;

    /// How many devices the configuration asks for, and what went wrong with the ones that are
    /// not being polled.
    ///
    /// The firmware already knew both at boot and said so in one log line each. Nothing else
    /// surfaced them, so a configured inverter that collided on an address or refused to start
    /// was invisible on every screen: the device list showed the ones that worked, the dashboard
    /// showed one, and the difference was a warn in a ring buffer. Every mistake the settings
    /// page can produce lands here, which is why it is here and not only in the log.
    size_t                   devicesConfigured = 0;
    /// How many of them the firmware actually managed to create at boot. Started is not
    /// answering -- a driver whose begin() succeeded counts here whether or not the inverter
    /// has ever replied -- but configured-minus-started is a fault by definition.
    size_t                   devicesStarted    = 0;
    /// One human-readable line per configured device that is NOT polling. Never a payload byte,
    /// never a secret -- the same strings the boot log carries.
    std::vector<std::string> deviceProblems;

    /// Onboard indicators, present only on boards that have them (board::kHasBootButton /
    /// kHasStatusLed). `bootButtonPressed` makes the hold-to-factory-reset input observable
    /// over REST -- the way its wiring gets verified without a scope. `statusLedColor` is the
    /// colour the policy last chose (green/amber/red/blue/off), so the LED is checkable in the
    /// API too. Absent from the payload when the board has neither, per the house rule.
    bool        hasBootButton    = false;
    bool        bootButtonPressed = false;
    bool        hasStatusLed     = false;
    std::string statusLedColor;
};

}  // namespace heliograph
