// SPDX-License-Identifier: MIT
//
// Records the conversation a working driver is already having, as it happens.
//
// The sibling of FrameCapture, and the answer to the one thing FrameCapture structurally cannot
// do. That one is PASSIVE: the bus owner runs it instead of a poll, so the bridge is silent for
// the whole window. Point it at an inverter this build can already talk to and it records
// nothing at all -- confirmed on hardware, 30 s against a live inverter with a working driver,
// zero frames. The driver that would produce the traffic is the thing being paused.
//
// So this one does not pause anything. It sits on the transport and watches the driver's own
// requests and replies go past. Polling continues throughout; that traffic IS the recording.
//
// WHAT MAKES THIS POSSIBLE, and it is worth stating because it decided the whole design: every
// driver reaches the bus through Transport, and both protocol layers hand a COMPLETE request to
// a single write() call (see pmu_transaction.cpp and modbus_client.cpp). So the transport knows
// which direction each byte went without any driver being asked, and a record can follow the
// transaction instead of guessing at silence.
//
// That second part matters. FrameCapture cuts records on the Modbus t3.5 idle gap, which is
// honest at 9600 and 19200 and approximate above that, because at 38400 the real gap is finer
// than any reader can resolve. Here the primary boundary is the direction turning around, which
// is exact at any baud rate. The idle gap is still used -- a device that answers twice, or a
// driver that reads a reply in pieces, needs it -- but it is no longer the only thing holding
// the framing up, and every record says which rule cut it.
//
// Deliberately protocol-blind, exactly like its sibling: bytes, direction and timing, plus the
// two checksum verdicts (shared with FrameCapture, one source of truth). Interpreting them stays
// with the per-protocol decoders in tools/ and the protocol documents in docs/.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "frame_capture.h"
#include "transport/serial_profile.h"

namespace heliograph::diag {

/// Which way the bytes went, from the bridge's point of view.
enum class BusDirection : uint8_t {
    /// The bridge wrote them. A driver's request.
    Tx,
    /// The bridge read them. A device's reply, or anything else on the bus.
    Rx,
};

const char* busDirectionName(BusDirection direction);

/// Why a record ended. Reported per record rather than inferred, because the reader cannot
/// otherwise tell a boundary the protocol made from one this recorder imposed -- and only the
/// first kind means "these bytes are one frame".
enum class CutReason : uint8_t {
    /// The conversation turned around. Exact at any baud rate; the boundary a driver's own
    /// transaction structure gives you for free.
    DirectionChange,
    /// Silence at least as long as the t3.5 gap. The same rule the passive capture leans on.
    IdleGap,
    /// Neither -- the record hit its byte bound and was split. A cut the protocol did not make,
    /// so the bytes on either side may well belong together. Recording continues after it.
    ByteCap,
    /// The capture's total byte budget ran out here. Also a cut the protocol did not make, and
    /// kept separate from ByteCap because the consequence differs: after ByteCap the recording
    /// carries on, after this one NOTHING further is recorded, however much of the window is
    /// left. Reported per record because "the window closed on it" and "it ran out of room"
    /// mean very different things about the traffic that followed, and the report-level
    /// `truncated` flag cannot say which record it happened to.
    CaptureFull,
    /// The window ended with this record still open.
    WindowEnd,
};

const char* cutReasonName(CutReason reason);

/// One run of bytes in one direction.
struct TappedFrame {
    BusDirection         direction = BusDirection::Tx;
    /// Milliseconds since the tap was armed, at the first byte of this record.
    uint32_t             offsetMs = 0;
    /// Silence before this record. Across a direction change this is the device's response time,
    /// which is protocol evidence in its own right: an inverter that answers in 20 ms behaves
    /// differently from one that takes 200, and a driver's read timeout has to cover the slower.
    uint32_t             gapBeforeMs = 0;
    std::vector<uint8_t> bytes;
    bool                 modbusCrcValid = false;
    bool                 pmuFrameValid  = false;
    CutReason            cutBy = CutReason::WindowEnd;
};

struct TapConfig {
    /// How long to watch. Unlike the passive capture this does not stop the inverter being
    /// polled, so the bound is about the size of the report rather than about downtime.
    uint32_t durationMs    = 30000;
    size_t   maxFrames     = 64;
    size_t   maxFrameBytes = 256;
    /// The bound that actually caps the response, for the same reason it does on CaptureConfig:
    /// maxFrames * maxFrameBytes is a product of two independently chosen limits and therefore a
    /// bound on nothing. 12 KB of bytes is ~36 KB of hex, which the Relay-6CH -- the one board
    /// with no PSRAM to fall back on -- can hold alongside a copy.
    size_t   maxTotalBytes = 12288;
    /// Silence that ends a record when the direction has not changed. Zero means "derive it from
    /// the line", which is what the runner passes; an explicit value exists for tests.
    uint32_t idleGapMs     = 0;
};

/// Accumulates tapped bytes into records.
///
/// Pure, like FrameCapture: no UART, no clock of its own, every time value passed in. It is also
/// NOT internally locked, and that is deliberate -- only the task that owns the bus ever feeds
/// it, so putting a mutex on the per-byte path would be paying for contention that cannot
/// happen. The runner that owns one is what guards the report handed to the web task.
class BusTap {
public:
    BusTap(const TapConfig& config, const SerialProfile& profile);

    /// Marks the start of the window. Every offsetMs is measured from here.
    void begin(uint64_t nowMs);

    /// Bytes the bridge just wrote.
    void recordTx(const uint8_t* data, size_t len, uint64_t nowMs);

    /// Bytes the bridge just read. `len == 0` is a normal and necessary call: a read that timed
    /// out still advances time, and without it the silence that ends the last record before a
    /// quiet stretch would never be observed.
    void recordRx(const uint8_t* data, size_t len, uint64_t nowMs);

    /// Closes the open record, if any, as WindowEnd. Call once when the window ends.
    void finish(uint64_t nowMs);

    /// True when the window has elapsed or a bound is reached.
    bool done(uint64_t nowMs) const;

    /// True when it stopped because it filled up rather than because time ran out. Full means
    /// STOPPED, not "oldest dropped" -- for a handshake the interesting part is at the
    /// beginning, and a ring buffer would reliably throw away exactly that.
    bool truncated() const { return truncated_; }

    const std::vector<TappedFrame>& frames() const { return frames_; }
    uint32_t totalBytes() const { return totalBytes_; }
    uint32_t idleGapMs() const { return gapMs_; }

    /// Records in each direction. A recording with requests and no replies is the single most
    /// informative shape this can produce -- it says the bridge is talking and the device is not
    /// answering, which no passive capture can distinguish from a dead bus.
    uint32_t txFrames() const;
    uint32_t rxFrames() const;
    uint32_t modbusFrames() const;
    uint32_t pmuFrames() const;

private:
    void feed(BusDirection direction, const uint8_t* data, size_t len, uint64_t nowMs);
    void closeFrame(CutReason reason);

    TapConfig                config_;
    uint32_t                 gapMs_      = 0;
    uint64_t                 startMs_    = 0;
    uint64_t                 lastByteMs_ = 0;
    uint32_t                 totalBytes_ = 0;
    bool                     truncated_  = false;
    TappedFrame              open_;
    bool                     openHasBytes_ = false;
    std::vector<TappedFrame> frames_;
};

}  // namespace heliograph::diag
