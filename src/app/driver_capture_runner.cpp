// SPDX-License-Identifier: MIT

#include "driver_capture_runner.h"

#include <utility>

namespace heliograph {

const char* driverCaptureStatusName(DriverCaptureStatus status) {
    switch (status) {
        case DriverCaptureStatus::Idle:      return "idle";
        case DriverCaptureStatus::Requested: return "requested";
        case DriverCaptureStatus::Running:   return "running";
        case DriverCaptureStatus::Done:      return "done";
    }
    return "unknown";
}

DriverCaptureRunner::DriverCaptureRunner(ClockFn clock) : clock_(std::move(clock)) {}

bool DriverCaptureRunner::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return report_.status == DriverCaptureStatus::Requested ||
           report_.status == DriverCaptureStatus::Running;
}

bool DriverCaptureRunner::request(const diag::TapConfig& config, const SerialProfile& profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (report_.status == DriverCaptureStatus::Requested ||
        report_.status == DriverCaptureStatus::Running) {
        return false;
    }
    // Discarded rather than appended to, same rule as the passive capture: two runs in one
    // report would mean the counts describe two different stretches of time.
    report_             = DriverCaptureReport{};
    report_.status      = DriverCaptureStatus::Requested;
    report_.config      = config;
    report_.profile     = profile;
    report_.requestedMs = clock_ ? clock_() : 0;
    return true;
}

DriverCaptureService DriverCaptureRunner::service(Transport& transport) {
    DriverCaptureStatus status = DriverCaptureStatus::Idle;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status = report_.status;
    }

    if (status == DriverCaptureStatus::Requested) {
        diag::TapConfig config;
        SerialProfile   profile;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            config  = report_.config;
            profile = report_.profile;
        }
        // Allocated on arming and released when collected, rather than held for the life of the
        // bridge: this is up to 12 KB, and the one board with no PSRAM has to fit it alongside
        // the JSON copy of it.
        tap_ = std::make_unique<diag::BusTap>(config, profile);
        tap_->begin(transport.nowMs());
        transport.setTap(tap_.get());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            report_.status    = DriverCaptureStatus::Running;
            report_.startedMs = clock_ ? clock_() : 0;
        }
        return DriverCaptureService::Armed;
    }

    if (status != DriverCaptureStatus::Running) {
        return DriverCaptureService::Idle;
    }
    if (tap_ == nullptr) {
        // Cannot happen through this class's own transitions, and is handled rather than
        // asserted because the alternative is a Running capture nobody can ever end.
        std::lock_guard<std::mutex> lock(mutex_);
        report_.status     = DriverCaptureStatus::Done;
        report_.finishedMs = clock_ ? clock_() : 0;
        return DriverCaptureService::Collected;
    }
    if (!tap_->done(transport.nowMs())) {
        return DriverCaptureService::Recording;
    }

    // Unhook FIRST. Everything after this point is reading a recorder nothing is feeding any
    // more, which is what lets the frames be copied out without a lock on the byte path.
    transport.setTap(nullptr);
    tap_->finish(transport.nowMs());
    collect(clock_ ? clock_() : 0);
    return DriverCaptureService::Collected;
}

void DriverCaptureRunner::collect(uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    report_.frames       = tap_->frames();
    report_.totalBytes   = tap_->totalBytes();
    report_.txFrames     = tap_->txFrames();
    report_.rxFrames     = tap_->rxFrames();
    report_.modbusFrames = tap_->modbusFrames();
    report_.pmuFrames    = tap_->pmuFrames();
    report_.truncated    = tap_->truncated();
    report_.idleGapMs    = tap_->idleGapMs();
    report_.status       = DriverCaptureStatus::Done;
    report_.finishedMs   = nowMs;
    tap_.reset();
}

DriverCaptureReport DriverCaptureRunner::report() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return report_;
}

}  // namespace heliograph
