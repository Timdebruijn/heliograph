// SPDX-License-Identifier: MIT

#include "command_queue.h"

namespace heliograph {

bool CommandQueue::submit(Request request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.has_value()) {
        return false;
    }
    pending_ = std::move(request);
    return true;
}

std::optional<CommandQueue::Request> CommandQueue::take() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_.has_value()) {
        return std::nullopt;
    }
    std::optional<Request> taken = std::move(pending_);
    pending_.reset();
    return taken;
}

void CommandQueue::recordOutcome(const std::string& deviceId, const std::string& requestId,
                                 DispatchOutcome outcome) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastOutcomeDeviceId_  = deviceId;
    lastOutcomeRequestId_ = requestId;
    lastOutcome_          = std::move(outcome);
}

std::optional<DispatchOutcome> CommandQueue::outcomeFor(const std::string& deviceId,
                                                       const std::string& requestId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lastOutcomeDeviceId_ != deviceId || lastOutcomeRequestId_ != requestId) {
        return std::nullopt;
    }
    return lastOutcome_;
}

}  // namespace heliograph
