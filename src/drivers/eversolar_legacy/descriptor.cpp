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
        // False, and not as a limitation of the application: this DRIVER cannot share a bus
        // with a second instance of itself, in three separate ways (#63).
        //
        // begin() broadcasts RE_REGISTER, which tells every inverter on the line to forget its
        // address -- so a second instance starting up de-registers the first, already-polling
        // one. registerDevice() runs the enumeration loop exactly once, so a second inverter is
        // never discovered anyway. And `assignedAddress` has no option wired to it, so both
        // instances would claim the same bus address regardless.
        // True, with one device per inverter and each holding its own bus address.
        //
        // The enumeration needs no loop of its own: a registered inverter ignores the broadcast
        // offline query, so the second instance's query is answered by the next unregistered
        // one. Instances start sequentially from the configuration, which is the ordering that
        // makes that work -- the same mechanism the reference implementation drives from a
        // timer instead (eversolar.pl:1038).
        //
        // NOT verified against two physical inverters; nobody involved has two. See #82 and
        // the pitfalls in docs/eversolar-protocol.md before wiring a second one.
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
            "auto", {"auto", "single", "dual"}},
            DriverOption{
            "address", "Assigned bus address",
            "Address this bridge hands to its inverter at registration. Leave at 16 unless you "
            "have more than one inverter on the same RS485 loop; then give each its own, "
            "counting up (16, 17, 18...). The inverter does not store it -- it is handed out "
            "at registration and forgotten on power loss. Range 16-254.",
            "16",
            {},
            kFirstInverterAddress, 254}};
        // Named the same as the sibling drivers' address option, which is what lets the
        // settings page warn when two configured devices would share one.
        x.addressOptionKey = "address";
        return x;
    }();
    return d;
}

std::unique_ptr<InverterDriver> factory(Transport& transport, const DriverOptions& options) {
    return std::make_unique<EversolarDriver>(transport, optionsFrom(options));
}

}  // namespace heliograph::eversolar
