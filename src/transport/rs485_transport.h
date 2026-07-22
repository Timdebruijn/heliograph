// SPDX-License-Identifier: MIT
//
// RS485 over the ESP32-S3 UART, with hardware direction control.
//
// Contains no protocol knowledge whatsoever: bytes, timeouts and a bus lock. Which bytes
// mean what is entirely the driver's business.

#pragma once

#include <mutex>

#include "transport.h"

#if defined(ESP32)
#include <HardwareSerial.h>
#endif

namespace heliograph {

class Rs485Transport : public Transport {
public:
    Rs485Transport();

    TransportType type() const override { return TransportType::Rs485; }

    /// Configures the UART and puts it in RS485 half-duplex mode, handing the DE/RE pin to
    /// the peripheral as RTS. Safe to call again to switch profiles.
    bool configure(const SerialProfile& profile) override;

    void   flushInput() override;
    size_t   write(const uint8_t* data, size_t len) override;
    size_t   read(uint8_t* buf, size_t len, uint32_t timeoutMs) override;
    uint64_t nowMs() const override;

    bool lock(uint32_t timeoutMs) override;
    void unlock() override;

    const TransportStats& stats() const override { return stats_; }

private:
    TransportStats stats_;
    // Exactly one component may talk on the bus at a time. std::mutex maps onto pthreads on
    // ESP32, so this is the same primitive the host tests exercise.
    std::timed_mutex busMutex_;
    bool             configured_ = false;

#if defined(ESP32)
    HardwareSerial uart_;
#endif
};

}  // namespace heliograph
