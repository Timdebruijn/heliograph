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

/// What the wire did, counted per transaction rather than per poll.
///
/// The bus counters used to be derived from the PollResult, and a poll is a verdict over
/// several transactions: any driver that reads more than one block reports Ok as soon as one of
/// them decodes. A bus corrupting a third of its frames therefore polled Ok almost every time
/// and moved the checksum counter almost never -- so the one metric that indicts the cabling
/// caught a bus that had already failed, and stayed flat on a bus that was degrading. That is
/// backwards for the thing an early warning is for.
///
/// Cumulative and monotonic for the driver's lifetime. DeviceContext takes the difference
/// across each poll, so a driver only has to count; it never has to know what the caller
/// already saw. Unsigned arithmetic makes the difference correct across a wrap too.
struct BusErrorCounts {
    /// Bytes came back corrupted: the wire. See docs/rs485-bus.md.
    uint32_t checksumErrors = 0;
    /// A read that got no answer at all.
    uint32_t timeouts       = 0;
    /// An intact frame that was not the one asked for: addressing, or a device quirk.
    uint32_t invalidFrames  = 0;
};

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

    /// Frame-level tallies for the metrics. Pure virtual on purpose: a default returning zero
    /// would let a new driver ship with a permanently flat checksum counter and nothing to
    /// notice, which is the failure mode this whole mechanism exists to remove.
    ///
    /// Errors seen during probe() count too -- but discovery runs before a DeviceContext
    /// exists, and the context takes its baseline at construction, so probe traffic never
    /// lands in the metric. The wizard reports on its own probes.
    virtual BusErrorCounts busErrors() const = 0;

    virtual DeviceIdentity       identity() const     = 0;
    virtual InverterCapabilities capabilities() const = 0;

    /// Returns CommandResult::Unsupported for a read-only driver.
    virtual CommandResult execute(const InverterCommand& command) = 0;
};

}  // namespace heliograph
