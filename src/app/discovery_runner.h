// SPDX-License-Identifier: MIT
//
// Runs discovery on the task that owns the RS485 bus.
//
// The web handler cannot call DiscoveryEngine directly: probing takes exclusive use of the
// bus for seconds and would block an AsyncTCP callback, and two components talking on the bus
// at once is precisely what the bus lock exists to prevent. So the handler *requests*, and
// rs485Task picks it up on its next cycle.
//
// Pure apart from the clock, so the handover and the "already running" rule are host-tested.

#pragma once

#include <functional>
#include <mutex>
#include <string>

#include "device/clock.h"
#include "drivers/discovery_engine.h"
#include "drivers/driver_registry.h"

namespace heliograph {

enum class DiscoveryStatus : uint8_t {
    /// Never run since boot.
    Idle,
    /// Requested, not yet picked up by rs485Task.
    Requested,
    Running,
    Done,
    /// The engine could not run at all (no transport, bus busy).
    Failed,
};

const char* discoveryStatusName(DiscoveryStatus status);

struct DiscoveryReport {
    DiscoveryStatus  status = DiscoveryStatus::Idle;
    DiscoveryMode    mode   = DiscoveryMode::Quick;
    DiscoveryOutcome outcome;
    uint64_t         requestedMs = 0;
    uint64_t         startedMs   = 0;
    uint64_t         finishedMs  = 0;
    /// Only set when status == Failed.
    std::string error;
};

class DiscoveryRunner {
public:
    DiscoveryRunner(const DriverRegistry& registry, ClockFn clock);

    /// Called from the web task. False when a run is already pending or in flight -- the REST
    /// layer turns that into 409 rather than queueing, because a second probe of the same bus
    /// tells you nothing the first one will not.
    bool request(bool extended);

    /// Called from rs485Task. Runs a pending request, if any, and returns true if it ran.
    /// Polling must be paused around this by the caller: probing re-registers every inverter
    /// on the bus.
    bool runIfRequested(Transport& transport);

    DiscoveryReport report() const;
    bool            busy() const;

private:
    const DriverRegistry& registry_;
    ClockFn               clock_;
    mutable std::mutex    mutex_;
    DiscoveryReport       report_;
};

}  // namespace heliograph
