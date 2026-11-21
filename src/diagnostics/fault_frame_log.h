// SPDX-License-Identifier: MIT
//
// A small ring of raw frames a driver thought worth keeping when a device's fault state moved
// (see device/fault_record.h). Brand-free: it stores bytes and never looks inside them.
//
// WHERE IT LIVES. The block is a plain-old-data struct that main.cpp places in RTC_NOINIT
// memory, like the breadcrumbs: it survives a software reset, an OTA restart and a panic, and
// not a power loss. Host tests give it ordinary memory and re-run begin() on the same block to
// simulate a reboot. A block that is blank (power-on garbage) or fails its CRC reads as empty --
// wrong data would be worse than none.
//
// THE SAME RULE AS breadcrumbs.h APPLIES AND IS LOAD-BEARING: no member of Storage or Entry may
// have a default member initialiser. That makes the type non-trivially-default-constructible,
// and the compiler then emits a static initialiser that zeroes RTC_NOINIT memory on every boot.
// The static_asserts below turn that into a build error. Read the note at the top of
// breadcrumbs.h for the evening it cost the first time.
//
// THREADS. push() comes from the bus task, snapshot() from the web task; both take the one
// mutex, as the other shared diagnostics state does. Neither blocks on anything else under it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "device/fault_record.h"

namespace heliograph::faultlog {

inline constexpr size_t kCapacity      = 8;
/// Sized so the longest poll reply a driver hands over today fits whole, with an echo of the
/// request in front of it on a half-duplex bus. A cut frame has lost its checksum, so a driver
/// with longer replies needs this raised (and kSchema bumped), not a truncated record.
/// Raising this or kCapacity also moves the breadcrumbs that sit behind this block in RTC
/// memory; the layout assert in main.cpp fails until that is accepted there.
inline constexpr size_t kFrameBytes    = 80;
/// A device id is the driver id plus a serial number or an instance key. 47 characters holds
/// the ones in use whole; a longer one is cut, and storedId() is how a reader matches it.
inline constexpr size_t kDeviceIdBytes = 48;

/// One slot. Explicit padding so the CRC over raw bytes never covers an indeterminate byte.
struct Entry {
    uint32_t epochSeconds;   ///< wall clock when stored; 0 when the clock was not synced
    uint32_t uptimeSeconds;  ///< always set, and the only time to trust on an unsynced board
    uint32_t signature;
    uint16_t statusCode;
    uint8_t  reason;         ///< FaultRecordReason
    uint8_t  frameLen;       ///< bytes kept in `frame`, at most kFrameBytes
    uint8_t  truncated;      ///< 1 when the frame was longer than kFrameBytes
    uint8_t  reserved[3];
    char     deviceId[kDeviceIdBytes];  ///< NUL-terminated, cut safely when longer
    uint8_t  frame[kFrameBytes];
};

struct Storage {
    uint32_t magic;
    uint32_t count;  ///< entries held, 0..kCapacity
    uint32_t head;   ///< slot the next push writes
    Entry    entries[kCapacity];
    uint32_t crc;    ///< over everything above, and the schema
};

static_assert(std::is_trivially_default_constructible_v<Entry> &&
                  std::is_trivially_default_constructible_v<Storage>,
              "No default member initialisers: a non-trivial default constructor zeroes "
              "RTC_NOINIT memory on every boot. See the note at the top of this file.");
static_assert(std::is_trivially_copyable_v<Storage>, "Storage is CRC'd as raw bytes.");
static_assert(sizeof(Entry) % 4 == 0 && sizeof(Storage) == 12 + kCapacity * sizeof(Entry) + 4,
              "Storage must have no padding: the CRC covers raw bytes.");

/// A device id as the log stores it: whole, or cut to what an Entry holds. Compare a live id
/// through this, never raw, or a long id stops matching its own records.
inline std::string storedId(const std::string& deviceId) {
    return deviceId.substr(0, kDeviceIdBytes - 1);
}

/// A stored record, decoded for readers.
struct StoredFault {
    uint32_t          epochSeconds = 0;
    uint32_t          uptimeSeconds = 0;
    std::string       deviceId;
    FaultRecordReason reason     = FaultRecordReason::Baseline;
    uint16_t          statusCode = 0;
    uint32_t          signature  = 0;
    std::vector<uint8_t> frame;
    bool              truncated = false;
};

class FaultFrameLog {
public:
    explicit FaultFrameLog(Storage& storage) : s_(storage) {}

    /// Validates the block and keeps its records, or starts empty when it is blank or corrupt.
    /// Call once in setup().
    void begin();

    /// Stores one record. Returns false when it was not stored: a Baseline or ErrorWordsChanged
    /// record while the ring already holds a frame of the same device, status code and
    /// signature, whatever that frame's reason. Two things repeat and would otherwise push the
    /// fault frames out: the bridge restarts on every deploy and learns its baseline again, and
    /// a device can show the same harmless change every day (one inverter does at every dusk).
    /// A StatusChanged record is always stored: the same fault twice is two events.
    ///
    /// ponytail: a device flapping in and out of a fault on every poll can still evict older
    /// records; add rate limiting per signature if a real unit does that
    bool push(const std::string& deviceId, const FaultRecord& record, uint32_t epochSeconds,
              uint32_t uptimeSeconds);

    /// Oldest first.
    std::vector<StoredFault> snapshot() const;

private:
    Storage&           s_;
    mutable std::mutex mutex_;
};

}  // namespace heliograph::faultlog
