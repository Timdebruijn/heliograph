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

/// What the twelve checksummed bytes MEAN. Not stored -- it is folded into the CRC, so it
/// costs no RTC bytes and a record written under a different schema simply fails validation.
///
/// This exists because renaming heartbeatUptimeMs to heartbeatUptimeSeconds changed a field's
/// UNIT without changing the layout, so an old record still CRC'd correctly and was read back
/// as warm: a bridge that had been up three days reported three thousand. That is not a
/// plausible-looking error, it is a thousandfold one, and it would have happened once on every
/// device in the fleet at its first update into that firmware.
///
/// Bump this whenever a field's meaning, unit or width changes. The cost of bumping is one
/// cold start -- the boot count restarts and the previous life is reported as unknown, which
/// is the honest answer, because the bytes genuinely cannot be interpreted.
inline constexpr uint32_t kStorageSchema = 2;

/// Plain CRC32 (reflected, poly 0xEDB88320), byte at a time, no table. Twelve bytes once a
/// second does not justify 1 KB of lookup table in a build where RAM is the scarce resource.
///
/// Takes and returns the RUNNING value, without the final inversion, so a checksum can span
/// more than one buffer -- the schema tag and the struct are two.
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

uint32_t storageCrc(const Storage& s) {
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32Update(crc, reinterpret_cast<const uint8_t*>(&kStorageSchema),
                      sizeof kStorageSchema);
    crc = crc32Update(crc, reinterpret_cast<const uint8_t*>(&s), offsetof(Storage, crc));
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace

BootRecord begin(Storage& storage, uint32_t runningFirmware) {
    BootRecord record;

    if (storage.crc == storageCrc(storage)) {
        // Warm: the previous life's last heartbeat says how far it got, and its firmware
        // field says what it was running. Its death reason is this boot's
        // esp_reset_reason(), which the caller already exposes -- nothing to store.
        record.coldStart        = false;
        record.previousUptimeMs = static_cast<uint64_t>(storage.heartbeatUptimeSeconds) * 1000ULL;
        record.previousFirmware = storage.runningFirmware;
        record.bootCount        = storage.bootCount + 1;
    }
    // Cold path and warm path converge: write this life's record. On cold, bootCount in
    // the record defaulted to 1.
    storage.bootCount         = record.bootCount;
    storage.heartbeatUptimeSeconds = 0;
    storage.runningFirmware   = runningFirmware;
    storage.crc               = storageCrc(storage);
    return record;
}

void tick(Storage& storage, uint64_t uptimeMs) {
    // One write per second of uptime, which is also why storing seconds loses nothing. Takes
    // the full 64-bit clock: the caller used to narrow it to uint32 here, and casting a
    // wrapped value to a wider type does not un-wrap it.
    const uint32_t seconds = static_cast<uint32_t>(uptimeMs / 1000);
    // <=, not !=. The old millisecond comparison was implicitly monotonic -- it could never
    // write a value smaller than the one stored -- and an equality check quietly gave that up.
    // esp_timer_get_time() does not run backwards, so nothing reaches this today; the property
    // is worth keeping anyway, because the field's whole meaning is "how far this life got".
    if (seconds <= storage.heartbeatUptimeSeconds) {
        return;
    }
    storage.heartbeatUptimeSeconds = seconds;
    storage.crc               = storageCrc(storage);
}

}  // namespace heliograph::breadcrumbs
