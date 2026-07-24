// SPDX-License-Identifier: MIT
//
// SunSpec decoding: the model chain, and the inverter models on it.
//
// SunSpec is not a vendor map, it is a published standard, and a device describes its own
// layout at runtime: a "SunS" marker, then a chain of {model id, length, payload} blocks
// terminated by 0xFFFF. That is why this driver has no register table of its own -- the table
// arrives from the device. What IS fixed, and lives here, is the layout WITHIN each standard
// model, transcribed from the official SunSpec model definitions (sunspec/models).
//
// Pure and transport-free, like the other parsers: the driver does the reading, this decides
// what the registers mean. Host-tested in test_sunspec_parser.
//
// Two SunSpec rules matter more than any offset, and both exist to stop a reading being
// invented:
//
//   * Every measurement carries a SCALE FACTOR in a separate register: value = raw * 10^sf,
//     where sf is a SIGNED int16. Reading the raw register alone gives an answer that is
//     wrong by orders of magnitude, silently.
//   * Points that a device does not implement carry a defined sentinel (0xFFFF for uint16,
//     0x8000 for int16 and for a scale factor). Those are NOT zero readings; they mean the
//     device has nothing to say, and this decoder reports them as absent.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace heliograph::sunspec {

/// "SunS", the marker that identifies a SunSpec map. Two registers.
inline constexpr uint16_t kMarkerHigh = 0x5375;  // "Su"
inline constexpr uint16_t kMarkerLow  = 0x6e53;  // "nS"

/// Terminates the model chain.
inline constexpr uint16_t kEndOfChain = 0xFFFF;

/// Common model: manufacturer, model, version, serial. Always the first block after the marker.
inline constexpr uint16_t kModelCommon = 1;

/// Inverter models. Single phase, split phase, three phase -- identical point layout,
/// verified against the official definitions; only which phase points carry meaning differs.
inline constexpr uint16_t kModelInverterSinglePhase = 101;
inline constexpr uint16_t kModelInverterSplitPhase  = 102;
inline constexpr uint16_t kModelInverterThreePhase  = 103;

bool isInverterModel(uint16_t modelId);

/// Not-implemented sentinels, straight from the specification.
inline constexpr uint16_t kNotImplementedU16 = 0xFFFF;
inline constexpr uint16_t kNotImplementedS16 = 0x8000;  // also the not-implemented scale factor

/// A decoded scale factor. `valid` is false when the device published the not-implemented
/// sentinel, in which case every value depending on it must be dropped rather than published
/// unscaled -- an unscaled reading is not a slightly-off reading, it is a wrong one.
struct ScaleFactor {
    bool  valid = false;
    int   exponent = 0;
};

ScaleFactor decodeScaleFactor(uint16_t raw);

/// raw * 10^exponent, for the small exponents SunSpec uses. Returns false when the raw value
/// is the not-implemented sentinel for its type, or the scale factor is unusable.
bool applyScale(uint16_t raw, bool isSigned, const ScaleFactor& sf, double& out);

/// 32-bit accumulator (two registers, high word first). SunSpec defines 0 as "not accumulated
/// yet" for these, which is a real reading of zero on a brand-new device -- so it is passed
/// through, and only the scale factor can make it absent.
bool applyScaleAcc32(uint16_t high, uint16_t low, const ScaleFactor& sf, double& out);

/// Reads a SunSpec string point: big-endian register pairs, NUL padded. Trailing spaces and
/// NULs are trimmed.
std::string decodeString(const uint16_t* regs, size_t registerCount);

/// Offsets of the points this driver reads, relative to the START of a model block (so offset
/// 0 is the model id and offset 1 is its length, matching the official definitions).
namespace inverter {
inline constexpr size_t kAphA   = 3;
inline constexpr size_t kA_SF   = 6;
inline constexpr size_t kPhVphA = 10;
inline constexpr size_t kV_SF   = 13;
inline constexpr size_t kW      = 14;
inline constexpr size_t kW_SF   = 15;
inline constexpr size_t kHz     = 16;
inline constexpr size_t kHz_SF  = 17;
inline constexpr size_t kWH     = 24;  // acc32, two registers
inline constexpr size_t kWH_SF  = 26;
inline constexpr size_t kDCW    = 31;
inline constexpr size_t kDCW_SF = 32;
inline constexpr size_t kTmpCab = 33;
inline constexpr size_t kTmp_SF = 37;
inline constexpr size_t kSt     = 38;
/// Every point above must exist before the block can be decoded.
inline constexpr size_t kMinRegisters = 39;
}  // namespace inverter

namespace common {
inline constexpr size_t kMn = 2;   // manufacturer, 16 registers
inline constexpr size_t kMd = 18;  // model, 16
inline constexpr size_t kVr = 42;  // version, 8
inline constexpr size_t kSN = 50;  // serial, 16
inline constexpr size_t kMinRegisters = 66;
}  // namespace common

/// What one inverter model block yielded. A field is only present when the device published
/// both a real value and a usable scale factor for it.
struct InverterReadings {
    bool   hasAcPower = false;
    double acPowerW   = 0;
    bool   hasAcVoltage = false;
    double acVoltageV   = 0;
    bool   hasAcCurrent = false;
    double acCurrentA   = 0;
    bool   hasFrequency = false;
    double frequencyHz  = 0;
    bool   hasEnergyTotal = false;
    double energyTotalKwh = 0;  ///< SunSpec reports Wh; converted here to the canonical kWh
    bool   hasDcPower = false;
    double dcPowerW   = 0;
    bool   hasTemperature = false;
    double temperatureC   = 0;
    bool     hasState = false;
    uint16_t state    = 0;
};

/// Decodes an inverter model block. `regs` starts at the model id. Returns false when the
/// block is too short to be one -- a truncated read must never be decoded from whatever
/// happened to be in the buffer.
bool decodeInverter(const uint16_t* regs, size_t count, InverterReadings& out);

struct CommonIdentity {
    std::string manufacturer;
    std::string model;
    std::string version;
    std::string serial;
};

bool decodeCommon(const uint16_t* regs, size_t count, CommonIdentity& out);

}  // namespace heliograph::sunspec
