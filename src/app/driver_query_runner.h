// SPDX-License-Identifier: MIT
//
// Runs one of a driver's diagnostic queries on request (see drivers/diagnostic_query.h): the
// way to ask a device something the poll loop never asks, and read the raw answer.
//
// Same handover as RegisterReader: the web task requests, rs485Task runs it in place of one
// poll because that task owns the bus, and the report is read under a mutex. Never run from
// the HTTP handler.
//
// Reads only, by construction: the driver's query list is the whole vocabulary and nothing here
// takes data to send. So, like RegisterReader, it is not under security.read_only_mode.
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "drivers/diagnostic_query.h"
#include "drivers/inverter_driver.h"

namespace heliograph {

enum class DriverQueryStatus : uint8_t {
    Idle,       ///< nothing requested since boot
    Requested,  ///< waiting for rs485Task
    Done,       ///< ran; `result` says what the device answered
};

const char* driverQueryStatusName(DriverQueryStatus status);

struct DriverQueryReport {
    DriverQueryStatus     status = DriverQueryStatus::Idle;
    std::string           deviceId;
    std::string           query;
    bool                  deviceFound = true;  ///< false: the id matched no configured device
    DiagnosticQueryResult result;
};

/// Finds the driver of a configured device, or nullptr. Supplied by the caller that owns the
/// devices, so the runner needs no knowledge of how they are stored.
using DriverLookup = std::function<InverterDriver*(const std::string& deviceId)>;

/// What a configured device offers, for the page and for validating a request. Only devices
/// whose driver lists at least one query appear.
struct DriverQueryCatalogEntry {
    std::string                  deviceId;
    std::vector<DiagnosticQuery> queries;
};

class DriverQueryRunner {
public:
    /// Web task. False when one is already waiting.
    bool request(const std::string& deviceId, const std::string& query);

    /// rs485Task. Runs a waiting query and returns true if it did. Refuses, without touching
    /// the bus, a device id that is not configured or a name the driver does not list.
    bool runIfRequested(const DriverLookup& lookup);

    DriverQueryReport report() const;
    bool              busy() const;

private:
    mutable std::mutex mutex_;
    DriverQueryReport  report_;
};

}  // namespace heliograph
