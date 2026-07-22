// SPDX-License-Identifier: MIT
// See protocols/pmu/pmu_protocol.h for provenance and licensing of the protocol knowledge.

#include "eversolar_parser.h"

#include <vector>

#include "protocols/pmu/pmu_protocol.h"  // kRegisterAck

namespace heliograph::eversolar {
namespace {

// Word indices for the single-string layout (14 words).
namespace single {
inline constexpr size_t kTemp        = 0;
inline constexpr size_t kEnergyToday = 1;
inline constexpr size_t kVpv         = 2;
inline constexpr size_t kIpv         = 3;
inline constexpr size_t kIac         = 4;
inline constexpr size_t kVac         = 5;
inline constexpr size_t kFrequency   = 6;
inline constexpr size_t kPac         = 7;
inline constexpr size_t kImpedance   = 8;
inline constexpr size_t kEnergyTotalHigh = 9;
inline constexpr size_t kEnergyTotalLow  = 10;
// word 11 (NA_2) unknown
inline constexpr size_t kHoursUp = 12;
inline constexpr size_t kOpMode  = 13;
}  // namespace single

// Word indices for the dual-string layout (16 words). Note that IAC/VAC swap places and
// PAC moves: reading one layout with the other's indices produces believable garbage.
namespace dual {
inline constexpr size_t kTemp        = 0;
inline constexpr size_t kEnergyToday = 1;
inline constexpr size_t kVpv         = 2;
inline constexpr size_t kVpv2        = 3;
inline constexpr size_t kIpv         = 4;
inline constexpr size_t kIpv2        = 5;
inline constexpr size_t kIac         = 6;
inline constexpr size_t kVac         = 7;
inline constexpr size_t kFrequency   = 8;
inline constexpr size_t kPac         = 9;
// word 10 (NA_0) unknown
inline constexpr size_t kEnergyTotalHigh = 11;
inline constexpr size_t kEnergyTotalLow  = 12;
// word 13 (NA_2) unknown
inline constexpr size_t kHoursUp = 14;
inline constexpr size_t kOpMode  = 15;
}  // namespace dual

bool isPrintable(uint8_t c) { return c >= 0x20 && c <= 0x7E; }

std::string toTrimmedString(const uint8_t* data, size_t len) {
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == 0x00) {
            break;  // NUL terminates, matching the reference's Z*/A* unpack
        }
        if (isPrintable(data[i])) {
            s.push_back(static_cast<char>(data[i]));
        }
    }
    // Trim trailing spaces; the ID string is space padded on some models.
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    return s;
}

}  // namespace

uint16_t readWord(const uint8_t* data, size_t index) {
    const size_t o = index * 2;
    return static_cast<uint16_t>((data[o] << 8) | data[o + 1]);
}

int16_t readSignedWord(const uint8_t* data, size_t index) {
    return static_cast<int16_t>(readWord(data, index));
}

uint32_t readDoubleWord(const uint8_t* data, size_t highIndex, size_t lowIndex) {
    return (static_cast<uint32_t>(readWord(data, highIndex)) << 16) | readWord(data, lowIndex);
}

DecodeResult decodeNormalInfo(const uint8_t* data, size_t len, NormalInfo& out,
                              LayoutSelection selection) {
    if (data == nullptr) {
        return DecodeResult::UnknownLayout;
    }

    NormalInfoLayout layout;
    switch (selection) {
        case LayoutSelection::Auto:
            // Known payload lengths only; anything else is rejected outright rather than
            // guessed at: the two layouts order their fields differently, so a wrong guess
            // yields believable numbers that are silently wrong. 44 is hardware truth, not
            // hypothesis: a real single-string TL3000-20 sends the 14 single-string words
            // plus an 8-word tail (captured 2026-07-19, kRespNormalInfoCaptured).
            if (len == kNormalInfoSingleStringBytes ||
                len == kNormalInfoSingleStringExtendedBytes) {
                layout = NormalInfoLayout::SingleString;
            } else if (len == kNormalInfoDualStringBytes ||
                       len == kNormalInfoDualStringExtendedBytes) {
                layout = NormalInfoLayout::DualString;
            } else {
                return DecodeResult::UnknownLayout;
            }
            break;
        case LayoutSelection::ForceSingleString:
            if (len < kNormalInfoSingleStringBytes) {
                return DecodeResult::LayoutMismatch;
            }
            layout = NormalInfoLayout::SingleString;
            break;
        case LayoutSelection::ForceDualString:
            if (len < kNormalInfoDualStringBytes) {
                return DecodeResult::LayoutMismatch;
            }
            layout = NormalInfoLayout::DualString;
            break;
        default:
            return DecodeResult::UnknownLayout;
    }

    out = NormalInfo{};

    if (layout == NormalInfoLayout::SingleString) {
        out.layout          = NormalInfoLayout::SingleString;
        out.temperatureC    = readSignedWord(data, single::kTemp) / 10.0;
        out.energyTodayKwh  = readWord(data, single::kEnergyToday) / 100.0;
        out.pvVoltage1      = readWord(data, single::kVpv) / 10.0;
        out.pvCurrent1      = readWord(data, single::kIpv) / 10.0;
        out.acCurrent       = readWord(data, single::kIac) / 10.0;
        out.acVoltage       = readWord(data, single::kVac) / 10.0;
        out.frequencyHz     = readWord(data, single::kFrequency) / 100.0;
        out.acPowerW        = readWord(data, single::kPac);  // no scale factor
        out.impedanceValid  = true;
        out.impedanceRaw    = readWord(data, single::kImpedance);
        out.energyTotalKwh =
            readDoubleWord(data, single::kEnergyTotalHigh, single::kEnergyTotalLow) / 10.0;
        out.operatingHours = readWord(data, single::kHoursUp);
        out.statusCode     = readWord(data, single::kOpMode);
        out.hasSecondString = false;
    } else {
        out.layout          = NormalInfoLayout::DualString;
        out.temperatureC    = readSignedWord(data, dual::kTemp) / 10.0;
        out.energyTodayKwh  = readWord(data, dual::kEnergyToday) / 100.0;
        out.pvVoltage1      = readWord(data, dual::kVpv) / 10.0;
        out.pvCurrent1      = readWord(data, dual::kIpv) / 10.0;
        out.pvVoltage2      = readWord(data, dual::kVpv2) / 10.0;
        out.pvCurrent2      = readWord(data, dual::kIpv2) / 10.0;
        out.acCurrent       = readWord(data, dual::kIac) / 10.0;
        out.acVoltage       = readWord(data, dual::kVac) / 10.0;
        out.frequencyHz     = readWord(data, dual::kFrequency) / 100.0;
        out.acPowerW        = readWord(data, dual::kPac);
        out.impedanceValid  = false;  // word 8 is FREQUENCY here; impedance is not reported
        out.energyTotalKwh =
            readDoubleWord(data, dual::kEnergyTotalHigh, dual::kEnergyTotalLow) / 10.0;
        out.operatingHours  = readWord(data, dual::kHoursUp);
        out.statusCode      = readWord(data, dual::kOpMode);
        out.hasSecondString = true;
    }

    return DecodeResult::Ok;
}

bool parseSerialNumber(const uint8_t* data, size_t len, std::string& out) {
    if (data == nullptr || len == 0) {
        return false;
    }
    out = toTrimmedString(data, len);
    return !out.empty();
}

bool isRegistrationAck(const uint8_t* data, size_t len) {
    return data != nullptr && len >= 1 && data[0] == kRegisterAck;
}

bool parseInverterId(const uint8_t* data, size_t len, std::string& out) {
    if (data == nullptr || len == 0) {
        return false;
    }
    out = toTrimmedString(data, len);
    return !out.empty();
}

const char* opModeText(uint16_t code) {
    switch (code) {
        case 0: return "Standby (not feeding)";
        case 1: return "Grid-connected (normal)";
        default: break;  // never observed: the caller reports "Unknown (n)"
    }
    return "";
}

std::string modelFromIdString(const std::string& idString, const std::string& manufacturer) {
    if (manufacturer.empty()) {
        return {};
    }
    // Fields are padded with runs of spaces; a single space could sit inside a field, so only
    // two or more count as a separator.
    std::vector<std::string> fields;
    std::string              current;
    size_t                   spaces = 0;
    for (const char c : idString) {
        if (c == ' ') {
            ++spaces;
            continue;
        }
        if (spaces >= 2 && !current.empty()) {
            fields.push_back(current);
            current.clear();
        } else if (spaces == 1 && !current.empty()) {
            current.push_back(' ');
        }
        spaces = 0;
        current.push_back(c);
    }
    if (!current.empty()) {
        fields.push_back(current);
    }

    for (size_t i = 1; i < fields.size(); ++i) {
        if (fields[i] == manufacturer) {
            return fields[i - 1];
        }
    }
    return {};
}

}  // namespace heliograph::eversolar
