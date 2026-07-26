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

#include <atomic>
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
    ///
    /// Atomic rather than mutex-guarded: it is one bool, and the whole point of a kill switch is
    /// that reaching it never waits. Guarding it with the same mutex as the rate limiter would
    /// have made turning it ON block behind whatever is currently dispatching.
    void setReadOnlyMode(bool readOnly) { readOnly_ = readOnly; }
    bool readOnlyMode() const { return readOnly_; }

    /// Checks, in order: read-only mode, capability, value range, rate limit. Only then does
    /// the command reach the driver.
    ///
    /// Thread-safe for its own state. CommandSource names Mqtt, Rest, ModbusTcp and Web, and the
    /// burst counter is read-modify-write, so two concurrent commands could each see the last
    /// free slot and both take it. (Nothing constructs a dispatcher yet -- there is no write
    /// path. This is what the contract must be when there is one.)
    ///
    /// The lock covers ONLY the rate-limiter bookkeeping and is released before the driver runs.
    /// execute() on a real driver is an RS485 transaction that waits up to 2 s for the bus lock
    /// and then up to 3 s for the reply, and holding a mutex across that would stall every other
    /// caller for seconds -- the same mistake the deferred-poll comment in main.cpp's REST
    /// action handler documents finding live in Phase 3, where a seconds-long bus transaction
    /// ran inside an AsyncTCP callback. Bus exclusivity is
    /// already the transport's job (Transport::lock), so this mutex does not need to provide it.
    ///
    /// Consequence worth knowing: the kill switch is re-read immediately before execute() to
    /// narrow the window, but a command already on the bus cannot be recalled by switching to
    /// read-only, exactly as a poll in flight cannot be.
    DispatchOutcome dispatch(const InverterCommand& command, InverterDriver& driver);

private:
    bool allowedByRateLimit(uint64_t nowMs);
    bool allowedAsRelease(uint64_t nowMs);

    ClockFn           clock_;
    RateLimitPolicy   rateLimit_;
    std::atomic<bool> readOnly_{true};

    /// Guards the rate-limiter fields below, and nothing else.
    std::mutex m_;

    // Explicit flag, not a "0 means never" sentinel: millis() at boot IS near zero, and
    // the sentinel collision let the first post-boot window bypass the throttle (found by
    // the RelayController's twin of this logic, 2026-07-22).
    bool     everAccepted_   = false;
    uint64_t lastAcceptedMs_ = 0;
    uint32_t burstUsed_      = 0;

    // Releases run on their own track: they are never blocked by restricting traffic, but they
    // still keep a minimum spacing so a loop cannot saturate the bus with them. See
    // changesRunState() for why they are treated apart at all.
    bool     everReleased_  = false;
    uint64_t lastReleaseMs_ = 0;
};

}  // namespace heliograph
