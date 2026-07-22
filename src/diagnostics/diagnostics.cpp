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
    s.pollFailureTotal        = pollFailureTotal_.load(std::memory_order_relaxed);
    s.consecutivePollFailures = consecutivePollFailures_.load(std::memory_order_relaxed);
    s.checksumErrorTotal      = checksumErrorTotal_.load(std::memory_order_relaxed);
    s.rs485TimeoutTotal       = rs485TimeoutTotal_.load(std::memory_order_relaxed);
    s.invalidFrameTotal       = invalidFrameTotal_.load(std::memory_order_relaxed);
    s.wifiReconnectTotal      = wifiReconnectTotal_.load(std::memory_order_relaxed);
    s.mqttReconnectTotal      = mqttReconnectTotal_.load(std::memory_order_relaxed);
    s.modbusClientConnections = modbusClientConnections_.load(std::memory_order_relaxed);
    s.restRequestTotal        = restRequestTotal_.load(std::memory_order_relaxed);
    s.lastSuccessfulPollMs    = lastSuccessfulPollMs_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        s.lastError = lastError_;
    }
    return s;
}

void Diagnostics::reset() {
    pollSuccessTotal_.store(0);
    pollFailureTotal_.store(0);
    consecutivePollFailures_.store(0);
    checksumErrorTotal_.store(0);
    rs485TimeoutTotal_.store(0);
    invalidFrameTotal_.store(0);
    wifiReconnectTotal_.store(0);
    mqttReconnectTotal_.store(0);
    modbusClientConnections_.store(0);
    restRequestTotal_.store(0);
    lastSuccessfulPollMs_.store(0);
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastError_.clear();
}

}  // namespace heliograph
