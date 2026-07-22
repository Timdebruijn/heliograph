// SPDX-License-Identifier: MIT

#include "publish_policy.h"

#include <cmath>

namespace heliograph::mqtt {

PublishThrottle::PublishThrottle(PublishPolicy policy) : policy_(policy) {}

double PublishThrottle::deadbandFor(MeasurementType type) const {
    switch (type) {
        case MeasurementType::Power:       return policy_.powerDeadbandW;
        case MeasurementType::Voltage:     return policy_.voltageDeadbandV;
        case MeasurementType::Current:     return policy_.currentDeadbandA;
        case MeasurementType::Frequency:   return policy_.frequencyDeadbandHz;
        case MeasurementType::Temperature: return policy_.temperatureDeadbandC;
        case MeasurementType::Energy:      return policy_.energyDeadbandKwh;
        default:                           return 0.0;
    }
}

bool PublishThrottle::shouldPublish(const DeviceState& state, uint64_t nowMs) {
    if (!everPublished_) {
        return true;
    }

    // Liveness transitions go out immediately. A consumer learning a minute late that the
    // inverter dropped off is exactly the failure this whole model exists to avoid.
    if (state.inverterOnline != lastInverterOnline_ || state.dataValid != lastDataValid_ ||
        state.dataStale != lastDataStale_ || state.statusCode != lastStatusCode_) {
        return true;
    }

    if (nowMs >= lastPublishMs_ && nowMs - lastPublishMs_ >= policy_.forceIntervalMs) {
        return true;
    }
    if (nowMs < lastPublishMs_) {
        return true;  // clock went backwards; do not go quiet until it catches up
    }

    for (const auto& m : state.measurements.all()) {
        if (!m.supported) {
            continue;
        }
        const Sample* previous = nullptr;
        for (const auto& s : lastPublished_) {
            if (s.id == m.id) {
                previous = &s;
                break;
            }
        }
        if (previous == nullptr) {
            return true;  // a channel appeared, e.g. a second MPPT was detected
        }
        // A validity change is a change, however small the number moved.
        if (previous->valid != m.valid || previous->stale != m.stale) {
            return true;
        }
        if (!m.valid) {
            continue;
        }
        if (std::fabs(m.value - previous->value) > deadbandFor(m.type)) {
            return true;
        }
    }
    return false;
}

void PublishThrottle::recordPublished(const DeviceState& state, uint64_t nowMs) {
    lastPublished_.clear();
    lastPublished_.reserve(state.measurements.size());
    for (const auto& m : state.measurements.all()) {
        if (m.supported) {
            lastPublished_.push_back(Sample{m.id, m.value, m.valid, m.stale});
        }
    }
    lastPublishMs_      = nowMs;
    everPublished_      = true;
    lastInverterOnline_ = state.inverterOnline;
    lastDataValid_      = state.dataValid;
    lastDataStale_      = state.dataStale;
    lastStatusCode_     = state.statusCode;
}

void PublishThrottle::reset() {
    // Called on reconnect: the broker may have dropped our retained messages, so the next
    // state must go out unconditionally.
    lastPublished_.clear();
    everPublished_ = false;
    lastPublishMs_ = 0;
}

}  // namespace heliograph::mqtt
