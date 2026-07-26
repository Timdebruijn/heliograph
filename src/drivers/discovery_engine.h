// SPDX-License-Identifier: MIT
//
// Safe automatic identification.
//
// Discovery only ever calls InverterDriver::probe(), never execute(). Everything a probe is
// forbidden from doing -- writes, function codes 5/6/15/16, broadcast writes, start/stop,
// power limits, clock setting, factory reset, firmware updates, brute-force scanning -- is
// therefore unreachable from here by construction, not by discipline.
//
// One honest exception, and it predates the address sweep: a driver for a protocol that
// REGISTERS devices (the AA55/PMU family) hands the inverter a bus address as part of probing
// it at all -- there is no read-only way to discover such a device. That is why those drivers
// name no addressOptionKey: the sweep must not turn one address assignment into nine.

#pragma once

#include <functional>
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

    /// Bus addresses Extended mode tries, on top of each driver's own default. Quick mode
    /// ignores this entirely.
    ///
    /// 1..8 rather than the protocol's full range: Modbus allows 1-247, and probing 247
    /// addresses across every driver and profile is hours of traffic on someone's live bus for
    /// a bridge that will poll at most kMaxDevices of them anyway. Eight is the number the
    /// firmware can actually use. An inverter parked at 30 is still configurable by hand -- the
    /// wizard just does not promise to find it, and reports which addresses it tried so that
    /// silence at 1-8 is not read as silence everywhere. See docs/rest-api.md.
    std::vector<int> sweepAddresses{1, 2, 3, 4, 5, 6, 7, 8};
};

struct DiscoveryCandidate {
    DriverDescriptor descriptor;
    ProbeResult      probe;
    /// The line settings this candidate actually answered at -- not the driver's first
    /// recommendation. Those were the same thing only because begin() used to undo the sweep,
    /// so every probe really did happen at the driver default. Now that the sweep works, the
    /// two can differ, and reporting the recommendation would be an active lie in the wizard.
    SerialProfile matchedProfile{};
    /// The options this candidate answered at -- in practice the bus address, when the driver
    /// has one. Empty means the driver's own defaults, which is every candidate Quick mode can
    /// produce. Handed to the wizard as-is, so what gets configured is what answered rather
    /// than a default that happened to be right the first time.
    DriverOptions matchedOptions;
    /// False when a repeat probe contradicted the first; the score is halved in that case.
    bool consistent = true;

    /// The bus address this answered at, or empty when the driver has no address option.
    std::string address() const {
        const auto it = matchedOptions.find(descriptor.addressOptionKey);
        return it == matchedOptions.end() ? std::string{} : it->second;
    }
};

/// An address where bytes came back that identified nothing.
///
/// The reason the sweep is worth having at all: two inverters left on one unit id answer
/// together, their replies collide, and the result is a failed checksum -- not silence. Reported
/// separately from the candidates because it is not a device, and thrown away before this
/// existed, so the wizard blamed the wiring for the one fault it could actually name.
struct UnidentifiedAddress {
    std::string driverId;
    std::string address;
    /// The probe's own words, shown verbatim.
    std::string note;
};

struct DiscoveryOutcome {
    /// Highest score first.
    std::vector<DiscoveryCandidate> candidates;
    /// Addresses that produced traffic without a usable reply. Never a device.
    std::vector<UnidentifiedAddress> unidentified;
    bool                            autoSelected = false;
    std::string                     selectedDriverId;
    /// The options the selected candidate answered at, so the wizard configures the device it
    /// found rather than the one at the driver's default address.
    DriverOptions selectedOptions;
    /// Addresses that were tried beyond each driver's default. Empty in Quick mode. Reported so
    /// "nothing else answered" can be told apart from "nothing else was asked".
    std::vector<int> sweptAddresses;
    /// Why it was or was not auto-selected, shown to the user verbatim.
    std::string reason;
};

class DiscoveryEngine {
public:
    DiscoveryEngine(const DriverRegistry& registry, Transport& transport);

    /// `onProbe` runs immediately before every probe. It exists for one reason: an extended
    /// sweep on a silent bus is dozens of response timeouts back to back, all inside a single
    /// rs485Task iteration, and the task watchdog does not care that the delay is legitimate.
    /// The caller feeds it here. Host tests pass nothing.
    DiscoveryOutcome run(DiscoveryMode mode, const DiscoveryConfig& config = {},
                         const std::function<void()>& onProbe = {});

private:
    const DriverRegistry& registry_;
    Transport&            transport_;
};

}  // namespace heliograph
