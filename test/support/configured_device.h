// SPDX-License-Identifier: MIT
//
// Builds a DriverSettings by NAME rather than by position. Header-only, like the rest of
// test/support, so every suite can include it.
//
// The aggregate form the tests used --
//
//     DriverSettings{"modbus_profile", false, {{"unit_id", "2"}}}
//
// warns about the field it leaves off (-Wmissing-field-initializers on `label`), which is the
// harmless half. The other half is that it is positional: `label` was added to the end of the
// struct and every one of these silently kept compiling, now meaning "no label" where before
// there was no such concept. The next field added to DriverSettings does the same again, and
// the compiler cannot tell the difference between a field a test declined to set and one it
// never heard of.
//
// C++20 designated initialisers would say this in the language. This project is gnu++17, so it
// is a function instead -- which has the advantage that a new field with a sensible default
// needs no change here at all.

#pragma once

#include <string>
#include <utility>

#include "config/configuration.h"

namespace heliograph::test {

/// An additional device as an operator would configure one: a driver, its options, and
/// optionally what they call it.
///
/// autoDetect is deliberately not a parameter. It defaults to false in the struct and no test
/// here sets it; a parameter nobody passes is one more thing to keep true.
inline DriverSettings configuredDevice(std::string driverId, DriverOptions options = {},
                                       std::string label = "") {
    DriverSettings device;
    device.id      = std::move(driverId);
    device.options = std::move(options);
    device.label   = std::move(label);
    return device;
}

}  // namespace heliograph::test
