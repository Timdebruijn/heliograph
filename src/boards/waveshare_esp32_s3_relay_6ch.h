// SPDX-License-Identifier: MIT
//
// Waveshare ESP32-S3-Relay-6CH board definition. STATUS: pins verified from the official
// schematic AND the official demo source, and CONFIRMED ON HARDWARE 2026-07-23 -- all six
// relays actuate, and the active-high polarity below was measured, not assumed.
//
// Sources (2026-07-22): official schematic (ESP32-S3-Relay-6CH-Sch.pdf), official demo
// (ESP32-S3-Relay-6CH-Demo.zip, WS_GPIO.h/WS_Serial.cpp), community ESPHome configuration
// (devices.esphome.io/devices/waveshare-6ch-relay) -- all three agree.
//
//   Wiki: https://www.waveshare.com/wiki/ESP32-S3-Relay-6CH
//
// The six relays are the multi-mode DRM actuators (DRM0-8 combinations) for inverters
// that cannot be curtailed over RS485. Failsafe by wiring: de-energised = DRM not
// asserted -- a dead bridge never blocks production.
//
// The failsafe is MEASURED, not merely designed (2026-07-23): with a relay energised,
// cutting power released the contact immediately, and the board came back up with all six
// de-energised. That second half matters as much as the first -- begin() drives every
// relay off and no relay state is persisted anywhere on purpose, so a reboot can never
// restore a curtailment nobody just asked for.

#pragma once

#include <cstdint>

namespace heliograph::board {

inline constexpr const char* kName = "Waveshare ESP32-S3-Relay-6CH";

// --- RS485 ---------------------------------------------------------------------------------
// WS_GPIO.h: TXD1 17, RXD1 18. Transceiver is an SP485E behind an isolator.
inline constexpr int kRs485Tx = 17;
inline constexpr int kRs485Rx = 18;

// No direction GPIO on this board -- VERIFIED, not unknown: the official demo transmits
// and receives with a plain begin(9600, SERIAL_8N1, RXD1, TXD1), no setPins/RS485 mode,
// so the schematic's TXDEN' net is driven by the board's own auto-direction circuit.
// -1 makes the transport skip RTS configuration. (GPIO21, the other boards' EN pin, is
// this board's buzzer.)
inline constexpr int kRs485De = -1;

/// UART peripheral used for RS485. UART0 is the USB-CDC console.
inline constexpr int kRs485UartNum = 1;

// --- Relays --------------------------------------------------------------------------------
// Schematic nets CH1..CH6 -> GPIO1, GPIO2, GPIO41, GPIO42, GPIO45, GPIO46 (45/46 are
// strapping pins; they only matter at reset, and the relays are driven afterwards).
// Order CONFIRMED ON HARDWARE 2026-07-23: relay index 0..5 drives CH1..CH6 one-to-one.
// Worth checking rather than assuming -- a shifted mapping would silently switch the
// wrong DRM line on an inverter.
// Active-high VERIFIED ON HARDWARE 2026-07-23, multimeter on the changeover contacts:
// driving the GPIO high energises the coil, closing COM-NO and opening COM-NC. So a
// de-energised relay leaves NO open and NC closed -- which is what makes the failsafe a
// wiring choice rather than a firmware one (docs/drm.md): whichever sense an inverter's
// DRM input needs, one of the two contacts keeps "bridge dead = inverter runs" true.
inline constexpr int  kRelayCount      = 6;
inline constexpr int  kRelayPins[6]    = {1, 2, 41, 42, 45, 46};
inline constexpr bool kRelayActiveHigh = true;

// --- RTC -----------------------------------------------------------------------------------
// None onboard: GPIO38 is the RGB status LED here (RTC is only available via a Pico HAT
// expansion, out of scope).
inline constexpr bool    kHasRtc        = false;
inline constexpr int     kRtcScl        = -1;
inline constexpr int     kRtcSda        = -1;
inline constexpr uint8_t kRtcI2cAddress = 0;

// --- BOOT button / status LED / buzzer -----------------------------------------------------
// BOOT on GPIO0 (Waveshare documentation, confirmed by Tim 2026-07-23) -- the SoC download
// strapping pin, reads high through its pull-up and low when pressed. Held ~5 s it factory-
// resets, the one recovery path on a headless board with no reset button exposed to the user.
inline constexpr bool kHasBootButton = true;
inline constexpr int  kBootPin       = 0;
// WS2812 status LED on GPIO38 and a transistor-driven buzzer on GPIO21 (documentation +
// official demo). On the RTC boards these very pins are the RTC I2C (38/39) and the RS485
// direction line (21) -- which is exactly why this is declared per board, never family-wide.
inline constexpr bool kHasStatusLed = true;
inline constexpr int  kStatusLedPin = 38;
inline constexpr bool kHasBuzzer    = true;
inline constexpr int  kBuzzerPin    = 21;

// --- Notes that are not pins ---------------------------------------------------------------
// Flash  : 8 MB -> uses partitions_8mb_ota.csv, NOT the 16 MB table
// Power  : USB-C, or 7-36 V DC terminal
// Relays : 6x <=10 A 250 VAC / 30 VDC, optocoupler + digital isolation
// I2C    : GPIO4 (SDA) / GPIO5 (SCL) are the Pico-HAT expansion I2C pins; the official
//          demo drives an EXTERNAL DS3231 module there. No onboard RTC (schematic clean).

}  // namespace heliograph::board
