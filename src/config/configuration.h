// SPDX-License-Identifier: MIT
//
// Configuration model: structure, defaults, validation and (redacted) serialisation.
//
// Deliberately free of NVS: persistence is ConfigurationStore's job (Phase 8). Keeping the
// model pure means the redaction rules -- the part that leaks credentials if it is wrong --
// are testable on the host.

#pragma once

#include <cstdint>
#include <string>

#include "drivers/driver_descriptor.h"

namespace heliograph {

/// Bumped when the stored layout changes. ConfigurationStore migrates on load.
inline constexpr uint16_t kConfigVersion = 1;

enum class LogLevel : uint8_t { Error, Warn, Info, Debug, Trace };
const char* logLevelName(LogLevel level);
bool        parseLogLevel(const std::string& name, LogLevel& out);

struct WifiConfig {
    std::string ssid;
    std::string password;  ///< never serialised, never logged
    std::string hostname = "heliograph";
};

struct MqttSettings {
    bool        enabled = false;
    std::string host;
    uint16_t    port = 1883;
    std::string username;
    std::string password;  ///< never serialised, never logged
    std::string baseTopic       = "heliograph";
    std::string discoveryPrefix = "homeassistant";
    bool        discoveryEnabled = true;
    uint8_t     qos              = 0;
};

struct ModbusSettings {
    bool     enabled           = true;
    uint16_t port              = 502;
    uint8_t  unitId            = 1;
    uint8_t  diagnosticsUnitId = 250;
    /// Stays false. There is no writable driver; the field exists so the default is explicit
    /// and documented rather than merely absent.
    bool writeEnabled = false;
};

struct PollingSettings {
    uint32_t intervalSeconds = 10;
};

struct DriverSettings {
    /// Empty means "let the application pick": the highest-priority driver compiled in.
    /// Not a hardcoded id -- a default naming one manufacturer is that manufacturer leaking
    /// into the config model.
    std::string id;
    bool        autoDetect = false;
    /// Driver-specific settings, opaque here. The driver declares which keys exist and what
    /// they accept (DriverDescriptor::options); validateDriverOptions checks them against it.
    DriverOptions options;
};

// There is deliberately no RS485/serial section here. Line settings are a property of the
// protocol: every driver configures the UART itself (from its descriptor or device profile).
// A user-facing rs485 section existed, was validated, persisted, rendered -- and read by
// nothing. Removed 0.4.14; stored configs carrying the old key load fine (unknown keys are
// ignored).

struct NtpSettings {
    bool enabled = true;
    /// Prefer an NTP server handed out by DHCP (option 42) when the network provides one. It
    /// lands at SNTP index 0 and so wins over `server`, which becomes the fallback. Turn off to
    /// use `server` exclusively.
    bool useDhcp = true;
    /// Fallback / default server, used when DHCP offers none (or useDhcp is off). A public pool
    /// so the clock works out of the box on any network; point it at a local server (a router,
    /// OPNsense) to avoid any outbound dependency.
    std::string server = "pool.ntp.org";
    /// POSIX TZ string, not an IANA name: this is what the C runtime needs for local time and
    /// DST. Default is Europe/Amsterdam (CET/CEST). Logs are stamped in this zone.
    std::string timezone = "CET-1CEST,M3.5.0,M10.5.0/3";
    /// IANA label ("Europe/Amsterdam") for the UI only -- many cities share one POSIX string,
    /// and the dropdown must re-select the city the user actually picked, not the first city
    /// that happens to share its rules. The firmware itself runs on `timezone` alone.
    std::string timezoneName = "Europe/Amsterdam";
};

struct SecuritySettings {
    std::string adminUsername = "admin";
    std::string adminPassword;  ///< never serialised, never logged; empty = mutations refused
    /// Global kill switch, independent of driver capabilities.
    bool readOnlyMode = true;
};

struct Configuration {
    uint16_t         version = kConfigVersion;
    std::string      bridgeName = "Heliograph";
    WifiConfig       wifi;
    MqttSettings     mqtt;
    ModbusSettings   modbus;
    PollingSettings  polling;
    DriverSettings   driver;
    NtpSettings      ntp;
    SecuritySettings security;
    LogLevel         logLevel = LogLevel::Info;

    /// True once WiFi is usable. Until then the device has no business joining a network.
    bool provisioned() const { return !wifi.ssid.empty(); }
};

struct ConfigError {
    std::string field;
    std::string message;
};

/// Checks ranges and enum values. Returns false and fills `error` on the first problem.
bool validate(const Configuration& config, ConfigError& error);

/// Serialises for `GET /api/v1/config`.
///
/// Passwords are NOT included -- not masked, omitted. A `*_set` boolean says whether one
/// exists. Masking with "***" still tells an attacker the length class and invites a client
/// to round-trip the mask back in as a literal password.
bool serializeConfig(const Configuration& config, std::string& out, size_t maxBytes = 4096);

/// Applies a `PATCH /api/v1/config` body.
///
/// Absent fields are left alone. A password field set to a string sets it; set to null clears
/// it. Validation runs on the merged result, so a patch can never leave a half-applied
/// configuration behind.
bool applyConfigPatch(const std::string& json, Configuration& config, ConfigError& error);

}  // namespace heliograph
