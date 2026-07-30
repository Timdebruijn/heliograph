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
// EVERY MEMBER HERE IS A PLAIN INTEGER WITH NO INITIALISER, AND THAT IS LOAD-BEARING.
// A default member initialiser -- `uint32_t bootCount = 0;` -- makes the type
// non-trivially-default-constructible, and the compiler then emits a static initialiser
// that runs from .init_array on EVERY boot and zeroes the object. `RTC_NOINIT_ATTR` places
// the storage where a reset cannot clear it; it does not stop our own startup code from
// clearing it. The section attribute and the type's triviality have to agree, and only the
// type is checked by the compiler.
//
// This is not theoretical. The first version of this file kept an eight-entry history ring
// whose Entry struct had `= 0` on its three members. On hardware the ring came back zeroed
// after every reset while the surrounding integers survived, which looked exactly like a
// platform fault and was chased as one for an evening (PR #147). It was this rule. Proven
// by A/B on a Relay-6CH, 2026-07-30: the identical struct without the initialisers survives
// six consecutive reboots with its ring intact; add the three `= 0` back and the corruption
// returns on the very next boot, deterministically.
//
// The sixteen-byte shape is therefore a choice, not a limit -- the ring could come back. It
// stays small because a boot counter, the previous life's uptime and its firmware answer
// the question this exists for, and a smaller trusted surface is a smaller thing to get
// wrong. If the ring ever returns, the rule above is what makes it work.
//
// Host-testable end to end: the logic operates on a caller-owned Storage struct, and only
// main.cpp places that struct in RTC_NOINIT_ATTR memory. Note that the host tests cannot
// catch a violation of the rule above -- there is no .init_array/RTC split on the host, so
// a reintroduced initialiser passes every test and fails only on the device.

#pragma once

#include <cstdint>
#include <type_traits>

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

// The guard for the rule at the top of this file, and the only one that can exist: the host
// tests cannot catch this, because the failure needs .init_array and RTC memory to be
// different things. Adding an initialiser to any member of Storage -- or nesting a type that
// has one -- breaks this build instead of silently zeroing the record on every boot of every
// device. Both traits are needed: `trivially_default_constructible` is what stops the
// compiler emitting a startup initialiser, and `trivially_copyable` keeps memcpy/CRC over the
// raw bytes well-defined.
static_assert(std::is_trivially_default_constructible_v<Storage>,
              "Storage must have no default member initialisers: a non-trivial default "
              "constructor emits a static initialiser that zeroes RTC_NOINIT memory on every "
              "boot. See the note at the top of this file.");
static_assert(std::is_trivially_copyable_v<Storage>,
              "Storage is CRC'd and memcpy'd as raw bytes.");

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
