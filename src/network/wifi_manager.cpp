// SPDX-License-Identifier: MIT

#include "wifi_manager.h"

#if defined(ESP32)

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_mac.h>

#include "diagnostics/logger.h"

namespace heliograph {

WifiManager::WifiManager(ProvisioningPolicy policy) : policy_(policy) {}

namespace {

/// Reads the factory MAC from efuse.
///
/// NOT WiFi.macAddress(): that returns 00:00:00:00:00:00 until the WiFi driver is up, and
/// this is called during begin() before any mode is set. The result feeds the setup SSID, the
/// MQTT topic root and the Home Assistant device identifier -- all of which must be stable
/// from the first line of setup() and unique per board. esp_read_mac() reads efuse directly
/// and works before WiFi exists.
void readFactoryMac(uint8_t mac[6]) {
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        for (int i = 0; i < 6; ++i) {
            mac[i] = 0;
        }
    }
}

}  // namespace

std::string WifiManager::bridgeId() const {
    static const std::string id = [] {
        uint8_t mac[6] = {};
        readFactoryMac(mac);
        char buf[32];
        snprintf(buf, sizeof(buf), "heliograph-%02x%02x%02x", mac[3], mac[4], mac[5]);
        return std::string(buf);
    }();
    return id;
}

bool WifiManager::connected() const { return WiFi.status() == WL_CONNECTED; }

std::string WifiManager::ipAddress() const {
    if (portalActive_ && !connected()) {
        return WiFi.softAPIP().toString().c_str();
    }
    return WiFi.localIP().toString().c_str();
}

int16_t WifiManager::rssi() const { return connected() ? static_cast<int16_t>(WiFi.RSSI()) : 0; }

void WifiManager::startPortal() {
    if (portalActive_) {
        return;
    }
    uint8_t mac[6] = {};
    readFactoryMac(mac);
    apSsid_ = setupApSsid(mac);

    // Open network on purpose. A generated password would have to be printed somewhere, and
    // this AP exists only to receive credentials on a network the user is standing next to.
    // The window is small and the alternative (a password nobody can read) is worse.
    // Documented in docs/security.md.
    WiFi.mode(config_.provisioned() ? WIFI_AP_STA : WIFI_AP);
    fireStackReadyHook();  // mode() brought the tcpip thread up; see the header
    WiFi.softAP(apSsid_.c_str());

    // Captive portal: answer every DNS query with our own address. The phone's
    // connectivity probe (generate_204 / hotspot-detect.html) then reaches our web server,
    // which redirects unknown paths to the setup page while the portal is up -- so joining
    // the AP pops the setup page automatically instead of leaving the user to guess an IP.
    auto* dns = new DNSServer();
    dns->start(53, "*", WiFi.softAPIP());
    dnsServer_    = dns;
    portalActive_ = true;
}

void WifiManager::fireStackReadyHook() {
    if (stackReadyFired_ || !stackReadyHook_) {
        return;
    }
    stackReadyFired_ = true;
    stackReadyHook_();
}

void WifiManager::stopPortal() {
    if (!portalActive_) {
        return;
    }
    if (dnsServer_ != nullptr) {
        auto* dns = static_cast<DNSServer*>(dnsServer_);
        dns->stop();
        delete dns;
        dnsServer_ = nullptr;
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    portalActive_ = false;
}

void WifiManager::startMdns() {
    // Registers <hostname>.local. Without this, the setup page's promise -- "find it again by
    // its hostname" -- only holds on networks whose router happens to serve the DHCP name as
    // DNS; a plain LAN has no other resolver. mdns is that resolver, on the bridge itself.
    //
    // Restart on every (re)connect: a portal AP->STA switch or a reconnect rebuilds the STA
    // netif, and the responder must re-bind to it. end() first so a second begin() does not
    // trip over an already-initialised responder (mdns_init returns ESP_ERR_INVALID_STATE).
    MDNS.end();
    if (!MDNS.begin(config_.wifi.hostname.c_str())) {
        log::warn("mdns: responder failed to start; %s.local will not resolve",
                  config_.wifi.hostname.c_str());
        return;
    }
    // Advertise the web UI (port 80, the RestApi default) so avahi/Home Assistant discovery can
    // list the bridge, not merely resolve its name.
    MDNS.addService("http", "tcp", 80);
    log::info("mdns: %s.local", config_.wifi.hostname.c_str());
}

void WifiManager::attemptConnect() {
    ++attempt_;
    attemptInFlight_  = true;
    attemptStartedMs_ = 0;  // set by loop() on the next tick
    WiFi.begin(config_.wifi.ssid.c_str(), config_.wifi.password.c_str());
}

void WifiManager::begin(const Configuration& config) {
    config_ = config;
    WiFi.persistent(false);  // we own the credentials; the SDK must not cache its own copy
    WiFi.setAutoReconnect(false);  // reconnection is this class's job, with bounded back-off

    // All channels, strongest AP. The default WIFI_FAST_SCAN associates with the FIRST BSSID
    // that matches the SSID, in channel-scan order -- on a network with several APs that
    // regularly means a far, weak one, and the device then clings to it until the link dies
    // (the ESP32 never roams while associated). Seen live when the bridge moved rooms.
    // Applies to every WiFi.begin(), so reconnects also land on the strongest AP.
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

    // Before any WiFi.mode() call, or it does nothing. setHostname only writes a buffer; the
    // framework stamps that buffer onto the STA netif at the moment STA is enabled
    // (WiFiGeneric.cpp, mode(): esp_netif_set_hostname on the STA->on transition). Setting it
    // after mode() left the DHCP request carrying the default esp32s3-XXXXXX name.
    WiFi.setHostname(config_.wifi.hostname.c_str());

    if (!config_.provisioned()) {
        state_ = ProvisioningState::NeedsProvisioning;
        startPortal();
        return;
    }
    WiFi.mode(WIFI_STA);
    // The mode() call above initialised the network stack; DHCP has not run yet -- the
    // one safe moment for pre-lease lwip configuration (see setNetworkStackReadyHook).
    fireStackReadyHook();
    state_ = ProvisioningState::Connecting;
    attemptConnect();
}

void WifiManager::loop(uint64_t nowMs) {
    if (dnsServer_ != nullptr) {
        static_cast<DNSServer*>(dnsServer_)->processNextRequest();
    }
    const bool isConnected = connected();

    if (isConnected) {
        if (!wasConnected_) {
            // Just (re)connected.
            consecutiveFailures_ = 0;
            attempt_             = 0;
            attemptInFlight_     = false;
            stopPortal();
            startMdns();  // needs the STA netif up with an IP, which is exactly now
            wasConnected_ = true;
        }
        state_ = ProvisioningState::Connected;
        return;
    }

    if (wasConnected_) {
        // Dropped. Count it and start over; the router may simply have rebooted.
        wasConnected_ = false;
        if (diagnostics_ != nullptr) {
            diagnostics_->recordWifiReconnect();
        }
        attemptInFlight_ = false;
        nextAttemptMs_   = nowMs;
    }

    if (!config_.provisioned()) {
        state_ = ProvisioningState::NeedsProvisioning;
        startPortal();
        return;
    }

    if (attemptInFlight_) {
        if (attemptStartedMs_ == 0) {
            attemptStartedMs_ = nowMs;
        }
        if (nowMs - attemptStartedMs_ < policy_.attemptTimeoutMs) {
            return;  // still trying
        }
        // Timed out.
        attemptInFlight_ = false;
        ++consecutiveFailures_;
        WiFi.disconnect();
        nextAttemptMs_ = nowMs + retryDelayMs(policy_, attempt_);
    }

    state_ = decideState(policy_, /*hasCredentials=*/true, /*connected=*/false,
                         consecutiveFailures_);
    if (state_ == ProvisioningState::PortalAfterFailures) {
        // The credentials are wrong or the network is gone, and without a reset button this
        // portal is the user's only way in. It goes up alongside the STA attempts rather than
        // instead of them: a router that comes back must still reconnect us on its own.
        startPortal();
    }

    if (!attemptInFlight_ && nowMs >= nextAttemptMs_ &&
        (policy_.keepRetryingWithPortalUp || !portalActive_)) {
        attemptConnect();
    }
}

}  // namespace heliograph

#else  // !ESP32

namespace heliograph {

WifiManager::WifiManager(ProvisioningPolicy policy) : policy_(policy) {}
void        WifiManager::begin(const Configuration& config) { config_ = config; }
void        WifiManager::loop(uint64_t) {}
bool        WifiManager::connected() const { return false; }
std::string WifiManager::ipAddress() const { return {}; }
int16_t     WifiManager::rssi() const { return 0; }
std::string WifiManager::bridgeId() const { return "heliograph-host"; }
void        WifiManager::startPortal() { portalActive_ = true; }
void        WifiManager::stopPortal() { portalActive_ = false; }
void        WifiManager::startMdns() {}
void        WifiManager::attemptConnect() {}
void        WifiManager::fireStackReadyHook() {}

}  // namespace heliograph

#endif
