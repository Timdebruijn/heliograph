// SPDX-License-Identifier: MIT

#include "driver_query_runner.h"

#include <utility>

namespace heliograph {

const char* driverQueryStatusName(DriverQueryStatus status) {
    switch (status) {
        case DriverQueryStatus::Idle:      return "idle";
        case DriverQueryStatus::Requested: return "requested";
        case DriverQueryStatus::Done:      return "done";
    }
    return "unknown";
}

bool DriverQueryRunner::request(const std::string& deviceId, const std::string& query) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (report_.status == DriverQueryStatus::Requested) {
        return false;
    }
    report_          = DriverQueryReport{};
    report_.status   = DriverQueryStatus::Requested;
    report_.deviceId = deviceId;
    report_.query    = query;
    return true;
}

bool DriverQueryRunner::runIfRequested(const DriverLookup& lookup) {
    std::string deviceId;
    std::string query;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (report_.status != DriverQueryStatus::Requested) {
            return false;
        }
        deviceId = report_.deviceId;
        query    = report_.query;
    }
    // Outside the lock: a silent device is a full response timeout, and report() has to stay
    // answerable for whoever is polling it. request() refuses while Requested, so nothing else
    // writes report_ meanwhile.
    InverterDriver*       driver = lookup(deviceId);
    DiagnosticQueryResult result;  // UnknownQuery, nothing sent
    if (driver != nullptr) {
        for (const DiagnosticQuery& q : driver->diagnosticQueries()) {
            if (q.name == query) {
                result = driver->runDiagnosticQuery(query);
                break;
            }
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    report_.deviceFound = driver != nullptr;
    report_.result      = std::move(result);
    report_.status      = DriverQueryStatus::Done;
    return true;
}

DriverQueryReport DriverQueryRunner::report() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return report_;
}

bool DriverQueryRunner::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return report_.status == DriverQueryStatus::Requested;
}

}  // namespace heliograph
