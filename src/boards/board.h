// SPDX-License-Identifier: MIT
//
// Board selection. Exactly one HELIOGRAPH_BOARD_* flag is defined per PlatformIO
// environment; everything outside src/boards/ includes only this header.
//
// Every board header provides the same constant set:
//   kName                                  -- exact product name, shown in the UI and HA
//   kRs485Tx / kRs485Rx / kRs485De         -- kRs485De < 0 means "no direction pin":
//                                             the transport then skips RTS configuration
//   kRs485UartNum
//   kRelayCount, kRelayPins[], kRelayActiveHigh
//   kHasRtc (+ kRtcScl/kRtcSda/kRtcI2cAddress when true)
//   kHasBootButton (+ kBootPin when true)   -- hold-to-factory-reset recovery
//   kHasStatusLed  (+ kStatusLedPin when true) -- single WS2812 health indicator
//   kHasBuzzer     (+ kBuzzerPin when true)    -- audible confirmation
//
// House rule applies here more than anywhere: no pin is ever guessed. A pin that could
// not be verified is absent or marked unverified in the board header itself.

#pragma once

#if defined(HELIOGRAPH_BOARD_RS485_CAN)
#include "boards/waveshare_esp32_s3_rs485_can.h"
#elif defined(HELIOGRAPH_BOARD_RELAY_1CH)
#include "boards/waveshare_esp32_s3_relay_1ch.h"
#elif defined(HELIOGRAPH_BOARD_RELAY_6CH)
#include "boards/waveshare_esp32_s3_relay_6ch.h"
#elif defined(ESP32)
#error "No HELIOGRAPH_BOARD_* flag defined; add one to the PlatformIO environment"
#else
// Host build (native tests): no board. Nothing in the host-testable core may include
// this header; the guard exists so an accidental include fails loudly on the ESP32 and
// compiles to nothing on the host.
#endif
