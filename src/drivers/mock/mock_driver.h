// SPDX-License-Identifier: MIT
//
// Simulated inverter. Two jobs:
//
//  1. Prove the "add a driver, touch no outputs" claim. It therefore models a three-phase
//     hybrid with two MPPTs and a battery -- deliberately unlike the single-phase, no-battery
//     EverSolar. If an output adapter has quietly assumed one phase or no battery, this is
//     what exposes it.
//  2. Let the whole stack run without hardware, including at night.

#pragma once

#include <functional>
#include <memory>

#include "device/clock.h"
#include "drivers/inverter_driver.h"

namespace heliograph::mock {

struct MockOptions {
    /// Grants write capabilities. Used to test that the dispatcher rejects on capability
    /// rather than on driver identity.
    bool writable = false;
    /// Simulates an inverter that has gone off the bus.
    bool offline = false;
    /// Simulates a corrupted reply.
    bool failChecksum = false;
    /// Simulates a device that never answers.
    bool timeout = false;
    /// Length of the simulated solar day.
    ///
    /// Ten minutes, not 24 hours. The curve is driven by uptime, so a realistic day would sit
    /// at midnight for the first six hours after boot -- producing 0 W and making the driver
    /// useless for the demonstrating and testing it exists to do. Configurable via the
    /// "day_length_minutes" driver option.
    uint64_t dayLengthMs = 10ULL * 60 * 1000;
};

/// Maps generic string options onto this driver's settings, like every driver does.
MockOptions optionsFrom(const heliograph::DriverOptions& values, bool writable);

const DriverDescriptor& readOnlyDescriptor();
const DriverDescriptor& writableDescriptor();

std::unique_ptr<InverterDriver> readOnlyFactory(Transport& transport,
                                                const DriverOptions& options);
std::unique_ptr<InverterDriver> writableFactory(Transport& transport,
                                                const DriverOptions& options);

class MockDriver : public InverterDriver {
public:
    MockDriver(ClockFn clock, MockOptions options);

    const DriverDescriptor& descriptor() const override;
    /// Ignores the transport entirely: nothing is simulated at the byte level.
    bool                 begin(Transport& transport) override;
    ProbeResult          probe() override;
    PollResult           poll(DeviceState& state) override;
    DeviceIdentity       identity() const override;
    InverterCapabilities capabilities() const override;
    CommandResult        execute(const InverterCommand& command) override;

    void setOptions(const MockOptions& options) { options_ = options; }
    const MockOptions& options() const { return options_; }

    /// Last value accepted by execute(), so tests can prove a command reached the driver
    /// instead of only that it was not rejected.
    double lastAcceptedValue() const { return lastAcceptedValue_; }
    uint32_t acceptedCommands() const { return acceptedCommands_; }

private:
    ClockFn              clock_;
    MockOptions          options_;
    DeviceIdentity       identity_;
    InverterCapabilities capabilities_;
    double               lastAcceptedValue_ = 0.0;
    uint32_t             acceptedCommands_  = 0;
};

}  // namespace heliograph::mock
