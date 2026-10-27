// SPDX-License-Identifier: MIT

#include "profile_tables.h"

#include <cstring>

// The profile tables (register maps, read blocks) live in profiles_generated.cpp, emitted
// at build time from profiles/growatt/*.toml. This file keeps only the decode logic.

namespace heliograph::profile {

namespace {

/// Per-string PV power, in the order the canonical model numbers the strings.
/// ponytail: five, because the canonical model names five. A profile with mppts > 5 never
/// derives a total; add the ids here when the model grows.
constexpr const char* kStringPower[] = {
    measurement_id::kDcMppt1Power, measurement_id::kDcMppt2Power, measurement_id::kDcMppt3Power,
    measurement_id::kDcMppt4Power, measurement_id::kDcMppt5Power,
};

bool profileMaps(const DeviceProfile& profile, const char* id) {
    for (size_t i = 0; i < profile.mappingCount; ++i) {
        if (std::strcmp(profile.mappings[i].measurementId, id) == 0) {
            return true;
        }
    }
    return false;
}

/// dc.power.total for a profile that reads power per string but has no total register --
/// several inverters simply do not report one, and four shipped profiles were publishing their
/// strings with no total beside them.
///
/// A device-reported total is never overruled: if the profile maps one, this does nothing.
///
/// The profile must map power for every tracker it declares (`mppts`). A profile that declares
/// three and maps two would otherwise publish two strings' worth as the whole array. That is
/// the most a profile can vouch for: the Deye profile says outright that larger units in the
/// family have a third or fourth string it does not map, so on such a unit the sum is of the
/// strings this map reads -- which is what the display name says, and why it is marked derived.
///
/// Every string the profile maps must have been read in THIS poll (timestamp == ts). A block
/// that was skipped leaves its channels holding the previous reading, so summing whatever is
/// valid would add two moments together and publish a total that never existed. One string
/// missing means no total, not a smaller one.
void deriveDcTotal(const DeviceProfile& profile, MeasurementSet& measurements, uint64_t ts) {
    if (profileMaps(profile, measurement_id::kDcPowerTotal)) {
        return;
    }
    size_t mapped = 0;
    for (const char* id : kStringPower) {
        mapped += profileMaps(profile, id) ? 1 : 0;
    }
    if (mapped == 0 || mapped != profile.mpptCount) {
        return;
    }
    double sum = 0.0;
    for (const char* id : kStringPower) {
        if (!profileMaps(profile, id)) {
            continue;
        }
        const Measurement* m = measurements.find(id);
        if (m == nullptr || !m->valid || m->timestampMs != ts) {
            measurements.invalidate(measurement_id::kDcPowerTotal);
            return;
        }
        sum += m->value;
    }
    measurements.declare(measurement_id::kDcPowerTotal, MeasurementType::Power, Unit::Watt,
                         "PV Power (sum of strings)", /*derived=*/true);
    measurements.set(measurement_id::kDcPowerTotal, sum, ts);
}

}  // namespace

const char* profileStatusName(ProfileStatus status) {
    switch (status) {
        case ProfileStatus::Experimental: return "experimental";
        case ProfileStatus::Beta:         return "beta";
        case ProfileStatus::Stable:       return "stable";
        case ProfileStatus::Deprecated:   return "deprecated";
    }
    return "unknown";
}

bool findRegister(const BlockData* blocks, size_t blockCount, RegSpace space, uint16_t address,
                  uint16_t& out) {
    for (size_t i = 0; i < blockCount; ++i) {
        const BlockData& b = blocks[i];
        if (b.space == space && address >= b.start && address < b.start + b.count) {
            out = b.values[address - b.start];
            return true;
        }
    }
    return false;
}

void applyProfile(const DeviceProfile& profile, const BlockData* blocks, size_t blockCount,
                  MeasurementSet& measurements, uint64_t ts) {
    for (size_t i = 0; i < profile.mappingCount; ++i) {
        const RegisterMapping& mp = profile.mappings[i];

        uint16_t hi = 0;
        if (!findRegister(blocks, blockCount, mp.space, mp.address, hi)) {
            continue;  // block not read -> leave undeclared, never a fabricated zero
        }
        int64_t raw = hi;
        if (mp.words == 2) {
            uint16_t second = 0;
            if (!findRegister(blocks, blockCount, mp.space, mp.address + 1, second)) {
                continue;
            }
            // `hi` is whatever sits at the mapping's own address; which HALF that is depends on
            // the device. Naming the locals after position rather than significance is deliberate
            // -- the bug this guards against is exactly the one where the two get conflated.
            const uint16_t high = mp.lowWordFirst ? second : hi;
            const uint16_t low  = mp.lowWordFirst ? hi : second;
            raw = (static_cast<int64_t>(high) << 16) | low;
        }

        // Before sign extension: the sentinel is a bit pattern the vendor documents, not a
        // number, so it is compared as one.
        if (mp.hasInvalid && static_cast<uint32_t>(raw) == mp.invalidRaw) {
            continue;  // undeclared, like a block that was never read
        }

        if (mp.isSigned) {
            const int64_t bits = mp.words == 2 ? 32 : 16;
            const int64_t signBit = int64_t{1} << (bits - 1);
            if (raw & signBit) {
                raw -= (int64_t{1} << bits);  // two's-complement sign-extend
            }
        }

        measurements.declare(mp.measurementId, mp.type, mp.unit, mp.displayName);
        measurements.set(mp.measurementId, static_cast<double>(raw) * mp.scale + mp.offset, ts);
    }
    deriveDcTotal(profile, measurements, ts);
}

bool decodeStatus(const DeviceProfile& profile, const BlockData* blocks, size_t blockCount,
                  uint16_t& code, std::string& text) {
    const StatusRegister* sr = profile.statusRegister;
    if (sr == nullptr || !findRegister(blocks, blockCount, sr->space, sr->address, code)) {
        return false;
    }
    for (size_t i = 0; i < sr->nameCount; ++i) {
        if (sr->names[i].value == code) {
            text = sr->names[i].label;
            return true;
        }
    }
    text = "Unknown (" + std::to_string(code) + ")";
    return true;
}

}  // namespace heliograph::profile
