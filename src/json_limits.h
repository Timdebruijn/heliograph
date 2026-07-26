// SPDX-License-Identifier: MIT
//
// One rule for turning a JsonDocument into bytes: measure first, refuse if it does not fit.
//
// Lives at the root rather than under outputs/ or config/ because both need it and neither
// owns it. It was duplicated byte-for-byte between outputs/json_util.h and
// config/configuration.cpp precisely because there was no such place -- config/ writing the
// NVS blob and outputs/ writing REST and MQTT payloads are the same problem, and a bound that
// drifted in one copy would have gone unnoticed in the other.
//
// It depends on ArduinoJson and nothing else, which is what makes it placeable here at all.

#pragma once

#include <ArduinoJson.h>

#include <string>

namespace heliograph::json_limits {

/// Serialises and enforces the size ceiling. The document is built first and measured after:
/// ArduinoJson v7 grows elastically, so the bound is applied here rather than by pre-sizing.
/// Returns false (leaving `out` untouched) on overflow rather than truncating -- a truncated
/// JSON document is not a smaller answer, it is an unparseable one.
inline bool finish(const JsonDocument& doc, std::string& out, size_t maxBytes) {
    if (doc.overflowed()) {
        return false;  // allocation failed under memory pressure
    }
    const size_t needed = measureJson(doc);
    if (needed > maxBytes) {
        return false;
    }
    std::string buffer;
    buffer.resize(needed + 1);
    buffer.resize(serializeJson(doc, buffer.data(), buffer.size()));
    out = std::move(buffer);
    return true;
}

}  // namespace heliograph::json_limits
