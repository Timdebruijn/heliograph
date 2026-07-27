// SPDX-License-Identifier: MIT
// See eversolar_driver.h for provenance.

#include "eversolar_driver.h"

#include <cstring>

namespace heliograph::eversolar {
namespace {

/// The reference sends the re-register broadcast 8 times, its own comment says the spec
/// calls for 3. We follow the comment: it is the only statement about the spec we have, and
/// 8 was never justified.
constexpr int kReRegisterRepeats = 3;
/// Consecutive QUERY_NORMAL_INFO timeouts before poll() runs a recovery probe. One is a
/// dropped frame; a run of them means the inverter dropped out. The probe is a NON-disruptive
/// offline query -- it never broadcasts RE_REGISTER, and it never drops the registration, so a
/// working inverter that merely went quiet for a moment is left alone. See poll().
constexpr uint8_t kTimeoutsBeforeRecoveryProbe = 3;

}  // namespace

EversolarOptions optionsFrom(const heliograph::DriverOptions& values) {
    EversolarOptions out;
    const std::string layout = eversolar::descriptor().optionOr(values, "layout");
    if (layout == "single") {
        out.layout = LayoutSelection::ForceSingleString;
    } else if (layout == "dual") {
        out.layout = LayoutSelection::ForceDualString;
    } else {
        out.layout = LayoutSelection::Auto;
    }
    long addr = 0;
    if (eversolar::descriptor().numericOption(values, "address", addr)) {
        out.assignedAddress = static_cast<uint8_t>(addr);
    } else {
        // Not an error: the option is absent on every install that predates it, and its
        // default IS kFirstInverterAddress -- so a single-inverter bridge keeps handing out
        // 0x10 exactly as before, and keeps sending the RE_REGISTER that only the holder of
        // that address sends.
        out.assignedAddress = kFirstInverterAddress;
    }
    return out;
}

EversolarDriver::EversolarDriver(Transport& transport, EversolarOptions options)
    : transport_(&transport), options_(options), address_(options.assignedAddress) {}

const DriverDescriptor& EversolarDriver::descriptor() const { return eversolar::descriptor(); }

bool EversolarDriver::begin(Transport& transport) {
    transport_       = &transport;
    registered_      = false;
    coldStartPending_ = true;  // this session's one RE_REGISTER is still to come
    address_         = options_.assignedAddress;

    const auto& profiles = eversolar::descriptor().recommendedSerialProfiles;
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
    // Deliberately absent: ReadErrors (the protocol carries no error code field) and
    // everything battery/multi-phase. write stays empty.
    capabilities_.phaseCount = 1;
    capabilities_.mpptCount  = 1;  // revised upward if a dual-string payload arrives
    capabilities_.hasBattery = false;

    identity_               = DeviceIdentity{};
    identity_.manufacturer  = "Ever-Solar";
    identity_.protocolName  = "EverSolar PMU RS485";
    identity_.driverId      = eversolar::descriptor().id;
    identity_.instanceKey   = std::to_string(options_.assignedAddress);

    // Only the holder of the first address broadcasts it.
    //
    // RE_REGISTER tells EVERY inverter on the line to forget its address, so an unconditional
    // broadcast here made a second instance's startup de-register the first, already-polling
    // one (#63). It is a BUS operation, not a device one -- the reference implementation calls
    // it once from inverter_connect() and never again (eversolar.pl:822).
    //
    // Derived from the address rather than configured, so there is no extra knob to get wrong
    // and no ordering contract between instances to honour. It also keeps a single-inverter
    // bridge byte-identical: its address is the default 0x10, so it still broadcasts exactly as
    // it always has.
    if (options_.assignedAddress == kFirstInverterAddress) {
        reRegisterAll();
    }
    return true;
}

EversolarDriver::TransactResult EversolarDriver::transact(CommandCode command,
                                                          Address     destination,
                                                          const uint8_t* data, size_t dataLen,
                                                          Address expectedSource,
                                                          uint8_t* payloadOut,
                                                          size_t   payloadCapacity,
                                                          size_t&  payloadLen,
                                                          const Address* altSource) {
    payloadLen = 0;
    if (transport_ == nullptr) {
        return TransactResult::TransportError;
    }

    uint8_t request[kMaxFrameSize];
    size_t  requestLen = 0;
    if (buildRequest(command, destination, data, dataLen, request, sizeof(request), requestLen) !=
        BuildResult::Ok) {
        return TransactResult::TransportError;
    }

    const pmu::Exchange exchange{request, requestLen, command, expectedSource, altSource};
    return pmu::transact(*transport_, exchange, payloadOut, payloadCapacity, payloadLen, tally_,
                         "RS485");
}

void EversolarDriver::reRegisterAll() {
    if (transport_ == nullptr) {
        return;
    }
    TransportLock lock(*transport_, pmu::kBusLockTimeoutMs);
    if (!lock.held()) {
        return;
    }
    uint8_t request[kMaxFrameSize];
    size_t  len = 0;
    if (buildRequest(cmd::kReRegister, kBroadcastAddress, nullptr, 0, request, sizeof(request),
                     len) != BuildResult::Ok) {
        return;
    }
    // No response is defined for this command, so there is nothing to wait for or verify.
    for (int i = 0; i < kReRegisterRepeats; ++i) {
        transport_->write(request, len);
    }
}

bool EversolarDriver::registerDevice(ProbeResult* probeOut, bool allowReRegister) {
    uint8_t payload[kMaxDataLength];
    size_t  payloadLen = 0;

    // 1. Ask any unregistered inverter to announce its serial number. A real TL3000-20
    // answers from 00 00 (it has no address yet; hardware, 2026-07-19); 00 <assigned> stays
    // accepted in case other firmware follows the old fixture hypothesis.
    const Address assigned = inverterAddress(address_);
    auto r = transact(cmd::kOfflineQuery, kBroadcastAddress, nullptr, 0,
                      kBroadcastAddress, payload, sizeof(payload), payloadLen, &assigned);
    if (r != TransactResult::Ok) {
        // Silence can mean the inverter is REGISTERED (it ignores offline queries by design)
        // or simply absent. On a COLD start the first case needs breaking: the inverter kept
        // an address across our reboot and would ignore offline queries forever, so broadcast
        // "forget your address" once and retry. During normal polling we must NOT do this --
        // it knocks a working inverter out, and a returned address-less inverter answers the
        // plain query above anyway. See the header note and 2026-07-21.
        if (!allowReRegister) {
            return false;
        }
        reRegisterAll();
        r = transact(cmd::kOfflineQuery, kBroadcastAddress, nullptr, 0,
                     kBroadcastAddress, payload, sizeof(payload), payloadLen, &assigned);
        if (r != TransactResult::Ok) {
            return false;
        }
    }
    if (probeOut != nullptr) {
        probeOut->responded     = true;
        probeOut->checksumValid = true;
        probeOut->confidenceScore += 40;
        probeOut->evidence.push_back("offline query answered with a valid checksum");
    }

    std::string serial;
    if (!parseSerialNumber(payload, payloadLen, serial)) {
        if (probeOut != nullptr) {
            probeOut->evidence.push_back("offline query payload was not printable ASCII");
        }
        return false;
    }
    if (probeOut != nullptr) {
        probeOut->serialNumber = serial;
        probeOut->confidenceScore += 25;
        probeOut->evidence.push_back("serial number decoded: " + serial);
    }

    // 2. Hand it an address. The serial echoed back plus the address we allocate.
    uint8_t assign[kMaxDataLength];
    const size_t serialLen = serial.size() < sizeof(assign) - 1 ? serial.size() : sizeof(assign) - 1;
    std::memcpy(assign, serial.data(), serialLen);
    assign[serialLen] = address_;

    // Which source acknowledges is unproven on hardware: the inverter may still say 00 00 or
    // already use its fresh address. Accept both; the TRACE capture will pin it down.
    r = transact(cmd::kSendAddress, kBroadcastAddress, assign, serialLen + 1,
                 inverterAddress(address_), payload, sizeof(payload), payloadLen,
                 &kBroadcastAddress);
    if (r != TransactResult::Ok || !isRegistrationAck(payload, payloadLen)) {
        if (probeOut != nullptr) {
            probeOut->evidence.push_back("address assignment was not acknowledged with 0x06");
        }
        return false;
    }
    if (probeOut != nullptr) {
        probeOut->confidenceScore += 20;
        probeOut->evidence.push_back("registration acknowledged (0x06)");
    }

    // 3. Read the identification string.
    std::string idString;
    // The id string is a composite (phases, firmware, model, manufacturer, serial) and using
    // it verbatim as the model made every Home Assistant name unreadable. Extract the bare
    // model where the manufacturer anchor allows it; keep the raw string otherwise.
    r = transact(cmd::kQueryInverterId, inverterAddress(address_), nullptr, 0,
                 inverterAddress(address_), payload, sizeof(payload), payloadLen);
    if (r == TransactResult::Ok && parseInverterId(payload, payloadLen, idString)) {
        const std::string parsedModel = modelFromIdString(idString, "Ever-Solar");
        const std::string& model      = parsedModel.empty() ? idString : parsedModel;
        if (probeOut != nullptr) {
            probeOut->confidenceScore += 10;
            probeOut->evidence.push_back("inverter id string: " + idString);
            probeOut->detectedManufacturer = "Ever-Solar";
            probeOut->detectedModel        = model;
        }
        identity_.model = model;
    }

    identity_.serialNumber = serial;
    registered_            = true;
    return true;
}

ProbeResult EversolarDriver::probe() {
    ProbeResult result;
    if (transport_ == nullptr) {
        return result;
    }

    registered_ = false;
    reRegisterAll();
    // Discovery is a cold start by definition: allow the RE_REGISTER fallback.
    if (!registerDevice(&result, /*allowReRegister=*/true)) {
        return result;
    }

    // A registration alone could in principle be coincidence. Reading real measurements of a
    // plausible size is the last piece of evidence.
    uint8_t payload[kMaxDataLength];
    size_t  payloadLen = 0;
    const auto r = transact(cmd::kQueryNormalInfo, inverterAddress(address_), nullptr, 0,
                            inverterAddress(address_), payload, sizeof(payload), payloadLen);
    if (r == TransactResult::Ok) {
        NormalInfo info;
        if (decodeNormalInfo(payload, payloadLen, info, options_.layout) == DecodeResult::Ok) {
            result.confidenceScore += 5;
            result.evidence.push_back(
                std::string("measurement payload decoded (") +
                (info.layout == NormalInfoLayout::SingleString ? "28" : "32") + " bytes)");
        } else {
            result.evidence.push_back("measurement payload had an unexpected length");
        }
    }

    if (result.confidenceScore > 100) {
        result.confidenceScore = 100;
    }
    return result;
}

void EversolarDriver::declareChannels(bool dualString) {
    if (channelsDeclared_ && declaredDual_ == dualString) {
        return;
    }
    channelsDeclared_ = true;
    declaredDual_     = dualString;
}

PollResult EversolarDriver::poll(DeviceState& state) {
    if (transport_ == nullptr) {
        return PollResult::TransportError;
    }
    // Registration is established once and then KEPT. The morning failure mode (2026-07-21) was
    // the opposite: a run of timeouts dropped the registration, the next poll re-registered
    // with a RE_REGISTER broadcast, and the measurement query fired in the same breath -- so
    // every query landed right after a fresh registration and the inverter, which flickers on
    // and off at the dawn sun threshold, ignored it. Overnight this looped forever. The
    // reference logger instead registers once, keeps polling the address patiently, and only
    // broadcasts at connect time. That is what this does.
    if (!registered_) {
        if (!registerDevice(nullptr, /*allowReRegister=*/coldStartPending_)) {
            return PollResult::NotRegistered;
        }
        coldStartPending_    = false;
        consecutiveTimeouts_ = 0;
    } else if (consecutiveTimeouts_ >= kTimeoutsBeforeRecoveryProbe) {
        // Registered but quiet for a while. An inverter that dropped out and came back has
        // lost its volatile address and answers the offline query again; pick it up with a
        // NON-disruptive probe (no broadcast). A still-registered inverter ignores the query,
        // so this cannot knock a merely-quiet one off the bus -- we just keep polling it.
        if (registerDevice(nullptr, /*allowReRegister=*/false)) {
            consecutiveTimeouts_ = 0;
        }
    }

    uint8_t payload[kMaxDataLength];
    size_t  payloadLen = 0;
    const auto r = transact(cmd::kQueryNormalInfo, inverterAddress(address_), nullptr, 0,
                            inverterAddress(address_), payload, sizeof(payload), payloadLen);
    switch (r) {
        case TransactResult::Timeout:
            // Registration is deliberately NOT dropped here. We keep polling the address; the
            // recovery probe above handles a genuinely returned inverter.
            ++consecutiveTimeouts_;
            return PollResult::Timeout;
        case TransactResult::ChecksumError:  return PollResult::ChecksumError;
        case TransactResult::InvalidFrame:   return PollResult::InvalidFrame;
        case TransactResult::TransportError: return PollResult::TransportError;
        case TransactResult::Ok:             break;
    }
    consecutiveTimeouts_ = 0;

    NormalInfo info;
    if (decodeNormalInfo(payload, payloadLen, info, options_.layout) != DecodeResult::Ok) {
        ++tally_.invalidFrames;
        return PollResult::InvalidFrame;
    }

    // Past this point the frame is fully validated, so touching `state` is safe.
    const bool dual = info.hasSecondString;
    declareChannels(dual);
    capabilities_.mpptCount = dual ? 2 : 1;
    if (dual) {
        capabilities_.addRead(InverterCapability::ReadMultipleMppts);
    }

    auto& m = state.measurements;
    m.declare(measurement_id::kAcPowerTotal, MeasurementType::Power, Unit::Watt, "AC Power");
    m.declare(measurement_id::kAcL1Voltage, MeasurementType::Voltage, Unit::Volt, "AC Voltage");
    m.declare(measurement_id::kAcL1Current, MeasurementType::Current, Unit::Ampere, "AC Current");
    m.declare(measurement_id::kAcFrequency, MeasurementType::Frequency, Unit::Hertz, "Grid Frequency");
    m.declare(measurement_id::kDcMppt1Voltage, MeasurementType::Voltage, Unit::Volt, "PV1 Voltage");
    m.declare(measurement_id::kDcMppt1Current, MeasurementType::Current, Unit::Ampere, "PV1 Current");
    m.declare(measurement_id::kDcMppt1Power, MeasurementType::Power, Unit::Watt, "PV1 Power", true);
    m.declare(measurement_id::kDcPowerTotal, MeasurementType::Power, Unit::Watt, "DC Power", true);
    m.declare(measurement_id::kEnergyToday, MeasurementType::Energy, Unit::KilowattHour, "Energy Today");
    m.declare(measurement_id::kEnergyTotal, MeasurementType::Energy, Unit::KilowattHour, "Total Energy");
    m.declare(measurement_id::kTemperature, MeasurementType::Temperature, Unit::Celsius, "Temperature");
    m.declare(measurement_id::kOperatingHours, MeasurementType::Duration, Unit::Hour, "Operating Hours");
    if (dual) {
        m.declare(measurement_id::kDcMppt2Voltage, MeasurementType::Voltage, Unit::Volt, "PV2 Voltage");
        m.declare(measurement_id::kDcMppt2Current, MeasurementType::Current, Unit::Ampere, "PV2 Current");
        m.declare(measurement_id::kDcMppt2Power, MeasurementType::Power, Unit::Watt, "PV2 Power", true);
    }

    const uint64_t ts = state.lastPollAttemptMs;
    m.set(measurement_id::kAcPowerTotal, info.acPowerW, ts);
    m.set(measurement_id::kAcL1Voltage, info.acVoltage, ts);
    m.set(measurement_id::kAcL1Current, info.acCurrent, ts);
    m.set(measurement_id::kAcFrequency, info.frequencyHz, ts);
    m.set(measurement_id::kDcMppt1Voltage, info.pvVoltage1, ts);
    m.set(measurement_id::kDcMppt1Current, info.pvCurrent1, ts);

    const double dcPower1 = info.pvVoltage1 * info.pvCurrent1;
    m.set(measurement_id::kDcMppt1Power, dcPower1, ts);
    double dcTotal = dcPower1;
    if (dual) {
        const double dcPower2 = info.pvVoltage2 * info.pvCurrent2;
        m.set(measurement_id::kDcMppt2Voltage, info.pvVoltage2, ts);
        m.set(measurement_id::kDcMppt2Current, info.pvCurrent2, ts);
        m.set(measurement_id::kDcMppt2Power, dcPower2, ts);
        dcTotal += dcPower2;
    }
    m.set(measurement_id::kDcPowerTotal, dcTotal, ts);

    m.set(measurement_id::kEnergyToday, info.energyTodayKwh, ts);
    m.set(measurement_id::kEnergyTotal, info.energyTotalKwh, ts);
    m.set(measurement_id::kTemperature, info.temperatureC, ts);
    m.set(measurement_id::kOperatingHours, static_cast<double>(info.operatingHours), ts);

    state.statusCode          = info.statusCode;
    state.statusCodeSupported = true;
    // Observed meanings only (see opModeText for the per-code evidence); anything never
    // seen on hardware stays an honest "Unknown (n)" until the day/night captures grow
    // the map further.
    const char* mode = opModeText(info.statusCode);
    state.statusText = mode[0] != '\0'
                           ? std::string(mode)
                           : "Unknown (" + std::to_string(info.statusCode) + ")";
    state.errorCodeSupported = false;
    state.errorCode          = 0;

    state.identity     = identity_;
    state.capabilities = capabilities_;
    return PollResult::Ok;
}

DeviceIdentity EversolarDriver::identity() const { return identity_; }

InverterCapabilities EversolarDriver::capabilities() const { return capabilities_; }

CommandResult EversolarDriver::execute(const InverterCommand& command) {
    (void)command;
    return CommandResult::Unsupported;
}

}  // namespace heliograph::eversolar
