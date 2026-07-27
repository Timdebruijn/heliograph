// SPDX-License-Identifier: MIT

#include "mock_driver.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

#include "device/device_state.h"  // kMaxDevices, the bound on the instance option
#include "diagnostics/logger.h"

namespace heliograph::mock {
namespace {

/// A clock for drivers built by the factory.
///
/// The factories used to pass nullptr, which made MockDriver fall back to now = 0 and freeze
/// the simulated day at midnight forever: the firmware reported 0 W and nothing else, on
/// hardware, permanently. Every test injects its own clock, so the path the firmware actually
/// uses was the one path never exercised. steady_clock works on the host and on ESP32 alike.
uint64_t defaultClock() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

constexpr double kPi          = 3.14159265358979323846;
constexpr double kPeakDcWatts = 6000.0;
/// One simulated day's yield, and where the lifetime counter starts. Named rather than written
/// out at the point of use because three channels have to agree on them: today integrates to
/// exactly kDailyYieldKwh by sunset, and the lifetime total is that yield times the days behind
/// us plus whatever today has produced so far.
constexpr double kDailyYieldKwh   = 12.5;
constexpr double kInitialTotalKwh = 24680.0;

/// Where an instance sits in its simulated day.
struct SimDay {
    /// Whole simulated days behind us. Only the lifetime counter needs this, and it needs it to
    /// keep climbing across a midnight instead of restarting.
    uint64_t completed = 0;
    /// 0.0 = midnight, 0.25 = sunrise, 0.5 = midday, 0.75 = sunset.
    double phase = 0.0;
};

/// The one place the simulated clock is turned into a position in the day. The curve and both
/// energy counters read it, so they cannot drift apart -- the counters have to roll over at
/// exactly the moment the curve calls it midnight, or the lifetime total jumps at the seam.
///
/// The cycle starts at midday rather than midnight. The phase origin is arbitrary in a
/// simulation, and starting at midnight means a freshly booted bridge reports 0 W until a
/// quarter of the day has passed -- which looks broken and is the opposite of what a
/// demonstration driver is for. Booting into peak output shows the stack working immediately,
/// and the night still arrives a few minutes later.
///
/// `instance` staggers the curve so that several simulated inverters do not move in lockstep.
/// What needs testing is several devices reporting DIFFERENT non-zero values at once: a fleet
/// reporting one identical number makes a shared store, a mislabelled Prometheus series or a
/// Modbus unit off by one look exactly like correct output.
///
/// A THIRTY-SECOND of a day apart, not a sixteenth. The cycle starts at midday and daylight
/// runs a quarter-day either side, so at a sixteenth the eighth instance would already be dark
/// the moment the bridge boots -- and a fleet whose last members report nothing from the outset
/// is not the demonstration this is for. At 1/32 the whole run of kMaxDevices spans 7/32 of a
/// day, which puts every one of them in daylight AT BOOT (1.0 down to 0.195 of peak).
///
/// They do NOT all stay there, and that is the second thing this buys. As the day advances they
/// set one by one, last instance first, so a running fleet is a mix of producing and dark
/// devices -- which is what exercises the rule that a total is null when no device reported the
/// channel rather than 0, and the per-device online/stale handling underneath it. A fleet that
/// rose and set in unison would never reach that state at all.
///
/// Instance 1 gets no offset, so the single-mock case is bit-for-bit what it always was.
SimDay simulatedDay(uint64_t nowMs, uint64_t dayLengthMs, uint8_t instance) {
    if (dayLengthMs == 0) {
        return {};  // midnight forever: no curve, no yield, lifetime total left at its start
    }
    const uint64_t offset = (instance > 0 ? instance - 1u : 0u) * (dayLengthMs / 32ULL);
    // Half a cycle forward, rather than folding the phase afterwards. Both express the same
    // midday origin, but shifting the input is what lets the day count and the phase fall out
    // of one division and therefore agree at the rollover.
    const uint64_t t = nowMs + offset + dayLengthMs / 2;
    return {t / dayLengthMs, static_cast<double>(t % dayLengthMs) / static_cast<double>(dayLengthMs)};
}

/// Bell-shaped output over the middle half of the day, zero at night. Zero here is a real
/// measurement, exactly as it is for a real inverter after sunset.
double solarFraction(double phase) {
    if (phase < 0.25 || phase > 0.75) {
        return 0.0;  // night
    }
    return std::sin((phase - 0.25) * 2.0 * kPi);
}

/// Energy generated since the simulated midnight -- the INTEGRAL of the curve above, not a
/// scaled copy of it.
///
/// This used to be `12.5 * fraction`, which meant "today" climbed until midday and then fell
/// back to zero by sunset. A daily counter that decreases is not merely a wrong number, it is a
/// different kind of number: every consumer downstream treats a drop as a meter reset, Home
/// Assistant's `total_increasing` classes included. Because it is the integral, its slope is
/// the instantaneous power -- so the two channels now describe the same inverter.
double energyToday(double phase) {
    if (phase <= 0.25) {
        return 0.0;  // between midnight and sunrise the new day has produced nothing yet
    }
    if (phase >= 0.75) {
        return kDailyYieldKwh;  // after sunset: the day's total, held until midnight
    }
    const double daylight = (phase - 0.25) / 0.5;  // 0 at sunrise, 1 at sunset
    return kDailyYieldKwh * (1.0 - std::cos(daylight * kPi)) / 2.0;
}

/// Lifetime yield: a fixed starting point plus every day behind us plus today's progress.
///
/// Continuous across midnight by construction -- `completed` gains the day at the same instant
/// `energyToday` drops it -- which is the property that makes it safe to hand to a counter that
/// may never go backwards. It was a hardcoded 24680 that never moved at all, so nothing
/// downstream that accumulates or rate-limits on this channel was ever exercised.
double energyTotal(const SimDay& day) {
    return kInitialTotalKwh + static_cast<double>(day.completed) * kDailyYieldKwh
           + energyToday(day.phase);
}

DriverDescriptor makeDescriptor(bool writable) {
    DriverDescriptor x;
    x.id           = writable ? "mock_inverter_writable" : "mock_inverter";
    x.displayName  = writable ? "Mock Inverter (writable)" : "Mock Inverter";
    x.manufacturer = "Heliograph open-source project";
    x.protocol     = "none (simulated)";
    x.description =
        "Simulated three-phase hybrid inverter with two MPPTs and a battery. Exists to run "
        "the full stack without hardware and to prove that output adapters make no "
        "assumptions about a specific device.";
    x.supportedTransports = {TransportType::Mock, TransportType::Rs485, TransportType::Tcp};
    x.recommendedSerialProfiles = {};
    x.supportLevel            = DriverSupportLevel::Stable;  // it is exactly what it claims
    // Below every real driver: a simulation must never win an auto-detection race.
    x.probePriority           = -100;
    x.supportsAutoDetection   = false;
    x.supportsMultipleDevices = true;
    x.supportsRead            = true;
    x.supportsWrite           = writable;
    x.options = {DriverOption{"day_length_minutes", "Simulated day length",
                              "How long one simulated sunrise-to-sunset cycle takes. Short by "
                              "default so a full curve is visible within minutes of booting.",
                              "10", {},
                              // Bounded like every other numeric option. Its parser has the
                              // same silent fallback the others had -- a non-positive or
                              // garbage value keeps the default with no log line at all -- and
                              // leaving the one driver everybody has compiled in unbounded
                              // would make "the numeric driver options" a claim about most of
                              // them (review, 2026-07-25).
                              1, 1440},
                 DriverOption{"unit_id", "Simulated unit",
                              "Which simulated inverter this is. Give each mock under Extra "
                              "devices its own number: it is what keeps them apart, and it "
                              "staggers their solar curves so they do not all report the same "
                              "value at the same moment.",
                              "1", {}, 1, static_cast<long>(kMaxDevices)}};
    // Named like the real drivers' address option, which buys two things: the settings page
    // already warns when two devices share a `unit_id`, and DiscoveryCandidate::address() has
    // something to report.
    //
    // Safe despite the note in discovery_engine.h about a sweep turning one address assignment
    // into nine: the engine skips every driver with supportsAutoDetection false, and this is
    // the driver that comment names. The sweep can never reach it.
    x.addressOptionKey = "unit_id";
    return x;
}

}  // namespace

std::string mockSerialNumber(uint8_t instance) {
    // Ten digits, zero-padded: instance 1 comes out as "MOCK-0000000001", exactly the constant
    // every mock reported before it could be told apart. An existing single-mock install
    // therefore keeps its device id -- and with it its REST path, its MQTT topic subtree and
    // its Home Assistant entities -- across this change, with no retained topics orphaned.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "MOCK-%010u", static_cast<unsigned>(instance));
    return buf;
}

const DriverDescriptor& readOnlyDescriptor() {
    static const DriverDescriptor d = makeDescriptor(false);
    return d;
}

const DriverDescriptor& writableDescriptor() {
    static const DriverDescriptor d = makeDescriptor(true);
    return d;
}

MockOptions optionsFrom(const heliograph::DriverOptions& values, bool writable) {
    MockOptions out;
    out.writable = writable;
    // The 1-1440 range comes from the descriptor row, which is where it was declared and never
    // enforced: this parser only checked `> 0`, so the one driver everybody has compiled in was
    // the one whose declared bound meant nothing.
    const DriverDescriptor& d = writable ? writableDescriptor() : readOnlyDescriptor();
    long minutes = 0;
    if (d.numericOption(values, "day_length_minutes", minutes)) {
        out.dayLengthMs = static_cast<uint64_t>(minutes) * 60ULL * 1000ULL;
    } else {
        log::warn("MOCK day_length_minutes '%s' invalid, using %llu minutes",
                  d.optionOr(values, "day_length_minutes").c_str(),
                  static_cast<unsigned long long>(out.dayLengthMs / 60000ULL));
    }
    long unit = 0;
    if (d.numericOption(values, "unit_id", unit)) {
        out.instance = static_cast<uint8_t>(unit);
    } else {
        // Warned about rather than silently defaulted: this option decides the device's
        // IDENTITY, so falling back to 1 on a typo hands the second mock the first one's id
        // and it disappears at boot as a duplicate -- which is the exact failure this option
        // was added to remove.
        log::warn("MOCK unit_id '%s' invalid, using %u",
                  d.optionOr(values, "unit_id").c_str(), static_cast<unsigned>(out.instance));
    }
    return out;
}

MockDriver::MockDriver(ClockFn clock, MockOptions options)
    : clock_(std::move(clock)), options_(options) {
    identity_.manufacturer    = "Heliograph open-source project";
    identity_.model           = "Mock Hybrid 6000";
    identity_.serialNumber    = mockSerialNumber(options.instance);
    identity_.firmwareVersion = "1.0.0";
    identity_.protocolName    = "none (simulated)";
    identity_.driverId        = options.writable ? "mock_inverter_writable" : "mock_inverter";
    // Set as well as the serial, not instead of it. deviceId() prefers the serial, so this
    // changes nothing today -- but instanceKey is what identifies a device before its first
    // poll, and leaving it empty here would make the mock the one driver that behaves
    // differently at exactly the moment the store keys are minted.
    identity_.instanceKey     = std::to_string(static_cast<unsigned>(options.instance));

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
    capabilities_.addRead(InverterCapability::ReadStatus);
    capabilities_.addRead(InverterCapability::ReadErrors);
    capabilities_.addRead(InverterCapability::ReadMultiplePhases);
    capabilities_.addRead(InverterCapability::ReadMultipleMppts);
    capabilities_.addRead(InverterCapability::ReadBatteryState);
    capabilities_.phaseCount = 3;
    capabilities_.mpptCount  = 2;
    capabilities_.hasBattery = true;

    if (options.writable) {
        capabilities_.addWrite(InverterCapability::SetActivePowerLimit);
        capabilities_.addWrite(InverterCapability::StartStop);
        capabilities_.numeric[static_cast<size_t>(InverterCommandType::SetActivePowerLimitPercent)] =
            NumericCapability{true, true, 0.0, 100.0, 1.0, Unit::Percent};
        capabilities_.numeric[static_cast<size_t>(InverterCommandType::SetActivePowerLimitWatts)] =
            NumericCapability{true, true, 0.0, 6000.0, 10.0, Unit::Watt};
    }
}

const DriverDescriptor& MockDriver::descriptor() const {
    return options_.writable ? writableDescriptor() : readOnlyDescriptor();
}

bool MockDriver::begin(Transport& transport) {
    (void)transport;
    return true;
}

ProbeResult MockDriver::probe() {
    ProbeResult r;
    if (options_.offline || options_.timeout) {
        return r;
    }
    r.responded            = true;
    r.checksumValid        = true;
    // Never high enough to be auto-selected: a simulation must not win over a real device.
    r.confidenceScore      = 30;
    r.detectedManufacturer = identity_.manufacturer;
    r.detectedModel        = identity_.model;
    r.serialNumber         = identity_.serialNumber;
    r.firmwareVersion      = identity_.firmwareVersion;
    r.evidence.push_back("simulated device; never auto-selected");
    return r;
}

PollResult MockDriver::poll(DeviceState& state) {
    if (options_.timeout || options_.offline) {
        ++busErrors_.timeouts;
        return PollResult::Timeout;
    }
    if (options_.failChecksum) {
        ++busErrors_.checksumErrors;
        return PollResult::ChecksumError;
    }

    const uint64_t now      = clock_ ? clock_() : 0;
    const SimDay   day      = simulatedDay(now, options_.dayLengthMs, options_.instance);
    const double   fraction = solarFraction(day.phase);
    const double   dcPower  = kPeakDcWatts * fraction;
    const double   acPower  = dcPower * 0.97;

    auto& m = state.measurements;
    m.declare(measurement_id::kAcPowerTotal, MeasurementType::Power, Unit::Watt, "AC Power");
    m.declare(measurement_id::kAcFrequency, MeasurementType::Frequency, Unit::Hertz, "Grid Frequency");
    m.declare(measurement_id::kDcPowerTotal, MeasurementType::Power, Unit::Watt, "DC Power");
    m.declare(measurement_id::kEnergyToday, MeasurementType::Energy, Unit::KilowattHour, "Energy Today");
    m.declare(measurement_id::kEnergyTotal, MeasurementType::Energy, Unit::KilowattHour, "Total Energy");
    m.declare(measurement_id::kTemperature, MeasurementType::Temperature, Unit::Celsius, "Temperature");

    // Static tables, not per-poll `"..." + std::to_string(i)`: the ids and names are fixed, so
    // building them fresh every poll only churned the heap (review, 2026-07-21). The phase
    // number stays in the display name -- Home Assistant builds the entity id from it, and
    // three entities called "Phase Voltage" would collide into _2/_3.
    static const char* kPhaseVoltage[] = {measurement_id::kAcL1Voltage,
                                          measurement_id::kAcL2Voltage,
                                          measurement_id::kAcL3Voltage};
    static const char* kPhaseCurrent[] = {measurement_id::kAcL1Current,
                                          measurement_id::kAcL2Current,
                                          measurement_id::kAcL3Current};
    static const char* kPhasePower[]   = {measurement_id::kAcL1Power, measurement_id::kAcL2Power,
                                          measurement_id::kAcL3Power};
    static const char* kPhaseVName[]   = {"Phase L1 Voltage", "Phase L2 Voltage",
                                          "Phase L3 Voltage"};
    static const char* kPhaseCName[]   = {"Phase L1 Current", "Phase L2 Current",
                                          "Phase L3 Current"};
    static const char* kPhasePName[]   = {"Phase L1 Power", "Phase L2 Power", "Phase L3 Power"};

    const uint64_t ts = state.lastPollAttemptMs != 0 ? state.lastPollAttemptMs : now;

    for (int i = 0; i < 3; ++i) {
        m.declare(kPhaseVoltage[i], MeasurementType::Voltage, Unit::Volt, kPhaseVName[i]);
        m.declare(kPhaseCurrent[i], MeasurementType::Current, Unit::Ampere, kPhaseCName[i]);
        m.declare(kPhasePower[i], MeasurementType::Power, Unit::Watt, kPhasePName[i]);
        const double phasePower = acPower / 3.0;
        const double voltage    = 230.0 + i;
        m.set(kPhaseVoltage[i], voltage, ts);
        m.set(kPhaseCurrent[i], phasePower / voltage, ts);
        m.set(kPhasePower[i], phasePower, ts);
    }

    static const char* kMpptV[]     = {measurement_id::kDcMppt1Voltage,
                                       measurement_id::kDcMppt2Voltage};
    static const char* kMpptC[]     = {measurement_id::kDcMppt1Current,
                                       measurement_id::kDcMppt2Current};
    static const char* kMpptP[]     = {measurement_id::kDcMppt1Power, measurement_id::kDcMppt2Power};
    static const char* kMpptVName[] = {"PV 1 Voltage", "PV 2 Voltage"};
    static const char* kMpptCName[] = {"PV 1 Current", "PV 2 Current"};
    static const char* kMpptPName[] = {"PV 1 Power", "PV 2 Power"};

    for (int i = 0; i < 2; ++i) {
        m.declare(kMpptV[i], MeasurementType::Voltage, Unit::Volt, kMpptVName[i]);
        m.declare(kMpptC[i], MeasurementType::Current, Unit::Ampere, kMpptCName[i]);
        m.declare(kMpptP[i], MeasurementType::Power, Unit::Watt, kMpptPName[i]);
        const double voltage = fraction > 0.0 ? 320.0 + i * 15.0 : 0.0;
        const double power   = dcPower / 2.0;
        m.set(kMpptV[i], voltage, ts);
        m.set(kMpptC[i], voltage > 0.0 ? power / voltage : 0.0, ts);
        m.set(kMpptP[i], power, ts);
    }

    // Constants, not string literals -- same channels as before. An id written out by hand is
    // invisible to anything that enumerates the vocabulary, which is how these four ended up
    // announceable but not clearable (review, 2026-07-26).
    m.declare(measurement_id::kBatterySoc, MeasurementType::Ratio, Unit::Percent, "Battery SoC");
    m.declare(measurement_id::kBatteryVoltage, MeasurementType::Voltage, Unit::Volt,
              "Battery Voltage");
    m.declare(measurement_id::kBatteryChargePower, MeasurementType::Power, Unit::Watt,
              "Battery Charge Power");
    m.declare(measurement_id::kBatteryDischargePower, MeasurementType::Power, Unit::Watt,
              "Battery Discharge Power");
    // The combined channel as well as the two rails. measurement.h asks a driver that reads
    // them separately to publish this one too, and the only driver that reads them separately
    // was the one ignoring that -- so the sign convention itself went untested, and everything
    // keyed on battery.power (the dashboard's charge/discharge column, the Home Assistant
    // entity, the MQTT topic) had nothing to show on the one device anybody can run without
    // hardware. Modbus TCP is the exception: its map carries the two rails at 304/306 and has
    // no register for the combined value, so that output is unchanged by this.
    m.declare(measurement_id::kBatteryPower, MeasurementType::Power, Unit::Watt, "Battery Power");
    const double soc = 40.0 + 40.0 * fraction;
    // `<= 0.0` rather than `== 0.0` only to avoid resting on an exact float compare; it does
    // NOT change which samples count as night, since both forms are true at exactly zero.
    // Sunrise, where the sine is a genuine zero in daylight, therefore still reads as night for
    // the one sample that lands on it -- harmless in a simulation, and cheaper to say than to
    // carry a phase check down here for.
    const double chargeW    = fraction > 0.3 ? 1000.0 : 0.0;
    const double dischargeW = fraction <= 0.0 ? 400.0 : 0.0;
    m.set(measurement_id::kBatterySoc, soc, ts);
    m.set(measurement_id::kBatteryVoltage, 48.0 + soc * 0.05, ts);
    m.set(measurement_id::kBatteryChargePower, chargeW, ts);
    m.set(measurement_id::kBatteryDischargePower, dischargeW, ts);
    m.set(measurement_id::kBatteryPower, chargeW - dischargeW, ts);  // + charging, - discharging

    m.set(measurement_id::kAcPowerTotal, acPower, ts);
    m.set(measurement_id::kAcFrequency, 50.0, ts);
    m.set(measurement_id::kDcPowerTotal, dcPower, ts);
    m.set(measurement_id::kEnergyToday, energyToday(day.phase), ts);
    m.set(measurement_id::kEnergyTotal, energyTotal(day), ts);
    m.set(measurement_id::kTemperature, 25.0 + 20.0 * fraction, ts);

    state.statusCode          = fraction > 0.0 ? 1 : 0;
    state.statusCodeSupported = true;
    state.statusText         = fraction > 0.0 ? "Normal" : "Standby";
    state.errorCode          = 0;
    state.errorCodeSupported = true;  // unlike EverSolar, this device really does report it
    state.identity           = identity_;
    state.capabilities       = capabilities_;
    return PollResult::Ok;
}

DeviceIdentity MockDriver::identity() const { return identity_; }

InverterCapabilities MockDriver::capabilities() const { return capabilities_; }

CommandResult MockDriver::execute(const InverterCommand& command) {
    if (!options_.writable) {
        return CommandResult::Unsupported;
    }
    if (!capabilities_.canWrite(requiredCapability(command.type))) {
        return CommandResult::Unsupported;
    }
    if (command.numericValue.has_value()) {
        lastAcceptedValue_ = *command.numericValue;
    }
    ++acceptedCommands_;
    return CommandResult::Ok;
}

std::unique_ptr<InverterDriver> readOnlyFactory(Transport& transport,
                                                const DriverOptions& options) {
    (void)transport;
    return std::make_unique<MockDriver>(defaultClock, optionsFrom(options, /*writable=*/false));
}

std::unique_ptr<InverterDriver> writableFactory(Transport& transport,
                                                const DriverOptions& options) {
    (void)transport;
    return std::make_unique<MockDriver>(defaultClock, optionsFrom(options, /*writable=*/true));
}

}  // namespace heliograph::mock
