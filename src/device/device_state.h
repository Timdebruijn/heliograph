// SPDX-License-Identifier: MIT
//
// Canonical device state. Every output adapter reads this and nothing else.

#pragma once

#include <cstdint>
#include <string>

#include "capability.h"
#include "device_identity.h"
#include "measurement.h"

namespace heliograph {

/// Thresholds for the online/stale state machine. Defaults follow docs/architecture.md.
struct StalenessPolicy {
    uint32_t failuresBeforeStale   = 3;
    uint32_t failuresBeforeOffline = 10;
    uint64_t measurementMaxAgeMs   = 30000;
};

struct DeviceState {
    bool bridgeOnline   = false;
    bool inverterOnline = false;
    bool dataValid      = false;
    bool dataStale      = false;

    uint64_t lastPollAttemptMs    = 0;
    uint64_t lastSuccessfulPollMs = 0;
    uint32_t consecutiveFailures  = 0;

    DeviceIdentity       identity;
    InverterCapabilities capabilities;
    MeasurementSet       measurements;

    uint16_t statusCode = 0;
    /// Human-readable status. A driver that cannot map its raw status code to text must say
    /// so ("Unknown (<n>)") rather than guess at a meaning.
    std::string statusText;

    /// 32-bit: some protocols report a 32-bit fault bitmask; truncating it would silently
    /// drop the upper faults. The 16-bit Modbus register saturates instead (register_map.cpp).
    uint32_t errorCode = 0;
    /// Not every protocol exposes a readable error code. When false, outputs must publish
    /// null rather than 0, because 0 would assert "no fault" -- a claim we cannot make.
    bool errorCodeSupported = false;

    /// Applies the outcome of a poll attempt. The only place the online/stale rules live.
    void recordPollSuccess(uint64_t nowMs, const StalenessPolicy& policy);
    void recordPollFailure(uint64_t nowMs, const StalenessPolicy& policy);
};

}  // namespace heliograph
