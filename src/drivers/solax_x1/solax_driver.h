// SPDX-License-Identifier: MIT
//
// SolaX X1 series driver (PMU family protocol over RS485). Read-only by protocol: the PMU
// frame format defines no usable write operation, so output curtailment on these units goes
// through the separate meter-emulation mode of the inverter, never through this driver.
//
// Modeled on the EverSolar driver -- same framing (protocols/pmu), same registration
// discipline hardened by the 2026-07-21 sunrise incident: register once, keep the
// registration through timeouts, recover with non-disruptive probes. One deliberate
// difference: no RE_REGISTER broadcast, ever. The reference implementation
// (syssi/esphome-solax-x1-mini) never sends it, and we do not need it either -- the address
// WE assign is deterministic, so a cold start against an inverter that kept its address is
// resolved by simply querying that address directly.
//
// STATUS: Experimental, transcribed from the reference implementation and the official X1
// protocol document; NOT yet confirmed against a real X1 Mini. First hardware target:
// an X1-1.1-S-D. See solax_parser.h for the payload provenance.

#pragma once

#include <memory>

#include "drivers/inverter_driver.h"
#include "drivers/solax_x1/solax_parser.h"

namespace heliograph::solax {

const DriverDescriptor& descriptor();
std::unique_ptr<InverterDriver> factory(Transport& transport, const DriverOptions& options);

struct SolaxOptions {
    /// Address handed to the inverter at registration. The reference implementation uses
    /// 0x0A; kept as an option so a bus shared with another PMU master can avoid a clash.
    uint8_t assignedAddress = 0x0A;
};

SolaxOptions optionsFrom(const heliograph::DriverOptions& values);

class SolaxDriver : public InverterDriver {
public:
    explicit SolaxDriver(Transport& transport, SolaxOptions options = {});

    const DriverDescriptor& descriptor() const override;
    bool                    begin(Transport& transport) override;
    ProbeResult             probe() override;
    PollResult              poll(DeviceState& state) override;
    DeviceIdentity          identity() const override;
    InverterCapabilities    capabilities() const override;

    /// Always Unsupported: no write operation exists in this protocol (see file header).
    CommandResult execute(const InverterCommand& command) override;

    bool registered() const { return registered_; }

private:
    enum class TransactResult { Ok, Timeout, ChecksumError, InvalidFrame, TransportError };

    /// Sends a request and waits for its matching response, resynchronising past bus noise
    /// and echoes. `source` exists because the reference sends the SEND_ADDRESS frame with
    /// source 00 00 instead of the PMU address; everything else passes kPmuAddress.
    TransactResult transact(Address source, CommandCode command, Address destination,
                            const uint8_t* data, size_t dataLen, Address expectedSource,
                            uint8_t* payloadOut, size_t payloadCapacity, size_t& payloadLen,
                            const Address* altSource = nullptr);

    /// Offline query -> assign 0x0A -> ACK (or verify by query) -> read device info.
    /// Non-disruptive: no broadcast exists in this driver at all.
    bool registerDevice(ProbeResult* probeOut);

    /// A status query straight at the assigned address. Recovers the session with an
    /// inverter that kept its address across OUR reboot (it ignores offline queries then).
    bool verifyByStatusQuery();

    void readDeviceInfo(ProbeResult* probeOut);

    Transport*   transport_ = nullptr;
    SolaxOptions options_;

    bool registered_ = false;
    /// Consecutive status-report timeouts; at kTimeoutsBeforeRecoveryProbe poll() runs the
    /// non-disruptive offline-query probe. The registration is NOT dropped on timeouts.
    uint8_t consecutiveTimeouts_ = 0;

    /// The 14 raw serial bytes announced at registration, echoed verbatim in the address
    /// assignment. Raw, not a trimmed string: the echo must be byte-exact.
    uint8_t serial_[kSerialNumberBytes] = {};

    DeviceIdentity       identity_;
    InverterCapabilities capabilities_;
    /// PV2 declared once its voltage proves the second MPPT exists (datasheet says the Mini
    /// has one; the payload carries two). See poll().
    bool pv2Declared_ = false;

    uint32_t checksumErrors_ = 0;
    uint32_t invalidFrames_  = 0;
    uint32_t timeouts_       = 0;

public:
    uint32_t checksumErrors() const { return checksumErrors_; }
    uint32_t invalidFrames() const { return invalidFrames_; }
    uint32_t timeouts() const { return timeouts_; }
};

}  // namespace heliograph::solax
