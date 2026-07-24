// SPDX-License-Identifier: MIT

#include "sunspec_parser.h"

#include <cmath>

namespace heliograph::sunspec {
namespace {

/// SunSpec scale factors are documented as -10..10. Anything outside that is not a scale
/// factor the device meant, so the reading is dropped rather than multiplied by a wild power
/// of ten -- an absent value is honest, a value off by 10^30 is not.
constexpr int kMinExponent = -10;
constexpr int kMaxExponent = 10;

double powerOfTen(int exponent) {
    double result = 1.0;
    if (exponent >= 0) {
        for (int i = 0; i < exponent; ++i) {
            result *= 10.0;
        }
    } else {
        for (int i = 0; i < -exponent; ++i) {
            result /= 10.0;
        }
    }
    return result;
}

}  // namespace

bool isInverterModel(uint16_t modelId) {
    return modelId == kModelInverterSinglePhase || modelId == kModelInverterSplitPhase ||
           modelId == kModelInverterThreePhase;
}

ScaleFactor decodeScaleFactor(uint16_t raw) {
    ScaleFactor sf;
    if (raw == kNotImplementedS16) {
        return sf;  // valid stays false
    }
    const int value = static_cast<int16_t>(raw);
    if (value < kMinExponent || value > kMaxExponent) {
        return sf;
    }
    sf.valid    = true;
    sf.exponent = value;
    return sf;
}

bool applyScale(uint16_t raw, bool isSigned, const ScaleFactor& sf, double& out) {
    if (!sf.valid) {
        return false;
    }
    if (isSigned) {
        if (raw == kNotImplementedS16) {
            return false;
        }
        out = static_cast<double>(static_cast<int16_t>(raw)) * powerOfTen(sf.exponent);
        return true;
    }
    if (raw == kNotImplementedU16) {
        return false;
    }
    out = static_cast<double>(raw) * powerOfTen(sf.exponent);
    return true;
}

bool applyScaleAcc32(uint16_t high, uint16_t low, const ScaleFactor& sf, double& out) {
    if (!sf.valid) {
        return false;
    }
    const uint32_t raw = (static_cast<uint32_t>(high) << 16) | low;
    out                = static_cast<double>(raw) * powerOfTen(sf.exponent);
    return true;
}

std::string decodeString(const uint16_t* regs, size_t registerCount) {
    std::string s;
    s.reserve(registerCount * 2);
    for (size_t i = 0; i < registerCount; ++i) {
        const char hi = static_cast<char>(regs[i] >> 8);
        const char lo = static_cast<char>(regs[i] & 0xFF);
        if (hi == '\0') {
            break;
        }
        s.push_back(hi);
        if (lo == '\0') {
            break;
        }
        s.push_back(lo);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) {
        s.pop_back();
    }
    return s;
}

bool decodeInverter(const uint16_t* regs, size_t count, InverterReadings& out) {
    if (regs == nullptr || count < inverter::kMinRegisters) {
        return false;
    }
    out = InverterReadings{};

    const ScaleFactor aSf   = decodeScaleFactor(regs[inverter::kA_SF]);
    const ScaleFactor vSf   = decodeScaleFactor(regs[inverter::kV_SF]);
    const ScaleFactor wSf   = decodeScaleFactor(regs[inverter::kW_SF]);
    const ScaleFactor hzSf  = decodeScaleFactor(regs[inverter::kHz_SF]);
    const ScaleFactor whSf  = decodeScaleFactor(regs[inverter::kWH_SF]);
    const ScaleFactor dcwSf = decodeScaleFactor(regs[inverter::kDCW_SF]);
    const ScaleFactor tmpSf = decodeScaleFactor(regs[inverter::kTmp_SF]);

    out.hasAcPower = applyScale(regs[inverter::kW], /*isSigned=*/true, wSf, out.acPowerW);
    out.hasAcVoltage =
        applyScale(regs[inverter::kPhVphA], /*isSigned=*/false, vSf, out.acVoltageV);
    out.hasAcCurrent = applyScale(regs[inverter::kAphA], /*isSigned=*/false, aSf, out.acCurrentA);
    out.hasFrequency = applyScale(regs[inverter::kHz], /*isSigned=*/false, hzSf, out.frequencyHz);
    out.hasDcPower   = applyScale(regs[inverter::kDCW], /*isSigned=*/true, dcwSf, out.dcPowerW);
    out.hasTemperature =
        applyScale(regs[inverter::kTmpCab], /*isSigned=*/true, tmpSf, out.temperatureC);

    double wh = 0;
    if (applyScaleAcc32(regs[inverter::kWH], regs[inverter::kWH + 1], whSf, wh)) {
        // SunSpec accumulates watt-hours; the canonical channel is kWh.
        out.hasEnergyTotal = true;
        out.energyTotalKwh = wh / 1000.0;
    }

    const uint16_t st = regs[inverter::kSt];
    if (st != kNotImplementedU16) {
        out.hasState = true;
        out.state    = st;
    }
    return true;
}

bool decodeCommon(const uint16_t* regs, size_t count, CommonIdentity& out) {
    if (regs == nullptr || count < common::kMinRegisters) {
        return false;
    }
    out.manufacturer = decodeString(regs + common::kMn, 16);
    out.model        = decodeString(regs + common::kMd, 16);
    out.version      = decodeString(regs + common::kVr, 8);
    out.serial       = decodeString(regs + common::kSN, 16);
    return true;
}

}  // namespace heliograph::sunspec
