// SPDX-License-Identifier: MIT

#include "sunspec_driver.h"

#include <cstdio>
#include <cstdlib>

#include "diagnostics/logger.h"

namespace heliograph::sunspec {
namespace {

constexpr uint32_t kTransactionDeadlineMs = 3000;
constexpr uint32_t kResponseTimeoutMs     = 1000;

/// Header of a chain block: the model id and its length.
constexpr uint16_t kHeaderRegisters = 2;

/// Modbus caps a single read; a long model is fetched in chunks.
constexpr uint16_t kChunk = 100;

}  // namespace

modbus::ReadOutcome SunspecDriver::read(uint16_t address, uint16_t count, uint16_t* out,
                                        uint16_t capacity) {
    const modbus::ReadTiming timing{kTransactionDeadlineMs, kResponseTimeoutMs};
    // Holding registers: SunSpec's own convention, and what every implementation this was
    // checked against uses.
    return modbus::readRegisters(*transport_, options_.unitId, modbus::kReadHoldingRegisters,
                                 address, count, out, capacity, timing);
}

bool SunspecDriver::walkChain() {
    chain_.clear();
    inverterEntry_ = nullptr;
    commonEntry_   = nullptr;
    walked_        = false;

    uint16_t marker[2] = {};
    if (read(options_.baseAddress, 2, marker, 2).status != modbus::ReadStatus::Ok) {
        return false;
    }
    if (marker[0] != kMarkerHigh || marker[1] != kMarkerLow) {
        log::debug("SUNSPEC no marker at %u (read %04X %04X)", options_.baseAddress, marker[0],
                   marker[1]);
        return false;
    }

    uint16_t cursor = static_cast<uint16_t>(options_.baseAddress + 2);
    for (size_t i = 0; i < kMaxChainEntries; ++i) {
        uint16_t header[2] = {};
        if (read(cursor, kHeaderRegisters, header, 2).status != modbus::ReadStatus::Ok) {
            // A chain that stops answering is not a broken device: several vendors simply do
            // not serve the terminator. Keep whatever was mapped so far.
            break;
        }
        if (header[0] == kEndOfChain) {
            break;
        }
        ChainEntry entry;
        entry.modelId = header[0];
        entry.length  = header[1];
        entry.address = cursor;
        chain_.push_back(entry);

        if (commonEntry_ == nullptr && entry.modelId == kModelCommon) {
            commonEntry_ = &chain_.back();
        }
        if (inverterEntry_ == nullptr && isInverterModel(entry.modelId)) {
            inverterEntry_ = &chain_.back();
        }

        const uint32_t next =
            static_cast<uint32_t>(cursor) + kHeaderRegisters + static_cast<uint32_t>(entry.length);
        if (next > 0xFFFF) {
            break;  // a length that walks off the address space: stop, keep what we have
        }
        cursor = static_cast<uint16_t>(next);
    }

    if (chain_.empty()) {
        return false;
    }
    // Pointers above were taken into a vector that has since grown; re-resolve them by index.
    inverterEntry_ = nullptr;
    commonEntry_   = nullptr;
    for (const auto& e : chain_) {
        if (commonEntry_ == nullptr && e.modelId == kModelCommon) {
            commonEntry_ = &e;
        }
        if (inverterEntry_ == nullptr && isInverterModel(e.modelId)) {
            inverterEntry_ = &e;
        }
    }

    // The full inventory, at INFO: this is the line someone pastes into an issue when their
    // device is not supported, and it is what says which models to implement next.
    std::string models;
    for (const auto& e : chain_) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%s%u", models.empty() ? "" : ", ", e.modelId);
        models += buf;
    }
    log::info("SUNSPEC chain at %u: %u model(s): %s", options_.baseAddress,
              static_cast<unsigned>(chain_.size()), models.c_str());

    walked_ = true;
    return true;
}

bool SunspecDriver::readModel(const ChainEntry& entry, std::vector<uint16_t>& out) {
    const uint16_t total = static_cast<uint16_t>(kHeaderRegisters + entry.length);
    out.assign(total, 0);
    uint16_t done = 0;
    while (done < total) {
        const uint16_t want = static_cast<uint16_t>((total - done) > kChunk ? kChunk : total - done);
        const auto     r    = read(static_cast<uint16_t>(entry.address + done), want,
                                   out.data() + done, want);
        if (r.status != modbus::ReadStatus::Ok) {
            return false;
        }
        done = static_cast<uint16_t>(done + want);
    }
    return true;
}

const DriverDescriptor& SunspecDriver::descriptor() const { return sunspec::descriptor(); }

bool SunspecDriver::begin(Transport& transport) {
    transport_ = &transport;
    chain_.clear();
    walked_ = false;
    identity_ = DeviceIdentity{};
    identity_.driverId     = descriptor().id;
    identity_.protocolName = descriptor().protocol;
    return true;
}

ProbeResult SunspecDriver::probe() {
    ProbeResult result;
    if (transport_ == nullptr || !walkChain()) {
        return result;
    }
    result.responded     = true;
    result.checksumValid = true;  // Modbus CRC was verified for every read that got here

    char note[96];
    std::snprintf(note, sizeof(note), "SunSpec marker at %u, %u model(s) advertised",
                  options_.baseAddress, static_cast<unsigned>(chain_.size()));
    result.evidence.emplace_back(note);

    if (commonEntry_ != nullptr) {
        std::vector<uint16_t> regs;
        CommonIdentity        id;
        if (readModel(*commonEntry_, regs) && decodeCommon(regs.data(), regs.size(), id)) {
            result.detectedManufacturer = id.manufacturer;
            result.detectedModel        = id.model;
            result.serialNumber         = id.serial;
            result.firmwareVersion      = id.version;
            identity_.manufacturer      = id.manufacturer;
            identity_.model             = id.model;
            identity_.serialNumber      = id.serial;
            identity_.firmwareVersion   = id.version;
            result.evidence.emplace_back("common model (1) identified the device");
        }
    }

    if (inverterEntry_ != nullptr) {
        char m[64];
        std::snprintf(m, sizeof(m), "inverter model %u present", inverterEntry_->modelId);
        result.evidence.emplace_back(m);
        // The marker plus a usable inverter model is about as unambiguous as identification
        // gets on a Modbus bus: no other protocol here answers with "SunS".
        result.confidenceScore = 95;
    } else {
        result.evidence.emplace_back(
            "no inverter model (101/102/103) on the chain -- device not readable by this driver");
        result.confidenceScore = 40;
    }
    return result;
}

PollResult SunspecDriver::poll(DeviceState& state) {
    if (transport_ == nullptr) {
        return PollResult::TransportError;
    }
    if (!walked_ && !walkChain()) {
        return PollResult::Timeout;
    }
    if (inverterEntry_ == nullptr) {
        return PollResult::InvalidFrame;  // mapped, but nothing this driver can read
    }

    std::vector<uint16_t> regs;
    if (!readModel(*inverterEntry_, regs)) {
        // Force a fresh walk next time: a device that stopped answering mid-chain may have
        // rebooted into a different layout.
        walked_ = false;
        return PollResult::Timeout;
    }

    InverterReadings r;
    if (!decodeInverter(regs.data(), regs.size(), r)) {
        return PollResult::InvalidFrame;
    }

    auto&          m  = state.measurements;
    const uint64_t ts = transport_->nowMs();
    if (r.hasAcPower) {
        m.set(measurement_id::kAcPowerTotal, r.acPowerW, ts);
    }
    if (r.hasAcVoltage) {
        m.set(measurement_id::kAcL1Voltage, r.acVoltageV, ts);
    }
    if (r.hasAcCurrent) {
        m.set(measurement_id::kAcL1Current, r.acCurrentA, ts);
    }
    if (r.hasFrequency) {
        m.set(measurement_id::kAcFrequency, r.frequencyHz, ts);
    }
    if (r.hasEnergyTotal) {
        m.set(measurement_id::kEnergyTotal, r.energyTotalKwh, ts);
    }
    if (r.hasDcPower) {
        m.set(measurement_id::kDcPowerTotal, r.dcPowerW, ts);
    }
    if (r.hasTemperature) {
        m.set(measurement_id::kTemperature, r.temperatureC, ts);
    }
    if (r.hasState) {
        state.statusCode = r.state;
    }
    return PollResult::Ok;
}

InverterCapabilities SunspecDriver::capabilities() const {
    // Declared from what the decoder can actually produce, not from what SunSpec defines:
    // a device that publishes none of these still advertises the capability, but every
    // measurement stays absent, which is the honest combination.
    InverterCapabilities c;
    c.addRead(InverterCapability::ReadAcPower);
    c.addRead(InverterCapability::ReadAcVoltage);
    c.addRead(InverterCapability::ReadAcCurrent);
    c.addRead(InverterCapability::ReadGridFrequency);
    c.addRead(InverterCapability::ReadDcPower);
    c.addRead(InverterCapability::ReadEnergyTotal);
    c.addRead(InverterCapability::ReadTemperature);
    return c;
}

CommandResult SunspecDriver::execute(const InverterCommand& command) {
    (void)command;
    // Read-only, like every driver here. SunSpec does define writable models, but enabling one
    // needs a hardware-verified map and a deliberate write path -- neither exists yet.
    return CommandResult::Unsupported;
}

}  // namespace heliograph::sunspec
