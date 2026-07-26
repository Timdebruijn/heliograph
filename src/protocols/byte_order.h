// SPDX-License-Identifier: MIT
//
// Reading and writing multi-byte integers out of a wire buffer.
//
// Every protocol here packs its numbers big-endian on the wire, and four separate places had
// written their own shifts to unpack them: two vendor payload parsers, the Modbus RTU codec
// (getBe16/putBe16, plus a hand-inlined little-endian read for the CRC field) and the PMU
// framing (a shift expression spelled out at the checksum). Identical arithmetic, four
// vocabularies, and a name like `u16At` that does not say which end comes first.
//
// Host-compilable and brand-free by construction: no Arduino, no ESP-IDF, no driver knowledge.
// It sits in src/protocols/ because that is the one place both the drivers and the codecs
// already depend on.
//
// Offsets are in BYTES. A protocol that indexes by 16-bit register multiplies by two at its own
// call site, where the register number is the meaningful unit.

#pragma once

#include <cstddef>
#include <cstdint>

namespace heliograph::bytes {

/// Unsigned 16-bit, most significant byte first.
inline uint16_t be16(const uint8_t* data, size_t offset = 0) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
}

/// Signed 16-bit, most significant byte first. Two's complement, which every protocol here uses.
inline int16_t be16s(const uint8_t* data, size_t offset = 0) {
    return static_cast<int16_t>(be16(data, offset));
}

/// Unsigned 16-bit, least significant byte first. Rare: Modbus RTU frames are big-endian
/// throughout EXCEPT their trailing CRC, which goes out low byte first.
inline uint16_t le16(const uint8_t* data, size_t offset = 0) {
    return static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8 | data[offset]);
}

/// Unsigned 32-bit, most significant byte first.
inline uint32_t be32(const uint8_t* data, size_t offset = 0) {
    return (static_cast<uint32_t>(be16(data, offset)) << 16) | be16(data, offset + 2);
}

/// Unsigned 32-bit, least significant byte first.
inline uint32_t le32(const uint8_t* data, size_t offset = 0) {
    return (static_cast<uint32_t>(le16(data, offset + 2)) << 16) | le16(data, offset);
}

/// Writes an unsigned 16-bit value most significant byte first. Caller guarantees two bytes.
inline void putBe16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(value & 0xFF);
}

/// Writes an unsigned 16-bit value least significant byte first. Caller guarantees two bytes.
inline void putLe16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

}  // namespace heliograph::bytes
