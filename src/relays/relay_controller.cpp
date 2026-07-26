// SPDX-License-Identifier: MIT

#include "relay_controller.h"

namespace heliograph {

RelayController::RelayController(ClockFn clock, RateLimitPolicy rateLimit)
    : clock_(std::move(clock)), limiter_(rateLimit) {}

void RelayController::begin(uint8_t count, RelayApplyFn apply) {
    count_ = count > kMaxRelays ? kMaxRelays : count;
    apply_ = std::move(apply);
    allOff();
}

void RelayController::allOff() {
    for (uint8_t i = 0; i < count_; ++i) {
        state_[i] = false;
        if (apply_) {
            apply_(i, false);
        }
    }
}

// Gates 1 and 2, in the order the dispatcher uses. One definition so a gate added later cannot
// be added to set() and forgotten in applyPattern() -- the direction in which that mistake
// fails is a relay that moves when it should have been refused.
//
//   1. Global kill switch, before anything else.
//   2. Feature flag. A relay board with default settings must be inert.
//
// Gates 3 (what is being addressed) and 4 (the rate limit) differ per operation and stay with
// their callers: an index versus a pattern width, one token versus one token for the whole
// pattern.
CommandResult RelayController::checkGates() const {
    if (readOnly_) {
        return CommandResult::ReadOnlyMode;
    }
    if (!enabled_) {
        return CommandResult::Rejected;
    }
    return CommandResult::Ok;
}

CommandResult RelayController::set(uint8_t index, bool energised) {
    if (const CommandResult gate = checkGates(); gate != CommandResult::Ok) {
        return gate;
    }
    // 3. Index validity.
    if (index >= count_) {
        return CommandResult::OutOfRange;
    }
    // 4. Rate limit -- but only towards asserting. Releasing curtailment (OFF) is the safe
    // direction and must never wait behind a throttle.
    if (energised) {
        const uint64_t now = clock_ ? clock_() : 0;
        if (!allowedToAssert(now)) {
            return CommandResult::RateLimited;
        }
    }
    state_[index] = energised;
    if (apply_) {
        apply_(index, energised);
    }
    return CommandResult::Ok;
}

CommandResult RelayController::applyPattern(const std::vector<bool>& pattern) {
    if (const CommandResult gate = checkGates(); gate != CommandResult::Ok) {
        return gate;
    }
    if (pattern.size() != static_cast<size_t>(count_)) {
        return CommandResult::OutOfRange;
    }
    bool assertsAny = false;
    for (uint8_t i = 0; i < count_; ++i) {
        if (pattern[i]) {
            assertsAny = true;
            break;
        }
    }
    // One token for the whole pattern, and only when it asserts anything: a release-only
    // pattern is the safe direction and passes unconditionally, like set(index, false).
    if (assertsAny) {
        const uint64_t now = clock_ ? clock_() : 0;
        if (!allowedToAssert(now)) {
            return CommandResult::RateLimited;
        }
    }
    // All gates passed: nothing below can refuse, so the pattern lands whole. OFFs first --
    // when switching modes, releasing the old role's contacts must precede asserting the
    // new one's.
    for (uint8_t i = 0; i < count_; ++i) {
        if (!pattern[i]) {
            state_[i] = false;
            if (apply_) {
                apply_(i, false);
            }
        }
    }
    for (uint8_t i = 0; i < count_; ++i) {
        if (pattern[i]) {
            state_[i] = true;
            if (apply_) {
                apply_(i, true);
            }
        }
    }
    return CommandResult::Ok;
}

}  // namespace heliograph
