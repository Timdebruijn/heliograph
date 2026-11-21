// SPDX-License-Identifier: MIT
//
// What a driver hands over when a device's fault state changes: one raw frame plus why it was
// kept. Brand-free: the driver decides what counts as a change and what the signature means;
// the log that stores these never looks inside a frame.
#pragma once

#include <cstdint>
#include <vector>

namespace heliograph {

enum class FaultRecordReason : uint8_t {
    Baseline,          ///< the reference the driver compares error words against
    StatusChanged,     ///< the status word moved into a state that is not normal, or out of one
    ErrorWordsChanged, ///< the device's error words differ from the previous poll's
};

inline const char* faultRecordReasonName(FaultRecordReason reason) {
    switch (reason) {
        case FaultRecordReason::Baseline:          return "baseline";
        case FaultRecordReason::StatusChanged:     return "status_changed";
        case FaultRecordReason::ErrorWordsChanged: return "error_words_changed";
    }
    return "unknown";
}

struct FaultRecord {
    FaultRecordReason reason     = FaultRecordReason::Baseline;
    uint16_t          statusCode = 0;
    /// A driver-defined fingerprint of the device's error state. Two records with the same
    /// signature from the same device describe the same state; that is all the log relies on.
    uint32_t             signature = 0;
    std::vector<uint8_t> frame;  ///< the raw bytes of the poll that produced this record
};

/// One value read out of a stored frame. `name` and `unit` are string literals.
struct FaultFrameField {
    const char* name  = "";
    double      value = 0.0;
    const char* unit  = "";
};

}  // namespace heliograph
