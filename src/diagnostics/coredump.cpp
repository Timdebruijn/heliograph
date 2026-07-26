// SPDX-License-Identifier: MIT

#include "coredump.h"

#if defined(ESP32)

#include <esp_core_dump.h>

#include <cstring>

namespace heliograph::diag {

CoredumpSummary readCoredumpSummary() {
    CoredumpSummary out;
    // image_check verifies the stored checksum, so a partition holding a half-written dump --
    // the plausible outcome of a panic during the panic handler -- reports absent rather than
    // handing back a garbage task name.
    if (esp_core_dump_image_check() != ESP_OK) {
        return out;
    }
    esp_core_dump_summary_t summary{};
    if (esp_core_dump_get_summary(&summary) != ESP_OK) {
        // A dump that passes its checksum but cannot be summarised is still a dump: say it is
        // there. The ELF is retrievable with the host tool even when this call cannot parse it.
        out.present = true;
        return out;
    }
    out.present        = true;
    out.programCounter = summary.exc_pc;
    out.version        = summary.core_dump_version;
    // exc_task is a fixed 16-byte field and is NOT guaranteed to be NUL-terminated when the
    // name uses all of it. strnlen bounds it; taking strlen would run into whatever follows.
    out.taskName.assign(summary.exc_task,
                        strnlen(summary.exc_task, sizeof(summary.exc_task)));
    return out;
}

bool eraseCoredump() { return esp_core_dump_image_erase() == ESP_OK; }

}  // namespace heliograph::diag

#else

namespace heliograph::diag {

// No partition off-device. Returning an empty summary keeps main.cpp free of #if blocks, and
// the payload builder that formats this is host-tested against constructed structs anyway.
CoredumpSummary readCoredumpSummary() { return {}; }
bool            eraseCoredump() { return false; }

}  // namespace heliograph::diag

#endif
