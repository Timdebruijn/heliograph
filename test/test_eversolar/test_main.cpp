// One binary for the whole Eversolar family — checksum, parser and driver — so the shared
// src/ is linked once here instead of three times, one per former suite. Each sub-suite keeps
// its own translation unit (checksum.cpp / parser.cpp / driver.cpp) and its file-local statics;
// this file owns the single Unity setUp/tearDown/main that the framework allows per binary.
#include <unity.h>

// Sub-suites, defined in the sibling translation units.
void run_eversolar_checksum();
void run_eversolar_parser();
void run_eversolar_driver();

// The driver suite runs against a fake clock; its original setUp reset that clock before every
// test. Only the driver tests use it, so calling this before the checksum/parser tests too is
// harmless — it just writes a value they never read.
void eversolar_driver_reset();

void setUp() { eversolar_driver_reset(); }
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    run_eversolar_checksum();
    run_eversolar_parser();
    run_eversolar_driver();
    return UNITY_END();
}
