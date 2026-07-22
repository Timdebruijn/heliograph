// SPDX-License-Identifier: MIT
//
// Facts about the bridge itself, as opposed to the inverter it talks to. Every output adapter
// needs these, so they do not belong to any one of them.

#pragma once

#include <cstdint>
#include <string>

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
    uint16_t resetReason      = 0;

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

    /// The board this firmware is running on. Reported to Home Assistant as the bridge
    /// device's model.
    /// Set by main from board::kName; the default only serves host tests, which have no
    /// board header.
    std::string boardName = "Waveshare ESP32-S3-RS485-CAN";
};

}  // namespace heliograph
