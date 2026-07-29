// SPDX-License-Identifier: MIT
//
// Reset breadcrumbs in RTC-domain SRAM: what this device was doing when it last died.
//
// RTC_NOINIT memory survives panic, watchdog and software reset on every supported board --
// including the Relay-6CH, which has no battery-backed clock and therefore no other way to
// annotate a reset that happened before NTP. Until this existed, the only post-reset evidence
// was esp_reset_reason() (one number, previous reset only) and a coredump (panics only). A
// bridge that watchdogged at night on variant C could not even say how long it had been up.
//
// The contract is deliberately clock-free: entries carry UPTIME at death, never wall time, so
// they are exactly as trustworthy on a board that boots at epoch 0 as on one with an RTC.
// Ordering comes from position in the ring, not from timestamps.
//
// This header is host-testable end to end: the logic operates on a caller-owned Storage
// struct, and only main.cpp places that struct in RTC_NOINIT_ATTR memory. Power loss clears
// RTC RAM on real hardware; the CRC is what turns that garbage into a clean cold start
// instead of a fabricated history -- wrong data would be worse than no data.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace heliograph::breadcrumbs {

/// One prior life of this device: how it ended, how long it had run, what it was running.
struct Entry {
    /// esp_reset_reason() value observed by the boot FOLLOWING this life. Numeric, matching
    /// the `reset_reason` field the diagnostics payload already exposes.
    uint8_t resetReason = 0;
    /// The heartbeat's last written uptime -- "it had been up this long when it died". The
    /// heartbeat ticks at most once a second, so this understates by less than a second.
    uint32_t uptimeMs = 0;
    /// (major<<16)|(minor<<8)|patch of the image that died. "Died right after an OTA" is the
    /// single most valuable thing a reset history can show.
    uint32_t firmware = 0;
};

inline constexpr size_t kRingSize = 8;

/// The raw bytes that live in RTC-domain SRAM. POD on purpose: RTC_NOINIT_ATTR memory is
/// never constructed, so anything with a constructor would be UB there.
struct Storage {
    uint32_t magic;
    uint32_t bootCount;
    uint32_t heartbeatUptimeMs;
    uint32_t runningFirmware;
    Entry    ring[kRingSize];
    uint8_t  ringNext;  // next slot to write; ring[ringNext-1] is the newest entry
    uint8_t  ringCount; // entries actually written, saturates at kRingSize
    /// Over everything above. Last on purpose: the CRC of a struct must not include itself.
    uint32_t crc;
};

/// What begin() learned about the past, for the diagnostics payload.
struct BootRecord {
    /// True when storage held no valid history: first boot ever, or power was lost (RTC RAM
    /// does not survive power-off) -- indistinguishable by design, and both honestly "cold".
    bool coldStart = true;
    /// 1 on a cold start; monotonic across warm resets.
    uint32_t bootCount = 1;
    /// Oldest first. Empty on a cold start.
    std::vector<Entry> history;
};

/// Validates storage, records the previous life (when warm), initialises for this one.
/// Call once, early in setup(), BEFORE anything that could crash: a boot that dies later
/// still leaves its predecessor recorded.
BootRecord begin(Storage& storage, uint8_t thisResetReason, uint32_t runningFirmware);

/// Heartbeat: remembers how far this life got. Throttled internally to one write per second
/// of uptime -- calling it every loop() pass is fine and expected.
void tick(Storage& storage, uint32_t uptimeMs);

}  // namespace heliograph::breadcrumbs
