// SPDX-License-Identifier: MIT

#include "ota_manager.h"

#if defined(ESP32)
#include <Update.h>
#include <esp_ota_ops.h>
#endif

namespace heliograph::ota {

const char* otaResultName(OtaResult result) {
    switch (result) {
        case OtaResult::Ok:             return "ok";
        case OtaResult::AlreadyRunning: return "already_running";
        case OtaResult::NotFirmware:    return "not_firmware";
        case OtaResult::TooLarge:       return "too_large";
        case OtaResult::WriteFailed:    return "write_failed";
        case OtaResult::NotFinished:    return "not_finished";
        case OtaResult::NoPartition:    return "no_partition";
        case OtaResult::HashMismatch:   return "hash_mismatch";
    }
    return "unknown";
}

OtaResult OtaManager::finishHash() {
    Digest digest{};
    hasher_.finish(digest);
    writtenSha256_ = toHex(digest.data(), digest.size());
    if (expectedSha_.empty()) {
        return OtaResult::Ok;  // nothing was promised; a hand-picked file has nothing to check
    }
    if (!hexDigestEquals(expectedSha_, digest)) {
        // Both digests in the message. "Checksum failed" with no numbers leaves nobody able to
        // tell a corrupted download from having been handed the wrong file entirely.
        lastError_ = "image hashes to " + writtenSha256_ + ", expected " + expectedSha_;
        return OtaResult::HashMismatch;
    }
    return OtaResult::Ok;
}

bool looksLikeFirmware(const uint8_t* data, size_t len) {
    return data != nullptr && len >= 1 && data[0] == kEspImageMagic;
}

bool shouldConfirmHealthyBoot(bool wifiConnected, uint64_t uptimeMs, bool alreadyConfirmed,
                              bool pollingHealthy) {
    if (alreadyConfirmed) {
        return false;  // once per boot; the caller latches this
    }
    // Fast path -- networked is the health bar: this bridge is managed and recovered over
    // the network, so an image that never brings WiFi up should normally roll back.
    if (wifiConnected) {
        return uptimeMs >= kHealthyBootThresholdMs;
    }
    // Bounded fallback: WiFi-less but demonstrably working (see the header). Both the long
    // uptime AND a successful poll are required; neither alone is health.
    return pollingHealthy && uptimeMs >= kOfflineConfirmThresholdMs;
}

/// Shared by both builds on purpose -- see the declaration for why an #if between two copies of
/// this is the dangerous kind of duplication.
void OtaManager::startTracking(size_t expectedSize, const std::string& expectedSha256) {
    running_      = true;
    magicChecked_ = false;
    written_      = 0;
    expected_     = expectedSize;
    expectedSha_  = expectedSha256;
    writtenSha256_.clear();
    hasher_.reset();
    lastError_.clear();
}

#if defined(ESP32)

// Overrides the WEAK hook in the Arduino core (esp32-hal-misc.c). Without this override,
// initArduino() -- which runs BEFORE setup() -- sees a pending-verify OTA image, calls the
// default verifyOta() (returns true unconditionally) and marks the image valid on the
// spot. Any crash after that point is then a permanent crash loop: the bootloader sees a
// "valid" image and never rolls back. That is exactly how the 0.4.4 boot-crash bricked a
// bridge on 2026-07-21 -- the rollback we thought we had was being cancelled before one
// line of our code ran. Returning true here defers verification to us:
// shouldConfirmHealthyBoot() in loop() (WiFi up + 30 s) is the real confirmation, and an
// image that never gets there is rolled back by the bootloader on the next reset.
extern "C" bool verifyRollbackLater(void) { return true; }

const char* imageStateName() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t   state;
    if (running == nullptr || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return "unknown";
    }
    switch (state) {
        case ESP_OTA_IMG_NEW:            return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_VALID:          return "valid";
        case ESP_OTA_IMG_INVALID:        return "invalid";
        case ESP_OTA_IMG_ABORTED:        return "aborted";
        case ESP_OTA_IMG_UNDEFINED:      return "undefined";
        default:                         return "unknown";
    }
}

OtaResult OtaManager::begin(size_t expectedSize, const std::string& expectedSha256) {
    if (running_) {
        return OtaResult::AlreadyRunning;
    }
    // UPDATE_SIZE_UNKNOWN lets Update size it from the partition when the client sent no
    // Content-Length.
    const size_t size = expectedSize > 0 ? expectedSize : UPDATE_SIZE_UNKNOWN;
    if (!Update.begin(size, U_FLASH)) {
        lastError_ = Update.errorString();
        return Update.getError() == UPDATE_ERROR_SPACE ? OtaResult::TooLarge
                                                       : OtaResult::NoPartition;
    }
    startTracking(expectedSize, expectedSha256);
    return OtaResult::Ok;
}

OtaResult OtaManager::write(const uint8_t* data, size_t len) {
    if (!running_) {
        return OtaResult::NotFinished;
    }
    if (!magicChecked_) {
        // Check before writing anything: uploading the wrong file (a filesystem image, a
        // .zip, an HTML error page a proxy substituted) must not reach flash at all.
        if (!looksLikeFirmware(data, len)) {
            lastError_ = "uploaded data does not start with the ESP32 image magic (0xE9)";
            abort();
            return OtaResult::NotFirmware;
        }
        magicChecked_ = true;
    }
    // Explicit envelope check, matching the host stub, rather than relying solely on
    // Update.write()'s internal accounting: reject an over-large upload before it reaches
    // flash, and keep both build paths verifying the same contract.
    if (expected_ > 0 && written_ + len > expected_) {
        lastError_ = "more data than announced";
        abort();
        return OtaResult::TooLarge;
    }
    if (Update.write(const_cast<uint8_t*>(data), len) != len) {
        lastError_ = Update.errorString();
        abort();
        return OtaResult::WriteFailed;
    }
    hasher_.update(data, len);
    written_ += len;
    return OtaResult::Ok;
}

OtaResult OtaManager::end() {
    if (!running_) {
        return OtaResult::NotFinished;
    }
    // Before Update::end(), which is what flips the boot partition: a hash mismatch has to be
    // a refusal, not a reboot into the rollback path. Update's own verify would catch a
    // corrupt image too, but only by its internal accounting -- it cannot know this is not the
    // image the release feed promised.
    if (const OtaResult hashed = finishHash(); hashed != OtaResult::Ok) {
        abort();
        return hashed;
    }
    // Update::end(true) verifies the image and only then flips the boot partition. Until this
    // succeeds the currently running image stays the one that boots.
    if (!Update.end(true)) {
        lastError_ = Update.errorString();
        running_   = false;
        return OtaResult::WriteFailed;
    }
    running_ = false;
    return OtaResult::Ok;
}

void OtaManager::abort() {
    if (running_) {
        Update.abort();
        running_ = false;
    }
}

void confirmHealthyBoot() {
    // Cancels the rollback the bootloader armed on the first boot after an OTA. Safe to call
    // when no rollback is pending -- it simply reports ESP_ERR_OTA_ROLLBACK_INVALID_STATE,
    // which we ignore. The caller only reaches here once per healthy boot anyway.
    esp_ota_mark_app_valid_cancel_rollback();
}

#else  // !ESP32

// Host builds test looksLikeFirmware() and the state machine's refusals; there is no flash.

void confirmHealthyBoot() {}  // no bootloader/rollback off-device

const char* imageStateName() { return "unknown"; }  // no otadata off-device

OtaResult OtaManager::begin(size_t expectedSize, const std::string& expectedSha256) {
    if (running_) {
        return OtaResult::AlreadyRunning;
    }
    startTracking(expectedSize, expectedSha256);
    return OtaResult::Ok;
}

OtaResult OtaManager::write(const uint8_t* data, size_t len) {
    if (!running_) {
        return OtaResult::NotFinished;
    }
    if (!magicChecked_) {
        if (!looksLikeFirmware(data, len)) {
            lastError_ = "uploaded data does not start with the ESP32 image magic (0xE9)";
            abort();
            return OtaResult::NotFirmware;
        }
        magicChecked_ = true;
    }
    if (expected_ > 0 && written_ + len > expected_) {
        lastError_ = "more data than announced";
        abort();
        return OtaResult::TooLarge;
    }
    hasher_.update(data, len);
    written_ += len;
    return OtaResult::Ok;
}

OtaResult OtaManager::end() {
    if (!running_) {
        return OtaResult::NotFinished;
    }
    if (!magicChecked_) {
        running_ = false;
        return OtaResult::NotFirmware;
    }
    if (const OtaResult hashed = finishHash(); hashed != OtaResult::Ok) {
        running_ = false;
        return hashed;
    }
    running_ = false;
    return OtaResult::Ok;
}

void OtaManager::abort() { running_ = false; }

#endif

}  // namespace heliograph::ota
