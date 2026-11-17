// SPDX-License-Identifier: MIT
//
// A driver's optional list of named diagnostic queries, and what running one returns.
//
// For someone working out what a device supports: ask it something the poll loop never asks,
// see the raw bytes. Every query a driver lists must be a READ; the interface has no way to
// pass data to the device, only a name chosen from the driver's own list. Brand-free: the
// names and their meaning live in the driver that lists them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace heliograph {

struct DiagnosticQuery {
    std::string name;         ///< stable, snake_case; what a caller passes back
    std::string description;  ///< one line for a human
};

enum class DiagnosticStatus : uint8_t {
    Ok,
    Timeout,
    ChecksumError,
    InvalidFrame,
    UnknownQuery,   ///< the driver lists no query with that name; nothing was sent
    NotRegistered,  ///< the device has no usable address right now; nothing was sent
    TransportError, ///< the bus lock or the write failed; the frame may not have gone out
    UnexpectedFrame, ///< no accepted reply, but bytes other than our own echo arrived: read rx
};

inline const char* diagnosticStatusName(DiagnosticStatus status) {
    switch (status) {
        case DiagnosticStatus::Ok:            return "ok";
        case DiagnosticStatus::Timeout:       return "timeout";
        case DiagnosticStatus::ChecksumError: return "checksum_error";
        case DiagnosticStatus::InvalidFrame:  return "invalid_frame";
        case DiagnosticStatus::UnknownQuery:  return "unknown_query";
        case DiagnosticStatus::NotRegistered: return "not_registered";
        case DiagnosticStatus::TransportError: return "transport_error";
        case DiagnosticStatus::UnexpectedFrame: return "unexpected_frame";
    }
    return "unknown";
}

struct DiagnosticQueryResult {
    DiagnosticStatus     status = DiagnosticStatus::UnknownQuery;
    std::vector<uint8_t> tx;       ///< whole request frame; empty when it was not (fully) sent
    std::vector<uint8_t> rx;       ///< every byte the line delivered, unparsed; empty = silence
    std::vector<uint8_t> payload;  ///< the reply's data bytes, undecoded; only with Ok
};

}  // namespace heliograph
