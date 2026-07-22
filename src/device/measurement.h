// SPDX-License-Identifier: MIT
//
// Canonical measurement model. Driver-agnostic by construction: nothing here knows about any
// specific manufacturer or protocol. Platform independent (host-testable).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace heliograph {

enum class Unit : uint8_t {
    None,
    Watt,
    Volt,
    Ampere,
    Hertz,
    Celsius,
    KilowattHour,
    Hour,
    Percent,
    Decibel,
    Second,
};

enum class MeasurementType : uint8_t {
    Power,
    Voltage,
    Current,
    Frequency,
    Temperature,
    Energy,
    Duration,
    Ratio,
    SignalStrength,
    Generic,
};

const char* unitSymbol(Unit unit);

/// One measurement channel.
///
/// The three flags are deliberately distinct and must not be collapsed:
///   supported=false            -> the driver never provides this; do not publish at all
///   supported, !valid          -> known channel, no usable reading yet -> publish null/NaN
///   supported, valid, stale    -> last known value, too old to trust
///   supported, valid, !stale   -> current reading
///
/// An unsupported or invalid channel must never be published as 0: a zero is a real
/// measurement (an inverter at night genuinely produces 0 W) and the two must stay
/// distinguishable downstream.
struct Measurement {
    // const char*, not std::string: these are always compile-time literals (the
    // measurement_id::k* constants and fixed display names), and declare() runs from inside
    // poll() every cycle while the whole set is deep-copied twice per poll. As std::string
    // that was ~80-120 short-lived heap allocations per poll on a months-uptime device -- a
    // classic fragmentation hazard. As const char* it is pure pointer copies. Callers must pass
    // pointers with static lifetime (they always do). Default "" keeps strcmp safe.
    const char*     id          = "";  ///< semantic id, e.g. "ac.power.total"
    const char*     displayName = "";
    MeasurementType type      = MeasurementType::Generic;
    Unit            unit      = Unit::None;
    double          value     = 0.0;
    bool            supported = false;
    bool            valid     = false;
    bool            stale     = false;
    /// Computed rather than measured (e.g. dc.power.total = V x I). Consumers should not
    /// infer meter-grade precision from a derived value.
    bool     derived     = false;
    uint64_t timestampMs = 0;
};

/// Well-known measurement ids. Drivers fill only what they actually read.
namespace measurement_id {
inline constexpr const char* kAcPowerTotal   = "ac.power.total";
inline constexpr const char* kAcFrequency    = "ac.frequency";
inline constexpr const char* kAcL1Voltage    = "ac.phase_l1.voltage";
inline constexpr const char* kAcL1Current    = "ac.phase_l1.current";
inline constexpr const char* kAcL1Power      = "ac.phase_l1.power";
inline constexpr const char* kDcPowerTotal   = "dc.power.total";
inline constexpr const char* kDcMppt1Voltage = "dc.mppt_1.voltage";
inline constexpr const char* kDcMppt1Current = "dc.mppt_1.current";
inline constexpr const char* kDcMppt1Power   = "dc.mppt_1.power";
inline constexpr const char* kDcMppt2Voltage = "dc.mppt_2.voltage";
inline constexpr const char* kDcMppt2Current = "dc.mppt_2.current";
inline constexpr const char* kDcMppt2Power   = "dc.mppt_2.power";
inline constexpr const char* kEnergyToday    = "energy.today";
inline constexpr const char* kEnergyTotal    = "energy.total";
inline constexpr const char* kTemperature    = "inverter.temperature";
inline constexpr const char* kOperatingHours = "inverter.operating_hours";
// Battery / hybrid channels. Named after the physical quantity, not any vendor register, and
// shaped after the SunSpec energy-storage model (Model 120: SoC, charge/discharge power,
// setpoints) so a hybrid driver maps onto a standard rather than inventing its own vocabulary.
// Sign convention for battery.power: positive = charging, negative = discharging (the SunSpec
// convention); a driver that reads separate charge/discharge rails combines them into this.
inline constexpr const char* kBatterySoc          = "battery.soc";
inline constexpr const char* kBatteryPower        = "battery.power";
inline constexpr const char* kBatteryVoltage      = "battery.voltage";
inline constexpr const char* kBatteryCurrent      = "battery.current";
inline constexpr const char* kBatteryTemperature  = "battery.temperature";
inline constexpr const char* kBatteryEnergyCharged    = "battery.energy_charged";
inline constexpr const char* kBatteryEnergyDischarged = "battery.energy_discharged";
}  // namespace measurement_id

/// An ordered collection of measurements keyed by id.
///
/// Deliberately a vector rather than a map: the sets are small (< 32 entries), iteration
/// order must be stable for the Modbus validity bitmap, and there is no heap churn per
/// lookup on an embedded target.
class MeasurementSet {
public:
    /// Registers a channel the driver can provide. Idempotent: re-declaring updates metadata
    /// but keeps any existing value. Declared channels start supported but not valid.
    void declare(const char* id, MeasurementType type, Unit unit,
                 const char* displayName, bool derived = false);

    /// Registers a channel the driver knows about but cannot read.
    ///
    /// There are two ways to say "this device does not have that": leave the channel out
    /// entirely, or declare it unsupported. Both are honoured identically by every output --
    /// nothing is published either way. The difference is intent, and it is worth having:
    ///   - omit, when the device simply has no such thing (a string inverter has no battery);
    ///   - declare unsupported, when the device has it but this protocol or firmware does not
    ///     expose it, so a fixed schema stays visible and a later driver revision can fill it
    ///     in without the channel set changing shape.
    ///
    /// Without this, `supported` would be a field that is always true -- a flag three output
    /// adapters check and nothing can ever set, which is a trap rather than a safeguard.
    void declareUnsupported(const char* id, MeasurementType type, Unit unit,
                            const char* displayName);

    /// Records a reading. No-op if the id was never declared, so a driver cannot silently
    /// introduce a channel it did not advertise via its capabilities.
    void set(const char* id, double value, uint64_t timestampMs);

    /// Marks a declared channel as having no usable reading. Keeps it supported.
    void invalidate(const char* id);

    const Measurement* find(const char* id) const;
    bool               isValid(const char* id) const;

    /// Marks every valid channel older than `maxAgeMs` as stale, and clears the flag on
    /// those that are fresh again. Values are retained either way.
    void updateStaleness(uint64_t nowMs, uint64_t maxAgeMs);

    /// Marks all channels stale without touching their values, e.g. when the device goes
    /// offline but the last readings remain informative.
    void markAllStale();

    const std::vector<Measurement>& all() const { return measurements_; }
    size_t                          size() const { return measurements_.size(); }
    void                            clear() { measurements_.clear(); }

private:
    Measurement*             findMutable(const char* id);
    std::vector<Measurement> measurements_;
};

}  // namespace heliograph
