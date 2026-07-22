// SPDX-License-Identifier: MIT

#include "solax_driver.h"

namespace heliograph::solax {

const DriverDescriptor& descriptor() {
    static const DriverDescriptor d = [] {
        DriverDescriptor x;
        x.id           = "solax_x1";
        x.displayName  = "SolaX X1 series (RS485)";
        x.manufacturer = "SolaX";
        x.protocol     = "SolaX X1 RS485 (PMU)";
        x.description =
            "SolaX X1 single-phase inverters (X1 Mini G1/G2/G3 and related) over the AA55 "
            "PMU-family RS485 protocol. Read-only: the protocol defines no write operation; "
            "output curtailment on these units works via the inverter's separate "
            "meter-emulation mode, not via this driver. Transcribed from the reference "
            "implementation and the official X1 protocol document; not yet confirmed on "
            "hardware.";
        x.supportedTransports = {TransportType::Rs485, TransportType::Mock};
        // One documented line speed for the whole family.
        x.recommendedSerialProfiles = {SerialProfile{9600, SerialParity::None, 8, 1, 1000, 3}};
        x.supportLevel              = DriverSupportLevel::Experimental;
        // Below the sibling PMU driver (10): the two speak the same framing, so both may
        // answer a probe of the same physical device. The margin rule in discovery then
        // forces a manual confirm -- correct behaviour until per-brand fingerprints (device
        // info layout) tell them apart automatically.
        x.probePriority           = 8;
        x.supportsAutoDetection   = true;
        x.supportsMultipleDevices = false;
        x.supportsRead            = true;
        x.supportsWrite           = false;
        x.options                 = {DriverOption{
            "address", "Assigned bus address",
            "Address handed to the inverter at registration (reference default 10 = 0x0A). "
            "Range 1-254.",
            "10",
            {}}};
        return x;
    }();
    return d;
}

std::unique_ptr<InverterDriver> factory(Transport& transport, const DriverOptions& options) {
    return std::make_unique<SolaxDriver>(transport, optionsFrom(options));
}

}  // namespace heliograph::solax
