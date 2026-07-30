// SPDX-License-Identifier: MIT
//
// Ties one driver to one state store and owns the polling rhythm.
//
// This is the only writer of a device's state. It runs on rs485Task and never blocks on any
// output adapter.

#pragma once

#include <functional>
#include <string>

#include "device/clock.h"
#include "device/device_state.h"
#include "diagnostics/diagnostics.h"
#include "drivers/inverter_driver.h"
#include "state/state_store.h"

namespace heliograph {

struct PollPolicy {
    uint32_t        intervalMs   = 10000;
    /// Back-off after failures: interval doubles up to this ceiling. Bounded on purpose --
    /// an inverter that is merely asleep must be picked up again promptly at sunrise, so the
    /// back-off exists to spare the bus, not to give up.
    uint32_t        maxBackoffMs = 60000;
    StalenessPolicy staleness{};
};

class DeviceContext {
public:
    /// `label` is the operator's name for this device, or empty. Taken at construction rather
    /// than through a setter because the constructor publishes a first snapshot, and rs485Task
    /// is already running by the time setup() gets here: a setter called afterwards would race
    /// the poll task for state_, which is exactly what the missing workingState() accessor
    /// below exists to prevent.
    DeviceContext(InverterDriver& driver, StateStore& store, Diagnostics& diagnostics,
                  ClockFn clock, const PollPolicy& policy = {}, std::string label = {});

    /// One poll attempt. Publishes a new snapshot either way, so that consumers always see
    /// current liveness even while the device is unreachable.
    PollResult pollOnce();

    /// Milliseconds until the next attempt, after back-off.
    uint32_t nextDelayMs() const;

    /// True when pollOnce() should be called again.
    bool due(uint64_t nowMs) const;

    /// The driver this context polls. Exposed so rs485Task can resolve a queued command to the
    /// right driver: dispatching, like pollOnce(), only ever happens on the task that owns the
    /// bus, so this cannot race pollOnce() -- one task, one bus, one operation per iteration.
    InverterDriver& driver() { return driver_; }

    // Deliberately no workingState() accessor and no setBridgeOnline(): both were unused traps
    // (review, 2026-07-21). A raw reference into the rs485Task-mutated state_ invited a data
    // race from any other task; readers must go through StateStore::snapshot(). And a
    // setBridgeOnline(false) was silently overwritten by pollOnce() on the next tick, so it
    // never worked -- bridgeOnline is simply true while the poll loop runs.

private:
    /// Feeds the difference in the driver's frame-level tallies into Diagnostics. Called on
    /// every poll, successful or not.
    void recordBusErrors();

    InverterDriver& driver_;
    StateStore&     store_;
    Diagnostics&    diagnostics_;
    ClockFn         clock_;
    PollPolicy      policy_;

    DeviceState state_;
    uint64_t    lastAttemptMs_ = 0;
    bool        everPolled_    = false;
    /// The driver's cumulative tallies as of the previous poll.
    BusErrorCounts lastBusErrors_{};
};

}  // namespace heliograph
