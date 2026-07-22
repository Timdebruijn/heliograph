// SPDX-License-Identifier: MIT
//
// Waveshare ESP32-S3-RS485-CAN board definition.
//
// This project shipped its first months believing it ran on the ESP32-S3-Relay-1CH -- the
// board named in the original project brief. The physical boards turned out to be the
// RS485-CAN (spotted 2026-07-22). Nothing ever misbehaved because the RS485 subsystem is
// pin-identical between the two designs; the constants below are confirmed by the
// RS485-CAN documentation AND by months of runtime on the real hardware.
//
//   Wiki: https://www.waveshare.com/wiki/ESP32-S3-RS485-CAN
//
// There is NO relay and NO RTC on this board (both existed only on the Relay-1CH).

#pragma once

namespace heliograph::board {

inline constexpr const char* kName = "Waveshare ESP32-S3-RS485-CAN";

// --- RS485 ---------------------------------------------------------------------------------
// Isolated transceiver. The EN pin is handed to the UART as its RTS line with
// UART_MODE_RS485_HALF_DUPLEX, so the peripheral switches direction on the exact bit
// boundary; toggling it in software cannot race the last stop bit reliably.
inline constexpr int kRs485Tx = 17;
inline constexpr int kRs485Rx = 18;
inline constexpr int kRs485De = 21;

/// UART peripheral used for RS485. UART0 is the USB-CDC console.
inline constexpr int kRs485UartNum = 1;

// --- CAN (unused) --------------------------------------------------------------------------
// Isolated CAN transceiver on the second terminal block. Not used by any driver today;
// recorded so a future CAN-based device (BMS protocols live here) starts from facts.
inline constexpr int kCanTx = 15;
inline constexpr int kCanRx = 16;

// --- Notes that are not pins ---------------------------------------------------------------
// Flash    : 16 MB (matches partitions_16mb_ota.csv; factory-flashed many times)
// PSRAM    : 8 MB
// USB      : native USB-C (no CH340) -> ARDUINO_USB_CDC_ON_BOOT=1
// Power    : USB-C, or 7-36 V DC terminal via onboard regulator
// Isolation: power + optocoupler isolation on both RS485 and CAN -- SGND is NOT GND
// Termination: 120R jumper-selectable, for RS485 and CAN separately
// Buttons  : BOOT and RESET are present on this board. The BOOT GPIO is not recorded as a
//            constant yet -- measure before building the hold-BOOT-to-factory-reset
//            recovery path (backlog), per the house rule that no pin is ever guessed.

}  // namespace heliograph::board
