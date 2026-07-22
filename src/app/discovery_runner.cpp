// SPDX-License-Identifier: MIT

#include "discovery_runner.h"

namespace heliograph {

const char* discoveryStatusName(DiscoveryStatus status) {
    switch (status) {
        case DiscoveryStatus::Idle:      return "idle";
        case DiscoveryStatus::Requested: return "requested";
        case DiscoveryStatus::Running:   return "running";
        case DiscoveryStatus::Done:      return "done";
        case DiscoveryStatus::Failed:    return "failed";
    }
    return "unknown";
}

DiscoveryRunner::DiscoveryRunner(const DriverRegistry& registry, ClockFn clock)
    : registry_(registry), clock_(std::move(clock)) {}

bool DiscoveryRunner::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return report_.status == DiscoveryStatus::Requested ||
           report_.status == DiscoveryStatus::Running;
}

bool DiscoveryRunner::request(bool extended) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (report_.status == DiscoveryStatus::Requested ||
        report_.status == DiscoveryStatus::Running) {
        return false;
    }
    // A new run discards the previous result rather than merging: the bus may have changed,
    // and a report that is half old and half new is worse than either.
    report_             = DiscoveryReport{};
    report_.status      = DiscoveryStatus::Requested;
    report_.mode        = extended ? DiscoveryMode::Extended : DiscoveryMode::Quick;
    report_.requestedMs = clock_ ? clock_() : 0;
    return true;
}

bool DiscoveryRunner::runIfRequested(Transport& transport) {
    DiscoveryMode mode;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (report_.status != DiscoveryStatus::Requested) {
            return false;
        }
        report_.status    = DiscoveryStatus::Running;
        report_.startedMs = clock_ ? clock_() : 0;
        mode              = report_.mode;
    }

    // The engine runs outside the lock: it takes seconds, and report() must stay answerable
    // for the web UI polling for progress the whole time.
    DiscoveryEngine engine(registry_, transport);
    DiscoveryOutcome outcome = engine.run(mode);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        report_.outcome    = std::move(outcome);
        report_.status     = DiscoveryStatus::Done;
        report_.finishedMs = clock_ ? clock_() : 0;
    }
    return true;
}

DiscoveryReport DiscoveryRunner::report() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return report_;
}

}  // namespace heliograph
