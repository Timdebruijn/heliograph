// SPDX-License-Identifier: MIT

#include "nvs_backend.h"

#if defined(ESP32)

#include <Preferences.h>

namespace heliograph {

// NOTE: NVS is not encrypted unless flash encryption is enabled. The stored blob therefore
// contains the WiFi and MQTT passwords in clear text, readable by anyone who can dump the
// flash over USB. Documented in docs/security.md rather than papered over -- physical access
// to the board is outside this threat model, but the user should know.

bool NvsBackend::read(const std::string& key, std::string& value) const {
    Preferences prefs;
    if (!prefs.begin(kStorageNamespace, /*readOnly=*/true)) {
        return false;
    }
    if (!prefs.isKey(key.c_str())) {
        prefs.end();
        return false;
    }
    value = prefs.getString(key.c_str(), "").c_str();
    prefs.end();
    // The key exists (checked above), so report found even if the stored value is empty --
    // the contract is "true iff the key exists". Gating on non-empty would make a legitimately
    // empty value indistinguishable from a missing key, a trap for any future empty-valued key.
    return true;
}

bool NvsBackend::write(const std::string& key, const std::string& value) {
    Preferences prefs;
    if (!prefs.begin(kStorageNamespace, /*readOnly=*/false)) {
        return false;
    }
    const size_t written = prefs.putString(key.c_str(), value.c_str());
    prefs.end();
    // A short write means the entry did not fit. Reporting it lets the REST layer return a
    // real error instead of pretending the settings were saved.
    return written == value.size();
}

bool NvsBackend::erase() {
    Preferences prefs;
    if (!prefs.begin(kStorageNamespace, /*readOnly=*/false)) {
        return false;
    }
    const bool ok = prefs.clear();
    prefs.end();
    return ok;
}

}  // namespace heliograph

#else

namespace heliograph {
bool NvsBackend::read(const std::string&, std::string&) const { return false; }
bool NvsBackend::write(const std::string&, const std::string&) { return false; }
bool NvsBackend::erase() { return false; }
}  // namespace heliograph

#endif
