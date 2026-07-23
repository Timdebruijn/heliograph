// SPDX-License-Identifier: MIT

#include "rs485_transport.h"

#if defined(ESP32)

#include <Arduino.h>
#include <esp_timer.h>

#include "boards/board.h"
#include "diagnostics/logger.h"

namespace heliograph {
namespace {

uint32_t toSerialConfig(const SerialProfile& p) {
    // Only the combinations a driver can actually ask for. Anything else is a bug, not a
    // configuration to accommodate.
    if (p.dataBits == 8 && p.stopBits == 1) {
        switch (p.parity) {
            case SerialParity::None: return SERIAL_8N1;
            case SerialParity::Even: return SERIAL_8E1;
            case SerialParity::Odd:  return SERIAL_8O1;
        }
    }
    if (p.dataBits == 8 && p.stopBits == 2) {
        switch (p.parity) {
            case SerialParity::None: return SERIAL_8N2;
            case SerialParity::Even: return SERIAL_8E2;
            case SerialParity::Odd:  return SERIAL_8O2;
        }
    }
    return SERIAL_8N1;
}

}  // namespace

Rs485Transport::Rs485Transport() : uart_(board::kRs485UartNum) {}

bool Rs485Transport::configure(const SerialProfile& profile) {
    if (configured_) {
        uart_.end();
    }

    uart_.begin(profile.baudRate, toSerialConfig(profile), board::kRs485Rx, board::kRs485Tx);

    // Boards with a direction pin (SP3485 DE/RE on a GPIO) hand it to the UART as RTS and
    // switch to RS485 half-duplex, so the peripheral flips direction on the exact bit
    // boundary -- a software toggle cannot race the last stop bit. Verified against the
    // official Waveshare demo (WS_RS485.cpp); see docs/hardware.md.
    //
    // kRs485De < 0 means the board declares no (verified) direction pin; nothing is
    // configured and the transceiver's own circuit has to handle direction. See the
    // Relay-6CH board header for why shipping a guessed pin is worse than this.
    if (board::kRs485De >= 0) {
        if (!uart_.setPins(-1, -1, -1, board::kRs485De)) {
            return false;
        }
        if (!uart_.setMode(UART_MODE_RS485_HALF_DUPLEX)) {
            return false;
        }
    }

    uart_.setTimeout(profile.responseTimeoutMs);
    configured_ = true;
    return true;
}

void Rs485Transport::flushInput() {
    while (uart_.available() > 0) {
        uart_.read();
    }
}

size_t Rs485Transport::write(const uint8_t* data, size_t len) {
    const size_t n = uart_.write(data, len);
    // Block until the last bit is on the wire. Without this the UART could drop RTS while
    // the tail of the frame is still shifting out.
    uart_.flush();
    stats_.bytesWritten += static_cast<uint32_t>(n);
    if (n != len) {
        ++stats_.writeErrors;
    }
    // Raw request as sent. TRACE only (self-gated), and driver-agnostic: this is where Phase 3
    // reads what actually goes onto the bus, without the transport learning any protocol. The
    // bus carries no secrets.
    log::traceHex("RS485 TX", data, n);
    return n;
}

size_t Rs485Transport::read(uint8_t* buf, size_t len, uint32_t timeoutMs) {
    // Elapsed-time form, not `millis() + timeoutMs`: the sum wraps once every ~49.7 days of
    // uptime and would return an instant spurious timeout on that tick. `millis() - start`
    // stays correct across the rollover.
    const uint32_t start = millis();
    size_t         got   = 0;

    // Return as soon as anything arrives: the caller reassembles frames and knows when it has
    // enough. Waiting for a full buffer would stall on every short reply.
    while (millis() - start < timeoutMs) {
        const int available = uart_.available();
        if (available > 0) {
            const size_t want = len < static_cast<size_t>(available) ? len : static_cast<size_t>(available);
            got = uart_.readBytes(buf, want);
            break;
        }
        delay(1);  // yields to the scheduler; never a busy-wait
    }

    if (got == 0) {
        ++stats_.readTimeouts;
    }
    stats_.bytesRead += static_cast<uint32_t>(got);
    // Deliberately NOT traced here. A reply arrives one or two bytes per read(), so per-read
    // tracing turned a single frame into ~40 log lines -- fine on a serial console, useless in
    // the bounded in-memory ring the REST log serves, where it evicted all surrounding
    // context (2026-07-20). The driver traces the assembled buffer once per transaction,
    // together with the outcome, which is the level a reader actually reasons at.
    return got;
}

bool Rs485Transport::lock(uint32_t timeoutMs) {
    if (busMutex_.try_lock_for(std::chrono::milliseconds(timeoutMs))) {
        return true;
    }
    ++stats_.lockTimeouts;
    return false;
}

void Rs485Transport::unlock() { busMutex_.unlock(); }

// esp_timer, not millis(): the drivers build absolute transaction deadlines from this
// (`nowMs() + 3000`), and a wrapped uint32 source would let a deadline armed just before
// the 49.7-day mark never fire. See the note at main.cpp's nowMs().
uint64_t Rs485Transport::nowMs() const { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

}  // namespace heliograph

#else  // !ESP32

// Host builds use MockTransport instead; there is no UART to open.
namespace heliograph {

Rs485Transport::Rs485Transport() = default;
bool   Rs485Transport::configure(const SerialProfile&) { return false; }
void   Rs485Transport::flushInput() {}
size_t Rs485Transport::write(const uint8_t*, size_t) { return 0; }
size_t   Rs485Transport::read(uint8_t*, size_t, uint32_t) { return 0; }
uint64_t Rs485Transport::nowMs() const { return 0; }
bool     Rs485Transport::lock(uint32_t) { return busMutex_.try_lock(); }
void     Rs485Transport::unlock() { busMutex_.unlock(); }

}  // namespace heliograph

#endif
