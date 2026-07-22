// SPDX-License-Identifier: MIT
//
// When to start the setup portal, and when to stop trying.
//
// Pure policy, no WiFi calls, so the state machine is host-tested. This board has no
// confirmed reset button (the BOOT GPIO could not be established from the schematic -- see
// docs/hardware.md), which makes this logic the ONLY way out of a bad WiFi configuration
// short of reflashing. It had better be right.

#pragma once

#include <cstdint>
#include <string>

namespace heliograph {

enum class ProvisioningState : uint8_t {
    /// No credentials stored: the portal is the whole user interface.
    NeedsProvisioning,
    Connecting,
    Connected,
    /// Credentials exist but do not work. The portal is up so they can be corrected, and
    /// reconnection keeps being attempted in the background.
    PortalAfterFailures,
};

const char* provisioningStateName(ProvisioningState state);

struct ProvisioningPolicy {
    /// Consecutive failed connection attempts before the setup portal comes up.
    ///
    /// Not 1: a router rebooting, or the bridge waking before the AP does, must not drop the
    /// device off the network into a portal nobody is watching. Not 50 either: with no reset
    /// button, an unreachable device is unrecoverable without USB.
    uint32_t failuresBeforePortal = 5;

    /// How long a single connection attempt may take.
    uint32_t attemptTimeoutMs = 15000;

    /// Back-off between attempts, capped. Bounded on purpose: the AP may come back at any
    /// moment and the bridge must notice within a minute, not an hour.
    uint32_t initialRetryMs = 2000;
    uint32_t maxRetryMs     = 60000;

    /// Keep retrying the stored credentials even while the portal is up. A router that comes
    /// back should reconnect the bridge without anyone touching the portal.
    bool keepRetryingWithPortalUp = true;
};

/// Decides state from stored credentials and the failure count.
ProvisioningState decideState(const ProvisioningPolicy& policy, bool hasCredentials,
                              bool connected, uint32_t consecutiveFailures);

/// Back-off delay for attempt N (1-based). Doubles, capped at maxRetryMs.
uint32_t retryDelayMs(const ProvisioningPolicy& policy, uint32_t attempt);

/// SSID for the setup access point, e.g. "Heliograph-Setup-A1B2".
///
/// Derived from the MAC so two bridges being provisioned in one room stay distinguishable.
std::string setupApSsid(const uint8_t mac[6]);

}  // namespace heliograph
