// SPDX-License-Identifier: MIT
//
// See breadcrumbs.h for the why. Implementation notes that matter here:
//
// The CRC covers every byte of Storage up to (not including) the crc field itself, and it is
// recomputed on every heartbeat. That sounds expensive and is not: the struct is ~120 bytes,
// the heartbeat is throttled to once per second, and CRC32 of 120 bytes is microseconds. What
// it buys is that a reset AT ANY MOMENT leaves storage either valid (the last completed write)
// or invalid (a torn write) -- and a torn write reads as a cold start, never as invented
// history. On this codebase's own rule that wrong data is worse than no data, that is the
// entire design.

#include "breadcrumbs.h"

#include <cstring>

namespace heliograph::breadcrumbs {
namespace {

constexpr uint32_t kMagic = 0x48454C42;  // "HELB"

/// Plain CRC32 (reflected, poly 0xEDB88320), byte at a time, no table. ~120 bytes once a
/// second does not justify 1 KB of lookup table in a build where RAM is the scarce resource.
uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

uint32_t storageCrc(const Storage& s) {
    return crc32(reinterpret_cast<const uint8_t*>(&s), offsetof(Storage, crc));
}

bool valid(const Storage& s) {
    return s.magic == kMagic && s.ringNext < kRingSize && s.ringCount <= kRingSize &&
           s.crc == storageCrc(s);
}

}  // namespace

BootRecord begin(Storage& storage, uint8_t thisResetReason, uint32_t runningFirmware) {
    BootRecord record;

    if (valid(storage)) {
        // Warm: the previous life ended with `thisResetReason`; its last heartbeat says how
        // far it got. Record it before touching anything else.
        Entry& slot      = storage.ring[storage.ringNext];
        slot.resetReason = thisResetReason;
        slot.uptimeMs    = storage.heartbeatUptimeMs;
        slot.firmware    = storage.runningFirmware;
        storage.ringNext = static_cast<uint8_t>((storage.ringNext + 1) % kRingSize);
        if (storage.ringCount < kRingSize) {
            ++storage.ringCount;
        }
        ++storage.bootCount;
        record.coldStart = false;
    } else {
        // Cold: garbage (power loss), or genuinely the first boot. Same answer either way.
        std::memset(&storage, 0, sizeof storage);
        storage.magic     = kMagic;
        storage.bootCount = 1;
    }

    storage.heartbeatUptimeMs = 0;
    storage.runningFirmware   = runningFirmware;
    storage.crc               = storageCrc(storage);

    record.bootCount = storage.bootCount;
    record.history.reserve(storage.ringCount);
    // Oldest first: with a full ring the oldest entry sits AT ringNext (the slot about to be
    // overwritten next); with a partial ring the entries start at 0.
    const size_t start =
        storage.ringCount == kRingSize ? storage.ringNext : 0;
    for (size_t i = 0; i < storage.ringCount; ++i) {
        record.history.push_back(storage.ring[(start + i) % kRingSize]);
    }
    return record;
}

void tick(Storage& storage, uint32_t uptimeMs) {
    // One write per second of uptime. The comparison also handles the first call (heartbeat
    // starts at 0) and is immune to loop() pace.
    if (uptimeMs < storage.heartbeatUptimeMs + 1000) {
        return;
    }
    storage.heartbeatUptimeMs = uptimeMs;
    storage.crc               = storageCrc(storage);
}

}  // namespace heliograph::breadcrumbs
