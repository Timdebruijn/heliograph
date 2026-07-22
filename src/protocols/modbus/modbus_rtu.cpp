// SPDX-License-Identifier: MIT

#include "modbus_rtu.h"

namespace heliograph::modbus {
namespace {

/// Appends the CRC of the first `len` bytes at out[len], low byte first (Modbus wire order),
/// and returns the new length. Caller guarantees room for two more bytes.
size_t appendCrc(uint8_t* out, size_t len) {
    const uint16_t crc = crc16(out, len);
    out[len]     = static_cast<uint8_t>(crc & 0xFF);
    out[len + 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    return len + 2;
}

void putBe16(uint8_t* out, uint16_t v) {
    out[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(v & 0xFF);
}

uint16_t getBe16(const uint8_t* in) {
    return static_cast<uint16_t>((static_cast<uint16_t>(in[0]) << 8) | in[1]);
}

bool crcOk(const uint8_t* buf, size_t len) {
    // len includes the trailing two CRC bytes.
    const uint16_t expected = crc16(buf, len - 2);
    const uint16_t actual   = static_cast<uint16_t>(buf[len - 2] | (buf[len - 1] << 8));
    return expected == actual;
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
        if (!crcOk(buf, 5)) {
            return ParseResult::BadCrc;
        }
        if (buf[0] != expectedUnit) {
            return ParseResult::WrongUnit;
        }
        // The echoed function (exception flag stripped) must match what we asked. Without this
        // a stale or misrouted exception frame -- right unit, valid CRC, but for a different
        // request -- would be accepted as the answer to this one on a shared multidrop bus.
        if ((buf[1] & ~kExceptionFlag) != expectedFunction) {
            return ParseResult::WrongFunction;
        }
        out.unitId        = buf[0];
        out.functionCode  = buf[1];
        out.exceptionCode = buf[2];
        return ParseResult::Exception;
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
    // Byte count must be even (whole registers) and match the caller's buffer.
    if ((byteCount & 1) != 0) {
        return ParseResult::Malformed;
    }
    const size_t registers = byteCount / 2;
    if (registers > regsCapacity) {
        return ParseResult::Malformed;
    }
    for (size_t i = 0; i < registers; ++i) {
        regsOut[i] = getBe16(buf + 3 + i * 2);
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
        if (!crcOk(buf, 5)) {
            return ParseResult::BadCrc;
        }
        if (buf[0] != expectedUnit) {
            return ParseResult::WrongUnit;
        }
        // The echoed function (exception flag stripped) must match what we asked. Without this
        // a stale or misrouted exception frame -- right unit, valid CRC, but for a different
        // request -- would be accepted as the answer to this one on a shared multidrop bus.
        if ((buf[1] & ~kExceptionFlag) != expectedFunction) {
            return ParseResult::WrongFunction;
        }
        out.unitId        = buf[0];
        out.functionCode  = buf[1];
        out.exceptionCode = buf[2];
        return ParseResult::Exception;
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
    out.address      = getBe16(buf + 2);
    out.value        = getBe16(buf + 4);
    return ParseResult::Ok;
}

}  // namespace heliograph::modbus
