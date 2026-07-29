// SPDX-License-Identifier: MIT

#include "diagnostics.h"

namespace heliograph {

void Diagnostics::setLastError(const std::string& message) {
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastError_ = message;
}

DiagnosticsSnapshot Diagnostics::snapshot() const {
    DiagnosticsSnapshot s;
    s.pollSuccessTotal        = pollSuccessTotal_.load(std::memory_order_relaxed);
    s.pollDurationCount       = pollDurationCount_.load(std::memory_order_relaxed);
    s.pollDurationLastMs      = pollDurationLastMs_.load(std::memory_order_relaxed);
    s.pollDurationMinMs       = pollDurationMinMs_.load(std::memory_order_relaxed);
    s.pollDurationMaxMs       = pollDurationMaxMs_.load(std::memory_order_relaxed);
    s.pollDurationEwmaMs      = pollDurationEwmaMs_.load(std::memory_order_relaxed);
    s.pollFailureTotal        = pollFailureTotal_.load(std::memory_order_relaxed);
    s.consecutivePollFailures = consecutivePollFailures_.load(std::memory_order_relaxed);
    s.checksumErrorTotal      = checksumErrorTotal_.load(std::memory_order_relaxed);
    s.rs485TimeoutTotal       = rs485TimeoutTotal_.load(std::memory_order_relaxed);
    s.invalidFrameTotal       = invalidFrameTotal_.load(std::memory_order_relaxed);
    s.wifiReconnectTotal      = wifiReconnectTotal_.load(std::memory_order_relaxed);
    s.mqttReconnectTotal      = mqttReconnectTotal_.load(std::memory_order_relaxed);
    s.modbusClientConnections = modbusClientConnections_.load(std::memory_order_relaxed);
    s.restRequestTotal        = restRequestTotal_.load(std::memory_order_relaxed);
    s.mqttPublishFailureTotal = mqttPublishFailureTotal_.load(std::memory_order_relaxed);
    s.lastSuccessfulPollMs    = lastSuccessfulPollMs_.load(std::memory_order_relaxed);
    s.rs485StackFreeBytes     = rs485StackFreeBytes_.load(std::memory_order_relaxed);
    s.loopStackFreeBytes      = loopStackFreeBytes_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        s.lastError = lastError_;
    }
    return s;
}

}  // namespace heliograph
