// SPDX-License-Identifier: MIT

#include "frame_capture.h"

#include <algorithm>

#include "protocols/modbus/modbus_rtu.h"
#include "protocols/pmu/pmu_protocol.h"

namespace heliograph::diag {
namespace {

/// Bits on the wire per character: start + data + optional parity + stop.
uint32_t bitsPerCharacter(const SerialProfile& profile) {
    return 1u + profile.dataBits + (profile.parity == SerialParity::None ? 0u : 1u) +
           profile.stopBits;
}

}  // namespace

bool modbusCrcHolds(const std::vector<uint8_t>& bytes) {
    // Four is the shortest thing that can carry one: unit, function and two CRC bytes. Below
    // that there is nothing to check rather than a check that fails.
    if (bytes.size() < 4) {
        return false;
    }
    const uint16_t expected = modbus::crc16(bytes.data(), bytes.size() - 2);
    // Transmitted low byte first.
    const uint16_t actual =
        static_cast<uint16_t>(bytes[bytes.size() - 2]) |
        static_cast<uint16_t>(static_cast<uint16_t>(bytes[bytes.size() - 1]) << 8);
    return expected == actual;
}

bool pmuFrameHolds(const std::vector<uint8_t>& bytes) {
    pmu::Frame frame;
    return pmu::parseFrame(bytes.data(), bytes.size(), frame) == pmu::ParseResult::Ok;
}

uint32_t idleGapMsFor(const SerialProfile& profile) {
    if (profile.baudRate == 0) {
        return 2;
    }
    // 3.5 characters, in milliseconds, rounded up so the gap is never shorter than the rule.
    const uint64_t bits   = static_cast<uint64_t>(bitsPerCharacter(profile)) * 7u;  // 3.5 * 2
    const uint64_t micros = (bits * 1000000u) / (2u * profile.baudRate);
    const uint32_t ms     = static_cast<uint32_t>((micros + 999u) / 1000u);
    // See the header: below this the reader cannot tell one gap from another, and claiming
    // otherwise would be a resolution we do not have.
    return std::max<uint32_t>(ms, 2);
}

FrameCapture::FrameCapture(const CaptureConfig& config, const SerialProfile& profile)
    : config_(config),
      gapMs_(config.idleGapMs != 0 ? config.idleGapMs : idleGapMsFor(profile)) {
    frames_.reserve(config_.maxFrames);
}

void FrameCapture::begin(uint64_t nowMs) {
    startMs_    = nowMs;
    lastByteMs_ = nowMs;
}

void FrameCapture::feed(const uint8_t* data, size_t len, uint64_t nowMs) {
    // Silence long enough to be a frame boundary closes whatever is open -- checked before the
    // new bytes are appended, so the gap is measured against the previous record and not
    // swallowed into it.
    if (openHasBytes_ && nowMs - lastByteMs_ >= gapMs_) {
        closeFrame();
    }
    if (len == 0 || data == nullptr) {
        return;
    }
    if (frames_.size() >= config_.maxFrames || totalBytes_ >= config_.maxTotalBytes) {
        truncated_ = true;
        return;
    }
    if (!openHasBytes_) {
        open_             = CapturedFrame{};
        open_.offsetMs    = static_cast<uint32_t>(nowMs - startMs_);
        open_.gapBeforeMs = static_cast<uint32_t>(nowMs - lastByteMs_);
        openHasBytes_     = true;
    }
    for (size_t i = 0; i < len; ++i) {
        if (totalBytes_ >= config_.maxTotalBytes) {
            truncated_ = true;
            break;
        }
        if (open_.bytes.size() >= config_.maxFrameBytes) {
            // A record at its byte bound is closed and a new one started rather than dropping
            // the overflow: on a fast line where the gap is unresolvable this is the only thing
            // keeping one giant record from being the whole capture. It is a cut the protocol
            // did not make, which is why gapBeforeMs on the next record will read 0.
            closeFrame();
            if (frames_.size() >= config_.maxFrames) {
                truncated_ = true;
                return;
            }
            open_             = CapturedFrame{};
            open_.offsetMs    = static_cast<uint32_t>(nowMs - startMs_);
            open_.gapBeforeMs = 0;
            openHasBytes_     = true;
        }
        open_.bytes.push_back(data[i]);
        ++totalBytes_;
    }
    lastByteMs_ = nowMs;
}

void FrameCapture::finish(uint64_t nowMs) {
    (void)nowMs;
    if (openHasBytes_) {
        closeFrame();
    }
}

void FrameCapture::closeFrame() {
    if (!openHasBytes_) {
        return;
    }
    // The verdicts are computed once, here, rather than on every read of the report: a report
    // may be fetched repeatedly by a page that is polling, and a CRC over every record each
    // time is work with a fixed answer.
    open_.modbusCrcValid = modbusCrcHolds(open_.bytes);
    open_.pmuFrameValid  = pmuFrameHolds(open_.bytes);
    frames_.push_back(std::move(open_));
    open_         = CapturedFrame{};
    openHasBytes_ = false;
}

bool FrameCapture::done(uint64_t nowMs) const {
    return truncated_ || frames_.size() >= config_.maxFrames ||
           totalBytes_ >= config_.maxTotalBytes || (nowMs - startMs_) >= config_.durationMs;
}

uint32_t FrameCapture::modbusFrames() const {
    return static_cast<uint32_t>(std::count_if(
        frames_.begin(), frames_.end(), [](const CapturedFrame& f) { return f.modbusCrcValid; }));
}

uint32_t FrameCapture::pmuFrames() const {
    return static_cast<uint32_t>(std::count_if(
        frames_.begin(), frames_.end(), [](const CapturedFrame& f) { return f.pmuFrameValid; }));
}

}  // namespace heliograph::diag
