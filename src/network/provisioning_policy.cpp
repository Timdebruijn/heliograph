// SPDX-License-Identifier: MIT

#include "provisioning_policy.h"

#include <cstdio>

namespace heliograph {

const char* provisioningStateName(ProvisioningState state) {
    switch (state) {
        case ProvisioningState::NeedsProvisioning:   return "needs_provisioning";
        case ProvisioningState::Connecting:          return "connecting";
        case ProvisioningState::Connected:           return "connected";
        case ProvisioningState::PortalAfterFailures: return "portal_after_failures";
    }
    return "unknown";
}

ProvisioningState decideState(const ProvisioningPolicy& policy, bool hasCredentials,
                              bool connected, uint32_t consecutiveFailures) {
    if (!hasCredentials) {
        return ProvisioningState::NeedsProvisioning;
    }
    if (connected) {
        // Being connected always wins, whatever the history. A device that reconnected must
        // leave the portal behind rather than sit in a half state.
        return ProvisioningState::Connected;
    }
    if (consecutiveFailures >= policy.failuresBeforePortal) {
        return ProvisioningState::PortalAfterFailures;
    }
    return ProvisioningState::Connecting;
}

uint32_t retryDelayMs(const ProvisioningPolicy& policy, uint32_t attempt) {
    if (attempt <= 1) {
        return policy.initialRetryMs;
    }
    uint64_t delay = policy.initialRetryMs;
    for (uint32_t i = 1; i < attempt && delay < policy.maxRetryMs; ++i) {
        delay *= 2;
    }
    return delay > policy.maxRetryMs ? policy.maxRetryMs : static_cast<uint32_t>(delay);
}

std::string setupApSsid(const uint8_t mac[6]) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Heliograph-Setup-%02X%02X", mac[4], mac[5]);
    return std::string(buf);
}

}  // namespace heliograph
