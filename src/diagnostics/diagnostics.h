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
    /// Duration of SUCCESSFUL polls only, in ms since the poll started. A failed poll lasts
    /// the transaction deadline by construction -- a constant -- and this fleet's inverter is
    /// dark every night, so counting failures would peg max at the deadline and drag the EWMA
    /// toward it a few thousand times per night. Failures are already counted above; what this
    /// answers is "how long does a poll take when the bus works", which is the number that
    /// moves when something else (web traffic, a second core user) is in the way.
    /// pollDurationCount is the presence signal: the mock driver polls in 0 ms, so a zero
    /// duration is a real sample and cannot double as "no data".
    uint32_t pollDurationCount         = 0;
    uint32_t pollDurationLastMs        = 0;
    uint32_t pollDurationMinMs         = 0;
    uint32_t pollDurationMaxMs         = 0;
    /// EWMA with alpha 1/8, integer arithmetic, seeded by the first sample. Chosen over a mean
    /// so a load change shows within ~a minute at a 10 s poll interval instead of being
    /// averaged into days of history.
    uint32_t pollDurationEwmaMs        = 0;
    uint32_t pollFailureTotal          = 0;
    uint32_t consecutivePollFailures   = 0;
    uint32_t checksumErrorTotal        = 0;
    uint32_t rs485TimeoutTotal         = 0;
    uint32_t invalidFrameTotal         = 0;
    uint32_t wifiReconnectTotal        = 0;
    uint32_t mqttReconnectTotal        = 0;
    uint32_t modbusClientConnections   = 0;
    uint32_t restRequestTotal          = 0;
    /// Publishes espMqttClient refused: disconnected, or its outbox out of memory.
    ///
    /// The symptom of an MQTT client that is wedged or falling behind while still reporting
    /// connected. Until this existed the return value of publish() was discarded at eleven of
    /// its twelve call sites, so a refused publish was indistinguishable from a delivered one
    /// on every output (audit, 2026-07-26).
    uint32_t mqttPublishFailureTotal   = 0;
    uint64_t lastSuccessfulPollMs      = 0;
    /// Lowest-ever free stack per application task, in bytes (ESP-IDF's xtensa port defines
    /// StackType_t as uint8_t, so uxTaskGetStackHighWaterMark already returns bytes). Each
    /// task samples its own; 0 = not sampled yet, and outputs publish null for it -- a
    /// monitoring rule on "stack headroom == 0" must not fire at boot.
    uint32_t rs485StackFreeBytes       = 0;
    uint32_t loopStackFreeBytes        = 0;
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
    /// Bus errors arrive in batches: one poll is several transactions, and each can fail on its
    /// own. Adding a count rather than one at a time is what lets the metric describe a bus
    /// that is degrading instead of only one that has already stopped working.
    void recordChecksumErrors(uint32_t n) {
        if (n != 0) {
            checksumErrorTotal_.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void recordTimeouts(uint32_t n) {
        if (n != 0) {
            rs485TimeoutTotal_.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void recordInvalidFrames(uint32_t n) {
        if (n != 0) {
            invalidFrameTotal_.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void recordWifiReconnect() { wifiReconnectTotal_.fetch_add(1, std::memory_order_relaxed); }
    void recordMqttReconnect() { mqttReconnectTotal_.fetch_add(1, std::memory_order_relaxed); }
    void recordModbusClient() { modbusClientConnections_.fetch_add(1, std::memory_order_relaxed); }
    void recordRestRequest() { restRequestTotal_.fetch_add(1, std::memory_order_relaxed); }
    /// Called from the MQTT task, hence the atomic like every other counter here.
    void recordMqttPublishFailure() {
        mqttPublishFailureTotal_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Stack high-water marks, in bytes. Each task reports its OWN mark
    /// (uxTaskGetStackHighWaterMark(nullptr)) so no task handle ever crosses a boundary.
    void recordRs485StackFree(uint32_t bytes) {
        rs485StackFreeBytes_.store(bytes, std::memory_order_relaxed);
    }
    void recordLoopStackFree(uint32_t bytes) {
        loopStackFreeBytes_.store(bytes, std::memory_order_relaxed);
    }

    /// One successful poll's duration. Single writer (rs485Task owns the bus, so every poll
    /// and therefore every sample happens on it); the atomics are for the readers on the web
    /// task, same as every counter here. No CAS needed for min/max under a single writer.
    void recordPollDuration(uint32_t ms) {
        const uint32_t n = pollDurationCount_.load(std::memory_order_relaxed);
        pollDurationLastMs_.store(ms, std::memory_order_relaxed);
        if (n == 0) {
            pollDurationMinMs_.store(ms, std::memory_order_relaxed);
            pollDurationMaxMs_.store(ms, std::memory_order_relaxed);
            pollDurationEwmaMs_.store(ms, std::memory_order_relaxed);
        } else {
            if (ms < pollDurationMinMs_.load(std::memory_order_relaxed)) {
                pollDurationMinMs_.store(ms, std::memory_order_relaxed);
            }
            if (ms > pollDurationMaxMs_.load(std::memory_order_relaxed)) {
                pollDurationMaxMs_.store(ms, std::memory_order_relaxed);
            }
            const uint32_t ewma = pollDurationEwmaMs_.load(std::memory_order_relaxed);
            // Integer EWMA, alpha 1/8. Signed intermediate: ms < ewma must pull DOWN.
            pollDurationEwmaMs_.store(
                static_cast<uint32_t>(static_cast<int64_t>(ewma) +
                                      (static_cast<int64_t>(ms) - ewma) / 8),
                std::memory_order_relaxed);
        }
        pollDurationCount_.store(n + 1, std::memory_order_relaxed);
    }

    /// Cheap atomic read for hot-path callers (the boot-confirm check runs every loop()
    /// iteration; a full snapshot() would copy a std::string each time).
    uint32_t pollSuccessTotal() const {
        return pollSuccessTotal_.load(std::memory_order_relaxed);
    }

    /// Must never contain a secret. Callers pass driver/transport level messages only.
    void setLastError(const std::string& message);

    DiagnosticsSnapshot snapshot() const;

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
    std::atomic<uint32_t> mqttPublishFailureTotal_{0};
    std::atomic<uint32_t> pollDurationCount_{0};
    std::atomic<uint32_t> pollDurationLastMs_{0};
    std::atomic<uint32_t> pollDurationMinMs_{0};
    std::atomic<uint32_t> pollDurationMaxMs_{0};
    std::atomic<uint32_t> pollDurationEwmaMs_{0};
    std::atomic<uint64_t> lastSuccessfulPollMs_{0};
    std::atomic<uint32_t> rs485StackFreeBytes_{0};
    std::atomic<uint32_t> loopStackFreeBytes_{0};

    mutable std::mutex errorMutex_;
    std::string        lastError_;
};

}  // namespace heliograph
