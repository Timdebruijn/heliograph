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
                outcome.status = ReadStatus::Ok;
                return outcome;
            case ParseResult::Exception:
                outcome.status        = ReadStatus::Exception;
                outcome.exceptionCode = resp.exceptionCode;
                return outcome;
            case ParseResult::Incomplete:
                break;  // fall through and read more
            default:
                // BadCrc / WrongUnit / WrongFunction / Malformed.
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

}  // namespace heliograph::modbus
