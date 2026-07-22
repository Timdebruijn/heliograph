// SPDX-License-Identifier: MIT

#include "measurement.h"

#include <cstring>

namespace heliograph {

namespace {
// ids are const char* now, so == would compare pointers. They are all the same static
// constants in practice, but strcmp keeps a stray literal from silently missing.
inline bool idEquals(const char* a, const char* b) { return std::strcmp(a, b) == 0; }
}  // namespace

const char* unitSymbol(Unit unit) {
    switch (unit) {
        case Unit::Watt:         return "W";
        case Unit::Volt:         return "V";
        case Unit::Ampere:       return "A";
        case Unit::Hertz:        return "Hz";
        case Unit::Celsius:      return "°C";
        case Unit::KilowattHour: return "kWh";
        case Unit::Hour:         return "h";
        case Unit::Percent:      return "%";
        case Unit::Decibel:      return "dBm";
        case Unit::Second:       return "s";
        case Unit::None:         break;
    }
    return "";
}

Measurement* MeasurementSet::findMutable(const char* id) {
    for (auto& m : measurements_) {
        if (idEquals(m.id, id)) {
            return &m;
        }
    }
    return nullptr;
}

const Measurement* MeasurementSet::find(const char* id) const {
    for (const auto& m : measurements_) {
        if (idEquals(m.id, id)) {
            return &m;
        }
    }
    return nullptr;
}

void MeasurementSet::declare(const char* id, MeasurementType type, Unit unit,
                             const char* displayName, bool derived) {
    if (auto* existing = findMutable(id)) {
        existing->type        = type;
        existing->unit        = unit;
        existing->displayName = displayName;
        existing->derived     = derived;
        existing->supported   = true;
        return;
    }

    Measurement m;
    m.id          = id;
    m.type        = type;
    m.unit        = unit;
    m.displayName = displayName;
    m.derived     = derived;
    m.supported   = true;
    m.valid       = false;
    m.stale       = false;
    m.value       = 0.0;
    m.timestampMs = 0;
    measurements_.push_back(std::move(m));
}

void MeasurementSet::declareUnsupported(const char* id, MeasurementType type, Unit unit,
                                        const char* displayName) {
    declare(id, type, unit, displayName);
    auto* m     = findMutable(id);
    m->supported = false;
    m->valid     = false;
    m->stale     = false;
    m->value     = 0.0;
}

void MeasurementSet::set(const char* id, double value, uint64_t timestampMs) {
    auto* m = findMutable(id);
    if (m == nullptr) {
        // Undeclared channel: ignore rather than create. A driver must advertise a channel
        // before it can report on it, otherwise capability filtering downstream is a lie.
        return;
    }
    if (!m->supported) {
        // The driver declared this channel unreadable and is now reporting a value for it.
        // That is a driver bug; honouring it would publish data the capabilities deny.
        return;
    }
    m->value       = value;
    m->valid       = true;
    m->stale       = false;
    m->timestampMs = timestampMs;
}

void MeasurementSet::invalidate(const char* id) {
    if (auto* m = findMutable(id)) {
        m->valid = false;
        m->stale = false;
    }
}

bool MeasurementSet::isValid(const char* id) const {
    const auto* m = find(id);
    return m != nullptr && m->supported && m->valid;
}

void MeasurementSet::updateStaleness(uint64_t nowMs, uint64_t maxAgeMs) {
    for (auto& m : measurements_) {
        if (!m.valid) {
            continue;
        }
        // Guard against a clock that has not advanced past the stored timestamp.
        const uint64_t age = nowMs > m.timestampMs ? nowMs - m.timestampMs : 0;
        m.stale            = age > maxAgeMs;
    }
}

void MeasurementSet::markAllStale() {
    for (auto& m : measurements_) {
        if (m.valid) {
            m.stale = true;
        }
    }
}

}  // namespace heliograph
