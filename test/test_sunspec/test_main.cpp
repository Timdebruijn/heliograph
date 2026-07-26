// One binary for the SunSpec family — parser and driver — so the shared src/ is linked once
// here instead of once per former suite. Each sub-suite keeps its own translation unit and its
// file-local statics; this file owns the single Unity setUp/tearDown/main.
#include <unity.h>

void run_sunspec_parser();
void run_sunspec_driver();

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    run_sunspec_parser();
    run_sunspec_driver();
    return UNITY_END();
}
