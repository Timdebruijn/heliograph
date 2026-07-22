// SPDX-License-Identifier: MIT
// See pmu_protocol.h for provenance and licensing of the protocol knowledge.

#include "pmu_protocol.h"

namespace heliograph::pmu {

uint16_t checksum(const uint8_t* data, size_t len) {
    uint16_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum = static_cast<uint16_t>(sum + data[i]);
    }
    return sum;
}

BuildResult buildRequest(CommandCode command,
                         Address     destination,
                         const uint8_t* data,
                         size_t         dataLen,
                         uint8_t*       out,
                         size_t         outCapacity,
                         size_t&        outLength) {
    return buildRequestFrom(kPmuAddress, command, destination, data, dataLen, out, outCapacity,
                            outLength);
}

BuildResult buildRequestFrom(Address     source,
                             CommandCode command,
                             Address     destination,
                             const uint8_t* data,
                             size_t         dataLen,
                             uint8_t*       out,
                             size_t         outCapacity,
                             size_t&        outLength) {
    if (dataLen > kMaxDataLength) {
        return BuildResult::DataTooLong;
    }
    const size_t frameLen = kFrameOverhead + dataLen;
    if (outCapacity < frameLen) {
        return BuildResult::BufferTooSmall;
    }

    out[0] = kHeader0;
    out[1] = kHeader1;
    out[2] = source.high;
    out[3] = source.low;
    out[4] = destination.high;
    out[5] = destination.low;
    out[6] = command.control;
    out[7] = command.function;
    out[8] = static_cast<uint8_t>(dataLen);

    for (size_t i = 0; i < dataLen; ++i) {
        out[kOffsetData + i] = data[i];
    }

    // Checksum covers every byte up to and including the payload, header included.
    const uint16_t sum = checksum(out, kOffsetData + dataLen);
    out[kOffsetData + dataLen]     = static_cast<uint8_t>(sum >> 8);
    out[kOffsetData + dataLen + 1] = static_cast<uint8_t>(sum & 0xFF);

    outLength = frameLen;
    return BuildResult::Ok;
}

size_t expectedFrameLength(const uint8_t* buf, size_t len) {
    if (len < kMinBytesForLength) {
        return 0;
    }
    return kFrameOverhead + buf[kOffsetLength];
}

ParseResult parseFrame(const uint8_t* buf, size_t len, Frame& out) {
    // Enough bytes to check the header at all?
    if (len < 2) {
        return ParseResult::Incomplete;
    }
    if (buf[0] != kHeader0 || buf[1] != kHeader1) {
        return ParseResult::BadHeader;
    }
    if (len < kMinBytesForLength) {
        return ParseResult::Incomplete;
    }

    const size_t dataLen  = buf[kOffsetLength];
    const size_t frameLen = kFrameOverhead + dataLen;
    if (len < frameLen) {
        return ParseResult::Incomplete;
    }

    const uint16_t calculated = checksum(buf, kOffsetData + dataLen);
    const uint16_t received =
        static_cast<uint16_t>((buf[kOffsetData + dataLen] << 8) | buf[kOffsetData + dataLen + 1]);
    if (calculated != received) {
        // Header and length byte were consistent up to this point, so the length is good
        // enough for resync: report it so the caller can skip the whole corrupt frame
        // instead of crawling through it one byte at a time.
        out.frameLength = frameLen;
        return ParseResult::BadChecksum;
    }

    // No "checksum must be non-zero" guard here: the header check above already rejects the
    // all-zero frame that guard exists for, and a byte sum that wraps to exactly 0 is a
    // legitimate checksum. See the note on checksum() in the header.

    out.source      = Address{buf[kOffsetSourceHigh], buf[kOffsetSourceHigh + 1]};
    out.destination = Address{buf[kOffsetDestinationHigh], buf[kOffsetDestinationHigh + 1]};
    out.control     = buf[kOffsetControl];
    out.function    = buf[kOffsetFunction];
    out.data        = dataLen > 0 ? buf + kOffsetData : nullptr;
    out.dataLength  = dataLen;
    out.frameLength = frameLen;
    return ParseResult::Ok;
}

ParseResult validateResponse(const Frame& frame, CommandCode expected, Address expectedSource) {
    if (frame.destination != kPmuAddress) {
        return ParseResult::WrongDestination;
    }
    if (frame.source != expectedSource) {
        return ParseResult::WrongSource;
    }
    if (frame.control != expected.control || frame.function != expected.responseFunction()) {
        return ParseResult::UnexpectedResponse;
    }
    return ParseResult::Ok;
}

}  // namespace heliograph::pmu
