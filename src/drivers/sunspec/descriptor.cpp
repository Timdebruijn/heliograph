// SPDX-License-Identifier: MIT

#include "diagnostics/logger.h"
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
            "needed. A device that also publishes model 123 gains an active power limit, "
            "bounded by the scale factor it publishes for it. Not yet confirmed against "
            "physical hardware -- see docs/sunspec.md for which devices are expected to work "
            "and which have actually been tested.";
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
        // The driver has a write path; whether a given DEVICE has one is a separate question,
        // answered by capabilities() after model 123 has been read. This flag is the static
        // claim about the driver, so it says yes -- a UI that hid the control surface here
        // would hide it for the devices that do publish 123 as well.
        x.supportsWrite           = true;
        x.addressOptionKey        = "unit_id";
        x.options                 = {
            DriverOption{"unit_id", "Modbus unit id",
                                         "Slave address of the inverter on the RS485 bus. Range 1-247.", "1", {},
                                         1, 247},
            DriverOption{"base_address", "SunSpec base register",
                                         "Where the 'SunS' marker lives. 40000 covers most devices; 50000 is the "
                                         "other common choice, and some vendors sit elsewhere. Each extra guess "
                                         "would cost a discovery round trip, so this is set rather than searched.",
                                         "40000",
                                         {},
                                         // Matches what optionsFrom() below actually accepts.
                                         // Declaring 1 here made the descriptor stricter than
                                         // its own parser and locked out base 0, which is one
                                         // of the standard SunSpec bases. The marker is two
                                         // registers, so 65534 still fits (review).
                                         0, 65534}};
        return x;
    }();
    return d;
}

std::unique_ptr<InverterDriver> factory(Transport& transport, const DriverOptions& options) {
    (void)transport;
    const DriverDescriptor& d = descriptor();
    SunspecOptions          o;

    // Both ranges come from the descriptor's own DriverOption rows. They used to be restated
    // here as literals, and had already drifted: base_address was accepted up to 0xFFFF here
    // while the declaration says 65534, for the documented reason that the 'SunS' marker is
    // two registers wide. The declaration wins.
    long unit = 0;
    if (d.numericOption(options, "unit_id", unit)) {
        o.unitId = static_cast<uint8_t>(unit);
    } else {
        log::warn("SUNSPEC unit_id '%s' invalid, using %u",
                  d.optionOr(options, "unit_id").c_str(), static_cast<unsigned>(o.unitId));
    }
    long base = 0;
    if (d.numericOption(options, "base_address", base)) {
        o.baseAddress = static_cast<uint16_t>(base);
    } else {
        log::warn("SUNSPEC base_address '%s' invalid, using %u",
                  d.optionOr(options, "base_address").c_str(), static_cast<unsigned>(o.baseAddress));
    }
    return std::make_unique<SunspecDriver>(o);
}

}  // namespace heliograph::sunspec
