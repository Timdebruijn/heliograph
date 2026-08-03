// SPDX-License-Identifier: MIT
//
// Serial line settings. Each driver advertises the profiles that are actually plausible for
// its protocol; discovery never brute-forces combinations.

#pragma once

#include <cstdint>
#include <string>

namespace heliograph {

enum class SerialParity : uint8_t { None, Even, Odd };

struct SerialProfile {
    uint32_t     baudRate          = 9600;
    SerialParity parity            = SerialParity::None;
    uint8_t      dataBits          = 8;
    uint8_t      stopBits          = 1;
    uint32_t     responseTimeoutMs = 1000;

    friend bool operator==(const SerialProfile& a, const SerialProfile& b) {
        return a.baudRate == b.baudRate && a.parity == b.parity && a.dataBits == b.dataBits &&
               a.stopBits == b.stopBits;
    }
};

const char* parityName(SerialParity parity);
/// Inverse of parityName(). False on anything else, so a stored or patched value that does not
/// name a parity is refused rather than quietly becoming None.
bool        parseParity(const std::string& name, SerialParity& out);

}  // namespace heliograph
