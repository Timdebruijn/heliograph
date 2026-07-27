// SPDX-License-Identifier: MIT

#include "protocols/modbus/modbus_client.h"

namespace heliograph::modbus {

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

    transport.flushInput();
    if (transport.write(req, reqLen) != reqLen) {
        outcome.status = ReadStatus::TransportError;
        return outcome;
    }

    uint8_t        rx[kMaxAdu];
    size_t         have     = 0;
    const uint64_t deadline = transport.nowMs() + timing.transactionDeadlineMs;
    for (;;) {
        // Checked before parsing as well as before reading: a device that keeps dribbling
        // bytes that never form a frame must still hit the ceiling.
        if (transport.nowMs() >= deadline) {
            outcome.status = ReadStatus::Timeout;
            return outcome;
        }

        ReadResponse resp;
        switch (parseReadResponse(rx, have, unitId, functionCode, out, count, resp)) {
            case ParseResult::Ok:
                // A reply that decodes cleanly but carries FEWER registers than were asked for
                // is not success. The codec only checks the byte count against the caller's
                // buffer capacity, so a short frame leaves the tail of `out` untouched -- while
                // callers record the block as covering the full count they requested. Whatever
                // that tail happens to hold is then decoded and published as genuine readings
                // on a poll reporting Ok: uninitialised scratch in one driver, zeros in another
                // (and a zero scale factor is legal, so zeros read as a real 0 W). Either way
                // it defeats the never-fabricate-a-reading rule one layer below where that rule
                // is enforced (review, 2026-07-25).
                if (resp.registerCount != count) {
                    outcome.status = ReadStatus::Protocol;
                    return outcome;
                }
                outcome.status = ReadStatus::Ok;
                return outcome;
            case ParseResult::Exception:
                outcome.status        = ReadStatus::Exception;
                outcome.exceptionCode = resp.exceptionCode;
                return outcome;
            case ParseResult::Incomplete:
                break;  // fall through and read more
            case ParseResult::BadCrc:
                // The one outcome that indicts the cable rather than the configuration. The
                // codec has always kept it separate; fusing it here is what made a Modbus
                // driver unable to ever report a checksum error.
                outcome.status = ReadStatus::Crc;
                return outcome;
            default:
                // WrongUnit / WrongFunction / Malformed.
                outcome.status = ReadStatus::Protocol;
                return outcome;
        }

        if (have >= sizeof(rx)) {
            // A full ADU of bytes that still does not parse: the bus is not speaking Modbus at
            // us. Reporting Protocol rather than reading forever.
            outcome.status = ReadStatus::Protocol;
            return outcome;
        }
        const size_t n = transport.read(rx + have, sizeof(rx) - have, timing.responseTimeoutMs);
        if (n == 0) {
            outcome.status = ReadStatus::Timeout;
            return outcome;
        }
        have += n;
    }
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

    transport.flushInput();
    if (transport.write(req, reqLen) != reqLen) {
        outcome.status = TransactionStatus::TransportError;
        return outcome;
    }

    uint8_t        rx[kMaxAdu];
    size_t         have     = 0;
    const uint64_t deadline = transport.nowMs() + timing.transactionDeadlineMs;
    for (;;) {
        if (transport.nowMs() >= deadline) {
            outcome.status = TransactionStatus::Timeout;
            return outcome;
        }

        WriteResponse resp;
        switch (parseWriteResponse(rx, have, unitId, kWriteSingleRegister, resp)) {
            case ParseResult::Ok:
                // The echo is the confirmation. A device that answers with a different address
                // or a different value has not done what was asked, however well-formed the
                // frame is -- and treating that as success is how a setpoint silently does not
                // arrive.
                if (resp.address != address || resp.value != value) {
                    outcome.status = TransactionStatus::Protocol;
                    return outcome;
                }
                outcome.status = TransactionStatus::Ok;
                return outcome;
            case ParseResult::Exception:
                // The ordinary answer to a control write that is refused: read-only register,
                // out-of-range value, or a device that wants an unlock first.
                outcome.status        = TransactionStatus::Exception;
                outcome.exceptionCode = resp.exceptionCode;
                return outcome;
            case ParseResult::Incomplete:
                break;  // fall through and read more
            case ParseResult::BadCrc:
                outcome.status = TransactionStatus::Crc;
                return outcome;
            default:
                outcome.status = TransactionStatus::Protocol;
                return outcome;
        }

        if (have >= sizeof(rx)) {
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

}  // namespace heliograph::modbus
