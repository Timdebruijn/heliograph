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
#include <string>

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
    /// Which simulated inverter this is, 1..kMaxDevices.
    ///
    /// The whole reason this exists: identity. Every mock used to report the same hardcoded
    /// serial, so deviceId() -- which prefers the serial -- was identical for every instance,
    /// and a second one configured under `additional_devices` was dropped at boot as a
    /// duplicate. The descriptor claimed supportsMultipleDevices and no two instances could
    /// actually differ in anything, which made that claim empty.
    ///
    /// It also staggers the simulated day, so several instances produce DIFFERENT non-zero
    /// values at the same moment. Without that a fleet of mocks is one curve multiplied by N,
    /// and a per-device bug -- a shared store, a mislabelled Prometheus series, a Modbus unit
    /// off by one -- looks exactly like correct output.
    uint8_t instance = 1;
};

/// Serial number a given instance reports. Instance 1 yields "MOCK-0000000001", byte-identical
/// to the constant every mock used to report -- so an existing single-mock install keeps its
/// device id, its MQTT topics and its Home Assistant entities across this change.
std::string mockSerialNumber(uint8_t instance);

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
    /// The simulation has no bytes, but it does have simulated outcomes: a mock told to fail
    /// its checksum must move the same counter a real driver would, or the metric path is
    /// untestable without hardware.
    BusErrorCounts       busErrors() const override { return busErrors_; }

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
    BusErrorCounts       busErrors_{};
};

}  // namespace heliograph::mock
