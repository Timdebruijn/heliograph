// SPDX-License-Identifier: MIT

#include <string>
#include <vector>

#include "modbus_profile_driver.h"

namespace heliograph::profile {
namespace {

/// The `profile` option's allowed values: every compiled-in profile id, plus the empty
/// string, which keeps meaning "use the default profile" rather than becoming a rejected
/// value the moment this list stops being empty.
std::vector<std::string> profileOptionValues() {
    std::vector<std::string> values;
    values.reserve(profileCount() + 1);
    values.emplace_back("");
    for (size_t i = 0; i < profileCount(); ++i) {
        values.emplace_back(profileAt(i).id);
    }
    return values;
}

}  // namespace

const DriverDescriptor& descriptor() {
    static const DriverDescriptor d = [] {
        DriverDescriptor x;
        x.id          = "modbus_profile";
        x.displayName = "Modbus RTU (profile-driven)";
        // Named after the protocol, not a vendor, because that is what it is: one driver for any
        // device whose register map can be written down as data. The BRAND belongs to the
        // profile -- each one declares its own manufacturer, and that is what the device reports
        // as its identity. This field used to name a single vendor, which was accurate while
        // there was one family of profiles and became false the moment a second vendor's map
        // arrived as a TOML file.
        x.manufacturer = "Various";
        x.protocol     = "Modbus RTU";
        x.description =
            "String inverters and hybrids that answer Modbus RTU register reads. The register "
            "map is data, not code: pick the profile matching your model. Adding a model is a "
            "TOML file -- see docs/adding-a-device.md.";
        x.supportedTransports = {TransportType::Rs485, TransportType::Mock};
        // Discovery's fallbacks, used only when the selected profile declares no [serial]
        // section of its own. 9600 8N1 is what every vendor protocol document consulted so far
        // specifies; 115200 stays because some units genuinely ship that way, and guessing wrong
        // on a live bus just looks like silence.
        x.recommendedSerialProfiles = {
            SerialProfile{9600, SerialParity::None, 8, 1, 1000, 3},
            SerialProfile{115200, SerialParity::None, 8, 1, 1000, 3},
        };
        // Experimental as a driver-level floor: every profile is transcribed from documentation
        // or community maps, and none has yet been confirmed against the device it describes.
        // Per-model status lives in the profile's own comments and in README's table.
        x.supportLevel            = DriverSupportLevel::Experimental;
        x.probePriority           = 5;
        x.supportsAutoDetection   = true;
        x.supportsMultipleDevices = true;
        x.supportsRead            = true;
        // The driver HAS a write path: execute() writes one holding register over FC06. Whether
        // a given DEVICE can be written to is a per-profile question -- it needs a [[write]] row
        // marked verified -- and capabilities() is the authoritative answer to that. This flag
        // says only "this driver can ever write", which is now true; it read false for a while
        // after the write path landed. See docs/device-profiles/write-path.md.
        x.supportsWrite    = true;
        x.addressOptionKey = "unit_id";
        x.options          = {
            // Bounded here, not only in optionsFrom(): the parser's fallback is silent by the
            // time it runs, and on a bus of identical inverters the value it falls back to is
            // the address the first one already uses.
            DriverOption{"unit_id", "Modbus unit id",
                         "The inverter's Modbus slave address. Most vendors ship 1, some ship "
                         "247 -- check the profile's notes. Range 1-247.",
                         "1",
                         {},
                         1, 247},
            DriverOption{"profile", "Register-map profile",
                         "Which register map to use (see profiles/). "
                         "Empty = the default profile.",
                         "",
                         profileOptionValues()},
        };
        return x;
    }();
    return d;
}

std::unique_ptr<InverterDriver> factory(Transport& transport, const DriverOptions& options) {
    return std::make_unique<ModbusProfileDriver>(transport, optionsFrom(options));
}

}  // namespace heliograph::profile
