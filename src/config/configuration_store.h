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

/// Ceiling on the serialised configuration blob.
///
/// NVS caps a single string entry at 4000 bytes. This sits just under it so a config that has
/// outgrown the store is REFUSED at serialise time rather than written truncated -- a truncated
/// blob fails to parse on the next boot and presents as corruption, which is a much worse
/// symptom than a save that says no. Named here because config_backup.h needs the same number
/// to size its own envelope bound, and a value restated in prose in a second file is a value
/// that will eventually be wrong in one of them.
inline constexpr size_t kMaxStoredConfigBytes = 3900;

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
    explicit ConfigurationStore(KeyValueBackend& backend);

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
    /// What was last successfully announced, and the tree it went to. An older record carries
    /// the ids with empty prefixes -- unknown, not "the default".
    ///
    /// Deliberately the only accessor. A convenience that returned the device list alone existed
    /// briefly and had no caller outside its own tests: every real user of this needs to know
    /// WHERE the ids were announced, because that is the fact nothing else survives a reboot
    /// with. Handing out the list without it invites exactly the reasoning this record exists to
    /// prevent.
    mqtt::AnnouncementRecord announcement();
    bool setAnnouncement(const mqtt::AnnouncementRecord& record);

    /// save(), with the configuration it replaces kept as the undo copy. What a restore calls,
    /// and nothing else: doing it on every save would double the write wear and, worse, make
    /// "the configuration from before the restore" mean nothing in particular.
    ///
    /// One call rather than a stash followed by a save, because the order between the two is
    /// what went wrong when they were separate: the stash ran first, so a restore that was then
    /// refused had already replaced the undo copy of the restore before it. Here nothing is
    /// touched until `config` has passed validate() and fits the stored size, and if the flash
    /// then refuses its write after the stash, the undo copy is put back as it was.
    ///
    /// `undoStored` is false when there was nothing to keep (a factory-fresh board) or the
    /// stash was refused. An undo copy from an earlier restore is then emptied, best effort,
    /// so that undo does not reach back past the restore that just happened. A refused stash
    /// is not a reason to abandon the restore -- NVS here is 20 KB shared with the WiFi stack's
    /// own calibration data, so a second copy of the configuration is the first thing that
    /// will not fit, and losing the safety net is a smaller harm than refusing the operation
    /// the safety net was for. The caller reports it instead.
    bool saveKeepingUndo(const Configuration& config, bool& undoStored);

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

    /// What rollback() would make live, without making it live: the undo point, parsed, and
    /// nothing written. For a preview, so the question "undo?" can say where the undo goes.
    ///
    /// `sameAsLive` is true when the undo point holds byte for byte what is stored as the live
    /// configuration. That is the one case a list of differences cannot show for what it is:
    /// an undo that would change nothing, which is what a swap leaves behind when its second
    /// write was refused. Results as for rollback().
    LoadResult peekRollback(Configuration& out, bool& sameAsLive);

private:
    KeyValueBackend&   backend_;
    mutable std::mutex mutex_;
};

/// Serialises INCLUDING secrets. For ConfigurationStore only.
///
/// Deliberately not called serializeConfig: that one omits every password and is what the
/// REST API uses. Two functions with clearly different names, so reaching for the wrong one
/// is a visible mistake rather than a silent leak.
bool serializeConfigForStorage(const Configuration& config, std::string& out);

/// How many bytes the stored blob of `config` needs, whether or not that fits
/// kMaxStoredConfigBytes. 0 when the document could not be built at all (out of memory).
///
/// For the one caller that has to explain a refused save: validate() bounds every string but
/// not their sum, so a configuration can pass it and still be too large to store.
size_t storedConfigSize(const Configuration& config);

/// Parses a stored blob, applying migrations from `storedVersion` to kConfigVersion.
LoadResult deserializeConfigFromStorage(const std::string& json, Configuration& out);

inline constexpr const char* kStorageNamespace = "heliograph";
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
