// SPDX-License-Identifier: MIT
//
// The single gate every write must pass through, whatever asked for it.
//
// Fully implemented in the MVP even though the only shipping driver rejects everything: the
// read-only guarantee is worth having as a tested contract rather than as an absence of code.
// When a writable driver is added later, the safety checks already exist and are already
// proven, instead of being written under pressure alongside the first thing that can move a
// real inverter.

#pragma once

#include <functional>
#include <mutex>
#include <string>

#include "device/capability.h"
#include "device/clock.h"
#include "device/command.h"
#include "drivers/inverter_driver.h"

namespace heliograph {

struct RateLimitPolicy {
    /// Minimum spacing between accepted commands.
    uint32_t minIntervalMs = 1000;
    /// How many may be issued back to back before the spacing applies.
    uint32_t burst = 3;
};

struct DispatchOutcome {
    CommandResult result = CommandResult::Rejected;
    /// Why, in terms a REST client or the web UI can show verbatim.
    std::string reason;
};

class CommandDispatcher {
public:
    explicit CommandDispatcher(ClockFn clock, RateLimitPolicy rateLimit = {});

    /// Global kill switch, independent of driver capabilities. On in the MVP.
    void setReadOnlyMode(bool readOnly);
    bool readOnlyMode() const;

    /// Checks, in order: read-only mode, capability, value range, rate limit. Only then does
    /// the command reach the driver.
    ///
    /// Thread-safe. CommandSource already names Mqtt, Rest, ModbusTcp and Web -- three different
    /// tasks -- and the burst counter is read-modify-write, so two concurrent commands could
    /// each see the last free slot and both take it, quietly exceeding the burst. Locked
    /// internally rather than left to every call site: RelayController gets away with being
    /// lock-free only because main.cpp wraps every one of its callers in the same mutex, and
    /// that is an easy contract to break from a new call site. Note the driver's execute() runs
    /// under this lock, which also serialises the bus access a write implies.
    DispatchOutcome dispatch(const InverterCommand& command, InverterDriver& driver);

private:
    bool allowedByRateLimit(uint64_t nowMs);

    mutable std::mutex m_;
    ClockFn            clock_;
    RateLimitPolicy    rateLimit_;
    bool               readOnly_ = true;

    // Explicit flag, not a "0 means never" sentinel: millis() at boot IS near zero, and
    // the sentinel collision let the first post-boot window bypass the throttle (found by
    // the RelayController's twin of this logic, 2026-07-22).
    bool     everAccepted_   = false;
    uint64_t lastAcceptedMs_ = 0;
    uint32_t burstUsed_      = 0;
};

}  // namespace heliograph
