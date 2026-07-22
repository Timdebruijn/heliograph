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

struct InverterCapabilities {
    std::bitset<kCapabilityCount> read;
    std::bitset<kCapabilityCount> write;
    // std::array, not std::map: a fixed set of at most kCommandTypeCount compile-time keys, and
    // this whole struct rides on every full DeviceState copy (twice per poll). A map is a
    // red-black tree with per-node heap allocations for that; the array is O(1), zero-alloc,
    // fixed-footprint. Indexed by static_cast<size_t>(InverterCommandType); the per-entry
    // `supported` flag replaces "is the key present". Default = all unsupported.
    std::array<NumericCapability, kCommandTypeCount> numeric{};

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
