// SPDX-License-Identifier: MIT

#include "solarmax_driver.h"

#include <cmath>
#include <cstring>

#include "diagnostics/logger.h"
#include "transport/transport.h"

namespace heliograph::solarmax {
namespace {

/// 19200 8N1, the one line setting both sources give for this family.
constexpr SerialProfile kLine{19200, SerialParity::None, 8, 1, 1500};

constexpr uint32_t kBusLockTimeoutMs = 2000;
/// A reply to a dozen codes is around 130 characters at 19200 baud, so under 100 ms on the wire.
/// The rest is the device's own turnaround, which neither source specifies.
constexpr uint32_t kReplyTimeoutMs = 1500;

/// What each query code carries, and what to divide it by.
///
/// THE DIVISORS ARE THE WEAK PART OF THIS DRIVER. Frame shape, checksum, addressing and line
/// settings are agreed by two independent sources; of the entries below only PAC, KDY, UL1 and
/// IL1 are. The rest come from one implementation and have never met a device.
///
/// Two codes are deliberately ABSENT:
///
///   UDC -- the two sources disagree outright: one scales it like AC voltage (divide by ten), the
///          other leaves it raw. Picking a winner is exactly what the two-source rule exists to
///          prevent, and a DC voltage wrong by a factor of ten is the kind of number an operator
///          would believe. It stays unmapped until a device settles it.
///   IDC -- omitted with it. Alone it would be a DC current with no voltage beside it, and it may
///          simply be another name for ID01 on a single-string unit; nothing in the sources says.
///
/// The per-string codes below cover the same ground without the ambiguity, so a device with
/// string readings loses nothing. A device that reports only the totals will show no DC channels
/// at all, which is the honest outcome rather than a plausible wrong one.
struct CodeMapping {
    const char*     code;
    const char*     measurementId;
    double          divisor;
    MeasurementType type;
    Unit            unit;
    const char*     displayName;
};

constexpr CodeMapping kMappings[] = {
    {"PAC", measurement_id::kAcPowerTotal, 2.0, MeasurementType::Power, Unit::Watt, "AC power"},
    {"PDC", measurement_id::kDcPowerTotal, 2.0, MeasurementType::Power, Unit::Watt, "DC power"},
    {"UL1", measurement_id::kAcL1Voltage, 10.0, MeasurementType::Voltage, Unit::Volt, "L1 voltage"},
    {"UL2", measurement_id::kAcL2Voltage, 10.0, MeasurementType::Voltage, Unit::Volt, "L2 voltage"},
    {"UL3", measurement_id::kAcL3Voltage, 10.0, MeasurementType::Voltage, Unit::Volt, "L3 voltage"},
    {"IL1", measurement_id::kAcL1Current, 100.0, MeasurementType::Current, Unit::Ampere, "L1 current"},
    {"IL2", measurement_id::kAcL2Current, 100.0, MeasurementType::Current, Unit::Ampere, "L2 current"},
    {"IL3", measurement_id::kAcL3Current, 100.0, MeasurementType::Current, Unit::Ampere, "L3 current"},
    {"TNF", measurement_id::kAcFrequency, 100.0, MeasurementType::Frequency, Unit::Hertz, "Grid frequency"},
    {"TKK", measurement_id::kTemperature, 1.0, MeasurementType::Temperature, Unit::Celsius, "Temperature"},
    {"UD01", measurement_id::kDcMppt1Voltage, 10.0, MeasurementType::Voltage, Unit::Volt, "String 1 voltage"},
    {"UD02", measurement_id::kDcMppt2Voltage, 10.0, MeasurementType::Voltage, Unit::Volt, "String 2 voltage"},
    {"UD03", measurement_id::kDcMppt3Voltage, 10.0, MeasurementType::Voltage, Unit::Volt, "String 3 voltage"},
    {"ID01", measurement_id::kDcMppt1Current, 100.0, MeasurementType::Current, Unit::Ampere, "String 1 current"},
    {"ID02", measurement_id::kDcMppt2Current, 100.0, MeasurementType::Current, Unit::Ampere, "String 2 current"},
    {"ID03", measurement_id::kDcMppt3Current, 100.0, MeasurementType::Current, Unit::Ampere, "String 3 current"},
    {"KDY", measurement_id::kEnergyToday, 10.0, MeasurementType::Energy, Unit::KilowattHour, "Energy today"},
    {"KT0", measurement_id::kEnergyTotal, 1.0, MeasurementType::Energy, Unit::KilowattHour, "Energy total"},
    {"KHR", measurement_id::kOperatingHours, 1.0, MeasurementType::Duration, Unit::Hour, "Operating hours"},
};
constexpr size_t kMappingCount = sizeof(kMappings) / sizeof(kMappings[0]);

/// Status and fault words, handled apart from the measurements because they land on DeviceState
/// fields rather than in the measurement set.
constexpr char kCodeStatus[] = "SYS";
constexpr char kCodeAlarm[]  = "SAL";
/// Identification, asked once.
constexpr char kCodeType[]     = "TYP";
constexpr char kCodeFirmware[] = "SWV";

/// Every code a poll asks for, in one frame.
const char* const kPollCodes[] = {
    "PAC", "PDC", "UL1", "UL2", "UL3", "IL1", "IL2", "IL3", "TNF", "TKK",
    "UD01", "UD02", "UD03", "ID01", "ID02", "ID03", "KDY", "KT0", "KHR",
    kCodeStatus, kCodeAlarm,
};
constexpr size_t kPollCodeCount = sizeof(kPollCodes) / sizeof(kPollCodes[0]);

}  // namespace

SolarmaxOptions optionsFrom(const heliograph::DriverOptions& values) {
    SolarmaxOptions out;
    long            addr = 0;
    if (solarmax::descriptor().numericOption(values, "address", addr)) {
        out.address = static_cast<uint8_t>(addr);
    } else {
        log::warn("SOLARMAX address '%s' invalid, using %u",
                  solarmax::descriptor().optionOr(values, "address").c_str(),
                  static_cast<unsigned>(out.address));
    }
    return out;
}

SolarmaxDriver::SolarmaxDriver(Transport& transport, SolarmaxOptions options)
    : transport_(&transport), options_(options) {
    identity_.manufacturer = "SolarMax";
    identity_.protocolName = "MaxTalk RS485";
    identity_.driverId     = solarmax::descriptor().id;
    identity_.instanceKey  = std::to_string(options_.address);

    capabilities_.addRead(InverterCapability::ReadAcPower);
    capabilities_.addRead(InverterCapability::ReadAcVoltage);
    capabilities_.addRead(InverterCapability::ReadAcCurrent);
    capabilities_.addRead(InverterCapability::ReadGridFrequency);
    capabilities_.addRead(InverterCapability::ReadDcPower);
    capabilities_.addRead(InverterCapability::ReadDcVoltage);
    capabilities_.addRead(InverterCapability::ReadDcCurrent);
    capabilities_.addRead(InverterCapability::ReadEnergyToday);
    capabilities_.addRead(InverterCapability::ReadEnergyTotal);
    capabilities_.addRead(InverterCapability::ReadTemperature);
    capabilities_.addRead(InverterCapability::ReadOperatingHours);
    capabilities_.addRead(InverterCapability::ReadStatus);
    capabilities_.addRead(InverterCapability::ReadErrors);
    // Declared optimistically and corrected by what the device actually answers: the family
    // spans single- and three-phase units, and asking a single-phase inverter for UL2 simply
    // gets no UL2 back rather than an error.
    capabilities_.phaseCount = 1;
    capabilities_.mpptCount  = 1;
}

const DriverDescriptor& SolarmaxDriver::descriptor() const { return solarmax::descriptor(); }

bool SolarmaxDriver::begin(Transport& transport) {
    transport_ = &transport;
    // No handshake, no registration, nothing written to the device: the address was set by its
    // operator and the driver simply starts asking. That is why probe() is genuinely read-only
    // for this family, unlike the AA55 drivers beside it.
    return transport_->configure(kLine);
}

maxtalk::ParseResult SolarmaxDriver::readCodes(const char* const* codes, size_t codeCount,
                                               maxtalk::Reading* out, size_t outCapacity,
                                               size_t& outCount) {
    outCount = 0;
    if (transport_ == nullptr) return maxtalk::ParseResult::NotAFrame;

    char         request[maxtalk::kMaxFrame];
    const size_t requestLen =
        maxtalk::buildRequest(options_.address, codes, codeCount, request, sizeof(request));
    if (requestLen == 0) {
        return maxtalk::ParseResult::Malformed;
    }

    TransportLock lock(*transport_, kBusLockTimeoutMs);
    if (!lock.held()) {
        ++busErrors_.timeouts;
        return maxtalk::ParseResult::Incomplete;
    }

    transport_->flushInput();
    if (transport_->write(reinterpret_cast<const uint8_t*>(request), requestLen) != requestLen) {
        ++busErrors_.timeouts;
        return maxtalk::ParseResult::Incomplete;
    }

    // Read until a closing brace, the deadline, or too many empty reads.
    //
    // TWO bounds, not one, and the second is not belt-and-braces. A loop bounded only by the
    // clock spins forever against a transport whose read() returns 0 without consuming time --
    // which is legal (the contract says read returns 0 on timeout, not that it blocks first) and
    // is exactly what the host mock does. That spin holds the BUS LOCK, so it would not merely
    // hang this driver, it would stop every other user of the bus.
    //
    // Found by the tests hanging rather than failing, which is the useful kind of discovery: on
    // real hardware the clock does advance and this would have shipped looking fine.
    constexpr size_t kMaxEmptyReads = 64;
    char             buffer[maxtalk::kMaxFrame];
    size_t           have       = 0;
    size_t           emptyReads = 0;
    const uint64_t   deadline   = transport_->nowMs() + kReplyTimeoutMs;
    size_t           frameLen   = 0;
    while (transport_->nowMs() < deadline && have < sizeof(buffer) &&
           emptyReads < kMaxEmptyReads) {
        const size_t n = transport_->read(reinterpret_cast<uint8_t*>(buffer) + have,
                                          sizeof(buffer) - have, 100);
        if (n == 0) {
            ++emptyReads;
            continue;
        }
        emptyReads = 0;
        have += n;
        frameLen = maxtalk::frameLength(buffer, have);
        if (frameLen != 0) break;
    }

    if (frameLen == 0) {
        ++busErrors_.timeouts;
        return maxtalk::ParseResult::Incomplete;
    }

    const auto result =
        maxtalk::parseReply(buffer, frameLen, options_.address, out, outCapacity, outCount);
    switch (result) {
        case maxtalk::ParseResult::BadChecksum:
            // The one counter that indicts the cabling; kept apart from the rest for that reason.
            ++busErrors_.checksumErrors;
            break;
        case maxtalk::ParseResult::Ok:
        case maxtalk::ParseResult::TooManyReadings:
            break;
        default:
            ++busErrors_.invalidFrames;
            break;
    }
    return result;
}

void SolarmaxDriver::declareChannels(DeviceState& state) const {
    for (size_t i = 0; i < kMappingCount; ++i) {
        state.measurements.declare(kMappings[i].measurementId, kMappings[i].type,
                                   kMappings[i].unit, kMappings[i].displayName);
    }
}

ProbeResult SolarmaxDriver::probe() {
    ProbeResult out;
    const char* codes[] = {kCodeType, kCodeFirmware};

    maxtalk::Reading readings[4];
    size_t           count  = 0;
    const auto       result = readCodes(codes, 2, readings, 4, count);

    if (result == maxtalk::ParseResult::Incomplete) {
        out.evidence.push_back("no reply at address " + std::to_string(options_.address));
        return out;
    }
    if (result == maxtalk::ParseResult::WrongSender) {
        // Traffic on the bus, but from somebody else. Reported as such: on an address sweep this
        // is precisely the "another device answered" diagnosis, not a failed probe.
        out.sawTraffic = true;
        out.evidence.push_back("a well-formed frame arrived from another address");
        return out;
    }
    if (result != maxtalk::ParseResult::Ok) {
        out.sawTraffic = true;
        out.evidence.push_back(std::string("reply rejected: ") + maxtalk::parseResultName(result));
        return out;
    }

    out.responded     = true;
    out.checksumValid = true;
    out.sawTraffic    = true;
    // A brace-delimited frame with a correct sum, from the address we asked, answering the codes
    // we asked. That is a strong fingerprint -- but the protocol carries no vendor marker, so it
    // cannot be a certainty the way a "SunS" identifier is.
    out.confidenceScore       = 70;
    out.detectedManufacturer  = "SolarMax";

    if (const auto* typ = maxtalk::find(readings, count, kCodeType)) {
        out.detectedModel = "type 0x" + std::to_string(typ->value);
        identity_.model   = out.detectedModel;
    }
    if (const auto* swv = maxtalk::find(readings, count, kCodeFirmware)) {
        out.firmwareVersion  = std::to_string(swv->value);
        identity_.firmwareVersion = out.firmwareVersion;
    }
    identified_ = true;
    out.evidence.push_back("MaxTalk frame with a valid checksum from address " +
                           std::to_string(options_.address));
    return out;
}

PollResult SolarmaxDriver::poll(DeviceState& state) {
    maxtalk::Reading readings[kPollCodeCount + 4];
    size_t           count  = 0;
    const auto       result = readCodes(kPollCodes, kPollCodeCount, readings, sizeof(readings) / sizeof(readings[0]), count);

    switch (result) {
        case maxtalk::ParseResult::Ok:
        case maxtalk::ParseResult::TooManyReadings:
            break;
        case maxtalk::ParseResult::Incomplete:
            return PollResult::Timeout;
        case maxtalk::ParseResult::BadChecksum:
            return PollResult::ChecksumError;
        case maxtalk::ParseResult::WrongSender:
            // Not our device. Reported as an invalid frame rather than a checksum error: the wire
            // is fine, the answer simply belongs to someone else.
            return PollResult::InvalidFrame;
        default:
            return PollResult::InvalidFrame;
    }

    // The contract: state is only touched on success, so a partially decoded reply can never
    // surface as data.
    declareChannels(state);

    size_t filled = 0;
    for (size_t i = 0; i < kMappingCount; ++i) {
        const auto* r = maxtalk::find(readings, count, kMappings[i].code);
        if (r == nullptr) {
            // A code this unit does not implement. Left invalid rather than zeroed -- absent and
            // zero are different claims, and a single-phase inverter genuinely has no L2.
            continue;
        }
        state.measurements.set(kMappings[i].measurementId,
                               static_cast<double>(r->value) / kMappings[i].divisor, state.lastPollAttemptMs);
        ++filled;
    }

    if (const auto* sys = maxtalk::find(readings, count, kCodeStatus)) {
        state.statusCode          = static_cast<uint16_t>(sys->value);
        state.statusCodeSupported = true;
        // Neither source enumerates the status table, so the raw value is reported without a
        // meaning attached. Inventing text here would be a guess wearing a label.
        state.statusText = "Status " + std::to_string(sys->value);
    }
    if (const auto* sal = maxtalk::find(readings, count, kCodeAlarm)) {
        state.errorCode          = sal->value;
        state.errorCodeSupported = true;
    }

    // Correct the declared shape from what actually came back, rather than trusting the optimistic
    // defaults set in the constructor.
    uint8_t phases = 1;
    if (maxtalk::find(readings, count, "UL3") != nullptr) {
        phases = 3;
    } else if (maxtalk::find(readings, count, "UL2") != nullptr) {
        phases = 2;
    }
    capabilities_.phaseCount = phases;
    if (phases > 1) capabilities_.addRead(InverterCapability::ReadMultiplePhases);

    uint8_t mppts = 1;
    if (maxtalk::find(readings, count, "UD03") != nullptr) {
        mppts = 3;
    } else if (maxtalk::find(readings, count, "UD02") != nullptr) {
        mppts = 2;
    }
    capabilities_.mpptCount = mppts;
    if (mppts > 1) capabilities_.addRead(InverterCapability::ReadMultipleMppts);

    state.capabilities = capabilities_;
    state.identity     = identity_;

    // A frame that parsed but carried none of the codes we map is not a successful poll: the
    // device answered something, but nothing usable, and reporting Ok would publish an empty set
    // as current data.
    return filled > 0 ? PollResult::Ok : PollResult::InvalidFrame;
}

BusErrorCounts SolarmaxDriver::busErrors() const { return busErrors_; }

DeviceIdentity SolarmaxDriver::identity() const { return identity_; }

InverterCapabilities SolarmaxDriver::capabilities() const { return capabilities_; }

CommandResult SolarmaxDriver::execute(const InverterCommand&) {
    return CommandResult::Unsupported;
}

}  // namespace heliograph::solarmax
