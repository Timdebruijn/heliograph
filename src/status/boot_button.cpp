// SPDX-License-Identifier: MIT

#include "boot_button.h"

namespace heliograph::status {

HoldDetector::Event HoldDetector::update(bool pressed, uint64_t nowMs) {
    if (!pressed) {
        // Released: re-arm for the next press.
        pressing_ = false;
        fired_    = false;
        return Event::Idle;
    }
    if (!pressing_) {
        // Rising edge: start timing this press.
        pressing_     = true;
        pressStartMs_ = nowMs;
        return Event::Holding;
    }
    if (fired_) {
        // Already acted on this hold; do nothing more until release.
        return Event::Idle;
    }
    // Held. nowMs - pressStartMs_ rather than a countdown, so a millis() that runs backwards
    // (it does not, but be defensive) cannot make the threshold unreachable.
    if (nowMs - pressStartMs_ >= holdMs_) {
        fired_ = true;
        return Event::Triggered;
    }
    return Event::Holding;
}

}  // namespace heliograph::status
