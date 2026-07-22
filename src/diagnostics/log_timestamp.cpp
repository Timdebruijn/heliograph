// SPDX-License-Identifier: MIT

#include "log_timestamp.h"

#include <cstdio>

namespace heliograph::log {

size_t formatLogTimestamp(char* buf, size_t n, bool synced, time_t epoch, uint64_t uptimeMs) {
    if (buf == nullptr || n == 0) {
        return 0;
    }
    if (synced) {
        // Wall-clock, or nothing. Never the uptime form: that means "not synced", and showing
        // it for a synced clock that merely did not fit the buffer would read as a regression to
        // 1970. localtime_r honours the process TZ (set by the time manager).
        std::tm tm{};
        if (localtime_r(&epoch, &tm) == nullptr) {
            return 0;
        }
        return std::strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);  // 0 if it did not fit
    }
    const unsigned long long secs   = uptimeMs / 1000;
    const unsigned long long tenths = (uptimeMs % 1000) / 100;
    const int written = std::snprintf(buf, n, "+%llu.%llus", secs, tenths);
    return (written > 0 && static_cast<size_t>(written) < n) ? static_cast<size_t>(written) : 0;
}

size_t formatIsoLocalTime(char* buf, size_t n, time_t epoch) {
    if (buf == nullptr || n == 0) {
        return 0;
    }
    std::tm tm{};
    if (localtime_r(&epoch, &tm) == nullptr) {
        return 0;
    }
    return std::strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);  // 0 if it did not fit
}

}  // namespace heliograph::log
