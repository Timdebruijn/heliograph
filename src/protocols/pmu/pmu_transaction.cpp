// SPDX-License-Identifier: MIT
//
// Moved here from the two PMU drivers, which held the same loop twice. The comments below are
// the originals: they record decisions made against real hardware and a real bus, and none of
// that reasoning changed by moving the code.

#include "pmu_transaction.h"

#include <cstdio>
#include <cstring>

#include "diagnostics/logger.h"

namespace heliograph::pmu {

TransactStatus transact(Transport& transport, const Exchange& exchange, uint8_t* payloadOut,
                        size_t payloadCapacity, size_t& payloadLen, Tally& tally,
                        const char* logPrefix) {
    payloadLen = 0;

    TransportLock lock(transport, kBusLockTimeoutMs);
    if (!lock.held()) {
        return TransactStatus::TransportError;
    }

    transport.flushInput();
    if (transport.write(exchange.request, exchange.requestLen) != exchange.requestLen) {
        // The one terminal state before the traceOutcome scaffolding exists. Left silent it
        // would be the only failure without a trace -- the exact blind spot this logging is
        // for (review, 2026-07-20).
        log::trace("RS485 tx failed: %u byte(s) not fully written",
                   static_cast<unsigned>(exchange.requestLen));
        return TransactStatus::TransportError;
    }

    uint8_t rx[kRxBufferSize];
    size_t  have             = 0;
    bool    sawChecksumError = false;

    // Tracing used to happen per read() in the transport, and a single reply arrives in ~40
    // one-byte chunks: one frame filled the entire log ring, so a captured failure showed a
    // byte and a half of context (2026-07-20). One line per transaction instead -- and it
    // reports a REJECTED frame, which previously vanished into the "timeout" bucket and made
    // a talking inverter indistinguishable from a dead bus.
    size_t  received  = 0;  ///< bytes ever read in this transaction, before any are consumed
    size_t  rejected  = 0;
    uint8_t rejSrc[2] = {0, 0};
    uint8_t rejCtrl = 0, rejFn = 0;
    const auto traceOutcome = [&](const char* outcome) {
        if (!log::enabled(LogLevel::Trace)) {
            return;
        }
        if (rejected > 0) {
            log::trace("%s %s: %u byte(s), %u frame(s) rejected, last from %02X %02X ctrl %02X fn %02X",
                       logPrefix, outcome, static_cast<unsigned>(received),
                       static_cast<unsigned>(rejected), rejSrc[0], rejSrc[1], rejCtrl, rejFn);
        } else {
            log::trace("%s %s: %u byte(s) received", logPrefix, outcome,
                       static_cast<unsigned>(received));
        }
        if (received > 0) {
            // "<bus> RX", exactly as both drivers wrote it before this moved. Built rather
            // than passed so the caller has one label to supply, not two that could disagree.
            char label[24];
            std::snprintf(label, sizeof(label), "%s RX", logPrefix);
            log::traceHex(label, rx, have);
        }
    };

    const uint64_t deadline = transport.nowMs() + kTransactionDeadlineMs;

    for (;;) {
        // Overall wall-clock bound on the whole exchange. Each read() renews its own 1 s
        // timeout, so a sustained trickle of bytes never trips the n==0 branch below and the
        // loop -- holding the bus lock -- could otherwise run unbounded until the watchdog
        // reboots (review, 2026-07-20).
        if (transport.nowMs() >= deadline) {
            ++tally.timeouts;
            traceOutcome("transaction deadline exceeded");
            return TransactStatus::Timeout;
        }

        Frame      frame;
        const auto parsed = parseFrame(rx, have, frame);

        if (parsed == ParseResult::Ok) {
            auto valid = validateResponse(frame, exchange.command, exchange.expectedSource);
            if (valid == ParseResult::WrongSource && exchange.altSource != nullptr) {
                valid = validateResponse(frame, exchange.command, *exchange.altSource);
            }
            if (valid == ParseResult::Ok) {
                if (frame.dataLength > payloadCapacity) {
                    // Unreachable today (every caller passes a full-size buffer), but this is
                    // an InvalidFrame return and every other one tallies. Left untallied it
                    // would be a return path that silently reports nothing the moment some
                    // future caller passes a smaller buffer.
                    ++tally.invalidFrames;
                    traceOutcome("payload too large");
                    return TransactStatus::InvalidFrame;
                }
                if (frame.dataLength > 0) {
                    std::memcpy(payloadOut, frame.data, frame.dataLength);
                }
                payloadLen = frame.dataLength;
                traceOutcome("ok");
                return TransactStatus::Ok;
            }
            // A well-formed frame that is not ours: our own transmission echoed back by the
            // half-duplex bus, or traffic for another inverter. Drop it and keep looking
            // rather than treat the whole exchange as failed. Recorded, though -- if the
            // inverter answers with something unexpected, that is the single most useful
            // fact about the failure and it used to leave no trace at all.
            ++rejected;
            rejSrc[0] = frame.source.high;
            rejSrc[1] = frame.source.low;
            rejCtrl   = frame.control;
            rejFn     = frame.function;
            std::memmove(rx, rx + frame.frameLength, have - frame.frameLength);
            have -= frame.frameLength;
            continue;
        }

        if (parsed == ParseResult::BadHeader) {
            // Resync: drop one byte and rescan. Line noise at the start of a reply must not
            // cost us the reply itself.
            std::memmove(rx, rx + 1, have - 1);
            --have;
            continue;
        }

        if (parsed == ParseResult::BadChecksum) {
            sawChecksumError = true;
            ++tally.checksumErrors;
            const size_t skip = frame.frameLength > 0 ? frame.frameLength : 1;
            const size_t n    = skip < have ? skip : have;
            std::memmove(rx, rx + n, have - n);
            have -= n;
            continue;
        }

        // Incomplete: read more.
        if (have >= sizeof(rx)) {
            ++tally.invalidFrames;
            traceOutcome("buffer full without a valid frame");
            return TransactStatus::InvalidFrame;
        }
        const size_t n = transport.read(rx + have, sizeof(rx) - have, kResponseTimeoutMs);
        if (n == 0) {
            if (sawChecksumError) {
                traceOutcome("checksum error");
                return TransactStatus::ChecksumError;
            }
            ++tally.timeouts;
            // "silent" only when nothing arrived at all; otherwise something answered and we
            // did not accept it, which is a different problem with a different fix.
            traceOutcome(received > 0 ? "timeout after partial/rejected data" : "silent");
            return TransactStatus::Timeout;
        }
        have += n;
        received += n;
    }
}

}  // namespace heliograph::pmu
