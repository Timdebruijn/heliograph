// SPDX-License-Identifier: MIT
//
// Decides when the state payload is worth republishing. Pure, so the deadband behaviour is
// testable without a broker.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "device/device_state.h"

namespace heliograph::mqtt {

struct PublishPolicy {
    /// Per-type deadbands. Solar output is never truly still; without these the bridge would
    /// publish a new payload every poll forever and fill a broker with noise.
    double powerDeadbandW      = 5.0;
    double voltageDeadbandV    = 0.5;
    double currentDeadbandA    = 0.1;
    double frequencyDeadbandHz = 0.02;
    double temperatureDeadbandC = 0.2;
    /// Energy counters move slowly and matter exactly, so any change is published.
    double energyDeadbandKwh = 0.0;

    /// Republish even without change, so a broker restart or a late subscriber does not sit
    /// on nothing until the weather shifts.
    uint64_t forceIntervalMs = 60000;
};

/// Tracks what was last published and answers whether the current state differs enough.
class PublishThrottle {
public:
    explicit PublishThrottle(PublishPolicy policy = {});

    /// True when `state` should be published now.
    ///
    /// Any change to a flag (online/valid/stale) or to the status code publishes immediately
    /// regardless of deadbands: those are the transitions a consumer must not learn a minute
    /// late.
    bool shouldPublish(const DeviceState& state, uint64_t nowMs);

    /// Records that a payload went out at `nowMs`.
    void recordPublished(const DeviceState& state, uint64_t nowMs);

    void reset();

private:
    double deadbandFor(MeasurementType type) const;

    struct Sample {
        std::string id;
        double      value;
        bool        valid;
        bool        stale;
    };

    PublishPolicy       policy_;
    std::vector<Sample> lastPublished_;
    uint64_t            lastPublishMs_ = 0;
    bool                everPublished_ = false;
    bool                lastInverterOnline_ = false;
    bool                lastDataValid_      = false;
    bool                lastDataStale_      = false;
    uint16_t            lastStatusCode_     = 0;
};

}  // namespace heliograph::mqtt
