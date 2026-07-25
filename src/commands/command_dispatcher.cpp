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

/// Whether this command lifts a restriction rather than imposing one.
///
/// The rate limiter exists to spare the bus, but it must never be the reason a curtailed
/// inverter stays curtailed. Which direction counts as safe is not obvious and is worth
/// stating: under a grid-protection reading you would exempt Stop, because being unable to shut
/// an inverter down is the hazard. This project takes the opposite stance throughout -- the
/// failsafe in docs/drm.md is "bridge dead, inverter keeps producing", and RelayController
/// exempts OFF for exactly that reason -- so here the protected direction is the one that gives
/// production back.
///
/// Deliberately narrow. Start is unambiguous. A limit command counts only when it is set to the
/// driver's own declared maximum, i.e. explicitly no limit; anything below that is still a
/// restriction and pays for its token. SoC floors and ceilings are not included: "maximum" does
/// not mean "unrestricted" for those, and inventing a direction for a command no driver
/// implements yet would be guessing.
bool liftsRestriction(const InverterCommand& command, const NumericCapability& nc) {
    if (command.type == InverterCommandType::Start) {
        return true;
    }
    const bool isLimit = command.type == InverterCommandType::SetActivePowerLimitPercent ||
                         command.type == InverterCommandType::SetActivePowerLimitWatts ||
                         command.type == InverterCommandType::SetExportLimitWatts;
    return isLimit && nc.supported && nc.writable && command.numericValue.has_value() &&
           *command.numericValue >= nc.maximum;
}

}  // namespace

void CommandDispatcher::setReadOnlyMode(bool readOnly) {
    std::lock_guard<std::mutex> lock(m_);
    readOnly_ = readOnly;
}

bool CommandDispatcher::readOnlyMode() const {
    std::lock_guard<std::mutex> lock(m_);
    return readOnly_;
}

DispatchOutcome CommandDispatcher::dispatch(const InverterCommand& command,
                                            InverterDriver&        driver) {
    std::lock_guard<std::mutex> lock(m_);
    const uint64_t              now = clock_ ? clock_() : 0;

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

    // 4. Rate limit, last: a rejected command should not consume the allowance. Commands that
    //    lift a restriction skip it entirely and spend no token -- see liftsRestriction().
    if (!liftsRestriction(command, nc) && !allowedByRateLimit(now)) {
        return {CommandResult::RateLimited, "too many commands; try again shortly"};
    }

    const auto result = driver.execute(command);
    if (result == CommandResult::Ok) {
        return {result, "accepted"};
    }
    return {result, std::string("driver rejected the command: ") + commandResultName(result)};
}

}  // namespace heliograph
