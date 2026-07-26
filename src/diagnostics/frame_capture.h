// SPDX-License-Identifier: MIT
//
// Passive recording of raw RS485 traffic, for a device no driver in this build can identify.
//
// This is the tool the "adding a device" guide asks a contributor to reach for first: watch the
// official dongle or monitoring software talk to the inverter, decode the frames, and only then
// write code. Until now that meant owning a USB-RS485 adapter and a laptop within cable reach
// of the inverter. The bridge is already wired to that bus.
//
// Deliberately protocol-blind. It knows nothing about what the bytes mean and needs no working
// driver -- that is the entire point, because the devices worth capturing are exactly the ones
// nothing here can talk to yet. What it does add is two checksum verdicts per record, which is
// not decoding: it is how you find out whether the line settings you guessed are right.
//
// FRAMING, AND ITS ONE HONEST LIMIT. There is no request/response structure to lean on the way
// a driver has, so records are cut on an idle gap -- the Modbus t3.5 rule, 3.5 character times
// of silence. That works cleanly at 9600 and 19200. At 38400 and above the real gap is under a
// millisecond, which is finer than the read loop can resolve, so adjacent frames may land in
// one record. The byte stream is still complete and in order; only the cut points are
// approximate, and a merged record shows up as a failed checksum rather than as silence. Said
// here, and in the UI, rather than left for a contributor to discover from confusing output.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "transport/serial_profile.h"

namespace heliograph::diag {

/// One run of bytes, bounded by silence on both sides (see the framing note above).
struct CapturedFrame {
    /// Milliseconds since the capture started, at the FIRST byte of this record. Relative
    /// rather than wall-clock: a capture is read as a sequence, and the bridge may well have no
    /// synced clock when someone is standing at it with the cover off.
    uint32_t             offsetMs = 0;
    /// Silence before this record, in milliseconds. Part of the protocol evidence: a device
    /// that answers 20 ms after a request behaves differently from one that takes 200.
    uint32_t             gapBeforeMs = 0;
    std::vector<uint8_t> bytes;
    /// Modbus RTU: the trailing two bytes are a correct CRC-16 over the rest.
    bool                 modbusCrcValid = false;
    /// The AA55 framing family: header, structure and 16-bit sum checksum all hold.
    bool                 pmuFrameValid = false;
};

struct CaptureConfig {
    /// How long to listen. The operator picks this: it has to cover whatever makes the other
    /// device talk, which might be a poll cycle or a button press on a dongle.
    uint32_t durationMs = 30000;
    size_t   maxFrames     = 64;
    size_t   maxFrameBytes = 256;
    /// The bound that actually matters, and the reason it is stated separately from the two
    /// above: those are a PRODUCT, and a product of two independently chosen limits is not a
    /// bound on anything. 256 records of 256 bytes each renders to roughly 220 KB of JSON hex,
    /// which no board here can hold, let alone copy into a response.
    ///
    /// This caps the payload directly, whatever shape the traffic turns out to have. 12 KB of
    /// captured bytes is ~36 KB of hex and a ~40 KB response -- which the Relay-6CH, the one
    /// board with no PSRAM to fall back on, can hold alongside a copy of it.
    ///
    /// Generous for the job: a registration handshake is a handful of frames of tens of bytes,
    /// and a full Modbus response is 255. This is not a logging facility.
    size_t   maxTotalBytes = 12288;
    /// Silence that ends a record. Zero means "derive it from the line", which is what the
    /// REST layer passes; an explicit value exists for tests and for a protocol whose inter-
    /// frame gap is known to differ.
    uint32_t idleGapMs = 0;
};

/// The 3.5-character silence Modbus RTU uses as a frame boundary, in whole milliseconds.
///
/// Floored at 2 ms, and that floor is doing real work: above 19200 baud the true gap is a
/// fraction of a millisecond, finer than anything that polls a UART can observe. Returning the
/// arithmetic answer would claim a resolution the reader does not have; the floor states the
/// resolution it actually has, and the framing note above says what that costs.
uint32_t idleGapMsFor(const SerialProfile& profile);

/// Accumulates bytes into records. Pure: no UART, no clock of its own, every time value passed
/// in. The reading loop lives in CaptureRunner, on the task that owns the bus.
class FrameCapture {
public:
    FrameCapture(const CaptureConfig& config, const SerialProfile& profile);

    /// Marks the start of the capture window. Every offsetMs is measured from here.
    void begin(uint64_t nowMs);

    /// Hands over bytes just read. `len == 0` is a normal and necessary call: it advances time
    /// so that silence can close the open record. A reader that only called this when it had
    /// bytes would never cut the last frame before a long quiet stretch.
    void feed(const uint8_t* data, size_t len, uint64_t nowMs);

    /// Closes the open record, if any. Call once when the window ends.
    void finish(uint64_t nowMs);

    /// True when the window has elapsed or the record bound is reached.
    bool done(uint64_t nowMs) const;

    /// True when the capture stopped because it filled up rather than because time ran out.
    ///
    /// Full means STOPPED, not "oldest dropped". For a handshake -- the case this exists for --
    /// the interesting part is the registration dance at the beginning, and a ring buffer would
    /// reliably throw away exactly that.
    bool truncated() const { return truncated_; }

    const std::vector<CapturedFrame>& frames() const { return frames_; }
    uint32_t totalBytes() const { return totalBytes_; }
    /// The gap actually in force, derived or explicit. Reported rather than recomputed by the
    /// caller: an explicit config value overrides the derivation, and a report that recomputed
    /// it would describe a rule the capture did not follow.
    uint32_t idleGapMs() const { return gapMs_; }
    /// Records whose Modbus CRC held. The single most useful number in the report: a capture at
    /// the wrong baud rate produces plenty of bytes and zero valid checksums.
    uint32_t modbusFrames() const;
    uint32_t pmuFrames() const;

private:
    void closeFrame();

    CaptureConfig        config_;
    uint32_t             gapMs_ = 0;
    uint64_t             startMs_ = 0;
    uint64_t             lastByteMs_ = 0;
    uint32_t             totalBytes_ = 0;
    bool                 truncated_ = false;
    CapturedFrame        open_;
    bool                 openHasBytes_ = false;
    std::vector<CapturedFrame> frames_;
};

}  // namespace heliograph::diag
