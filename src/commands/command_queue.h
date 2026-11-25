// SPDX-License-Identifier: MIT
//
// One command in flight at a time, requested from any task and drained by the bus owner.
//
// Same request/consume shape as g_manualPollRequested and the discovery/capture requests in
// main.cpp: a caller can only ever ASK, never run a command itself, because only rs485Task may
// touch the bus. This generalises that shape from a bare flag to an actual command, because a
// write needs to say WHAT and for WHICH device, and the caller needs to learn what became of the
// specific request it made -- hence requestId rather than a fire-and-forget bool.
//
// Built ahead of any driver that could accept a command, for the same reason
// CommandDispatcher itself was: the tested
// contract is worth having before the first thing that needs it, not written under pressure
// alongside it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "command_dispatcher.h"
#include "device/command.h"

namespace heliograph {

/// How long a request may wait for the bus before it is refused instead of run. A setpoint is
/// sent for NOW -- "limit to 0 % while the price is negative" -- and rs485Task can be busy for
/// minutes (a discovery scan, a capture, a run of timeouts against a silent device). Executed
/// that late it would act on a moment that has passed, with nothing telling the sender it did.
inline constexpr uint64_t kMaxCommandAgeMs = 30000;

/// How many completed outcomes are kept for callers to read back. Enough that a reader polling
/// slowly -- MQTT while its broker is down, a script once a minute -- still finds its own after
/// several other clients' commands; at roughly 150 bytes a slot.
inline constexpr std::size_t kCommandOutcomeSlots = 8;

class CommandQueue {
public:
    struct Request {
        std::string     deviceId;
        InverterCommand command;
        /// When it was submitted, on the same monotonic clock take() is given.
        uint64_t submittedAtMs = 0;
    };

    /// False, with no state change, if a request is already pending -- the same one-in-flight
    /// rule the manual-poll request already applies. A caller that gets false must surface it
    /// as "busy, try again", not queue a second request that could then execute out of order.
    ///
    /// On success, any kept outcome under the same device and request id is forgotten. Callers
    /// choose their own ids and some reuse one ("setpoint", every time); without this the new
    /// request read back as the PREVIOUS one's outcome the moment it was queued, and MQTT
    /// published that stale result as the new command's.
    bool submit(Request request);

    /// Consumed by the bus owner: takes the pending request if there is one, clearing the slot.
    /// A request older than kMaxCommandAgeMs is not handed out: its outcome is recorded as
    /// Rejected ("expired") and nothing is returned, so the sender learns it was not run. When
    /// `expired` is given, the refused request is moved there, for the caller to log -- this
    /// class has no logger, and a refusal nobody can see in the log is a refusal nobody finds.
    std::optional<Request> take(uint64_t nowMs, std::optional<Request>* expired = nullptr);

    /// Recorded by the bus owner once dispatch() has returned. The last kCommandOutcomeSlots
    /// outcomes are kept, newest first. One slot used to be all there was, and take() frees the
    /// pending slot BEFORE dispatch runs, so a second client could submit, complete and
    /// overwrite the first client's outcome before the first one ever read it -- leaving it
    /// polling "pending" forever (MQTT would never publish its result).
    /// Scoped by device as well as request id. A caller may choose its own request id, so two
    /// devices can legitimately carry the same one; keyed on the id alone, polling device A
    /// could return device B's result. Dispatch was never affected -- that is scoped by
    /// deviceId in Request -- but the answer an operator reads was.
    void recordOutcome(const std::string& deviceId, const std::string& requestId,
                       DispatchOutcome outcome);

    /// The outcome of the most recently COMPLETED request with this id. Empty if that id was
    /// never submitted, is still pending, or has since been pushed out by kCommandOutcomeSlots
    /// later outcomes.
    std::optional<DispatchOutcome> outcomeFor(const std::string& deviceId,
                                             const std::string& requestId) const;

private:
    struct Outcome {
        std::string     deviceId;
        std::string     requestId;
        DispatchOutcome outcome;
    };
    void recordLocked(const std::string& deviceId, const std::string& requestId,
                      DispatchOutcome outcome);

    mutable std::mutex                                 mutex_;
    std::optional<Request>                             pending_;
    std::array<std::optional<Outcome>, kCommandOutcomeSlots> outcomes_{};
    std::size_t                                        next_ = 0;  // slot the next outcome takes
};

}  // namespace heliograph
