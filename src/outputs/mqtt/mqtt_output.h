// SPDX-License-Identifier: MIT
//
// MQTT output, backed by espMqttClient (MIT, non-blocking, LWT, QoS 0/1/2, auto-reconnect).
//
// Everything worth testing -- payloads, discovery, throttling -- lives in the pure units next
// to this file and is covered by test_mqtt. This is the wiring: connection lifecycle,
// reconnect back-off, and calling those units.
//
// MQTT is optional and failure here is contained: if the broker is gone, polling, Modbus TCP
// and REST carry on untouched. That is an acceptance criterion, not a nicety.
//
// VERIFIED ON HARDWARE 2026-07-17 against Mosquitto 7.1.0: connect with credentials, LWT,
// discovery, state publishing and the reconnect back-off. Two seam bugs were only found by
// running: connect() reporting the *attempt* as success (back-off never armed, ~3.5
// attempts/s), and the pointer-lifetime bug at clientId_ below (client id read from freed
// heap; broke only after a config change reshuffled the heap).

#pragma once

#include <cstdint>
#include <string>

#include <atomic>
#include <functional>

#include "device/bridge_info.h"
#include "device/command.h"
#include "device/device_state.h"
#include "diagnostics/diagnostics.h"
#include "mqtt_topics.h"
#include "publish_policy.h"

namespace heliograph::mqtt {

struct MqttConfig {
    bool        enabled  = false;  ///< off until a broker is configured
    std::string host;
    uint16_t    port     = 1883;
    std::string username;          ///< optional
    std::string password;          ///< optional; never logged, never published
    std::string baseTopic       = kDefaultBaseTopic;
    std::string discoveryPrefix = kDefaultDiscoveryPrefix;
    bool        discoveryEnabled = true;
    uint8_t     qos              = 0;
    /// How often diagnostics go out. Slower than state: nobody needs heap size at 10 s.
    uint32_t diagnosticsIntervalMs = 60000;
};

class MqttOutput {
public:
    MqttOutput(MqttConfig config, PublishPolicy publishPolicy = {});

    /// Sets up the client and starts connecting. Non-blocking.
    bool begin(const BridgeInfo& bridge);

    /// Drives reconnects and publishing. Call regularly from the MQTT task; never blocks.
    void loop(const DeviceState& state, const BridgeInfo& bridge,
              const DiagnosticsSnapshot& diagnostics, uint64_t nowMs);

    void stop();
    bool connected() const;

    void setDiagnostics(Diagnostics* diagnostics) { diagnostics_ = diagnostics; }

    /// Handles a relay command arriving on <prefix>/relay/<n>/set. Wired by main to the
    /// RelayController behind a mutex: this callback runs on the MQTT task while REST
    /// commands arrive on the AsyncTCP task. The switch state in Home Assistant follows
    /// the ACK on the state topic, so a refused command visibly snaps back.
    using RelayCommandFn = std::function<CommandResult(uint8_t index, bool energised)>;
    void setRelayCommandHandler(RelayCommandFn handler) { relayCommand_ = std::move(handler); }

    /// Handles a DRM mode command from <prefix>/drm/set. Same task/locking rules as the
    /// relay handler; returns false for a mode that is not an option.
    using DrmCommandFn = std::function<bool(const std::string& mode)>;
    void setDrmCommandHandler(DrmCommandFn handler) { drmCommand_ = std::move(handler); }

private:
    void onConnected(const DeviceState& state, const BridgeInfo& bridge);
    void publishDiscovery(const DeviceState& state, const BridgeInfo& bridge);

    MqttConfig      config_;
    MqttTopics      topics_;
    PublishThrottle throttle_;
    Diagnostics*    diagnostics_ = nullptr;

    // espMqttClient's setClientId/setWill/setServer/setCredentials store the POINTER, not a
    // copy (MqttClientSetup.h: `_clientId = clientId;`). Every string handed to them must
    // therefore live as long as this object. Passing bridge.bridgeId.c_str() straight through
    // worked only while the freed temporary happened to keep its bytes; a config change that
    // reshuffled the heap turned the client id into garbage and the broker refused it with
    // "client identifier not valid".
    std::string clientId_;
    std::string willTopic_;

    bool     started_            = false;
    bool     discoveryPublished_ = false;
    /// discoverySignature() at the last discovery publish. Discovery re-announces when the
    /// model changes: the first MQTT connect often beats the first successful poll of a real
    /// inverter. A signature rather than a count, so a same-size swap of channels is caught.
    std::string discoveredSignature_;
    uint64_t lastDiagnosticsMs_  = 0;
    uint64_t nextReconnectMs_    = 0;
    /// Exponential back-off, capped. An unreachable broker must not turn into a busy loop.
    uint32_t reconnectDelayMs_ = 1000;
    /// espMqttClientTypes::DisconnectReason of the last drop, for diagnostics.
    uint8_t lastDisconnectReason_ = 0;
    /// Edge detection for the reconnect counter. `wasConnected_` tracks the previous loop's
    /// link state so a false→true transition is counted once; `everConnected_` makes the
    /// FIRST connect at boot not count as a reconnect. See loop().
    bool wasConnected_  = false;
    bool everConnected_ = false;

    RelayCommandFn relayCommand_;
    DrmCommandFn   drmCommand_;
    uint8_t        relayCount_ = 0;  ///< copied at begin() for topic parsing in the callback
    /// Set by onMessage (MQTT task) on EVERY received relay command, consumed by loop().
    /// Without it a refused or no-op command changes no state, nothing gets published, and
    /// the Home Assistant switch stays stuck "switching" instead of snapping back
    /// (Copilot review on PR #2). Atomic: two tasks touch it.
    std::atomic<bool> relayAckRequested_{false};
    /// Relay ack-state tracking: force a publish on connect and on every mask/enabled
    /// change. `lastRelaysEnabled_` also triggers a discovery re-announce, because
    /// enabling/disabling adds or removes the switch entities themselves.
    bool    relayStateForced_  = true;
    uint8_t lastRelayMask_     = 0;
    bool    lastRelaysEnabled_ = false;
    /// Signature of the configured roles at the last publish. Roles rename the switches,
    /// rebuild the select options AND change the derived mode, so a role change must
    /// re-announce discovery and re-ack state -- "applied immediately" would otherwise
    /// only be true after the next reconnect (self-review of PR #3).
    std::string lastRelayRolesSig_;

public:
    uint8_t lastDisconnectReason() const { return lastDisconnectReason_; }
};

inline constexpr uint32_t kMaxReconnectDelayMs = 60000;

}  // namespace heliograph::mqtt
