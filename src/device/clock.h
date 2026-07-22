// SPDX-License-Identifier: MIT
//
// One shared clock type. Injectable so the whole core is host-testable with a scripted clock
// and never reaches for millis() directly. Previously redeclared identically in four headers.

#pragma once

#include <cstdint>
#include <functional>

namespace heliograph {

/// Returns milliseconds since boot.
using ClockFn = std::function<uint64_t()>;

}  // namespace heliograph
