// SPDX-License-Identifier: MIT

#include "log_buffer.h"

#include <cstring>

#if defined(ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace heliograph::log {
namespace {

char     g_lines[kLogBufferLines][kLogLineChars];
size_t   g_next  = 0;  ///< where the next line goes
size_t   g_held  = 0;  ///< lines currently stored, <= kLogBufferLines
uint32_t g_total = 0;  ///< lines ever pushed, including overwritten ones

#if defined(ESP32)
// A real mutex, not a spinlock: readers allocate strings while holding it, and allocating
// inside a critical section is how a logger takes down the device it was meant to explain.
SemaphoreHandle_t mutex() {
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}
struct Guard {
    Guard() {
        if (mutex() != nullptr) {
            xSemaphoreTake(mutex(), portMAX_DELAY);
        }
    }
    ~Guard() {
        if (mutex() != nullptr) {
            xSemaphoreGive(mutex());
        }
    }
};
#else
// Host tests are single-threaded, so this guards nothing -- but it keeps the RAII shape
// identical to the ESP32 build. The user-provided ctor/dtor pair is what stops the
// compiler from flagging every `Guard guard;` as an unused variable.
struct Guard {
    Guard() {}
    ~Guard() {}
};
#endif

}  // namespace

void pushLine(const char* line) {
    if (line == nullptr) {
        return;
    }
    Guard guard;
    std::strncpy(g_lines[g_next], line, kLogLineChars - 1);
    g_lines[g_next][kLogLineChars - 1] = '\0';
    g_next = (g_next + 1) % kLogBufferLines;
    if (g_held < kLogBufferLines) {
        ++g_held;
    }
    ++g_total;
}

std::vector<std::string> recentLines(size_t max) {
    Guard guard;
    const size_t take = max < g_held ? max : g_held;
    std::vector<std::string> out;
    out.reserve(take);
    // g_next points one past the newest line; walk back `take` and read forward from there so
    // the caller always gets chronological order.
    const size_t start = (g_next + kLogBufferLines - take) % kLogBufferLines;
    for (size_t i = 0; i < take; ++i) {
        out.emplace_back(g_lines[(start + i) % kLogBufferLines]);
    }
    return out;
}

uint32_t totalLines() {
    Guard guard;
    return g_total;
}

void clearLines() {
    Guard guard;
    g_next  = 0;
    g_held  = 0;
    g_total = 0;
}

}  // namespace heliograph::log
