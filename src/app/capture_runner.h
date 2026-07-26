// SPDX-License-Identifier: MIT
//
// Runs a passive bus capture on the task that owns the RS485 bus.
//
// Exactly the DiscoveryRunner pattern, and for exactly the same reason: the web handler cannot
// listen to the bus itself. It would block an AsyncTCP callback for the whole capture window,
// and two components on the bus at once is what the bus lock exists to prevent. So the handler
// *requests*, and rs485Task picks it up.
//
// That handover is also the answer to "does this interfere with the driver's polling?" -- not a
// flag someone has to remember to check, but structure: rs485Task runs the capture INSTEAD of
// its poll cycle, one or the other per iteration, the same way discovery already works. Polling
// cannot overlap a capture because there is no iteration in which both happen.
//
// The one thing this does that discovery does not: it reconfigures the line to settings the
// operator chose, which for an unidentified device is usually not the driver's. The caller must
// put the line back afterwards -- see the restoreDriverLine() call in main.

#pragma once

#include <functional>
#include <mutex>
#include <string>

#include "device/clock.h"
#include "diagnostics/frame_capture.h"
#include "transport/transport.h"

namespace heliograph {

enum class CaptureStatus : uint8_t {
    Idle,
    /// Requested, not yet picked up by rs485Task.
    Requested,
    Running,
    Done,
    /// Could not run at all (the bus lock was not free).
    Failed,
};

const char* captureStatusName(CaptureStatus status);

struct CaptureReport {
    CaptureStatus status = CaptureStatus::Idle;
    diag::CaptureConfig        config;
    SerialProfile              profile;
    std::vector<diag::CapturedFrame> frames;
    uint32_t totalBytes    = 0;
    uint32_t modbusFrames  = 0;
    uint32_t pmuFrames     = 0;
    bool     truncated     = false;
    /// True once the line has been reconfigured to `profile`, so the caller knows it has to put
    /// the driver's own settings back.
    ///
    /// Not the same as "the capture ran". A run that could not take the bus never touched the
    /// line, and restoring it anyway means calling begin() on every driver -- which for the
    /// AA55 family is a registration handshake, i.e. real traffic on a bus that just reported
    /// itself busy. Doing that as a reflex after a failure is how a capture that recorded
    /// nothing still disturbs a working install.
    bool     lineReconfigured = false;
    /// The idle gap actually used, in ms. Reported because it is derived from the baud rate and
    /// floored, so it is not something the operator can work out from what they asked for.
    uint32_t idleGapMs     = 0;
    uint64_t requestedMs   = 0;
    uint64_t startedMs     = 0;
    uint64_t finishedMs    = 0;
    std::string error;  ///< only set when status == Failed
};

class CaptureRunner {
public:
    explicit CaptureRunner(ClockFn clock);

    /// Called from the web task. False when a capture is already pending or running -- the REST
    /// layer turns that into 409. A second capture of the same bus while the first is listening
    /// would have to share the line settings anyway.
    bool request(const diag::CaptureConfig& config, const SerialProfile& profile);

    /// Called from rs485Task. Runs a pending request and returns true if it ran.
    ///
    /// `onTick` is called on every read iteration, on the caller's task. A capture window is
    /// tens of seconds inside a single rs485Task iteration, so the task watchdog has to be fed
    /// from in here -- the same arrangement discovery needs for its probe loop, and for the
    /// same reason.
    bool runIfRequested(Transport& transport, const std::function<void()>& onTick = {});

    CaptureReport report() const;
    bool          busy() const;

private:
    ClockFn            clock_;
    mutable std::mutex mutex_;
    CaptureReport      report_;
};

/// How long a single read may block inside the capture loop.
///
/// Short on purpose. It bounds how precisely a gap can be seen, it bounds how often the
/// watchdog is fed, and it bounds how long a stop takes to notice. 5 ms is well inside the
/// t3.5 gap at 9600 and 19200 -- the rates at which framing is honest at all -- and gives the
/// watchdog two hundred feeds a second.
inline constexpr uint32_t kCaptureReadTimeoutMs = 5;

/// Ceiling on a requested window. A capture holds the bus lock for its whole duration, so this
/// is also the longest anyone can stop the inverter being polled with one authenticated
/// request.
inline constexpr uint32_t kMaxCaptureSeconds = 300;

}  // namespace heliograph
