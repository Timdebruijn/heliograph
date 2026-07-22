// SPDX-License-Identifier: MIT
//
// The driver interface. This is the only place brand-specific code is reachable from, and
// the only component allowed to touch the physical bus.

#pragma once

#include <string>
#include <vector>

#include "device/capability.h"
#include "device/command.h"
#include "device/device_identity.h"
#include "device/device_state.h"
#include "driver_descriptor.h"
#include "transport/transport.h"

namespace heliograph {

struct ProbeResult {
    bool responded      = false;
    bool checksumValid  = false;
    /// 0-100. Only meaningful relative to other drivers probed on the same bus.
    int  confidenceScore = 0;

    std::string detectedManufacturer;
    std::string detectedModel;
    std::string serialNumber;
    std::string firmwareVersion;

    /// Human-readable trail of what was observed, shown verbatim in the discovery wizard.
    /// The user has to be able to judge a match themselves when the score is ambiguous.
    std::vector<std::string> evidence;
};

enum class PollResult : uint8_t {
    Ok,
    Timeout,
    ChecksumError,
    InvalidFrame,
    NotRegistered,
    TransportError,
};

const char* pollResultName(PollResult result);

class InverterDriver {
public:
    virtual ~InverterDriver() = default;

    virtual const DriverDescriptor& descriptor() const = 0;

    virtual bool begin(Transport& transport) = 0;

    /// Read-only identification attempt. Must never write to the device.
    virtual ProbeResult probe() = 0;

    /// Reads the device and fills `state`.
    ///
    /// Contract: `state` may only be modified when returning Ok. On any failure the caller
    /// keeps the previous state, so a partially decoded frame can never surface as data.
    virtual PollResult poll(DeviceState& state) = 0;

    virtual DeviceIdentity       identity() const     = 0;
    virtual InverterCapabilities capabilities() const = 0;

    /// Returns CommandResult::Unsupported for a read-only driver.
    virtual CommandResult execute(const InverterCommand& command) = 0;
};

}  // namespace heliograph
