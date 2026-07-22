// SPDX-License-Identifier: MIT
//
// Wall-clock time via SNTP, and the log-timestamp provider that hangs off it.
//
// Precedence, per the brief: an NTP server handed out by DHCP (option 42) wins; a configured
// server -- defaulting to a public pool so the clock works on any network -- is the fallback.
// Timezone is a POSIX TZ string so logs read in local time with DST.
//
// The measurement pipeline does not depend on this: canonical timestamps are monotonic uptime,
// and MQTT/Home Assistant stamp on receive. This exists so the device's own logs (serial, and
// diagnostics) carry a real time, which matters for an unattended Phase-3 capture over a day.

#pragma once

#include <ctime>
#include <string>

#include "config/configuration.h"

namespace heliograph {

class TimeManager {
public:
    /// Arms DHCP option 42 harvesting. Timing is a two-sided trap:
    ///   - it must run BEFORE the DHCP lease arrives: lwip stores the DHCP-offered NTP
    ///     server only if the flag is set when the lease ACK is processed (armed later,
    ///     the offer is dropped until renewal -- half a lease away; live, 2026-07-21);
    ///   - but AFTER the network stack is up: esp_sntp_servermode_dhcp() is dispatched via
    ///     tcpip_callback(), which aborts when the tcpip thread does not exist yet --
    ///     calling this from setup() before WiFi bricked the boot (0.4.4).
    /// The one safe moment is WifiManager's network-stack-ready hook; call it from there
    /// and nowhere else.
    void prepareDhcp(const Configuration& config);

    /// Installs the log-timestamp provider, independent of SNTP. Call it as the first thing
    /// in setup(): lines before a valid clock carry an uptime stamp ("+0.4s"), and from the
    /// moment the clock is valid -- an RTC restore seconds later, or NTP much later -- every
    /// line carries wall-clock. Without this, boot lines stayed stamp-less until WiFi came
    /// up and begin() ran, which defeated the point of restoring the RTC before the network
    /// (live, 2026-07-22: the "rtc: clock restored" line itself had no timestamp).
    /// Idempotent; begin() installs the same provider again.
    static void installLogTimestamps();

    /// Sets TZ, starts SNTP and installs the log-timestamp provider. Call once WiFi is up; the
    /// DHCP-provided NTP server is only known after the lease is in. Safe to call again after a
    /// config change: it restarts SNTP rather than stacking a second client (a changed
    /// useDhcp setting then still needs a lease renewal or reboot to take effect -- see
    /// prepareDhcp).
    void begin(const Configuration& config);

    /// True once a real wall-clock time is available (the RTC has moved past a sane epoch).
    bool synced() const;

    /// Wall-clock moment of the most recent SNTP synchronisation, or 0 when none happened
    /// this session. Fed by the SNTP notification callback; survives nothing (it is
    /// session-local by design -- the RTC may keep ticking across a soft reboot, but a
    /// fresh boot has demonstrably not synced yet).
    time_t lastSyncEpoch() const;

    /// Which server the clock actually came from. `server` empty = not determinable; the
    /// caller must then report unknown rather than guess.
    struct SyncSource {
        std::string server;            ///< hostname or IP as text
        bool        fromDhcp = false;  ///< DHCP option 42 vs the configured fallback
    };

    /// Identifies the answering server via lwip's per-server reachability register
    /// (RFC 5905): the slot that recently answered is the source. Falls back to "the only
    /// configured slot" when exactly one exists; otherwise reports unknown.
    SyncSource syncSource() const;

private:
    // Kept alive for the lifetime of SNTP: esp_sntp_setservername may store the pointer rather
    // than a copy (the same lifetime trap as espMqttClient's client id). A member guarantees the
    // bytes outlive the call regardless of the library's internal behaviour.
    std::string serverName_;
    bool        started_ = false;
};

}  // namespace heliograph
