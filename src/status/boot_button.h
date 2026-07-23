// SPDX-License-Identifier: MIT
//
// Hold-to-trigger detector for the BOOT button, so a long press factory-resets a bridge
// with no other recovery path (headless, no display, config may be wrong enough to lock
// you out of the web UI). Pure and clock-injected: the whole thing -- short press ignored,
// long press fires exactly once, release re-arms -- is host-tested with a fake millis, no
// GPIO involved. main.cpp only feeds it a debounced pin read and acts on Triggered.

#pragma once

#include <cstdint>

namespace heliograph::status {

class HoldDetector {
public:
    /// `holdMs` is how long the button must stay down before the action fires. Long on
    /// purpose (main uses 5 s): a factory reset must never be one accidental brush.
    explicit HoldDetector(uint64_t holdMs) : holdMs_(holdMs) {}

    enum class Event {
        Idle,       ///< not pressed, or already fired this press -- do nothing
        Holding,    ///< pressed, threshold not yet reached -- show the countdown
        Triggered,  ///< threshold just crossed -- act, once, until release
    };

    /// `pressed` is the debounced button state (true = held down). Returns Triggered on the
    /// single call where the hold threshold is first crossed, then Idle until the button is
    /// released and pressed again -- so the action never repeats while the button stays down.
    Event update(bool pressed, uint64_t nowMs);

private:
    uint64_t holdMs_;
    bool     pressing_     = false;
    bool     fired_        = false;
    uint64_t pressStartMs_ = 0;
};

}  // namespace heliograph::status
