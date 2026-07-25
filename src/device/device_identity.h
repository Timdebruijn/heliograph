// SPDX-License-Identifier: MIT
//
// Generic device identity. Unknown fields stay empty; nothing here is ever invented.

#pragma once

#include <string>

namespace heliograph {

struct DeviceIdentity {
    std::string manufacturer;
    std::string model;
    std::string serialNumber;
    std::string firmwareVersion;
    std::string hardwareVersion;
    std::string protocolName;
    std::string protocolVersion;
    std::string driverId;
    /// What distinguishes THIS device from another one served by the same driver on the same
    /// bus: the Modbus unit id, the PMU address. Set by the driver from its own options, which
    /// is the only place that knows which option does the addressing.
    ///
    /// Exists because the serial number does not, at the moment it is needed. deviceId() is
    /// read once, in setup(), to key the state store -- and at that point no driver in this
    /// build has a serial: some never read one at all, and the rest only learn theirs during
    /// the first poll or chain walk. So several identical inverters all resolved to the bare
    /// driver id, DeviceManager handed them the SAME store, and they overwrote each other's
    /// readings into one set of Home Assistant entities (review, 2026-07-25).
    std::string instanceKey;

    /// A field is "not available" precisely when it is empty. Outputs must omit such fields
    /// rather than emit an empty string, so consumers can tell "unknown" from "blank".
    static bool available(const std::string& field) { return !field.empty(); }

    /// Stable per-device id, used in REST paths and MQTT topics.
    ///
    /// Prefers the serial number, which identifies the physical unit no matter how it is
    /// addressed. Falls back to the instance key, which is known at boot and is what keeps two
    /// inverters of the same model on one bus apart. Only when neither exists is it the bare
    /// driver id -- which is correct for the single-device case that has always been the norm,
    /// and is why the collision went unnoticed.
    std::string deviceId() const {
        if (!serialNumber.empty()) {
            return driverId + "-" + serialNumber;
        }
        if (!instanceKey.empty()) {
            return driverId + "-" + instanceKey;
        }
        return driverId;
    }
};

}  // namespace heliograph
