// SPDX-License-Identifier: MIT
//
// What a POST /api/v1/actions/capture query asks for, and whether it is allowed to ask it.
//
// Split out of the route handler for one reason: rest_api.cpp is 97% inside `#if defined(ESP32)`,
// so on the host build it compiles to a stub. Every bound, every refusal and every default in
// that endpoint was therefore unreachable by any test -- not untested by oversight, untestable
// by construction. The validation is pure, it has more branches than anything else in the file,
// and it is exactly the layer where a wrong 400 or a missing bound gets in.
//
// The web server does not appear here at all. The caller passes a lookup, so the rules can be
// exercised with a std::map on the host and with AsyncWebServerRequest on the device -- one
// implementation, two callers, no second copy of the bounds to drift.

#pragma once

#include <functional>
#include <optional>

#include "app/driver_capture_runner.h"
#include "diagnostics/bus_tap.h"
#include "diagnostics/frame_capture.h"
#include "rest_payloads.h"
#include "transport/serial_profile.h"

namespace heliograph::rest {

/// A checked request. Exactly one of the two halves is meaningful, per `driverMode`.
struct CaptureRequest {
    bool                driverMode = false;
    diag::CaptureConfig config;   ///< passive mode
    SerialProfile       profile;  ///< passive mode
    diag::TapConfig     tap;      ///< driver mode
};

/// Reads one query parameter. Returns nullptr when it is absent -- which is different from
/// present-and-empty, and the difference matters: `?parity=` is a value the operator typed and
/// gets validated, while an absent parity keeps the default.
using QueryParam = std::function<const char*(const char* name)>;

/// Validates, and fills `out` when it passes. Returns the error to send otherwise.
///
/// Knows nothing about whether the build can capture or whether the bus is free -- those are
/// facts about the running bridge, not about the request, and they stay with the handler that
/// can see them.
std::optional<ApiError> parseCaptureRequest(const QueryParam& param, CaptureRequest& out);

/// Bounds for the passive mode, here rather than as literals in the handler so a test asserts
/// the same numbers the endpoint enforces.
inline constexpr long kMaxPassiveCaptureFrames = 256;
inline constexpr long kMinBaudRate             = 300;
inline constexpr long kMaxBaudRate             = 921600;

}  // namespace heliograph::rest
