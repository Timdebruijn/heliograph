// SPDX-License-Identifier: MIT
//
// DRM semantics on top of the relay layer (AS/NZS 4777.2 demand response modes).
//
// The RelayController knows contacts; this layer knows what the contacts MEAN. The user
// assigns each relay a role in the configuration (drm0..drm8, or none), and from those
// roles this module derives the selectable modes, the relay pattern for a mode, and the
// mode implied by a relay mask. Everything here is pure and host-tested; asserting the
// pattern goes through the RelayController and its gates like any other relay command.
//
// Firmware semantics are fixed: relay ENERGISED = that DRM line ASSERTED. Whether
// "asserted" closes or opens the actual circuit at the inverter is a WIRING choice
// (NO vs NC terminal) documented in docs/drm.md -- the failsafe rule (bridge dead =>
// inverter runs) decides which terminal to use per line.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace heliograph::drm {

/// Sentinel mode names, next to the drm0..drm8 role names themselves.
inline constexpr const char* kModeNormal = "normal";  ///< nothing asserted
inline constexpr const char* kModeCustom = "custom";  ///< a mask no single mode explains

/// True for a valid per-relay role string: "none" or "drm0".."drm8".
bool isValidRole(const std::string& role);

/// The selectable modes for a role assignment: "normal" plus every non-"none" role, in
/// relay order. Empty when no relay has a role -- callers then publish no select at all.
std::vector<std::string> optionsFor(const std::vector<std::string>& roles);

/// The relay pattern that realises `mode`: exactly the relay(s) carrying that role are
/// energised, everything else released. Returns false for a mode that is not an option
/// (including "custom", which is a state, not a command).
bool patternFor(const std::vector<std::string>& roles, const std::string& mode,
                std::vector<bool>& outPattern);

/// The mode a relay mask implies: "normal" when nothing is energised, the role name when
/// exactly the relays of one role (and nothing else) are energised, "custom" otherwise
/// (e.g. hand-toggled combinations via the individual switches).
std::string modeFrom(const std::vector<std::string>& roles, uint8_t mask);

}  // namespace heliograph::drm
