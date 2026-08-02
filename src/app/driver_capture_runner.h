// SPDX-License-Identifier: MIT
//
// Runs a capture that does NOT stop the bridge polling.
//
// The opposite mechanics to CaptureRunner, from the same starting point. That one takes an
// iteration of rs485Task away from polling and spends the whole window inside it, because for an
// unidentified device the bridge's own traffic would be noise in the recording. Here the
// bridge's own traffic IS the recording, so taking the bus away would record nothing -- which is
// exactly what happened on hardware and is what issue #62 is about.
//
// So this runner never occupies an iteration. It ARMS: hangs a BusTap on the transport and
// returns immediately. Polling continues, the tap fills from inside the driver's own
// transactions, and a later visit unhooks it when the window has closed. Two lines in
// rs485Task, no `continue`, no bus lock, and no line reconfiguration -- so the whole
// lineReconfigured / restoreDriverLine() failure class that the passive mode has to handle does
// not exist here.
//
// WHICH TASK DOES WHAT, because the split is the only thing keeping this safe without a lock on
// the byte path:
//
//   - request() runs on the web task. It only sets a pending state.
//   - service() runs on rs485Task. It is the ONLY thing that touches the tap or the transport's
//     tap pointer, which is what the transport's lifetime rule requires.
//   - report() runs on the web task, under the mutex, and returns frames only once the capture
//     is Done. A page polling for progress gets status and elapsed time while it runs; handing
//     out a half-written frame list would mean copying a vector the bus task is appending to.

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "device/clock.h"
#include "diagnostics/bus_tap.h"
#include "transport/transport.h"

namespace heliograph {

enum class DriverCaptureStatus : uint8_t {
    Idle,
    /// Requested, not yet picked up by rs485Task.
    Requested,
    /// Armed: the tap is on the transport and the bridge is polling normally.
    Running,
    Done,
};

const char* driverCaptureStatusName(DriverCaptureStatus status);

/// What one visit from the bus task actually did.
enum class DriverCaptureService : uint8_t {
    /// Nothing pending, nothing running.
    Idle,
    /// A pending request was armed on this visit. The tap is now on the transport.
    Armed,
    /// Armed on an earlier visit and still inside its window.
    Recording,
    /// The window closed on this visit: the tap is off and the report is complete.
    Collected,
};

struct DriverCaptureReport {
    DriverCaptureStatus            status = DriverCaptureStatus::Idle;
    diag::TapConfig                config;
    /// The driver's own line, echoed back. Not a guess the operator supplied -- there is a
    /// working driver, so the line is known -- but it still belongs in the report: the idle gap
    /// derives from it, and a reader a day later cannot check the framing without it.
    SerialProfile                  profile;
    std::vector<diag::TappedFrame> frames;
    uint32_t totalBytes   = 0;
    uint32_t txFrames     = 0;
    uint32_t rxFrames     = 0;
    uint32_t modbusFrames = 0;
    uint32_t pmuFrames    = 0;
    bool     truncated    = false;
    uint32_t idleGapMs    = 0;
    uint64_t requestedMs  = 0;
    uint64_t startedMs    = 0;
    uint64_t finishedMs   = 0;
};

class DriverCaptureRunner {
public:
    explicit DriverCaptureRunner(ClockFn clock);

    /// Called from the web task. False when one is already pending or running; the REST layer
    /// turns that into 409.
    bool request(const diag::TapConfig& config, const SerialProfile& profile);

    /// Called from rs485Task, once per iteration, before anything else it does.
    ///
    /// Arms a pending request and unhooks a finished one. Returns immediately in every case --
    /// this must NEVER cause the caller to skip its poll, because the poll is what it is
    /// recording.
    ///
    /// The return says which transition happened rather than merely whether something is
    /// running, so the caller can log the two interesting ones exactly once each. A bool would
    /// leave "armed" and "still recording" indistinguishable, and the caller would have to
    /// reconstruct the difference from the report -- which is how a once-per-capture log line
    /// becomes a once-per-iteration one.
    DriverCaptureService service(Transport& transport);

    // No abandon()/cancel(). Nothing that would take the bus for itself can start while this is
    // armed -- discovery and the passive capture are both refused at request time -- so an
    // unhook-early path would have no caller, and an unused escape hatch is worse than none:
    // it invites a future caller to reach for it instead of asking why the guard exists.

    DriverCaptureReport report() const;
    bool                busy() const;

private:
    void collect(uint64_t nowMs);

    ClockFn                        clock_;
    mutable std::mutex             mutex_;
    DriverCaptureReport            report_;
    /// Touched only by service()/abandon(), i.e. only by the bus task. Deliberately outside the
    /// mutex: putting the tap behind the same lock the web task takes would drag that lock into
    /// the per-byte path for no benefit, since no other task may look at it.
    std::unique_ptr<diag::BusTap>  tap_;
};

/// Ceiling on a requested window.
///
/// The same number as the passive capture, for a different reason. There it bounds how long one
/// authenticated request can stop the inverter being polled; nothing is stopped here, so this
/// bounds how long a recorder sits in the RS485 path and how much of the report can accumulate.
inline constexpr uint32_t kMaxDriverCaptureSeconds = 300;

}  // namespace heliograph
