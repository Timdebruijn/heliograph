// SPDX-License-Identifier: MIT

#include "command_dispatcher.h"

#include <cmath>

namespace heliograph {
namespace {

bool onStep(double value, double minimum, double step) {
    if (step <= 0.0) {
        return true;
    }
    const double steps = (value - minimum) / step;
    return std::fabs(steps - std::round(steps)) < 1e-6;
}

}  // namespace

CommandDispatcher::CommandDispatcher(ClockFn clock, RateLimitPolicy rateLimit)
    : clock_(std::move(clock)), rateLimit_(rateLimit) {}

bool CommandDispatcher::allowedByRateLimit(uint64_t nowMs) {
    if (lastAcceptedMs_ != 0 && nowMs - lastAcceptedMs_ >= rateLimit_.minIntervalMs) {
        burstUsed_ = 0;  // quiet long enough, the allowance refills
    }
    if (burstUsed_ < rateLimit_.burst) {
        ++burstUsed_;
        lastAcceptedMs_ = nowMs;
        return true;
    }
    if (lastAcceptedMs_ == 0 || nowMs - lastAcceptedMs_ >= rateLimit_.minIntervalMs) {
        lastAcceptedMs_ = nowMs;
        return true;
    }
    return false;
}

DispatchOutcome CommandDispatcher::dispatch(const InverterCommand& command,
                                            InverterDriver&        driver) {
    const uint64_t now = clock_ ? clock_() : 0;

    // 1. Global read-only mode. Checked first and independently of the driver, so that
    //    turning it on is sufficient on its own -- it cannot be defeated by a driver that
    //    advertises capabilities it should not have.
    if (readOnly_) {
        return {CommandResult::ReadOnlyMode,
                "bridge is in read-only mode; no write commands are accepted"};
    }

    const auto caps         = driver.capabilities();
    const auto needed       = requiredCapability(command.type);
    const std::string cname = commandTypeName(command.type);

    // 2. Capability. Note this asks the capabilities, never the driver id.
    if (needed == InverterCapability::_Count || !caps.canWrite(needed)) {
        return {CommandResult::Unsupported,
                "the active driver does not support '" + cname + "'"};
    }

    // 3. Range and step, from the driver's own declared bounds.
    const NumericCapability& nc = caps.numeric[static_cast<size_t>(command.type)];
    if (nc.supported && nc.writable) {
        if (!command.numericValue.has_value()) {
            return {CommandResult::OutOfRange, "'" + cname + "' requires a numeric value"};
        }
        const double v = *command.numericValue;
        if (std::isnan(v) || v < nc.minimum || v > nc.maximum) {
            return {CommandResult::OutOfRange,
                    "value for '" + cname + "' is outside the supported range"};
        }
        if (!onStep(v, nc.minimum, nc.step)) {
            return {CommandResult::OutOfRange,
                    "value for '" + cname + "' is not a multiple of the step size"};
        }
    }

    // 4. Rate limit, last: a rejected command should not consume the allowance.
    if (!allowedByRateLimit(now)) {
        return {CommandResult::RateLimited, "too many commands; try again shortly"};
    }

    const auto result = driver.execute(command);
    if (result == CommandResult::Ok) {
        return {result, "accepted"};
    }
    return {result, std::string("driver rejected the command: ") + commandResultName(result)};
}

}  // namespace heliograph
