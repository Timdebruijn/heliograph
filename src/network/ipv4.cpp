// SPDX-License-Identifier: MIT

#include "ipv4.h"

namespace heliograph::net {

bool parseIpv4(const std::string& text, uint32_t& out) {
    uint32_t addr   = 0;
    size_t   i      = 0;
    const size_t n  = text.size();
    for (int octet = 0; octet < 4; ++octet) {
        if (octet > 0) {
            if (i >= n || text[i] != '.') return false;
            ++i;
        }
        if (i >= n || text[i] < '0' || text[i] > '9') return false;
        // A leading zero is refused rather than accepted-and-decimal. "010" is octal to some
        // tools and decimal to others; a config that means different things to different
        // readers is exactly what this module exists to prevent.
        const bool leadingZero = text[i] == '0';
        uint32_t   value       = 0;
        size_t     digits      = 0;
        while (i < n && text[i] >= '0' && text[i] <= '9') {
            value = value * 10 + static_cast<uint32_t>(text[i] - '0');
            if (value > 255) return false;
            ++i;
            ++digits;
        }
        if (digits > 1 && leadingZero) return false;
        addr = (addr << 8) | value;
    }
    if (i != n) return false;  // trailing text, including a trailing dot or a port
    out = addr;
    return true;
}

std::string formatIpv4(uint32_t addr) {
    std::string out;
    out.reserve(15);
    for (int shift = 24; shift >= 0; shift -= 8) {
        if (shift != 24) out.push_back('.');
        out += std::to_string((addr >> shift) & 0xFFu);
    }
    return out;
}

bool isContiguousMask(uint32_t mask) {
    // Ones then zeros: the complement plus one must be a power of two. 0.0.0.0 is not a usable
    // mask here (it would put every address in one subnet and defeat every check below), and
    // 255.255.255.255 leaves no host addresses at all.
    if (mask == 0 || mask == 0xFFFFFFFFu) return false;
    const uint32_t inverted = ~mask;
    return (inverted & (inverted + 1)) == 0;
}

uint8_t maskPrefixLength(uint32_t mask) {
    uint8_t bits = 0;
    while (mask & 0x80000000u) {
        ++bits;
        mask <<= 1;
    }
    return bits;
}

bool sameSubnet(uint32_t a, uint32_t b, uint32_t mask) { return (a & mask) == (b & mask); }

bool isNetworkAddress(uint32_t addr, uint32_t mask) { return (addr & ~mask) == 0; }

bool isBroadcastAddress(uint32_t addr, uint32_t mask) { return (addr & ~mask) == (~mask); }

bool looksLikeIpLiteral(const std::string& text) {
    uint32_t ignored = 0;
    return parseIpv4(text, ignored);
}

}  // namespace heliograph::net
