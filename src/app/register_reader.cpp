// SPDX-License-Identifier: MIT

#include "register_reader.h"

namespace heliograph {

const char* registerReadStatusName(RegisterReadStatus status) {
    switch (status) {
        case RegisterReadStatus::Idle:      return "idle";
        case RegisterReadStatus::Requested: return "requested";
        case RegisterReadStatus::Done:      return "done";
    }
    return "unknown";
}

bool RegisterReader::request(const RegisterReadRequest& request) {
    const bool readFunction = request.functionCode == modbus::kReadHoldingRegisters ||
                              request.functionCode == modbus::kReadInputRegisters;
    if (!readFunction || request.count == 0 || request.count > kMaxRegisterReadCount ||
        request.start + request.count > 0x10000u) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (report_.status == RegisterReadStatus::Requested) {
        return false;
    }
    report_         = RegisterReadReport{};
    report_.status  = RegisterReadStatus::Requested;
    report_.request = request;
    return true;
}

bool RegisterReader::runIfRequested(Transport& transport) {
    RegisterReadRequest request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (report_.status != RegisterReadStatus::Requested) {
            return false;
        }
        request = report_.request;
    }
    // Outside the lock: a silent unit is a full response timeout, and report() has to stay
    // answerable for the page polling it. Nothing else writes report_ meanwhile -- request()
    // refuses while the status is still Requested.
    uint16_t values[kMaxRegisterReadCount] = {};
    const modbus::ReadOutcome outcome =
        modbus::readRegisters(transport, request.unitId, request.functionCode, request.start,
                              request.count, values, kMaxRegisterReadCount);
    std::lock_guard<std::mutex> lock(mutex_);
    report_.outcome = outcome;
    if (outcome.status == modbus::ReadStatus::Ok) {
        report_.values.assign(values, values + request.count);
    }
    report_.finishedMs = clock_ ? clock_() : 0;
    report_.status     = RegisterReadStatus::Done;
    return true;
}

RegisterReadReport RegisterReader::report() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return report_;
}

bool RegisterReader::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return report_.status == RegisterReadStatus::Requested;
}

}  // namespace heliograph
