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

    /// Observes every byte crossing this transport, in either direction.
    ///
    /// Lives here rather than in the drivers, and that is the whole design. A tap in a driver
    /// records only that driver, has to be added again for every driver written afterwards, and
    /// can be forgotten -- and a recording with a driver's own requests silently missing is
    /// worse than no recording, because it reads like a device that answered unprompted.
    /// Here there is no path around it: write() and read() below are not virtual, so an
    /// implementation cannot move bytes without passing through them.
    ///
    /// It also answers "whose byte was that?" for free. The passive capture records one
    /// anonymous stream because it is only listening; here the direction is simply which of the
    /// two calls was made.
    class Tap {
    public:
        enum class Direction : uint8_t { Tx, Rx };
        virtual ~Tap()                                                             = default;
        virtual void onBytes(Direction direction, const uint8_t* data, size_t len) = 0;
    };

    /// Installs or removes the tap. `nullptr` removes it.
    ///
    /// NOT thread-safe, deliberately: it is meant to be called from the task that owns the bus,
    /// which is the same task that does every write and read. The capture path keeps to that --
    /// the web handler only sets a request flag, and rs485Task installs the tap when it picks
    /// the request up. An atomic here would buy safety for a call pattern that must not exist
    /// anyway, and would hide it.
    void setTap(Tap* tap) { tap_ = tap; }
    Tap* tap() const { return tap_; }

    size_t write(const uint8_t* data, size_t len) {
        const size_t written = writeBytes(data, len);
        if (tap_ != nullptr && written > 0) {
            tap_->onBytes(Tap::Direction::Tx, data, written);
        }
        return written;
    }

    /// Reads at most `len` bytes, returning as soon as any are available. Returns 0 on
    /// timeout. A short read is normal and not an error: the caller reassembles.
    size_t read(uint8_t* buf, size_t len, uint32_t timeoutMs) {
        const size_t got = readBytes(buf, len, timeoutMs);
        if (tap_ != nullptr && got > 0) {
            tap_->onBytes(Tap::Direction::Rx, buf, got);
        }
        return got;
    }

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

protected:
    /// What an implementation provides. The public write()/read() above wrap these so the tap
    /// cannot be bypassed; nothing else should call them.
    virtual size_t writeBytes(const uint8_t* data, size_t len)               = 0;
    virtual size_t readBytes(uint8_t* buf, size_t len, uint32_t timeoutMs)   = 0;

private:
    Tap* tap_ = nullptr;
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
