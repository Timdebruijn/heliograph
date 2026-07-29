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
        // The only driver left saying no, and unlike the others it is not saying "impossible".
        //
        // The mechanism EverSolar uses looks available here: same AA55 family, same broadcast
        // offline query that a registered inverter ignores, and the same per-row assigned
        // address. This driver has no RE_REGISTER at all, so the specific hazard that makes a
        // second instance dangerous over there -- a starting instance telling the already-polling
        // inverter to forget its address -- does not exist in this one.
        //
        // It stays false because this driver has never exchanged a byte with a real inverter
        // (the descriptor above says so, and the one field session returned nothing). Enabling a
        // second instance would be building an untested path on top of an unverified one. Turn it
        // on when a single X1 is confirmed working, not before.
        x.supportsMultipleDevices = false;
        x.supportsRead            = true;
        x.supportsWrite           = false;
        // Deliberately NOT addressOptionKey. This option is the address the bridge HANDS the
        // inverter at registration, not one the inverter already answers at -- sweeping it would
        // not discover anything, it would assign nine different addresses in a row and leave the
        // device on the last one, while the report named the first (review, 2026-07-26). A PMU
        // device is found by its broadcast offline query, which needs no address at all.
        x.options                 = {DriverOption{
            "address", "Assigned bus address",
            "Address handed to the inverter at registration (reference default 10 = 0x0A). "
            "Range 1-254.",
            "10",
            {},
            1, 254}};
        return x;
    }();
    return d;
}

std::unique_ptr<InverterDriver> factory(Transport& transport, const DriverOptions& options) {
    return std::make_unique<SolaxDriver>(transport, optionsFrom(options));
}

}  // namespace heliograph::solax
