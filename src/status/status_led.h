// SPDX-License-Identifier: MIT
//
// Status LED colour policy: one addressable RGB LED (WS2812) condensed into the same
// health story the web UI header tells with its green/amber/red dots -- but for a single
// light, so the rules are about NOT crying wolf as much as about showing faults.
//
// Deliberately pure and hardware-free: the decision is a function of booleans, host-tested
// exhaustively in test_status_led, and main.cpp only turns the result into a neopixel write.
// The board layer decides whether there is an LED at all (board::kHasStatusLed).

#pragma once

namespace heliograph::status {

enum class LedColor { Off, Green, Amber, Red, Blue };

/// A colour plus whether it should blink. Blink is reserved for states that want to grab
/// attention while something is actively happening (a factory-reset hold counting down),
/// as opposed to a steady health colour.
struct LedIndication {
    LedColor color = LedColor::Off;
    bool     blink = false;

    bool operator==(const LedIndication& o) const { return color == o.color && blink == o.blink; }
    bool operator!=(const LedIndication& o) const { return !(*this == o); }
};

/// Everything the policy needs, gathered by main from wifi, the device state and the config.
/// Optional outputs (MQTT, Modbus TCP) carry BOTH an "enabled" and a live flag on purpose:
/// an output the owner never turned on must not colour the LED, or a bridge that only ever
/// serves REST would sit permanently amber and the light would mean nothing.
struct LedInputs {
    bool provisioned         = false;  ///< false → still in the setup portal
    bool factoryResetHolding = false;  ///< BOOT held, reset counting down (wins over health)
    bool wifiConnected       = false;
    /// Whether a driver is configured at all. A relay-only board (a 6CH used purely as a DRM
    /// actuator, no inverter) has no data to miss, so the inverter rules below are skipped
    /// and its health is just WiFi + outputs -- otherwise it would sit permanently red.
    bool inverterExpected    = false;
    bool inverterOnline      = false;  ///< the core job: is real data coming in
    bool dataValid           = false;
    bool dataStale           = false;
    bool mqttEnabled         = false;
    bool mqttConnected       = false;
    bool modbusEnabled       = false;  ///< Modbus TCP server configured on
    bool modbusListening     = false;
};

/// Priority-ordered, first match wins. Red is reserved for the core function being broken
/// (no inverter data); an enabled-but-failing output or stale data degrades only to amber,
/// so the light distinguishes "not doing its job" from "doing its job, one detail off".
LedIndication decide(const LedInputs& in);

/// Stable lower-case name, for the REST payload so the LED state is observable without
/// looking at the board.
const char* colorName(LedColor color);

}  // namespace heliograph::status
