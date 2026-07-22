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
    void setReadOnlyMode(bool readOnly) { readOnly_ = readOnly; }
    bool readOnlyMode() const { return readOnly_; }

    /// Checks, in order: read-only mode, capability, value range, rate limit. Only then does
    /// the command reach the driver.
    DispatchOutcome dispatch(const InverterCommand& command, InverterDriver& driver);

private:
    bool allowedByRateLimit(uint64_t nowMs);

    ClockFn         clock_;
    RateLimitPolicy rateLimit_;
    bool            readOnly_ = true;

    // Explicit flag, not a "0 means never" sentinel: millis() at boot IS near zero, and
    // the sentinel collision let the first post-boot window bypass the throttle (found by
    // the RelayController's twin of this logic, 2026-07-22).
    bool     everAccepted_   = false;
    uint64_t lastAcceptedMs_ = 0;
    uint32_t burstUsed_      = 0;
};

}  // namespace heliograph
