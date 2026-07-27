// SPDX-License-Identifier: MIT
//
// One request/response exchange on a half-duplex PMU bus.
//
// SEPARATE FROM pmu_protocol.h ON PURPOSE. That header is a pure codec -- build a frame, parse
// a frame, validate a reply -- with no I/O and no clock. This one performs a transaction, so it
// depends on Transport. Keeping them apart means the codec stays something you can reason about
// without a bus at all, which is what makes its tests cheap.
//
// WHY IT EXISTS: the two PMU drivers each carried their own copy of this loop. Normalised for
// whitespace they were 119 and 116 lines with 25 differing, and of those the only real
// differences were the function signature, which builder made the request, and the string in the
// log line. Everything that decides what happens on the wire -- the transaction deadline,
// resynchronising past line noise, dropping a frame addressed to someone else, tallying a
// checksum error, reassembling a reply split across reads -- was identical twice. A fix applied
// to one and not the other is the failure this prevents; the deadline itself was once exactly
// that kind of fix.
//
// Deliberately brand-free, comments included: layering rule 1 caught an earlier draft of this
// header naming both drivers, and it was right to. A third PMU device should find a description
// of the protocol here, not of whoever happened to need it first.
//
// Not platform code: Transport is an interface over cstdint, so this compiles and is tested on
// the host like the codec beside it.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pmu_protocol.h"
#include "transport/transport.h"

namespace heliograph::pmu {

/// Shared line timing. Identical in both drivers before this module existed, declared twice.
inline constexpr uint32_t kResponseTimeoutMs = 1000;
/// Wall-clock ceiling on one transact() exchange. A real reply lands well inside a second
/// (first byte within the response timeout, then ~90 bytes stream in milliseconds); 3 s leaves
/// ample slack while still bounding a pathological trickle that never completes a frame.
inline constexpr uint32_t kTransactionDeadlineMs = 3000;
inline constexpr uint32_t kBusLockTimeoutMs      = 2000;
inline constexpr size_t   kRxBufferSize          = kMaxFrameSize + 16;

enum class TransactStatus : uint8_t { Ok, Timeout, ChecksumError, InvalidFrame, TransportError };

/// Bus-error tallies, owned by the driver and bumped here.
///
/// Passed in rather than derived from the return value, and that is not a style choice: a
/// transaction can meet several bad-checksum frames while returning ChecksumError once. Counting
/// from the result would quietly undercount exactly the failure these numbers exist to expose.
struct Tally {
    uint32_t checksumErrors = 0;
    uint32_t invalidFrames  = 0;
    uint32_t timeouts       = 0;
};

/// What to send and what counts as an answer to it.
struct Exchange {
    const uint8_t* request    = nullptr;
    size_t         requestLen = 0;
    CommandCode    command{};
    Address        expectedSource{};
    /// A second acceptable source, or nullptr. Used where an inverter may answer from the
    /// address it was given or from the one it had before being told.
    const Address* altSource = nullptr;
};

/// Sends `exchange` and waits for a matching reply, taking the bus lock for the whole thing.
///
/// `logPrefix` labels this driver's traffic in trace output; it is the only thing here that
/// differs per driver and it changes nothing about behaviour.
TransactStatus transact(Transport& transport, const Exchange& exchange, uint8_t* payloadOut,
                        size_t payloadCapacity, size_t& payloadLen, Tally& tally,
                        const char* logPrefix);

}  // namespace heliograph::pmu
