// SPDX-License-Identifier: MIT
//
// Modbus RTU framing and PDU codec — the wire format, nothing device-specific.
//
// A shared protocol, not a brand: this is the open Modbus Application Protocol (v1.1b3) over
// a serial line, spoken by most modern string and hybrid inverters. It therefore lives
// outside src/drivers/ -- a driver decides which registers mean what; this file only knows
// how to put a request on the wire and validate the reply.
//
// Platform independent by construction: no Arduino, no ESP-IDF, so the whole codec is tested
// on the host. Register framing (when a reply is complete) is the caller's job, exactly as the
// transport contract states -- expectedReadResponseLength() exists to help with that.

#pragma once

#include <cstddef>
#include <cstdint>

namespace heliograph::modbus {

// The function codes this bridge needs. Reads first: every driver starts read-only, and
// battery control is a small set of holding-register writes on top.
inline constexpr uint8_t kReadHoldingRegisters   = 0x03;
inline constexpr uint8_t kReadInputRegisters     = 0x04;
inline constexpr uint8_t kWriteSingleRegister    = 0x06;
inline constexpr uint8_t kWriteMultipleRegisters = 0x10;

/// A device signals an error by echoing the function code with the high bit set and appending
/// a one-byte exception code. This bit is how a reply is recognised as an exception.
inline constexpr uint8_t kExceptionFlag = 0x80;

/// Max Modbus RTU ADU: 1 unit id + 253 PDU + 2 CRC.
inline constexpr size_t kMaxAdu = 256;
/// A single request may not ask for more than 125 holding/input registers (0x7D): the byte
/// count that carries them back is a single byte, and 125*2 = 250 fits it.
inline constexpr uint16_t kMaxReadRegisters  = 125;
/// Write-multiple is capped at 123 registers for the same single-byte reason on the way out.
inline constexpr uint16_t kMaxWriteRegisters = 123;

/// Modbus CRC-16 (polynomial 0xA001, initial 0xFFFF). Transmitted low byte first.
uint16_t crc16(const uint8_t* data, size_t len);

enum class BuildResult : uint8_t {
    Ok,
    BufferTooSmall,
    InvalidQuantity,  ///< zero, or above the per-function register cap
};

/// Read holding (0x03) or input (0x04) registers. `functionCode` must be one of those two.
BuildResult buildReadRequest(uint8_t unitId, uint8_t functionCode, uint16_t startAddress,
                             uint16_t quantity, uint8_t* out, size_t capacity, size_t& outLen);

/// Write one holding register (0x06).
BuildResult buildWriteSingleRegister(uint8_t unitId, uint16_t address, uint16_t value,
                                     uint8_t* out, size_t capacity, size_t& outLen);

/// Write a contiguous block of holding registers (0x10). `values` is `count` registers, host
/// order; they go on the wire big-endian.
BuildResult buildWriteMultipleRegisters(uint8_t unitId, uint16_t startAddress,
                                        const uint16_t* values, uint16_t count, uint8_t* out,
                                        size_t capacity, size_t& outLen);

/// Bytes a well-formed read reply of `quantity` registers occupies: unit + function + byte
/// count + 2*quantity data + 2 CRC. An exception reply is always 5 bytes and shorter, so a
/// caller that has read this many bytes has either the full reply or an exception.
size_t expectedReadResponseLength(uint16_t quantity);

enum class ParseResult : uint8_t {
    Ok,
    Incomplete,     ///< fewer bytes than the frame needs; read more and retry
    BadCrc,
    WrongUnit,      ///< reply from a different unit id than addressed
    WrongFunction,  ///< function code echoed does not match the request
    Exception,      ///< device returned an exception; see ReadResponse::exceptionCode
    Malformed,      ///< byte count inconsistent with the frame
};

struct ReadResponse {
    uint8_t unitId        = 0;
    uint8_t functionCode  = 0;
    uint8_t registerCount = 0;  ///< registers decoded into the caller's buffer
    uint8_t exceptionCode = 0;  ///< meaningful only when ParseResult::Exception
};

/// Validates a read reply and decodes its registers (big-endian) into `regsOut`. Checks, in
/// order: length, CRC, unit id, exception flag, function code, byte-count consistency. Nothing
/// is written to `regsOut` unless the result is Ok.
ParseResult parseReadResponse(const uint8_t* buf, size_t len, uint8_t expectedUnit,
                              uint8_t expectedFunction, uint16_t* regsOut, size_t regsCapacity,
                              ReadResponse& out);

struct WriteResponse {
    uint8_t  unitId        = 0;
    uint8_t  functionCode  = 0;
    uint16_t address       = 0;  ///< echoed start address
    uint16_t value         = 0;  ///< 0x06: written value; 0x10: register count
    uint8_t  exceptionCode = 0;  ///< meaningful only when ParseResult::Exception
};

/// Validates the echo a device returns for 0x06 / 0x10. A write is only confirmed when this
/// returns Ok and the echoed address (and, for 0x06, value) match what was sent -- checking
/// that is the caller's responsibility, since only the caller knows what it asked for.
ParseResult parseWriteResponse(const uint8_t* buf, size_t len, uint8_t expectedUnit,
                               uint8_t expectedFunction, WriteResponse& out);

}  // namespace heliograph::modbus
