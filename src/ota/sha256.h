// SPDX-License-Identifier: MIT
//
// SHA-256, streaming, portable.
//
// WHY NOT mbedtls, which is already in the image. Because it is not on the host, and this is
// the one piece of the update path where being wrong is silent: a hash that is subtly
// mis-implemented accepts a corrupted image instead of refusing it, and nothing in normal
// operation would ever show it. One implementation compiled into both builds means the ESP32
// runs exactly the code the NIST vectors in test_ota were checked against, rather than the
// host testing a policy while the device runs something else.
//
// It is FIPS 180-4 with no options and no configuration -- the algorithm has not changed since
// 2001 and will not. That is what makes it a reasonable thing to carry: a hundred lines that
// can never need maintaining, against a platform split in the code path that decides whether
// firmware is written to flash.
//
// Integrity, not authenticity. It catches a truncated download, a proxy that mangled the body,
// a bad flash write. It cannot tell you the image is one this project published -- the hash
// travels with the binary, so anyone who can replace one can replace both. See docs/security.md.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace heliograph::ota {

/// A SHA-256 digest: exactly 32 bytes, and the type says so.
///
/// `uint8_t out[32]` in a parameter list decays to a pointer, which makes the 32 a comment. That
/// matters more here than almost anywhere else in this firmware: finish() and hexDigestEquals()
/// sit on either side of the check that decides whether an update is accepted, and if they ever
/// disagreed about the length, the disagreement would be a buffer overrun on one side and a
/// silently short comparison on the other -- neither of which the build would notice.
using Digest = std::array<uint8_t, 32>;

/// Feed it the image as it arrives; ask for the digest at the end.
class Sha256 {
public:
    Sha256() { reset(); }

    void reset();
    void update(const uint8_t* data, size_t len);

    /// Writes the 32-byte digest. The object must not be updated afterwards without reset().
    void finish(Digest& out);

    /// Lowercase hex, the form every tool prints and the form latest.json carries.
    std::string finishHex();

private:
    void compress(const uint8_t block[64]);

    uint32_t state_[8]{};
    uint64_t totalBits_ = 0;
    uint8_t  buffer_[64]{};
    size_t   buffered_ = 0;
};

/// Lowercase hex of a byte range. Separate from Sha256 because the OTA path needs both forms
/// of the same digest -- the bytes, to compare against what was promised, and the text, to put
/// in an error message and the status payload -- and finish() may only be called once.
std::string toHex(const uint8_t* data, size_t len);

/// Compares a user-supplied hex digest against a computed one, case-insensitively.
///
/// False on any length other than 64 or any non-hex character, so a truncated or malformed
/// expectation is a refusal rather than a comparison that happens to fail. Constant-time is
/// deliberately NOT attempted: this compares a public checksum, not a secret, and pretending
/// otherwise would be security theatre.
bool hexDigestEquals(const std::string& expectedHex, const Digest& digest);

}  // namespace heliograph::ota
