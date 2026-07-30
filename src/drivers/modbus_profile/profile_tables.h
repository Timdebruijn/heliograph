// SPDX-License-Identifier: MIT
//
// Register-map tables — the vendor knowledge, held as data so a new inverter family is a new
// table rather than a new driver.
//
// Platform independent: no Arduino/ESP-IDF, so the register→canonical mapping is host-tested.
// The IO (Modbus transactions) lives in modbus_profile_driver.cpp; this file only says how a
// raw register word becomes a canonical measurement.
//
// The tables themselves are NOT hand-written: they are generated at build time from the TOML
// device profiles in profiles/<vendor>/ (tools/gen_profiles.py → profiles_generated.cpp).
// Adding a model is a TOML file, not C++ — see docs/adding-a-device.md.

#pragma once

#include <cstddef>
#include <cstdint>

#include "device/capability.h"
#include "device/measurement.h"
#include "transport/serial_profile.h"

namespace heliograph::profile {

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
    double          scale;       ///< raw * scale + offset = value
    bool            isSigned;    ///< interpret the raw integer as two's-complement
    /// Added to the scaled value. Zero for almost every register, and not zero for the ones that
    /// matter: several vendors store temperature biased so it never goes negative on the wire,
    /// reporting 1000 for 0 °C. Without this the only options were publishing 100 °C or not
    /// publishing temperature at all, and temperature is not an optional channel.
    ///
    /// Defaulted so it can be appended here without touching hand-written aggregate initialisers.
    double offset = 0.0;
};

/// A contiguous register range to read in one Modbus transaction. Kept small and explicit so a
/// profile never asks for more than the 125-register limit or reads registers it does not use.
struct RegBlock {
    RegSpace space;
    uint16_t start;
    uint16_t count;
    /// A block read only to answer a bring-up question, mapping nothing. Its silence is a fact
    /// about the register map, not about the wire, so it must not move the RS485 bus counters:
    /// otherwise a profile probing a range this model does not implement would put a permanent
    /// slope on the timeout metric of a perfectly wired installation -- ~360 an hour at the
    /// default poll interval, forever, on hardware with nothing wrong with it.
    bool probe = false;
};

/// A writable numeric setpoint register, declared in a profile's [[write]] section.
///
/// A profile row alone must never make a device writable. `verified` must also be true --
/// hardware-confirmed on a real device, per row -- so the table can hold register research that
/// has been recorded and reviewed but not yet proven. No profile in the tree sets it today.
///
/// The driver's write path itself exists: execute() writes one holding register over FC06 and
/// checks the device's echo. What it deliberately cannot do (32-bit setpoints, FC16, enum modes)
/// is in docs/device-profiles/write-path.md, together with why.
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

struct DeviceProfile {
    const char* id;           ///< stable, e.g. "sph"
    const char* displayName;  ///< e.g. "SPH (3-6 kW)"
    /// The vendor this map belongs to, declared per profile rather than per driver: one driver
    /// serves many brands, so this is the only place that can honestly answer "what is it".
    /// Reported as the device identity, which is what Home Assistant shows.
    const char* manufacturer;
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
const DeviceProfile* findProfile(const char* id);

/// The profile marked `default = true` in its TOML file — used when the `profile` driver
/// option is not set. Implemented in profiles_generated.cpp.
const DeviceProfile& defaultProfile();

/// Enumerates the compiled-in profiles, so the descriptor can offer them as the option's
/// allowed values instead of accepting free-form text. With more than one profile in the
/// build, a typo that falls back to the default silently applies ANOTHER family's register
/// map — readings that look plausible and are wrong. Rejecting the value at configuration
/// time is the only point where that is still visible to the user.
/// Both implemented in profiles_generated.cpp.
size_t                profileCount();
const DeviceProfile& profileAt(size_t index);

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
void applyProfile(const DeviceProfile& profile, const BlockData* blocks, size_t blockCount,
                  MeasurementSet& measurements, uint64_t ts);

}  // namespace heliograph::profile
