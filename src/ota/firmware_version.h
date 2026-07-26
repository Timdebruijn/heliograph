// SPDX-License-Identifier: MIT
//
// Comparing firmware versions, so the dashboard can say "there is a newer one" without being
// wrong about it.
//
// Pure, and host-tested, because the failure modes are all quiet. A comparison that reads
// "0.9.0" as newer than "0.14.0" -- string ordering does exactly that -- nags forever about an
// update that is actually a downgrade. One that never fires leaves everybody on an old image
// believing they are current. Neither shows up as an error anywhere.
//
// The version this firmware reports about ITSELF carries a build stamp:
//
//     0.14.0 (Jul 26 2026 17:31:45)
//
// so the parser has to take the leading semver and ignore whatever follows. That stamp is
// useful (it tells two builds of the same version apart) and is not going away, which is
// exactly why parsing has to be deliberate rather than assuming the field is clean.

#pragma once

#include <cstdint>
#include <string>

namespace heliograph::ota {

struct SemVer {
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t patch = 0;

    friend bool operator==(const SemVer& a, const SemVer& b) {
        return a.major == b.major && a.minor == b.minor && a.patch == b.patch;
    }
    friend bool operator<(const SemVer& a, const SemVer& b) {
        if (a.major != b.major) return a.major < b.major;
        if (a.minor != b.minor) return a.minor < b.minor;
        return a.patch < b.patch;
    }
};

/// Reads a leading "MAJOR.MINOR.PATCH", tolerating a leading 'v' and ignoring any trailing
/// build stamp or suffix.
///
/// False when the three numbers are not all there. Refusing is the point: an unparseable
/// version must never compare as older or newer, because both answers are a guess and one of
/// them offers somebody a firmware image on the strength of it.
bool parseSemVer(const std::string& text, SemVer& out);

/// True when `candidate` is strictly newer than `current`.
///
/// False whenever either side will not parse -- including the case where a release feed has
/// been replaced by something unexpected. A bad feed produces no notification rather than a
/// notification nobody can trust.
bool isNewer(const std::string& current, const std::string& candidate);

}  // namespace heliograph::ota
