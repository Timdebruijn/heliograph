// SPDX-License-Identifier: MIT

#include "transport.h"

#include "diagnostics/bus_tap.h"

namespace heliograph {

// The timestamp comes from the transport's own clock rather than from the caller: it is the one
// clock the recorder and the bus agree on, and a driver has no reason to be passing time values
// into a facility it does not know exists.
void Transport::tapTxImpl(const uint8_t* data, size_t len) { tap_->recordTx(data, len, nowMs()); }
void Transport::tapRxImpl(const uint8_t* data, size_t len) { tap_->recordRx(data, len, nowMs()); }

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
