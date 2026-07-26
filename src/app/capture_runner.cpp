// SPDX-License-Identifier: MIT

#include "capture_runner.h"

#include <array>
#include <utility>

namespace heliograph {

const char* captureStatusName(CaptureStatus status) {
    switch (status) {
        case CaptureStatus::Idle:      return "idle";
        case CaptureStatus::Requested: return "requested";
        case CaptureStatus::Running:   return "running";
        case CaptureStatus::Done:      return "done";
        case CaptureStatus::Failed:    return "failed";
    }
    return "unknown";
}

CaptureRunner::CaptureRunner(ClockFn clock) : clock_(std::move(clock)) {}

bool CaptureRunner::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return report_.status == CaptureStatus::Requested || report_.status == CaptureStatus::Running;
}

bool CaptureRunner::request(const diag::CaptureConfig& config, const SerialProfile& profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (report_.status == CaptureStatus::Requested || report_.status == CaptureStatus::Running) {
        return false;
    }
    // The previous capture is discarded rather than appended to. Two runs at different line
    // settings in one report would be actively misleading -- the whole value of the checksum
    // counts is that they describe ONE guess at the line.
    report_             = CaptureReport{};
    report_.status      = CaptureStatus::Requested;
    report_.config      = config;
    report_.profile     = profile;
    report_.requestedMs = clock_ ? clock_() : 0;
    return true;
}

bool CaptureRunner::runIfRequested(Transport& transport, const std::function<void()>& onTick) {
    diag::CaptureConfig config;
    SerialProfile       profile;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (report_.status != CaptureStatus::Requested) {
            return false;
        }
        report_.status    = CaptureStatus::Running;
        report_.startedMs = clock_ ? clock_() : 0;
        config            = report_.config;
        profile           = report_.profile;
    }

    diag::FrameCapture capture(config, profile);

    // Everything below runs OUTSIDE the lock: it takes as long as the capture window, and
    // report() has to stay answerable the whole time for the page polling for progress.
    {
        // The bus lock, held for the whole window. A capture that shared the line with a poll
        // would record the bridge's own traffic interleaved with the device's and present it as
        // one stream. The timeout is generous but finite: a bus held by a wedged transaction
        // should report "busy", not hang the task.
        TransportLock lock(transport, 2000);
        if (!lock.held()) {
            std::lock_guard<std::mutex> guard(mutex_);
            report_.status     = CaptureStatus::Failed;
            report_.error      = "the RS485 bus was busy";
            report_.finishedMs = clock_ ? clock_() : 0;
            return true;
        }
        if (!transport.configure(profile)) {
            std::lock_guard<std::mutex> guard(mutex_);
            report_.status     = CaptureStatus::Failed;
            report_.error      = "those line settings could not be applied";
            report_.finishedMs = clock_ ? clock_() : 0;
            return true;
        }
        // Anything already buffered predates the capture and belongs to whatever the driver was
        // doing; timestamping it as the first frame would put a lie at offset 0.
        transport.flushInput();

        const uint64_t start = transport.nowMs();
        capture.begin(start);
        std::array<uint8_t, 128> buffer{};
        while (!capture.done(transport.nowMs())) {
            if (onTick) {
                onTick();
            }
            const size_t got =
                transport.read(buffer.data(), buffer.size(), kCaptureReadTimeoutMs);
            // Fed even when nothing arrived: a zero-length feed is what advances time, and
            // without it the silence that ends a record would never be observed. A reader that
            // only fed real bytes would emit one record per burst of traffic and never close
            // the last one.
            capture.feed(buffer.data(), got, transport.nowMs());
        }
        capture.finish(transport.nowMs());
    }

    {
        std::lock_guard<std::mutex> guard(mutex_);
        report_.frames       = capture.frames();
        report_.totalBytes   = capture.totalBytes();
        report_.modbusFrames = capture.modbusFrames();
        report_.pmuFrames    = capture.pmuFrames();
        report_.truncated    = capture.truncated();
        report_.idleGapMs    = capture.idleGapMs();
        report_.status       = CaptureStatus::Done;
        report_.finishedMs   = clock_ ? clock_() : 0;
    }
    return true;
}

CaptureReport CaptureRunner::report() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return report_;
}

}  // namespace heliograph
