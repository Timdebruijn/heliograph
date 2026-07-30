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
// L2/L3 exist as constants because three-phase drivers already publish them: a canonical id
// that only lives as a string literal inside a driver cannot be enumerated, and so cannot be
// cleared when the device that published it goes away (review, 2026-07-26).
inline constexpr const char* kAcL2Voltage    = "ac.phase_l2.voltage";
inline constexpr const char* kAcL2Current    = "ac.phase_l2.current";
inline constexpr const char* kAcL2Power      = "ac.phase_l2.power";
inline constexpr const char* kAcL3Voltage    = "ac.phase_l3.voltage";
inline constexpr const char* kAcL3Current    = "ac.phase_l3.current";
inline constexpr const char* kAcL3Power      = "ac.phase_l3.power";
inline constexpr const char* kDcPowerTotal   = "dc.power.total";
inline constexpr const char* kDcMppt1Voltage = "dc.mppt_1.voltage";
inline constexpr const char* kDcMppt1Current = "dc.mppt_1.current";
inline constexpr const char* kDcMppt1Power   = "dc.mppt_1.power";
inline constexpr const char* kDcMppt2Voltage = "dc.mppt_2.voltage";
inline constexpr const char* kDcMppt2Current = "dc.mppt_2.current";
inline constexpr const char* kDcMppt2Power   = "dc.mppt_2.power";
/// Strings 3 to 5. A profile could always DECLARE up to eight trackers -- `mppts` has allowed
/// 0-8 since the schema existed -- while only two of them had anywhere to be published, so a
/// four-string hybrid reported its tracker count honestly and then dropped half its strings.
///
/// Five, not eight, and the ceiling is not arbitrary: the Modbus TCP map gives each tracker a
/// 20-register slot in the region 200-299 and the battery block starts at 300. Going further
/// means moving a published region, which is a schema break for every client reading it. Five
/// covers residential and small-commercial hardware; a sixth string is a reason to bump that
/// schema deliberately, not a reason to shuffle registers now.
inline constexpr const char* kDcMppt3Voltage = "dc.mppt_3.voltage";
inline constexpr const char* kDcMppt3Current = "dc.mppt_3.current";
inline constexpr const char* kDcMppt3Power   = "dc.mppt_3.power";
inline constexpr const char* kDcMppt4Voltage = "dc.mppt_4.voltage";
inline constexpr const char* kDcMppt4Current = "dc.mppt_4.current";
inline constexpr const char* kDcMppt4Power   = "dc.mppt_4.power";
inline constexpr const char* kDcMppt5Voltage = "dc.mppt_5.voltage";
inline constexpr const char* kDcMppt5Current = "dc.mppt_5.current";
inline constexpr const char* kDcMppt5Power   = "dc.mppt_5.power";
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
/// The raw rails, for a device that reads them separately and a consumer that wants them apart:
/// the Modbus TCP register map has held registers for both since it was written. They exist as
/// constants because ids that live only as string literals cannot be enumerated -- and an id
/// that cannot be enumerated is an entity that can never be cleared (review, 2026-07-26).
inline constexpr const char* kBatteryChargePower    = "battery.charge_power";
inline constexpr const char* kBatteryDischargePower = "battery.discharge_power";
/// House-level flows, for hybrids with a meter. Also long-standing Modbus registers.
inline constexpr const char* kGridImportPower = "grid.import_power";
inline constexpr const char* kGridExportPower = "grid.export_power";

/// What the inverter is currently limited TO, as a percentage of its nameplate maximum, and
/// whether that limit is switched on at all.
///
/// A control that is written must also be readable, or nobody can tell what the device is doing
/// -- only what it was last asked to do. Those differ whenever a write was refused, a second
/// controller is on the bus, or the inverter reverted the limit on its own timer, which several
/// of them do by design.
inline constexpr const char* kActivePowerLimitPct = "control.active_power_limit";
inline constexpr const char* kActivePowerLimitEnabled = "control.active_power_limit_enabled";

/// Every id above, in one place.
///
/// Needed because a device removed from the configuration leaves retained Home Assistant
/// discovery topics behind, and clearing them means naming every topic it could have
/// published -- which is derived from these ids. Kept in sync by tools/check_measurement_ids.py,
/// which fails the build if a constant above is missing here.
inline constexpr const char* kAll[] = {
    kAcPowerTotal,   kAcFrequency,    kAcL1Voltage,    kAcL1Current,    kAcL1Power,
    kAcL2Voltage,    kAcL2Current,    kAcL2Power,      kAcL3Voltage,    kAcL3Current,
    kAcL3Power,      kDcPowerTotal,   kDcMppt1Voltage, kDcMppt1Current, kDcMppt1Power,
    kDcMppt2Voltage, kDcMppt2Current, kDcMppt2Power,
    kDcMppt3Voltage, kDcMppt3Current, kDcMppt3Power,
    kDcMppt4Voltage, kDcMppt4Current, kDcMppt4Power,
    kDcMppt5Voltage, kDcMppt5Current, kDcMppt5Power,
    kEnergyToday,    kEnergyTotal,
    kTemperature,    kOperatingHours, kBatterySoc,     kBatteryPower,   kBatteryVoltage,
    kBatteryCurrent, kBatteryTemperature, kBatteryEnergyCharged, kBatteryEnergyDischarged,
    kBatteryChargePower, kBatteryDischargePower, kGridImportPower, kGridExportPower,
    kActivePowerLimitPct, kActivePowerLimitEnabled,
};

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
