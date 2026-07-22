// SPDX-License-Identifier: MIT
//
// Home Assistant MQTT Discovery.
//
// Entities are generated from measurements and capabilities alone. There is no per-device
// field table anywhere in this file and there must never be one: the mapping runs
// MeasurementType/Unit -> device_class/state_class, so a driver that reports a battery gets
// battery entities without this code being touched.

#pragma once

#include <string>
#include <vector>

#include "device/bridge_info.h"
#include "device/device_state.h"
#include "mqtt_topics.h"

namespace heliograph::mqtt {

struct DiscoveryEntity {
    std::string configTopic;  ///< retained; publish an empty payload here to remove the entity
    std::string uniqueId;
    std::string payload;
};

/// Builds one config message per publishable entity.
///
/// Rules that are deliberately structural rather than conventional:
///   - a measurement with supported=false produces no entity at all;
///   - no control entity is emitted while capabilities.write is empty -- checked on the
///     bitset, never on the driver id;
///   - availability tracks the BRIDGE, so a sleeping inverter leaves entities available and
///     reporting unknown, instead of vanishing from the dashboard every night.
std::vector<DiscoveryEntity> buildDiscoveryEntities(const DeviceState& state,
                                                    const BridgeInfo&  bridge,
                                                    const MqttTopics&  topics,
                                                    const std::string& discoveryPrefix);

/// Diagnostic entities for the bridge itself (RSSI, uptime, heap, poll counters).
std::vector<DiscoveryEntity> buildBridgeDiagnosticEntities(const BridgeInfo&  bridge,
                                                           const MqttTopics&  topics,
                                                           const std::string& discoveryPrefix);

/// Turns a measurement id into an id fragment usable in a topic and unique_id.
/// "ac.power.total" -> "ac_power_total"
std::string sanitizeId(const std::string& measurementId);

/// Fingerprint of the discovery-relevant measurement model: the ordered ids of supported
/// measurements. The MQTT output republishes discovery when this changes; comparing a bare
/// count would miss a same-size swap of one channel for another.
std::string discoverySignature(const DeviceState& state);

}  // namespace heliograph::mqtt
