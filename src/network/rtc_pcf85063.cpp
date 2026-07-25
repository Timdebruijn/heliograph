// SPDX-License-Identifier: MIT

#include "rtc_pcf85063.h"

#include "rtc_time.h"

#if defined(ESP32)

#include <Wire.h>

#include "boards/board.h"
#include "diagnostics/logger.h"

namespace heliograph::rtc {
namespace {

// PCF85063A register map (datasheet rev. 8, section 7).
constexpr uint8_t kRegControl1 = 0x00;
constexpr uint8_t kRegSeconds  = 0x04;  // bit 7 = OS: oscillator stopped, time unreliable
constexpr uint8_t kOsFlag      = 0x80;

bool    g_present = false;

bool readRegs(uint8_t reg, uint8_t* buf, size_t n) {
    Wire.beginTransmission(board::kRtcI2cAddress);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom(static_cast<int>(board::kRtcI2cAddress), static_cast<int>(n)) !=
        static_cast<int>(n)) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        buf[i] = static_cast<uint8_t>(Wire.read());
    }
    return true;
}

}  // namespace

bool begin() {
    if constexpr (!board::kHasRtc) {
        return false;
    }
    Wire.begin(board::kRtcSda, board::kRtcScl);
    uint8_t ctrl = 0;
    g_present    = readRegs(kRegControl1, &ctrl, 1);
    if (!g_present) {
        log::warn("rtc: board declares a PCF85063 but it did not answer on I2C");
    }
    return g_present;
}

bool readUtc(time_t& out) {
    if (!g_present) {
        return false;
    }
    uint8_t r[7];
    if (!readRegs(kRegSeconds, r, sizeof(r))) {
        return false;
    }
    // Everything past the I2C read is pure and lives in rtc_time.cpp, where it is host-tested:
    // the OS flag, BCD validation, range and year bounds. A wrong decode here is silent, so it
    // is the half that must not be untestable.
    return decodeRegisters(r, out);
}

bool writeUtc(time_t t) {
    if (!g_present || t <= 0) {
        return false;
    }
    uint8_t r[7];
    encodeRegisters(t, r);
    r[4] = static_cast<uint8_t>(((t / 86400) + 4) % 7);  // weekday; 1970-01-01 was a Thursday

    Wire.beginTransmission(board::kRtcI2cAddress);
    Wire.write(kRegSeconds);
    for (uint8_t b : r) {
        Wire.write(b);
    }
    return Wire.endTransmission() == 0;
}

}  // namespace heliograph::rtc

#else  // !ESP32

namespace heliograph::rtc {
bool begin() { return false; }
bool readUtc(time_t&) { return false; }
bool writeUtc(time_t) { return false; }
}  // namespace heliograph::rtc

#endif
