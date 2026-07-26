// SPDX-License-Identifier: MIT

#include "discovery_engine.h"

#include <algorithm>
#include <cstdlib>

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

/// The bus addresses one driver is probed at, in order, as option values.
///
/// The driver's own default always comes first and is always included, whatever the sweep set
/// says: SolaX assigns 10 by default, which is outside 1..8, and a wizard that stopped finding
/// the device it used to find would be a regression dressed as a feature.
///
/// A single empty string means "no address option": probe once, with no options, exactly as
/// before. That is every driver in Quick mode, and drivers whose protocol addresses devices
/// itself in any mode.
std::vector<std::string> addressesFor(const DriverDescriptor& descriptor, DiscoveryMode mode,
                                      const DiscoveryConfig& config) {
    // Quick mode probes with NO options at all, exactly as it always has. Passing the driver's
    // own default looks equivalent -- same value, same probe -- but it makes every quick
    // candidate claim an address it did not discover, and the wizard prefers a discovered
    // address over the stored configuration. A bridge configured at unit id 7 would have had
    // quick discovery quietly propose 1 (review, 2026-07-26).
    if (mode != DiscoveryMode::Extended || !descriptor.hasSweepableAddress()) {
        return {std::string{}};
    }
    const std::string        def = descriptor.optionOr(DriverOptions{}, descriptor.addressOptionKey);
    const auto               bounds = descriptor.addressRange();
    std::vector<std::string> out;
    // The driver's own default first -- but only if the driver would accept it. A default
    // outside its own declared bounds would be probed, reported, offered by the wizard, and
    // then refused by the PATCH gate: a dead end on the confirm step.
    const long parsedDefault = std::strtol(def.c_str(), nullptr, 10);
    if (!def.empty() && parsedDefault >= bounds.first && parsedDefault <= bounds.second) {
        out.push_back(def);
    }
    for (const int address : config.sweepAddresses) {
        // Outside what the driver declares is not a gap in the sweep, it is a value the driver
        // would refuse anyway -- the option bounds are the protocol's.
        if (address < bounds.first || address > bounds.second) {
            continue;
        }
        const std::string value = std::to_string(address);
        if (std::find(out.begin(), out.end(), value) == out.end()) {
            out.push_back(value);
        }
    }
    if (out.empty()) {
        out.push_back(std::string{});  // nothing sweepable after all; probe once, as before
    }
    return out;
}

/// Same driver, same non-empty serial number, two addresses: one inverter mid-reconfiguration,
/// or one still answering on an address it was moved off. Reported once, at the lowest address,
/// with the fact recorded -- two entries for one device would have the owner configure it
/// twice, and two configured devices that resolve to one physical inverter is the collision the
/// boot loop exists to refuse.
///
/// "Lowest", numerically, and not "the first one probed": the driver's own default is probed
/// first and is not necessarily the lowest, so keeping the first would report a device at the
/// higher of the two addresses while claiming the lower (review, 2026-07-26).
void mergeDuplicateSerials(std::vector<DiscoveryCandidate>& candidates) {
    const auto lower = [](const std::string& a, const std::string& b) {
        return std::strtol(a.c_str(), nullptr, 10) < std::strtol(b.c_str(), nullptr, 10);
    };
    for (auto it = candidates.begin(); it != candidates.end(); ++it) {
        if (it->probe.serialNumber.empty()) {
            continue;  // no serial: nothing to match on, and two silent units are two units
        }
        for (auto other = it + 1; other != candidates.end();) {
            if (other->descriptor.id != it->descriptor.id ||
                other->probe.serialNumber != it->probe.serialNumber) {
                ++other;
                continue;
            }
            if (lower(other->address(), it->address())) {
                // Keep the lower address, and with it the options the wizard will configure.
                std::swap(it->matchedOptions, other->matchedOptions);
                std::swap(it->matchedProfile, other->matchedProfile);
            }
            it->probe.evidence.push_back("the same serial number also answered at address " +
                                         other->address());
            other = candidates.erase(other);
        }
    }
}

}  // namespace

DiscoveryEngine::DiscoveryEngine(const DriverRegistry& registry, Transport& transport)
    : registry_(registry), transport_(transport) {}

DiscoveryOutcome DiscoveryEngine::run(DiscoveryMode mode, const DiscoveryConfig& config,
                                      const std::function<void()>& onProbe) {
    DiscoveryOutcome outcome;
    if (mode == DiscoveryMode::Extended) {
        outcome.sweptAddresses = config.sweepAddresses;
    }
    const auto tick = [&onProbe]() {
        if (onProbe) {
            onProbe();
        }
    };

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

        // Every address the driver could be answering at, not just its default. On a chain of
        // identical inverters only the one at the default address was ever a candidate, and the
        // wizard's answer to "which inverters are on this bus" was "there is one at address 1"
        // (#37). The address is also the field a typo makes invisible, which is the failure this
        // is meant to head off.
        const std::vector<std::string> addresses = addressesFor(descriptor, mode, config);

        for (const auto& profile : profiles) {
            bool answeredAtThisProfile = false;
            for (const auto& address : addresses) {
                DriverOptions options;
                if (!address.empty()) {
                    options[descriptor.addressOptionKey] = address;
                }
                auto driver = registry_.create(descriptor.id, transport_, options);
                if (!driver) {
                    continue;
                }
                transport_.configure(profile);
                if (!driver->begin(transport_)) {
                    continue;
                }
                // Re-applied, because begin() configures the line to the driver's OWN first
                // choice. That is correct at boot -- a driver started straight from config must
                // not depend on a discovery run having configured the UART, which was a real
                // bug -- but here it silently undid the sweep: every iteration probed at the
                // driver's default, so the second and later profiles were never actually tried.
                // A device shipped at the family's other baud rate could not be found, and the
                // failure looked exactly like bad wiring (review, 2026-07-25).
                transport_.configure(profile);

                tick();
                ProbeResult first = driver->probe();
                if (!first.responded) {
                    // Bytes that identified nothing are the duplicate-address signal, and the
                    // only one there is: two units on one id answer together and collide. Kept
                    // rather than discarded with the probe -- but only when an address was
                    // actually selected, since at the wrong baud rate the same garbage is
                    // routine and says nothing about addressing.
                    if (first.sawTraffic && !address.empty()) {
                        outcome.unidentified.push_back(
                            {descriptor.id, address,
                             first.evidence.empty() ? std::string{} : first.evidence.back()});
                    }
                    continue;
                }

                DiscoveryCandidate candidate;
                candidate.descriptor     = descriptor;
                candidate.probe          = first;
                candidate.matchedProfile = profile;
                candidate.matchedOptions = options;

                if (config.requireConsistentProbes) {
                    tick();
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
                if (!address.empty()) {
                    candidate.probe.evidence.push_back("answered at address " + address);
                }

                outcome.candidates.push_back(std::move(candidate));
                answeredAtThisProfile = true;
            }
            if (answeredAtThisProfile) {
                // One bus, one set of line settings: if something answered at this profile,
                // every other device on the wire is at this profile too. Trying the driver's
                // remaining baud rates would be silence by construction, at a response timeout
                // per address.
                break;
            }
        }
    }

    mergeDuplicateSerials(outcome.candidates);

    std::stable_sort(outcome.candidates.begin(), outcome.candidates.end(),
                     [](const DiscoveryCandidate& a, const DiscoveryCandidate& b) {
                         return a.probe.confidenceScore > b.probe.confidenceScore;
                     });

    if (outcome.candidates.empty()) {
        // Traffic without an identification is a different fault from silence, and pointing at
        // the wiring when the bus is demonstrably alive sends someone to re-crimp a cable that
        // is fine. Two units left on one address is what this usually is.
        if (!outcome.unidentified.empty()) {
            outcome.reason = "no device could be identified, but address " +
                             outcome.unidentified.front().address +
                             " produced traffic: " + outcome.unidentified.front().note;
            return outcome;
        }
        outcome.reason = "no device answered; check wiring, termination and the RS485 A/B order";
        // Which addresses were asked, so silence at 1..8 is not mistaken for silence
        // everywhere: an inverter parked outside the swept range is configured by hand.
        if (!outcome.sweptAddresses.empty()) {
            outcome.reason += " (addresses " + std::to_string(outcome.sweptAddresses.front()) +
                              "-" + std::to_string(outcome.sweptAddresses.back()) +
                              " and each driver's own default were tried)";
        }
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
    // The runner-up has to be a DIFFERENT driver. Three identical inverters on one bus are now
    // three candidates with the same driver id and near-identical scores, and reading that as
    // "too close to call" would have made the sweep defeat auto-selection exactly on the bus it
    // was built for -- the ambiguity this rule guards against is which PROTOCOL a device speaks,
    // never how many devices answered it.
    const auto runnerUp =
        std::find_if(outcome.candidates.begin(), outcome.candidates.end(),
                     [&best](const DiscoveryCandidate& c) {
                         return c.descriptor.id != best.descriptor.id;
                     });
    if (runnerUp != outcome.candidates.end()) {
        const int margin = best.probe.confidenceScore - runnerUp->probe.confidenceScore;
        if (margin < config.minMargin) {
            outcome.reason = "'" + best.descriptor.id + "' (" +
                             std::to_string(best.probe.confidenceScore) + ") and '" +
                             runnerUp->descriptor.id + "' (" +
                             std::to_string(runnerUp->probe.confidenceScore) +
                             ") are too close to call; confirm manually";
            return outcome;
        }
    }

    const size_t sameDriver =
        static_cast<size_t>(std::count_if(outcome.candidates.begin(), outcome.candidates.end(),
                                          [&best](const DiscoveryCandidate& c) {
                                              return c.descriptor.id == best.descriptor.id;
                                          }));

    outcome.autoSelected     = true;
    outcome.selectedDriverId = best.descriptor.id;
    outcome.selectedOptions  = best.matchedOptions;
    outcome.reason           = "'" + best.descriptor.id + "' matched convincingly (" +
                     std::to_string(best.probe.confidenceScore) + "/100)";
    if (!best.address().empty()) {
        outcome.reason += " at address " + best.address();
    }
    // Said here rather than left to be counted off the candidate list: the wizard configures
    // ONE device, and on a bus of three the other two are the ones the owner has to remember.
    if (sameDriver > 1) {
        outcome.reason += "; " + std::to_string(sameDriver) +
                          " devices answered it -- add the others under Settings, Extra devices";
    }
    return outcome;
}

}  // namespace heliograph
