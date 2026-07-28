// SPDX-License-Identifier: MIT
//
// Which configured devices get started, and which are refused before they can do damage.
//
// This lived inside setup(), which is the one file the host build does not compile -- so the
// rule it enforces had no test at all. That rule is not cosmetic: for a driver that cannot
// share a bus, begin() IS the damage. The AA55 handshake opens with a bus-wide RE_REGISTER
// broadcast, so a second instance starting up tells the first, already-polling inverter to
// forget its address. A check that ran afterwards would report the refusal from the far side
// of the harm (#63).
//
// Pure: a configuration and a registry in, a plan out. No transport, no clock, no hardware --
// which is the whole point, because it means the rule can be tested rather than reasoned about.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "config/configuration.h"
#include "drivers/driver_registry.h"

namespace heliograph::app {

/// One configured device and what should happen to it.
struct PlannedDevice {
    std::string          id;
    std::string          label;
    /// Points INTO the configuration that was planned. The plan does not own it, so the
    /// configuration must outlive the plan -- which it does in setup(), where g_config is a
    /// global. Worth stating: the first version of the tests passed a temporary and held
    /// pointers into it, and passed only because nothing read them.
    const DriverOptions* options = nullptr;
    /// As the settings page numbers them: 1 is the `driver` section, 2..N are the additional
    /// devices. Carried because a bring-up failure has to say which ROW to go and look at.
    size_t row = 0;
    /// Empty means start it. Set means refused, in the words the diagnostics surface reports.
    ///
    /// Deliberately not a bool beside a string: two fields that must agree are two fields that
    /// can disagree, and "start it, and here is why it was refused" is not a state worth being
    /// able to represent.
    std::string problem;

    bool shouldStart() const { return problem.empty(); }
};

/// "device 2" or "device 2 (Schuur)".
///
/// The label goes in because "device 2 could not be started" sends someone to count rows on a
/// settings page, and "device 2 (Schuur)" sends them to the shed. The number stays because an
/// unlabelled bridge still has to be told which row.
std::string describeRow(size_t row, const std::string& label);

/// The devices to start, in the order they are configured, each carrying its verdict.
///
/// Order is load-bearing twice over: the poll loop walks this, and the refusal rule is
/// "the FIRST one wins" -- so a plan that reordered would change which device gets the bus.
std::vector<PlannedDevice> planDevices(const Configuration& config, const std::string& driverId,
                                       const DriverRegistry& registry);

}  // namespace heliograph::app
