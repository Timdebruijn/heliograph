// SPDX-License-Identifier: MIT
//
// PMU-family RS485 protocol — framing layer, shared by every driver that speaks it.
//
// The AA 55 "PMU" protocol is a family: several legacy single-phase inverter brands speak
// byte-for-byte the same framing (header, 2+2 byte addressing, control/function codes,
// 16-bit sum checksum, response = function | 0x80) with brand-specific *payloads*. This
// file is the shared framing only; payload decoding stays in each driver under
// src/drivers/, because that is where brand knowledge belongs — which is also why no brand
// is named here.
//
// The protocol knowledge was re-implemented from community references and vendor protocol
// documents; no code was copied. Provenance and licensing per source:
// LICENSE-THIRD-PARTY.md. Protocol description: the protocol notes under docs/.
//
// This translation unit is platform independent: it must never include Arduino or
// ESP-IDF headers, so that it can be tested on the host (env:native).

#pragma once

#include <cstddef>
#include <cstdint>

namespace heliograph::pmu {

inline constexpr uint8_t kHeader0 = 0xAA;
inline constexpr uint8_t kHeader1 = 0x55;

// 2 header + 2 source + 2 destination + 1 control + 1 function + 1 length + 2 checksum
inline constexpr size_t kFrameOverhead = 11;
inline constexpr size_t kMaxDataLength = 255;  // length field is a single byte
inline constexpr size_t kMaxFrameSize  = kFrameOverhead + kMaxDataLength;

// Offsets within a frame.
inline constexpr size_t kOffsetSourceHigh      = 2;
inline constexpr size_t kOffsetDestinationHigh = 4;
inline constexpr size_t kOffsetControl         = 6;
inline constexpr size_t kOffsetFunction        = 7;
inline constexpr size_t kOffsetLength          = 8;
inline constexpr size_t kOffsetData            = 9;

// The number of bytes that must be present before the length field can be read.
inline constexpr size_t kMinBytesForLength = kOffsetLength + 1;

struct Address {
    uint8_t high = 0;
    uint8_t low  = 0;

    friend constexpr bool operator==(const Address& a, const Address& b) {
        return a.high == b.high && a.low == b.low;
    }
    friend constexpr bool operator!=(const Address& a, const Address& b) { return !(a == b); }
};

// The PMU (this bridge) always identifies as 01 00; inverters live at 00 <addr>.
inline constexpr Address kPmuAddress{0x01, 0x00};
inline constexpr Address kBroadcastAddress{0x00, 0x00};

// The PMU hands out inverter addresses itself. This is not a fixed slave address like
// Modbus: it is assigned during registration and forgotten by the inverter when it loses
// power. Which address a driver assigns is brand convention (reference implementations
// use 0x10 or 0x0A) and lives in each driver's options; this is only the family default.
inline constexpr uint8_t kFirstInverterAddress = 0x10;

inline constexpr Address inverterAddress(uint8_t addr) { return Address{0x00, addr}; }

// Control codes 0x12 (WRITE) and 0x13 (EXECUTE) exist in the protocol but no function
// codes for them are known, so no write operation can be expressed. This is why the
// PMU-family drivers are read-only by nature rather than by MVP scope.
struct CommandCode {
    uint8_t control  = 0;
    uint8_t function = 0;

    // Responses set bit 7 of the function code (0x02 -> 0x82).
    constexpr uint8_t responseFunction() const { return static_cast<uint8_t>(function | 0x80); }
};

namespace cmd {
// Control code 0x10 — REGISTER
inline constexpr CommandCode kOfflineQuery{0x10, 0x00};  // -> serial number (ASCII)
inline constexpr CommandCode kSendAddress{0x10, 0x01};   // -> 1 byte ACK (kRegisterAck)
inline constexpr CommandCode kReRegister{0x10, 0x04};    // broadcast, no response
// Control code 0x11 — READ
inline constexpr CommandCode kQueryDescription{0x11, 0x00};
inline constexpr CommandCode kQueryNormalInfo{0x11, 0x02};  // -> measurement payload
inline constexpr CommandCode kQueryInverterId{0x11, 0x03};  // -> device id payload
}  // namespace cmd

inline constexpr uint8_t kRegisterAck = 0x06;

/// Sum of all bytes, truncated to 16 bits. Not a CRC.
///
/// The reference implementation additionally rejects any frame whose checksum is 0, to stop
/// an all-zero frame from validating (0 == 0) and being published as a real zero reading. We
/// do not carry that guard over: it only exists because the reference never checks the AA 55
/// header, and header validation rejects all-zero frames strictly earlier. Keeping it would
/// also wrongly reject the ~1-in-65536 legitimate frame whose byte sum wraps to exactly 0.
uint16_t checksum(const uint8_t* data, size_t len);

enum class BuildResult {
    Ok,
    DataTooLong,     ///< dataLen exceeds kMaxDataLength
    BufferTooSmall,  ///< out cannot hold the frame
};

/// Builds a request from the PMU (source 01 00) to `destination`. `out` needs
/// kFrameOverhead + dataLen bytes.
BuildResult buildRequest(CommandCode command,
                         Address     destination,
                         const uint8_t* data,
                         size_t         dataLen,
                         uint8_t*       out,
                         size_t         outCapacity,
                         size_t&        outLength);

/// Like buildRequest, but with an explicit source address. One brand's reference
/// implementation sends the SEND_ADDRESS registration frame with source 00 00 rather than
/// the PMU address; every other request uses the plain buildRequest above.
BuildResult buildRequestFrom(Address     source,
                             CommandCode command,
                             Address     destination,
                             const uint8_t* data,
                             size_t         dataLen,
                             uint8_t*       out,
                             size_t         outCapacity,
                             size_t&        outLength);

enum class ParseResult {
    Ok,
    Incomplete,       ///< need more bytes; not an error
    BadHeader,        ///< does not start with AA 55
    BadChecksum,      ///< checksum mismatch
    WrongDestination, ///< not addressed to the PMU
    WrongSource,      ///< not from the expected inverter
    UnexpectedResponse, ///< control/function code is not the expected response
    BadPayloadLength, ///< payload length not valid for this response type
};

struct Frame {
    Address        source;
    Address        destination;
    uint8_t        control  = 0;
    uint8_t        function = 0;
    const uint8_t* data     = nullptr;  ///< points into the caller's buffer
    size_t         dataLength = 0;
    size_t         frameLength = 0;     ///< total bytes consumed, = kFrameOverhead + dataLength
};

/// Total frame length implied by the length byte, or 0 if `len` is too short to tell.
/// Lets the transport read exactly the right number of bytes instead of guessing.
size_t expectedFrameLength(const uint8_t* buf, size_t len);

/// Parses one frame from the front of `buf`. Performs structural and checksum validation
/// only; addressing and response matching are checked by validateResponse().
ParseResult parseFrame(const uint8_t* buf, size_t len, Frame& out);

/// Checks that `frame` is the response to `expected` coming from `expectedSource`.
ParseResult validateResponse(const Frame& frame, CommandCode expected, Address expectedSource);

}  // namespace heliograph::pmu
