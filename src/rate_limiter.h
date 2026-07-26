// SPDX-License-Identifier: MIT
//
// Token-bucket throttle: a burst allowance that refills after a quiet period.
//
// WHY THIS IS SHARED, when the two users deliberately kept their own copies until 0.16.0.
// The comment on RelayController's copy argued that "two small, independently testable copies
// beat a coupling between the inverter write path and the bridge actuator" -- and then, in the
// same paragraph, recorded that the copies had drifted: the relay copy found two bugs that the
// dispatcher only adopted afterwards. That is the argument against duplication, written out by
// the duplication itself.
//
// The coupling that comment feared does not follow from sharing. This is a value type, not a
// base class: each user owns an instance, keeps its own policy, its own locking (the dispatcher
// holds a mutex around its call; the relay controller does not need one), and its own decision
// about WHICH operations are throttled at all. Nothing about the inverter write path reaches
// the relays through it. What is shared is only the arithmetic -- and the arithmetic is where
// both bugs were.
//
// THE TWO TRAPS, both found on hardware and both fixed here once:
//
//  1. "Never accepted" is an explicit flag, not a lastAcceptedMs_ == 0 sentinel. millis() at
//     boot IS near zero, so the sentinel collides exactly during the first seconds after a
//     restart -- the throttle would let an unlimited burst through precisely when a bridge is
//     coming up and something is retrying.
//
//  2. Elapsed time is clamped, never wrapped. On an unsigned type a clock that steps backwards
//     turns the subtraction into an enormous number, which reads as "quiet for ages" and
//     refills the whole allowance. The throttle would fail OPEN. Not reachable with the
//     esp_timer-backed monotonic clock this ships with, but the clock is injectable and the
//     failure direction is the permissive one.
//
// Header-only: fifteen lines of arithmetic that both callers should be able to inline, and no
// new translation unit to register in platformio.ini's build_src_filter.

#pragma once

#include <cstdint>

namespace heliograph {

struct RateLimitPolicy {
    /// Minimum spacing between accepted commands.
    uint32_t minIntervalMs = 1000;
    /// How many may be issued back to back before the spacing applies.
    uint32_t burst = 3;
};

/// Not thread-safe by design. Callers that need it hold their own lock around allow(), because
/// only they know how much else belongs inside the same critical section.
class RateLimiter {
public:
    RateLimiter() = default;
    explicit RateLimiter(RateLimitPolicy policy) : policy_(policy) {}

    void setPolicy(RateLimitPolicy policy) { policy_ = policy; }

    /// True if this call may proceed, and records it as accepted. False leaves the state
    /// untouched, so a refused call does not consume the allowance.
    bool allow(uint64_t nowMs) {
        const uint64_t since = nowMs > lastAcceptedMs_ ? nowMs - lastAcceptedMs_ : 0;  // trap 2
        if (everAccepted_ && since >= policy_.minIntervalMs) {
            burstUsed_ = 0;  // quiet long enough, the allowance refills
        }
        if (burstUsed_ < policy_.burst) {
            ++burstUsed_;
            accept(nowMs);
            return true;
        }
        if (!everAccepted_ || since >= policy_.minIntervalMs) {  // trap 1
            accept(nowMs);
            return true;
        }
        return false;
    }

private:
    void accept(uint64_t nowMs) {
        everAccepted_   = true;
        lastAcceptedMs_ = nowMs;
    }

    RateLimitPolicy policy_{};
    bool            everAccepted_   = false;
    uint64_t        lastAcceptedMs_ = 0;
    uint32_t        burstUsed_      = 0;
};

}  // namespace heliograph
