// SPDX-License-Identifier: MIT
// See eversolar_driver.h for provenance.

#include "eversolar_driver.h"
#include "drivers/pmu_pv_capabilities.h"

#include <algorithm>
#include <cstdio>
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
/// Minimum silence between two of our frames. A Zeversolar ignores a request that follows the
/// previous frame too closely: an offline query 12 ms after the RE_REGISTER burst, and an
/// address assignment sent the moment its announcement arrived, both went unanswered, while
/// the same query a second later was answered (hardware, 2026-10-20). The reference sleeps a
/// full second after every request (eversolar.pl send_request, "allow time for inverter to
/// respond"), so no request of its follows another sooner than this. The RE_REGISTER repeats
/// are the one exception: a single broadcast burst, about 12 ms apart, that nobody answers.
constexpr uint32_t kInterFrameGapMs = 1000;

}  // namespace

EversolarOptions optionsFrom(const heliograph::DriverOptions& values) {
    EversolarOptions out;
    const std::string layout = eversolar::descriptor().optionOr(values, "layout");
    if (layout == "single") {
        out.layout = LayoutSelection::ForceSingleString;
    } else if (layout == "dual") {
        out.layout = LayoutSelection::ForceDualString;
    } else if (layout == "three") {
        out.layout = LayoutSelection::ForceThreePhase;
    } else {
        out.layout = LayoutSelection::Auto;
    }
    // An install that predates this option has no "address" key stored, and that case is
    // handled by the TRUE branch, not the false one: numericOption falls back to the option's
    // DECLARED default when the key is absent (optionOr in driver_descriptor.h), and that
    // default is "16" -- kFirstInverterAddress. So a single-inverter bridge upgrading from an
    // older firmware keeps handing out 0x10 exactly as before, and keeps sending the
    // RE_REGISTER that only the holder of that address sends.
    //
    // The else below is therefore reached only when the descriptor and this code disagree --
    // no such option, not numeric, or a stored value outside the declared range. Restating the
    // same default there is deliberate: this driver must never end up with address 0.
    long addr = 0;
    if (eversolar::descriptor().numericOption(values, "address", addr)) {
        out.assignedAddress = static_cast<uint8_t>(addr);
    } else {
        out.assignedAddress = kFirstInverterAddress;
    }
    return out;
}

EversolarDriver::EversolarDriver(Transport& transport, EversolarOptions options)
    : transport_(&transport), options_(options), address_(options.assignedAddress) {
    // A healthy reply with an echo of our request is ~66 bytes; this holds noise on top of
    // that without growing, so the steady-state poll never touches the heap.
    pollRaw_.reserve(128);
}

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

    // Shared with the other PMU-family driver; see pmu_pv_capabilities.h. mpptCount starts at 1
    // and is revised upward if a dual-string payload arrives.
    //
    // Deliberately NOT added here: ReadErrors. This variant of the protocol carries no error
    // code field, so declaring it would advertise a channel that can never produce a value.
    // Battery and multi-phase are absent for the same reason, and write stays empty.
    capabilities_ = pmuPvCapabilities();

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
                                                          const Address* altSource,
                                                          std::vector<uint8_t>* rawTx,
                                                          std::vector<uint8_t>* rawRx) {
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

    waitForInterFrameGap();
    pmu::Exchange exchange{request, requestLen, command, expectedSource, altSource};
    if (rawTx != nullptr) {
        rawTx->assign(request, request + requestLen);
    }
    exchange.rawReply = rawRx;
    const auto result = pmu::transact(*transport_, exchange, payloadOut, payloadCapacity,
                                      payloadLen, tally_, "RS485");
    lastFrameMs_ = transport_->nowMs();
    sentFrame_   = true;
    return result;
}

void EversolarDriver::waitForInterFrameGap() {
    if (!sentFrame_) {
        return;
    }
    const uint64_t due = lastFrameMs_ + kInterFrameGapMs;
    // Waiting by reading: the transport has no sleep, and a read with a timeout is one. Stray
    // bytes are dropped here, as flushInput() would drop them before the next request anyway.
    uint8_t discard[32];
    for (uint64_t now = transport_->nowMs(); now < due;) {
        const size_t   got   = transport_->read(discard, sizeof(discard),
                                                static_cast<uint32_t>(due - now));
        const uint64_t later = transport_->nowMs();
        // A read that drained buffered bytes returns within the same millisecond, so a
        // still clock alone does not mean the clock cannot move: only a still clock on an
        // empty read does (simulated transport). Breaking on the first would send early on
        // any bus with stray traffic.
        if (later == now && got == 0) {
            break;
        }
        now = later;
    }
}

void EversolarDriver::reRegisterAll() {
    if (transport_ == nullptr) {
        return;
    }
    waitForInterFrameGap();
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
    lastFrameMs_ = transport_->nowMs();
    sentFrame_   = true;
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

    // 2. Hand it an address. The announcement echoed back VERBATIM plus the address we allocate.
    // Not the decoded serial: a Zeversolar answers with 18 bytes, "SX00086011740017" + 00 02,
    // and the trimmed string drops the tail, so the inverter never recognises its own serial and
    // never ACKs (hardware, 2026-10-20). The reference does the same: unpack("C*") of the raw
    // response, not the cleaned string (eversolar.pl:609-610).
    uint8_t assign[kMaxDataLength];
    const size_t serialLen = payloadLen < sizeof(assign) - 1 ? payloadLen : sizeof(assign) - 1;
    std::memcpy(assign, payload, serialLen);
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
        // The anchor is the brand as the id string spells it. A TL3000-20 says "Ever-Solar";
        // a Zeversolar TLC8000 says "ZEVERSolar" (captured 2026-10-22, see
        // test_a_zeversolar_id_string_yields_the_bare_model for the whole string), and without its
        // own anchor the whole composite became the model, and every HA entity id with it.
        struct Brand {
            const char* anchor;
            const char* manufacturer;
        };
        static constexpr Brand kBrands[] = {{"Ever-Solar", "Ever-Solar"},
                                            {"ZEVERSolar", "Zeversolar"}};
        std::string parsedModel;
        for (const Brand& brand : kBrands) {
            parsedModel = modelFromIdString(idString, brand.anchor);
            if (!parsedModel.empty()) {
                identity_.manufacturer = brand.manufacturer;
                break;
            }
        }
        const std::string& model = parsedModel.empty() ? idString : parsedModel;
        if (probeOut != nullptr) {
            probeOut->confidenceScore += 10;
            probeOut->evidence.push_back("inverter id string: " + idString);
            probeOut->detectedManufacturer = identity_.manufacturer;
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
                std::to_string(payloadLen) + " bytes)");
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
                            inverterAddress(address_), payload, sizeof(payload), payloadLen,
                            nullptr, nullptr, &pollRaw_);
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
    // Recomputed every poll like mpptCount, so a layout change cannot leave it stuck at 3.
    capabilities_.phaseCount = info.hasThreePhases ? 3 : 1;
    if (info.hasThreePhases) {
        capabilities_.addRead(InverterCapability::ReadMultiplePhases);
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
    if (info.hasThreePhases) {
        m.declare(measurement_id::kAcL2Voltage, MeasurementType::Voltage, Unit::Volt, "AC Voltage L2");
        m.declare(measurement_id::kAcL2Current, MeasurementType::Current, Unit::Ampere, "AC Current L2");
        m.declare(measurement_id::kAcL3Voltage, MeasurementType::Voltage, Unit::Volt, "AC Voltage L3");
        m.declare(measurement_id::kAcL3Current, MeasurementType::Current, Unit::Ampere, "AC Current L3");
    }
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
    if (info.hasThreePhases) {
        m.set(measurement_id::kAcL2Voltage, info.acVoltage2, ts);
        m.set(measurement_id::kAcL2Current, info.acCurrent2, ts);
        m.set(measurement_id::kAcL3Voltage, info.acVoltage3, ts);
        m.set(measurement_id::kAcL3Current, info.acCurrent3, ts);
    }
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

    // Fault status. Derived from the mode word alone -- no error bit is decoded anywhere. 2 and
    // 3 are vendor-documented and not yet observed; 0 and 1 are observed. Any other mode is
    // unknown, and an unknown mode is not "no fault": the flag is invalidated, not set false,
    // so it publishes null and Home Assistant shows "unknown".
    m.declare(measurement_id::kInverterFault, MeasurementType::Flag, Unit::None, "Fault");
    if (info.statusCode == 2 || info.statusCode == 3) {
        m.set(measurement_id::kInverterFault, 1.0, ts);
    } else if (info.statusCode == 0 || info.statusCode == 1) {
        m.set(measurement_id::kInverterFault, 0.0, ts);
    } else {
        m.invalidate(measurement_id::kInverterFault);
    }

    trackFaultState(info);
    if (info.tailValid) {
        // The raw words as hex, in the order the unit sends them. Not errorCode: see
        // DeviceState::errorRaw.
        char words[10];
        std::snprintf(words, sizeof(words), "%04X %04X", info.tail.errorWordH,
                      info.tail.errorWordL);
        state.errorRaw          = words;
        state.errorRawSupported = true;
        m.declare(measurement_id::kInverterErrorWordsChanged, MeasurementType::Flag, Unit::None,
                  "Error words changed");
        if (baselineKnown_) {
            m.set(measurement_id::kInverterErrorWordsChanged, wordsChanged_ ? 1.0 : 0.0, ts);
        } else {
            m.invalidate(measurement_id::kInverterErrorWordsChanged);
        }
    } else {
        // A layout without a readable tail: nothing to report, and nothing left over from an
        // earlier reply that had one.
        state.errorRaw.clear();
        state.errorRawSupported = false;
        m.invalidate(measurement_id::kInverterErrorWordsChanged);
    }

    state.identity     = identity_;
    state.capabilities = capabilities_;
    return PollResult::Ok;
}

void EversolarDriver::trackFaultState(const NormalInfo& info) {
    const uint16_t mode = info.statusCode;
    const bool     tail = info.tailValid;
    const uint32_t signature =
        tail ? (static_cast<uint32_t>(info.tail.errorWordH) << 16) | info.tail.errorWordL : 0;

    auto keep = [&](FaultRecordReason reason) {
        // Drained after every poll, so this never fills in the firmware. The bound is for a
        // caller that polls without draining: a flapping unit must not grow the heap.
        if (faultRecords_.size() >= kMaxPendingFaultRecords) {
            return;
        }
        FaultRecord rec;
        rec.reason     = reason;
        rec.statusCode = mode;
        rec.signature  = signature;
        rec.frame.assign(pollRaw_.begin(), pollRaw_.end());
        faultRecords_.push_back(std::move(rec));
    };

    // The mode moved (or this is the first poll we ever saw) into something other than a normal
    // state. 2 and 3 are the documented faults; an unknown mode is recorded too, because an
    // unknown frame is exactly the one worth having later.
    // The way back out is a status event as well. The log keeps a change of the error words only
    // once per state, and the words a unit returns to are usually ones it already holds, so as a
    // change record the recovery would be dropped and the fault would have no end.
    // When the mode and the words move in the same reply, the status record already holds that
    // frame, and the log has only a few slots: one record, not two. The baseline below is the
    // exception, and a rare one -- at most once per boot, and the log keeps only one copy of it.
    const auto normal    = [](uint16_t m) { return m == 0 || m == 1; };
    const bool modeMoved = !havePrevious_ || previousMode_ != mode;
    if (modeMoved && (!normal(mode) || (havePrevious_ && !normal(previousMode_)))) {
        keep(FaultRecordReason::StatusChanged);
    } else if (tail && havePrevious_ && previousTail_ &&
               (previousWordH_ != info.tail.errorWordH ||
                previousWordL_ != info.tail.errorWordL)) {
        keep(FaultRecordReason::ErrorWordsChanged);
    }
    if (!baselineKnown_ && tail && mode == 1) {
        baselineKnown_ = true;
        baselineWordH_ = info.tail.errorWordH;
        baselineWordL_ = info.tail.errorWordL;
        keep(FaultRecordReason::Baseline);
    }

    wordsChanged_ = tail && baselineKnown_ &&
                    (info.tail.errorWordH != baselineWordH_ ||
                     info.tail.errorWordL != baselineWordL_);
    havePrevious_  = true;
    previousMode_  = mode;
    previousTail_  = tail;
    previousWordH_ = tail ? info.tail.errorWordH : 0;
    previousWordL_ = tail ? info.tail.errorWordL : 0;
}

std::vector<FaultRecord> EversolarDriver::takeFaultRecords() {
    std::vector<FaultRecord> out;
    out.swap(faultRecords_);
    return out;
}

std::vector<FaultFrameField> EversolarDriver::decodeFaultFrame(const uint8_t* frame,
                                                               size_t         len) const {
    std::vector<FaultFrameField> out;
    if (frame == nullptr) {
        return out;
    }
    // The stored bytes are a raw record and may begin with an echo of our own request, so look
    // for the reply rather than assume where it starts. parseFrame checks the checksum.
    for (size_t i = 0; i + kFrameOverhead <= len; ++i) {
        Frame f;
        if (frame[i] != kHeader0 || parseFrame(frame + i, len - i, f) != ParseResult::Ok ||
            f.control != cmd::kQueryNormalInfo.control ||
            f.function != (cmd::kQueryNormalInfo.function | 0x80)) {
            continue;
        }
        NormalInfo info;
        if (decodeNormalInfo(f.data, f.dataLength, info, LayoutSelection::Auto) !=
                DecodeResult::Ok ||
            !info.tailValid) {
            return out;
        }
        // Scaling from the vendor protocol document (V1.1), UNVERIFIED on a faulted unit. A raw
        // 0xFFFF is left out: a healthy unit sends it for the impedance (observed), and 65.535
        // would be a number made up from a filler, while 0 would claim a measurement. Unknown
        // is never zero.
        struct Spec {
            const char* name;
            double      scale;
            const char* unit;
            bool        isSigned;
        };
        static constexpr Spec kSpecs[6] = {
            {"grid_voltage_at_fault", 10.0, "V", false},
            {"grid_frequency_at_fault", 100.0, "Hz", false},
            {"grid_impedance_at_fault", 1000.0, "ohm", false},
            {"temperature_at_fault", 10.0, "C", true},
            {"pv1_voltage_at_fault", 10.0, "V", false},
            {"residual_current_at_fault", 1000.0, "A", false},
        };
        for (size_t k = 0; k < 6; ++k) {
            const uint16_t raw = info.tail.faultValue[k];
            if (raw == 0xFFFF) {
                continue;
            }
            const double v = kSpecs[k].isSigned ? static_cast<int16_t>(raw) : raw;
            out.push_back({kSpecs[k].name, v / kSpecs[k].scale, kSpecs[k].unit});
        }
        return out;
    }
    return out;
}

DeviceIdentity EversolarDriver::identity() const { return identity_; }

InverterCapabilities EversolarDriver::capabilities() const { return capabilities_; }

CommandResult EversolarDriver::execute(const InverterCommand& command) {
    (void)command;
    return CommandResult::Unsupported;
}

namespace {
struct QueryDef {
    const char* name;
    const char* description;
    CommandCode command;
};
// All READ (control 0x11). Reply payloads are not decoded: see the header.
constexpr QueryDef kQueries[] = {
    {"read_description", "read-only data codes the inverter lists", cmd::kQueryDescription},
    {"rw_description", "writable data numbers the inverter lists", cmd::kQueryRwDescription},
    {"set_info", "current setting values", cmd::kQuerySetInfo},
    {"versions", "master, slave and logger versions", cmd::kQueryVersions},
};
}  // namespace

std::vector<DiagnosticQuery> EversolarDriver::diagnosticQueries() const {
    std::vector<DiagnosticQuery> out;
    for (const QueryDef& q : kQueries) {
        out.push_back({q.name, q.description});
    }
    return out;
}

DiagnosticQueryResult EversolarDriver::runDiagnosticQuery(const std::string& name) {
    DiagnosticQueryResult result;
    const QueryDef*       def = nullptr;
    for (const QueryDef& q : kQueries) {
        if (name == q.name) {
            def = &q;
        }
    }
    if (def == nullptr) {
        return result;  // UnknownQuery, nothing sent
    }
    if (!registered_) {
        result.status = DiagnosticStatus::NotRegistered;
        return result;
    }
    uint8_t payload[kMaxDataLength];
    size_t  payloadLen = 0;
    const Address inverter = inverterAddress(address_);
    // One attempt, no retry: this reports what the wire did.
    //
    // Not counted in the bus-error tallies. Those indict the cabling, and a query the inverter
    // does not know times out by design; left in, every diagnostic session would show up as a
    // degrading bus on the health page and in Prometheus.
    const pmu::Tally tallyBefore = tally_;
    const auto r = transact(def->command, inverter, nullptr, 0, inverter, payload,
                            sizeof(payload), payloadLen, nullptr, &result.tx, &result.rx);
    tally_ = tallyBefore;
    switch (r) {
        case TransactResult::Ok:
            result.status = DiagnosticStatus::Ok;
            result.payload.assign(payload, payload + payloadLen);
            break;
        case TransactResult::Timeout: {
            // rx is every byte the line delivered, so on a bus that echoes it holds our own
            // request even when the inverter stayed silent. Only bytes beyond that echo mean
            // something answered.
            const bool onlyEcho =
                result.rx.size() <= result.tx.size() &&
                std::equal(result.rx.begin(), result.rx.end(), result.tx.begin());
            result.status = onlyEcho ? DiagnosticStatus::Timeout : DiagnosticStatus::UnexpectedFrame;
            break;
        }
        case TransactResult::ChecksumError:  result.status = DiagnosticStatus::ChecksumError; break;
        case TransactResult::InvalidFrame:   result.status = DiagnosticStatus::InvalidFrame; break;
        case TransactResult::TransportError:
            // The lock or the write failed, so the frame may never have reached the line.
            result.status = DiagnosticStatus::TransportError;
            result.tx.clear();
            break;
    }
    return result;
}

}  // namespace heliograph::eversolar
