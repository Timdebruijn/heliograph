// SPDX-License-Identifier: MIT
//
// Waveshare ESP32-S3-Relay-6CH board definition. STATUS: pins verified from the official
// schematic AND the official demo source; this firmware has not run on the physical board
// yet (relay polarity check pending).
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
// Community configuration drives HIGH to energise; treat active-high as unconfirmed
// polarity until the first hardware session.
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

// --- Notes that are not pins ---------------------------------------------------------------
// Flash  : 8 MB -> uses partitions_8mb_ota.csv, NOT the 16 MB table
// Buzzer : GPIO21 (transistor-driven; unused by this firmware)
// RGB LED: GPIO38 (WS2812; unused by this firmware)
// BOOT   : GPIO0 per the community configuration; unverified on hardware
// Power  : USB-C, or 7-36 V DC terminal
// Relays : 6x <=10 A 250 VAC / 30 VDC, optocoupler + digital isolation
// I2C    : GPIO4 (SDA) / GPIO5 (SCL) are the Pico-HAT expansion I2C pins; the official
//          demo drives an EXTERNAL DS3231 module there. No onboard RTC (schematic clean).

}  // namespace heliograph::board
