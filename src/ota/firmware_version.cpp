// SPDX-License-Identifier: MIT

#include "firmware_version.h"

#include <cctype>

namespace heliograph::ota {
namespace {

/// Reads decimal digits at `pos`, bounded so a long run of digits cannot wrap a uint16_t into
/// a small number -- which would silently make 65536 compare as 0 and offer a downgrade.
bool readNumber(const std::string& text, size_t& pos, uint16_t& out) {
    const size_t start = pos;
    uint32_t     value = 0;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
        value = value * 10 + static_cast<uint32_t>(text[pos] - '0');
        if (value > 65535) {
            return false;
        }
        ++pos;
    }
    if (pos == start) {
        return false;  // no digits at all
    }
    out = static_cast<uint16_t>(value);
    return true;
}

}  // namespace

bool parseSemVer(const std::string& text, SemVer& out) {
    size_t pos = 0;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    if (pos < text.size() && (text[pos] == 'v' || text[pos] == 'V')) {
        ++pos;  // release tags are written v0.14.0
    }
    SemVer parsed;
    if (!readNumber(text, pos, parsed.major)) return false;
    if (pos >= text.size() || text[pos] != '.') return false;
    ++pos;
    if (!readNumber(text, pos, parsed.minor)) return false;
    if (pos >= text.size() || text[pos] != '.') return false;
    ++pos;
    if (!readNumber(text, pos, parsed.patch)) return false;

    // Whatever follows is ignored -- "0.14.0 (Jul 26 2026 17:31:45)" and "0.14.0-rc1" are both
    // this version -- EXCEPT a fourth numeric component. "1.2.3.4" is a versioning scheme we do
    // not understand, and silently reading it as 1.2.3 would make 1.2.3.4 and 1.2.3.5 compare
    // equal: an update that exists and is never offered.
    //
    // Testing for a digit here rather than a '.' would be dead code: readNumber consumes every
    // consecutive digit, so the character after patch is never one.
    if (pos + 1 < text.size() && text[pos] == '.' &&
        std::isdigit(static_cast<unsigned char>(text[pos + 1]))) {
        return false;
    }
    out = parsed;
    return true;
}

bool isNewer(const std::string& current, const std::string& candidate) {
    SemVer a;
    SemVer b;
    if (!parseSemVer(current, a) || !parseSemVer(candidate, b)) {
        return false;
    }
    return a < b;
}

}  // namespace heliograph::ota
