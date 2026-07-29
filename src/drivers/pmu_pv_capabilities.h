// SPDX-License-Identifier: MIT
//
// The read channels a single-phase PV inverter on the PMU protocol family reports.
//
// Two drivers declared this list independently, line for line identical, twelve calls each.
// That is not a coincidence worth preserving by hand: both speak the same framing and their
// parsers emit the SAME fifteen measurement ids, because the channels come from the payload
// layout rather than from the manufacturer. Adding a channel to one and forgetting the other
// would have been silent -- the capability list is what /api/v1/devices/<id>/capabilities
// publishes and what the dashboard uses to decide which rows exist, so a driver that stops
// declaring a channel it still delivers simply stops showing it.
//
// It lives in the drivers layer, not in src/protocols/pmu/. That directory is a pure codec
// which includes <cstddef> and <cstdint> and nothing else; teaching it about the canonical
// device model would be the more expensive kind of tidying.
//
// WHAT THIS DELIBERATELY DOES NOT COVER: anything a specific device adds or lacks. The two
// payload variants are not identical -- one carries a fault bitmask and the other has no error
// field at all -- so each driver states its own difference next to the call, where the reason
// for it can be read. A helper that tried to cover both would need a flag per difference, and
// a list of flags is the same duplication with more indirection.

#pragma once

#include "device/capability.h"

namespace heliograph {

/// Every read channel the PMU-family PV payload carries, with the single-phase, single-string
/// shape both devices boot with.
///
/// mpptCount is the value at boot, not a verdict: both drivers revise it upward when a second
/// string turns out to report real voltage, which is why it is set here rather than derived.
inline InverterCapabilities pmuPvCapabilities() {
    InverterCapabilities caps;
    caps.addRead(InverterCapability::ReadAcPower);
    caps.addRead(InverterCapability::ReadAcVoltage);
    caps.addRead(InverterCapability::ReadAcCurrent);
    caps.addRead(InverterCapability::ReadGridFrequency);
    caps.addRead(InverterCapability::ReadDcVoltage);
    caps.addRead(InverterCapability::ReadDcCurrent);
    caps.addRead(InverterCapability::ReadDcPower);
    caps.addRead(InverterCapability::ReadEnergyToday);
    caps.addRead(InverterCapability::ReadEnergyTotal);
    caps.addRead(InverterCapability::ReadTemperature);
    caps.addRead(InverterCapability::ReadOperatingHours);
    caps.addRead(InverterCapability::ReadStatus);
    caps.phaseCount = 1;
    caps.mpptCount  = 1;
    caps.hasBattery = false;
    return caps;
}

}  // namespace heliograph
