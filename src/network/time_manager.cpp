// SPDX-License-Identifier: MIT
//
// SNTP API read from the framework sources (esp_sntp.h in framework-arduinoespressif32-libs),
// not from memory. DHCP option 42 is available because CONFIG_LWIP_DHCP_GET_NTP_SRV=y in the
// esp32s3 sdkconfig; without that the servermode_dhcp call would be a no-op.

#include "time_manager.h"

#if defined(ESP32)

#include <Arduino.h>
#include <esp_sntp.h>
#include <esp_timer.h>

#include <atomic>
#include <ctime>

#include "diagnostics/log_timestamp.h"
#include "diagnostics/logger.h"

namespace heliograph {
namespace {

// 2024-01-01 UTC. A wall clock past this can only mean SNTP set it; before sync the RTC sits
// near 1970. This threshold, not sntp_get_sync_status(), decides "synced": that status flag
// resets to RESET right after a successful update, so it is false most of the time even when
// the clock is correct.
constexpr time_t kSaneEpoch = 1704067200;

size_t provideTimestamp(char* buf, size_t n) {
    const time_t now = time(nullptr);
    // esp_timer, not millis(): the uptime stamp on a never-synced clock must keep counting
    // past 49.7 days instead of restarting at zero. See the note at main.cpp's nowMs().
    return log::formatLogTimestamp(buf, n, now > kSaneEpoch, now,
                                   static_cast<uint64_t>(esp_timer_get_time() / 1000));
}

// Written from the SNTP task via the notification callback, read from the main loop and
// the AsyncTCP handlers building payloads: atomic, not volatile (same reasoning as
// g_rebootAtMs -- a 64-bit store is not single-instruction on this MCU).
std::atomic<int64_t> g_lastSyncEpoch{0};
// Whether begin() enabled DHCP option 42; the callback and syncSource() need it to label
// a name-less slot-0 IP as DHCP-provided.
std::atomic<bool> g_dhcpEnabled{false};

/// One configured SNTP slot, as text. Empty when the slot is unused.
struct SlotInfo {
    std::string server;
    bool        fromDhcp  = false;
    bool        reachable = false;
};

SlotInfo readSlot(uint8_t idx) {
    SlotInfo out;
    // A server set by name (ours) reports its name; a DHCP-provided one is set as a bare
    // IP by lwip and has no name. Both checked, name first.
    const char* name = esp_sntp_getservername(idx);
    if (name != nullptr && name[0] != '\0') {
        out.server = name;
    } else {
        const ip_addr_t* ip = esp_sntp_getserver(idx);
        if (ip != nullptr && !ip_addr_isany(ip)) {
            out.server   = ipaddr_ntoa(ip);
            out.fromDhcp = g_dhcpEnabled.load();
        }
    }
    // RFC 5905 reachability shift register: nonzero = this slot answered recently.
    // Verified against the installed lwip: SNTP_MONITOR_SERVER_REACHABILITY defaults to 1
    // (sntp_opts.h) and nothing in the esp32s3 sdkconfig disables it.
    out.reachable = !out.server.empty() && esp_sntp_getreachability(idx) != 0;
    return out;
}

/// The answering server, or empty when it cannot be determined honestly.
SlotInfo determineSyncSource() {
    SlotInfo reachableSlot;
    SlotInfo onlySlot;
    int      configured = 0;
    int      reachable  = 0;
    for (uint8_t i = 0; i < SNTP_MAX_SERVERS; ++i) {
        const SlotInfo s = readSlot(i);
        if (s.server.empty()) {
            continue;
        }
        ++configured;
        onlySlot = s;
        if (s.reachable) {
            ++reachable;
            reachableSlot = s;
        }
    }
    if (reachable == 1) {
        return reachableSlot;  // exactly one slot answered: that is the source
    }
    if (configured == 1) {
        return onlySlot;  // only one candidate exists: no ambiguity either
    }
    return SlotInfo{};  // ambiguous (or nothing configured): report unknown, not a guess
}

void onTimeSynced(struct timeval* tv) {
    const time_t now = tv != nullptr ? tv->tv_sec : time(nullptr);
    g_lastSyncEpoch.store(static_cast<int64_t>(now));
    // The one unmissable feedback line: the moment the clock became real. From here on
    // every log line carries local wall-clock instead of uptime.
    char stamp[24];
    const size_t   n      = log::formatIsoLocalTime(stamp, sizeof(stamp), now);
    const SlotInfo source = determineSyncSource();
    if (!source.server.empty()) {
        log::info("ntp: time synchronised: %s (server %s, %s)", n > 0 ? stamp : "?",
                  source.server.c_str(), source.fromDhcp ? "dhcp" : "configured");
    } else {
        log::info("ntp: time synchronised: %s", n > 0 ? stamp : "?");
    }
}

}  // namespace

void TimeManager::prepareDhcp(const Configuration& config) {
    // Just a flag inside lwip; safe before any netif exists, and it must be set this early
    // (see the header). The SNTP client itself still starts in begin(), after the network.
    const bool enable = config.ntp.enabled && config.ntp.useDhcp;
    esp_sntp_servermode_dhcp(enable);
    g_dhcpEnabled.store(enable);
}

void TimeManager::installLogTimestamps() { log::setTimestampProvider(&provideTimestamp); }

void TimeManager::begin(const Configuration& config) {
    // TZ first, so the first stamped line is already local. setenv copies its value, so no
    // lifetime concern here (unlike the server name below).
    setenv("TZ", config.ntp.timezone.c_str(), 1);
    tzset();

    // Install the provider regardless of NTP: before sync it yields uptime, after sync
    // wall-clock. A user who disables NTP still gets a monotonic marker on every line.
    installLogTimestamps();

    if (!config.ntp.enabled) {
        log::info("ntp: disabled; logs carry uptime only");
        return;
    }

    if (esp_sntp_enabled()) {
        esp_sntp_stop();  // begin() may run a second time after a settings change
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_set_time_sync_notification_cb(&onTimeSynced);
    // DHCP-provided server lands at index 0 and so wins; the configured/default server is the
    // fallback one slot down. With DHCP off the configured server is the only one, at index 0.
    esp_sntp_servermode_dhcp(config.ntp.useDhcp);
    g_dhcpEnabled.store(config.ntp.useDhcp);
    if (!config.ntp.server.empty()) {
        serverName_ = config.ntp.server;  // see the note in the header on lifetime
        esp_sntp_setservername(config.ntp.useDhcp ? 1 : 0, serverName_.c_str());
    }
    esp_sntp_init();
    started_ = true;

    // The server name is not a secret; it may be logged.
    log::info("ntp: started (dhcp=%s, fallback=%s, tz=%s)", config.ntp.useDhcp ? "on" : "off",
              config.ntp.server.empty() ? "(none)" : config.ntp.server.c_str(),
              config.ntp.timezone.c_str());
}

bool TimeManager::synced() const { return time(nullptr) > kSaneEpoch; }

time_t TimeManager::lastSyncEpoch() const {
    return static_cast<time_t>(g_lastSyncEpoch.load());
}

TimeManager::SyncSource TimeManager::syncSource() const {
    const SlotInfo s = determineSyncSource();
    return SyncSource{s.server, s.fromDhcp};
}

}  // namespace heliograph

#else  // !ESP32

namespace heliograph {

void TimeManager::prepareDhcp(const Configuration&) {}
void TimeManager::installLogTimestamps() {}
void TimeManager::begin(const Configuration&) {}
bool TimeManager::synced() const { return false; }
time_t TimeManager::lastSyncEpoch() const { return 0; }
TimeManager::SyncSource TimeManager::syncSource() const { return {}; }

}  // namespace heliograph

#endif
