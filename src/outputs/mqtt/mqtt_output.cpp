// SPDX-License-Identifier: MIT
//
// espMqttClient wiring. API read from the library sources (src/MqttClient.h,
// src/MqttClientSetup.h, src/espMqttClient.h, examples/simple-esp32), not from memory.

#include "mqtt_output.h"

#include "home_assistant_discovery.h"
#include "mqtt_payloads.h"
#include "outputs/json_util.h"
#include "relays/drm.h"

#include <cstring>

#include "diagnostics/logger.h"

#if defined(ESP32)

#include <espMqttClient.h>

namespace heliograph::mqtt {
namespace {

// One client per process; espMqttClient owns its own task.
espMqttClient g_client;

}  // namespace

MqttOutput::MqttOutput(MqttConfig config, PublishPolicy publishPolicy)
    : config_(std::move(config)),
      topics_(config_.baseTopic, "pending"),
      publishPolicy_(publishPolicy) {}

bool MqttOutput::begin(const BridgeInfo& bridge) {
    if (!config_.enabled || config_.host.empty()) {
        return false;
    }
    topics_ = MqttTopics(config_.baseTopic, bridge.bridgeId);

    // Copied into members first: the library stores pointers, and `bridge` may be a temporary
    // (main.cpp passes bridgeInfo() by value). See the note at clientId_ in the header.
    clientId_  = bridge.bridgeId;
    willTopic_ = topics_.availability();

    g_client.setServer(config_.host.c_str(), config_.port);
    g_client.setClientId(clientId_.c_str());
    // Authenticate whenever either field is set. Gating only on username meant a config with a
    // password but no username (nothing forbids it) silently connected anonymously instead.
    if (!config_.username.empty() || !config_.password.empty()) {
        g_client.setCredentials(config_.username.c_str(), config_.password.c_str());
    }

    // Last Will: if the bridge drops off ungracefully the broker announces it for us.
    // Retained and QoS 1 -- a subscriber connecting later must still learn we are gone.
    g_client.setWill(willTopic_.c_str(), 1, true, kPayloadOffline);

    g_client.onDisconnect([this](espMqttClientTypes::DisconnectReason reason) {
        // Runs on the library's 'mqttclient' task while loop() runs on the caller's task, so
        // only atomics and the mutex-guarded diagnostics may be touched here. The actual
        // resync (throttle reset + discovery republish) happens in loop(), which owns that
        // state -- same pattern as relayAckRequested_.
        resyncRequested_ = true;
        // NOTE: the reconnect counter is NOT bumped here. espMqttClient fires onDisconnect
        // on every failed connect *attempt* too, so counting here inflated the total with
        // each back-off retry during an outage and, worse, never ticked on the actual
        // successful reconnection -- a "reconnect_total" that meant the opposite of its name
        // (live MQTT-outage test, 2026-07-22). The count is now taken on the connected edge
        // in loop(). We still surface WHY here, which stays useful.
        if (diagnostics_ != nullptr) {
            // Without this the only symptom is mqtt_connected staying false, which tells a
            // user nothing about whether the broker refused the credentials, rejected the
            // client id, or simply is not there. The library's reason strings carry no secrets.
            diagnostics_->setLastError(
                std::string("mqtt disconnected: ") +
                espMqttClientTypes::disconnectReasonToString(reason));
        }
    });

    relayCount_ = bridge.relayCount;
    if (relayCount_ > 0) {
        // The one message handler this firmware has. Topic and payload are attacker-
        // influenced data from anyone who can publish on the broker; parse defensively and
        // let the RelayController's gates decide. Runs on the MQTT task -- the handler
        // installed by main serialises against REST with a mutex.
        g_client.onMessage([this](const espMqttClientTypes::MessageProperties&,
                                  const char* topic, const uint8_t* payload, size_t len,
                                  size_t index, size_t total) {
            if (topic == nullptr || index != 0 || len != total) {
                return;  // fragmented messages are never valid for these short payloads
            }
            const std::string t(topic);
            if (t == topics_.drmSet()) {
                if (drmCommand_ == nullptr || len == 0 || len > 16) {
                    return;  // mode names are short; anything longer is not one
                }
                std::string mode(reinterpret_cast<const char*>(payload), len);
                drmCommand_(mode);
                relayAckRequested_ = true;  // ack states + mode, accepted or not
                return;
            }
            const std::string prefix = topics_.prefix() + "/relay/";
            if (relayCommand_ == nullptr || t.rfind(prefix, 0) != 0 ||
                t.size() <= prefix.size()) {
                return;
            }
            const size_t slash = t.find('/', prefix.size());
            if (slash == std::string::npos || t.substr(slash) != "/set") {
                return;
            }
            const std::string num = t.substr(prefix.size(), slash - prefix.size());
            if (num.empty() || num.size() > 2 ||
                num.find_first_not_of("0123456789") != std::string::npos) {
                return;
            }
            const int idx = std::stoi(num);
            if (idx < 0 || idx >= relayCount_) {
                return;
            }
            char body[8] = {};
            memcpy(body, payload, len < sizeof(body) - 1 ? len : sizeof(body) - 1);
            bool on;
            if (strcmp(body, "ON") == 0 || strcmp(body, "1") == 0) {
                on = true;
            } else if (strcmp(body, "OFF") == 0 || strcmp(body, "0") == 0) {
                on = false;
            } else {
                return;  // unknown payload: ignore rather than guess
            }
            // Outcome is not published from here (wrong task); the flag makes loop() ack
            // the real state on its next tick REGARDLESS of whether the state changed --
            // a refused or no-op command otherwise changes no mask bit, nothing would be
            // published, and the HA switch would hang in "switching" instead of snapping
            // back.
            relayCommand_(static_cast<uint8_t>(idx), on);
            relayAckRequested_ = true;
        });
    }

    started_          = true;
    nextReconnectMs_  = 0;
    reconnectDelayMs_ = 1000;
    return true;
}

bool MqttOutput::connected() const { return started_ && g_client.connected(); }

MqttOutput::Channel& MqttOutput::channelFor(const DeviceView& view, const BridgeInfo& bridge) {
    for (auto& c : channels_) {
        if (c.id == view.id) {
            return c;
        }
    }
    // The primary device keeps the bridge-scoped tree every install had before this became
    // plural: same topics, same unique ids, same Home Assistant entities and history. Which
    // device that is comes from the caller, not from arrival order -- see DeviceView::primary.
    Channel c{view.id,
              MqttTopics(config_.baseTopic, bridge.bridgeId,
                         deviceTopicKey(view.primary, view.id)),
              deviceUniqueBase(view.primary, bridge.bridgeId, view.id),
              PublishThrottle(publishPolicy_),
              false,
              0};
    channels_.push_back(std::move(c));
    return channels_.back();
}

void MqttOutput::publishDiscovery(Channel& channel, const DeviceState& state,
                                  const BridgeInfo& bridge) {
    if (!config_.discoveryEnabled) {
        return;
    }
    // Purely derived from measurements and capabilities. Nothing in here knows which device
    // it is talking to; a driver reporting a battery gets battery entities for free.
    // topics_ for availability, deliberately: it is the BRIDGE's liveness, and it is the only
    // availability topic anything publishes.
    for (const auto& e : buildDiscoveryEntities(state, bridge, channel.topics,
                                                topics_.availability(), config_.discoveryPrefix,
                                                channel.uniqueBase)) {
        g_client.publish(e.configTopic.c_str(), 1, true, e.payload.c_str());
    }
    // Bridge entities belong to the bridge and are announced once -- publishing them per
    // device would create N copies of the same RSSI sensor. Tied to the bridge-scoped channel
    // rather than to whichever channel happens to be first in the vector.
    if (channel.uniqueBase == bridge.bridgeId) {
        for (const auto& e :
             buildBridgeDiagnosticEntities(bridge, topics_, config_.discoveryPrefix)) {
            g_client.publish(e.configTopic.c_str(), 1, true, e.payload.c_str());
        }
        // Relay switches -- or, when the feature is disabled on a relay board, empty retained
        // payloads that remove previously announced switches from Home Assistant.
        for (const auto& e : buildRelayEntities(bridge, topics_, config_.discoveryPrefix)) {
            g_client.publish(e.configTopic.c_str(), 1, true, e.payload.c_str());
        }
    }
    channel.discoveryPublished = true;
}

void MqttOutput::onConnected(Channel& channel, const DeviceState& state,
                             const BridgeInfo& bridge) {
    g_client.publish(topics_.availability().c_str(), 1, true, kPayloadOnline);

    std::string payload;
    if (buildIdentityPayload(state.identity, payload)) {
        g_client.publish(channel.topics.identity().c_str(), 1, true, payload.c_str());
    }
    if (json_util::buildCapabilitiesPayload(state.capabilities, payload, kMaxPayloadBytes)) {
        g_client.publish(channel.topics.capabilities().c_str(), 1, true, payload.c_str());
    }
    publishDiscovery(channel, state, bridge);

    // The relay command topic is the only subscription; inverter drivers stay read-only
    // and get no command topic -- that follows from the capabilities, not a decision here.
    if (relayCount_ > 0) {
        g_client.subscribe(topics_.relaySetWildcard().c_str(), 1);
        g_client.subscribe(topics_.drmSet().c_str(), 1);
        relayStateForced_ = true;  // fresh session: ack the current states once
    }
}

void MqttOutput::loop(const std::vector<DeviceView>& devices, const BridgeInfo& bridge,
                      const DiagnosticsSnapshot& diagnostics, uint64_t nowMs) {
    if (!started_) {
        return;
    }

    if (!g_client.connected()) {
        wasConnected_ = false;
        if (nowMs < nextReconnectMs_) {
            return;
        }
        // connect() is NON-BLOCKING: it returns true when the attempt was *started*, not when
        // it succeeded. Treating that as success meant the back-off never armed and this
        // loop hammered the broker at the caller's tick rate -- measured at ~3.5 attempts per
        // second against a real broker. Always arm the back-off; only a genuine connection
        // clears it.
        g_client.connect();
        nextReconnectMs_  = nowMs + reconnectDelayMs_;
        reconnectDelayMs_ = reconnectDelayMs_ * 2 > kMaxReconnectDelayMs
                                ? kMaxReconnectDelayMs
                                : reconnectDelayMs_ * 2;
        return;
    }

    // Connected: reset the back-off so a later blip starts from 1 s again.
    reconnectDelayMs_ = 1000;

    // Count the disconnected→connected edge, once. The very first connect after boot is not
    // a reconnect, so it is skipped; every later re-establishment (what the user watches
    // during an outage) increments mqtt_reconnect_total by exactly one.
    if (!wasConnected_) {
        wasConnected_ = true;
        if (everConnected_ && diagnostics_ != nullptr) {
            diagnostics_->recordMqttReconnect();
        }
        everConnected_ = true;
    }

    // Retained messages may not have survived the broker outage; force a full republish.
    // Consumed here rather than done in onDisconnect: nothing publishes while disconnected
    // anyway, and this way throttle_ and discoveryPublished_ are only ever touched on the
    // task that runs loop().
    if (resyncRequested_.exchange(false)) {
        for (auto& c : channels_) {
            c.throttle.reset();
            c.discoveryPublished = false;
        }
    }

    // Per device: its own discovery announcement, its own throttle. Re-announce when the
    // measurement model has grown since the last publish -- discovery is derived from
    // measurements, and against a real inverter the first MQTT connect regularly wins the race
    // against the first successful poll. Publishing once and never again left Home Assistant
    // with only the status entities (observed live, 2026-07-19). Retained config topics make
    // republishing idempotent.
    for (const auto& view : devices) {
        if (view.state == nullptr) {
            continue;
        }
        const DeviceState& state   = *view.state;
        Channel&           channel = channelFor(view, bridge);

        const auto signature = discoverySignature(state);
        if (!channel.discoveryPublished || signature != channel.discoveredSignature) {
            onConnected(channel, state, bridge);
            channel.discoveredSignature = signature;
        }

        if (channel.throttle.shouldPublish(state, nowMs)) {
            std::string payload;
            if (buildStatePayload(state, payload)) {
                g_client.publish(channel.topics.state().c_str(), config_.qos, true,
                                 payload.c_str());
                channel.throttle.recordPublished(state, nowMs);
            }
            // If the payload did not fit it is dropped rather than truncated, and the throttle
            // is left untouched so the next attempt retries.
        }
    }

    if (bridge.relayCount > 0) {
        // Enabling/disabling the feature adds or removes the switch entities themselves,
        // and a ROLE change renames switches, rebuilds the select options and changes the
        // derived mode -- both force a discovery re-announce; a mask change just acks.
        const uint64_t rolesSig = stringListFingerprint(bridge.relayRoles);
        if (bridge.relaysEnabled != lastRelaysEnabled_ || rolesSig != lastRelayRolesSig_) {
            lastRelaysEnabled_  = bridge.relaysEnabled;
            lastRelayRolesSig_  = rolesSig;
            // Relay entities ride along with the bridge-scoped channel's announcement.
            for (auto& c : channels_) {
                if (c.uniqueBase == bridge.bridgeId) {
                    c.discoveryPublished = false;
                }
            }
            relayStateForced_ = true;
        }
        if (relayStateForced_ || relayAckRequested_.exchange(false) ||
            bridge.relayMask != lastRelayMask_) {
            for (uint8_t i = 0; i < bridge.relayCount; ++i) {
                const bool on = (bridge.relayMask >> i) & 1;
                g_client.publish(topics_.relayState(i).c_str(), 1, true,
                                 on ? "ON" : "OFF");
            }
            // The DRM mode is derived state over the same mask; ack it in the same breath
            // so the HA select and the switches can never disagree for long.
            std::vector<std::string> roles = bridge.relayRoles;
            roles.resize(bridge.relayCount, "none");
            if (!drm::optionsFor(roles).empty()) {
                const std::string mode = drm::modeFrom(roles, bridge.relayMask);
                g_client.publish(topics_.drmState().c_str(), 1, true, mode.c_str());
            }
            lastRelayMask_    = bridge.relayMask;
            relayStateForced_ = false;
        }
    }

    if (nowMs - lastDiagnosticsMs_ >= config_.diagnosticsIntervalMs) {
        std::string payload;
        if (buildDiagnosticsPayload(diagnostics, bridge, payload)) {
            g_client.publish(topics_.diagnostics().c_str(), 0, true, payload.c_str());
        }
        lastDiagnosticsMs_ = nowMs;
    }
}

bool MqttOutput::forgetDevice(const DeviceId& id, const BridgeInfo& bridge) {
    if (!started_ || !g_client.connected()) {
        return false;
    }
    // Always the device-scoped tree; see the header for why the bridge-scoped one is off limits.
    const MqttTopics  topics(config_.baseTopic, bridge.bridgeId, deviceTopicKey(false, id));
    const std::string base = deviceUniqueBase(false, bridge.bridgeId, id);

    // Every publish is checked. A packet id of 0 means the client refused it -- the link went
    // down between the connected() check and here, or the outbox could not allocate -- and a
    // clear that never left must not be recorded as done (review, 2026-07-26).
    bool ok = true;
    const auto clear = [&](const std::string& topic) {
        ok = (g_client.publish(topic.c_str(), 1, true, "") != 0) && ok;
    };

    // The device's own retained payloads. Empty, retained: a subscriber that connects later
    // must not be handed a reading from an inverter that is no longer configured.
    for (const auto& topic : {topics.state(), topics.identity(), topics.capabilities()}) {
        clear(topic);
    }

    // Every discovery config it could have announced. Enumerated rather than derived from the
    // measurement set, which is gone along with the device. Publishing an empty retained
    // payload to a topic that was never used is harmless -- Home Assistant has nothing to
    // remove and the broker drops the retained entry.
    for (const char* mid : measurement_id::kAll) {
        clear(config_.discoveryPrefix + "/sensor/" + base + "/" + sanitizeId(mid) + "/config");
    }
    clear(config_.discoveryPrefix + "/sensor/" + base + "/status/config");
    clear(config_.discoveryPrefix + "/binary_sensor/" + base + "/inverter_online/config");

    if (ok) {
        heliograph::log::info("MQTT: cleared retained topics for removed device %s", id.c_str());
    }
    return ok;
}

void MqttOutput::stop() {
    if (started_) {
        // Say goodbye properly so subscribers do not have to wait for the will.
        g_client.publish(topics_.availability().c_str(), 1, true, kPayloadOffline);
        g_client.disconnect();
        started_ = false;
    }
}

}  // namespace heliograph::mqtt

#else  // !ESP32

namespace heliograph::mqtt {

MqttOutput::MqttOutput(MqttConfig config, PublishPolicy publishPolicy)
    : config_(std::move(config)), topics_(config_.baseTopic, "host"),
      publishPolicy_(publishPolicy) {}

bool MqttOutput::begin(const BridgeInfo&) { return false; }
void MqttOutput::loop(const std::vector<DeviceView>&, const BridgeInfo&,
                      const DiagnosticsSnapshot&,
                      uint64_t) {}
void MqttOutput::stop() { started_ = false; }
bool MqttOutput::forgetDevice(const DeviceId&, const BridgeInfo&) { return false; }
bool MqttOutput::connected() const { return false; }
void MqttOutput::onConnected(Channel&, const DeviceState&, const BridgeInfo&) {}
void MqttOutput::publishDiscovery(Channel&, const DeviceState&, const BridgeInfo&) {}
MqttOutput::Channel& MqttOutput::channelFor(const DeviceView&, const BridgeInfo&) {
    static Channel unused{"", MqttTopics("", ""), "", PublishThrottle({}), false, 0};
    return unused;
}

}  // namespace heliograph::mqtt

#endif
