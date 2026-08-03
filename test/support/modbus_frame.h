// SPDX-License-Identifier: MIT
//
// Modbus RTU's trailing checksum, appended the one way the wire accepts it: LOW byte first.
//
// This lived inline as the same three lines in sixteen places -- every hand-built request in
// test_capture, every scripted reply in test_modbus_profile and test_modbus, and the fake
// SunSpec device. A byte order written out sixteen times is sixteen chances to write it
// backwards once, and a test that builds its frame backwards and then asserts against a decoder
// that reads it backwards passes. Both halves have to agree with the bus, not merely with each
// other, so the order belongs in one place.
//
// Only for frames assembled in a std::vector. test_modbus/rtu.cpp keeps its own arithmetic: it
// is the suite that tests crc16 itself, and a helper built on the thing under test would assert
// nothing.

#pragma once

#include <cstdint>
#include <vector>

#include "protocols/modbus/modbus_rtu.h"

// heliograph::test, not a global `test`: that is where mock_transport.h and the fake devices
// already live, and a second top-level namespace with the same name is ambiguous the moment a
// suite says `using namespace heliograph;` -- which most of them do.
namespace heliograph::test {

inline void appendModbusCrc(std::vector<uint8_t>& frame) {
    const uint16_t crc = modbus::crc16(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<uint8_t>(crc >> 8));
}

}  // namespace heliograph::test
