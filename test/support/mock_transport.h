// SPDX-License-Identifier: MIT
//
// Scripted transport for host tests. Header-only so every test suite can include it.
//
// Models the bus at byte level: replies are queued, delivered in chunks, and can be made to
// time out or arrive corrupted. That is what lets a full poll cycle -- registration,
// measurement, night-time silence -- be exercised without an inverter.

#pragma once

#include <cstring>
#include <deque>
#include <functional>
#include <vector>

#include "transport/transport.h"

namespace heliograph::test {

/// Answers a request the way a device would: by looking at what was asked.
///
/// Preferred over the ordered reply queue. A queue forces the test to know how many writes
/// the driver makes internally -- including broadcasts that a real device never answers --
/// so it breaks whenever the driver's internals shift, for no good reason.
///
/// Return false to stay silent. The transport stays protocol-agnostic: the responder is
/// where any framing knowledge lives, in the test that owns that protocol.
using Responder =
    std::function<bool(const std::vector<uint8_t>& request, std::vector<uint8_t>& reply)>;

class MockTransport : public Transport {
public:
    TransportType type() const override { return type_; }
    void          setType(TransportType t) { type_ = t; }

    bool configure(const SerialProfile& profile) override {
        profile_ = profile;
        ++configureCalls;
        return configureSucceeds;
    }

    void flushInput() override {
        rx_.clear();
        ++flushCalls;
    }

    size_t write(const uint8_t* data, size_t len) override {
        writes.emplace_back(data, data + len);
        if (writeFails) {
            return 0;
        }
        if (responder_) {
            std::vector<uint8_t> reply;
            if (responder_(writes.back(), reply)) {
                rx_.insert(rx_.end(), reply.begin(), reply.end());
            }
            return len;
        }
        // Fallback: ordered script. Only for tests that genuinely care about sequencing.
        if (!replies_.empty()) {
            Reply r = std::move(replies_.front());
            replies_.pop_front();
            if (!r.silent) {
                rx_.insert(rx_.end(), r.bytes.begin(), r.bytes.end());
            }
        }
        return len;
    }

    size_t read(uint8_t* buf, size_t len, uint32_t timeoutMs) override {
        (void)timeoutMs;
        // A read advances the simulated clock so tests can drive an overall transaction
        // deadline: `msPerRead` models bytes that trickle in just under each read's own
        // timeout without ever completing a frame.
        clockMs_ += msPerRead;
        if (infiniteNoise) {
            // A line that never falls silent and never forms a valid frame: the pathological
            // case a transaction deadline exists to bound. Would loop forever without one.
            // A cyclic pattern lets a test keep a length-prefixed parser (Modbus) perpetually
            // "incomplete" -- e.g. a byte-count field that promises more than ever arrives.
            if (len == 0) {
                return 0;
            }
            if (!noisePattern.empty()) {
                buf[0] = noisePattern[noiseIdx_ % noisePattern.size()];
                ++noiseIdx_;
            } else {
                buf[0] = noiseByte;
            }
            return 1;
        }
        if (rx_.empty()) {
            ++readTimeouts;
            return 0;
        }
        size_t n = rx_.size() < len ? rx_.size() : len;
        if (chunkSize > 0 && n > chunkSize) {
            n = chunkSize;  // force the caller to reassemble across reads
        }
        std::memcpy(buf, rx_.data(), n);
        rx_.erase(rx_.begin(), rx_.begin() + static_cast<long>(n));
        return n;
    }

    uint64_t nowMs() const override { return clockMs_; }
    /// Milliseconds the simulated clock advances per read(). 0 (default) = time stands still.
    uint32_t msPerRead = 0;
    /// When set, every read() returns one noise byte and never a valid frame or silence.
    bool     infiniteNoise = false;
    uint8_t  noiseByte     = 0x00;  // not the 0xAA/0x55 header, not a Modbus reply
    /// Optional cyclic noise (overrides noiseByte when non-empty), so a length-prefixed
    /// parser can be kept incomplete forever rather than tripping on a short bad frame.
    std::vector<uint8_t> noisePattern;

    bool lock(uint32_t timeoutMs) override {
        (void)timeoutMs;
        if (lockFails) {
            return false;
        }
        ++lockCalls;
        locked = true;
        return true;
    }

    void unlock() override {
        locked = false;
        ++unlockCalls;
    }

    const TransportStats& stats() const override { return stats_; }

    // --- scripting ---

    /// Installs a request-driven responder. Takes precedence over the reply queue.
    void setResponder(Responder r) { responder_ = std::move(r); }
    void clearResponder() { responder_ = nullptr; }

    /// Queues bytes to be delivered in response to the next write().
    void replyWith(const uint8_t* data, size_t len) {
        replies_.push_back(Reply{std::vector<uint8_t>(data, data + len), false});
    }
    void replyWith(const std::vector<uint8_t>& bytes) {
        replies_.push_back(Reply{bytes, false});
    }
    /// The next write() gets no answer at all: the device is asleep or gone.
    void replyWithSilence() { replies_.push_back(Reply{{}, true}); }

    /// Bytes that arrive unbidden, e.g. an echo of our own transmission or another
    /// inverter's traffic.
    void injectNoise(const std::vector<uint8_t>& bytes) {
        rx_.insert(rx_.end(), bytes.begin(), bytes.end());
    }

    void clearScript() {
        replies_.clear();
        rx_.clear();
        writes.clear();
    }

    size_t pendingReplies() const { return replies_.size(); }
    const SerialProfile& profile() const { return profile_; }

    // --- observation ---
    std::vector<std::vector<uint8_t>> writes;
    uint32_t configureCalls = 0;
    uint32_t flushCalls     = 0;
    uint32_t readTimeouts   = 0;
    uint32_t lockCalls      = 0;
    uint32_t unlockCalls    = 0;
    bool     locked         = false;

    // --- fault injection ---
    bool   configureSucceeds = true;
    bool   writeFails        = false;
    bool   lockFails         = false;
    size_t chunkSize         = 0;  // 0 = deliver everything available at once

private:
    struct Reply {
        std::vector<uint8_t> bytes;
        bool                 silent;
    };
    TransportType        type_ = TransportType::Mock;
    SerialProfile        profile_{};
    TransportStats       stats_{};
    Responder            responder_;
    std::deque<Reply>    replies_;
    std::vector<uint8_t> rx_;
    uint64_t             clockMs_  = 0;
    size_t               noiseIdx_ = 0;
};

}  // namespace heliograph::test
