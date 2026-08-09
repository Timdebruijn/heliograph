// SPDX-License-Identifier: MIT
//
// SolarMax driver: MaxTalk over RS485, read-only.
//
// The vendor went bankrupt and the monitoring went with it, which is the case this firmware was
// built for. The protocol framing lives in protocols/maxtalk/ and names no brand; everything
// brand-specific -- which query code carries which measurement, and what to divide it by -- is
// here, because that is where brand knowledge belongs.
//
// Read-only by protocol: neither source documents a write or control command, so execute()
// returns Unsupported and the dispatcher's capability gate refuses writes with no special
// handling.
//
// NOT CONFIRMED ON HARDWARE. Nobody on this project owns a SolarMax. The framing is agreed by two
// independent sources; several scaling factors rest on one, and one of them is actively
// contested. See the protocol notes under docs/ for which is which, and readCodes() below for the
// two the driver deliberately does not publish because of it.

#pragma once

#include <memory>

#include "device/device_identity.h"
#include "drivers/inverter_driver.h"
#include "protocols/maxtalk/maxtalk.h"

namespace heliograph::solarmax {

const DriverDescriptor&         descriptor();
std::unique_ptr<InverterDriver> factory(Transport& transport, const DriverOptions& options);

struct SolarmaxOptions {
    /// The inverter's own address, as set in its display menu. There is no registration
    /// handshake in this protocol and nothing is assigned over the wire: the device answers at
    /// whatever address its operator configured, so this has to be told to us rather than
    /// discovered. Several inverters on one bus is the ordinary arrangement.
    uint8_t address = 1;
};

SolarmaxOptions optionsFrom(const heliograph::DriverOptions& values);

class SolarmaxDriver : public InverterDriver {
public:
    explicit SolarmaxDriver(Transport& transport, SolarmaxOptions options = {});

    const DriverDescriptor& descriptor() const override;
    bool                    begin(Transport& transport) override;
    ProbeResult             probe() override;
    PollResult              poll(DeviceState& state) override;
    BusErrorCounts          busErrors() const override;
    DeviceIdentity          identity() const override;
    InverterCapabilities    capabilities() const override;

    /// Always Unsupported: no write operation appears in either source for this protocol.
    CommandResult execute(const InverterCommand& command) override;

private:
    /// Sends one request for `codes` and decodes the reply.
    ///
    /// A single frame carries many codes and gets many answers, so a whole poll is one
    /// transaction rather than one per channel -- which matters on a shared half-duplex bus.
    maxtalk::ParseResult readCodes(const char* const* codes, size_t codeCount,
                                   maxtalk::Reading* out, size_t outCapacity, size_t& outCount);

    /// Declares the channels this driver can fill. Idempotent; called once per poll so a device
    /// that starts answering a code mid-run is picked up without a restart.
    void declareChannels(DeviceState& state) const;

    Transport*      transport_ = nullptr;
    SolarmaxOptions options_;

    BusErrorCounts       busErrors_;
    DeviceIdentity       identity_;
    InverterCapabilities capabilities_;
    bool                 identified_ = false;
};

}  // namespace heliograph::solarmax
