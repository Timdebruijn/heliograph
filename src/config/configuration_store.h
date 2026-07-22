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
    explicit ConfigurationStore(KeyValueBackend& backend);

    /// Reads, migrates if needed, and validates. On anything other than Ok/Migrated, `out` is
    /// left at its defaults -- never partially populated.
    LoadResult load(Configuration& out);

    /// Writes atomically enough for NVS: one blob, one version. Returns false if the backend
    /// refused, so the caller can report a real failure instead of pretending it saved.
    bool save(const Configuration& config);

    /// Wipes everything, including credentials. Used by the provisioning reset.
    bool factoryReset();

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

/// Parses a stored blob, applying migrations from `storedVersion` to kConfigVersion.
LoadResult deserializeConfigFromStorage(const std::string& json, Configuration& out);

inline constexpr const char* kStorageNamespace = "heliograph";
inline constexpr const char* kStorageKeyConfig = "config";

}  // namespace heliograph
