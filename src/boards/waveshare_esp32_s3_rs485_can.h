// SPDX-License-Identifier: MIT
//
// Waveshare ESP32-S3-RS485-CAN board definition.
//
// This project shipped its first months believing it ran on the ESP32-S3-Relay-1CH -- the
// board named in the original project brief. The physical boards turned out to be the
// RS485-CAN (spotted 2026-07-22). Nothing ever misbehaved because the RS485 subsystem is
// pin-identical between the two designs.
//
// Pins verified against the official schematic (ESP32-S3-RS485-CAN-Schematic.pdf,
// files.waveshare.com) on 2026-07-22 AND by months of runtime on the real hardware.
//
//   Wiki: https://www.waveshare.com/wiki/ESP32-S3-RS485-CAN

#pragma once

#include <cstdint>

namespace heliograph::board {

inline constexpr const char* kName = "Waveshare ESP32-S3-RS485-CAN";

// --- RS485 ---------------------------------------------------------------------------------
// Isolated transceiver (SP3485EN behind a pi163E31 isolator; schematic nets TXD1/RXD1/
// RS485_EN). The EN pin is handed to the UART as its RTS line with
// UART_MODE_RS485_HALF_DUPLEX, so the peripheral switches direction on the exact bit
// boundary; toggling it in software cannot race the last stop bit reliably.
inline constexpr int kRs485Tx = 17;
inline constexpr int kRs485Rx = 18;
inline constexpr int kRs485De = 21;

/// UART peripheral used for RS485. UART0 is the USB-CDC console.
inline constexpr int kRs485UartNum = 1;

// --- Relays --------------------------------------------------------------------------------
// None on this board.
inline constexpr int  kRelayCount      = 0;
inline constexpr int  kRelayPins[1]    = {-1};  // size 1: zero-length arrays are non-standard
inline constexpr bool kRelayActiveHigh = true;

// --- RTC -----------------------------------------------------------------------------------
// PCF85063AT with a 32.768 kHz crystal. Schematic GPIO matrix: RTC_SCL = IO38,
// RTC_SDA = IO39, RTC_INT = IO40 (INT unused by this firmware). An earlier revision of
// this header claimed "no RTC" on the strength of an incomplete community document; the
// official schematic says otherwise. Verified 2026-07-22.
inline constexpr bool    kHasRtc        = true;
inline constexpr int     kRtcScl        = 38;
inline constexpr int     kRtcSda        = 39;
inline constexpr uint8_t kRtcI2cAddress = 0x51;

// --- CAN (unused) --------------------------------------------------------------------------
// Isolated CAN transceiver (TJA1051T) on the second terminal block. Not used by any
// driver today; recorded so a future CAN-based device (BMS protocols live here) starts
// from facts.
inline constexpr int kCanTx = 15;
inline constexpr int kCanRx = 16;

// --- BOOT button / status LED / buzzer -----------------------------------------------------
// BOOT on GPIO0 (confirmed against the schematic by Tim 2026-07-23), so the hold-to-factory-
// reset recovery works on the production board too. No status LED or buzzer: GPIO38/39 are the
// RTC I2C here and GPIO21 is the RS485 direction line, so a factory reset is silent -- the
// reboot is its only signal. Runtime read still to be confirmed on hardware at a convenient
// flash (this is the production board running a multi-day soak; no need to disturb it for it).
inline constexpr bool kHasBootButton = true;
inline constexpr int  kBootPin       = 0;
inline constexpr bool kHasStatusLed  = false;
inline constexpr int  kStatusLedPin  = -1;
inline constexpr bool kHasBuzzer     = false;
inline constexpr int  kBuzzerPin     = -1;

// --- Notes that are not pins ---------------------------------------------------------------
// Flash    : 16 MB (matches partitions_16mb_ota.csv; factory-flashed many times)
// PSRAM    : 8 MB (ESP32-S3R8)
// USB      : native USB-C (no CH340) -> ARDUINO_USB_CDC_ON_BOOT=1
// Power    : USB-C, or 7-36 V DC terminal via onboard regulator
// Isolation: power + optocoupler isolation on both RS485 and CAN -- SGND is NOT GND

}  // namespace heliograph::board
