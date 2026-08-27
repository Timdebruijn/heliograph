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

namespace diag {
class BusTap;
}

/// Which physical medium a transport speaks.
///
/// THREE OF THESE ARE RESERVED, NOT IMPLEMENTED, and that distinction is the whole reason this
/// comment exists -- a reader otherwise cannot tell an unfinished feature from a deliberate one.
///
///   Rs485  the only transport compiled into the firmware.
///   Mock   the host-test double.
///   Tcp    reserved. The profile schema already accepts `transports = ["tcp"]` and the SunSpec
///          driver's own notes explain why it matters: most SunSpec devices in the field are
///          reached over TCP. What is missing is a Modbus TCP *client*, not this enum value.
///   Can    reserved, and not speculative -- the RS485-CAN board carries an isolated CAN
///          interface that nothing in this firmware speaks yet.
///   Rs232  reserved. No hardware here uses it; kept because the set reads as "which medium",
///          and a medium list that omits the obvious sibling invites the question every time.
///
/// Reserved values cost nothing at runtime: an enumerator is a compile-time constant, not code.
/// The cost of leaving them undocumented is that nobody can tell which ones work.
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

    virtual size_t write(const uint8_t* data, size_t len) = 0;

    /// Reads at most `len` bytes, returning as soon as any are available. Returns 0 on
    /// timeout. A short read is normal and not an error: the caller reassembles.
    virtual size_t read(uint8_t* buf, size_t len, uint32_t timeoutMs) = 0;

    /// Milliseconds since boot. Lives on the transport because time is a hardware concern and
    /// the host-testable driver core has no other clock: a driver bounds a whole transaction
    /// against this, so a sustained trickle of bytes (each read short of its own timeout) can
    /// never hold the bus lock indefinitely. See the receive loops in the drivers.
    virtual uint64_t nowMs() const = 0;

    /// Exclusive access to the bus. Exactly one component may talk at a time: a driver's
    /// transaction, a discovery probe, or a passive capture -- all through here, never touching
    /// the UART directly.
    ///
    /// (This used to name a "raw TCP bridge" as one of the users. There has never been one in
    /// this tree; the Modbus TCP server reads the cached register map and never sees a
    /// Transport. Found by review, 2026-08-02, three lines above a facility that was being
    /// extended on the strength of who takes this lock.)
    virtual bool lock(uint32_t timeoutMs) = 0;
    virtual void unlock() = 0;

    virtual const TransportStats& stats() const = 0;

    /// Installs a recorder that sees every byte, with the direction it went. `nullptr` removes
    /// it, which is the normal state.
    ///
    /// Concrete and non-virtual on purpose. A pure virtual would oblige every implementation to
    /// grow a member it has no opinion about; this way an implementation opts in by calling the
    /// two helpers below in its read/write, and one that never does is simply never tapped.
    ///
    /// THE TAP NEVER TAKES THE BUS LOCK. It runs inside a transaction whose caller already holds
    /// it -- taking it again would deadlock, and taking it separately would defeat the whole
    /// point, which is to watch traffic that is happening anyway rather than to stop it.
    ///
    /// Ownership stays with the caller, and so does the lifetime rule: whoever installs a tap
    /// must remove it before the object dies, and must only install from the task that owns the
    /// bus. There is no locking here because there is nothing to lock against -- one task reads
    /// and writes, and the pointer changes only between its own transactions.
    void setTap(diag::BusTap* tap) { tap_ = tap; }
    bool tapped() const { return tap_ != nullptr; }

protected:
    /// Called by implementations from write()/read(). Pass what actually went on the wire, not
    /// what was asked for -- a short write means the tail never left, and a recording that
    /// showed it would be describing a frame the device never saw.
    ///
    /// The null check is inline and the work is not: when nothing is recording, which is almost
    /// always, the cost in the RS485 hot path is one compare against a member already in cache.
    void tapTx(const uint8_t* data, size_t len) {
        if (tap_ != nullptr) {
            tapTxImpl(data, len);
        }
    }
    /// `len == 0` is a normal call and must not be skipped: a read that timed out is how the
    /// recorder learns that time passed, and without it the last record before a quiet stretch
    /// would never be closed.
    void tapRx(const uint8_t* data, size_t len) {
        if (tap_ != nullptr) {
            tapRxImpl(data, len);
        }
    }

private:
    // Out of line so transport.h does not have to include the recorder's header, and so the
    // inline part above stays a single branch.
    void tapTxImpl(const uint8_t* data, size_t len);
    void tapRxImpl(const uint8_t* data, size_t len);

    diag::BusTap* tap_ = nullptr;
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
