// SPDX-License-Identifier: MIT
//
// Configuration model: structure, defaults, validation and (redacted) serialisation.
//
// Deliberately free of NVS: persistence is ConfigurationStore's job (Phase 8). Keeping the
// model pure means the redaction rules -- the part that leaks credentials if it is wrong --
// are testable on the host.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "device/device_state.h"
#include "drivers/driver_descriptor.h"
#include "transport/serial_profile.h"

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

    /// Static addressing. An empty `ip` means DHCP, and that is the whole switch -- there is no
    /// separate enable flag, because a flag and an address can disagree and then something has
    /// to decide which one wins while the operator is looking at the other.
    ///
    /// Kept as text rather than packed integers to match the rest of the config model, to make
    /// a backup file readable, and because "what did I type" is the question an operator asks
    /// when a bridge does not come back. validate() refuses anything that does not parse.
    std::string ip;
    std::string gateway;
    std::string subnet;
    std::string dns1;
    std::string dns2;

    bool staticIp() const { return !ip.empty(); }
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
    /// What the operator calls this inverter: "Schuur", "Balkon". Optional, and empty means
    /// exactly what it means today -- every surface falls back to the registered id.
    ///
    /// DISPLAY ONLY. It must never reach an identifier: not the registered device id, not a
    /// REST path, not an MQTT topic, not a Home Assistant unique_id. Those are keys, and a
    /// rename would silently strand every entity's history behind a new one. It is stored here,
    /// beside the device row, rather than on DeviceIdentity for the same reason -- deviceId()
    /// lives on that struct, and a label in scope there is one line away from ending up in it.
    ///
    /// Home Assistant is the exception that proves the rule: discovery announces it as the
    /// device NAME, which HA is free to change on an existing device, while the unique_id it
    /// keys entities by stays derived from the id (#76).
    std::string label;

    friend bool operator==(const DriverSettings& a, const DriverSettings& b) {
        // label included deliberately: it is applied when the device is created in setup() and
        // announced to Home Assistant at connect, so a changed label needs a restart like every
        // other property of a device row. Leaving it out would make the settings page report no
        // restart needed and then show the old name until the next reboot anyway.
        return a.id == b.id && a.autoDetect == b.autoDetect && a.label == b.label &&
               a.options == b.options;
    }
    friend bool operator!=(const DriverSettings& a, const DriverSettings& b) { return !(a == b); }
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

struct RelaySettings {
    /// Master enable for the board's relays (DRM curtailment contacts). Off by default:
    /// a relay board with factory settings must be inert. Boards without relays ignore
    /// this entirely. Note the double gate: security.readOnlyMode must ALSO be off before
    /// a relay moves -- enabling relays alone is not enough, by design.
    bool enabled = false;
    /// Per-relay DRM role ("none", or "drm0".."drm8"), index-aligned with the board's
    /// relays. Entries beyond the board's relay count are ignored; missing entries mean
    /// "none". Roles drive the Home Assistant switch names and the DRM mode select --
    /// see src/relays/drm.h.
    std::vector<std::string> roles;
};

/// Whether the dashboard looks for a newer firmware release.
///
/// The check runs in the BROWSER, not on the bridge: the page fetches a small JSON from the
/// project's GitHub Pages site and compares it with the version this bridge reports. The device
/// itself never opens an outbound connection, which is what keeps "runs entirely on your own
/// network" true even with this on.
///
/// On by default, because an update nobody hears about is an update nobody installs -- and the
/// request comes from a browser that is already on the internet. Off means the dashboard shows
/// nothing and asks nobody; the manual "check now" button still works, because that is a
/// deliberate act rather than something happening in the background.
struct UpdateSettings {
    bool checkEnabled = true;
};

struct SecuritySettings {
    std::string adminUsername = "admin";
    std::string adminPassword;  ///< never serialised, never logged; empty = mutations refused
    /// Global kill switch, independent of driver capabilities.
    bool readOnlyMode = true;
};

/// A line-settings override that survives a reboot.
///
/// Normally the driver picks the line: every driver advertises the profiles plausible for its
/// protocol and configures the first one in begin(). That is right until EXTENDED discovery
/// finds the device at one of the other profiles it tried -- a Modbus inverter answering at
/// 115200 when its driver leads with 9600, say. (Quick discovery only tries the first profile,
/// so it cannot find such a device at all.) The wizard showed which profile answered and then
/// threw it away, so the selection was saved, the bridge rebooted onto the driver's default,
/// and the device it had just positively identified went silent with nothing to explain it.
///
/// Off unless something set it, so a healthy install keeps following its driver and a device
/// profile that declares its own [serial] block still wins by default. Applied after every
/// begin(), which means at boot AND after a discovery run -- missing the second one silently
/// undid the override on a running bridge.
///
/// responseTimeoutMs and retries within `profile` are carried but unused: read deadlines are
/// per-driver compile-time constants, and Rs485Transport::read() takes its timeout from the
/// caller. Exposing them would have been a control that changes nothing.
struct SerialOverride {
    bool          enabled = false;
    SerialProfile profile{};
};

struct Configuration {
    uint16_t         version = kConfigVersion;
    std::string      bridgeName = "Heliograph";
    WifiConfig       wifi;
    MqttSettings     mqtt;
    ModbusSettings   modbus;
    PollingSettings  polling;
    /// Device 1. Kept as its own field rather than folded into a list: every existing config,
    /// every REST client and the whole settings page address it by this name, and renaming it
    /// would be a migration with no benefit to show for it.
    DriverSettings   driver;
    /// Devices 2..N on the same bus, in poll order. Empty on every single-inverter install,
    /// which is what keeps this invisible until someone actually chains a second unit.
    ///
    /// Deliberately NOT called `devices`: a field of that name sitting next to `driver` reads
    /// as "all of them", and a client that iterated it would silently skip the first inverter.
    std::vector<DriverSettings> additionalDevices;
    RelaySettings    relays;
    NtpSettings      ntp;
    UpdateSettings   updates;
    SerialOverride   serial;
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

/// Serialises for `GET /api/v1/config` and the `PATCH` response.
///
/// Credential material is NOT included -- not masked, omitted. Masking with "***" still tells
/// an attacker the length class and invites a client to round-trip the mask back in as a
/// literal secret. Passwords get a `*_set` boolean saying whether one exists, and so does the
/// MQTT username: a username is half of a credential pair, so it is treated like the password
/// it accompanies.
///
/// `security.admin_username` is omitted on the same grounds but has NO `*_set` companion --
/// validate() refuses an empty one, so the flag could only ever be true. This endpoint is
/// unauthenticated, so serving that name reduced guessing an admin login with no brute-force
/// protection to guessing only the password. Nothing else in the firmware can read it back
/// either; that is deliberate, and docs/security.md tells the owner to write it down.
///
/// Non-credential config (SSID, broker host, topics) stays readable because the UI needs it
/// and it is not a secret.
///
/// When `rebootRequired` is non-null its value is emitted as a top-level `reboot_required`
/// boolean -- the PATCH handler passes the result of configChangeRequiresReboot(); GET passes
/// nullptr and the field is absent.
bool serializeConfig(const Configuration& config, std::string& out, size_t maxBytes = 4096,
                     const bool* rebootRequired = nullptr);

/// True when moving from `before` to `after` changes any setting the firmware reads only at
/// boot (WiFi, MQTT, Modbus, polling interval, driver, NTP), so the change is stored but does
/// not take effect until a restart. The complement -- bridge_name, relays, security,
/// logging level -- is applied live by the REST layer and returns false. Kept in lockstep
/// with the RESTART_NEEDED map in the web UI.
bool configChangeRequiresReboot(const Configuration& before, const Configuration& after);

/// Resolves a driver id to its descriptor, or nullptr when the id names no compiled-in driver.
/// The config layer has no registry of its own, so it asks; the returned pointer is only read
/// for the option keys the driver declares.
using DriverLookupFn = std::function<const DriverDescriptor*(const std::string& driverId)>;

/// Applies a `PATCH /api/v1/config` body.
///
/// Absent fields are left alone. A password field set to a string sets it; set to null clears
/// it. Validation runs on the merged result, so a patch can never leave a half-applied
/// configuration behind.
///
/// `lookupDriver` is optional. When supplied, a stored driver option that the *resulting* driver
/// does not declare and that this patch did not itself supply is dropped, because it can only be
/// an orphan left behind by a previous driver. Options are scoped to the driver that declares
/// them, and the merge has no other way to remove a key -- an orphan otherwise fails every later
/// PATCH once the REST layer validates options against the descriptor, which is what made
/// switching drivers impossible in 0.12.0.
///
/// Deliberately narrow: a key this patch DID supply is never dropped, so a typo'd option is
/// still reported rather than silently swallowed, and nothing is dropped when lookupDriver
/// returns nullptr, so a typo'd *driver* id destroys nothing and stays recoverable by correcting
/// it.
bool applyConfigPatch(const std::string& json, Configuration& config, ConfigError& error,
                      const DriverLookupFn& lookupDriver = {});

}  // namespace heliograph
