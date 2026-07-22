// SPDX-License-Identifier: MIT
//
// Waveshare ESP32-S3-Relay-1CH board definition.
//
// Every pin here was read out of the official demo source and cross-checked against the
// official schematic. None of it is guessed. See docs/hardware.md for the full trail.
//
//   Demo:      https://files.waveshare.com/wiki/ESP32-S3-Relay-1CH/ESP32-S3-Relay-1CH-Demo.zip
//   Schematic: https://files.waveshare.com/wiki/ESP32-S3-Relay-1CH/ESP32-S3-Relay-1CH-schematic.pdf

#pragma once

#include <cstdint>

namespace heliograph::board {

inline constexpr const char* kName = "Waveshare ESP32-S3-Relay-1CH";

// --- RS485 ---------------------------------------------------------------------------------
// WS_GPIO.h: TXD1 17, RXD1 18, TXD1EN 21.
// Schematic: TXD1/RXD1 run through the pi131M31 isolator to the SP3485EN's DI/RO.
inline constexpr int kRs485Tx = 17;
inline constexpr int kRs485Rx = 18;

// Direction control. NOT a passive auto-direction circuit: the SP3485EN has discrete RE (pin
// 2) and DE (pin 3) tied together on net RS485_EN, driven from this GPIO via the isolator.
//
// It is nonetheless wrong to toggle this in software. The demo hands the pin to the UART as
// its RTS line and enables UART_MODE_RS485_HALF_DUPLEX, so the peripheral switches direction
// on the exact bit boundary. A software toggle cannot race the last stop bit reliably.
inline constexpr int kRs485De = 21;

/// UART peripheral used for RS485. UART0 is the USB-CDC console.
inline constexpr int kRs485UartNum = 1;

// --- Relay ---------------------------------------------------------------------------------
// WS_GPIO.h: GPIO_PIN_CH1 47. WS_Relay.cpp drives HIGH to energise, so the safe state is LOW.
//
// Out of scope for the MVP. It must be driven LOW as the very first thing in setup(), before
// WiFi, RS485 or anything that can fail, and nothing else may ever touch it.
inline constexpr int  kRelayCh1     = 47;
inline constexpr bool kRelayActiveHigh = true;

// --- I2C / RTC -----------------------------------------------------------------------------
// I2C_Driver.h: SCL 38, SDA 39. Schematic: PCF85063AT at 0x51. Unused in the MVP.
inline constexpr int     kI2cScl        = 38;
inline constexpr int     kI2cSda        = 39;
inline constexpr uint8_t kRtcI2cAddress = 0x51;

// --- Notes that are not pins ---------------------------------------------------------------
// Module   : ESP32-S3-WROOM-1U (ESP32-S3R8 -> 8 MB octal PSRAM)
// Flash    : W25Q128JVSI -> 16 MB
// USB      : native (D_N/D_P straight to the SoC, no CH340) -> ARDUINO_USB_CDC_ON_BOOT=1
// Isolation: B0505S-1WR3 + pi131M31; SGND is NOT GND. Do not bridge them.
// Termination: 120R (R23) on header H1. Fit the jumper only at a physical end of the bus.
//
// NOT DEFINED HERE ON PURPOSE: the BOOT button GPIO. The FAQ mentions BOOT+RESET, but the
// schematic text extraction did not pin it down unambiguously, and ESP32-S3 convention
// (GPIO0) is convention, not evidence. Provisioning reset needs it -- measure it on real
// hardware first (Phase 3/8) rather than shipping a guess.

}  // namespace heliograph::board
