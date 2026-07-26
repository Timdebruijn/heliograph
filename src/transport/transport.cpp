// SPDX-License-Identifier: MIT

#include "transport.h"

namespace heliograph {

const char* parityName(SerialParity parity) {
    switch (parity) {
        case SerialParity::None: return "none";
        case SerialParity::Even: return "even";
        case SerialParity::Odd:  return "odd";
    }
    return "unknown";
}

bool parseParity(const std::string& name, SerialParity& out) {
    if (name == "none") { out = SerialParity::None; return true; }
    if (name == "even") { out = SerialParity::Even; return true; }
    if (name == "odd")  { out = SerialParity::Odd;  return true; }
    return false;
}

}  // namespace heliograph
