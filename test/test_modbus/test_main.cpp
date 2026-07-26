// One binary for the Modbus family — the RTU framing codec and the client — so the shared src/
// is linked once here instead of once per former suite. Each sub-suite keeps its own translation
// unit and its file-local statics; this file owns the single Unity setUp/tearDown/main.
#include <unity.h>

void run_modbus_rtu();
void run_modbus_client();

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    run_modbus_rtu();
    run_modbus_client();
    return UNITY_END();
}
