// SPDX-License-Identifier: MIT
//
// Driver metadata. Everything an output adapter or the web UI needs to talk about a driver
// without knowing which driver it is.

#pragma once

#include <map>
#include <string>
#include <vector>

#include "transport/serial_profile.h"
#include "transport/transport.h"

namespace heliograph {

/// A driver-specific setting, declared by the driver rather than baked into the config model.
///
/// This exists so that no manufacturer-specific field ever appears in Configuration. The
/// moment one does ("eversolar_layout"), adding a second driver means editing the config
/// struct, the validator, the serialiser and the web form -- which is exactly the coupling
/// the architecture is supposed to prevent.
///
/// It is also self-describing: the web UI renders these generically, so a new driver's
/// options appear in the interface without a line of frontend work.
struct DriverOption {
    std::string key;          ///< stable, snake_case, e.g. "layout"
    std::string displayName;
    std::string description;
    std::string defaultValue;
    /// Empty means free-form text. Otherwise the value must be one of these.
    std::vector<std::string> allowedValues;
};

using DriverOptions = std::map<std::string, std::string>;

enum class DriverSupportLevel : uint8_t {
    /// Protocol re-implemented from a reference, not yet confirmed against real hardware.
    Experimental,
    /// Confirmed against real hardware, not yet run long enough to trust unattended.
    Beta,
    /// Validated and soak-tested.
    Stable,
    Deprecated,
};

const char* supportLevelName(DriverSupportLevel level);

struct DriverDescriptor {
    /// Stable identifier, used in config, MQTT topics and REST paths. Never rename: doing so
    /// silently orphans a user's stored configuration.
    std::string id;
    std::string displayName;
    std::string manufacturer;
    std::string protocol;
    std::string description;

    std::vector<TransportType> supportedTransports;
    /// Profiles that are actually plausible for this protocol. Discovery tries these and
    /// nothing else -- guessing baud rates on a live bus is not a read-only operation in any
    /// meaningful sense.
    std::vector<SerialProfile> recommendedSerialProfiles;

    DriverSupportLevel supportLevel = DriverSupportLevel::Experimental;

    /// Higher runs first during discovery. Lets a specific driver claim a device before a
    /// generic one gets to it.
    int probePriority = 0;

    bool supportsAutoDetection   = false;
    bool supportsMultipleDevices = false;
    bool supportsRead            = true;
    bool supportsWrite           = false;

    /// Settings this driver understands. See DriverOption.
    std::vector<DriverOption> options;

    /// Looks up an option value, falling back to the declared default.
    std::string optionOr(const DriverOptions& values, const std::string& key) const {
        if (const auto it = values.find(key); it != values.end()) {
            return it->second;
        }
        for (const auto& o : options) {
            if (o.key == key) {
                return o.defaultValue;
            }
        }
        return {};
    }
};

struct DriverOptionError {
    std::string key;
    std::string message;
};

/// Validates option values against what the driver declares. Unknown keys are rejected: a
/// silently ignored setting is worse than a refused one, because the user believes it applied.
bool validateDriverOptions(const DriverDescriptor& descriptor, const DriverOptions& values,
                           DriverOptionError& error);

}  // namespace heliograph
