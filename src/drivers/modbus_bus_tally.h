// SPDX-License-Identifier: MIT
//
// One mapping from a Modbus read outcome onto the frame-level bus counters, shared by every
// Modbus-RTU driver.
//
// Kept here rather than in protocols/modbus so the codec stays free of anything driver-shaped,
// and shared rather than written out per driver so the two Modbus drivers cannot drift into
// describing the same bus condition differently -- which they already did once, when one
// reported a short reply as an invalid frame and the other as an unregistered device.

#pragma once

#include "drivers/inverter_driver.h"
#include "protocols/modbus/modbus_client.h"

namespace heliograph {

/// Adds one transaction's outcome to `counts`. Ok and TransportError move nothing (the first
/// is not an error, the second never reached the wire); nor does an exception reply, which is a
/// healthy device declining a range -- see docs/prometheus.md on why that must not indict the
/// cabling.
inline void tallyModbusRead(BusErrorCounts& counts, modbus::ReadStatus status) {
    switch (status) {
        case modbus::ReadStatus::Crc:      ++counts.checksumErrors; break;
        case modbus::ReadStatus::Timeout:  ++counts.timeouts;       break;
        case modbus::ReadStatus::Protocol: ++counts.invalidFrames;  break;
        case modbus::ReadStatus::Ok:
        case modbus::ReadStatus::Exception:
        case modbus::ReadStatus::TransportError:
            break;
    }
}

}  // namespace heliograph
