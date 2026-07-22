// SPDX-License-Identifier: MIT
// See solax_driver.h for provenance and the hardware-verification status.

#include "solax_driver.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "diagnostics/logger.h"

namespace heliograph::solax {
namespace {

constexpr uint32_t kResponseTimeoutMs = 1000;
/// Wall-clock ceiling on one transact() exchange; same reasoning as the sibling PMU driver.
constexpr uint32_t kTransactionDeadlineMs = 3000;
constexpr size_t   kRxBufferSize          = kMaxFrameSize + 16;
constexpr uint32_t kBusLockTimeoutMs      = 2000;
/// Consecutive status-report timeouts before poll() runs the non-disruptive recovery probe.
/// One is a dropped frame; a run means the inverter dropped out (e.g. powered down at dusk).
constexpr uint8_t kTimeoutsBeforeRecoveryProbe = 3;

}  // namespace

SolaxOptions optionsFrom(const heliograph::DriverOptions& values) {
    SolaxOptions out;
    const std::string addr = solax::descriptor().optionOr(values, "address");
    if (!addr.empty()) {
        // Base 10, not 0: auto-detection would silently read "010" as octal 8 (a valid
        // address, so no warning would fire). Same pattern as the growatt unit_id parse.
        const long parsed = strtol(addr.c_str(), nullptr, 10);
        if (parsed >= 0x01 && parsed <= 0xFE) {
            out.assignedAddress = static_cast<uint8_t>(parsed);
        } else {
            log::warn("SOLAX address '%s' invalid (want 1-254), using 0x%02X", addr.c_str(),
                      out.assignedAddress);
        }
    }
    return out;
}

SolaxDriver::SolaxDriver(Transport& transport, SolaxOptions options)
    : transport_(&transport), options_(options) {}

const DriverDescriptor& SolaxDriver::descriptor() const { return solax::descriptor(); }

bool SolaxDriver::begin(Transport& transport) {
    transport_  = &transport;
    registered_ = false;
    consecutiveTimeouts_ = 0;
    pv2Declared_         = false;

    // Configure the line for this protocol (9600 8N1). Same rule as every driver: a boot
    // straight into this driver must not poll an unconfigured UART.
    const auto& profiles = solax::descriptor().recommendedSerialProfiles;
    if (profiles.empty() || !transport_->configure(profiles.front())) {
        return false;
    }

    capabilities_ = InverterCapabilities{};
    capabilities_.addRead(InverterCapability::ReadAcPower);
    capabilities_.addRead(InverterCapability::ReadAcVoltage);
    capabilities_.addRead(InverterCapability::ReadAcCurrent);
    capabilities_.addRead(InverterCapability::ReadGridFrequency);
    capabilities_.addRead(InverterCapability::ReadDcVoltage);
    capabilities_.addRead(InverterCapability::ReadDcCurrent);
    capabilities_.addRead(InverterCapability::ReadDcPower);
    capabilities_.addRead(InverterCapability::ReadEnergyToday);
    capabilities_.addRead(InverterCapability::ReadEnergyTotal);
    capabilities_.addRead(InverterCapability::ReadTemperature);
    capabilities_.addRead(InverterCapability::ReadOperatingHours);
    capabilities_.addRead(InverterCapability::ReadStatus);
    capabilities_.addRead(InverterCapability::ReadErrors);  // 32-bit fault bitmask
    capabilities_.phaseCount = 1;
    capabilities_.mpptCount  = 1;  // datasheet; revised upward if PV2 shows real voltage
    capabilities_.hasBattery = false;

    identity_              = DeviceIdentity{};
    identity_.manufacturer = "SolaX";
    identity_.protocolName = "SolaX X1 RS485 (PMU)";
    identity_.driverId     = solax::descriptor().id;
    return true;
}

SolaxDriver::TransactResult SolaxDriver::transact(Address source, CommandCode command,
                                                  Address destination, const uint8_t* data,
                                                  size_t dataLen, Address expectedSource,
                                                  uint8_t* payloadOut, size_t payloadCapacity,
                                                  size_t& payloadLen, const Address* altSource) {
    payloadLen = 0;
    if (transport_ == nullptr) {
        return TransactResult::TransportError;
    }

    TransportLock lock(*transport_, kBusLockTimeoutMs);
    if (!lock.held()) {
        return TransactResult::TransportError;
    }

    uint8_t request[kMaxFrameSize];
    size_t  requestLen = 0;
    if (buildRequestFrom(source, command, destination, data, dataLen, request, sizeof(request),
                         requestLen) != BuildResult::Ok) {
        return TransactResult::TransportError;
    }

    transport_->flushInput();
    if (transport_->write(request, requestLen) != requestLen) {
        log::trace("RS485 tx failed: %u byte(s) not fully written",
                   static_cast<unsigned>(requestLen));
        return TransactResult::TransportError;
    }

    uint8_t rx[kRxBufferSize];
    size_t  have             = 0;
    bool    sawChecksumError = false;
    size_t  received         = 0;
    size_t  rejected         = 0;
    uint8_t rejSrc[2]        = {0, 0};
    uint8_t rejCtrl = 0, rejFn = 0;
    const auto traceOutcome = [&](const char* outcome) {
        if (!log::enabled(LogLevel::Trace)) {
            return;
        }
        if (rejected > 0) {
            log::trace("SOLAX %s: %u byte(s), %u frame(s) rejected, last from %02X %02X ctrl %02X fn %02X",
                       outcome, static_cast<unsigned>(received), static_cast<unsigned>(rejected),
                       rejSrc[0], rejSrc[1], rejCtrl, rejFn);
        } else {
            log::trace("SOLAX %s: %u byte(s) received", outcome, static_cast<unsigned>(received));
        }
        if (received > 0) {
            log::traceHex("SOLAX RX", rx, have);
        }
    };

    const uint64_t deadline = transport_->nowMs() + kTransactionDeadlineMs;

    for (;;) {
        if (transport_->nowMs() >= deadline) {
            ++timeouts_;
            traceOutcome("transaction deadline exceeded");
            return TransactResult::Timeout;
        }

        Frame      frame;
        const auto parsed = parseFrame(rx, have, frame);

        if (parsed == ParseResult::Ok) {
            auto valid = validateResponse(frame, command, expectedSource);
            if (valid == ParseResult::WrongSource && altSource != nullptr) {
                valid = validateResponse(frame, command, *altSource);
            }
            if (valid == ParseResult::Ok) {
                if (frame.dataLength > payloadCapacity) {
                    traceOutcome("payload too large");
                    return TransactResult::InvalidFrame;
                }
                if (frame.dataLength > 0) {
                    std::memcpy(payloadOut, frame.data, frame.dataLength);
                }
                payloadLen = frame.dataLength;
                traceOutcome("ok");
                return TransactResult::Ok;
            }
            // A well-formed frame that is not ours (our own echo, or another master's
            // traffic). Drop it, record it, keep looking.
            ++rejected;
            rejSrc[0] = frame.source.high;
            rejSrc[1] = frame.source.low;
            rejCtrl   = frame.control;
            rejFn     = frame.function;
            std::memmove(rx, rx + frame.frameLength, have - frame.frameLength);
            have -= frame.frameLength;
            continue;
        }

        if (parsed == ParseResult::BadHeader) {
            std::memmove(rx, rx + 1, have - 1);
            --have;
            continue;
        }

        if (parsed == ParseResult::BadChecksum) {
            sawChecksumError = true;
            ++checksumErrors_;
            const size_t skip = frame.frameLength > 0 ? frame.frameLength : 1;
            const size_t n    = skip < have ? skip : have;
            std::memmove(rx, rx + n, have - n);
            have -= n;
            continue;
        }

        // Incomplete: read more.
        if (have >= sizeof(rx)) {
            ++invalidFrames_;
            traceOutcome("buffer full without a valid frame");
            return TransactResult::InvalidFrame;
        }
        const size_t n = transport_->read(rx + have, sizeof(rx) - have, kResponseTimeoutMs);
        if (n == 0) {
            if (sawChecksumError) {
                traceOutcome("checksum error");
                return TransactResult::ChecksumError;
            }
            ++timeouts_;
            traceOutcome(received > 0 ? "timeout after partial/rejected data" : "silent");
            return TransactResult::Timeout;
        }
        have += n;
        received += n;
    }
}

bool SolaxDriver::verifyByStatusQuery() {
    uint8_t payload[kMaxDataLength];
    size_t  payloadLen = 0;
    const Address addr = inverterAddress(options_.assignedAddress);
    const auto r = transact(kPmuAddress, cmd::kQueryNormalInfo, addr, nullptr, 0, addr, payload,
                            sizeof(payload), payloadLen);
    if (r != TransactResult::Ok) {
        return false;
    }
    StatusReport report;
    return decodeStatusReport(payload, payloadLen, report) == DecodeResult::Ok;
}

bool SolaxDriver::registerDevice(ProbeResult* probeOut) {
    uint8_t payload[kMaxDataLength];
    size_t  payloadLen = 0;

    // 1. Ask any unregistered inverter to announce itself. An unregistered X1 answers from
    // 00 00 (it has no address yet); accept the assigned address too in case a firmware
    // answers with the address it still holds.
    const Address assigned = inverterAddress(options_.assignedAddress);
    auto r = transact(kPmuAddress, cmd::kOfflineQuery, kBroadcastAddress, nullptr, 0,
                      kBroadcastAddress, payload, sizeof(payload), payloadLen, &assigned);
    if (r != TransactResult::Ok) {
        // Silence can mean the inverter kept OUR deterministic address across a bridge
        // reboot and, per the family protocol, ignores offline queries while registered.
        // There is no RE_REGISTER in this driver (see header): query the address directly
        // instead -- non-disruptive, and conclusive either way.
        if (verifyByStatusQuery()) {
            registered_ = true;
            if (probeOut != nullptr) {
                probeOut->responded     = true;
                probeOut->checksumValid = true;
                probeOut->confidenceScore += 40;
                probeOut->evidence.push_back(
                    "inverter still registered from a previous session answered a status query");
            }
            // This is the COMMON restart path (the ESP reboots far more often than the
            // inverter loses power), and registration is sticky afterwards -- skipping the
            // identity read here left model/serial empty for the whole session (review,
            // 2026-07-21).
            readDeviceInfo(probeOut);
            return true;
        }
        return false;
    }
    if (probeOut != nullptr) {
        probeOut->responded     = true;
        probeOut->checksumValid = true;
        probeOut->confidenceScore += 40;
        probeOut->evidence.push_back("offline query answered with a valid checksum");
    }

    if (!serialLooksValid(payload, payloadLen)) {
        if (probeOut != nullptr) {
            probeOut->evidence.push_back("announcement payload was not a plausible serial");
        }
        return false;
    }
    std::memcpy(serial_, payload, kSerialNumberBytes);

    std::string printable;
    for (size_t i = 0; i < kSerialNumberBytes; ++i) {
        if (serial_[i] > 0x20 && serial_[i] < 0x7F) {
            printable.push_back(static_cast<char>(serial_[i]));
        }
    }
    if (probeOut != nullptr) {
        probeOut->serialNumber = printable;
        probeOut->confidenceScore += 25;
        probeOut->evidence.push_back("serial number announced: " + printable);
    }

    // 2. Hand it the address: the 14 serial bytes echoed verbatim plus our address. The
    // reference sends this frame with source 00 00; we mirror that exactly.
    uint8_t assign[kSerialNumberBytes + 1];
    std::memcpy(assign, serial_, kSerialNumberBytes);
    assign[kSerialNumberBytes] = options_.assignedAddress;

    r = transact(kBroadcastAddress, cmd::kSendAddress, kBroadcastAddress, assign, sizeof(assign),
                 assigned, payload, sizeof(payload), payloadLen, &kBroadcastAddress);
    const bool acked = r == TransactResult::Ok && payloadLen >= 1 && payload[0] == kRegisterAck;
    if (!acked) {
        // The reference implementation never waits for this ACK at all, so its absence is
        // weak evidence. The status query is the ground truth: if the inverter answers at
        // the address we just assigned, the registration took.
        if (!verifyByStatusQuery()) {
            if (probeOut != nullptr) {
                probeOut->evidence.push_back(
                    "address assignment not acknowledged and no answer at the assigned address");
            }
            return false;
        }
    }
    if (probeOut != nullptr) {
        probeOut->confidenceScore += 20;
        probeOut->evidence.push_back(acked ? "registration acknowledged (0x06)"
                                           : "registration verified by a status query");
    }

    registered_ = true;
    readDeviceInfo(probeOut);
    return true;
}

void SolaxDriver::readDeviceInfo(ProbeResult* probeOut) {
    uint8_t payload[kMaxDataLength];
    size_t  payloadLen = 0;
    const Address addr = inverterAddress(options_.assignedAddress);
    const auto r = transact(kPmuAddress, cmd::kQueryInverterId, addr, nullptr, 0, addr, payload,
                            sizeof(payload), payloadLen);
    if (r != TransactResult::Ok) {
        return;  // identity stays at driver defaults; never invented
    }
    DeviceInfo info;
    if (!decodeDeviceInfo(payload, payloadLen, info)) {
        log::trace("SOLAX device info had unexpected length %u",
                   static_cast<unsigned>(payloadLen));
        return;
    }
    if (!info.factoryName.empty()) {
        identity_.manufacturer = info.factoryName;
    }
    if (!info.moduleName.empty()) {
        identity_.model = info.moduleName;
    }
    identity_.firmwareVersion = info.firmwareVersion;
    // Prefer the structured serial from device info over the raw registration bytes: this
    // one is documented as ASCII.
    if (!info.serialNumber.empty()) {
        identity_.serialNumber = info.serialNumber;
    }
    if (probeOut != nullptr) {
        probeOut->confidenceScore += 10;
        probeOut->detectedManufacturer = identity_.manufacturer;
        probeOut->detectedModel        = identity_.model;
        probeOut->evidence.push_back("device info decoded: " + info.moduleName);
    }
}

ProbeResult SolaxDriver::probe() {
    ProbeResult result;
    if (transport_ == nullptr) {
        return result;
    }
    registered_ = false;
    if (!registerDevice(&result)) {
        return result;
    }
    if (result.confidenceScore > 100) {
        result.confidenceScore = 100;
    }
    return result;
}

PollResult SolaxDriver::poll(DeviceState& state) {
    if (transport_ == nullptr) {
        return PollResult::TransportError;
    }
    // Registration discipline inherited from the sunrise incident (2026-07-21): establish
    // once, KEEP it through timeouts, recover with non-disruptive probes only.
    if (!registered_) {
        if (!registerDevice(nullptr)) {
            return PollResult::NotRegistered;
        }
        consecutiveTimeouts_ = 0;
    } else if (consecutiveTimeouts_ >= kTimeoutsBeforeRecoveryProbe) {
        // Registered but quiet for a while. An inverter that powered down and came back has
        // lost its volatile address and answers the offline query again; a still-registered
        // one ignores it. Either way this cannot knock a working inverter off the bus.
        if (registerDevice(nullptr)) {
            consecutiveTimeouts_ = 0;
        }
    }

    uint8_t payload[kMaxDataLength];
    size_t  payloadLen = 0;
    const Address addr = inverterAddress(options_.assignedAddress);
    const auto r = transact(kPmuAddress, cmd::kQueryNormalInfo, addr, nullptr, 0, addr, payload,
                            sizeof(payload), payloadLen);
    switch (r) {
        case TransactResult::Timeout:
            // Registration is deliberately NOT dropped here; see the sibling driver.
            ++consecutiveTimeouts_;
            return PollResult::Timeout;
        case TransactResult::ChecksumError:  return PollResult::ChecksumError;
        case TransactResult::InvalidFrame:   return PollResult::InvalidFrame;
        case TransactResult::TransportError: return PollResult::TransportError;
        case TransactResult::Ok:             break;
    }
    consecutiveTimeouts_ = 0;

    StatusReport report;
    if (decodeStatusReport(payload, payloadLen, report) != DecodeResult::Ok) {
        ++invalidFrames_;
        log::trace("SOLAX status report too short: %u byte(s)",
                   static_cast<unsigned>(payloadLen));
        return PollResult::InvalidFrame;
    }

    // The datasheet says the Mini family has one MPPT, but the payload carries two field
    // sets. A second MPPT proves itself by real voltage; once seen, it stays declared so
    // the channel does not flap.
    if (!pv2Declared_ && report.pv2Voltage > 1.0) {
        pv2Declared_ = true;
    }
    capabilities_.mpptCount = pv2Declared_ ? 2 : 1;
    if (pv2Declared_) {
        capabilities_.addRead(InverterCapability::ReadMultipleMppts);
    }

    auto& m = state.measurements;
    m.declare(measurement_id::kAcPowerTotal, MeasurementType::Power, Unit::Watt, "AC Power");
    m.declare(measurement_id::kAcL1Voltage, MeasurementType::Voltage, Unit::Volt, "AC Voltage");
    m.declare(measurement_id::kAcL1Current, MeasurementType::Current, Unit::Ampere, "AC Current");
    m.declare(measurement_id::kAcFrequency, MeasurementType::Frequency, Unit::Hertz,
              "Grid Frequency");
    m.declare(measurement_id::kDcMppt1Voltage, MeasurementType::Voltage, Unit::Volt, "PV1 Voltage");
    m.declare(measurement_id::kDcMppt1Current, MeasurementType::Current, Unit::Ampere,
              "PV1 Current");
    m.declare(measurement_id::kDcMppt1Power, MeasurementType::Power, Unit::Watt, "PV1 Power", true);
    m.declare(measurement_id::kDcPowerTotal, MeasurementType::Power, Unit::Watt, "DC Power", true);
    m.declare(measurement_id::kEnergyToday, MeasurementType::Energy, Unit::KilowattHour,
              "Energy Today");
    m.declare(measurement_id::kEnergyTotal, MeasurementType::Energy, Unit::KilowattHour,
              "Total Energy");
    m.declare(measurement_id::kTemperature, MeasurementType::Temperature, Unit::Celsius,
              "Temperature");
    m.declare(measurement_id::kOperatingHours, MeasurementType::Duration, Unit::Hour,
              "Operating Hours");
    if (pv2Declared_) {
        m.declare(measurement_id::kDcMppt2Voltage, MeasurementType::Voltage, Unit::Volt,
                  "PV2 Voltage");
        m.declare(measurement_id::kDcMppt2Current, MeasurementType::Current, Unit::Ampere,
                  "PV2 Current");
        m.declare(measurement_id::kDcMppt2Power, MeasurementType::Power, Unit::Watt, "PV2 Power",
                  true);
    }

    const uint64_t ts = state.lastPollAttemptMs;
    m.set(measurement_id::kAcPowerTotal, report.acPowerW, ts);
    m.set(measurement_id::kAcL1Voltage, report.acVoltage, ts);
    m.set(measurement_id::kAcL1Current, report.acCurrent, ts);
    m.set(measurement_id::kAcFrequency, report.frequencyHz, ts);
    m.set(measurement_id::kDcMppt1Voltage, report.pv1Voltage, ts);
    m.set(measurement_id::kDcMppt1Current, report.pv1Current, ts);

    const double dcPower1 = report.pv1Voltage * report.pv1Current;
    m.set(measurement_id::kDcMppt1Power, dcPower1, ts);
    double dcTotal = dcPower1;
    if (pv2Declared_) {
        const double dcPower2 = report.pv2Voltage * report.pv2Current;
        m.set(measurement_id::kDcMppt2Voltage, report.pv2Voltage, ts);
        m.set(measurement_id::kDcMppt2Current, report.pv2Current, ts);
        m.set(measurement_id::kDcMppt2Power, dcPower2, ts);
        dcTotal += dcPower2;
    }
    m.set(measurement_id::kDcPowerTotal, dcTotal, ts);

    m.set(measurement_id::kEnergyToday, report.energyTodayKwh, ts);
    m.set(measurement_id::kEnergyTotal, report.energyTotalKwh, ts);
    m.set(measurement_id::kTemperature, report.temperatureC, ts);
    m.set(measurement_id::kOperatingHours, static_cast<double>(report.runtimeHours), ts);

    state.statusCode = report.mode;
    // The mode table is documented (0 Wait .. 6 Self Test); a code outside it is reported
    // as unknown rather than named.
    const char* mode = modeText(report.mode);
    state.statusText = mode[0] != '\0'
                           ? std::string(mode)
                           : "Unknown (" + std::to_string(report.mode) + ")";
    state.errorCodeSupported = true;
    state.errorCode          = report.errorBits;

    state.identity     = identity_;
    state.capabilities = capabilities_;
    return PollResult::Ok;
}

DeviceIdentity SolaxDriver::identity() const { return identity_; }

InverterCapabilities SolaxDriver::capabilities() const { return capabilities_; }

CommandResult SolaxDriver::execute(const InverterCommand& command) {
    (void)command;
    return CommandResult::Unsupported;
}

}  // namespace heliograph::solax
