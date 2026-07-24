// SPDX-License-Identifier: MIT
// See growatt_driver.h for provenance.

#include "growatt_driver.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "diagnostics/logger.h"
#include "protocols/modbus/modbus_client.h"
#include "protocols/modbus/modbus_rtu.h"

namespace heliograph::growatt {
namespace {

constexpr uint32_t kResponseTimeoutMs = 1000;
/// Wall-clock ceiling on one block read. A Modbus reply arrives in milliseconds once it
/// starts; 3 s bounds a trickle that never completes while leaving generous slack.
constexpr uint32_t kTransactionDeadlineMs = 3000;

/// TRACE-dumps a raw register block, ~8 registers per line, prefixed with the absolute
/// register number. This is the bring-up tool: with the map still unconfirmed, reading these
/// against the inverter display is how the correct addresses and scaling are found. TRACE-only,
/// so it costs nothing in normal operation.
void traceBlock(RegSpace space, uint16_t start, uint16_t count, const uint16_t* values) {
    if (!log::enabled(LogLevel::Trace)) {
        return;
    }
    const char* spaceName = space == RegSpace::Input ? "in" : "hold";
    constexpr uint16_t perLine = 8;
    for (uint16_t i = 0; i < count; i += perLine) {
        char line[128];
        int  pos = snprintf(line, sizeof(line), "GROWATT %s %u:", spaceName,
                            static_cast<unsigned>(start + i));
        for (uint16_t j = 0; j < perLine && i + j < count; ++j) {
            pos += snprintf(line + pos, sizeof(line) - pos, " %04X", values[i + j]);
        }
        log::trace("%s", line);
    }
}

}  // namespace

GrowattOptions optionsFrom(const heliograph::DriverOptions& values) {
    GrowattOptions o;
    const std::string unit = descriptor().optionOr(values, "unit_id");
    if (!unit.empty()) {
        const long parsed = strtol(unit.c_str(), nullptr, 10);
        if (parsed >= 1 && parsed <= 247) {  // valid Modbus unit id range
            o.unitId = static_cast<uint8_t>(parsed);
        } else {
            // Config validation only length-checks option strings, so a typo lands here.
            // Falling back silently would poll the wrong slave with no clue why it is silent.
            log::warn("GROWATT unit_id '%s' invalid (want 1-247), using %u", unit.c_str(),
                      static_cast<unsigned>(o.unitId));
        }
    }
    const std::string profileId = descriptor().optionOr(values, "profile");
    if (!profileId.empty()) {
        if (const GrowattProfile* p = findProfile(profileId.c_str())) {
            o.profile = p;
        } else {
            // Same reasoning as unit_id above: a typo must not silently poll with the wrong
            // register map -- fall back loudly.
            log::warn("GROWATT profile '%s' unknown, using '%s'", profileId.c_str(),
                      defaultProfile().id);
        }
    }
    return o;
}

GrowattDriver::GrowattDriver(Transport& transport, GrowattOptions options)
    : transport_(&transport), options_(options) {}

const DriverDescriptor& GrowattDriver::descriptor() const { return growatt::descriptor(); }

bool GrowattDriver::begin(Transport& transport) {
    transport_ = &transport;
    // Configure the line for this protocol, exactly as the EverSolar driver does. Without
    // this, a boot that goes straight into this driver -- every reboot once it is the
    // selected driver -- polls an unconfigured UART and hears silence forever; it only
    // worked after a discovery run because probing happens to configure the transport
    // (2026-07-21 discovery review). The profile's declared [serial] settings win when
    // present; the descriptor's first recommended profile is the fallback.
    if (options_.profile->hasSerial) {
        return transport.configure(options_.profile->serial);
    }
    const auto& profiles = descriptor().recommendedSerialProfiles;
    return !profiles.empty() && transport.configure(profiles.front());
}

GrowattDriver::ReadResult GrowattDriver::readBlock(RegSpace space, uint16_t start, uint16_t count,
                                                   uint16_t* out) {
    if (transport_ == nullptr) {
        return ReadResult::TransportError;
    }
    const uint8_t fn = space == RegSpace::Input ? modbus::kReadInputRegisters
                                                : modbus::kReadHoldingRegisters;

    // The exchange itself is protocol-generic and lives in protocols/modbus/modbus_client:
    // SunSpec needs the identical transaction against a completely different register map.
    // What stays here is the part that is Growatt's business -- which register space a block
    // means, and the TRACE dump the bring-up procedure depends on.
    const modbus::ReadTiming timing{kTransactionDeadlineMs, kResponseTimeoutMs};
    const auto outcome =
        modbus::readRegisters(*transport_, options_.unitId, fn, start, count, out, count, timing);

    switch (outcome.status) {
        case modbus::ReadStatus::Ok:
            traceBlock(space, start, count, out);
            return ReadResult::Ok;
        case modbus::ReadStatus::Exception:
            lastException_ = outcome.exceptionCode;
            return ReadResult::Exception;
        case modbus::ReadStatus::Timeout:
            return ReadResult::Timeout;
        case modbus::ReadStatus::TransportError:
            return ReadResult::TransportError;
        case modbus::ReadStatus::Protocol:
            break;
    }
    return ReadResult::Protocol;
}

PollResult GrowattDriver::poll(DeviceState& state) {
    if (transport_ == nullptr) {
        return PollResult::TransportError;
    }
    const GrowattProfile& profile = *options_.profile;

    size_t validCount  = 0;
    bool   sawTimeout  = false;
    bool   sawResponse = false;  // device answered *something* (exception / bad frame) = alive

    // A block the device refuses (exception) or that arrives corrupt is skipped, not fatal:
    // during bring-up the profile deliberately probes ranges that may not exist on this
    // firmware (the 1000- vs 3000-series generation question). Only a device that answers
    // nothing is a real failure. This also means the raw TRACE dump shows exactly the ranges
    // this inverter actually supports.
    for (size_t i = 0; i < profile.blockCount && validCount < kMaxBlocks; ++i) {
        const RegBlock& b    = profile.blocks[i];
        BlockData&      data = blocks_[validCount];
        data.space = b.space;
        data.start = b.start;
        data.count = b.count;
        const ReadResult r = readBlock(b.space, b.start, b.count, data.values);
        switch (r) {
            case ReadResult::Ok:
                ++validCount;
                break;
            case ReadResult::Exception:
                sawResponse = true;
                log::warn("GROWATT block %u+%u refused (exception 0x%02X) -- skipped", b.start,
                          b.count, lastException_);
                break;
            case ReadResult::Protocol:
                sawResponse = true;
                log::warn("GROWATT block %u+%u unreadable (bad frame) -- skipped", b.start,
                          b.count);
                break;
            case ReadResult::Timeout:
                sawTimeout = true;
                break;
            case ReadResult::TransportError:
                return PollResult::TransportError;
        }
    }

    if (validCount == 0) {
        // Nothing usable. Report "silent" only when the device truly said nothing: a refused
        // range or a bad frame proves it is present and addressable, so that outranks a
        // timeout on another block -- InvalidFrame, not the misleading Timeout.
        return (sawTimeout && !sawResponse) ? PollResult::Timeout : PollResult::InvalidFrame;
    }

    // At least one block is good -> safe to touch state.
    const uint64_t ts = state.lastPollAttemptMs;
    applyProfile(profile, blocks_, validCount, state.measurements, ts);

    InverterCapabilities caps;
    caps.phaseCount = profile.phaseCount;
    caps.mpptCount  = profile.mpptCount;
    caps.hasBattery = profile.hasBattery;
    if (profile.hasBattery) {
        caps.addRead(InverterCapability::ReadBatteryState);
    }
    caps.addRead(InverterCapability::ReadAcPower);
    caps.addRead(InverterCapability::ReadDcPower);
    state.capabilities = caps;

    state.identity = identity();  // already carries driverId (see identity())
    // The protocol exposes a status/fault word, but its code space is undocumented for the SPH
    // and unconfirmed here. Naming a status would be inventing one.
    state.statusText         = "";
    state.errorCodeSupported = false;
    state.errorCode          = 0;
    return PollResult::Ok;
}

ProbeResult GrowattDriver::probe() {
    ProbeResult result;
    if (transport_ == nullptr) {
        return result;
    }
    // A single input-register read is enough to tell "a Modbus device answers at this unit id"
    // from silence. Read-only and cheap. Deeper identification waits until the map is trusted.
    const RegBlock& first = options_.profile->blocks[0];
    uint16_t        scratch[125];
    const ReadResult r = readBlock(first.space, first.start,
                                   first.count < 8 ? first.count : 8, scratch);
    if (r == ReadResult::Ok) {
        result.responded      = true;
        result.checksumValid  = true;
        result.confidenceScore += 40;
        result.evidence.push_back("Modbus device answered a register read with a valid CRC");
        result.detectedManufacturer = "Growatt";
    } else if (r == ReadResult::Exception) {
        // It answered, just not for that range: still a Modbus device on this unit id.
        result.responded = true;
        result.confidenceScore += 20;
        result.evidence.push_back("Modbus device answered with an exception (wrong register?)");
    } else {
        result.evidence.push_back("no Modbus reply at this unit id and line speed");
    }
    return result;
}

DeviceIdentity GrowattDriver::identity() const {
    DeviceIdentity id;
    id.manufacturer = "Growatt";
    id.model        = options_.profile->displayName;
    id.protocolName = "Modbus RTU";
    id.driverId     = descriptor().id;
    // Serial number lives in a register block we do not map yet; it stays empty rather than
    // being invented, so deviceId() falls back to the driver id.
    return id;
}

InverterCapabilities GrowattDriver::capabilities() const {
    InverterCapabilities caps;
    caps.phaseCount = options_.profile->phaseCount;
    caps.mpptCount  = options_.profile->mpptCount;
    caps.hasBattery = options_.profile->hasBattery;
    return caps;
}

CommandResult GrowattDriver::execute(const InverterCommand&) {
    return CommandResult::Unsupported;  // read-only until the map is confirmed on hardware
}

}  // namespace heliograph::growatt
