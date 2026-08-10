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
#include <mutex>
#include <optional>

#include "commands/command_dispatcher.h"
#include "device/bridge_info.h"
#include "device/command.h"
#include "device/device_state.h"
#include "diagnostics/diagnostics.h"
#include "mqtt_topics.h"
#include "publish_policy.h"
#include "state/state_store.h"

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
    explicit MqttOutput(MqttConfig config, PublishPolicy publishPolicy = {});

    /// Sets up the client and starts connecting. Non-blocking.
    bool begin(const BridgeInfo& bridge);

    /// One inverter's current state, as handed to loop().
    struct DeviceView {
        DeviceId           id;
        const DeviceState* state;
        /// True for the device that keeps the bridge-scoped topics and unique ids. Decided by
        /// the caller from the CONFIGURED order, not by arrival: keying it on "first one seen"
        /// meant that a boot where device 1 failed to start handed device 2 its topics, its
        /// entities and its recorder history. At most one, possibly none.
        bool primary;

        /// Spelled out rather than aggregate-initialised, and with no default on `primary`, so
        /// that omitting it does not compile.
        ///
        /// It shipped omitted. The field was added with a default of false and the caller was
        /// never updated, so every device -- including the first -- took the device-scoped
        /// topics, and the back-compat guarantee the whole design rests on was quietly not in
        /// force. `{id, state}` still compiled and still meant something reasonable, which is
        /// exactly why nothing caught it (2026-07-25).
        DeviceView(DeviceId deviceId, const DeviceState* deviceState, bool isPrimary)
            : id(std::move(deviceId)), state(deviceState), primary(isPrimary) {}
    };

    /// Drives reconnects and publishing. Call regularly from the MQTT task; never blocks.
    ///
    /// `devices` is every polled device, in a stable order -- the FIRST one keeps the
    /// bridge-scoped topics and unique_ids it has always had, so an existing install's Home
    /// Assistant entities and their history survive this becoming plural. An empty list is
    /// legal and publishes only the bridge's own topics.
    void loop(const std::vector<DeviceView>& devices, const BridgeInfo& bridge,
              const DiagnosticsSnapshot& diagnostics, uint64_t nowMs);

    /// Clears what a device left on its own `<base>/<bridgeId>/device/<id>/` tree: the retained
    /// state, identity and capabilities, and every Home Assistant discovery config it could have
    /// published there.
    ///
    /// Retained messages outlive the device that sent them. Because availability is
    /// bridge-scoped -- deliberately, so a sleeping inverter does not vanish at night -- an
    /// orphaned entity does not go "unavailable"; it reports ONLINE forever, showing whatever
    /// it last read, and anything summing the inverters keeps counting it.
    ///
    /// Device-scoped ONLY, and takes no `primary` flag by design: the bridge-scoped tree always
    /// has a live or incoming owner, and clearing it would delete that owner's entities. Which
    /// devices qualify is decided by mqtt::devicesToForget(), which is host-testable; this is
    /// not.
    ///
    /// Empty retained payloads are how Home Assistant is told to forget an entity -- the same
    /// mechanism the relay switches already use when the feature is disabled. The topics are
    /// enumerated from the canonical measurement ids rather than from the device's state,
    /// because by the time this runs there is no state: the device is gone.
    ///
    /// Returns false if any publish was refused (link down, outbox out of memory). The caller
    /// must not record the device as forgotten in that case -- nothing else remembers it.
    bool forgetDevice(const DeviceId& id, const BridgeInfo& bridge);

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

    /// Handles a write command arriving on a device's <prefix>/command/set. The SAME function
    /// RestContext::submitCommand is built from in main.cpp, so REST and MQTT enqueue through
    /// one counter and one CommandQueue rather than two independent paths that could disagree
    /// about what "one in flight" means. Returns the request id used on success (a JSON ack is
    /// published to that device's commandResult() topic), or nullopt when one was already
    /// pending.
    ///
    /// Like the relay handler, this runs on the MQTT task and only ever *enqueues* -- it must
    /// never itself run the RS485 transaction. Unlike the relay handler it needs no mutex here:
    /// CommandQueue is its own lock, not something this class and REST share directly.
    using CommandSubmitFn =
        std::function<std::optional<std::string>(const std::string& deviceId,
                                                   InverterCommand   command)>;
    void setCommandHandler(CommandSubmitFn handler) { commandSubmit_ = std::move(handler); }

    /// Looks up the real outcome of a request id once rs485Task has actually processed it. The
    /// SAME function RestContext::commandOutcome reads from -- both transports observe the one
    /// CommandQueue. loop() polls this for whichever channel is awaiting a result and, once it
    /// resolves, publishes it to that channel's commandResult() topic (see loop()'s own note).
    using CommandOutcomeFn = std::function<std::optional<DispatchOutcome>(
        const std::string& deviceId, const std::string& requestId)>;
    void setCommandOutcomeProvider(CommandOutcomeFn provider) {
        commandOutcomeProvider_ = std::move(provider);
    }

private:
    /// Everything that is per-inverter rather than per-bridge, keyed by device id. Every
    /// channel is created in the first loop() pass -- the device list is fixed at boot -- but
    /// keying by id rather than by position means nothing depends on that staying true.
    struct Channel {
        DeviceId        id;
        MqttTopics      topics;
        std::string     uniqueBase;
        PublishThrottle throttle;
        bool            discoveryPublished  = false;
        uint64_t        discoveredSignature = 0;
        /// The request id THIS channel most recently submitted and is still awaiting the real
        /// outcome for, or empty when nothing is outstanding. Set in onMessage on a successful
        /// submitCommand(); cleared by loop() once commandOutcomeProvider_ resolves it.
        std::string pendingCommandRequestId;
    };

    /// publish() whose refusals are counted. Every publish in this class goes through it --
    /// see the definition for why discarding the return value was hiding a real failure mode.
    bool publishTracked(const char* topic, uint8_t qos, bool retain, const char* payload);

    Channel& channelFor(const DeviceView& view, const BridgeInfo& bridge);

    /// Both return whether every publish they attempted actually left -- discovery's entities
    /// specifically, not availability/identity/capabilities, which loop() does not gate on.
    /// A caller that ignores the return and commits its bookkeeping anyway is exactly the bug
    /// a review caught here (2026-07-30): a refusal from the new memory guard looked identical
    /// to a delivered announcement, so a channel could mark itself "discovery done" while Home
    /// Assistant never got the entities -- and, since the signature would already match on the
    /// next tick, nothing would ever retry short of an actual reconnect.
    bool onConnected(Channel& channel, const DeviceState& state, const BridgeInfo& bridge);
    bool publishDiscovery(Channel& channel, const DeviceState& state, const BridgeInfo& bridge);

    MqttConfig   config_;
    /// Bridge-scoped: availability, diagnostics, relay and DRM. Never a device's.
    MqttTopics   topics_;
    Diagnostics* diagnostics_ = nullptr;

    std::vector<Channel> channels_;
    /// Guards channels_'s STRUCTURE (channelFor()'s push_back can reallocate it) and each
    /// Channel's pendingCommandRequestId. Both are touched from two tasks: loop() and
    /// channelFor() run on whatever task calls loop() (rs485Task), onMessage's new command
    /// handling runs on espMqttClient's own task -- the same task boundary resyncRequested_
    /// and relayAckRequested_ already guard against, but those are lone bools; a vector and a
    /// string need a mutex, not an atomic. Every OTHER Channel field (topics, uniqueBase,
    /// throttle, discoveryPublished, discoveredSignature) stays single-task (only loop()/
    /// channelFor() ever touch them), so nothing else needs to take this lock.
    std::mutex channelsMutex_;
    PublishPolicy        publishPolicy_;

    // espMqttClient's setClientId/setWill/setServer/setCredentials store the POINTER, not a
    // copy (MqttClientSetup.h: `_clientId = clientId;`). Every string handed to them must
    // therefore live as long as this object. Passing bridge.bridgeId.c_str() straight through
    // worked only while the freed temporary happened to keep its bytes; a config change that
    // reshuffled the heap turned the client id into garbage and the broker refused it with
    // "client identifier not valid".
    std::string clientId_;
    std::string willTopic_;

    bool     started_           = false;
    uint64_t lastDiagnosticsMs_  = 0;
    uint64_t nextReconnectMs_    = 0;
    /// Exponential back-off, capped. An unreachable broker must not turn into a busy loop.
    uint32_t reconnectDelayMs_ = 1000;
    /// Set by onDisconnect (library task), consumed by loop() (caller's task): forces a
    /// throttle reset and a discovery republish after a reconnect, because retained
    /// messages may not have survived the broker outage. The disconnect callback itself
    /// must not touch throttle_/discoveryPublished_ -- those belong to loop()'s task.
    std::atomic<bool> resyncRequested_{false};
    /// Edge detection for the reconnect counter. `wasConnected_` tracks the previous loop's
    /// link state so a false→true transition is counted once; `everConnected_` makes the
    /// FIRST connect at boot not count as a reconnect. See loop().
    bool wasConnected_  = false;
    bool everConnected_ = false;

    RelayCommandFn relayCommand_;
    DrmCommandFn   drmCommand_;
    CommandSubmitFn  commandSubmit_;
    CommandOutcomeFn commandOutcomeProvider_;
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
    /// Fingerprint of the configured roles at the last publish. Roles rename the switches,
    /// rebuild the select options AND change the derived mode, so a role change must
    /// re-announce discovery and re-ack state -- "applied immediately" would otherwise
    /// only be true after the next reconnect (self-review of PR #3).
    uint64_t lastRelayRolesSig_ = 0;
};

inline constexpr uint32_t kMaxReconnectDelayMs = 60000;

/// Largest contiguous internal block below which this output stops handing work to the MQTT
/// client. Matches espMqttClient's own EMC_MIN_FREE_MEMORY deliberately: the intent is the
/// library's, only the pool is different -- see refusePublishForMemory.
inline constexpr uint32_t kMinFreeBlockBytes = 16384;

/// Whether a publish must be refused because internal memory is too low to risk it.
///
/// espMqttClient already refuses a PUBLISH when memory is short: Packets/Packet.cpp calls
/// _allocate(len, check=true), which bails below EMC_MIN_FREE_MEMORY (16384). The problem is
/// WHICH memory it looks at. EMC_GET_FREE_MEMORY() is a hard #define -- no #ifndef, so a build
/// flag cannot reach it -- reading std::max(ESP.getMaxAllocHeap(), ESP.getMaxAllocPsram()). A
/// publish is ~100-200 bytes, far under SPIRAM_MALLOC_ALWAYSINTERNAL (4096), so malloc serves
/// it from INTERNAL SRAM; but on a board with 8 MB of mostly-idle PSRAM that max() reports
/// megabytes and the guard never fires. The board with NO PSRAM is therefore the only one where
/// the library's own safety net works, and the two with more memory are the ones that can
/// exhaust internal SRAM with the failure counter reading zero throughout (audit F5,
/// docs/audit-2026-07-29.md).
///
/// This applies the same 16 KB intent to the pool the allocation actually comes from, on every
/// variant, without forking the dependency. Pure, and takes the figure as an argument, so the
/// decision is host-tested; only the caller reaches for ESP.getMaxAllocHeap().
///
/// The trade-off, stated because it is real: under transient pressure from something else --
/// e.g. the dashboard reload storm the audit measured on the 6CH -- publishes are refused
/// rather than queued. Measurements self-heal, because every poll cycle republishes them. A
/// one-shot discovery message does not self-heal on its own, but loop() now retries the whole
/// announcement on the next tick when a refusal happens (see onConnected/publishDiscovery) --
/// that used to not be true, and a review caught it (2026-07-30). Accepted: at this threshold
/// the alternative is risking the allocation that takes the device down, and a refusal is
/// counted where an exhaustion is silent.
///
/// UNVERIFIED under real load, and worth being honest about: the audit's reload-storm figure
/// (a 93 KB dip on the 6CH, docs/audit-2026-07-29.md) is ESP.getMinFreeHeap() -- total free
/// heap's since-boot low-water mark -- not ESP.getMaxAllocHeap(), the largest CONTIGUOUS block
/// this guard actually reads. The two diverge under fragmentation (the same report shows
/// 78 744 B min-free next to a 102 388 B largest block in one snapshot), and nobody has
/// captured the largest-block figure specifically during that storm on any variant. The 16 KB
/// threshold is therefore validated against the resting figure (90 100 B measured, comfortably
/// above it), not against a load transient in the metric that matters here.
///
/// Cost, asked about and not free: ESP.getMaxAllocHeap() -> heap_caps_get_largest_free_block()
/// walks the internal heap's free-block bookkeeping under the heap lock, once per publish --
/// dozens of times per loop() tick on a four-device board. Not cached across a tick on
/// purpose: publishTracked() is also called from forgetDevice(), reached from the REST task
/// rather than loop()'s, and a cache shared across tasks needs the same synchronisation care
/// as every other cross-task field in this class (resyncRequested_, relayAckRequested_) for a
/// saving nobody has shown matters yet. Per the project's own rule, that trade avoids adding
/// complexity for a cost that is real but not demonstrated to be a problem.
inline bool refusePublishForMemory(uint32_t largestFreeBlockBytes) {
    return largestFreeBlockBytes < kMinFreeBlockBytes;
}

}  // namespace heliograph::mqtt
