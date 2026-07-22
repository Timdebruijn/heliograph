// SPDX-License-Identifier: MIT
//
// Safe automatic identification.
//
// Discovery only ever calls InverterDriver::probe(), never execute(). Everything a probe is
// forbidden from doing -- writes, function codes 5/6/15/16, broadcast writes, start/stop,
// power limits, address changes, clock setting, factory reset, firmware updates, brute-force
// scanning -- is therefore unreachable from here by construction, not by discipline.

#pragma once

#include <string>
#include <vector>

#include "driver_registry.h"
#include "inverter_driver.h"

namespace heliograph {

enum class DiscoveryMode : uint8_t {
    /// Auto-detectable drivers, recommended profile, one round each.
    Quick,
    /// All profiles, repeated rounds. Slow and noisier, so the user must ask for it.
    Extended,
};

struct DiscoveryConfig {
    /// A candidate below this is never auto-selected.
    int minConfidence = 80;
    /// The runner-up must be at least this far behind, or the choice is the user's. Two
    /// plausible drivers scoring 82 and 78 is exactly the case where guessing is wrong.
    int minMargin = 20;
    /// Probes run twice and must agree. A single lucky reply is not identification.
    bool requireConsistentProbes = true;
};

struct DiscoveryCandidate {
    DriverDescriptor descriptor;
    ProbeResult      probe;
    /// False when a repeat probe contradicted the first; the score is halved in that case.
    bool consistent = true;
};

struct DiscoveryOutcome {
    /// Highest score first.
    std::vector<DiscoveryCandidate> candidates;
    bool                            autoSelected = false;
    std::string                     selectedDriverId;
    /// Why it was or was not auto-selected, shown to the user verbatim.
    std::string reason;
};

class DiscoveryEngine {
public:
    DiscoveryEngine(const DriverRegistry& registry, Transport& transport);

    DiscoveryOutcome run(DiscoveryMode mode, const DiscoveryConfig& config = {});

private:
    const DriverRegistry& registry_;
    Transport&            transport_;
};

}  // namespace heliograph
