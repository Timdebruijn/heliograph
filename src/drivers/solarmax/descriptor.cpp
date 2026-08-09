// SPDX-License-Identifier: MIT

#include "solarmax_driver.h"

namespace heliograph::solarmax {

const DriverDescriptor& descriptor() {
    static const DriverDescriptor d = [] {
        DriverDescriptor x;
        x.id           = "solarmax";
        x.displayName  = "SolarMax (MaxTalk RS485)";
        x.manufacturer = "SolarMax";
        x.protocol     = "MaxTalk RS485";
        x.description =
            "SolarMax string inverters over the MaxTalk ASCII protocol on RS485. The vendor is "
            "gone and its monitoring portal with it, so a local reader is the only way these "
            "units report anything. Read-only: neither published source documents a write or "
            "control command. Frame format, checksum and line settings are agreed by two "
            "independent sources; several scaling factors rest on one, and DC voltage is left "
            "unmapped because the two sources disagree about it. Not yet confirmed against "
            "physical hardware -- see the protocol notes under docs/.";
        x.supportedTransports = {TransportType::Rs485, TransportType::Mock};
        // One line setting, given identically by both sources. No sweep is offered because there
        // is nothing to sweep: the protocol does not vary its speed by model.
        x.recommendedSerialProfiles = {SerialProfile{19200, SerialParity::None, 8, 1, 1500}};
        x.supportLevel              = DriverSupportLevel::Experimental;
        // Above the vendor Modbus driver and below the SunSpec one. A MaxTalk frame is a strong
        // fingerprint -- braces, a fixed payload marker and a checksum that has to add up -- but
        // it carries no vendor identifier the way SunSpec's marker does, so it is evidence rather
        // than proof.
        x.probePriority         = 15;
        x.supportsAutoDetection = true;
        // Several units on one bus is the ordinary arrangement here: each carries an address set
        // in its own display menu, and nothing is assigned over the wire. That also makes a probe
        // read-only in the strict sense -- it asks a question and changes no state -- which is
        // what lets discovery treat this family more freely than the AA55 one.
        x.addressOptionKey = "address";
        x.options.push_back(DriverOption{
            "address", "Inverter address",
            "The address set in the inverter's own display menu. Each unit on a shared bus needs "
            "its own; there is no auto-assignment in this protocol, so a duplicate here is a "
            "duplicate on the wire.",
            "1", {}, 0, 255});
        return x;
    }();
    return d;
}

std::unique_ptr<InverterDriver> factory(Transport& transport, const DriverOptions& options) {
    return std::make_unique<SolarmaxDriver>(transport, optionsFrom(options));
}

}  // namespace heliograph::solarmax
