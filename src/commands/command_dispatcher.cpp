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
    // Clamped rather than wrapped. On unsigned types a clock that goes backwards turns the
    // subtraction into a huge number, which reads as "ages ago" and refills the whole allowance
    // -- the throttle fails OPEN. Not reachable with the esp_timer-backed monotonic clock this
    // ships with, but the clock is injectable and the failure direction is the permissive one,
    // so it is clamped here the way DeviceContext::due() and updateStaleness() already do.
    const uint64_t since = nowMs > lastAcceptedMs_ ? nowMs - lastAcceptedMs_ : 0;
    if (everAccepted_ && since >= rateLimit_.minIntervalMs) {
        burstUsed_ = 0;  // quiet long enough, the allowance refills
    }
    if (burstUsed_ < rateLimit_.burst) {
        ++burstUsed_;
        everAccepted_   = true;
        lastAcceptedMs_ = nowMs;
        return true;
    }
    if (!everAccepted_ || since >= rateLimit_.minIntervalMs) {
        everAccepted_   = true;
        lastAcceptedMs_ = nowMs;
        return true;
    }
    return false;
}

namespace {

/// Whether this command changes whether the inverter runs at all, rather than by how much.
///
/// These get their own rate-limit track: never blocked by a burst of restricting traffic, but
/// still spaced, so they cannot be dropped when they matter and cannot be looped to saturate the
/// bus either.
///
/// Why they are special at all, stated carefully because an earlier version of this comment got
/// it wrong. It is tempting to borrow the relay failsafe from docs/drm.md -- "bridge dead,
/// inverter keeps producing" -- and conclude that releasing is the protected direction. That
/// argument does NOT transfer. A relay failsafe works because a de-energised coil physically
/// falls back with no firmware involved. A curtailment written to a holding register does the
/// opposite: it SURVIVES a dead bridge, and undoing it needs a living one.
///
/// That is the actual reason to protect these. Because there is no passive fallback for a
/// register write, the only way out of any commanded state is another command -- so the throttle
/// must never be the reason one cannot be issued. And RateLimited here is a drop, not a defer:
/// nothing queues or retries, so a refused command is simply lost. Losing "run" or "stop" is
/// worse than losing "run at 60%", which an automation will send again on its next tick.
///
/// Both directions, deliberately. Whether stopping or starting is the safer failure depends on
/// whose hazard you are reasoning about -- grid protection wants a guaranteed stop, an
/// availability argument wants a guaranteed start -- and there is no need to pick: both carry no
/// value that could be wrong, and both are a single small frame.
///
/// Value-carrying commands are NOT included, including a limit set to its maximum. An earlier
/// version exempted that as "explicitly no limit", which a driver declaring minimum == maximum
/// (or leaving both at 0) turned inside out: full curtailment then satisfied "value >= maximum"
/// and skipped the limiter entirely. Deriving intent from bounds a driver may not have thought
/// about is exactly the trust this batch removed from gate 3.
bool changesRunState(const InverterCommand& command) {
    return command.type == InverterCommandType::Start ||
           command.type == InverterCommandType::Stop;
}

}  // namespace

bool CommandDispatcher::allowedAsRelease(uint64_t nowMs) {
    const uint64_t since = nowMs > lastReleaseMs_ ? nowMs - lastReleaseMs_ : 0;
    if (everReleased_ && since < rateLimit_.minIntervalMs) {
        return false;
    }
    everReleased_  = true;
    lastReleaseMs_ = nowMs;
    return true;
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

    // 3. Range and step. Whether a value is required comes from the COMMAND TYPE, not from what
    //    the driver declared. This check used to sit inside `if (nc.supported && nc.writable)`,
    //    so a driver that set the write bit and left its NumericCapability at the defaults
    //    skipped range checking altogether and an unbounded value went straight to execute() --
    //    a bypass dressed as a check. A declared write with no published bounds is now a
    //    refusal: the driver is telling us it can move something without telling us how far.
    const NumericCapability& nc = caps.numeric[static_cast<size_t>(command.type)];
    if (commandTakesNumericValue(command.type)) {
        if (!nc.supported || !nc.writable) {
            return {CommandResult::Unsupported,
                    "the active driver declares '" + cname +
                        "' writable but publishes no range for it"};
        }
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
    } else if (commandTakesEnumValue(command.type) && !command.enumValue.has_value()) {
        // There is no EnumCapability to range-check against yet, so this only pins presence.
        // The first driver implementing a mode write has to bring its own validation, and this
        // is the reminder that the dispatcher is not doing it for it.
        return {CommandResult::OutOfRange, "'" + cname + "' requires a mode selection"};
    }

    // 4. Rate limit, last: a rejected command should not consume the allowance. Run/stop
    //    commands use a separate track so a burst of restricting traffic cannot swallow them --
    //    see changesRunState(). Only this bookkeeping is locked; the driver runs outside it.
    {
        std::lock_guard<std::mutex> lock(m_);
        const bool allowed = changesRunState(command) ? allowedAsRelease(now)
                                                      : allowedByRateLimit(now);
        if (!allowed) {
            return {CommandResult::RateLimited, "too many commands; try again shortly"};
        }
    }

    // Re-read the kill switch as late as possible: it is not held across the gates, so it can
    // have been thrown while this command was being checked. Narrows the window to the call
    // itself -- a command already on the bus cannot be recalled either way.
    if (readOnly_) {
        return {CommandResult::ReadOnlyMode,
                "bridge is in read-only mode; no write commands are accepted"};
    }

    const auto result = driver.execute(command);
    if (result == CommandResult::Ok) {
        return {result, "accepted"};
    }
    return {result, std::string("driver rejected the command: ") + commandResultName(result)};
}

}  // namespace heliograph
