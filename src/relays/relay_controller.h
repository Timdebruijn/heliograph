// SPDX-License-Identifier: MIT
//
// Bridge-local relay actuator: the DRM curtailment contacts on relay boards.
//
// This is deliberately NOT part of the inverter command path. The relays belong to the
// BRIDGE -- potential-free contacts wired to an inverter's DRM port -- and no driver has,
// or will ever get, a path to them. The controller is host-testable: the actual pin write
// is an injected callback, so the gate order and the failsafe are proven on the host
// before any coil ever clicks.
//
// Failsafe, decided with the owner (2026-07-22): de-energised = DRM not asserted = the
// inverter runs. Boot turns everything off, a crash leaves the pins to the reset default
// (inputs, relays released), and there is no state persistence on purpose -- a reboot
// must never re-assert curtailment the operator did not just ask for.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "rate_limiter.h"  // RateLimitPolicy, RateLimiter

#include "device/clock.h"
#include "device/command.h"  // CommandResult vocabulary

namespace heliograph {

/// Writes one relay's coil state to hardware. `energised` is the logical state; polarity
/// (active-high vs low) is the board layer's business, not the controller's.
using RelayApplyFn = std::function<void(uint8_t index, bool energised)>;

class RelayController {
public:
    static constexpr uint8_t kMaxRelays = 8;

    explicit RelayController(ClockFn clock, RateLimitPolicy rateLimit = {});

    /// Declares the relay count and the hardware write hook, and drives every relay to the
    /// failsafe state (de-energised). Count is clamped to kMaxRelays.
    void begin(uint8_t count, RelayApplyFn apply);

    /// The same global kill switch the command dispatcher honours. On by default.
    void setReadOnlyMode(bool readOnly) { readOnly_ = readOnly; }
    /// The relays.enabled config flag. Off by default: a relay board with default settings
    /// must be inert.
    void setEnabled(bool enabled) { enabled_ = enabled; }

    /// Gate order mirrors CommandDispatcher: read-only mode, feature enabled, valid index,
    /// rate limit -- only then does the apply hook run. Turning a relay OFF passes the
    /// rate limiter unconditionally: releasing curtailment must never be throttled.
    CommandResult set(uint8_t index, bool energised);

    /// Applies a full relay pattern (a DRM mode) as ONE command: same gates as set(), but a
    /// single rate-limit token for the whole pattern, however many relays it spans. Charging
    /// per relay made any pattern wider than the burst impossible to assert -- the tail ONs
    /// were refused by the throttle that exists to stop relay chatter, not an atomic mode
    /// switch. Releases (OFF) are written before asserts (ON), and an all-off pattern is
    /// never throttled, matching set()'s safe direction. `pattern` must be exactly count()
    /// entries.
    CommandResult applyPattern(const std::vector<bool>& pattern);

    /// Failsafe: everything de-energised, immediately, regardless of gates. For shutdown
    /// paths and for disabling the feature at runtime.
    void allOff();

    uint8_t count() const { return count_; }
    bool    energised(uint8_t index) const { return index < count_ && state_[index]; }
    bool    enabled() const { return enabled_; }
    bool    readOnlyMode() const { return readOnly_; }

private:
    /// The gates every relay operation passes, whatever it is asking for: the global kill
    /// switch, then the feature flag. Ok means "keep going", anything else is the refusal to
    /// return. Shared so a gate added later cannot land in one entry point and not the other.
    CommandResult checkGates() const;

    /// One token per accepted assert. Its own instance, so relay traffic and inverter write
    /// traffic can never consume each other's allowance -- see rate_limiter.h for why the
    /// arithmetic is shared but the state is not.
    bool allowedToAssert(uint64_t nowMs) { return limiter_.allow(nowMs); }

    ClockFn      clock_;
    RelayApplyFn apply_;
    RateLimiter  limiter_;
    uint8_t      count_    = 0;
    bool         readOnly_ = true;
    bool         enabled_  = false;
    bool         state_[kMaxRelays] = {};
};

}  // namespace heliograph
