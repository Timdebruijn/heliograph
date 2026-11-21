// SPDX-License-Identifier: MIT
// See fault_frame_log.h.

#include "fault_frame_log.h"

#include <algorithm>
#include <cstring>

namespace heliograph::faultlog {
namespace {

inline constexpr uint32_t kMagic = 0x48464C47;  // "HFLG"
/// Folded into the CRC, not stored: a block written under another layout or meaning fails
/// validation and reads as empty, which is the honest answer. Bump when Entry or Storage change.
inline constexpr uint32_t kSchema = 1;

// Same plain bitwise CRC32 as breadcrumbs.cpp (it is private there; eleven lines do not justify
// a shared header). Over about 1 KB once per stored record, so no table.
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
    crc = crc32Update(crc, reinterpret_cast<const uint8_t*>(&kSchema), sizeof kSchema);
    crc = crc32Update(crc, reinterpret_cast<const uint8_t*>(&s), offsetof(Storage, crc));
    return crc ^ 0xFFFFFFFFu;
}

void seal(Storage& s) { s.crc = storageCrc(s); }

bool valid(const Storage& s) {
    if (s.magic != kMagic || s.count > kCapacity || s.head >= kCapacity ||
        s.crc != storageCrc(s)) {
        return false;
    }
    for (size_t i = 0; i < s.count; ++i) {
        if (s.entries[i].frameLen > kFrameBytes) {
            return false;
        }
    }
    return true;
}

}  // namespace

void FaultFrameLog::begin() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (valid(s_)) {
        return;  // survives: a warm reset keeps its records
    }
    std::memset(&s_, 0, sizeof s_);
    s_.magic = kMagic;
    seal(s_);
}

bool FaultFrameLog::push(const std::string& deviceId, const FaultRecord& record,
                         uint32_t epochSeconds, uint32_t uptimeSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);

    // A fault is an event and is always stored. Anything else is a state the device was seen
    // in, and one frame of a state is enough: a second one would only push a fault out.
    if (record.reason != FaultRecordReason::StatusChanged) {
        for (size_t i = 0; i < s_.count; ++i) {
            const Entry& e = s_.entries[i];
            if (e.signature == record.signature && e.statusCode == record.statusCode &&
                std::strncmp(e.deviceId, deviceId.c_str(), kDeviceIdBytes - 1) == 0) {
                return false;
            }
        }
    }

    Entry& e = s_.entries[s_.head];
    std::memset(&e, 0, sizeof e);
    e.epochSeconds  = epochSeconds;
    e.uptimeSeconds = uptimeSeconds;
    e.signature     = record.signature;
    e.statusCode    = record.statusCode;
    e.reason        = static_cast<uint8_t>(record.reason);
    const size_t kept = std::min(record.frame.size(), kFrameBytes);
    e.frameLen      = static_cast<uint8_t>(kept);
    e.truncated     = record.frame.size() > kFrameBytes ? 1 : 0;
    if (kept > 0) {
        std::memcpy(e.frame, record.frame.data(), kept);
    }
    // Cut at the array, always leaving the NUL memset put there.
    std::strncpy(e.deviceId, deviceId.c_str(), kDeviceIdBytes - 1);

    s_.head = (s_.head + 1) % kCapacity;
    if (s_.count < kCapacity) {
        ++s_.count;
    }
    seal(s_);
    return true;
}

std::vector<StoredFault> FaultFrameLog::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<StoredFault>    out;
    out.reserve(s_.count);
    // head is the next write slot, so the oldest is `count` slots behind it.
    const size_t first = (s_.head + kCapacity - s_.count) % kCapacity;
    for (size_t i = 0; i < s_.count; ++i) {
        const Entry& e = s_.entries[(first + i) % kCapacity];
        StoredFault  f;
        f.epochSeconds  = e.epochSeconds;
        f.uptimeSeconds = e.uptimeSeconds;
        f.deviceId.assign(e.deviceId,
                          std::find(e.deviceId, e.deviceId + kDeviceIdBytes, '\0'));
        f.reason     = static_cast<FaultRecordReason>(e.reason);
        f.statusCode = e.statusCode;
        f.signature  = e.signature;
        f.frame.assign(e.frame, e.frame + std::min<size_t>(e.frameLen, kFrameBytes));
        f.truncated = e.truncated != 0;
        out.push_back(std::move(f));
    }
    return out;
}

}  // namespace heliograph::faultlog
