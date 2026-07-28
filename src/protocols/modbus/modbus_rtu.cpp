// SPDX-License-Identifier: MIT

#include "modbus_rtu.h"

#include "protocols/byte_order.h"

namespace heliograph::modbus {
namespace {

/// Appends the CRC of the first `len` bytes at out[len], low byte first (Modbus wire order),
/// and returns the new length. Caller guarantees room for two more bytes.
size_t appendCrc(uint8_t* out, size_t len) {
    bytes::putLe16(out + len, crc16(out, len));  // Modbus sends the CRC low byte first
    return len + 2;
}

using bytes::be16;
using bytes::putBe16;

bool crcOk(const uint8_t* buf, size_t len) {
    // len includes the trailing two CRC bytes. The CRC is the one little-endian field in an
    // otherwise big-endian frame -- hence le16 here and be16 everywhere else.
    return crc16(buf, len - 2) == bytes::le16(buf, len - 2);
}

}  // namespace

uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 1) {
                crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

BuildResult buildReadRequest(uint8_t unitId, uint8_t functionCode, uint16_t startAddress,
                             uint16_t quantity, uint8_t* out, size_t capacity, size_t& outLen) {
    outLen = 0;
    if (quantity == 0 || quantity > kMaxReadRegisters) {
        return BuildResult::InvalidQuantity;
    }
    if (capacity < 8) {  // unit + fn + 2 addr + 2 qty + 2 crc
        return BuildResult::BufferTooSmall;
    }
    out[0] = unitId;
    out[1] = functionCode;
    putBe16(out + 2, startAddress);
    putBe16(out + 4, quantity);
    outLen = appendCrc(out, 6);
    return BuildResult::Ok;
}

BuildResult buildWriteSingleRegister(uint8_t unitId, uint16_t address, uint16_t value,
                                     uint8_t* out, size_t capacity, size_t& outLen) {
    outLen = 0;
    if (capacity < 8) {  // unit + fn + 2 addr + 2 value + 2 crc
        return BuildResult::BufferTooSmall;
    }
    out[0] = unitId;
    out[1] = kWriteSingleRegister;
    putBe16(out + 2, address);
    putBe16(out + 4, value);
    outLen = appendCrc(out, 6);
    return BuildResult::Ok;
}

BuildResult buildWriteMultipleRegisters(uint8_t unitId, uint16_t startAddress,
                                        const uint16_t* values, uint16_t count, uint8_t* out,
                                        size_t capacity, size_t& outLen) {
    outLen = 0;
    if (count == 0 || count > kMaxWriteRegisters) {
        return BuildResult::InvalidQuantity;
    }
    const size_t byteCount = static_cast<size_t>(count) * 2;
    const size_t needed    = 7 + byteCount + 2;  // header(7) + data + crc
    if (capacity < needed) {
        return BuildResult::BufferTooSmall;
    }
    out[0] = unitId;
    out[1] = kWriteMultipleRegisters;
    putBe16(out + 2, startAddress);
    putBe16(out + 4, count);
    out[6] = static_cast<uint8_t>(byteCount);
    for (uint16_t i = 0; i < count; ++i) {
        putBe16(out + 7 + i * 2, values[i]);
    }
    outLen = appendCrc(out, 7 + byteCount);
    return BuildResult::Ok;
}

size_t expectedReadResponseLength(uint16_t quantity) {
    return 5 + static_cast<size_t>(quantity) * 2;  // unit + fn + bytecount + data + crc
}

namespace {

/// The exception reply, which is the same frame whatever was asked.
///
/// unit + (function | 0x80) + code + CRC, five bytes, and every check on it is identical for a
/// read and for a write. It was written out twice, and the two copies had NOT drifted -- but
/// this is the path a device uses to say "I refuse", and a fix applied to one of two copies
/// leaves the other quietly wrong. The function-code check below is exactly such a fix (#67).
///
/// A template because ReadResponse and WriteResponse are separate types that happen to carry
/// the same three fields; giving them a shared base to avoid one template would be the larger
/// change. The caller tests the exception flag first -- it has to, to know to come here at all
/// -- and has already established len >= 5.
template <typename Response>
ParseResult parseExceptionFrame(const uint8_t* buf, uint8_t expectedUnit,
                                uint8_t expectedFunction, Response& out) {
    if (!crcOk(buf, 5)) {
        return ParseResult::BadCrc;
    }
    if (buf[0] != expectedUnit) {
        return ParseResult::WrongUnit;
    }
    // The echoed function (exception flag stripped) must match what we asked. Without this a
    // stale or misrouted exception frame -- right unit, valid CRC, but for a different request
    // -- would be accepted as the answer to this one on a shared multidrop bus.
    if ((buf[1] & ~kExceptionFlag) != expectedFunction) {
        return ParseResult::WrongFunction;
    }
    out.unitId        = buf[0];
    out.functionCode  = buf[1];
    out.exceptionCode = buf[2];
    return ParseResult::Exception;
}

}  // namespace

ParseResult parseReadResponse(const uint8_t* buf, size_t len, uint8_t expectedUnit,
                              uint8_t expectedFunction, uint16_t* regsOut, size_t regsCapacity,
                              ReadResponse& out) {
    // An exception reply is the shortest thing that can arrive: unit + fn|0x80 + code + CRC.
    if (len < 5) {
        return ParseResult::Incomplete;
    }
    // Exception first: it is 5 bytes, so demanding the full data-frame length would stall on a
    // device that is trying to tell us the request was illegal.
    if ((buf[1] & kExceptionFlag) != 0) {
        return parseExceptionFrame(buf, expectedUnit, expectedFunction, out);
    }

    const uint8_t byteCount = buf[2];
    const size_t  frameLen  = 3 + static_cast<size_t>(byteCount) + 2;
    if (len < frameLen) {
        return ParseResult::Incomplete;
    }
    if (!crcOk(buf, frameLen)) {
        return ParseResult::BadCrc;
    }
    if (buf[0] != expectedUnit) {
        return ParseResult::WrongUnit;
    }
    if (buf[1] != expectedFunction) {
        return ParseResult::WrongFunction;
    }
    // Byte count must be even (whole registers) and fit the caller's buffer. This is a
    // CAPACITY check only -- the codec is not told how many registers were requested, so a
    // reply carrying fewer than that still parses as Ok with a smaller registerCount. Callers
    // must compare registerCount against what they asked for; readRegisters() does.
    if ((byteCount & 1) != 0) {
        return ParseResult::Malformed;
    }
    const size_t registers = byteCount / 2;
    if (registers > regsCapacity) {
        return ParseResult::Malformed;
    }
    for (size_t i = 0; i < registers; ++i) {
        regsOut[i] = be16(buf + 3 + i * 2);
    }
    out.unitId        = buf[0];
    out.functionCode  = buf[1];
    out.registerCount = static_cast<uint8_t>(registers);
    return ParseResult::Ok;
}

ParseResult parseWriteResponse(const uint8_t* buf, size_t len, uint8_t expectedUnit,
                               uint8_t expectedFunction, WriteResponse& out) {
    // 0x06 and 0x10 both echo unit + fn + 2 address + 2 (value|count) + CRC = 8 bytes, and an
    // exception is 5 -- the exception is the shorter frame, so it is checked on its own length.
    if (len < 5) {
        return ParseResult::Incomplete;
    }
    if ((buf[1] & kExceptionFlag) != 0) {
        return parseExceptionFrame(buf, expectedUnit, expectedFunction, out);
    }
    if (len < 8) {
        return ParseResult::Incomplete;
    }
    if (!crcOk(buf, 8)) {
        return ParseResult::BadCrc;
    }
    if (buf[0] != expectedUnit) {
        return ParseResult::WrongUnit;
    }
    if (buf[1] != expectedFunction) {
        return ParseResult::WrongFunction;
    }
    out.unitId       = buf[0];
    out.functionCode = buf[1];
    out.address      = be16(buf + 2);
    out.value        = be16(buf + 4);
    return ParseResult::Ok;
}

}  // namespace heliograph::modbus
