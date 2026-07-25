// SPDX-License-Identifier: MIT
//
// Generic command model. Fully defined in the MVP even though the only shipping driver
// rejects every command: the read-only guarantee is a tested contract, not a promise.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "capability.h"

namespace heliograph {

enum class CommandSource : uint8_t {
    Mqtt,
    Rest,
    ModbusTcp,
    Web,
    Internal,
};

struct InverterCommand {
    InverterCommandType     type{};
    std::optional<double>   numericValue;
    std::optional<int32_t>  enumValue;
    CommandSource           source = CommandSource::Internal;
    std::string             requestId;
    uint64_t                createdAtMs = 0;
};

enum class CommandResult : uint8_t {
    Ok,
    /// The driver does not implement this command at all. A read-only driver returns this
    /// for every command type.
    Unsupported,
    Rejected,
    ReadOnlyMode,
    OutOfRange,
    RateLimited,
    DriverError,
    Timeout,
};

const char* commandResultName(CommandResult result);
const char* commandTypeName(InverterCommandType type);

/// The capability a command type requires. Used by the dispatcher to check a command
/// against the active driver's capabilities without knowing which driver it is.
InverterCapability requiredCapability(InverterCommandType type);

/// Whether this command type carries a number at all.
///
/// A property of the COMMAND, not of what a driver happened to declare. The dispatcher's range
/// check used to be conditional on the driver publishing a NumericCapability, so a driver that
/// set the write bit and left the bounds at their defaults skipped the check entirely and an
/// unbounded value reached execute(). Asking the command type instead means a missing bound is
/// a refusal rather than a bypass.
bool commandTakesNumericValue(InverterCommandType type);

/// Whether this command type carries an enum selection instead of a number.
bool commandTakesEnumValue(InverterCommandType type);

}  // namespace heliograph
