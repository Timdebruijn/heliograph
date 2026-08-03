// SPDX-License-Identifier: MIT
//
// The real EverSolar driver against the simulated inverter, plus the BridgeInfo that goes with
// it -- the fixture the MQTT and REST suites both poll to get a populated DeviceState.
//
// These forty-six lines were byte-for-byte identical in test_mqtt and test_rest, including five
// literal bridge values (id, RSSI, uptime, heap, firmware version). Those literals are what made
// it worth extracting rather than tolerating: the two suites assert on the SAME payload builders
// reading the SAME BridgeInfo, so the day one copy's uptime or version drifted, one suite would
// have started describing a bridge the other did not, with nothing to say which was right.
//
// The fake clock lives here too, because Rig::poll() feeds it to DeviceContext. It keeps the
// name `g_now` deliberately: the two suites reference it about ninety times between them, and
// renaming those to gain a shared declaration would have been churn rather than cleanup. Pull it
// in with `using heliograph::test::g_now;` and every existing call site still reads the same.

#pragma once

#include <cstdint>

#include "device/device_context.h"
#include "device/device_state.h"
#include "diagnostics/diagnostics.h"
#include "drivers/eversolar_legacy/eversolar_driver.h"
#include "state/state_store.h"
#include "support/fake_eversolar_device.h"
#include "support/mock_transport.h"

namespace heliograph::test {

/// Settable "now", in milliseconds. Each suite's setUp() chooses the starting value.
inline uint64_t g_now = 0;
inline uint64_t clockFn() { return g_now; }

inline BridgeInfo makeBridge() {
    BridgeInfo b;
    b.bridgeId        = "heliograph-a1b2c3";
    b.bridgeOnline    = true;
    b.wifiConnected   = true;
    b.wifiRssiDbm     = -57;
    b.uptimeSeconds   = 86400;
    b.freeHeapBytes   = 180000;
    b.firmwareVersion = "0.1.0";
    return b;
}

/// The real EverSolar driver against the simulated inverter.
struct Rig {
    MockTransport              transport;
    FakeEversolarDevice        device;
    eversolar::EversolarDriver driver{transport};
    StateStore                 store;
    Diagnostics                diagnostics;

    Rig() {
        device.installOn(transport);
        driver.begin(transport);
    }
    DeviceState poll() {
        DeviceContext ctx(driver, store, diagnostics, clockFn);
        ctx.pollOnce();
        return *store.snapshot();
    }
};

}  // namespace heliograph::test
