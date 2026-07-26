// SPDX-License-Identifier: MIT
//
// Prometheus text exposition. Pure string building, host-tested.
//
// Naming rules followed here: lowercase, snake_case, base unit in the name, counters end in
// _total. An unknown measurement is OMITTED rather than exported as 0 -- Prometheus handles a
// missing sample correctly (the series simply has a gap), whereas a zero would be recorded as
// a real reading and averaged into the graph.

#pragma once

#include <string>
#include <vector>

#include "device/bridge_info.h"
#include "device/device_state.h"
#include "diagnostics/diagnostics.h"

namespace heliograph::prometheus {

/// One inverter, as /metrics sees it.
struct DeviceMetrics {
    /// The registered device id -- the same string REST serves at /api/v1/devices and MQTT
    /// uses in its topics. One id per inverter across every interface, so a Grafana panel and
    /// a Home Assistant entity can be lined up without a lookup table.
    std::string        id;
    const DeviceState* state = nullptr;
};

/// Renders the /metrics body.
///
/// Every inverter series carries a `device` label, including the first. That is a breaking
/// change for a dashboard built when there was one inverter -- the query still matches, but it
/// returns one series per device instead of one -- and it was chosen over leaving the first
/// unlabelled: an asymmetry that has to be explained forever is worse than a one-off edit, and
/// `sum by (device)` with a blank label is not something anyone should have to debug.
/// docs/prometheus.md says exactly what to change.
///
/// Bridge-wide series (uptime, heap, WiFi, the RS485 and poll counters, relays) carry no device
/// label, because they are not per device: the counters live in one Diagnostics for the whole
/// bus.
///
/// The serial number is deliberately not a label: it is high cardinality by definition and
/// would multiply every series by the number of devices ever seen by a scraper.
std::string buildMetrics(const std::vector<DeviceMetrics>& devices, const BridgeInfo& bridge,
                         const DiagnosticsSnapshot& diagnostics);

}  // namespace heliograph::prometheus
