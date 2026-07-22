// SPDX-License-Identifier: MIT
//
// REST API + web UI, backed by ESP32Async/ESPAsyncWebServer (LGPL-3.0).
//
// Handlers only read a shared_ptr<const DeviceState> snapshot, so a slow or misbehaving HTTP
// client can never delay the RS485 poll. That is an acceptance criterion, and it is why the
// snapshot model exists at all.
//
// The response bodies themselves live in rest_payloads.* and are host-tested; this file is
// routing, auth and rate limiting.
//
// VERIFIED ON HARDWARE 2026-07-17: serves the API and the web UI, HTTP Basic refuses
// unauthenticated mutations (401), and an unknown device id returns a real 404.
//
// Two bugs that only running found, both in the seam between tested pieces:
//   - a bare string URI matches "^uri(/.*)?$", not exactly, so /api/v1/devices swallowed
//     /api/v1/devices/<id>/capabilities. Hence AsyncURIMatcher::exact below.
//   - calling ESP.restart() straight after request->send() drops the response: the send is
//     queued, not flushed. Reboot is deferred to loop() by main.cpp.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class AsyncWebServerRequest;

#include "config/configuration.h"
#include "device/bridge_info.h"
#include "device/command.h"
#include "device/device_state.h"
#include "diagnostics/diagnostics.h"
#include "app/discovery_runner.h"
#include "drivers/driver_registry.h"
#include "state/state_store.h"

namespace heliograph::rest {

/// What the API needs from the rest of the firmware, injected rather than reached for, so
/// the wiring stays visible in one place.
struct RestContext {
    DeviceManager*        devices    = nullptr;
    Diagnostics*          diagnostics = nullptr;
    const DriverRegistry* registry   = nullptr;
    Configuration*        config     = nullptr;
    /// Publishes a new configuration to the shared global. Handlers must use this instead
    /// of assigning through `config` directly: the assignment replaces std::string members
    /// while loop()/rs485Task may be mid-read of them (bridgeInfo, startOutputs), and only
    /// the owner of the global knows which lock guards it. Reads from REST handlers stay
    /// direct -- they run on the same AsyncTCP task as the writes and are serialized.
    std::function<void(const Configuration&)> applyConfig;
    std::function<BridgeInfo()>       bridgeInfo;
    std::function<uint64_t()>         clock;
    /// Persist the configuration. Returns false if it could not be written.
    std::function<bool(const Configuration&)> saveConfig;
    /// Force an immediate poll. Returns false if the bus is busy.
    std::function<bool()> requestPoll;
    std::function<void()> requestReboot;
    /// Start discovery. Returns false when one is already running.
    std::function<bool(bool extended)> requestDiscovery;
    /// Current discovery report, for the wizard to poll.
    std::function<DiscoveryReport()> discoveryReport;
    /// Wipes stored configuration including credentials, then reboots into the setup portal.
    /// With no reset button on this board, this is a user's main way back from a bad config.
    std::function<bool()> requestFactoryReset;
    /// True while the setup portal is up: the API then serves the setup page and /provision.
    std::function<bool()> portalActive;
    /// Scans for networks; returns a JSON body. Portal only.
    std::function<std::string()> scanNetworks;
    /// Sets a bridge relay (DRM contact). Wired by main to the RelayController behind the
    /// same mutex the MQTT path uses. Unset (nullptr) on boards without relays -- the
    /// endpoint then answers 404, matching the absent-not-zero rule for hardware.
    std::function<CommandResult(uint8_t index, bool energised)> setRelay;
    /// Applies a named DRM mode (see src/relays/drm.h): the role's relays energised,
    /// everything else released, atomically behind the relay mutex. Returns the gate
    /// verdict; OutOfRange doubles as "not a valid mode for the configured roles".
    std::function<CommandResult(const std::string& mode)> setDrmMode;
};

class RestApi {
public:
    RestApi(RestContext context, uint16_t port = 80);
    ~RestApi();

    RestApi(const RestApi&)            = delete;
    RestApi& operator=(const RestApi&) = delete;

    bool begin();
    void stop();

    /// Pushes a state update to any Server-Sent Events subscribers. Rate limited internally.
    /// SSE is an optimisation: the UI falls back to polling /status if it is unavailable.
    void notifyState(const DeviceState& state, uint64_t nowMs);

private:
    /// Accumulates a chunked body into bodyBuffer_. See the note on bodyBuffer_.
    bool collectBody(AsyncWebServerRequest* request, const uint8_t* data, size_t len, size_t index,
                     size_t total, std::string*& out);
    void releaseBody();

    RestContext context_;
    uint16_t    port_;
    bool        started_        = false;
    uint64_t    lastSseMs_      = 0;
    uint64_t    lastActionMs_   = 0;

    // Request body accumulation.
    //
    // Deliberately NOT AsyncWebServerRequest::_tempObject: the library frees it with a raw
    // free() (WebRequest.cpp:114), so putting a `new std::string` there skips the destructor
    // and leaks the string's heap buffer on every request. One buffer, one owner, bounded by
    // kMaxRequestBytes; a second concurrent body-carrying request gets 409 rather than
    // corrupting this one.
    std::string                  bodyBuffer_;
    const void*                  bodyOwner_ = nullptr;
};

/// Minimum spacing between /actions/* calls, per the security model.
inline constexpr uint32_t kActionRateLimitMs = 1000;
/// Maximum SSE clients. Bounded to protect the heap.
inline constexpr size_t kMaxSseClients = 4;
inline constexpr uint32_t kSseMinIntervalMs = 1000;

}  // namespace heliograph::rest
