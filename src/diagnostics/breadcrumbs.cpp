// SPDX-License-Identifier: MIT
//
// See breadcrumbs.h for the sixteen-byte story. Implementation notes that matter here:
//
// The CRC covers the twelve bytes before it and is rewritten on every heartbeat, so a reset
// at any moment leaves storage either valid (the last completed write) or invalid (a torn
// write) -- and a torn write reads as a cold start, never as an invented past. On this
// codebase's own rule that wrong data is worse than no data, that is the entire design.

#include "breadcrumbs.h"

#include <cstddef>
#include <cstring>

namespace heliograph::breadcrumbs {
namespace {

/// Plain CRC32 (reflected, poly 0xEDB88320), byte at a time, no table. Twelve bytes once a
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

}  // namespace

BootRecord begin(Storage& storage, uint32_t runningFirmware) {
    BootRecord record;

    if (storage.crc == storageCrc(storage)) {
        // Warm: the previous life's last heartbeat says how far it got, and its firmware
        // field says what it was running. Its death reason is this boot's
        // esp_reset_reason(), which the caller already exposes -- nothing to store.
        record.coldStart        = false;
        record.previousUptimeMs = storage.heartbeatUptimeMs;
        record.previousFirmware = storage.runningFirmware;
        record.bootCount        = storage.bootCount + 1;
    }
    // Cold path and warm path converge: write this life's record. On cold, bootCount in
    // the record defaulted to 1.
    storage.bootCount         = record.bootCount;
    storage.heartbeatUptimeMs = 0;
    storage.runningFirmware   = runningFirmware;
    storage.crc               = storageCrc(storage);
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
