// SPDX-License-Identifier: MIT
//
// Reads a handful of registers on request, for someone working out a register map: enter a
// register, see what the device answers, without rebuilding and flashing to try a guess.
//
// Read-only by construction. The only functions it can send are 0x03 and 0x04; there is no
// write path here to switch off, so it is not covered by security.read_only_mode and does not
// need to be.
//
// Same handover as discovery and capture: the web handler requests, rs485Task runs it in place
// of one poll, because that task owns the bus. One read is one transaction, so it costs the
// poll schedule at most one cycle.
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "device/clock.h"
#include "protocols/modbus/modbus_client.h"
#include "transport/transport.h"

namespace heliograph {

/// The most registers one read returns. Enough for a 32-bit value and its neighbours, small
/// enough that the answer fits on a phone screen.
inline constexpr uint16_t kMaxRegisterReadCount = 16;

struct RegisterReadRequest {
    uint8_t  unitId       = 1;
    uint8_t  functionCode = modbus::kReadHoldingRegisters;  ///< 0x03 or 0x04, nothing else
    uint16_t start        = 0;
    uint16_t count        = 1;
};

enum class RegisterReadStatus : uint8_t {
    Idle,       ///< nothing requested since boot
    Requested,  ///< waiting for rs485Task
    Done,       ///< ran; `outcome` says what the device answered
};

const char* registerReadStatusName(RegisterReadStatus status);

struct RegisterReadReport {
    RegisterReadStatus    status = RegisterReadStatus::Idle;
    RegisterReadRequest   request;
    modbus::ReadOutcome   outcome;
    std::vector<uint16_t> values;  ///< only when outcome.status == Ok
    uint64_t              finishedMs = 0;
};

class RegisterReader {
public:
    explicit RegisterReader(ClockFn clock) : clock_(std::move(clock)) {}

    /// Web task. False when a read is already waiting, or when the request is outside what this
    /// reader sends -- the REST layer validates first, this is the last line.
    bool request(const RegisterReadRequest& request);

    /// rs485Task. Runs a waiting read and returns true if it did.
    bool runIfRequested(Transport& transport);

    RegisterReadReport report() const;
    bool               busy() const;

private:
    ClockFn            clock_;
    mutable std::mutex mutex_;
    RegisterReadReport report_;
};

}  // namespace heliograph
