// SPDX-License-Identifier: MIT
//
// Transport abstraction: bytes in, bytes out, timeouts and bus locking.
//
// The transport knows nothing about framing. It cannot: the same RS485 line will later carry
// Modbus RTU and other protocols whose frame boundaries are found in completely different
// ways. Deciding when a frame is complete is the driver's job.

#pragma once

#include <cstddef>
#include <cstdint>

#include "serial_profile.h"

namespace heliograph {

enum class TransportType : uint8_t { Rs485, Rs232, Can, Tcp, Mock };

const char* transportTypeName(TransportType type);

struct TransportStats {
    uint32_t bytesWritten   = 0;
    uint32_t bytesRead      = 0;
    uint32_t readTimeouts   = 0;
    uint32_t writeErrors    = 0;
    uint32_t lockTimeouts   = 0;
};

class Transport {
public:
    virtual ~Transport() = default;

    virtual TransportType type() const = 0;

    /// Applies line settings. May be called again to switch profiles during discovery.
    virtual bool configure(const SerialProfile& profile) = 0;

    /// Discards anything already buffered. Called before a request so that a late reply to a
    /// previous request cannot be mistaken for the answer to this one.
    virtual void flushInput() = 0;

    virtual size_t write(const uint8_t* data, size_t len) = 0;

    /// Reads at most `len` bytes, returning as soon as any are available. Returns 0 on
    /// timeout. A short read is normal and not an error: the caller reassembles.
    virtual size_t read(uint8_t* buf, size_t len, uint32_t timeoutMs) = 0;

    /// Milliseconds since boot. Lives on the transport because time is a hardware concern and
    /// the host-testable driver core has no other clock: a driver bounds a whole transaction
    /// against this, so a sustained trickle of bytes (each read short of its own timeout) can
    /// never hold the bus lock indefinitely. See the receive loops in the drivers.
    virtual uint64_t nowMs() const = 0;

    /// Exclusive access to the bus. Exactly one component may talk at a time; the raw TCP
    /// bridge and discovery both go through this rather than touching the UART directly.
    virtual bool lock(uint32_t timeoutMs) = 0;
    virtual void unlock() = 0;

    virtual const TransportStats& stats() const = 0;
};

/// RAII helper for Transport::lock/unlock.
class TransportLock {
public:
    TransportLock(Transport& transport, uint32_t timeoutMs)
        : transport_(transport), held_(transport.lock(timeoutMs)) {}
    ~TransportLock() {
        if (held_) {
            transport_.unlock();
        }
    }
    TransportLock(const TransportLock&)            = delete;
    TransportLock& operator=(const TransportLock&) = delete;

    bool held() const { return held_; }

private:
    Transport& transport_;
    bool       held_;
};

}  // namespace heliograph
