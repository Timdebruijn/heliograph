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
#include <optional>
#include <string>

class AsyncWebServerRequest;

#include "commands/command_dispatcher.h"
#include "config/configuration.h"
#include "device/bridge_info.h"
#include "device/command.h"
#include "device/device_state.h"
#include "diagnostics/diagnostics.h"
#include "app/capture_runner.h"
#include "app/discovery_runner.h"
#include "app/driver_capture_runner.h"
#include "drivers/driver_registry.h"
#include "rest_payloads.h"
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
    /// The Modbus TCP unit id a device is served at, or 0 when it is not served. Injected
    /// because only main knows the mapping from configuration slot to device, and because
    /// otherwise the only way to learn it is to count positions in the device list.
    std::function<uint8_t(const DeviceId&)> modbusUnitIdFor;

    /// Which CONFIGURED row this running device came from: 0 is the primary driver, 1..N are
    /// additional_devices[0..N-1]. -1 when the id matches no configured slot.
    ///
    /// Only setup() knows this. It plans the rows, starts them, and watches some of them be
    /// refused -- a driver that cannot share a bus, or a second row resolving to an id another
    /// row already took -- so the surviving devices are NOT the configured rows minus a suffix.
    /// The web UI guessed the mapping twice, by counting and then by driver id, and both guesses
    /// put a Remove button on the wrong inverter. This hands over the answer instead.
    std::function<int(const DeviceId&)> configSlotFor;
    /// Persist the configuration. Returns false if it could not be written.
    std::function<bool(const Configuration&)> saveConfig;
    /// Force an immediate poll. Returns false if the bus is busy.
    std::function<bool()> requestPoll;
    std::function<void()> requestReboot;
    /// Start discovery. Returns false when one is already running.
    std::function<bool(bool extended)> requestDiscovery;
    /// Current discovery report, for the wizard to poll.
    std::function<DiscoveryReport()> discoveryReport;
    /// Start a passive bus capture. Returns false when one is already running -- or when
    /// discovery is, since both take exclusive use of the same bus.
    std::function<bool(const diag::CaptureConfig&, const SerialProfile&)> requestCapture;
    /// The current capture report, for the wizard to poll and then download.
    std::function<CaptureReport()> captureReport;
    /// Start a capture of the driver's own conversation. No SerialProfile argument, and that is
    /// the point: there is a working driver, so the line is a fact about the bridge rather than
    /// a guess the operator supplies. Returns false when anything else is using the bus.
    std::function<bool(const diag::TapConfig&)> requestDriverCapture;
    /// The current driver-capture report. Carries frames only once it is done -- while it runs,
    /// the bus task is still appending to them.
    std::function<DriverCaptureReport()> driverCaptureReport;
    /// Wipes stored configuration including credentials, then reboots into the setup portal.
    /// The same wipe the BOOT-hold performs, reached over the network instead of the button --
    /// which is what a user without physical access has when a config locks them out.
    std::function<bool()> requestFactoryReset;
    /// Copies the stored configuration into the rollback slot, so the restore about to run can
    /// be undone. False when there was nothing stored yet, or the write was refused -- both
    /// are reported to the caller rather than aborting the restore over them.
    std::function<bool()> stashRollback;
    /// Swaps the rollback copy back in and hands over the configuration that is now live.
    /// False when there is no rollback copy, or it could not be read.
    std::function<bool(Configuration&)> rollbackConfig;
    /// Whether an undo is currently available, for the preview to say so up front.
    std::function<bool()> hasRollback;
    /// Erases a stored crash dump. Returns false when there was none, or the flash refused.
    /// Admin-gated and rate-limited like every other action -- it destroys diagnostic evidence,
    /// which is exactly the sort of thing an unauthenticated caller must not be able to do.
    std::function<bool()> clearCoredump;
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
    /// Queues a write command for `deviceId` to run on the bus. Returns the request id
    /// actually used (server-generated if the caller supplied none) on success, or nullopt
    /// when one is already pending -- the same one-in-flight rule manual poll, discovery and
    /// capture already apply.
    ///
    /// This can only ever say "accepted for the queue", never "succeeded": a real driver's
    /// execute() is a multi-second RS485 transaction, and running it from this handler would
    /// repeat the exact Phase-3 bus race requestPoll's comment documents finding live. See
    /// commandOutcome for how the caller learns what actually happened.
    std::function<std::optional<std::string>(const std::string& deviceId, InverterCommand command)>
        submitCommand;
    /// The outcome of the most recently completed command with this request id. Empty while
    /// it is still pending, was never submitted, or has since been superseded by a later
    /// request -- only one outcome is remembered at a time, matching submitCommand's
    /// one-in-flight rule.
    std::function<std::optional<DispatchOutcome>(const std::string& requestId)> commandOutcome;
};

class RestApi {
public:
    explicit RestApi(RestContext context, uint16_t port = 80);
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
    ///
    /// `maxBytes` is per route rather than global: a configuration restore is legitimately
    /// larger than any other body this API takes, and raising the bound for every endpoint to
    /// suit the one that needs it would loosen the heap guarantee everywhere else.
    bool collectBody(AsyncWebServerRequest* request, const uint8_t* data, size_t len, size_t index,
                     size_t total, std::string*& out, size_t maxBytes = kMaxRequestBytes);

    /// True when collectBody already answered this request (413 too large, 409 busy).
    ///
    /// The request handler MUST return without sending when this is true. The body handler runs
    /// first but the request handler runs anyway, and send() replaces any previously stored
    /// response -- so the unconditional "a JSON body is required" that every one of these
    /// handlers falls through to was overwriting the real reason. Upload a file one byte over
    /// the limit and the API said the body was missing.
    ///
    /// Same lesson the OTA route learned with its _tempObject marker, arrived at from the
    /// other direction: there it was the specific error being replaced by a generic one.
    bool bodyAlreadyAnswered(AsyncWebServerRequest* request);

    /// True when the handler must stop: either the body handler already answered, or no body
    /// arrived at all. `expected` names what was wanted, because "a JSON body" and "a backup
    /// file" are different things to whoever is reading the error.
    ///
    /// One function because the ORDER of those two checks is the whole point and it was
    /// written out three times. Ask "was this already answered?" first: collectBody may have
    /// stored a 413 or a 409, and send() replaces the stored response, so testing for a
    /// missing body first would overwrite the real reason with a generic one. Three copies of
    /// a rule that is only correct in one order is three chances to get it wrong, and a fourth
    /// route would have been a fourth.
    bool bodyMissing(AsyncWebServerRequest* request, const char* expected);
    void releaseBody();

    /// Merges the collected body into `updated`, or answers 400 and returns false.
    ///
    /// Releases the body either way, and that is the reason this is a function rather than a
    /// pattern: the release belongs BETWEEN the parse and the error, so each of the two copies
    /// was a chance to leak the buffer down an error path.
    ///
    /// The message itself is shaped by invalidConfig() in the .cpp, not here -- a third site
    /// reaches the same 400 from validate() on a restored backup rather than from a patch, so
    /// the wording has one owner and the parsing has another.
    bool applyBodyTo(AsyncWebServerRequest* request, Configuration& updated,
                     const DriverLookupFn& lookupDriver = {});

    /// Persists `updated` and publishes it to the running system, or answers 500 and
    /// returns false.
    ///
    /// The ORDER is the invariant: save first, publish second. Publishing a configuration that
    /// failed to persist leaves the device running something it will not have after a reboot.
    ///
    /// Deliberately NOT used by the rollback route. rollbackConfig() has already swapped the
    /// two stored blobs, so calling saveConfig() there would re-serialise the same
    /// configuration over the copy just put in place -- it applies without saving, and says so.
    bool saveAndApply(AsyncWebServerRequest* request, const Configuration& updated);

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
    /// The request collectBody has already answered with an error. Compared by pointer and
    /// never dereferenced; cleared as soon as it is read, and whenever a new body starts, so a
    /// recycled request address cannot inherit it.
    const void*                  bodyRejected_ = nullptr;
};

/// Minimum spacing between /actions/* calls, per the security model.
inline constexpr uint32_t kActionRateLimitMs = 1000;
/// Maximum SSE clients. Bounded to protect the heap.
inline constexpr size_t kMaxSseClients = 4;
inline constexpr uint32_t kSseMinIntervalMs = 1000;

}  // namespace heliograph::rest
