// SPDX-License-Identifier: MIT

#include "status_led.h"

namespace heliograph::status {

LedIndication decide(const LedInputs& in) {
    // 1. Not configured yet: the light should invite setup, not read as a fault.
    if (!in.provisioned) {
        return {LedColor::Blue, false};
    }
    // 2. A factory-reset hold is in progress: show it, blinking, over any health state --
    //    the owner is holding the button and needs to see it register.
    if (in.factoryResetHolding) {
        return {LedColor::Red, true};
    }
    // 3. No WiFi: nothing reaches anyone regardless of the inverter, but it is a
    //    reconnecting-degraded state, not the core job failing -- amber.
    if (!in.wifiConnected) {
        return {LedColor::Amber, false};
    }
    // 4. The core job: no live inverter data. This is the one red-worthy steady state --
    //    but only when a driver is configured. A relay-only board expects no data.
    if (in.inverterExpected && !in.inverterOnline) {
        return {LedColor::Red, false};
    }
    // 5. Online but the data is not fresh/valid -- working, one detail off.
    if (in.inverterExpected && (in.dataStale || !in.dataValid)) {
        return {LedColor::Amber, false};
    }
    // 6/7. An output the owner DID enable is not carrying data. A disabled output is
    //      silent here on purpose (mqttEnabled / modbusEnabled gate), so an unused
    //      integration never dims the light.
    if (in.mqttEnabled && !in.mqttConnected) {
        return {LedColor::Amber, false};
    }
    if (in.modbusEnabled && !in.modbusListening) {
        return {LedColor::Amber, false};
    }
    // 8. WiFi up, real fresh data, and every enabled output healthy.
    return {LedColor::Green, false};
}

const char* colorName(LedColor color) {
    switch (color) {
        case LedColor::Green: return "green";
        case LedColor::Amber: return "amber";
        case LedColor::Red:   return "red";
        case LedColor::Blue:  return "blue";
        case LedColor::Off:   return "off";
    }
    return "off";
}

}  // namespace heliograph::status
