// SPDX-License-Identifier: MIT

#include "growatt_driver.h"

namespace heliograph::growatt {

const DriverDescriptor& descriptor() {
    static const DriverDescriptor d = [] {
        DriverDescriptor x;
        x.id           = "growatt_modbus";
        x.displayName  = "Growatt (Modbus RTU)";
        x.manufacturer = "Growatt";
        x.protocol     = "Modbus RTU";
        x.description =
            "Growatt string and hybrid inverters over Modbus RTU (Protocol II). Read-only "
            "until the register map is confirmed on hardware; battery control follows. "
            "Table-driven per model family -- SPH first.";
        x.supportedTransports = {TransportType::Rs485, TransportType::Mock};
        // Growatt's two documented line speeds. Discovery tries both; the SPH default is 9600
        // but some units ship at 115200, and guessing wrong on a live bus just looks like
        // silence. 8N1, unit id 1 by default.
        x.recommendedSerialProfiles = {
            SerialProfile{9600, SerialParity::None, 8, 1, 1000, 3},
            SerialProfile{115200, SerialParity::None, 8, 1, 1000, 3},
        };
        // Experimental: transcribed from community register maps, not yet seen against an
        // SPH6000. Promotes to Beta once reads match the inverter display (see
        // docs/growatt-sph-protocol.md).
        x.supportLevel            = DriverSupportLevel::Experimental;
        x.probePriority           = 5;
        x.supportsAutoDetection   = true;
        x.supportsMultipleDevices = true;
        x.supportsRead            = true;
        // No write path is wired yet. This is the read-only bring-up build; battery control is
        // the deliberate next step, gated on the map being confirmed.
        x.supportsWrite = false;
        x.options       = {
            DriverOption{"unit_id", "Modbus unit id",
                         "The inverter's Modbus slave address (Growatt default is 1). "
                         "Range 1-247.",
                         "1",
                         {}},
            DriverOption{"profile", "Register-map profile",
                         "Which Growatt register map to use (profiles/growatt/). "
                         "Empty = the default profile.",
                         "",
                         {}},
        };
        return x;
    }();
    return d;
}

std::unique_ptr<InverterDriver> factory(Transport& transport, const DriverOptions& options) {
    return std::make_unique<GrowattDriver>(transport, optionsFrom(options));
}

}  // namespace heliograph::growatt
