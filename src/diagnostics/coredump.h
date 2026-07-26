// SPDX-License-Identifier: MIT
//
// Reading the crash dump the bootloader already writes.
//
// A panic writes a full ELF core dump to the `coredump` partition -- the partition has existed
// in both tables since the OTA layout was designed, and CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH has
// always been set in the prebuilt IDF config. Nothing ever read it. So the answer to "it
// rebooted at 03:00, why" was `reset_reason` as an integer, while a complete dump naming the
// faulting task sat in flash where only a cable and espcoredump.py could reach it (audit,
// 2026-07-26).
//
// This is the summary, not the dump. Retrieving the ELF itself still wants the host tool -- see
// docs/hardware.md -- but knowing a dump EXISTS, and which task died, is what turns "something
// happened" into a question worth taking the cable out for.

#pragma once

#include <cstdint>
#include <string>

namespace heliograph::diag {

/// What the crash left behind. `present` false means no valid dump is stored, which is the
/// normal state and the state after an erase.
///
/// The other fields are only meaningful when `present` is true; outputs must report them as
/// absent otherwise rather than as zero, since task "" at PC 0 is not a fact about anything.
struct CoredumpSummary {
    bool        present = false;
    std::string taskName;         ///< task that caused the exception, "" if the dump omits it
    uint32_t    programCounter = 0;
    uint32_t    version        = 0;  ///< core dump format version, for reading the ELF later
};

/// Reads the summary from flash. Call ONCE, at boot: it verifies a checksum over the whole
/// stored image, which is far too much work to repeat per REST request.
///
/// Host builds return `{}` -- there is no partition, and the diagnostics payload that formats
/// this stays fully testable because the formatting takes the struct, not the flash.
CoredumpSummary readCoredumpSummary();

/// Erases the stored dump. Returns false if nothing was erased (no dump, or the flash refused).
///
/// Worth having rather than leaving the partition to overwrite itself: with a dump present,
/// every later diagnostics read keeps reporting the same old crash, and an operator who has
/// dealt with it needs a way to say so. CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE is not set, so
/// a NEW crash does overwrite the old one -- this is about clearing a handled one, not about
/// making room.
bool eraseCoredump();

}  // namespace heliograph::diag
