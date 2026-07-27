// SPDX-License-Identifier: MIT

#include "protocols/modbus/modbus_client.h"

namespace heliograph::modbus {
namespace {

/// What a caller's parse step reports back.
///
/// `result` is the codec's verdict on the bytes. `accepted` is the caller's own verdict on a
/// frame the codec decoded cleanly -- the two are not the same question, and collapsing them
/// is how a well-formed answer to a different question gets recorded as success.
struct ParseStep {
    ParseResult result        = ParseResult::Malformed;
    bool        accepted      = true;
    uint8_t     exceptionCode = 0;
};

/// The half of a Modbus transaction that does not depend on what was asked: send, then read
/// until the answer parses, the deadline passes, or the line stops making sense.
///
/// Written once because it WAS written twice. The read path and the write path carried the
/// same deadline handling, the same incomplete-frame loop, the same full-buffer bail-out and
/// the same timeout mapping, in two functions that drifted apart only in their comments. Every
/// one of those rules is a decision about a bus that answers slowly or not at all, and having
/// two copies meant a fix to one leaving the other exactly as wrong as before.
///
/// A template rather than std::function: this runs per poll on a task with a fixed stack, and
/// there is no reason to pay for an indirect call and a possible allocation to express "the
/// caller decides how to parse".
template <typename ParseFn>
TransactionOutcome runTransaction(Transport& transport, const uint8_t* req, size_t reqLen,
                                  const ReadTiming& timing, ParseFn parse) {
    TransactionOutcome outcome;

    transport.flushInput();
    if (transport.write(req, reqLen) != reqLen) {
        outcome.status = TransactionStatus::TransportError;
        return outcome;
    }

    uint8_t        rx[kMaxAdu];
    size_t         have     = 0;
    const uint64_t deadline = transport.nowMs() + timing.transactionDeadlineMs;
    for (;;) {
        // Checked before parsing as well as before reading: a device that keeps dribbling
        // bytes that never form a frame must still hit the ceiling.
        if (transport.nowMs() >= deadline) {
            outcome.status = TransactionStatus::Timeout;
            return outcome;
        }

        const ParseStep step = parse(rx, have);
        switch (step.result) {
            case ParseResult::Ok:
                // Decoded cleanly is not the same as acceptable. What `accepted` means is the
                // caller's business -- see the two call sites, where the reasoning lives.
                outcome.status = step.accepted ? TransactionStatus::Ok
                                               : TransactionStatus::Protocol;
                return outcome;
            case ParseResult::Exception:
                outcome.status        = TransactionStatus::Exception;
                outcome.exceptionCode = step.exceptionCode;
                return outcome;
            case ParseResult::Incomplete:
                break;  // fall through and read more
            case ParseResult::BadCrc:
                // The one outcome that indicts the cable rather than the configuration. The
                // codec has always kept it separate; fusing it here is what made a Modbus
                // driver unable to ever report a checksum error.
                outcome.status = TransactionStatus::Crc;
                return outcome;
            default:
                // WrongUnit / WrongFunction / Malformed.
                outcome.status = TransactionStatus::Protocol;
                return outcome;
        }

        if (have >= sizeof(rx)) {
            // A full ADU of bytes that still does not parse: the bus is not speaking Modbus at
            // us. Reporting Protocol rather than reading forever.
            outcome.status = TransactionStatus::Protocol;
            return outcome;
        }
        const size_t n = transport.read(rx + have, sizeof(rx) - have, timing.responseTimeoutMs);
        if (n == 0) {
            outcome.status = TransactionStatus::Timeout;
            return outcome;
        }
        have += n;
    }
}

}  // namespace

ReadOutcome readRegisters(Transport& transport, uint8_t unitId, uint8_t functionCode,
                          uint16_t start, uint16_t count, uint16_t* out, uint16_t outCapacity,
                          const ReadTiming& timing) {
    ReadOutcome outcome;
    if (out == nullptr || count == 0 || count > outCapacity) {
        outcome.status = ReadStatus::Protocol;
        return outcome;
    }

    uint8_t req[8];
    size_t  reqLen = 0;
    if (buildReadRequest(unitId, functionCode, start, count, req, sizeof(req), reqLen) !=
        BuildResult::Ok) {
        outcome.status = ReadStatus::Protocol;
        return outcome;
    }

    return runTransaction(
        transport, req, reqLen, timing, [&](const uint8_t* rx, size_t have) {
            ParseStep    step;
            ReadResponse resp;
            step.result = parseReadResponse(rx, have, unitId, functionCode, out, count, resp);
            // Read on every pass, acted on only when the result is Exception. Zero otherwise,
            // because both response structs default-initialise every member.
            step.exceptionCode = resp.exceptionCode;
            // A reply that decodes cleanly but carries FEWER registers than were asked for is
            // not success. The codec only checks the byte count against the caller's buffer
            // capacity, so a short frame leaves the tail of `out` untouched -- while callers
            // record the block as covering the full count they requested. Whatever that tail
            // happens to hold is then decoded and published as genuine readings on a poll
            // reporting Ok: uninitialised scratch in one driver, zeros in another (and a zero
            // scale factor is legal, so zeros read as a real 0 W). Either way it defeats the
            // never-fabricate-a-reading rule one layer below where that rule is enforced
            // (review, 2026-07-25).
            step.accepted = resp.registerCount == count;
            return step;
        });
}

TransactionOutcome writeSingleRegister(Transport& transport, uint8_t unitId, uint16_t address,
                                       uint16_t value, const ReadTiming& timing) {
    TransactionOutcome outcome;

    uint8_t req[8];
    size_t  reqLen = 0;
    if (buildWriteSingleRegister(unitId, address, value, req, sizeof(req), reqLen) !=
        BuildResult::Ok) {
        outcome.status = TransactionStatus::Protocol;
        return outcome;
    }

    return runTransaction(
        transport, req, reqLen, timing, [&](const uint8_t* rx, size_t have) {
            ParseStep     step;
            WriteResponse resp;
            step.result = parseWriteResponse(rx, have, unitId, kWriteSingleRegister, resp);
            // Carried out of the response whatever the verdict; the skeleton reads it only
            // when the result IS Exception. That case is the ordinary answer to a refused
            // control write -- a read-only register, an out-of-range value, or a device that
            // wants an unlock first -- which is why it is a status of its own rather than a
            // protocol error.
            step.exceptionCode = resp.exceptionCode;
            // The echo is the confirmation. A device that answers with a different address or
            // a different value has not done what was asked, however well-formed the frame is
            // -- and treating that as success is how a setpoint silently does not arrive.
            step.accepted = resp.address == address && resp.value == value;
            return step;
        });
}

}  // namespace heliograph::modbus
