// SPDX-License-Identifier: MIT
//
// Configuration backup and restore: one downloadable file, and the rules for reading it back.
//
// The file is deliberately NOT a new serialisation of the configuration. It is a thin envelope
// around the document ConfigurationStore already writes to NVS, which means the field list has
// exactly one point of truth (buildStorageDocument, via serializeConfigForStorage) and a
// setting added tomorrow lands in the backup without anyone remembering to add it here. The
// alternative -- a second hand-maintained field list -- fails silently and is discovered by
// someone whose restore quietly dropped a setting.
//
// Reading it back reuses deserializeConfigFromStorage for the same reason, and inherits its
// migration chain for free: a backup taken on an older firmware is upgraded on the way in,
// exactly as a stored config would be at boot.
//
// SECRETS. A backup omits every password unless the operator explicitly asks for them. That is
// not squeamishness: this file lands in a downloads folder, syncs to a cloud drive, and gets
// attached to issues. What makes the redacted form usable rather than merely safe is the merge
// rule below -- an absent password means "keep what the bridge already has", so a redacted
// backup restored onto a running bridge is complete. Only on a factory-fresh board does it
// leave the credentials unset, and that board comes up in the setup portal, which is the
// recovery path anyway.

#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "configuration.h"

namespace heliograph {

/// Identifies the file. Checked on restore, so a JSON document that is not one of ours is
/// refused with an explanation rather than half-applied.
inline constexpr const char* kBackupFormat        = "heliograph-config-backup";
/// The ENVELOPE version, independent of kConfigVersion inside it. Bumped only when the
/// envelope's own shape changes; the configuration within it versions itself.
inline constexpr uint16_t    kBackupFormatVersion = 1;

/// Upper bound on a backup we will accept. The configuration itself cannot exceed 3900 bytes
/// (NVS refuses to store more), and the envelope adds a couple of hundred; the rest is slack
/// for pretty-printing by whoever edited the file in between.
inline constexpr size_t kMaxBackupBytes = 8192;

struct BackupOptions {
    /// Off by default, and the UI has to ask. See the note at the top of this file.
    bool        includeSecrets = false;
    /// Informational only -- recorded so a restore that goes wrong can be traced to the image
    /// that produced the file. Never used to accept or refuse anything: firmware version and
    /// config compatibility are different questions, and conflating them would refuse a
    /// perfectly restorable file because the patch number moved.
    std::string firmwareVersion;
    /// ISO-8601 UTC. Empty when the bridge has no synced clock -- an absent timestamp is
    /// honest, a 1970 one is a lie that later reads as a real date.
    std::string exportedAt;
};

/// Renders `epoch` as "YYYY-MM-DDTHH:MM:SSZ". Returns empty when the epoch is implausible as a
/// wall clock (before 2020), which is what an unsynced bridge has.
std::string isoUtcTimestamp(time_t epoch);

/// Builds the backup document. False only when the underlying configuration cannot be
/// serialised at all -- in practice, one too large for NVS, which cannot have been stored.
bool buildConfigBackup(const Configuration& config, const BackupOptions& options,
                       std::string& out);

enum class BackupResult : uint8_t {
    Ok,
    /// Not JSON, or not a JSON object.
    NotJson,
    /// Valid JSON, but missing or wrong `format`. The likeliest cause by far is the wrong
    /// file, which is why it is told apart from the versions below.
    NotABackup,
    /// Envelope written by a newer firmware than this one understands.
    FutureFormat,
    /// The configuration inside was written by a newer firmware. Refused rather than
    /// reinterpreted -- same rule, and the same reason, as loading from NVS.
    FutureConfig,
    /// The configuration inside is unreadable, or does not pass validate().
    Corrupt,
};
const char* backupResultName(BackupResult result);

/// A parsed backup, before anything has been applied.
///
/// The three `has*Password` flags are the whole reason this is a struct and not just a
/// Configuration: `config` carries an EMPTY password both when the file redacted it and when
/// the file recorded an empty one, and those two mean opposite things at merge time.
struct BackupContents {
    Configuration config;
    bool          hasWifiPassword  = false;
    bool          hasMqttPassword  = false;
    bool          hasAdminPassword = false;
    /// What the file says about itself. Only ever used to explain the file to the operator;
    /// the flags above are what the merge acts on, because they describe what is actually
    /// in the document rather than what its header claims.
    bool          includesSecrets = false;
    uint16_t      formatVersion   = 0;
    std::string   firmwareVersion;
    std::string   exportedAt;
};

/// Parses and validates. `detail` receives a sentence naming what was wrong, for the API to
/// hand back verbatim -- "rejected" with no reason is the failure mode this exists to avoid.
BackupResult parseConfigBackup(const std::string& json, BackupContents& out, std::string& detail);

/// Merges a parsed backup onto `target`.
///
/// Everything that is not a credential is replaced outright. Each credential is replaced ONLY
/// when the file actually carried it, so a redacted backup keeps the bridge's own passwords
/// and cannot lock anyone out of the device they just restored.
void applyBackup(const BackupContents& backup, Configuration& target);

/// One changed setting, ready to render. Values are already strings: the preview is for a
/// human deciding whether to press the button, not for a machine.
struct ConfigDiffEntry {
    std::string field;   ///< dotted path, e.g. "mqtt.host"
    std::string before;
    std::string after;
};

/// Every setting that differs between the two configurations, in stored-document order.
///
/// Computed by walking the two storage documents rather than the two structs: the walk covers
/// a field added later without being told about it, which a hand-written comparison does not.
///
/// Passwords are never rendered as values. Any key named `password` or ending in `_password`
/// comes out as "(set)" or "(not set)" -- a rule rather than a list of three, so a credential
/// added in future is redacted before anyone has to remember that it should be.
///
/// False only if either configuration fails to serialise.
bool diffConfigurations(const Configuration& before, const Configuration& after,
                        std::vector<ConfigDiffEntry>& out);

}  // namespace heliograph
