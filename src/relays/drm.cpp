// SPDX-License-Identifier: MIT

#include "drm.h"

namespace heliograph::drm {

bool isValidRole(const std::string& role) {
    if (role == "none") {
        return true;
    }
    if (role.size() != 4 || role.rfind("drm", 0) != 0) {
        return false;
    }
    return role[3] >= '0' && role[3] <= '8';
}

std::vector<std::string> optionsFor(const std::vector<std::string>& roles) {
    std::vector<std::string> options;
    for (const auto& role : roles) {
        if (role == "none" || !isValidRole(role)) {
            continue;
        }
        bool seen = false;
        for (const auto& existing : options) {
            if (existing == role) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            options.push_back(role);
        }
    }
    if (!options.empty()) {
        options.insert(options.begin(), kModeNormal);
    }
    return options;
}

bool patternFor(const std::vector<std::string>& roles, const std::string& mode,
                std::vector<bool>& outPattern) {
    outPattern.assign(roles.size(), false);
    if (mode == kModeNormal) {
        // Valid only when a select exists at all; "normal" on a role-less board is
        // meaningless and refused rather than silently accepted.
        return !optionsFor(roles).empty();
    }
    if (!isValidRole(mode) || mode == "none") {
        return false;
    }
    bool found = false;
    for (size_t i = 0; i < roles.size(); ++i) {
        if (roles[i] == mode) {
            outPattern[i] = true;
            found         = true;
        }
    }
    return found;
}

std::string modeFrom(const std::vector<std::string>& roles, uint8_t mask) {
    if (mask == 0) {
        return kModeNormal;
    }
    // Exactly one role's relays energised, and nothing outside that role?
    for (const auto& option : optionsFor(roles)) {
        if (option == kModeNormal) {
            continue;
        }
        uint8_t expected = 0;
        for (size_t i = 0; i < roles.size() && i < 8; ++i) {
            if (roles[i] == option) {
                expected |= static_cast<uint8_t>(1u << i);
            }
        }
        if (mask == expected) {
            return option;
        }
    }
    return kModeCustom;
}

}  // namespace heliograph::drm
