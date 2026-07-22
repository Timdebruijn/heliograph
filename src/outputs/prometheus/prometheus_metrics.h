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

#include "device/bridge_info.h"
#include "device/device_state.h"
#include "diagnostics/diagnostics.h"

namespace heliograph::prometheus {

/// Renders the /metrics body.
///
/// The serial number is deliberately not a label: it is high cardinality by definition and
/// would multiply every series by the number of devices ever seen by a scraper.
std::string buildMetrics(const DeviceState& state, const BridgeInfo& bridge,
                         const DiagnosticsSnapshot& diagnostics);

}  // namespace heliograph::prometheus
