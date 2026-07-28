// SPDX-License-Identifier: MIT
//
// Waveshare ESP32-S3-Relay-1CH board definition.
//
// Every pin here was read out of the official demo source and cross-checked against the
// official schematic (2026-07 research round; restored from git history when multi-board
// support landed). None of it is guessed.
//
//   Demo:      https://files.waveshare.com/wiki/ESP32-S3-Relay-1CH/ESP32-S3-Relay-1CH-Demo.zip
//   Schematic: https://files.waveshare.com/wiki/ESP32-S3-Relay-1CH/ESP32-S3-Relay-1CH-schematic.pdf
//
// The single relay is the DRM0 actuator for inverters that cannot be curtailed over
// RS485: a potential-free contact on the inverter's DRM port. Failsafe by wiring: the
// relay de-energised means DRM not asserted -- a dead bridge never blocks production.
//
// ACTUATION VERIFIED ON HARDWARE 2026-07-28, on a bench board with nothing wired to the
// contacts. GPIO47 active-high drives the coil: the relay was heard to switch, the indicator
// LED followed it in both directions, and three independent readings agreed with the hardware
// -- REST bridge.relays [true], heliograph_relay_energised{relay="0"} 1, and back to false/0 on
// release. drm_mode was correctly ABSENT throughout, the role being "none".
//
// The wiring half of the failsafe is still untested: it cannot be, without an inverter on the
// other end. What is proven is that the firmware can energise and release this coil on demand.

#pragma once

#include <cstdint>

namespace heliograph::board {

inline constexpr const char* kName = "Waveshare ESP32-S3-Relay-1CH";

/// Stable slug identifying this board to machines, as opposed to kName which is for people.
///
/// Deliberately the SAME string as the PlatformIO environment suffix and the release asset
/// name (heliograph-<version>-{id}.bin), because that is what lets the update flow ask for the
/// image built for this board rather than guessing from a display name. Changing it renames a
/// release asset, so it does not change.
inline constexpr const char* kId = "relay-1ch";

// --- RS485 ---------------------------------------------------------------------------------
// WS_GPIO.h: TXD1 17, RXD1 18, TXD1EN 21. Schematic: TXD1/RXD1 run through the pi131M31
// isolator to the SP3485EN's DI/RO; EN handed to the UART as RTS
// (UART_MODE_RS485_HALF_DUPLEX), never toggled in software.
inline constexpr int kRs485Tx = 17;
inline constexpr int kRs485Rx = 18;
inline constexpr int kRs485De = 21;

/// UART peripheral used for RS485. UART0 is the USB-CDC console.
inline constexpr int kRs485UartNum = 1;

// --- Relays --------------------------------------------------------------------------------
// WS_GPIO.h: GPIO_PIN_CH1 47. WS_Relay.cpp drives HIGH to energise, so the safe
// (de-energised) state is LOW. The RelayController owns this pin exclusively; boot state
// is LOW, and no driver or output has any other path to it.
inline constexpr int  kRelayCount      = 1;
inline constexpr int  kRelayPins[1]    = {47};
inline constexpr bool kRelayActiveHigh = true;

// --- RTC -----------------------------------------------------------------------------------
// PCF85063AT at 0x51. I2C_Driver.h: SCL 38, SDA 39.
inline constexpr bool    kHasRtc        = true;
inline constexpr int     kRtcScl        = 38;
inline constexpr int     kRtcSda        = 39;
inline constexpr uint8_t kRtcI2cAddress = 0x51;

// --- BOOT button / status LED / buzzer -----------------------------------------------------
// BOOT on GPIO0 (Waveshare documentation, confirmed by Tim 2026-07-23) -- the SoC download
// strapping pin, so the hold-to-factory-reset recovery works here too.
//
// There is no SOFTWARE-DRIVEN status LED or buzzer: GPIO38/39 are the RTC I2C and GPIO21 is
// unassigned here, so a factory reset on this board is silent and the reboot is its only signal.
//
// The board DOES carry a relay indicator LED, wired across the coil in hardware rather than to
// a GPIO -- which is why kHasStatusLed stays false and is not a contradiction. For bring-up it
// is the better witness of the two: it sits after the driver transistor, so it lights when the
// coil is genuinely energised, where a software LED would only prove the firmware believed it
// had switched.
inline constexpr bool kHasBootButton = true;
inline constexpr int  kBootPin       = 0;
inline constexpr bool kHasStatusLed  = false;
inline constexpr int  kStatusLedPin  = -1;
inline constexpr bool kHasBuzzer     = false;
inline constexpr int  kBuzzerPin     = -1;

// --- Notes that are not pins ---------------------------------------------------------------
// Module   : ESP32-S3-WROOM-1U (ESP32-S3R8 -> 8 MB octal PSRAM)
// Flash    : W25Q128JVSI -> 16 MB
// USB      : native (D_N/D_P straight to the SoC, no CH340) -> ARDUINO_USB_CDC_ON_BOOT=1
// Isolation: B0505S-1WR3 + pi131M31; SGND is NOT GND. Do not bridge them.
// Termination: 120R (R23) on header H1. Fit the jumper only at a physical end of the bus.

}  // namespace heliograph::board
