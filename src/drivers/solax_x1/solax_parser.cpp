// SPDX-License-Identifier: MIT
// See solax_parser.h for provenance and the hardware-verification status.

#include "solax_parser.h"

namespace heliograph::solax {
namespace {

uint16_t u16At(const uint8_t* data, size_t offset) {
    return static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
}

int16_t s16At(const uint8_t* data, size_t offset) {
    return static_cast<int16_t>(u16At(data, offset));
}

uint32_t u32BigAt(const uint8_t* data, size_t offset) {
    return (static_cast<uint32_t>(data[offset]) << 24) |
           (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) |
           static_cast<uint32_t>(data[offset + 3]);
}

uint32_t u32LittleAt(const uint8_t* data, size_t offset) {
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

/// Fixed-width space/NUL-padded ASCII field -> trimmed string. Non-printable bytes are
/// dropped rather than copied, so a corrupt field cannot smuggle control characters into
/// MQTT topics or web pages.
std::string asciiField(const uint8_t* data, size_t offset, size_t width) {
    std::string out;
    for (size_t i = 0; i < width; ++i) {
        const char c = static_cast<char>(data[offset + i]);
        if (c >= 0x21 && c <= 0x7E) {
            out.push_back(c);
        } else if (c == ' ' && !out.empty()) {
            out.push_back(' ');  // interior spaces stay; leading ones do not
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

}  // namespace

DecodeResult decodeStatusReport(const uint8_t* data, size_t len, StatusReport& out) {
    if (len < kStatusReportMinBytes) {
        return DecodeResult::TooShort;
    }
    out                = StatusReport{};
    out.temperatureC   = static_cast<double>(s16At(data, 0));
    out.energyTodayKwh = u16At(data, 2) * 0.1;
    out.pv1Voltage     = u16At(data, 4) * 0.1;
    out.pv2Voltage     = u16At(data, 6) * 0.1;
    out.pv1Current     = u16At(data, 8) * 0.1;
    out.pv2Current     = u16At(data, 10) * 0.1;
    out.acCurrent      = u16At(data, 12) * 0.1;
    out.acVoltage      = u16At(data, 14) * 0.1;
    out.frequencyHz    = u16At(data, 16) * 0.01;
    out.acPowerW       = static_cast<double>(u16At(data, 18));
    // offset 20 unused per the reference.
    out.energyTotalKwh = u32BigAt(data, 22) * 0.1;
    out.runtimeHours   = u32BigAt(data, 26);
    out.mode           = u16At(data, 30);
    // offsets 32-44: protection thresholds, deliberately not decoded (see header).
    out.errorBits = u32LittleAt(data, 46);
    return DecodeResult::Ok;
}

const char* modeText(uint16_t mode) {
    switch (mode) {
        case 0: return "Wait";
        case 1: return "Check";
        case 2: return "Normal";
        case 3: return "Fault";
        case 4: return "Permanent Fault";
        case 5: return "Update";
        case 6: return "Self Test";
        default: break;
    }
    return "";  // unknown mode: no invented name
}

bool decodeDeviceInfo(const uint8_t* data, size_t len, DeviceInfo& out) {
    if (len != kDeviceInfoBytes) {
        return false;
    }
    out                 = DeviceInfo{};
    out.deviceType      = data[0];
    out.ratedPower      = asciiField(data, 1, 6);
    out.firmwareVersion = asciiField(data, 7, 5);
    out.moduleName      = asciiField(data, 12, 14);
    out.factoryName     = asciiField(data, 26, 14);
    out.serialNumber    = asciiField(data, 40, 14);
    return true;
}

bool serialLooksValid(const uint8_t* data, size_t len) {
    if (data == nullptr || len != kSerialNumberBytes) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (data[i] > 0x20 && data[i] < 0x7F) {
            return true;
        }
    }
    return false;
}

}  // namespace heliograph::solax
