// SPDX-License-Identifier: MIT
// See solax_parser.h for provenance and the hardware-verification status.

#include "solax_parser.h"

#include "protocols/byte_order.h"

namespace heliograph::solax {
namespace {

using bytes::be16;
using bytes::be16s;
using bytes::be32;
using bytes::le32;

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
    out.temperatureC   = static_cast<double>(be16s(data, 0));
    out.energyTodayKwh = be16(data, 2) * 0.1;
    out.pv1Voltage     = be16(data, 4) * 0.1;
    out.pv2Voltage     = be16(data, 6) * 0.1;
    out.pv1Current     = be16(data, 8) * 0.1;
    out.pv2Current     = be16(data, 10) * 0.1;
    out.acCurrent      = be16(data, 12) * 0.1;
    out.acVoltage      = be16(data, 14) * 0.1;
    out.frequencyHz    = be16(data, 16) * 0.01;
    out.acPowerW       = static_cast<double>(be16(data, 18));
    // offset 20 unused per the reference.
    out.energyTotalKwh = be32(data, 22) * 0.1;
    out.runtimeHours   = be32(data, 26);
    out.mode           = be16(data, 30);
    // offsets 32-44: protection thresholds, deliberately not decoded (see header).
    out.errorBits = le32(data, 46);
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
