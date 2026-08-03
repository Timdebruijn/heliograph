// SPDX-License-Identifier: MIT

#include "drivers/modbus_profile/register_dump.h"

#include <cstdio>

namespace heliograph::profile {

size_t formatRegisterLine(char* out, size_t outSize, uint8_t unitId, RegSpace space,
                          uint16_t start, const uint16_t* values, size_t count) {
    if (out == nullptr || outSize == 0) {
        return 0;  // nowhere to write, not even a NUL
    }
    out[0] = '\0';
    if (values == nullptr) {
        count = 0;
    }

    const char* spaceName = space == RegSpace::Input ? "in" : "hold";
    const int   prefix    = snprintf(out, outSize, "MODBUS unit %u %s %u:",
                                     static_cast<unsigned>(unitId), spaceName,
                                     static_cast<unsigned>(start));
    if (prefix < 0) {
        // Encoding error. snprintf leaves the buffer indeterminate on failure, so the NUL above
        // is not enough -- restore it before handing the caller a string.
        out[0] = '\0';
        return 0;
    }
    size_t pos = static_cast<size_t>(prefix);

    for (size_t i = 0; i < count; ++i) {
        // snprintf returns what it WOULD have written, not what it did, so `pos` can run past
        // the buffer -- and then `outSize - pos` underflows to a huge size_t and becomes the
        // next call's buffer size. Stopping while pos is still inside the buffer makes that
        // subtraction unconditionally safe.
        if (pos >= outSize) {
            return i;
        }
        const int written = snprintf(out + pos, outSize - pos, " %04X",
                                     static_cast<unsigned>(values[i]));
        if (written < 0) {
            return i;  // keep pos non-negative; the registers before this one are intact
        }
        if (static_cast<size_t>(written) >= outSize - pos) {
            // Truncated: snprintf NUL-terminated what fitted, but a half-written register is a
            // lie in a dump that gets read against a device. Cut the line back to the last whole
            // one and report how many that was.
            out[pos] = '\0';
            return i;
        }
        pos += static_cast<size_t>(written);
    }
    return count;
}

}  // namespace heliograph::profile
