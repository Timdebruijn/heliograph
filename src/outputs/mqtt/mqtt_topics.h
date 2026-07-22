// SPDX-License-Identifier: MIT

#pragma once

#include <string>

namespace heliograph::mqtt {

/// Topic layout. Kept in one place so nothing builds a topic by string concatenation at the
/// call site, where a typo silently publishes into the void.
class MqttTopics {
public:
    MqttTopics(std::string baseTopic, std::string bridgeId)
        : prefix_(std::move(baseTopic) + "/" + std::move(bridgeId)) {}

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

    const std::string& prefix() const { return prefix_; }

private:
    std::string prefix_;
};

inline constexpr const char* kPayloadOnline  = "online";
inline constexpr const char* kPayloadOffline = "offline";

inline constexpr const char* kDefaultBaseTopic       = "heliograph";
inline constexpr const char* kDefaultDiscoveryPrefix = "homeassistant";

}  // namespace heliograph::mqtt
