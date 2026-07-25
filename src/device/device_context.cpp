// SPDX-License-Identifier: MIT

#include "device_context.h"

namespace heliograph {

DeviceContext::DeviceContext(InverterDriver& driver, StateStore& store, Diagnostics& diagnostics,
                             ClockFn clock, PollPolicy policy)
    : driver_(driver),
      store_(store),
      diagnostics_(diagnostics),
      clock_(std::move(clock)),
      policy_(policy) {
    state_.bridgeOnline = true;
    state_.identity     = driver_.identity();
    state_.capabilities = driver_.capabilities();
    // Read the driver's tally rather than assuming zero. Today it always IS zero -- main.cpp
    // builds a fresh driver right before this context, discovery uses its own throwaway
    // instances, and no begin() touches the bus -- so this is defence, not a fix for anything
    // observed. It costs one read and it means a driver that arrives pre-used cannot inject its
    // history into the installation's metrics as one enormous step.
    lastBusErrors_ = driver_.busErrors();
    store_.publish(state_);
}

void DeviceContext::recordBusErrors() {
    const BusErrorCounts now = driver_.busErrors();
    // Unsigned differences, so a counter that wraps still yields the right delta.
    diagnostics_.recordChecksumErrors(now.checksumErrors - lastBusErrors_.checksumErrors);
    diagnostics_.recordTimeouts(now.timeouts - lastBusErrors_.timeouts);
    diagnostics_.recordInvalidFrames(now.invalidFrames - lastBusErrors_.invalidFrames);
    lastBusErrors_ = now;
}

PollResult DeviceContext::pollOnce() {
    const uint64_t now = clock_ ? clock_() : 0;
    lastAttemptMs_     = now;
    everPolled_        = true;

    // Set before polling so the driver can stamp its measurements with this attempt's time.
    state_.lastPollAttemptMs = now;

    // The driver writes into a scratch copy. On any failure we discard it wholesale, which is
    // what makes "a corrupted frame never becomes data" true by construction rather than by
    // each driver remembering to be careful.
    DeviceState working = state_;
    const PollResult result = driver_.poll(working);

    // Unconditionally, and before the verdict is even looked at. The bus counters used to be a
    // switch over `result`, which meant they only ever moved on a poll that failed OUTRIGHT --
    // and a driver reading several blocks fails outright only when every one of them does. A
    // bus corrupting a third of its frames kept polling Ok, so the counter that indicts the
    // cabling stayed at zero for exactly the degrading bus it should have caught first.
    recordBusErrors();

    if (result == PollResult::Ok) {
        state_ = std::move(working);
        state_.recordPollSuccess(now, policy_.staleness);
        diagnostics_.recordPollSuccess(now);
    } else {
        state_.recordPollFailure(now, policy_.staleness);
        diagnostics_.recordPollFailure();
        // Only the outcome, never payload bytes: this string is published over MQTT and REST.
        diagnostics_.setLastError(std::string("poll failed: ") + pollResultName(result));
    }

    state_.bridgeOnline = true;
    store_.publish(state_);
    return result;
}

uint32_t DeviceContext::nextDelayMs() const {
    if (state_.consecutiveFailures == 0) {
        return policy_.intervalMs;
    }
    uint32_t delay = policy_.intervalMs;
    for (uint32_t i = 1; i < state_.consecutiveFailures && delay < policy_.maxBackoffMs; ++i) {
        delay *= 2;
    }
    return delay > policy_.maxBackoffMs ? policy_.maxBackoffMs : delay;
}

bool DeviceContext::due(uint64_t nowMs) const {
    if (!everPolled_) {
        return true;
    }
    if (nowMs < lastAttemptMs_) {
        return true;  // clock moved backwards; do not stall for the rest of the epoch
    }
    return nowMs - lastAttemptMs_ >= nextDelayMs();
}

}  // namespace heliograph
