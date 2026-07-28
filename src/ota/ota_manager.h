// SPDX-License-Identifier: MIT
//
// Local OTA over HTTP, using the core's Update.h (no cloud, no extra dependency).
//
// ElegantOTA was rejected: its free edition moved from MIT to AGPL-3.0, which is a real
// consideration for anyone distributing binaries. See docs/decisions.md.
//
// The dual-app partition table (partitions_16mb_ota.csv) means a failed or rejected image
// leaves the running one bootable: Update writes to the inactive slot and only flips the
// boot pointer once the image is complete and verified.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "sha256.h"

namespace heliograph::ota {

/// First byte of every ESP32 firmware image.
inline constexpr uint8_t kEspImageMagic = 0xE9;

enum class OtaResult : uint8_t {
    Ok,
    AlreadyRunning,
    /// First byte is not 0xE9: not a firmware image at all. Rejected before a single byte is
    /// written, so uploading the wrong file cannot brick the inactive partition.
    NotFirmware,
    TooLarge,
    /// Update.h refused (bad flash, size mismatch, checksum).
    WriteFailed,
    NotFinished,
    NoPartition,
    /// Every byte arrived and the image hashes to something other than what was promised.
    /// Told apart from WriteFailed on purpose: that one means the flash refused, this one
    /// means the flash did as it was told and the bytes were wrong.
    HashMismatch,
};

const char* otaResultName(OtaResult result);

/// Checks whether a buffer starts like an ESP32 image. Pure, so the rejection path is tested
/// without flashing anything.
bool looksLikeFirmware(const uint8_t* data, size_t len);

/// How long the freshly-booted image must run healthily before it confirms itself valid.
inline constexpr uint64_t kHealthyBootThresholdMs = 30000;
/// The bounded fallback for a WiFi-less but otherwise working boot: after this long, a
/// bridge that is demonstrably doing its job (successful inverter polls) confirms even
/// without a network. Without this, a router outage spanning an OTA left the image
/// pending-verify indefinitely, and the next power blip silently reverted a healthy update
/// (review, 2026-07-21). 30 minutes comfortably covers a router reboot/ISP hiccup while a
/// genuinely broken image can still be recovered by a power-cycle inside that window.
inline constexpr uint64_t kOfflineConfirmThresholdMs = 30ull * 60ull * 1000ull;

/// Decides whether a freshly-flashed image should now confirm itself valid to the bootloader
/// (cancelling the pending rollback). Pure and host-tested.
///
/// Fast path: networked and stable for kHealthyBootThresholdMs. Deliberately NOT gated on a
/// successful inverter poll: an inverter is absent every night, so requiring one would roll
/// back every evening OTA.
/// Fallback path: no network, but up for kOfflineConfirmThresholdMs AND at least one
/// successful poll this boot (`pollingHealthy`) -- proof the bridge does its actual job, so
/// a long router outage cannot turn the next power blip into a silent downgrade. A boot
/// that is merely not-crashing (no network AND no polls) never confirms and stays eligible
/// for rollback.
bool shouldConfirmHealthyBoot(bool wifiConnected, uint64_t uptimeMs, bool alreadyConfirmed,
                              bool pollingHealthy);

/// Tells the bootloader this image is good, cancelling the rollback armed on the first boot
/// after an OTA. Without this, the Arduino-ESP32 bootloader (rollback enabled by default)
/// rolls back to the previous image on the next reboot. No-op on the host. Idempotent-safe:
/// only meaningful once, guard the caller with shouldConfirmHealthyBoot().
void confirmHealthyBoot();

/// The running image's otadata state as text ("pending_verify", "valid", ...). Surfaced in
/// the diagnostics API so the rollback window is observable: right after an OTA boot this
/// reads "pending_verify"; once confirmHealthyBoot() has run it reads "valid". "unknown"
/// on the host and for a factory-flashed image without otadata state.
const char* imageStateName();

class OtaManager {
public:
    /// `expectedSize` may be 0 when the client did not send Content-Length.
    ///
    /// `expectedSha256` is the digest the image must hash to, lowercase or uppercase hex.
    /// Empty means "do not check", which is what a hand-picked file from the settings page
    /// still does -- there is nothing to compare it against.
    ///
    /// Checking here rather than trusting the sender is the point. The dashboard already
    /// verifies the download against the release feed before it uploads, so this catches what
    /// that cannot: a mangled upload across the LAN, a proxy in between, a half-written flash.
    /// And it fails BEFORE end() flips the boot partition, so a bad image is a clean error
    /// rather than a reboot into the rollback path.
    ///
    /// Integrity, not authenticity: the hash travels with the binary, so it cannot tell you
    /// the image is one this project published. See docs/security.md.
    OtaResult begin(size_t expectedSize, const std::string& expectedSha256 = {});

    /// Feeds a chunk. The first chunk is checked for the image magic before anything is
    /// written.
    OtaResult write(const uint8_t* data, size_t len);

    /// Finalises and marks the new image bootable. The caller reboots.
    OtaResult end();

    void abort();

    bool     running() const { return running_; }
    size_t   written() const { return written_; }
    /// Last failure, for the REST response and diagnostics. Never contains a credential.
    const std::string& lastError() const { return lastError_; }

    /// The digest of what was actually written, once end() has run. Reported so a mismatch can
    /// say what arrived as well as what was wanted -- "checksum failed" with no numbers leaves
    /// nobody able to tell a corrupted download from the wrong file entirely.
    const std::string& writtenSha256() const { return writtenSha256_; }

private:
    /// Computes the digest of everything written and compares it with what was promised.
    /// Defined once, outside the ESP32/host split, so the host tests exercise the decision the
    /// device actually makes rather than a parallel one.
    OtaResult finishHash();

    // Atomic by codebase convention for anything a second task could observe. Today every
    // call site lives on the single AsyncTCP task, so this is belt-and-braces -- but that
    // single-task property is an implicit library detail, not something this class enforces,
    // and running() is exactly the kind of flag a future caller polls from another task.
    /// The state an upload starts from, set in ONE place for both builds.
    ///
    /// These eight fields were assigned identically in the ESP32 begin() and the host begin(),
    /// which is a worse duplication than most: the two live on opposite sides of an #if, so a
    /// field added to one and forgotten in the other would leave the host tests setting up a
    /// different starting state than the device. The tests would pass and the bridge would
    /// carry a stale value into a firmware update.
    void startTracking(size_t expectedSize, const std::string& expectedSha256);

    std::atomic<bool> running_{false};
    bool        magicChecked_ = false;
    size_t      written_    = 0;
    size_t      expected_   = 0;
    std::string lastError_;
    std::string expectedSha_;
    std::string writtenSha256_;
    Sha256      hasher_;
};

}  // namespace heliograph::ota
