// SPDX-License-Identifier: MIT
//
// Generic SunSpec Modbus driver.
//
// Unlike every other driver here, this one carries no register map. SunSpec devices describe
// their own layout at runtime: a "SunS" marker, then a chain of {model id, length, payload}
// blocks ending in 0xFFFF. The driver walks that chain, records everything it finds, and reads
// the models it understands. What each standard model contains is in sunspec_parser.
//
// The WHOLE chain is mapped, not just the first model this driver can use. Two reasons: a
// device that turns out to be unsupported can then be reported precisely ("it offers models
// 1, 103, 160, 802" rather than "it did not work"), and that list is what tells us which
// models are worth implementing next. It is the difference between a bug report we can act on
// and one we cannot.
//
// Defensive by design, because vendors implement SunSpec with quirks: the base address is
// configurable, a chain that never terminates is bounded rather than followed forever, and a
// model whose length runs past what the device will actually serve stops the walk instead of
// failing the whole device.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "drivers/inverter_driver.h"
#include "drivers/modbus_bus_tally.h"
#include "drivers/sunspec/sunspec_parser.h"
#include "protocols/modbus/modbus_client.h"

namespace heliograph::sunspec {

/// The conventional SunSpec base. Configurable because 50000 is also in use, and some devices
/// sit elsewhere entirely -- guessing costs a discovery round trip per guess, so the owner
/// says where to look rather than the driver trying everything.
inline constexpr uint16_t kDefaultBaseAddress = 40000;

/// A device offering more blocks than this is not a device we understand; the ceiling stops a
/// corrupt or hostile chain from being followed indefinitely.
inline constexpr size_t kMaxChainEntries = 32;

struct SunspecOptions {
    uint8_t  unitId      = 1;
    uint16_t baseAddress = kDefaultBaseAddress;
};

/// One block on the chain, as advertised by the device.
struct ChainEntry {
    uint16_t modelId = 0;
    uint16_t length  = 0;   ///< payload registers, excluding the two header registers
    uint16_t address = 0;   ///< register address of the model id itself
};

class SunspecDriver final : public InverterDriver {
public:
    explicit SunspecDriver(SunspecOptions options) : options_(options) {}

    const DriverDescriptor& descriptor() const override;
    bool                    begin(Transport& transport) override;
    ProbeResult             probe() override;
    PollResult              poll(DeviceState& state) override;
    DeviceIdentity          identity() const override { return identity_; }
    InverterCapabilities    capabilities() const override;
    CommandResult           execute(const InverterCommand& command) override;
    BusErrorCounts          busErrors() const override { return busErrors_; }

    /// Everything the device advertised, in chain order. Empty until a successful walk.
    const std::vector<ChainEntry>& chain() const { return chain_; }

private:
    /// Reads the marker and walks the chain. Fills chain_, inverterEntry_ and commonEntry_.
    /// Maps the model chain. On failure `outFailure` says WHY, so poll() can report a corrupt
    /// wire differently from a silent one -- collapsing both into Timeout hid CRC errors from
    /// the one counter the alerting rules watch.
    bool walkChain(PollResult& outFailure);
    /// Reads one model's payload into `out`, chunked to respect the Modbus read ceiling.
    bool readModel(const ChainEntry& entry, std::vector<uint16_t>& out, PollResult& outFailure);
    modbus::ReadOutcome read(uint16_t address, uint16_t count, uint16_t* out, uint16_t capacity);

    /// Reads model 123 and refreshes controls_. Returns false when the device has no controls
    /// block or the read failed; the caller decides whether that is fatal, because a device
    /// that reads fine and simply offers no controls is not a broken device.
    bool refreshControls();
    /// One control register, written and its echo verified. Refuses before touching the bus
    /// when there is no controls block to write into.
    CommandResult writeControl(size_t pointOffset, uint16_t value);

    Transport*     transport_ = nullptr;
    SunspecOptions options_;
    BusErrorCounts busErrors_{};

    std::vector<ChainEntry> chain_;
    const ChainEntry*       inverterEntry_ = nullptr;  ///< into chain_
    const ChainEntry*       commonEntry_   = nullptr;
    const ChainEntry*       controlsEntry_ = nullptr;
    DeviceIdentity          identity_;
    bool                    walked_ = false;

    /// Last read of model 123, and whether it has ever been read.
    ///
    /// Cached because capabilities() is const and is asked what this device can do before any
    /// caller would think to poll -- and the answer is not a property of the driver, it is a
    /// property of the device in front of it. The bounds of the power limit come from the
    /// device's own scale factor, so until the block has been read there is nothing honest to
    /// advertise and the capability stays absent.
    ControlsReadings controls_{};
    bool             controlsRead_ = false;
};

const DriverDescriptor&         descriptor();
std::unique_ptr<InverterDriver> factory(Transport& transport, const DriverOptions& options);

}  // namespace heliograph::sunspec
