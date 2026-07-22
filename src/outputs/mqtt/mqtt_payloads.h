// SPDX-License-Identifier: MIT
//
// JSON payload construction. Pure: no MQTT client, no Arduino, no driver knowledge.
//
// ArduinoJson is used here even in host builds -- it is a plain C++ library and does not
// require the Arduino framework. That keeps every payload testable without hardware, which is
// where the actual risk lives (a wrong value_template is invisible until Home Assistant
// quietly shows nothing).

#pragma once

#include <cstddef>
#include <string>

#include "device/bridge_info.h"
#include "device/device_state.h"
#include "diagnostics/diagnostics.h"

namespace heliograph::mqtt {

/// Hard ceiling per payload. The brief forbids unbounded JSON documents; rather than trust
/// that a document happens to stay small, every builder refuses to emit above this and says
/// so. Truncated JSON is worse than no JSON: a consumer cannot tell it apart from real data.
inline constexpr size_t kMaxPayloadBytes = 4096;

/// Builds the state payload.
///
/// Only `supported` measurements appear at all, and an unsupported/invalid/stale channel is
/// published as `null` rather than 0 -- Home Assistant renders null as "unknown", which is
/// the truth, while 0 would enter the energy statistics as a real reading.
///
/// Returns false if the payload would exceed `maxBytes`; `out` is then untouched.
bool buildStatePayload(const DeviceState& state, std::string& out,
                       size_t maxBytes = kMaxPayloadBytes);

bool buildDiagnosticsPayload(const DiagnosticsSnapshot& diagnostics, const BridgeInfo& bridge,
                             std::string& out, size_t maxBytes = kMaxPayloadBytes);

/// Identity. Fields that are unknown are omitted entirely rather than sent as "", so a
/// consumer can distinguish "not reported by this protocol" from "reported as blank".
bool buildIdentityPayload(const DeviceIdentity& identity, std::string& out,
                          size_t maxBytes = kMaxPayloadBytes);

bool buildCapabilitiesPayload(const InverterCapabilities& capabilities, std::string& out,
                              size_t maxBytes = kMaxPayloadBytes);

/// Stable snake_case name for a capability, used in the capabilities payload and the REST API.
const char* capabilityName(InverterCapability capability);

}  // namespace heliograph::mqtt
