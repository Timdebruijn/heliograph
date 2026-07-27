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
    Ok,         ///< `count` registers decoded into the caller's buffer
    Exception,  ///< the device answered, refusing: see ReadOutcome::exceptionCode
    Timeout,    ///< nothing, or not a whole frame, arrived in time
    /// Bytes arrived and failed the CRC. Kept apart from Protocol on purpose: this is the one
    /// outcome that means the WIRE is bad -- a missing ground, no termination, a stub, noise
    /// coupled from the inverter's own output. Drivers map it to PollResult::ChecksumError,
    /// which is what the alerting rules key on, precisely because every night legitimately
    /// produces timeouts and would drown a rule built on those. Fusing it into Protocol made
    /// that counter structurally unreachable on a Modbus bus (review, 2026-07-25).
    Crc,
    /// The frame was intact but not what we asked for: wrong unit, wrong function code,
    /// malformed, or fewer registers than requested. Points at addressing or a device quirk,
    /// not at the cable.
    Protocol,
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

/// The outcomes of a Modbus transaction, whichever direction it went.
///
/// Aliases rather than a parallel enum for writes: a write fails in exactly the same ways a read
/// does -- silence, a mangled CRC, an exception, a frame that is not the answer to our question
/// -- and two lists would be two things to keep in step. The names below are the ones to use in
/// new code; the Read* spellings stay because they are what every existing driver says.
using TransactionStatus  = ReadStatus;
using TransactionOutcome = ReadOutcome;

/// Writes one holding register (0x06) and confirms the device's echo.
///
/// Success means the device echoed BOTH the address and the value we sent. That check is the
/// whole reason this returns something other than "the bytes went out": a write whose echo is
/// not verified is a request, not a setting, and on a control register the difference is
/// between an inverter that is limited and one that everybody believes is limited.
///
/// A mismatched echo reports Protocol -- the documented meaning of "the frame was intact but
/// not what we asked for" -- rather than Ok.
///
/// Deliberately single-register only. Write-multiple exists in the codec, but a control model
/// interleaves the points somebody wants to set with timing and ramp registers they do not, and
/// a span write cannot express that difference.
TransactionOutcome writeSingleRegister(Transport& transport, uint8_t unitId, uint16_t address,
                                       uint16_t value, const ReadTiming& timing = {});

}  // namespace heliograph::modbus
