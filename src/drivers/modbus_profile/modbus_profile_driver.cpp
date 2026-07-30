// SPDX-License-Identifier: MIT
// See modbus_profile_driver.h for provenance.

#include "modbus_profile_driver.h"

#include <cstdio>
#include <cstring>

#include "diagnostics/logger.h"
#include "protocols/modbus/modbus_client.h"
#include "protocols/modbus/modbus_rtu.h"

namespace heliograph::profile {
namespace {

constexpr uint32_t kResponseTimeoutMs = 1000;
/// Wall-clock ceiling on one block read. A Modbus reply arrives in milliseconds once it
/// starts; 3 s bounds a trickle that never completes while leaving generous slack.
constexpr uint32_t kTransactionDeadlineMs = 3000;

/// TRACE-dumps a raw register block, ~8 registers per line, prefixed with the absolute
/// register number. This is the bring-up tool: with the map still unconfirmed, reading these
/// against the inverter display is how the correct addresses and scaling are found. TRACE-only,
/// so it costs nothing in normal operation.
void traceBlock(uint8_t unitId, RegSpace space, uint16_t start, uint16_t count,
                const uint16_t* values) {
    if (!log::enabled(LogLevel::Trace)) {
        return;
    }
    const char* spaceName = space == RegSpace::Input ? "in" : "hold";
    constexpr uint16_t perLine = 8;
    for (uint16_t i = 0; i < count; i += perLine) {
        char line[128];
        int  pos = snprintf(line, sizeof(line), "MODBUS unit %u %s %u:",
                            static_cast<unsigned>(unitId), spaceName,
                            static_cast<unsigned>(start + i));
        for (uint16_t j = 0; j < perLine && i + j < count; ++j) {
            pos += snprintf(line + pos, sizeof(line) - pos, " %04X", values[i + j]);
        }
        log::trace("%s", line);
    }
}

}  // namespace

ProfileOptions optionsFrom(const heliograph::DriverOptions& values) {
    ProfileOptions o;
    // Range comes from the descriptor's own DriverOption, not from a literal repeated here.
    long unit = 0;
    if (descriptor().numericOption(values, "unit_id", unit)) {
        o.unitId = static_cast<uint8_t>(unit);
    } else {
        // Falling back silently would poll the wrong slave with no clue why it is silent.
        log::warn("MODBUS unit_id '%s' invalid, using %u",
                  descriptor().optionOr(values, "unit_id").c_str(),
                  static_cast<unsigned>(o.unitId));
    }
    const std::string profileId = descriptor().optionOr(values, "profile");
    if (!profileId.empty()) {
        if (const DeviceProfile* p = findProfile(profileId.c_str())) {
            o.profile = p;
        } else {
            // Same reasoning as unit_id above: a typo must not silently poll with the wrong
            // register map -- fall back loudly.
            log::warn("MODBUS profile '%s' unknown, using '%s'", profileId.c_str(),
                      defaultProfile().id);
        }
    }
    return o;
}

ModbusProfileDriver::ModbusProfileDriver(Transport& transport, ProfileOptions options)
    : transport_(&transport), options_(options) {}

const DriverDescriptor& ModbusProfileDriver::descriptor() const { return profile::descriptor(); }

bool ModbusProfileDriver::begin(Transport& transport) {
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

ModbusProfileDriver::ReadResult ModbusProfileDriver::readBlock(RegSpace space, uint16_t start, uint16_t count,
                                                   uint16_t* out, bool probe) {
    if (transport_ == nullptr) {
        return ReadResult::TransportError;
    }
    const uint8_t fn = space == RegSpace::Input ? modbus::kReadInputRegisters
                                                : modbus::kReadHoldingRegisters;

    // The exchange itself is protocol-generic and lives in protocols/modbus/modbus_client:
    // SunSpec needs the identical transaction against a completely different register map.
    // What stays here is the part that is the profile's business -- which register space a block
    // means, and the TRACE dump the bring-up procedure depends on.
    const modbus::ReadTiming timing{kTransactionDeadlineMs, kResponseTimeoutMs};
    const auto outcome =
        modbus::readRegisters(*transport_, options_.unitId, fn, start, count, out, count, timing);
    // Per transaction, not per poll: this driver reads several blocks and reports Ok as soon as
    // one decodes, so counting from the poll verdict made a bus that corrupted most of its
    // frames register as healthy.
    //
    // A probe block is exempt. Asking a model whether it implements a range we are not sure it
    // has is the firmware's own question, and an inverter is entitled to answer with silence
    // rather than the exception the Modbus spec prefers. Counting that silence as a bus timeout
    // would slope the metric of a healthy install for as long as the probe stays in the profile.
    if (!probe) {
        tallyModbusRead(busErrors_, outcome.status);
    }

    switch (outcome.status) {
        case modbus::ReadStatus::Ok:
            traceBlock(options_.unitId, space, start, count, out);
            return ReadResult::Ok;
        case modbus::ReadStatus::Exception:
            lastException_ = outcome.exceptionCode;
            return ReadResult::Exception;
        case modbus::ReadStatus::Timeout:
            return ReadResult::Timeout;
        case modbus::ReadStatus::TransportError:
            return ReadResult::TransportError;
        case modbus::ReadStatus::Crc:
            return ReadResult::Crc;
        case modbus::ReadStatus::Protocol:
            break;
    }
    return ReadResult::Protocol;
}

PollResult ModbusProfileDriver::poll(DeviceState& state) {
    if (transport_ == nullptr) {
        return PollResult::TransportError;
    }
    const DeviceProfile& profile = *options_.profile;

    size_t validCount  = 0;
    bool   sawCrcError = false;  // bytes arrived corrupted -> the cable, not the configuration
    bool   sawResponse = false;  // device answered *something* (exception / bad frame) = alive
    bool   sawBadFrame = false;  // an intact frame that was not ours: addressing, not cabling

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
        const ReadResult r = readBlock(b.space, b.start, b.count, data.values, b.probe);
        switch (r) {
            case ReadResult::Ok:
                ++validCount;
                break;
            case ReadResult::Exception:
                sawResponse = true;
                log::warn("MODBUS unit %u block %u+%u refused (exception 0x%02X) -- skipped",
                          options_.unitId, b.start, b.count, lastException_);
                break;
            case ReadResult::Crc:
                sawResponse = true;
                sawCrcError = true;
                log::warn("MODBUS unit %u block %u+%u failed checksum -- check ground, termination "
                          "and cable routing", options_.unitId, b.start, b.count);
                break;
            case ReadResult::Protocol:
                sawResponse = true;
                sawBadFrame = true;
                log::warn("MODBUS unit %u block %u+%u unreadable (bad frame) -- skipped",
                          options_.unitId, b.start, b.count);
                break;
            case ReadResult::Timeout:
                break;  // silence on this block; only total silence is a timeout verdict
            case ReadResult::TransportError:
                return PollResult::TransportError;
        }
    }

    if (validCount == 0) {
        // Nothing usable. A checksum failure outranks everything else: it is the only outcome
        // that points at the cable, and it is what the alerting rule watches. Then a bad frame,
        // which proves the device is present and addressable.
        if (sawCrcError) {
            return PollResult::ChecksumError;
        }
        if (sawBadFrame) {
            return PollResult::InvalidFrame;
        }
        // Nothing but exceptions: the device is present, correctly addressed, and refusing
        // every range this profile asks for -- a wrong profile or unit id, not a wire fault.
        // This used to report InvalidFrame, which pointed the field diagnosis at the cabling
        // AND contradicted the bus counters, since an exception rightly moves none of them: the
        // poll failed every ten seconds while all three RS485 counters read zero. SunSpec has
        // always called this NotRegistered (failureFor()); the two Modbus drivers now agree.
        if (sawResponse) {
            return PollResult::NotRegistered;
        }
        return PollResult::Timeout;
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

ProbeResult ModbusProfileDriver::probe() {
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
        // The SELECTED profile's vendor, which is a statement about what was configured, not
        // about what answered. Every Modbus RTU device on the planet replies to a register read
        // the same way; nothing in that reply identifies a brand. Naming one vendor here used to
        // be harmless because there was only one family of profiles -- with several vendors in
        // the tree it would assert an identification the probe cannot make.
        result.detectedManufacturer = options_.profile->manufacturer;
    } else if (r == ReadResult::Exception) {
        // It answered, just not for that range: still a Modbus device on this unit id.
        result.responded = true;
        result.confidenceScore += 20;
        result.evidence.push_back("Modbus device answered with an exception (wrong register?)");
    } else if (r == ReadResult::Crc) {
        // Bytes DID come back, they just did not survive the CRC. Deliberately still
        // responded=false: the discovery engine stops sweeping the remaining serial profiles as
        // soon as a candidate responds, and garbage that happens to frame up as a bad-CRC reply
        // is entirely normal at the wrong line speed -- claiming a device here would abort the
        // sweep before the right baud rate is ever tried. Only the wording changes, and that
        // wording is shown to the user verbatim in the wizard.
        result.sawTraffic = true;
        result.evidence.push_back(
            "replies arrived but failed their checksum: wrong line speed, two devices sharing "
            "this address, or a noisy bus (check ground, termination and cable routing)");
    } else {
        result.evidence.push_back("no Modbus reply at this unit id and line speed");
    }
    return result;
}

DeviceIdentity ModbusProfileDriver::identity() const {
    DeviceIdentity id;
    id.manufacturer = options_.profile->manufacturer;
    id.model        = options_.profile->displayName;
    id.protocolName = "Modbus RTU";
    id.driverId     = descriptor().id;
    // The unit id is what distinguishes three identical inverters on one bus, and it is known
    // before a single byte goes out. Without it all three key the same state store.
    id.instanceKey  = std::to_string(options_.unitId);
    // Serial number lives in a register block we do not map yet; it stays empty rather than
    // being invented, so deviceId() falls back to the driver id.
    return id;
}

/// The write row for a command, or nullptr when this profile has none it can actually serve.
///
/// Three separate reasons to say no, and they are checked here so that capabilities() and
/// execute() can never disagree about which commands exist -- a driver that ADVERTISES a
/// setpoint and then refuses it is worse than one that never offered.
const WriteMapping* ModbusProfileDriver::writeFor(InverterCommandType type) const {
    for (size_t i = 0; i < options_.profile->writeCount; ++i) {
        const WriteMapping& w = options_.profile->writes[i];
        if (w.command != type) {
            continue;
        }
        // Unverified rows are documented research, not a control surface. A row goes live only
        // when somebody has written the register on real hardware, read it back, and confirmed
        // against the inverter's own reported output that it took effect.
        if (!w.verified) {
            return nullptr;
        }
        // The Modbus client speaks FC06 only. A row needing FC16 -- more than one register, or
        // a firmware that demands write-multiple for a single one -- cannot be served, and
        // pretending otherwise would send a frame the device never asked for.
        if (w.words != 1 || w.useWriteMultiple) {
            return nullptr;
        }
        return &w;
    }
    return nullptr;
}

InverterCapabilities ModbusProfileDriver::capabilities() const {
    InverterCapabilities caps;
    caps.phaseCount = options_.profile->phaseCount;
    caps.mpptCount  = options_.profile->mpptCount;
    caps.hasBattery = options_.profile->hasBattery;

    // Advertised from the same lookup execute() uses, so the two cannot drift apart. A profile
    // with only unverified rows reports exactly what it did before this existed: read-only.
    for (size_t i = 0; i < options_.profile->writeCount; ++i) {
        const WriteMapping* w = writeFor(options_.profile->writes[i].command);
        if (w == nullptr) {
            continue;
        }
        caps.addWrite(requiredCapability(w->command));
        NumericCapability& n = caps.numeric[static_cast<size_t>(w->command)];
        n.supported          = true;
        n.writable           = true;
        n.minimum            = w->minimum;
        n.maximum            = w->maximum;
        n.step               = w->step;
        n.unit               = w->unit;
    }
    return caps;
}

CommandResult ModbusProfileDriver::execute(const InverterCommand& command) {
    if (transport_ == nullptr) {
        return CommandResult::Unsupported;
    }
    const WriteMapping* w = writeFor(command.type);
    if (w == nullptr) {
        return CommandResult::Unsupported;
    }
    if (!command.numericValue.has_value()) {
        return CommandResult::Rejected;
    }
    // Bounds are checked HERE as well as in the dispatcher. The dispatcher enforces what
    // capabilities() advertised; this enforces what the register actually accepts, and the two
    // are the same number today only because both read it from the same row.
    const double value = *command.numericValue;
    if (value < w->minimum || value > w->maximum) {
        return CommandResult::OutOfRange;
    }
    const double raw = value / w->scale;
    if (raw < 0.0 || raw > 65535.0) {
        return CommandResult::OutOfRange;
    }

    const modbus::TransactionOutcome outcome = modbus::writeSingleRegister(
        *transport_, options_.unitId, w->address, static_cast<uint16_t>(raw + 0.5),
        modbus::ReadTiming{kTransactionDeadlineMs, kResponseTimeoutMs});
    switch (outcome.status) {
        case modbus::TransactionStatus::Ok:
            return CommandResult::Ok;
        case modbus::TransactionStatus::Exception:
            // The device understood and refused: a read-only register, a value it will not take,
            // or a mode that has to be unlocked first. Rejected, not a transport failure -- the
            // bus worked perfectly.
            return CommandResult::Rejected;
        case modbus::TransactionStatus::Timeout:
            // Its own result, not folded into DriverError: silence on the bus points at wiring
            // or a unit id, and a driver fault points at this code. Two different call-outs.
            return CommandResult::Timeout;
        default:
            // Crc, Protocol, TransportError. The write may or may not have landed -- a CRC
            // failure on the ECHO means the device answered and we could not read it. Reported
            // as an error rather than guessed either way.
            return CommandResult::DriverError;
    }
}

}  // namespace heliograph::profile
