// SPDX-License-Identifier: MIT
//
// REST response bodies. Pure: no web server, no Arduino.

#pragma once

#include <string>
#include <vector>

#include "device/bridge_info.h"
#include "device/device_state.h"
#include "diagnostics/diagnostics.h"
#include "app/discovery_runner.h"
#include "drivers/driver_descriptor.h"

namespace heliograph::rest {

inline constexpr size_t kMaxResponseBytes = 8192;
/// Bodies above this are refused with 413 before parsing, so a large POST cannot exhaust the
/// heap just by arriving.
inline constexpr size_t kMaxRequestBytes = 4096;

struct ApiError {
    int         httpStatus;
    std::string code;
    std::string message;
};

/// Uniform error body. Every failure looks the same, and a failure is never a 200.
bool buildErrorPayload(const ApiError& error, const std::string& requestId, std::string& out);

/// `deviceId` is the id the device is REGISTERED under -- the key in /api/v1/devices and the
/// per-device routes. Not identity.deviceId(): that one changes when a late-arriving serial
/// number completes the identity (the store key was minted at begin(), before registration on
/// the bus), and reporting it sent clients to a path that 404s. Seen live 2026-07-19.
bool buildStatusPayload(const DeviceState& state, const std::string& deviceId,
                        const BridgeInfo& bridge, const DiagnosticsSnapshot& diagnostics,
                        const DriverDescriptor* driver, uint64_t nowMs, std::string& out,
                        size_t maxBytes = kMaxResponseBytes);

bool buildDevicesPayload(const std::vector<std::string>& deviceIds, std::string& out,
                         size_t maxBytes = kMaxResponseBytes);

bool buildDevicePayload(const DeviceState& state, const std::string& deviceId,
                        const DriverDescriptor* driver, std::string& out,
                        size_t maxBytes = kMaxResponseBytes);

bool buildMeasurementsPayload(const DeviceState& state, std::string& out,
                              size_t maxBytes = kMaxResponseBytes);

bool buildCapabilitiesPayload(const InverterCapabilities& capabilities, std::string& out,
                              size_t maxBytes = kMaxResponseBytes);

bool buildDiagnosticsPayload(const DiagnosticsSnapshot& diagnostics, const BridgeInfo& bridge,
                             std::string& out, size_t maxBytes = kMaxResponseBytes);

/// The discovery report, with every piece of evidence §28 asks the wizard to show: which
/// driver and serial profile were tried, whether anything answered, whether the checksum held,
/// the confidence score, and the reason a match was or was not accepted automatically.
bool buildDiscoveryPayload(const DiscoveryReport& report, uint64_t nowMs, std::string& out,
                           size_t maxBytes = kMaxResponseBytes);

/// Drivers compiled into this build. The discovery wizard needs this rather than a hardcoded
/// list in the frontend.
bool buildDriversPayload(const std::vector<DriverDescriptor>& drivers, std::string& out,
                         size_t maxBytes = kMaxResponseBytes);

/// Recent log lines, oldest first. Bounded separately: a hex-dump log is bulkier than any
/// other response, and truncating it silently would defeat the point of reading it.
///
/// Sized from the worst case, not a round number: 64 ring lines x 255 chars each plus JSON
/// quoting/array overhead is ~17 KB. The first pick (16384) was exceeded exactly when the
/// ring was full of maximum-length TRACE lines -- a 500 at the moment the tool matters most.
inline constexpr size_t kMaxLogResponseBytes = 24576;
/// `level` is the active log level name, echoed in the payload. It answers the question a
/// reader otherwise cannot: "why is there no TRACE data in here?" -- because the level says
/// info, not because the bus is silent. Added for driver bring-up sessions, where the raw
/// dumps are TRACE-only and the default level would silently hide them.
bool buildLogsPayload(const std::vector<std::string>& lines, uint32_t totalLines,
                      const std::string& level, std::string& out,
                      size_t maxBytes = kMaxLogResponseBytes);

}  // namespace heliograph::rest
