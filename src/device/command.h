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

/// Every request id this firmware generates itself (see submitCommand() in main.cpp) starts
/// with this. A caller-supplied id (REST body or MQTT payload) is refused if it also starts
/// with it, so the two namespaces can never collide -- without this, a caller-chosen
/// "auto-3" could later collide with the third auto-generated id and silently steal or leak
/// another request's outcome from CommandQueue::outcomeFor() (review, 2026-07-30).
inline constexpr const char* kAutoRequestIdPrefix = "auto-";

/// A caller-supplied request id longer than this is refused rather than truncated. Bounds the
/// JSON accept/reject/outcome acks that embed it (see json_util.h's buildCommand*Payload) well
/// under their byte budgets, so an oversized id cannot itself make one of those payloads
/// overflow and silently ship empty.
inline constexpr size_t kMaxRequestIdLength = 64;

/// Ceiling on a raw command-request body (REST POST /commands or an MQTT command/set message)
/// before it is even handed to parseCommandRequest. A real command body -- type, one numeric
/// or enum value, an optional request_id up to kMaxRequestIdLength -- fits in well under 200
/// bytes; this leaves generous headroom for a future field without accepting an arbitrarily
/// large payload from an unauthenticated MQTT publisher (REST bodies are already bounded by
/// the API's own kMaxRequestBytes at the transport layer, but MQTT has no equivalent gate
/// upstream of this).
inline constexpr size_t kMaxCommandPayloadBytes = 512;

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

/// The reverse of commandTypeName: the type whose name is `name`, or false if none matches.
/// Reuses commandTypeName's own table by comparing against it, so the two directions cannot
/// drift apart the way two independent switches could.
bool commandTypeFromName(const std::string& name, InverterCommandType& out);

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
