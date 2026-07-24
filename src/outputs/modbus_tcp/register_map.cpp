// SPDX-License-Identifier: MIT

#include "register_map.h"

#include <cstring>
#include <string>

namespace heliograph::modbus {
namespace {

/// IEEE-754 float32 bit pattern, high word first (ABCD). Documented in
/// docs/modbus-register-map.md; changing it is a schema break.
uint32_t floatBits(float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float32 expected");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

}  // namespace

RegisterMap::RegisterMap() {
    // Everything starts as "unknown" rather than zero, so a client reading before the first
    // poll cannot mistake an empty map for a device sitting idle at 0 W.
    for (size_t i = 0; i < kRegisterCount; ++i) {
        registers_[i] = kInvalidU16;
    }
    writeU32(reg::kSchemaVersionAddr, kSchemaVersion);
}

void RegisterMap::writeU16(uint16_t address, uint16_t value) {
    if (address < kRegisterCount) {
        registers_[address] = value;
    }
}

void RegisterMap::writeU32(uint16_t address, uint32_t value) {
    if (static_cast<size_t>(address) + 1 < kRegisterCount) {
        registers_[address]     = static_cast<uint16_t>(value >> 16);  // high word first
        registers_[address + 1] = static_cast<uint16_t>(value & 0xFFFF);
    }
}

void RegisterMap::writeI32(uint16_t address, int32_t value) {
    writeU32(address, static_cast<uint32_t>(value));
}

void RegisterMap::writeFloat(uint16_t address, double value, bool valid) {
    if (!valid) {
        writeU32(address, kFloatNaNBits);
        return;
    }
    writeU32(address, floatBits(static_cast<float>(value)));
}

void RegisterMap::writeString(uint16_t address, const std::string& value, size_t maxRegisters) {
    // Two chars per register, big-endian, NUL padded. An empty string is unambiguously
    // "unknown": all zeros, which no valid identity string can be.
    for (size_t i = 0; i < maxRegisters; ++i) {
        const size_t  hi = i * 2;
        const size_t  lo = hi + 1;
        const uint8_t a  = hi < value.size() ? static_cast<uint8_t>(value[hi]) : 0;
        const uint8_t b  = lo < value.size() ? static_cast<uint8_t>(value[lo]) : 0;
        writeU16(static_cast<uint16_t>(address + i), static_cast<uint16_t>((a << 8) | b));
    }
}

void RegisterMap::writeBitset64(uint16_t address, const std::bitset<kCapabilityCount>& bits) {
    uint64_t v = 0;
    for (size_t i = 0; i < kCapabilityCount && i < 64; ++i) {
        if (bits.test(i)) {
            v |= (1ULL << i);
        }
    }
    writeU16(address, static_cast<uint16_t>((v >> 48) & 0xFFFF));
    writeU16(address + 1, static_cast<uint16_t>((v >> 32) & 0xFFFF));
    writeU16(address + 2, static_cast<uint16_t>((v >> 16) & 0xFFFF));
    writeU16(address + 3, static_cast<uint16_t>(v & 0xFFFF));
}

void RegisterMap::setValidity(ValidityBit bit, bool valid) {
    const size_t index    = static_cast<size_t>(bit);
    const uint16_t addr   = static_cast<uint16_t>(reg::kValidityBitmap + index / 16);
    const uint16_t mask   = static_cast<uint16_t>(1u << (index % 16));
    uint16_t       cur    = registers_[addr];
    if (valid) {
        cur |= mask;
    } else {
        cur &= static_cast<uint16_t>(~mask);
    }
    registers_[addr] = cur;
}

bool RegisterMap::validityBit(ValidityBit bit) const {
    const size_t   index = static_cast<size_t>(bit);
    const uint16_t addr  = static_cast<uint16_t>(reg::kValidityBitmap + index / 16);
    return (registers_[addr] & (1u << (index % 16))) != 0;
}

void RegisterMap::publishMeasurement(const DeviceState& state, const char* id, uint16_t address,
                                     ValidityBit bit) {
    const auto* m = state.measurements.find(id);
    // Unsupported by this driver, never read, or gone stale -> NaN plus a cleared bit. Both
    // signals always agree, so a client can use whichever it can handle.
    //
    // `m->supported` is strictly redundant here: MeasurementSet guarantees an unsupported
    // channel is never valid (declareUnsupported clears valid, and set() refuses values on
    // one), and Modbus renders both cases as NaN regardless. It is kept as depth, but be
    // aware it is not load-bearing -- removing it breaks no test, because it cannot change
    // the output. In MQTT and discovery the same flag *is* load-bearing, since there the
    // difference is a key or an entity existing at all.
    const bool usable = m != nullptr && m->supported && m->valid && !m->stale;
    writeFloat(address, usable ? m->value : 0.0, usable);
    setValidity(bit, usable);
}

void RegisterMap::update(const DeviceState& state, const BridgeInfo& bridge,
                         const DiagnosticsSnapshot& diagnostics, uint64_t nowMs) {
    writeU32(reg::kSchemaVersionAddr, kSchemaVersion);

    writeU16(reg::kBridgeOnline, bridge.bridgeOnline ? 1 : 0);
    writeU16(reg::kInverterOnline, state.inverterOnline ? 1 : 0);
    writeU16(reg::kDataValid, state.dataValid ? 1 : 0);
    writeU16(reg::kDataStale, state.dataStale ? 1 : 0);

    // Status is only meaningful while the data is valid; otherwise it is last night's value.
    const bool statusUsable = state.dataValid && !state.dataStale;
    writeU16(reg::kStatusCode, statusUsable ? state.statusCode : kInvalidU16);
    writeU16(reg::kStatusCodeMirror, statusUsable ? state.statusCode : kInvalidU16);
    setValidity(ValidityBit::StatusCode, statusUsable);

    // A driver whose protocol has no error code field must not publish 0 here: 0 means
    // "no fault", which is a claim we cannot make.
    const bool errorUsable = state.errorCodeSupported && statusUsable;
    // A 32-bit error code cannot fit this 16-bit register: saturate to 0xFFFE ("fault
    // present, code exceeds 16 bits -- consult MQTT/REST") rather than truncate, which
    // would silently rename the fault. 0xFFFF stays the not-usable sentinel.
    const uint16_t errorU16 = state.errorCode <= 0xFFFDu
                                  ? static_cast<uint16_t>(state.errorCode)
                                  : 0xFFFEu;
    writeU16(reg::kErrorCode, errorUsable ? errorU16 : kInvalidU16);
    writeU16(reg::kErrorCodeMirror, errorUsable ? errorU16 : kInvalidU16);
    setValidity(ValidityBit::ErrorCode, errorUsable);

    writeU16(reg::kConsecutiveFailures,
             static_cast<uint16_t>(state.consecutiveFailures > 0xFFFF ? 0xFFFF
                                                                      : state.consecutiveFailures));
    writeString(reg::kStatusText, statusUsable ? state.statusText : std::string(), 16);

    publishMeasurement(state, measurement_id::kAcPowerTotal, reg::kAcPowerTotal,
                       ValidityBit::AcPowerTotal);
    publishMeasurement(state, measurement_id::kAcL1Voltage, reg::kAcL1Voltage,
                       ValidityBit::AcL1Voltage);
    publishMeasurement(state, measurement_id::kAcL1Current, reg::kAcL1Current,
                       ValidityBit::AcL1Current);
    publishMeasurement(state, measurement_id::kAcFrequency, reg::kAcFrequency,
                       ValidityBit::AcFrequency);
    publishMeasurement(state, measurement_id::kDcPowerTotal, reg::kDcPowerTotal,
                       ValidityBit::DcPowerTotal);
    publishMeasurement(state, measurement_id::kDcMppt1Voltage, reg::kDcMppt1Voltage,
                       ValidityBit::DcMppt1Voltage);
    publishMeasurement(state, measurement_id::kDcMppt1Current, reg::kDcMppt1Current,
                       ValidityBit::DcMppt1Current);
    publishMeasurement(state, measurement_id::kTemperature, reg::kTemperature,
                       ValidityBit::Temperature);
    publishMeasurement(state, measurement_id::kEnergyToday, reg::kEnergyToday,
                       ValidityBit::EnergyToday);
    publishMeasurement(state, measurement_id::kEnergyTotal, reg::kEnergyTotal,
                       ValidityBit::EnergyTotal);

    const auto* hours = state.measurements.find(measurement_id::kOperatingHours);
    const bool hoursUsable = hours != nullptr && hours->supported && hours->valid && !hours->stale;
    writeU32(reg::kOperatingHours,
             hoursUsable ? static_cast<uint32_t>(hours->value) : kInvalidU32);
    setValidity(ValidityBit::OperatingHours, hoursUsable);

    // --- phase block ---
    static const char* kPhaseVoltage[] = {measurement_id::kAcL1Voltage, "ac.phase_l2.voltage",
                                          "ac.phase_l3.voltage"};
    static const char* kPhaseCurrent[] = {measurement_id::kAcL1Current, "ac.phase_l2.current",
                                          "ac.phase_l3.current"};
    static const char* kPhasePower[]   = {measurement_id::kAcL1Power, "ac.phase_l2.power",
                                          "ac.phase_l3.power"};
    static const ValidityBit kPhaseVoltageBit[] = {ValidityBit::AcL1Voltage,
                                                   ValidityBit::AcL2Voltage,
                                                   ValidityBit::AcL3Voltage};
    static const ValidityBit kPhaseCurrentBit[] = {ValidityBit::AcL1Current,
                                                   ValidityBit::AcL2Current,
                                                   ValidityBit::AcL3Current};
    static const ValidityBit kPhasePowerBit[]   = {ValidityBit::AcL1Power, ValidityBit::AcL2Power,
                                                   ValidityBit::AcL3Power};
    for (uint16_t i = 0; i < 3; ++i) {
        const uint16_t base = static_cast<uint16_t>(reg::kPhaseBase + i * reg::kPhaseStride);
        publishMeasurement(state, kPhaseVoltage[i], base + reg::kPhaseVoltageOffset,
                           kPhaseVoltageBit[i]);
        publishMeasurement(state, kPhaseCurrent[i], base + reg::kPhaseCurrentOffset,
                           kPhaseCurrentBit[i]);
        publishMeasurement(state, kPhasePower[i], base + reg::kPhasePowerOffset, kPhasePowerBit[i]);
    }

    // --- MPPT block ---
    static const char* kMpptVoltage[] = {measurement_id::kDcMppt1Voltage,
                                         measurement_id::kDcMppt2Voltage};
    static const char* kMpptCurrent[] = {measurement_id::kDcMppt1Current,
                                         measurement_id::kDcMppt2Current};
    static const char* kMpptPower[]   = {measurement_id::kDcMppt1Power,
                                         measurement_id::kDcMppt2Power};
    static const ValidityBit kMpptVoltageBit[] = {ValidityBit::DcMppt1Voltage,
                                                  ValidityBit::DcMppt2Voltage};
    static const ValidityBit kMpptCurrentBit[] = {ValidityBit::DcMppt1Current,
                                                  ValidityBit::DcMppt2Current};
    static const ValidityBit kMpptPowerBit[]   = {ValidityBit::DcMppt1Power,
                                                  ValidityBit::DcMppt2Power};
    for (uint16_t i = 0; i < 2; ++i) {
        const uint16_t base = static_cast<uint16_t>(reg::kMpptBase + i * reg::kMpptStride);
        publishMeasurement(state, kMpptVoltage[i], base + reg::kMpptVoltageOffset,
                           kMpptVoltageBit[i]);
        publishMeasurement(state, kMpptCurrent[i], base + reg::kMpptCurrentOffset,
                           kMpptCurrentBit[i]);
        publishMeasurement(state, kMpptPower[i], base + reg::kMpptPowerOffset, kMpptPowerBit[i]);
    }

    // --- battery and grid: empty for a driver without them, and that is visible ---
    publishMeasurement(state, "battery.soc", reg::kBatterySoc, ValidityBit::BatterySoc);
    publishMeasurement(state, "battery.voltage", reg::kBatteryVoltage, ValidityBit::BatteryVoltage);
    publishMeasurement(state, "battery.charge_power", reg::kBatteryChargePower,
                       ValidityBit::BatteryChargePower);
    publishMeasurement(state, "battery.discharge_power", reg::kBatteryDischargePower,
                       ValidityBit::BatteryDischargePower);
    publishMeasurement(state, "grid.import_power", reg::kGridImportPower,
                       ValidityBit::GridImportPower);
    publishMeasurement(state, "grid.export_power", reg::kGridExportPower,
                       ValidityBit::GridExportPower);

    // --- capabilities ---
    writeBitset64(reg::kCapabilitiesRead, state.capabilities.read);
    writeBitset64(reg::kCapabilitiesWrite, state.capabilities.write);
    writeU16(reg::kPhaseCount, state.capabilities.phaseCount);
    writeU16(reg::kMpptCount, state.capabilities.mpptCount);
    writeU16(reg::kBatteryPresent, state.capabilities.hasBattery ? 1 : 0);
    // Tells a client up front that writing is pointless, which beats only finding out via an
    // exception on function code 6.
    writeU16(reg::kDriverReadOnly, state.capabilities.isReadOnly() ? 1 : 0);

    // --- identity ---
    writeString(reg::kManufacturer, state.identity.manufacturer, 16);
    writeString(reg::kModel, state.identity.model, 16);
    writeString(reg::kSerialNumber, state.identity.serialNumber, 16);
    writeString(reg::kFirmwareVersion, state.identity.firmwareVersion, 8);
    writeString(reg::kDriverId, state.identity.driverId, 8);
    writeString(reg::kBridgeFirmware, bridge.firmwareVersion, 8);

    // --- counters ---
    const uint32_t secondsSincePoll =
        state.lastSuccessfulPollMs == 0
            ? kInvalidU32
            : static_cast<uint32_t>((nowMs - state.lastSuccessfulPollMs) / 1000);
    writeU32(reg::kSecondsSincePoll, secondsSincePoll);
    writeU32(reg::kPollSuccessTotal, diagnostics.pollSuccessTotal);
    writeU32(reg::kPollFailureTotal, diagnostics.pollFailureTotal);
    writeU32(reg::kChecksumErrors, diagnostics.checksumErrorTotal);
    writeU32(reg::kRs485Timeouts, diagnostics.rs485TimeoutTotal);
    // Never 0 on wifi-down: 0 dBm reads as a perfect connection. Sentinel instead, exactly as
    // kSecondsSincePoll above -- a client must be able to tell "unknown" from a real reading.
    //
    // Known, accepted ambiguity: the all-ones sentinel is bit-identical to a signed -1, so a
    // CONNECTED reading of exactly -1 dBm would be indistinguishable from "no wifi". Left as
    // is on purpose -- -1 dBm is not a physically reachable RSSI (real values run -30..-100),
    // and moving the sentinel would break every existing Modbus client that already tests for
    // 0xFFFF. JSON outputs have no such ambiguity; they publish null.
    writeI32(reg::kWifiRssi,
             bridge.wifiConnected ? bridge.wifiRssiDbm : static_cast<int32_t>(kInvalidU32));
    writeU32(reg::kBridgeUptime, bridge.uptimeSeconds);

    // --- diagnostics block, also served on the diagnostics unit id ---
    writeU32(reg::kDiagUptime, bridge.uptimeSeconds);
    writeU32(reg::kDiagFreeHeap, bridge.freeHeapBytes);
    writeU32(reg::kDiagMinFreeHeap, bridge.minFreeHeapBytes);
    writeU16(reg::kDiagResetReason, bridge.resetReason);
    // Same rule, and this one used to not even check the flag -- it published the last known
    // RSSI after a disconnect, a stale-but-plausible non-zero value.
    writeU16(reg::kDiagWifiRssi,
             bridge.wifiConnected ? static_cast<uint16_t>(bridge.wifiRssiDbm) : kInvalidU16);
    writeU32(reg::kDiagWifiReconnect, diagnostics.wifiReconnectTotal);
    writeU32(reg::kDiagMqttReconnect, diagnostics.mqttReconnectTotal);
    writeU32(reg::kDiagModbusClients, diagnostics.modbusClientConnections);
    writeU32(reg::kDiagRestRequests, diagnostics.restRequestTotal);
    writeU32(reg::kDiagInvalidFrames, diagnostics.invalidFrameTotal);
    writeU16(reg::kDiagFirmwareMajor, bridge.firmwareMajor);
    writeU16(reg::kDiagFirmwareMinor, bridge.firmwareMinor);
    writeU16(reg::kDiagFirmwarePatch, bridge.firmwarePatch);
    // Read-only observation of the bridge relays; the sentinel distinguishes "no relay
    // hardware" from "relays, none energised". Control never goes through Modbus.
    if (bridge.relayCount > 0) {
        writeU16(reg::kDiagRelayCount, bridge.relayCount);
        writeU16(reg::kDiagRelayMask, bridge.relayMask);
    } else {
        writeU16(reg::kDiagRelayCount, kInvalidU16);
        writeU16(reg::kDiagRelayMask, kInvalidU16);
    }
}

bool RegisterMap::read(uint16_t address, uint16_t count, uint16_t* out) const {
    if (count == 0 || out == nullptr) {
        return false;
    }
    if (static_cast<size_t>(address) + count > kRegisterCount) {
        return false;
    }
    for (uint16_t i = 0; i < count; ++i) {
        out[i] = registers_[address + i];
    }
    return true;
}

uint16_t RegisterMap::at(uint16_t address) const {
    return address < kRegisterCount ? registers_[address] : kInvalidU16;
}

}  // namespace heliograph::modbus
