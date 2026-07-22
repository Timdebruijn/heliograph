// SPDX-License-Identifier: MIT

#include "relay_controller.h"

namespace heliograph {

RelayController::RelayController(ClockFn clock, RateLimitPolicy rateLimit)
    : clock_(std::move(clock)), rateLimit_(rateLimit) {}

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

// Same refill logic as CommandDispatcher::allowedByRateLimit, with one fix: "has ever
// accepted" is an explicit flag instead of the lastAcceptedMs_ == 0 sentinel. millis() at
// boot is near zero, so the sentinel collides exactly when the device just started -- and
// the first seconds after a boot are when unthrottled relay chatter is least welcome.
// Duplicated rather than shared through a base class by intent: two small, independently
// testable copies beat a coupling between the inverter write path and the bridge actuator.
bool RelayController::allowedByRateLimit(uint64_t nowMs) {
    if (everAccepted_ && nowMs - lastAcceptedMs_ >= rateLimit_.minIntervalMs) {
        burstUsed_ = 0;
    }
    if (burstUsed_ < rateLimit_.burst) {
        ++burstUsed_;
        everAccepted_   = true;
        lastAcceptedMs_ = nowMs;
        return true;
    }
    if (!everAccepted_ || nowMs - lastAcceptedMs_ >= rateLimit_.minIntervalMs) {
        everAccepted_   = true;
        lastAcceptedMs_ = nowMs;
        return true;
    }
    return false;
}

CommandResult RelayController::set(uint8_t index, bool energised) {
    // 1. Global kill switch, before anything else -- identical position to the dispatcher.
    if (readOnly_) {
        return CommandResult::ReadOnlyMode;
    }
    // 2. Feature flag. A relay board with default settings must be inert.
    if (!enabled_) {
        return CommandResult::Rejected;
    }
    // 3. Index validity.
    if (index >= count_) {
        return CommandResult::OutOfRange;
    }
    // 4. Rate limit -- but only towards asserting. Releasing curtailment (OFF) is the safe
    // direction and must never wait behind a throttle.
    if (energised) {
        const uint64_t now = clock_ ? clock_() : 0;
        if (!allowedByRateLimit(now)) {
            return CommandResult::RateLimited;
        }
    }
    state_[index] = energised;
    if (apply_) {
        apply_(index, energised);
    }
    return CommandResult::Ok;
}

}  // namespace heliograph
