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

#include "drivers/driver_descriptor.h"
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

/// Whether a register read belongs on a bus these drivers are using: only when every one of them
/// speaks Modbus, recognised by its address being a Modbus unit id. On a line running a
/// registration protocol or an ASCII one, a Modbus frame is foreign traffic in the middle of
/// another conversation, and the timeout it earns explains nothing. No driver at all is allowed:
/// that is the bridge someone uses to work out a map for a device nothing supports yet.
bool registerReadFitsBus(const std::vector<const DriverDescriptor*>& drivers);

struct RegisterReadReport {
    RegisterReadStatus    status = RegisterReadStatus::Idle;
    RegisterReadRequest   request;
    modbus::ReadOutcome   outcome;
    std::vector<uint16_t> values;  ///< only when outcome.status == Ok
};

class RegisterReader {
public:
    /// Web task. False when a read is already waiting, or when the request is outside what this
    /// reader sends -- the REST layer validates first, this is the last line.
    bool request(const RegisterReadRequest& request);

    /// rs485Task. Runs a waiting read and returns true if it did.
    bool runIfRequested(Transport& transport);

    RegisterReadReport report() const;
    bool               busy() const;

private:
    mutable std::mutex mutex_;
    RegisterReadReport report_;
};

}  // namespace heliograph
