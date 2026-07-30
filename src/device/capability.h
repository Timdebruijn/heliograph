// SPDX-License-Identifier: MIT
//
// Capability model. This is the only gate between drivers and output adapters: an output
// asks "can this device do X?" and never "which driver is this?".

#pragma once

#include <array>
#include <bitset>
#include <cstdint>

#include "measurement.h"

namespace heliograph {

enum class InverterCapability : uint8_t {
    ReadAcPower = 0,
    ReadAcVoltage,
    ReadAcCurrent,
    ReadGridFrequency,
    ReadDcPower,
    ReadDcVoltage,
    ReadDcCurrent,
    ReadEnergyToday,
    ReadEnergyTotal,
    ReadTemperature,
    ReadOperatingHours,
    ReadStatus,
    ReadErrors,
    ReadMultiplePhases,
    ReadMultipleMppts,
    ReadBatteryState,
    SetActivePowerLimit,
    SetExportLimit,
    StartStop,
    SetReactivePower,
    SetBatteryChargeLimit,
    SetBatteryDischargeLimit,
    SetBatteryOperatingMode,
    SetMinimumSoc,
    SetMaximumSoc,
    SynchronizeTime,
    _Count,
};

inline constexpr size_t kCapabilityCount = static_cast<size_t>(InverterCapability::_Count);

enum class InverterCommandType : uint8_t {
    SetActivePowerLimitPercent = 0,
    SetActivePowerLimitWatts,
    SetExportLimitWatts,
    Start,
    Stop,
    SetReactivePower,
    SetBatteryChargeLimitWatts,
    SetBatteryDischargeLimitWatts,
    SetBatteryOperatingMode,
    SetMinimumSoc,
    SetMaximumSoc,
    SynchronizeTime,
    _Count,
};

inline constexpr size_t kCommandTypeCount = static_cast<size_t>(InverterCommandType::_Count);

/// Stable snake_case name for a capability, used in the MQTT and REST capabilities payloads.
///
/// Lives here, next to the enum, rather than in one of the output adapters. It spent a while in
/// heliograph::mqtt, which meant the REST payload builder included outputs/mqtt/mqtt_payloads.h
/// for nothing else -- an output adapter reaching into a sibling adapter to name a value out of
/// the device model. Its two siblings, commandTypeName and unitSymbol, were always here.
const char* capabilityName(InverterCapability capability);

/// Bounds for a writable numeric property. Validated centrally by the dispatcher so that
/// every driver gets range checking without implementing it.
struct NumericCapability {
    bool   supported = false;
    bool   writable  = false;
    double minimum   = 0.0;
    double maximum   = 0.0;
    double step      = 0.0;
    Unit   unit      = Unit::None;
};

/// One selectable value of an enum setpoint -- a battery work mode, an EMS mode, a load-limit
/// selector. `value` is the raw number the device expects; `label` is what a human picks.
///
/// The two are kept together because neither is usable alone: a raw value nobody can name is not
/// a control surface, and a name with no value cannot be written.
struct EnumOption {
    int         value = 0;
    const char* label = nullptr;
};

/// The selectable values of one enum setpoint.
///
/// `options` POINTS at the driver's own compile-time table and is never owned here. That is
/// deliberate: this struct rides along on every DeviceState copy, and copying a variable-length
/// option list per poll -- for a set of values that is fixed at build time and lives in flash --
/// would be heap churn for nothing. The pointer stays valid because it targets static data.
struct EnumCapability {
    bool              supported   = false;
    bool              writable    = false;
    const EnumOption* options     = nullptr;
    size_t            optionCount = 0;

    /// Whether `value` is one of the declared options.
    ///
    /// This is the whole point of an enum setpoint: unlike a numeric range there is no
    /// interpolating between valid values, and a mode number the device does not implement is
    /// not a smaller mistake than one outside a range -- it is a register set to something
    /// nobody has ever tested. An empty option list therefore accepts nothing.
    bool accepts(int value) const;
};

struct InverterCapabilities {
    std::bitset<kCapabilityCount> read;
    std::bitset<kCapabilityCount> write;
    // std::array, not std::map: a fixed set of at most kCommandTypeCount compile-time keys, and
    // this whole struct rides on every full DeviceState copy (twice per poll). A map is a
    // red-black tree with per-node heap allocations for that; the array is O(1), zero-alloc,
    // fixed-footprint. Indexed by static_cast<size_t>(InverterCommandType); the per-entry
    // `supported` flag replaces "is the key present". Default = all unsupported.
    std::array<NumericCapability, kCommandTypeCount> numeric{};
    /// Same shape and same reasoning as `numeric`, for the setpoints that select from a list
    /// rather than moving along a range. Only the option POINTER travels here (see
    /// EnumCapability), so this costs a fixed handful of bytes per command type rather than a
    /// copy of every mode name on every poll.
    std::array<EnumCapability, kCommandTypeCount> enums{};

    uint8_t phaseCount = 1;
    uint8_t mpptCount  = 1;
    bool    hasBattery = false;

    bool has(InverterCapability c) const { return read.test(static_cast<size_t>(c)); }
    bool canWrite(InverterCapability c) const { return write.test(static_cast<size_t>(c)); }
    void addRead(InverterCapability c) { read.set(static_cast<size_t>(c)); }
    void addWrite(InverterCapability c) { write.set(static_cast<size_t>(c)); }

    /// True when the device exposes no write operation at all. Outputs use this to decide
    /// whether to expose any control surface — not a driver id check.
    bool isReadOnly() const { return write.none(); }
};

}  // namespace heliograph
