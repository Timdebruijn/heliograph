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
// This is the summary, not the dump. Pulling the ELF still wants the host tool -- see
// docs/hardware.md -- but the summary now carries the exception cause, the faulting address and
// a sixteen-deep backtrace, which is usually the whole answer. "LoadProhibited at 0x00000000"
// is a null dereference, and no cable makes that clearer.
//
// The cable is for turning backtrace ADDRESSES into file and line, and only when the cause and
// the address were not enough.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace heliograph::diag {

/// What the crash left behind. `present` false means no valid dump is stored, which is the
/// normal state and the state after an erase.
///
/// The other fields are only meaningful when `present` is true; outputs must report them as
/// absent otherwise rather than as zero, since task "" at PC 0 is not a fact about anything.
/// How deep a backtrace the IDF summary can hold. Fixed by esp_core_dump_bt_info_t, not chosen
/// here -- mirrored as a constant so the host build and its tests do not need the IDF header.
inline constexpr size_t kMaxBacktrace = 16;

struct CoredumpSummary {
    bool        present = false;
    std::string taskName;         ///< task that caused the exception, "" if the dump omits it
    uint32_t    programCounter = 0;
    uint32_t    version        = 0;  ///< core dump format version, for reading the ELF later

    /// Why it faulted, as an Xtensa EXCCAUSE, and the address it was reaching for.
    ///
    /// These two answer the question before a single symbol is looked up: "LoadProhibited at
    /// 0x00000000" is a null dereference, and no ELF is needed to know that. Both were sitting
    /// in the summary the firmware already read and were thrown away.
    uint32_t exceptionCause = 0;
    uint32_t faultAddress   = 0;
    /// False when the dump records no usable cause. A cause of 0 counts as none: 0 is
    /// EXCCAUSE_ILLEGAL on the ISA, but an abort, a failed assert or a watchdog leaves the
    /// whole field zeroed, and naming that "IllegalInstruction" invents a fault.
    bool causeKnown = false;
    /// False when the cause is not one that HAS a faulting address. exc_vaddr is what a load or
    /// a store reached for; on any other cause it is leftover, and a 0 there reads exactly like
    /// a null-pointer dereference without being one.
    bool faultAddressKnown = false;

    /// The call stack at the moment of the fault, innermost first.
    ///
    /// This is what `programCounter` alone could never give. That single PC is where the panic
    /// handler was running, which is why decoding it landed in Cache_Freeze_ICache_Enable and
    /// said nothing about the fault (2026-07-28).
    uint32_t backtrace[kMaxBacktrace] = {};
    size_t   backtraceDepth           = 0;
    /// The IDF's own verdict on whether the stack walk stayed sane. A corrupted backtrace is
    /// still worth showing -- it is often right for the first frame or two -- but it must be
    /// shown as suspect rather than as fact.
    bool backtraceCorrupted = false;
};

/// The EXCCAUSE as a name, or nullptr when the code is not one this table knows.
///
/// Returned rather than formatted so the caller decides how an unknown code reads. Values from
/// xtensa/corebits.h, transcribed rather than included: this header is compiled on the host too,
/// where that file does not exist.
const char* exceptionCauseName(uint32_t cause);

/// Whether a faulting ADDRESS means anything for this cause.
///
/// Only a load or a store has one. Asked separately from the name because they are separate
/// questions, and answered from the same table because two lists about the same causes had
/// already drifted apart once.
bool causeHasFaultAddress(uint32_t cause);

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
