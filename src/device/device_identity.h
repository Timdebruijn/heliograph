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

    /// A field is "not available" precisely when it is empty. Outputs must omit such fields
    /// rather than emit an empty string, so consumers can tell "unknown" from "blank".
    static bool available(const std::string& field) { return !field.empty(); }

    /// Stable per-device id, used in REST paths and MQTT topics. Chosen so that adding a
    /// second physical device later needs no API change.
    std::string deviceId() const {
        if (serialNumber.empty()) {
            return driverId;
        }
        return driverId + "-" + serialNumber;
    }
};

}  // namespace heliograph
