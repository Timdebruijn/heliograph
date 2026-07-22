// SPDX-License-Identifier: MIT

#include "growatt_registers.h"

// The profile tables (register maps, read blocks) live in profiles_generated.cpp, emitted
// at build time from profiles/growatt/*.toml. This file keeps only the decode logic.

namespace heliograph::growatt {

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

void applyProfile(const GrowattProfile& profile, const BlockData* blocks, size_t blockCount,
                  MeasurementSet& measurements, uint64_t ts) {
    for (size_t i = 0; i < profile.mappingCount; ++i) {
        const RegisterMapping& mp = profile.mappings[i];

        uint16_t hi = 0;
        if (!findRegister(blocks, blockCount, mp.space, mp.address, hi)) {
            continue;  // block not read -> leave undeclared, never a fabricated zero
        }
        int64_t raw = hi;
        if (mp.words == 2) {
            uint16_t lo = 0;
            if (!findRegister(blocks, blockCount, mp.space, mp.address + 1, lo)) {
                continue;
            }
            raw = (static_cast<int64_t>(hi) << 16) | lo;
        }

        if (mp.isSigned) {
            const int64_t bits = mp.words == 2 ? 32 : 16;
            const int64_t signBit = int64_t{1} << (bits - 1);
            if (raw & signBit) {
                raw -= (int64_t{1} << bits);  // two's-complement sign-extend
            }
        }

        measurements.declare(mp.measurementId, mp.type, mp.unit, mp.displayName);
        measurements.set(mp.measurementId, static_cast<double>(raw) * mp.scale, ts);
    }
}

}  // namespace heliograph::growatt
