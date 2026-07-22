// SPDX-License-Identifier: MIT
//
// EverSolar / Zeversolar legacy PMU driver.
//
// Protocol knowledge derived from eversolar-monitor (MIT, (c) 2021 Henrik Moller Jorgensen),
// itself based on Steve Cliffe's Eversolar PMU logger. See docs/eversolar-protocol.md.

#pragma once

#include <memory>
#include <string>

#include "drivers/inverter_driver.h"
#include "eversolar_parser.h"
#include "protocols/pmu/pmu_protocol.h"

namespace heliograph::eversolar {

// The AA55 framing this driver was born with now lives in protocols/pmu (hoisted 2026-07-21
// when SolaX X1 turned out to speak the same family protocol). EverSolar is the founding
// member, so its code keeps using the names unqualified.
using namespace ::heliograph::pmu;

const DriverDescriptor& descriptor();
std::unique_ptr<InverterDriver> factory(Transport& transport, const DriverOptions& options);

struct EversolarOptions {
    /// Override the payload layout when the length-based hypothesis does not hold for a
    /// particular device. See docs/eversolar-protocol.md.
    LayoutSelection layout = LayoutSelection::Auto;
    /// Address handed to the first inverter that registers.
    uint8_t assignedAddress = kFirstInverterAddress;
};

/// Maps the generic, string-keyed options from the configuration onto this driver's own
/// settings. The config layer stays free of manufacturer specifics; the translation lives
/// here, where the meaning is known.
EversolarOptions optionsFrom(const heliograph::DriverOptions& values);

class EversolarDriver : public InverterDriver {
public:
    explicit EversolarDriver(Transport& transport, EversolarOptions options = {});

    const DriverDescriptor& descriptor() const override;
    bool                    begin(Transport& transport) override;
    ProbeResult             probe() override;
    PollResult              poll(DeviceState& state) override;
    DeviceIdentity          identity() const override;
    InverterCapabilities    capabilities() const override;

    /// Always Unsupported. Control codes 0x12 (WRITE) and 0x13 (EXECUTE) exist in the frame
    /// format but carry no known function codes, so there is no write operation to implement.
    /// This is a property of the protocol, not an MVP shortcut.
    CommandResult execute(const InverterCommand& command) override;

    bool registered() const { return registered_; }

private:
    enum class TransactResult { Ok, Timeout, ChecksumError, InvalidFrame, TransportError };

    /// Sends a request and waits for its matching response, resynchronising past bus noise
    /// and any echo of our own transmission.
    ///
    /// `altSource`: a second source address to accept. The registration broadcasts need it: a
    /// real TL3000-20 answers the offline query from 00 00 -- it has no address yet, which is
    /// the whole point of the exchange -- while the constructed fixtures assumed 00 <assigned>.
    /// Verified on hardware 2026-07-19. Addressed queries pass nullptr and stay strict.
    TransactResult transact(CommandCode command, Address destination, const uint8_t* data,
                            size_t dataLen, Address expectedSource, uint8_t* payloadOut,
                            size_t payloadCapacity, size_t& payloadLen,
                            const Address* altSource = nullptr);

    /// Broadcast: every inverter forgets its address. Disruptive, so only on begin().
    void reRegisterAll();

    /// Offline query -> assign address -> read id. All read-only from the device's point of
    /// view: the assigned address is volatile and forgotten on power loss.
    ///
    /// `allowReRegister`: when the offline query goes unanswered, broadcast RE_REGISTER and
    /// retry. Only true on a COLD start (bridge boot / discovery), where the inverter may still
    /// hold an address from before and would ignore an offline query forever. During normal
    /// polling it is false: RE_REGISTER knocks a working inverter out of registration, and
    /// sending it every recovery cycle is what left the bridge stuck through sunrise on
    /// 2026-07-21. A returned, address-less inverter answers the plain offline query anyway.
    bool registerDevice(ProbeResult* probeOut, bool allowReRegister);

    void declareChannels(bool dualString);

    Transport*    transport_ = nullptr;
    EversolarOptions options_;

    bool           registered_ = false;
    /// True until the first successful registration after begin(). Gates the one RE_REGISTER
    /// broadcast that a cold start is allowed; cleared for the rest of the session so normal
    /// polling never broadcasts.
    bool coldStartPending_ = true;
    /// Consecutive normal-info timeouts; at kTimeoutsBeforeRecoveryProbe poll() runs a
    /// non-disruptive recovery probe. The registration is NOT dropped on timeouts.
    uint8_t consecutiveTimeouts_ = 0;
    uint8_t        address_    = kFirstInverterAddress;
    DeviceIdentity identity_;
    InverterCapabilities capabilities_;
    bool                 channelsDeclared_ = false;
    bool                 declaredDual_     = false;

    uint32_t checksumErrors_ = 0;
    uint32_t invalidFrames_  = 0;
    uint32_t timeouts_       = 0;

public:
    uint32_t checksumErrors() const { return checksumErrors_; }
    uint32_t invalidFrames() const { return invalidFrames_; }
    uint32_t timeouts() const { return timeouts_; }
};

}  // namespace heliograph::eversolar
