// SPDX-License-Identifier: MIT
//
// WiFi lifecycle and the setup portal.
//
// The decisions live in provisioning_policy.* and are host-tested; this drives the radio.
//
// This board has no confirmed reset button, so the only ways back from a wrong password are:
//   1. the portal that comes up after ProvisioningPolicy::failuresBeforePortal failures;
//   2. POST /api/v1/actions/factory-reset from the web UI while still reachable;
//   3. reflashing over USB.
// That makes (1) load-bearing rather than a nicety.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "config/configuration.h"
#include "diagnostics/diagnostics.h"
#include "provisioning_policy.h"

namespace heliograph {

class WifiManager {
public:
    WifiManager(ProvisioningPolicy policy = {});

    void setDiagnostics(Diagnostics* diagnostics) { diagnostics_ = diagnostics; }

    /// Runs ONCE, right after the first WiFi.mode() call has brought up the network stack
    /// (lwip/tcpip thread) and strictly before any DHCP lease can arrive. This is the only
    /// safe window for lwip calls that (a) must precede the lease and (b) are dispatched via
    /// tcpip_callback(), which aborts when the tcpip thread does not exist yet -- calling
    /// esp_sntp_servermode_dhcp() from setup() before WiFi did exactly that and bricked the
    /// boot (0.4.4, 2026-07-21).
    void setNetworkStackReadyHook(std::function<void()> hook) { stackReadyHook_ = std::move(hook); }

    /// Starts connecting, or brings the portal up when there are no credentials.
    void begin(const Configuration& config);

    /// Non-blocking. Drives retries and portal transitions. Call from loopTask.
    void loop(uint64_t nowMs);

    ProvisioningState state() const { return state_; }
    bool              connected() const;
    bool              portalActive() const { return portalActive_; }
    uint32_t          consecutiveFailures() const { return consecutiveFailures_; }
    std::string       ipAddress() const;
    std::string       apSsid() const { return apSsid_; }
    int16_t           rssi() const;

    /// Bridge id from the MAC: "heliograph-a1b2c3". Stable across reboots, because it is the
    /// Home Assistant device identifier and the MQTT topic root.
    std::string bridgeId() const;

private:
    void startPortal();
    void stopPortal();
    void startMdns();
    void attemptConnect();
    void fireStackReadyHook();
    /// Captive-portal DNS: while the setup AP is up, every name resolves to us, so the
    /// phone's connectivity probe lands on our web server and the OS pops the setup page
    /// by itself. Started/stopped with the portal; pumped from loop(). ESP32-only (the
    /// host build has no portal); implemented via an opaque pointer to keep DNSServer out
    /// of this platform-neutral header.
    void* dnsServer_ = nullptr;

    std::function<void()> stackReadyHook_;
    bool                  stackReadyFired_ = false;

    ProvisioningPolicy policy_;
    Configuration      config_;
    Diagnostics*       diagnostics_ = nullptr;

    ProvisioningState state_               = ProvisioningState::NeedsProvisioning;
    bool              portalActive_        = false;
    uint32_t          consecutiveFailures_ = 0;
    uint32_t          attempt_             = 0;
    uint64_t          nextAttemptMs_       = 0;
    uint64_t          attemptStartedMs_    = 0;
    bool              attemptInFlight_     = false;
    bool              wasConnected_        = false;
    std::string       apSsid_;
};

}  // namespace heliograph
