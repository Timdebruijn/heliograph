// SPDX-License-Identifier: MIT

#pragma once

#include <string>

namespace heliograph::mqtt {

/// Topic layout. Kept in one place so nothing builds a topic by string concatenation at the
/// call site, where a typo silently publishes into the void.
class MqttTopics {
public:
    /// `deviceKey` empty means the bridge-scoped tree: `<base>/<bridgeId>/...`. That is also
    /// what the FIRST device publishes on, unchanged from before this bridge could poll more
    /// than one -- deliberately, because moving it would change every Home Assistant entity's
    /// unique_id and cost an existing install its whole recorder history for a feature it is
    /// not using. Devices 2..N get `<base>/<bridgeId>/device/<key>/...`.
    ///
    /// The asymmetry is the price of not breaking what already works, and it is confined to
    /// this constructor: everything downstream just uses the prefix it is given.
    MqttTopics(std::string baseTopic, std::string bridgeId, std::string deviceKey = {})
        : prefix_(std::move(baseTopic) + "/" + std::move(bridgeId)) {
        if (!deviceKey.empty()) {
            prefix_ += "/device/" + std::move(deviceKey);
        }
    }

    /// Whether the BRIDGE is reachable -- not the inverter.
    ///
    /// An inverter that is off at night must not make these entities unavailable in Home
    /// Assistant: that would turn every night into a gap in the history rather than a flat
    /// line. Inverter liveness lives in the state payload as `inverter_online`.
    std::string availability() const { return prefix_ + "/availability"; }
    std::string state() const { return prefix_ + "/state"; }
    std::string diagnostics() const { return prefix_ + "/diagnostics"; }
    std::string identity() const { return prefix_ + "/identity"; }
    std::string capabilities() const { return prefix_ + "/capabilities"; }

    /// Bridge relay control (DRM contacts). These belong to the BRIDGE, not to any inverter,
    /// so they are only ever built from a bridge-scoped MqttTopics -- never from a device one.
    /// Commands come in on relaySet, acknowledged
    /// state goes out on relayState. The one wildcard subscription this firmware has.
    std::string relaySet(uint8_t index) const {
        return prefix_ + "/relay/" + std::to_string(index) + "/set";
    }
    std::string relayState(uint8_t index) const {
        return prefix_ + "/relay/" + std::to_string(index) + "/state";
    }
    std::string relaySetWildcard() const { return prefix_ + "/relay/+/set"; }

    /// DRM mode select (relay boards with configured roles). Mode name in, mode name out.
    std::string drmSet() const { return prefix_ + "/drm/set"; }
    std::string drmState() const { return prefix_ + "/drm/state"; }

    /// A write command for THIS device -- unlike the relay/DRM topics above, this is built
    /// from a per-device MqttTopics (bridge-scoped for the primary device, device-scoped for
    /// every other one), so each device gets its own topic rather than sharing the bridge's.
    /// Command in on commandSet, a one-shot accepted/rejected ack out on commandResult -- never
    /// retained, unlike relayState: there is no steady state to reflect, only an event.
    std::string commandSet() const { return prefix_ + "/command/set"; }
    std::string commandResult() const { return prefix_ + "/command/result"; }

    const std::string& prefix() const { return prefix_; }

private:
    std::string prefix_;
};

/// Which topic subtree a device publishes on, and what prefixes its Home Assistant ids.
///
/// Pure and host-testable on purpose: this pair of decisions IS the back-compat contract of
/// multi-device MQTT, and it used to live inside MqttOutput -- which is compiled only for
/// ESP32, so no test could reach it. The tests that "pinned the contract" were exercising the
/// MqttTopics constructor instead, and would have passed with this rule inverted (review).
///
/// `primary` marks the device that keeps the bridge-scoped tree it has always used. Exactly
/// one device may be primary, and none may be: when the configured first device fails to
/// start, nobody inherits its topics, its unique ids or its recorder history.
inline std::string deviceTopicKey(bool primary, const std::string& deviceId) {
    return primary ? std::string{} : deviceId;
}

inline std::string deviceUniqueBase(bool primary, const std::string& bridgeId,
                                    const std::string& deviceId) {
    return primary ? bridgeId : bridgeId + "_" + deviceId;
}

inline constexpr const char* kPayloadOnline  = "online";
inline constexpr const char* kPayloadOffline = "offline";

inline constexpr const char* kDefaultBaseTopic       = "heliograph";
inline constexpr const char* kDefaultDiscoveryPrefix = "homeassistant";

}  // namespace heliograph::mqtt
