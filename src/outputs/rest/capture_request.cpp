// SPDX-License-Identifier: MIT

#include "capture_request.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace heliograph::rest {
namespace {

/// A numeric parameter, or its default when absent.
///
/// strtol with no errno check, deliberately and unchanged from the handler this came out of:
/// every unparseable form it accepts ("", "abc", "-5") yields a value that the range check
/// below rejects anyway, so adding a second error path would produce a different message for
/// the same outcome. "30abc" reads as 30, which is the one lenient case and has been the
/// endpoint's behaviour since it existed.
long number(const QueryParam& param, const char* name, long fallback) {
    const char* raw = param(name);
    return raw == nullptr ? fallback : std::strtol(raw, nullptr, 10);
}

ApiError invalid(std::string message) {
    return ApiError{400, "invalid_parameter", std::move(message)};
}

}  // namespace

std::optional<ApiError> parseCaptureRequest(const QueryParam& param, CaptureRequest& out) {
    out = CaptureRequest{};

    // Which conversation to record. Absent means passive, so every caller written before the
    // second mode existed keeps its meaning.
    const char* mode = param("mode");
    if (mode != nullptr) {
        if (std::strcmp(mode, "driver") == 0) {
            out.driverMode = true;
        } else if (std::strcmp(mode, "passive") != 0) {
            return invalid("mode must be passive or driver");
        }
    }

    if (out.driverMode) {
        // Refused, not ignored. There is a working driver, so the line is a fact about the
        // bridge; accepting a baud rate and then recording at a different one would put a
        // number in the report that the capture never ran at.
        for (const char* name : {"baud", "parity", "data_bits", "stop_bits"}) {
            if (param(name) != nullptr) {
                return invalid(std::string("mode=driver records the line the driver is already "
                                           "using; remove '") +
                               name + "'");
            }
        }
        const long seconds = number(param, "seconds", 30);
        if (seconds < 1 || seconds > static_cast<long>(kMaxDriverCaptureSeconds)) {
            return invalid("seconds must be between 1 and " +
                           std::to_string(kMaxDriverCaptureSeconds));
        }
        out.tap.durationMs = static_cast<uint32_t>(seconds) * 1000u;

        const long frames = number(param, "frames", 64);
        // Lower than the passive mode's ceiling, and refused rather than clamped: this report
        // carries a direction and a cut reason per record, and past ~160 records it no longer
        // fits the response bound. A capture that completes and can only answer 500 has spent
        // real bus time for a report nobody can fetch.
        if (frames < 1 || frames > kMaxDriverCaptureFrames) {
            return invalid("frames must be between 1 and " +
                           std::to_string(kMaxDriverCaptureFrames) + " in mode=driver");
        }
        out.tap.maxFrames = static_cast<size_t>(frames);
        return std::nullopt;
    }

    const long seconds = number(param, "seconds", 30);
    if (seconds < 1 || seconds > static_cast<long>(kMaxCaptureSeconds)) {
        // Bounded because a passive capture holds the bus for its whole window: this is also the
        // longest one authenticated request can stop the inverter being polled.
        return invalid("seconds must be between 1 and " + std::to_string(kMaxCaptureSeconds));
    }
    out.config.durationMs = static_cast<uint32_t>(seconds) * 1000u;

    const long frames = number(param, "frames", 64);
    if (frames < 1 || frames > kMaxPassiveCaptureFrames) {
        return invalid("frames must be between 1 and " + std::to_string(kMaxPassiveCaptureFrames));
    }
    out.config.maxFrames = static_cast<size_t>(frames);

    // The line to listen at. There is no driver to ask -- that is the whole situation this mode
    // exists for -- so the operator supplies it, and a wrong guess shows up in the report as
    // bytes with no valid checksums rather than as silence.
    const long baud = number(param, "baud", 9600);
    if (baud < kMinBaudRate || baud > kMaxBaudRate) {
        return invalid("baud must be between " + std::to_string(kMinBaudRate) + " and " +
                       std::to_string(kMaxBaudRate));
    }
    out.profile.baudRate = static_cast<uint32_t>(baud);

    if (const char* parity = param("parity")) {
        SerialParity parsed{};
        if (!parseParity(parity, parsed)) {
            return invalid("parity must be none, even or odd");
        }
        out.profile.parity = parsed;
    }

    const long dataBits = number(param, "data_bits", 8);
    const long stopBits = number(param, "stop_bits", 1);
    if (dataBits < 5 || dataBits > 8 || stopBits < 1 || stopBits > 2) {
        return invalid("data_bits must be 5-8 and stop_bits 1 or 2");
    }
    out.profile.dataBits = static_cast<uint8_t>(dataBits);
    out.profile.stopBits = static_cast<uint8_t>(stopBits);
    return std::nullopt;
}

}  // namespace heliograph::rest
