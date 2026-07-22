// SPDX-License-Identifier: MIT
//
// Counters shared across tasks. Atomics rather than a mutex: every writer does a single
// increment, and diagnostics must never be able to block the poll loop.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace heliograph {

struct DiagnosticsSnapshot {
    uint32_t pollSuccessTotal          = 0;
    uint32_t pollFailureTotal          = 0;
    uint32_t consecutivePollFailures   = 0;
    uint32_t checksumErrorTotal        = 0;
    uint32_t rs485TimeoutTotal         = 0;
    uint32_t invalidFrameTotal         = 0;
    uint32_t wifiReconnectTotal        = 0;
    uint32_t mqttReconnectTotal        = 0;
    uint32_t modbusClientConnections   = 0;
    uint32_t restRequestTotal          = 0;
    uint64_t lastSuccessfulPollMs      = 0;
    std::string lastError;
};

class Diagnostics {
public:
    void recordPollSuccess(uint64_t nowMs) {
        pollSuccessTotal_.fetch_add(1, std::memory_order_relaxed);
        consecutivePollFailures_.store(0, std::memory_order_relaxed);
        lastSuccessfulPollMs_.store(nowMs, std::memory_order_relaxed);
    }
    void recordPollFailure() {
        pollFailureTotal_.fetch_add(1, std::memory_order_relaxed);
        consecutivePollFailures_.fetch_add(1, std::memory_order_relaxed);
    }
    void recordChecksumError() { checksumErrorTotal_.fetch_add(1, std::memory_order_relaxed); }
    void recordTimeout() { rs485TimeoutTotal_.fetch_add(1, std::memory_order_relaxed); }
    void recordInvalidFrame() { invalidFrameTotal_.fetch_add(1, std::memory_order_relaxed); }
    void recordWifiReconnect() { wifiReconnectTotal_.fetch_add(1, std::memory_order_relaxed); }
    void recordMqttReconnect() { mqttReconnectTotal_.fetch_add(1, std::memory_order_relaxed); }
    void recordModbusClient() { modbusClientConnections_.fetch_add(1, std::memory_order_relaxed); }
    void recordRestRequest() { restRequestTotal_.fetch_add(1, std::memory_order_relaxed); }

    /// Cheap atomic read for hot-path callers (the boot-confirm check runs every loop()
    /// iteration; a full snapshot() would copy a std::string each time).
    uint32_t pollSuccessTotal() const {
        return pollSuccessTotal_.load(std::memory_order_relaxed);
    }

    /// Must never contain a secret. Callers pass driver/transport level messages only.
    void setLastError(const std::string& message);

    DiagnosticsSnapshot snapshot() const;
    void                reset();

private:
    std::atomic<uint32_t> pollSuccessTotal_{0};
    std::atomic<uint32_t> pollFailureTotal_{0};
    std::atomic<uint32_t> consecutivePollFailures_{0};
    std::atomic<uint32_t> checksumErrorTotal_{0};
    std::atomic<uint32_t> rs485TimeoutTotal_{0};
    std::atomic<uint32_t> invalidFrameTotal_{0};
    std::atomic<uint32_t> wifiReconnectTotal_{0};
    std::atomic<uint32_t> mqttReconnectTotal_{0};
    std::atomic<uint32_t> modbusClientConnections_{0};
    std::atomic<uint32_t> restRequestTotal_{0};
    std::atomic<uint64_t> lastSuccessfulPollMs_{0};

    mutable std::mutex errorMutex_;
    std::string        lastError_;
};

}  // namespace heliograph
