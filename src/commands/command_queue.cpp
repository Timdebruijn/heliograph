// SPDX-License-Identifier: MIT

#include "command_queue.h"

namespace heliograph {

bool CommandQueue::submit(Request request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.has_value()) {
        return false;
    }
    for (auto& slot : outcomes_) {
        if (slot.has_value() && slot->deviceId == request.deviceId &&
            slot->requestId == request.command.requestId) {
            slot.reset();
        }
    }
    pending_ = std::move(request);
    return true;
}

std::optional<CommandQueue::Request> CommandQueue::take(uint64_t nowMs,
                                                        std::optional<Request>* expired) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_.has_value()) {
        return std::nullopt;
    }
    std::optional<Request> taken = std::move(pending_);
    pending_.reset();
    // Clamped, never wrapped: a clock behind the stamp reads as "just submitted", not as an
    // enormous age. The clock is monotonic in the firmware; the clamp is for the injected one.
    const uint64_t ageMs = nowMs > taken->submittedAtMs ? nowMs - taken->submittedAtMs : 0;
    if (ageMs > kMaxCommandAgeMs) {
        // Rounded up, so a wait just past the limit never reads as equal to it.
        recordLocked(taken->deviceId, taken->command.requestId,
                     DispatchOutcome{CommandResult::Rejected,
                                     "expired: waited " + std::to_string((ageMs + 999) / 1000) +
                                         " s for the bus (limit " +
                                         std::to_string(kMaxCommandAgeMs / 1000) +
                                         " s); not sent, send it again"});
        if (expired != nullptr) {
            *expired = std::move(taken);
        }
        return std::nullopt;
    }
    return taken;
}

void CommandQueue::recordOutcome(const std::string& deviceId, const std::string& requestId,
                                 DispatchOutcome outcome) {
    std::lock_guard<std::mutex> lock(mutex_);
    recordLocked(deviceId, requestId, std::move(outcome));
}

void CommandQueue::recordLocked(const std::string& deviceId, const std::string& requestId,
                                DispatchOutcome outcome) {
    outcomes_[next_] = Outcome{deviceId, requestId, std::move(outcome)};
    next_            = (next_ + 1) % outcomes_.size();
}

std::optional<DispatchOutcome> CommandQueue::outcomeFor(const std::string& deviceId,
                                                       const std::string& requestId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Newest first, so a request id reused by its caller answers with its latest outcome.
    for (std::size_t i = 1; i <= outcomes_.size(); ++i) {
        const auto& slot = outcomes_[(next_ + outcomes_.size() - i) % outcomes_.size()];
        if (slot.has_value() && slot->deviceId == deviceId && slot->requestId == requestId) {
            return slot->outcome;
        }
    }
    return std::nullopt;
}

}  // namespace heliograph
