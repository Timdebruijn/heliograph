// SPDX-License-Identifier: MIT

#include "device_state.h"

namespace heliograph {

void DeviceState::recordPollSuccess(uint64_t nowMs, const StalenessPolicy& policy) {
    lastPollAttemptMs    = nowMs;
    lastSuccessfulPollMs = nowMs;
    consecutiveFailures  = 0;
    inverterOnline       = true;
    dataValid            = true;
    dataStale            = false;
    measurements.updateStaleness(nowMs, policy.measurementMaxAgeMs);
}

void DeviceState::recordPollFailure(uint64_t nowMs, const StalenessPolicy& policy) {
    lastPollAttemptMs = nowMs;
    if (consecutiveFailures < UINT32_MAX) {
        ++consecutiveFailures;
    }

    // A single failure changes nothing: the previous reading stays valid and fresh. This is
    // what keeps a lone dropped frame from producing a gap in Home Assistant's history.
    if (consecutiveFailures >= policy.failuresBeforeOffline) {
        // Prolonged silence: the inverter is gone (normal at night). Values are kept but no
        // longer valid, so outputs publish null instead of a stale-but-plausible number.
        inverterOnline = false;
        dataValid      = false;
        dataStale      = true;
        measurements.markAllStale();
    } else if (consecutiveFailures >= policy.failuresBeforeStale) {
        // Data is old but still the best we have; keep publishing it, flagged.
        dataStale = true;
        measurements.markAllStale();
    }
}

}  // namespace heliograph
