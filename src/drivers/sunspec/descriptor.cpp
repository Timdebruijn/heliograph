// SPDX-License-Identifier: MIT

#include <cstdlib>

#include "sunspec_driver.h"

namespace heliograph::sunspec {

const DriverDescriptor& descriptor() {
    static const DriverDescriptor d = [] {
        DriverDescriptor x;
        x.id          = "sunspec";
        x.displayName = "SunSpec Modbus (generic)";
        // Named after the protocol, not a manufacturer, because that is exactly what it is:
        // the first driver here that is not tied to one vendor's map.
        x.manufacturer = "SunSpec";
        x.protocol     = "SunSpec Modbus RTU";
        x.description =
            "Any inverter implementing the SunSpec Modbus standard (models 101/102/103). The "
            "device describes its own register layout at runtime, so no per-vendor map is "
            "needed. Read-only. Not yet confirmed against physical hardware -- see "
            "docs/sunspec.md for which devices are expected to work and which have actually "
            "been tested.";
        x.supportedTransports = {TransportType::Rs485, TransportType::Mock};
        // SunSpec does not mandate a line speed; 9600 and 19200 are both common, so both are
        // offered and discovery tries them in order.
        x.recommendedSerialProfiles = {SerialProfile{9600, SerialParity::None, 8, 1, 1000, 3},
                                       SerialProfile{19200, SerialParity::None, 8, 1, 1000, 3}};
        x.supportLevel              = DriverSupportLevel::Experimental;
        // Above the vendor Modbus driver: the "SunS" marker is a far stronger fingerprint than
        // any register-shape heuristic, so when a device answers it, it is not a guess.
        x.probePriority           = 20;
        x.supportsAutoDetection   = true;
        x.supportsMultipleDevices = false;
        x.supportsRead            = true;
        x.supportsWrite           = false;
        x.options                 = {
            DriverOption{"unit_id", "Modbus unit id",
                                         "Slave address of the inverter on the RS485 bus. Range 1-247.", "1", {}},
            DriverOption{"base_address", "SunSpec base register",
                                         "Where the 'SunS' marker lives. 40000 covers most devices; 50000 is the "
                                         "other common choice, and some vendors sit elsewhere. Each extra guess "
                                         "would cost a discovery round trip, so this is set rather than searched.",
                                         "40000",
                                         {}}};
        return x;
    }();
    return d;
}

std::unique_ptr<InverterDriver> factory(Transport& transport, const DriverOptions& options) {
    (void)transport;
    const DriverDescriptor& d = descriptor();
    SunspecOptions          o;

    // Base 10 explicitly on both: strtol with base 0 reads a leading zero as octal, which
    // turned a driver option into a different number once already in this project.
    const long unit = std::strtol(d.optionOr(options, "unit_id").c_str(), nullptr, 10);
    if (unit >= 1 && unit <= 247) {
        o.unitId = static_cast<uint8_t>(unit);
    }
    const long base = std::strtol(d.optionOr(options, "base_address").c_str(), nullptr, 10);
    if (base >= 0 && base <= 0xFFFF) {
        o.baseAddress = static_cast<uint16_t>(base);
    }
    return std::make_unique<SunspecDriver>(o);
}

}  // namespace heliograph::sunspec
