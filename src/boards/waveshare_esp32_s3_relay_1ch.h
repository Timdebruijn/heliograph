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

#pragma once

#include <cstdint>

namespace heliograph::board {

inline constexpr const char* kName = "Waveshare ESP32-S3-Relay-1CH";

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

// --- Notes that are not pins ---------------------------------------------------------------
// Module   : ESP32-S3-WROOM-1U (ESP32-S3R8 -> 8 MB octal PSRAM)
// Flash    : W25Q128JVSI -> 16 MB
// USB      : native (D_N/D_P straight to the SoC, no CH340) -> ARDUINO_USB_CDC_ON_BOOT=1
// Isolation: B0505S-1WR3 + pi131M31; SGND is NOT GND. Do not bridge them.
// Termination: 120R (R23) on header H1. Fit the jumper only at a physical end of the bus.
// NOT DEFINED: the BOOT button GPIO -- not pinned down unambiguously; measure first.

}  // namespace heliograph::board
