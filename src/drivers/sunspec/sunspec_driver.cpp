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
    const auto outcome = modbus::readRegisters(*transport_, options_.unitId,
                                               modbus::kReadHoldingRegisters, address, count, out,
                                               capacity, timing);
    // Every read on the wire passes through here. A steady-state poll is ONE transaction on a
    // device without model 123 and TWO on a device with it: the chain walk is gated on walked_
    // and runs once per session, and model 103 and model 123 each fit inside a single chunk.
    // The dozen-transaction case is the walk itself, where only the read that stopped it ever
    // reached the old poll-verdict counter.
    tallyModbusRead(busErrors_, outcome.status);
    return outcome;
}

// Translates a failed read into the poll outcome that describes it honestly. Everything used
// to become Timeout, which meant a bus with a bad ground -- CRC failures -- was invisible in the
// one counter the alerting rules key on, and pointed the field diagnosis at the wrong thing.
static PollResult failureFor(modbus::ReadStatus status) {
    switch (status) {
        case modbus::ReadStatus::Crc:
            return PollResult::ChecksumError;
        case modbus::ReadStatus::Timeout:
            return PollResult::Timeout;
        case modbus::ReadStatus::TransportError:
            return PollResult::TransportError;
        case modbus::ReadStatus::Protocol:
            // An intact frame that was not ours: a neighbour on the multidrop bus answering, or
            // a short reply. Addressing or a device quirk -- which is exactly what
            // docs/prometheus.md defines invalid frames as, and what the Growatt driver already
            // reports for the same status. Reporting NotRegistered here instead left the two
            // Modbus drivers describing identical bus conditions differently.
            return PollResult::InvalidFrame;
        case modbus::ReadStatus::Exception:
        case modbus::ReadStatus::Ok:
            break;
    }
    // An exception means the device is present, addressed correctly, and refusing this range.
    // Nothing is wrong with the wire and no frame was malformed, so neither error counter
    // should move.
    return PollResult::NotRegistered;
}

bool SunspecDriver::walkChain(PollResult& outFailure) {
    chain_.clear();
    inverterEntry_ = nullptr;
    commonEntry_   = nullptr;
    controlsEntry_ = nullptr;
    // Dropped with the chain, not kept across it: a re-walk happens because the device stopped
    // answering mid-chain, and one that came back with a different layout must not be written
    // into using bounds read from the layout it had before.
    controlsRead_  = false;
    controls_      = ControlsReadings{};
    walked_        = false;

    uint16_t   marker[2] = {};
    const auto markerRead = read(options_.baseAddress, 2, marker, 2);
    if (markerRead.status != modbus::ReadStatus::Ok) {
        outFailure = failureFor(markerRead.status);
        return false;
    }
    if (marker[0] != kMarkerHigh || marker[1] != kMarkerLow) {
        log::debug("SUNSPEC no marker at %u (read %04X %04X)", options_.baseAddress, marker[0],
                   marker[1]);
        // A healthy device that simply is not SunSpec. Saying Timeout here accused the wiring
        // of a fault that does not exist.
        outFailure = PollResult::NotRegistered;
        return false;
    }

    uint16_t cursor = static_cast<uint16_t>(options_.baseAddress + 2);
    // Why the walk stopped, for the case where it stopped before mapping anything at all.
    // Silence there is a device that answered the marker and then went quiet; a CRC failure is
    // a bad wire; an exception is a device refusing the range. Left unset, the caller's default
    // reported all three as Timeout -- the exact mislabel this change exists to remove.
    modbus::ReadStatus stoppedBecause = modbus::ReadStatus::Timeout;
    for (size_t i = 0; i < kMaxChainEntries; ++i) {
        uint16_t   header[2]  = {};
        const auto headerRead = read(cursor, kHeaderRegisters, header, 2);
        if (headerRead.status != modbus::ReadStatus::Ok) {
            // A chain that stops answering is not a broken device: several vendors simply do
            // not serve the terminator. Keep whatever was mapped so far -- but remember why,
            // because if nothing was mapped this is the only clue we will have.
            stoppedBecause = headerRead.status;
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
        // No pointers taken here on purpose: every push_back can reallocate, so anything
        // captured mid-walk is dangling by the next iteration. They are resolved once below,
        // after the vector has stopped growing.

        const uint32_t next =
            static_cast<uint32_t>(cursor) + kHeaderRegisters + static_cast<uint32_t>(entry.length);
        if (next > 0xFFFF) {
            break;  // a length that walks off the address space: stop, keep what we have
        }
        cursor = static_cast<uint16_t>(next);
    }

    if (chain_.empty()) {
        // The marker was there, so something SunSpec-shaped is on the bus, but not one model
        // could be read. Report the reason the walk stopped rather than a blanket Timeout.
        outFailure = failureFor(stoppedBecause);
        return false;
    }
    // Resolved now that the vector is final and cannot reallocate under these pointers.
    for (const auto& e : chain_) {
        if (commonEntry_ == nullptr && e.modelId == kModelCommon) {
            commonEntry_ = &e;
        }
        if (inverterEntry_ == nullptr && isInverterModel(e.modelId)) {
            inverterEntry_ = &e;
        }
        if (controlsEntry_ == nullptr && e.modelId == kModelControls) {
            // Only accept a block long enough to hold the points that will be written. A device
            // advertising a truncated 123 is not one to write into on the assumption that the
            // missing tail is where we think it is: the enable point sits at offset 9 and the
            // scale factor at 23, so a short block puts a control write into whatever follows.
            if (static_cast<size_t>(kHeaderRegisters + e.length) >= controls::kMinRegisters) {
                controlsEntry_ = &e;
            } else {
                log::warn("SUNSPEC model 123 at %u is %u registers, needs %u -- controls ignored",
                          e.address, static_cast<unsigned>(kHeaderRegisters + e.length),
                          static_cast<unsigned>(controls::kMinRegisters));
            }
        }
    }

    // Identity is read here rather than only in probe(): a driver selected by hand never gets
    // probed, and the device would then stay nameless in the UI and in Home Assistant for the
    // whole session even though the information was one read away.
    if (commonEntry_ != nullptr) {
        std::vector<uint16_t> regs;
        CommonIdentity        id;
        PollResult            ignored = PollResult::Timeout;  // identity is best-effort here
        if (readModel(*commonEntry_, regs, ignored) &&
            decodeCommon(regs.data(), regs.size(), id)) {
            identity_.manufacturer    = id.manufacturer;
            identity_.model           = id.model;
            identity_.serialNumber    = id.serial;
            identity_.firmwareVersion = id.version;
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

bool SunspecDriver::readModel(const ChainEntry& entry, std::vector<uint16_t>& out,
                              PollResult& outFailure) {
    const uint16_t total = static_cast<uint16_t>(kHeaderRegisters + entry.length);
    out.assign(total, 0);
    uint16_t done = 0;
    while (done < total) {
        const uint16_t want = static_cast<uint16_t>((total - done) > kChunk ? kChunk : total - done);
        const auto     r    = read(static_cast<uint16_t>(entry.address + done), want,
                                   out.data() + done, want);
        if (r.status != modbus::ReadStatus::Ok) {
            outFailure = failureFor(r.status);
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
    walked_                = false;
    controlsEntry_         = nullptr;
    controlsRead_          = false;
    controls_              = ControlsReadings{};
    identity_              = DeviceIdentity{};
    identity_.driverId     = descriptor().id;
    identity_.instanceKey  = std::to_string(options_.unitId);
    identity_.protocolName = descriptor().protocol;

    // Configure the line, exactly as the sibling Modbus driver does and for the reason its
    // comment records: without this, a boot that goes straight into this driver -- every
    // reboot once it is the selected driver -- polls an unconfigured UART and hears silence
    // forever. Discovery hides the bug, because probing happens to configure the transport.
    // SunSpec mandates no line speed, so the descriptor's first recommendation is the default
    // and the owner can pick the other from the settings page.
    const auto& profiles = descriptor().recommendedSerialProfiles;
    return !profiles.empty() && transport.configure(profiles.front());
}

ProbeResult SunspecDriver::probe() {
    ProbeResult result;
    PollResult  ignored = PollResult::Timeout;  // probing only cares whether it worked
    if (transport_ == nullptr || !walkChain(ignored)) {
        return result;
    }
    result.responded     = true;
    result.checksumValid = true;  // Modbus CRC was verified for every read that got here

    char note[96];
    std::snprintf(note, sizeof(note), "SunSpec marker at %u, %u model(s) advertised",
                  options_.baseAddress, static_cast<unsigned>(chain_.size()));
    result.evidence.emplace_back(note);

    // walkChain() already read the common model into identity_; reuse it rather than spending
    // a second round trip on the same registers.
    if (!identity_.manufacturer.empty() || !identity_.serialNumber.empty()) {
        result.detectedManufacturer = identity_.manufacturer;
        result.detectedModel        = identity_.model;
        result.serialNumber         = identity_.serialNumber;
        result.firmwareVersion      = identity_.firmwareVersion;
        result.evidence.emplace_back("common model (1) identified the device");
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
    PollResult failure = PollResult::Timeout;
    if (!walked_ && !walkChain(failure)) {
        return failure;
    }
    if (inverterEntry_ == nullptr) {
        // Mapped fine, but carries no model this driver reads -- a battery-only device, say.
        // Deliberately NOT InvalidFrame: that counter means "bytes arrived corrupted" and
        // feeds the alerting rule that tells someone to go check their ground and
        // termination. Nothing is wrong with this bus, so it must not say so.
        return PollResult::NotRegistered;
    }

    std::vector<uint16_t> regs;
    if (!readModel(*inverterEntry_, regs, failure)) {
        // Force a fresh walk next time: a device that stopped answering mid-chain may have
        // rebooted into a different layout.
        walked_ = false;
        return failure;
    }

    InverterReadings r;
    if (!decodeInverter(regs.data(), regs.size(), r)) {
        // Counted here, not in read(): every read succeeded with a valid CRC, so the tally in
        // read() saw nothing wrong. The frame is intact and simply not the one this driver can
        // use -- which is what invalidFrames means, and what the PMU drivers already count on
        // their own decode failures. Missing it left this path failing every poll with all
        // three bus counters flat at zero, and walked_ is not reset here, so it stays that way.
        ++busErrors_.invalidFrames;
        return PollResult::InvalidFrame;
    }

    auto&          m  = state.measurements;
    const uint64_t ts = transport_->nowMs();

    // Declare before setting: set() ignores an id that was never declared, which is what stops
    // a driver inventing a channel. Declared on every poll rather than once at begin() because
    // WHICH points a SunSpec device implements is only known after reading it -- and a device
    // that publishes nothing for a point still gets the channel, left without a reading, so
    // outputs can say "supported but no value" instead of staying silent about it.
    m.declare(measurement_id::kAcPowerTotal, MeasurementType::Power, Unit::Watt, "AC Power");
    m.declare(measurement_id::kAcL1Voltage, MeasurementType::Voltage, Unit::Volt, "AC Voltage");
    m.declare(measurement_id::kAcL1Current, MeasurementType::Current, Unit::Ampere, "AC Current");
    m.declare(measurement_id::kAcFrequency, MeasurementType::Frequency, Unit::Hertz,
              "Grid Frequency");
    m.declare(measurement_id::kEnergyTotal, MeasurementType::Energy, Unit::KilowattHour,
              "Total Energy");
    m.declare(measurement_id::kDcPowerTotal, MeasurementType::Power, Unit::Watt, "DC Power");
    m.declare(measurement_id::kTemperature, MeasurementType::Temperature, Unit::Celsius,
              "Temperature");

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
        state.statusCode          = r.state;
        state.statusCodeSupported = true;
    }

    // Read on every poll, not once: the limit is a control somebody else can also change --
    // another controller on the bus, the installer's app, or the inverter's own revert timer,
    // which model 123 defines and several devices implement. Reporting what we last WROTE
    // rather than what the device currently holds would make those changes invisible.
    if (controlsEntry_ != nullptr && refreshControls()) {
        m.declare(measurement_id::kActivePowerLimitPct, MeasurementType::Ratio, Unit::Percent,
                  "Active Power Limit");
        // Both channels, because either alone misleads: a stored limit of 50% that is switched
        // off means the inverter is not limited at all, and "enabled" without the value does
        // not say limited to what.
        m.declare(measurement_id::kActivePowerLimitEnabled, MeasurementType::Generic, Unit::None,
                  "Active Power Limit Enabled");
        if (controls_.hasPowerLimit) {
            m.set(measurement_id::kActivePowerLimitPct, controls_.powerLimitPct, ts);
        }
        if (controls_.hasLimitEnabled) {
            m.set(measurement_id::kActivePowerLimitEnabled, controls_.limitEnabled ? 1.0 : 0.0,
                  ts);
        }
    }
    return PollResult::Ok;
}

bool SunspecDriver::refreshControls() {
    if (controlsEntry_ == nullptr) {
        return false;
    }
    std::vector<uint16_t> regs;
    PollResult            ignored = PollResult::Timeout;
    // Best-effort, like the identity read: a controls block that does not answer must not fail
    // a poll whose inverter readings arrived intact. It costs the control surface for this
    // round, which the absent measurement already says.
    const bool ok = readModel(*controlsEntry_, regs, ignored);
    ControlsReadings fresh;
    if (ok && decodeControls(regs.data(), regs.size(), fresh)) {
        controls_     = fresh;
        controlsRead_ = true;
        return true;
    }

    // Give up on a device that advertises the model and has NEVER served it -- but keep
    // retrying one that has.
    //
    // Without this, such a device costs a failed transaction on every poll, forever: the full
    // response timeout added to each cycle, and every one of those failures tallied into the
    // bus error counters that the alerting rules watch. The inverter read still succeeds, so
    // the poll reports Ok while the counters climb -- a healthy bus made to look like a
    // degrading one, by a control surface nobody is using. A device that has answered before
    // is a different case: that is a real control surface and a glitch is worth riding out.
    if (!controlsRead_) {
        log::warn("SUNSPEC model 123 at %u advertised but unreadable -- controls disabled",
                  controlsEntry_->address);
        controlsEntry_ = nullptr;
    }
    return false;
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

    // Write capabilities are the device's, not this driver's. They appear only once model 123
    // has actually been READ -- not merely advertised on the chain -- because the bounds come
    // from the scale factor inside it. A capability offered before that would be a control
    // surface with invented limits, and the first thing anybody does with a power limit is
    // drag it to a number.
    if (!controlsRead_) {
        return c;
    }
    const LimitBounds bounds = limitBounds(controls_.limitScale);
    if (bounds.usable) {
        c.addRead(InverterCapability::SetActivePowerLimit);
        c.addWrite(InverterCapability::SetActivePowerLimit);
        auto& n    = c.numeric[static_cast<size_t>(InverterCommandType::SetActivePowerLimitPercent)];
        n.supported = true;
        n.writable  = true;
        n.minimum   = bounds.minimum;
        n.maximum   = bounds.maximum;
        n.step      = bounds.step;
        n.unit      = Unit::Percent;
    }
    // Conn is a separate point with its own sentinel: a device may implement the limit and not
    // the disconnect, or the other way round, so they are advertised independently rather than
    // as one "controls" capability.
    if (controls_.hasConnection) {
        c.addRead(InverterCapability::StartStop);
        c.addWrite(InverterCapability::StartStop);
    }
    return c;
}

CommandResult SunspecDriver::writeControl(size_t pointOffset, uint16_t value) {
    if (transport_ == nullptr || controlsEntry_ == nullptr) {
        return CommandResult::Unsupported;
    }
    const uint16_t address = static_cast<uint16_t>(controlsEntry_->address + pointOffset);
    const auto     outcome =
        modbus::writeSingleRegister(*transport_, options_.unitId, address, value);
    switch (outcome.status) {
        case modbus::TransactionStatus::Ok:
            return CommandResult::Ok;
        case modbus::TransactionStatus::Exception:
            // The device answered and refused. Distinct from a bus failure: the write reached
            // it, so retrying the same value will be refused again.
            log::warn("SUNSPEC write %u = %u refused, exception %u", address, value,
                      static_cast<unsigned>(outcome.exceptionCode));
            return CommandResult::Rejected;
        case modbus::TransactionStatus::Crc:
            ++busErrors_.checksumErrors;
            return CommandResult::DriverError;
        case modbus::TransactionStatus::Timeout:
            ++busErrors_.timeouts;
            return CommandResult::Timeout;
        default:
            // Protocol / TransportError. Includes the echo not matching what was sent, which is
            // the one failure a write has and a read does not.
            ++busErrors_.invalidFrames;
            log::warn("SUNSPEC write %u = %u was not confirmed", address, value);
            return CommandResult::DriverError;
    }
}

CommandResult SunspecDriver::execute(const InverterCommand& command) {
    // Every gate is a real one: the capability set above is what the dispatcher checks, and it
    // is absent unless this device published a model 123 that could be read. The checks here
    // are the driver's own, for a caller that reached execute() another way.
    if (controlsEntry_ == nullptr || !controlsRead_) {
        return CommandResult::Unsupported;
    }

    switch (command.type) {
        case InverterCommandType::SetActivePowerLimitPercent: {
            if (!command.numericValue.has_value()) {
                return CommandResult::Rejected;
            }
            uint16_t raw = 0;
            if (!encodePowerLimitPct(*command.numericValue, controls_.limitScale, raw)) {
                // Refused rather than clamped: see encodePowerLimitPct. A caller that asked for
                // 150% has made a mistake, and running the inverter at 100% instead would hide
                // it behind an apparently successful command.
                return CommandResult::OutOfRange;
            }
            // Value first, enable second. The other order arms whatever limit the register
            // happened to be holding -- possibly one set by somebody else, possibly the 0% that
            // a device ships with -- for however long the two writes are apart.
            const CommandResult set = writeControl(controls::kWMaxLimPct, raw);
            if (set != CommandResult::Ok) {
                return set;
            }
            return writeControl(controls::kWMaxLim_Ena, controls::kEnabled);
        }
        case InverterCommandType::Start:
            return writeControl(controls::kConn, controls::kConnect);
        case InverterCommandType::Stop:
            return writeControl(controls::kConn, controls::kDisconnect);
        default:
            return CommandResult::Unsupported;
    }
}

}  // namespace heliograph::sunspec
