// SPDX-License-Identifier: MIT
//
// Virtual Modbus register map.
//
// This map is entirely ours. The inverter does not speak Modbus; the driver decodes whatever
// it does speak, and this renders the canonical state into registers. Nothing here knows
// which driver produced the state -- a three-phase hybrid and a single-phase string inverter
// go through exactly the same code.
//
// Platform independent: pure conversion, no eModbus and no Arduino. The wiring to an actual
// TCP socket lives in modbus_tcp_server.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "device/bridge_info.h"
#include "device/device_state.h"
#include "diagnostics/diagnostics.h"

namespace heliograph::modbus {

/// Bump on any breaking change to the layout. Published at register 0 so a client can refuse
/// to guess.
inline constexpr uint32_t kSchemaVersion = 1;

inline constexpr size_t kRegisterCount = 900;

/// Sentinels for "not a real measurement". Never 0: a solar inverter genuinely produces 0 W
/// at night, and a client must be able to tell that apart from "unknown".
inline constexpr uint32_t kFloatNaNBits = 0x7FC00000u;
inline constexpr uint16_t kInvalidU16   = 0xFFFFu;
inline constexpr uint32_t kInvalidU32   = 0xFFFFFFFFu;

namespace reg {
// --- core (0-99) ---
inline constexpr uint16_t kSchemaVersionAddr = 0;    // uint32
inline constexpr uint16_t kBridgeOnline      = 2;    // uint16
inline constexpr uint16_t kInverterOnline    = 3;    // uint16
inline constexpr uint16_t kDataValid         = 4;    // uint16
inline constexpr uint16_t kDataStale         = 5;    // uint16
inline constexpr uint16_t kStatusCode        = 6;    // uint16
inline constexpr uint16_t kErrorCode         = 7;    // uint16
inline constexpr uint16_t kAcPowerTotal      = 10;   // float32
inline constexpr uint16_t kAcL1Voltage       = 12;   // float32
inline constexpr uint16_t kAcL1Current       = 14;   // float32
inline constexpr uint16_t kAcFrequency       = 16;   // float32
inline constexpr uint16_t kDcPowerTotal      = 20;   // float32
inline constexpr uint16_t kDcMppt1Voltage    = 22;   // float32
inline constexpr uint16_t kDcMppt1Current    = 24;   // float32
inline constexpr uint16_t kTemperature       = 30;   // float32
inline constexpr uint16_t kEnergyToday       = 40;   // float32
inline constexpr uint16_t kEnergyTotal       = 42;   // float32
inline constexpr uint16_t kOperatingHours    = 44;   // uint32
inline constexpr uint16_t kSecondsSincePoll  = 50;   // uint32
inline constexpr uint16_t kPollSuccessTotal  = 52;   // uint32
inline constexpr uint16_t kPollFailureTotal  = 54;   // uint32
inline constexpr uint16_t kChecksumErrors    = 56;   // uint32
inline constexpr uint16_t kRs485Timeouts     = 58;   // uint32
inline constexpr uint16_t kWifiRssi          = 60;   // int32
inline constexpr uint16_t kBridgeUptime      = 62;   // uint32

// --- AC phases (100-199), 20 registers apart ---
inline constexpr uint16_t kPhaseBase   = 100;
inline constexpr uint16_t kPhaseStride = 20;
inline constexpr uint16_t kPhaseVoltageOffset = 0;
inline constexpr uint16_t kPhaseCurrentOffset = 2;
inline constexpr uint16_t kPhasePowerOffset   = 4;

// --- DC / MPPT (200-299), 20 registers apart ---
inline constexpr uint16_t kMpptBase   = 200;
inline constexpr uint16_t kMpptStride = 20;
inline constexpr uint16_t kMpptVoltageOffset = 0;
inline constexpr uint16_t kMpptCurrentOffset = 2;
inline constexpr uint16_t kMpptPowerOffset   = 4;

// --- battery (300-399) ---
inline constexpr uint16_t kBatterySoc           = 300;  // float32
inline constexpr uint16_t kBatteryVoltage       = 302;  // float32
inline constexpr uint16_t kBatteryChargePower   = 304;  // float32
inline constexpr uint16_t kBatteryDischargePower = 306; // float32

// --- grid meter (400-499) ---
inline constexpr uint16_t kGridImportPower = 400;  // float32
inline constexpr uint16_t kGridExportPower = 402;  // float32

// --- status (500-599) ---
inline constexpr uint16_t kStatusCodeMirror   = 500;  // uint16
inline constexpr uint16_t kErrorCodeMirror    = 501;  // uint16
inline constexpr uint16_t kConsecutiveFailures = 502; // uint16
inline constexpr uint16_t kStatusText         = 510;  // 16 regs / 32 chars

// --- capabilities (600-699) ---
inline constexpr uint16_t kCapabilitiesRead  = 600;  // 4 regs, 64 bits
inline constexpr uint16_t kCapabilitiesWrite = 604;  // 4 regs, 64 bits
inline constexpr uint16_t kValidityBitmap    = 610;  // 8 regs, 128 bits
inline constexpr uint16_t kPhaseCount        = 620;  // uint16
inline constexpr uint16_t kMpptCount         = 621;  // uint16
inline constexpr uint16_t kBatteryPresent    = 622;  // uint16
inline constexpr uint16_t kDriverReadOnly    = 623;  // uint16

// --- identity strings (700-799) ---
inline constexpr uint16_t kManufacturer     = 700;  // 16 regs
inline constexpr uint16_t kModel            = 716;  // 16 regs
inline constexpr uint16_t kSerialNumber     = 732;  // 16 regs
inline constexpr uint16_t kFirmwareVersion  = 748;  // 8 regs
inline constexpr uint16_t kDriverId         = 756;  // 8 regs
inline constexpr uint16_t kBridgeFirmware   = 764;  // 8 regs

// --- bridge diagnostics (800-899) ---
inline constexpr uint16_t kDiagUptime        = 800;  // uint32
inline constexpr uint16_t kDiagFreeHeap      = 802;  // uint32
inline constexpr uint16_t kDiagMinFreeHeap   = 804;  // uint32
inline constexpr uint16_t kDiagResetReason   = 806;  // uint16
inline constexpr uint16_t kDiagWifiRssi      = 807;  // uint16 (int16 bits)
inline constexpr uint16_t kDiagWifiReconnect = 810;  // uint32
inline constexpr uint16_t kDiagMqttReconnect = 812;  // uint32
inline constexpr uint16_t kDiagModbusClients = 814;  // uint32
inline constexpr uint16_t kDiagRestRequests  = 816;  // uint32
inline constexpr uint16_t kDiagInvalidFrames = 818;  // uint32
inline constexpr uint16_t kDiagFirmwareMajor = 820;  // uint16
inline constexpr uint16_t kDiagFirmwareMinor = 821;  // uint16
inline constexpr uint16_t kDiagFirmwarePatch = 822;  // uint16
}  // namespace reg

/// Bit positions in the validity bitmap at reg::kValidityBitmap.
///
/// This ordering is part of schema version 1 and must not change within it: clients index
/// into it by number. Append only, and bump kSchemaVersion if anything moves.
enum class ValidityBit : uint8_t {
    AcPowerTotal = 0,
    AcL1Voltage  = 1,
    AcL1Current  = 2,
    AcFrequency  = 3,
    DcPowerTotal = 4,
    DcMppt1Voltage = 5,
    DcMppt1Current = 6,
    Temperature    = 7,
    EnergyToday    = 8,
    EnergyTotal    = 9,
    OperatingHours = 10,
    StatusCode     = 11,
    ErrorCode      = 12,
    AcL1Power      = 13,
    AcL2Voltage    = 14,
    AcL2Current    = 15,
    AcL2Power      = 16,
    AcL3Voltage    = 17,
    AcL3Current    = 18,
    AcL3Power      = 19,
    DcMppt1Power   = 20,
    DcMppt2Voltage = 21,
    DcMppt2Current = 22,
    DcMppt2Power   = 23,
    BatterySoc     = 24,
    BatteryVoltage = 25,
    BatteryChargePower    = 26,
    BatteryDischargePower = 27,
    GridImportPower = 28,
    GridExportPower = 29,
    _Count
};

class RegisterMap {
public:
    RegisterMap();

    /// Renders a state snapshot into registers. `nowMs` drives the "seconds since last valid
    /// poll" register.
    void update(const DeviceState& state, const BridgeInfo& bridge,
                const DiagnosticsSnapshot& diagnostics, uint64_t nowMs);

    /// Reads `count` registers starting at `address`. Returns false when the range falls
    /// outside the map, which the server turns into exception 0x02.
    bool read(uint16_t address, uint16_t count, uint16_t* out) const;

    uint16_t at(uint16_t address) const;

    /// Address space size, for range checks.
    static constexpr size_t size() { return kRegisterCount; }

    bool validityBit(ValidityBit bit) const;

private:
    void writeU16(uint16_t address, uint16_t value);
    void writeU32(uint16_t address, uint32_t value);
    void writeI32(uint16_t address, int32_t value);
    /// Writes a float, or NaN when `valid` is false.
    void writeFloat(uint16_t address, double value, bool valid);
    void writeString(uint16_t address, const std::string& value, size_t maxRegisters);
    void writeBitset64(uint16_t address, const std::bitset<kCapabilityCount>& bits);
    void setValidity(ValidityBit bit, bool valid);

    /// Publishes one measurement plus its validity bit, honouring supported/valid/stale.
    void publishMeasurement(const DeviceState& state, const char* id, uint16_t address,
                            ValidityBit bit);

    uint16_t registers_[kRegisterCount] = {};
};

}  // namespace heliograph::modbus
