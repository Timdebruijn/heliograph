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
// Built ahead of any driver that can accept a command (every shipping driver returns
// CommandResult::Unsupported), for the same reason CommandDispatcher itself was: the tested
// contract is worth having before the first thing that needs it, not written under pressure
// alongside it.

#pragma once

#include <mutex>
#include <optional>
#include <string>

#include "command_dispatcher.h"
#include "device/command.h"

namespace heliograph {

class CommandQueue {
public:
    struct Request {
        std::string     deviceId;
        InverterCommand command;
    };

    /// False, with no state change, if a request is already pending -- the same one-in-flight
    /// rule the manual-poll request already applies. A caller that gets false must surface it
    /// as "busy, try again", not queue a second request that could then execute out of order.
    bool submit(Request request);

    /// Consumed by the bus owner: takes the pending request if there is one, clearing the slot.
    std::optional<Request> take();

    /// Recorded by the bus owner once dispatch() has returned. Overwrites whatever the previous
    /// outcome was -- only the most recent request's outcome is kept, matching submit()'s
    /// one-slot rule.
    /// Scoped by device as well as request id. A caller may choose its own request id, so two
    /// devices can legitimately carry the same one; keyed on the id alone, polling device A
    /// could return device B's result. Dispatch was never affected -- that is scoped by
    /// deviceId in Request -- but the answer an operator reads was.
    void recordOutcome(const std::string& deviceId, const std::string& requestId,
                       DispatchOutcome outcome);

    /// The outcome of the most recently COMPLETED request with this id. Empty if that id was
    /// never submitted, is still pending, or has since been superseded by a later request's
    /// outcome.
    std::optional<DispatchOutcome> outcomeFor(const std::string& deviceId,
                                             const std::string& requestId) const;

private:
    mutable std::mutex             mutex_;
    std::optional<Request>         pending_;
    std::string                    lastOutcomeDeviceId_;
    std::string                    lastOutcomeRequestId_;
    std::optional<DispatchOutcome> lastOutcome_;
};

}  // namespace heliograph
