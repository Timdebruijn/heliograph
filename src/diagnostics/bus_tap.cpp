// SPDX-License-Identifier: MIT

#include "bus_tap.h"

#include <algorithm>
#include <utility>

namespace heliograph::diag {

const char* busDirectionName(BusDirection direction) {
    switch (direction) {
        case BusDirection::Tx: return "rx";
        case BusDirection::Rx: return "tx";
    }
    return "unknown";
}

const char* cutReasonName(CutReason reason) {
    switch (reason) {
        case CutReason::DirectionChange: return "direction_change";
        case CutReason::IdleGap:         return "idle_gap";
        case CutReason::ByteCap:         return "byte_cap";
        case CutReason::CaptureFull:     return "capture_full";
        case CutReason::WindowEnd:       return "window_end";
    }
    return "unknown";
}

BusTap::BusTap(const TapConfig& config, const SerialProfile& profile)
    : config_(config),
      gapMs_(config.idleGapMs != 0 ? config.idleGapMs : idleGapMsFor(profile)) {
    frames_.reserve(config_.maxFrames);
}

void BusTap::begin(uint64_t nowMs) {
    startMs_    = nowMs;
    lastByteMs_ = nowMs;
}

void BusTap::recordTx(const uint8_t* data, size_t len, uint64_t nowMs) {
    feed(BusDirection::Tx, data, len, nowMs);
}

void BusTap::recordRx(const uint8_t* data, size_t len, uint64_t nowMs) {
    feed(BusDirection::Rx, data, len, nowMs);
}

void BusTap::feed(BusDirection direction, const uint8_t* data, size_t len, uint64_t nowMs) {
    // The window is a window. Unlike the passive capture -- which sits in a loop it controls and
    // simply stops looping -- this one is fed by whatever the driver happens to be doing, and
    // the runner can only unhook it on its next visit to the bus task. Without this guard a
    // 30 s capture would quietly keep recording through the poll that straddles the deadline.
    // finish() still closes whatever was open, as WindowEnd.
    if (nowMs - startMs_ >= config_.durationMs) {
        return;
    }
    if (len == 0 || data == nullptr) {
        // A read that timed out. It carries no direction -- nothing went on the wire -- so it
        // may only close an open record on silence, never on a direction change. Treating an
        // empty read as "the conversation turned around" would cut a reply in half every time
        // the driver's read loop came back empty mid-transaction.
        if (openHasBytes_ && nowMs - lastByteMs_ >= gapMs_) {
            closeFrame(CutReason::IdleGap);
            // Not advanced on the silent path: the next real byte's gapBeforeMs has to measure
            // from the last BYTE, not from the last time anyone looked.
        }
        return;
    }

    if (openHasBytes_) {
        // Direction is checked FIRST, and the order is the point. When a device answers after a
        // long pause both rules fire, and only one of them is the real boundary: the turn-around
        // is exact, the gap is a threshold that happens to have been crossed. Reporting IdleGap
        // there would understate what is known about the cut.
        if (open_.direction != direction) {
            closeFrame(CutReason::DirectionChange);
        } else if (nowMs - lastByteMs_ >= gapMs_) {
            closeFrame(CutReason::IdleGap);
        }
    }

    if (frames_.size() >= config_.maxFrames || totalBytes_ >= config_.maxTotalBytes) {
        truncated_ = true;
        return;
    }
    if (!openHasBytes_) {
        open_             = TappedFrame{};
        open_.direction   = direction;
        open_.offsetMs    = static_cast<uint32_t>(nowMs - startMs_);
        open_.gapBeforeMs = static_cast<uint32_t>(nowMs - lastByteMs_);
        openHasBytes_     = true;
    }
    for (size_t i = 0; i < len; ++i) {
        if (totalBytes_ >= config_.maxTotalBytes) {
            break;
        }
        if (open_.bytes.size() >= config_.maxFrameBytes) {
            closeFrame(CutReason::ByteCap);
            if (frames_.size() >= config_.maxFrames) {
                truncated_ = true;
                return;
            }
            open_           = TappedFrame{};
            open_.direction = direction;
            open_.offsetMs  = static_cast<uint32_t>(nowMs - startMs_);
            // Zero, and honestly so: no time passed at a cut the protocol did not make.
            open_.gapBeforeMs = 0;
            openHasBytes_     = true;
        }
        open_.bytes.push_back(data[i]);
        ++totalBytes_;
    }
    lastByteMs_ = nowMs;

    // Asked AFTER the loop, on the state, rather than flagged inside it -- and that distinction
    // IS the bug this replaced. A flag set only on the loop's early exit misses the write whose
    // last byte lands exactly on the budget: that loop ends normally, so the record stayed open,
    // finish() later called it WindowEnd, and truncated() said false while nothing further could
    // ever be recorded. Every subsequent feed() is refused by the cap check above, so "the
    // budget is gone" is the whole condition, however the loop happened to leave.
    //
    // Closed here rather than left for finish() for the same reason: WindowEnd is a false
    // account when the budget ran out with most of the window still to go, and cut_by exists
    // precisely so a reader does not have to guess at that.
    if (totalBytes_ >= config_.maxTotalBytes) {
        truncated_ = true;
        closeFrame(CutReason::CaptureFull);
    }
}

void BusTap::finish(uint64_t nowMs) {
    (void)nowMs;
    if (openHasBytes_) {
        closeFrame(CutReason::WindowEnd);
    }
}

void BusTap::closeFrame(CutReason reason) {
    if (!openHasBytes_) {
        return;
    }
    // Computed once, here, rather than on every read of the report: a page polling for progress
    // fetches the report repeatedly, and a CRC over every record each time is work with a fixed
    // answer. Shared with FrameCapture so the two recorders cannot disagree about a frame.
    open_.modbusCrcValid = modbusCrcHolds(open_.bytes);
    open_.pmuFrameValid  = pmuFrameHolds(open_.bytes);
    open_.cutBy          = reason;
    frames_.push_back(std::move(open_));
    open_         = TappedFrame{};
    openHasBytes_ = false;
}

bool BusTap::done(uint64_t nowMs) const {
    return truncated_ || frames_.size() >= config_.maxFrames ||
           totalBytes_ >= config_.maxTotalBytes || (nowMs - startMs_) >= config_.durationMs;
}

uint32_t BusTap::txFrames() const {
    return static_cast<uint32_t>(std::count_if(frames_.begin(), frames_.end(), [](const TappedFrame& f) {
        return f.direction == BusDirection::Tx;
    }));
}

uint32_t BusTap::rxFrames() const {
    return static_cast<uint32_t>(std::count_if(frames_.begin(), frames_.end(), [](const TappedFrame& f) {
        return f.direction == BusDirection::Rx;
    }));
}

uint32_t BusTap::modbusFrames() const {
    return static_cast<uint32_t>(std::count_if(
        frames_.begin(), frames_.end(), [](const TappedFrame& f) { return f.modbusCrcValid; }));
}

uint32_t BusTap::pmuFrames() const {
    return static_cast<uint32_t>(std::count_if(
        frames_.begin(), frames_.end(), [](const TappedFrame& f) { return f.pmuFrameValid; }));
}

}  // namespace heliograph::diag
