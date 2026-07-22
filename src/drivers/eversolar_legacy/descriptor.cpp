// SPDX-License-Identifier: MIT

#include "eversolar_driver.h"

namespace heliograph::eversolar {

const DriverDescriptor& descriptor() {
    static const DriverDescriptor d = [] {
        DriverDescriptor x;
        x.id           = "eversolar_legacy";
        x.displayName  = "Ever-Solar / Zeversolar (legacy PMU)";
        x.manufacturer = "Ever-Solar";
        x.protocol     = "EverSolar PMU RS485";
        x.description =
            "Manufacturer-specific PMU protocol over RS485, as used by Ever-Solar and "
            "Zeversolar TL-series inverters. Read-only: the protocol defines no write "
            "operations.";
        x.supportedTransports = {TransportType::Rs485, TransportType::Mock};
        // 9600 8N1 is hardcoded in the reference implementation and is the only profile
        // known to work. Offering more would be guessing on a live bus.
        x.recommendedSerialProfiles = {SerialProfile{9600, SerialParity::None, 8, 1, 1000, 3}};
        // Beta since 2026-07-19: Phase 3 exit criteria met against a real TL3000-20 -- stable
        // reads over hours, values matching eversolar-monitor within the documented tolerance
        // (energy.total exactly +HI*0.1 kWh, hours/status/impedance/serial exact), and both
        // captured frames committed as fixtures. Stable only after the Phase 9 soak test.
        x.supportLevel            = DriverSupportLevel::Beta;
        x.probePriority           = 10;
        x.supportsAutoDetection   = true;
        // The protocol assigns addresses to multiple inverters; the MVP allows only one
        // active device, but that is an application limit, not a driver limit.
        x.supportsMultipleDevices = true;
        x.supportsRead            = true;
        x.supportsWrite           = false;
        // Declared here, not in Configuration: the payload-length hypothesis is this
        // driver's problem, and the config model must stay free of manufacturer specifics.
        // See docs/eversolar-protocol.md for why an override exists at all.
        x.options = {DriverOption{
            "layout", "Payload layout",
            "How to interpret the measurement payload. 'auto' derives it from the frame "
            "length (28 bytes = 1 string, 32 = 2). Force one only if a device contradicts "
            "that.",
            "auto", {"auto", "single", "dual"}}};
        return x;
    }();
    return d;
}

std::unique_ptr<InverterDriver> factory(Transport& transport, const DriverOptions& options) {
    return std::make_unique<EversolarDriver>(transport, optionsFrom(options));
}

}  // namespace heliograph::eversolar
