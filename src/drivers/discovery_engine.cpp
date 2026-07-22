// SPDX-License-Identifier: MIT

#include "discovery_engine.h"

#include <algorithm>

namespace heliograph {
namespace {

/// Two probes agree when they identify the same physical thing. A differing serial number is
/// the strongest possible signal that the "identification" was noise.
bool probesAgree(const ProbeResult& a, const ProbeResult& b) {
    if (a.responded != b.responded) {
        return false;
    }
    if (a.serialNumber != b.serialNumber) {
        return false;
    }
    return a.detectedModel == b.detectedModel;
}

}  // namespace

DiscoveryEngine::DiscoveryEngine(const DriverRegistry& registry, Transport& transport)
    : registry_(registry), transport_(transport) {}

DiscoveryOutcome DiscoveryEngine::run(DiscoveryMode mode, const DiscoveryConfig& config) {
    DiscoveryOutcome outcome;

    for (const auto& descriptor : registry_.availableDrivers()) {
        if (!descriptor.supportsAutoDetection) {
            continue;  // e.g. the mock driver: real, useful, but must never be auto-chosen
        }
        const auto& supported = descriptor.supportedTransports;
        if (std::find(supported.begin(), supported.end(), transport_.type()) == supported.end()) {
            continue;
        }

        // Quick mode uses only the driver's first recommended profile; Extended tries them
        // all. Neither ever invents a profile the driver did not name.
        std::vector<SerialProfile> profiles = descriptor.recommendedSerialProfiles;
        if (profiles.empty()) {
            profiles.push_back(SerialProfile{});
        }
        if (mode == DiscoveryMode::Quick && profiles.size() > 1) {
            profiles.resize(1);
        }

        for (const auto& profile : profiles) {
            auto driver = registry_.create(descriptor.id, transport_);
            if (!driver) {
                continue;
            }
            transport_.configure(profile);
            if (!driver->begin(transport_)) {
                continue;
            }

            ProbeResult first = driver->probe();
            if (!first.responded) {
                continue;
            }

            DiscoveryCandidate candidate;
            candidate.descriptor = descriptor;
            candidate.probe      = first;

            if (config.requireConsistentProbes) {
                const ProbeResult second = driver->probe();
                candidate.consistent     = probesAgree(first, second);
                if (!candidate.consistent) {
                    candidate.probe.confidenceScore /= 2;
                    candidate.probe.evidence.push_back(
                        "second probe disagreed with the first; confidence halved");
                } else {
                    candidate.probe.evidence.push_back("second probe gave the same result");
                }
            }

            outcome.candidates.push_back(std::move(candidate));
            break;  // a profile answered; no reason to keep poking the bus
        }
    }

    std::sort(outcome.candidates.begin(), outcome.candidates.end(),
              [](const DiscoveryCandidate& a, const DiscoveryCandidate& b) {
                  return a.probe.confidenceScore > b.probe.confidenceScore;
              });

    if (outcome.candidates.empty()) {
        outcome.reason = "no device answered; check wiring, termination and the RS485 A/B order";
        return outcome;
    }

    const auto& best = outcome.candidates.front();

    if (!best.probe.checksumValid) {
        outcome.reason = "a device answered but no reply passed its checksum; select a driver manually";
        return outcome;
    }
    if (best.probe.confidenceScore < config.minConfidence) {
        outcome.reason = "best match '" + best.descriptor.id + "' scored " +
                         std::to_string(best.probe.confidenceScore) + ", below the threshold of " +
                         std::to_string(config.minConfidence) + "; confirm manually";
        return outcome;
    }
    if (config.requireConsistentProbes && !best.consistent) {
        outcome.reason = "repeated probes of '" + best.descriptor.id +
                         "' disagreed; confirm manually";
        return outcome;
    }
    if (outcome.candidates.size() > 1) {
        const int margin =
            best.probe.confidenceScore - outcome.candidates[1].probe.confidenceScore;
        if (margin < config.minMargin) {
            outcome.reason = "'" + best.descriptor.id + "' (" +
                             std::to_string(best.probe.confidenceScore) + ") and '" +
                             outcome.candidates[1].descriptor.id + "' (" +
                             std::to_string(outcome.candidates[1].probe.confidenceScore) +
                             ") are too close to call; confirm manually";
            return outcome;
        }
    }

    outcome.autoSelected     = true;
    outcome.selectedDriverId = best.descriptor.id;
    outcome.reason           = "'" + best.descriptor.id + "' matched convincingly (" +
                     std::to_string(best.probe.confidenceScore) + "/100)";
    return outcome;
}

}  // namespace heliograph
