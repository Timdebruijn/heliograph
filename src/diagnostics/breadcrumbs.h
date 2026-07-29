// SPDX-License-Identifier: MIT
//
// Reset breadcrumbs in RTC-domain SRAM: how many lives this device has had since it last
// lost power, and how far the previous one got.
//
// RTC_NOINIT memory survives panic, watchdog and software reset on every supported board --
// including the Relay-6CH, which has no battery-backed clock and therefore no other way to
// annotate a reset that happens before NTP. The contract is deliberately clock-free: the
// record carries UPTIME, never wall time, so it is exactly as trustworthy on a board that
// boots at epoch 0 as on one with an RTC. The reason the previous life ended is not stored
// at all: the boot that reads this record learns it first-hand from esp_reset_reason().
//
// WHY SIXTEEN BYTES, AND NOT THE RING THIS STARTED AS. The first version kept an
// eight-entry history ring in a 120-byte struct. On real hardware (Relay-6CH, 2026-07-29,
// serial session -- see PR #147) the full firmware's restart transition zeroes bytes
// [16,116) of that struct on every single reset, while bytes [0,16) survive; a bare sketch
// on the same board, same flavor, same bank, survives entirely. What performs that zeroing
// is still unknown -- bootloader BSS, absolute-address writers, OTA-slot ping-pong and
// mid-life corruption were all ruled out with direct evidence -- so this design trusts ONLY
// the sixteen bytes that were measured to survive, with the CRC inside them. If even that
// window ever stops surviving, the CRC turns it into a clean cold start: wrong data is
// worse than no data, and a cold start is not wrong. The ring returns if the
// grow-the-repro investigation ever names the culprit; git history has it.
//
// Host-testable end to end: the logic operates on a caller-owned Storage struct, and only
// main.cpp places that struct in RTC_NOINIT_ATTR memory.

#pragma once

#include <cstdint>

namespace heliograph::breadcrumbs {

/// The sixteen bytes that live in RTC-domain SRAM. POD on purpose: RTC_NOINIT memory is
/// never constructed. No magic field -- the CRC over the first twelve bytes is the
/// validity test, and twelve bytes of power-on garbage passing a CRC32 is a
/// once-per-four-billion event that still only costs one fabricated boot count.
struct Storage {
    uint32_t bootCount;
    uint32_t heartbeatUptimeMs;
    /// (major<<16)|(minor<<8)|patch of the image that is running. "The previous life died
    /// right after an OTA" is the single most valuable thing this record can show.
    uint32_t runningFirmware;
    /// Over the three fields above. Inside the trusted window, unlike the ring version,
    /// whose CRC lived at offset 116 and only survived by accident of layout.
    uint32_t crc;
};

/// What begin() learned about the past, for the diagnostics payload.
struct BootRecord {
    /// True when storage held no valid record: first boot ever, or power was lost (RTC RAM
    /// does not survive power-off) -- indistinguishable by design, and both honestly "cold".
    bool coldStart = true;
    /// 1 on a cold start; monotonic across warm resets.
    uint32_t bootCount = 1;
    /// The previous life's last heartbeat: "it had been up this long when it died".
    /// Meaningless on a cold start; the payload reports it absent then.
    uint32_t previousUptimeMs = 0;
    /// The image the previous life was running, same encoding as Storage::runningFirmware.
    uint32_t previousFirmware = 0;
};

/// Validates storage, captures the previous life (when warm), initialises for this one.
/// Call once, first thing in setup(): a boot that dies later still leaves its predecessor
/// on record, and its own death becomes the next boot's record.
BootRecord begin(Storage& storage, uint32_t runningFirmware);

/// Heartbeat: remembers how far this life got. Throttled internally to one write per second
/// of uptime -- calling it every loop() pass is fine and expected.
void tick(Storage& storage, uint32_t uptimeMs);

}  // namespace heliograph::breadcrumbs
