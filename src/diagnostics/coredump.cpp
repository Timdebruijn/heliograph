// SPDX-License-Identifier: MIT

#include "coredump.h"

namespace heliograph::diag {

/// Xtensa EXCCAUSE values, from xtensa/corebits.h. Only the ones a firmware fault plausibly
/// produces: the TLB and PIF causes exist on the architecture but not on this part in any way
/// a driver bug reaches, and listing them would suggest a precision this table does not have.
const char* exceptionCauseName(uint32_t cause) {
    switch (cause) {
        case 0:  return "IllegalInstruction";
        case 1:  return "Syscall";
        case 2:  return "InstructionFetchError";
        case 3:  return "LoadStoreError";
        case 5:  return "Alloca";
        case 6:  return "DivideByZero";
        case 7:  return "IllegalPC";
        case 8:  return "PrivilegedInstruction";
        case 9:  return "UnalignedLoadStore";
        case 20: return "InstructionProhibited";
        case 28: return "LoadProhibited";
        case 29: return "StoreProhibited";
        default: return nullptr;
    }
}

}  // namespace heliograph::diag

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

    // The two fields that answer "why" without an ELF. exc_cause is an Xtensa EXCCAUSE and
    // exc_vaddr is the address the faulting access reached for -- together they turn a reboot
    // into "a null dereference on a load" before anything is decoded.
    out.exceptionCause = summary.ex_info.exc_cause;
    out.faultAddress   = summary.ex_info.exc_vaddr;

    // A CAUSE OF 0 IS TREATED AS NO CAUSE, and that is a deliberate trade.
    //
    // 0 is EXCCAUSE_ILLEGAL on the ISA, so this gives up the ability to name a genuine illegal
    // instruction. It is still right: a panic that is not a CPU exception at all -- an abort, a
    // failed assert, a watchdog -- leaves the whole ex_info zeroed, and that is far and away
    // the common case. Reporting a zeroed field as "IllegalInstruction at 0x00000000" invents
    // a fault, which is the one thing this project does not do with a reading.
    //
    // Found on a real dump from a production bridge: cause 0, vaddr 0, and a fault PC inside
    // the chip's own ROM. An illegal instruction in Espressif's ROM is not a thing that
    // happens; an abort walking out through the panic handler is (2026-07-28).
    out.causeKnown = summary.ex_info.exc_cause != 0;

    // The faulting address only means something for a fault that HAS one. exc_vaddr is the
    // address a load or a store reached for; on a divide-by-zero or an illegal instruction it
    // is whatever happened to be in the field, and 0 there is noise wearing the shape of a
    // null-pointer dereference.
    switch (summary.ex_info.exc_cause) {
        case 3:   // LoadStoreError
        case 9:   // UnalignedLoadStore
        case 26:  // LoadStoreRing
        case 28:  // LoadProhibited
        case 29:  // StoreProhibited
            out.faultAddressKnown = true;
            break;
        default:
            out.faultAddressKnown = false;
            break;
    }

    // The call stack, innermost first. Bounded by BOTH the reported depth and the array size:
    // depth comes out of a dump that has already survived a crash, and trusting it alone would
    // read past the end of a fixed 16-entry array if it were wrong.
    // The bound is only a bound if the two sizes agree. kMaxBacktrace is mirrored in the header
    // so the host build needs no IDF include, and a mirror is exactly the kind of constant that
    // drifts silently: were the IDF ever to shrink bt[], this loop would read past its end while
    // still looking careful.
    static_assert(sizeof(summary.exc_bt_info.bt) / sizeof(summary.exc_bt_info.bt[0]) ==
                      kMaxBacktrace,
                  "kMaxBacktrace no longer matches esp_core_dump_bt_info_t::bt");
    const size_t depth = summary.exc_bt_info.depth < kMaxBacktrace
                             ? summary.exc_bt_info.depth
                             : kMaxBacktrace;
    for (size_t i = 0; i < depth; ++i) {
        out.backtrace[i] = summary.exc_bt_info.bt[i];
    }
    out.backtraceDepth      = depth;
    out.backtraceCorrupted  = summary.exc_bt_info.corrupted;
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
