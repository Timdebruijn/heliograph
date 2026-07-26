// SPDX-License-Identifier: MIT
//
// Configuration persistence and migration.
//
// NVS sits behind KeyValueBackend so that load/save/migrate can be host-tested. Migration
// logic you cannot test is migration logic that eats someone's configuration on the first
// firmware update, and you find out from a bug report.

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "configuration.h"
#include "outputs/mqtt/announced_devices.h"

namespace heliograph {

/// Minimal key/value persistence. The whole configuration is stored as one JSON blob plus a
/// version key -- simpler than a key per field, and migration becomes ordinary JSON work.
class KeyValueBackend {
public:
    virtual ~KeyValueBackend() = default;
    virtual bool read(const std::string& key, std::string& value) const = 0;
    virtual bool write(const std::string& key, const std::string& value) = 0;
    virtual bool erase()                                                 = 0;
};

/// In-memory backend for tests.
class MemoryBackend : public KeyValueBackend {
public:
    bool read(const std::string& key, std::string& value) const override;
    bool write(const std::string& key, const std::string& value) override;
    bool erase() override;

    /// Fault injection: makes every write fail, as a worn or full flash would.
    bool writeFails = false;

    size_t size() const { return values_.size(); }
    bool   contains(const std::string& key) const;
    /// Lets a test inspect exactly what hit the flash.
    std::string raw(const std::string& key) const;

private:
    std::map<std::string, std::string> values_;
};

enum class LoadResult {
    Ok,
    /// Nothing stored yet: first boot. `out` keeps its defaults.
    NotFound,
    /// Stored blob is unreadable. Defaults are used rather than a half-parsed config.
    Corrupt,
    /// Stored by a NEWER firmware than this one. Refused rather than guessed at: a downgrade
    /// must not silently reinterpret fields it does not understand.
    FutureVersion,
    Migrated,
};

const char* loadResultName(LoadResult result);

class ConfigurationStore {
public:
    /// `legacy`, when given, is a read-only fallback: if the primary backend holds no config
    /// but the legacy one does, load() adopts that blob and writes it to the primary. Exists
    /// because the project rename (0.5.0) changed the NVS namespace, which stranded every
    /// existing configuration under the old name -- a device that updated over the air came
    /// up "unprovisioned" with its config sitting intact in flash (live, 2026-07-22, Tim's
    /// bridge). The legacy blob is left in place on purpose: a rollback to a 0.4.x image
    /// must still find it.
    explicit ConfigurationStore(KeyValueBackend& backend, KeyValueBackend* legacy = nullptr);

    /// Reads, migrates if needed, and validates. On anything other than Ok/Migrated, `out` is
    /// left at its defaults -- never partially populated.
    LoadResult load(Configuration& out);

    /// Writes atomically enough for NVS: one blob, one version. Returns false if the backend
    /// refused, so the caller can report a real failure instead of pretending it saved.
    bool save(const Configuration& config);

    /// Wipes everything, including credentials. Used by the provisioning reset.
    bool factoryReset();

    /// Bookkeeping the firmware keeps about itself, kept OUT of Configuration on purpose: it is
    /// not something a user sets, it must never appear in GET /config, and it must not take
    /// part in the change-diff that decides whether a save needs a reboot.
    ///
    /// Currently one thing: which devices have been announced to Home Assistant, and on which
    /// topic tree. Discovery configs are retained on the broker, so a device that is removed or
    /// re-addressed leaves its entities behind -- and because availability is bridge-scoped they
    /// report ONLINE forever with their last value, straight into an energy dashboard. Clearing
    /// them needs the one fact nothing else survives a reboot with: what we announced last time.
    std::vector<mqtt::AnnouncedDevice> announcedDevices();
    bool setAnnouncedDevices(const std::vector<mqtt::AnnouncedDevice>& devices);

    /// Copies the stored configuration into the rollback slot, so the restore about to
    /// overwrite it can be undone. Call immediately before a restore, and nowhere else: doing
    /// it on every save would double the write wear and, worse, make "the configuration from
    /// before the restore" mean nothing in particular.
    ///
    /// False when nothing is stored yet (a factory-fresh board has nothing to roll back to)
    /// or when the write was refused. A refusal is not a reason to abandon the restore -- NVS
    /// here is 20 KB shared with the WiFi stack's own calibration data, so a second copy of
    /// the configuration is the first thing that will not fit, and losing the safety net is
    /// a smaller harm than refusing the operation the safety net was for. The caller reports
    /// it instead.
    bool stashRollback();

    /// True when a rollback copy is available.
    bool hasRollback();

    /// SWAPS the rollback copy with the live configuration, and returns the now-live one.
    ///
    /// A swap rather than a one-shot restore, because the two writes cost the same and the
    /// swap is what makes the button honest when pressed twice: undo, look, undo again. A
    /// one-shot would leave the second press doing nothing with no way to say why.
    ///
    /// NotFound when there is no rollback copy. On any parse failure the live configuration
    /// is left exactly as it was -- a rollback that half-applies is worse than one that
    /// refuses, because the operator reaching for it is already recovering from something.
    LoadResult rollback(Configuration& out);

private:
    KeyValueBackend&   backend_;
    KeyValueBackend*   legacy_;
    mutable std::mutex mutex_;
};

/// Serialises INCLUDING secrets. For ConfigurationStore only.
///
/// Deliberately not called serializeConfig: that one omits every password and is what the
/// REST API uses. Two functions with clearly different names, so reaching for the wrong one
/// is a visible mistake rather than a silent leak.
bool serializeConfigForStorage(const Configuration& config, std::string& out);

/// Parses a stored blob, applying migrations from `storedVersion` to kConfigVersion.
LoadResult deserializeConfigFromStorage(const std::string& json, Configuration& out);

inline constexpr const char* kStorageNamespace = "heliograph";
/// The pre-rename namespace (project was called differently before 0.5.0). Read-only
/// migration source; never written to.
inline constexpr const char* kLegacyStorageNamespace = "solarbridge";
inline constexpr const char* kStorageKeyConfig = "config";
/// Separate key, not a field in the config blob: bookkeeping must not be able to make a user's
/// configuration unloadable, and a factory reset should take it with everything else.
inline constexpr const char* kStorageKeyAnnounced = "announced";
/// The configuration as it was before the last restore. Its own key rather than a second blob
/// inside the config document: it must never be able to make the LIVE configuration
/// unloadable, and a factory reset (which clears the whole namespace) must take it along.
///
/// NVS keys are capped at 15 characters, so this cannot be spelled "config.previous".
inline constexpr const char* kStorageKeyRollback = "config.prev";

}  // namespace heliograph
