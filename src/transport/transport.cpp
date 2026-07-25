// SPDX-License-Identifier: MIT

#include "transport.h"

namespace heliograph {

const char* transportTypeName(TransportType type) {
    switch (type) {
        case TransportType::Rs485: return "rs485";
        case TransportType::Rs232: return "rs232";
        case TransportType::Can:   return "can";
        case TransportType::Tcp:   return "tcp";
        case TransportType::Mock:  return "mock";
    }
    return "unknown";
}

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
