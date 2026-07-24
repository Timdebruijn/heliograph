// SPDX-License-Identifier: MIT
//
// One complete Modbus read transaction over a Transport: send the request, collect the reply
// under a wall-clock deadline, hand back decoded registers.
//
// Deliberately a SEPARATE file from modbus_rtu.h. That one is a pure codec -- it knows bytes,
// checksums and framing, includes nothing but <cstddef>/<cstdint>, and stays host-testable
// without any notion of a bus. This file is the layer that does know about a bus, and keeping
// the two apart is what stops the codec quietly acquiring a transport dependency. The PMU
// family draws the same line: shared framing in protocols/pmu, transport work in the drivers.
//
// It lives here rather than inside a driver because unrelated register maps need the exact same
// exchange: a vendor's own proprietary block layout and the published SunSpec model chain have
// nothing in common as maps, yet the transaction below is identical for both. Sharing the
// exchange while keeping the maps apart in their own drivers is the whole point -- which is
// also why nothing in this file may name a manufacturer (see tools/check_layering.sh).

#pragma once

#include <cstdint>

#include "protocols/modbus/modbus_rtu.h"
#include "transport/transport.h"

namespace heliograph::modbus {

enum class ReadStatus : uint8_t {
    Ok,              ///< `count` registers decoded into the caller's buffer
    Exception,       ///< the device answered, refusing: see ReadOutcome::exceptionCode
    Timeout,         ///< nothing, or not a whole frame, arrived in time
    Protocol,        ///< bad CRC, wrong unit, wrong function code, or malformed
    TransportError,  ///< the request could not be written at all
};

struct ReadTiming {
    /// Ceiling on the WHOLE exchange. Each read() renews its own timeout, so without this a
    /// slow trickle of bytes could hold the bus for many seconds without ever completing a
    /// frame (found in review, 2026-07-20).
    uint32_t transactionDeadlineMs = 3000;
    /// Per-read timeout while waiting for more of the reply.
    uint32_t responseTimeoutMs = 1000;
};

struct ReadOutcome {
    ReadStatus status        = ReadStatus::TransportError;
    uint8_t    exceptionCode = 0;  ///< only meaningful when status == Exception
};

/// Reads `count` registers starting at `start` into `out`.
///
/// `functionCode` selects the register space (kReadInputRegisters / kReadHoldingRegisters).
/// `outCapacity` is how many registers `out` can actually hold; a request for more than fits
/// is refused as Protocol rather than trusted, so a caller's arithmetic mistake cannot become
/// a buffer overrun on a byte count the device chose.
///
/// Does no logging and no tracing: what a raw block means is the driver's business, and a
/// shared helper that traced would either guess at brand vocabulary or force one on everyone.
ReadOutcome readRegisters(Transport& transport, uint8_t unitId, uint8_t functionCode,
                          uint16_t start, uint16_t count, uint16_t* out, uint16_t outCapacity,
                          const ReadTiming& timing = {});

}  // namespace heliograph::modbus
