// SPDX-License-Identifier: MIT
//
// Growatt register map — the brand knowledge, table-driven so a second family (MIN TL-XH,
// MOD, MIC) is a new table rather than a new driver. See docs/growatt-sph-protocol.md.
//
// Platform independent: no Arduino/ESP-IDF, so the register→canonical mapping is host-tested.
// The IO (Modbus transactions) lives in growatt_driver.cpp; this file only says how a raw
// register word becomes a canonical measurement.
//
// The tables themselves are NOT hand-written: they are generated at build time from the TOML
// device profiles in profiles/growatt/ (tools/gen_profiles.py → profiles_generated.cpp).
// Adding a Growatt family is a TOML file, not C++ — see docs/adding-a-device.md.

#pragma once

#include <cstddef>
#include <cstdint>

#include "device/capability.h"
#include "device/measurement.h"
#include "transport/serial_profile.h"

namespace heliograph::growatt {

enum class RegSpace : uint8_t { Input, Holding };

/// One register (or register pair) and the canonical measurement it feeds.
struct RegisterMapping {
    const char*     measurementId;
    MeasurementType type;
    Unit            unit;
    const char*     displayName;
    RegSpace        space;
    uint16_t        address;     ///< first register
    uint8_t         words;       ///< 1 = 16-bit, 2 = 32-bit (high word first)
    double          scale;       ///< raw * scale = value
    bool            isSigned;    ///< interpret the raw integer as two's-complement
};

/// A contiguous register range to read in one Modbus transaction. Kept small and explicit so a
/// profile never asks for more than the 125-register limit or reads registers it does not use.
struct RegBlock {
    RegSpace space;
    uint16_t start;
    uint16_t count;
};

/// A writable numeric setpoint register, declared in a profile's [[write]] section.
///
/// Schema-level support only. Two independent gates stand between a row here and a byte on
/// the bus: `verified` must be true (hardware-confirmed on a real device, per row), and the
/// driver must actually implement a write path (none does today — execute() returns
/// Unsupported). A profile row alone must never make a device writable; the table exists so
/// register research can be recorded and reviewed long before writing is enabled.
struct WriteMapping {
    InverterCommandType command;      ///< canonical numeric setpoint this register implements
    const char*         displayName;
    RegSpace            space;        ///< always Holding (validated; input regs are read-only)
    uint16_t            address;      ///< first register
    uint8_t             words;        ///< 1 = 16-bit, 2 = 32-bit (high word first)
    bool                useWriteMultiple;  ///< FC 0x10 even for one word (some firmwares demand it)
    double              scale;        ///< raw = value / scale
    double              minimum;      ///< bounds in canonical units, enforced by the dispatcher
    double              maximum;
    double              step;
    Unit                unit;
    bool                verified;     ///< confirmed on hardware; unverified rows stay dormant
};

struct GrowattProfile {
    const char* id;           ///< stable, e.g. "sph"
    const char* displayName;  ///< e.g. "Growatt SPH (3-6 kW)"
    bool        hasBattery;
    uint8_t     phaseCount;
    uint8_t     mpptCount;

    const RegBlock*        blocks;
    size_t                 blockCount;
    const RegisterMapping* mappings;
    size_t                 mappingCount;

    // Schema v2 additions (defaulted so older aggregate initializers stay valid).
    const WriteMapping* writes     = nullptr;  ///< dormant until verified + driver write path
    size_t              writeCount = 0;
    bool                supportsRtu = true;
    bool                supportsTcp = false;  ///< declared for future TCP transport; no consumer yet
    uint16_t            tcpPort     = 0;
    /// Line settings this family actually ships with, when the profile declares them.
    /// Overrides the driver descriptor's generic candidates once discovery consumes it.
    bool          hasSerial = false;
    SerialProfile serial{};
};

/// Looks up a profile by its stable id (e.g. "sph"). Returns nullptr when unknown, so a
/// config typo is a loud warning with a fallback rather than a silent wrong map.
/// Implemented in profiles_generated.cpp (build-time generated from profiles/growatt/).
const GrowattProfile* findProfile(const char* id);

/// The profile marked `default = true` in its TOML file — used when the `profile` driver
/// option is not set. Implemented in profiles_generated.cpp.
const GrowattProfile& defaultProfile();

/// Raw register data read back from the device, one entry per block.
struct BlockData {
    RegSpace space;
    uint16_t start;
    uint16_t count;
    uint16_t values[125];  ///< max registers per Modbus read
};

/// Looks up a single raw register across the read blocks. False when no block covers it, so a
/// mapping that points outside the declared blocks is caught rather than reading zero.
bool findRegister(const BlockData* blocks, size_t blockCount, RegSpace space, uint16_t address,
                  uint16_t& out);

/// Fills `measurements` from the raw blocks according to `profile`. Pure and host-tested: a
/// register whose block was not read is left undeclared, never published as zero. `ts` is the
/// poll timestamp stamped on each reading.
void applyProfile(const GrowattProfile& profile, const BlockData* blocks, size_t blockCount,
                  MeasurementSet& measurements, uint64_t ts);

}  // namespace heliograph::growatt
