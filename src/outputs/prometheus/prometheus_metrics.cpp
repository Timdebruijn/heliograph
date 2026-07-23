// SPDX-License-Identifier: MIT

#include "prometheus_metrics.h"

#include <cstdio>

namespace heliograph::prometheus {
namespace {

void appendHelp(std::string& out, const char* name, const char* help, const char* type) {
    out += "# HELP ";
    out += name;
    out += ' ';
    out += help;
    out += "\n# TYPE ";
    out += name;
    out += ' ';
    out += type;
    out += '\n';
}

void appendValue(std::string& out, const char* name, double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s %.3f\n", name, value);
    out += buf;
}

void appendValue(std::string& out, const char* name, unsigned long value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s %lu\n", name, value);
    out += buf;
}

void appendValue(std::string& out, const char* name, long value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s %ld\n", name, value);
    out += buf;
}

/// Exports a gauge from a measurement, or nothing at all if it is not a current reading.
void appendGauge(std::string& out, const DeviceState& state, const char* id, const char* name,
                 const char* help) {
    const auto* m = state.measurements.find(id);
    if (m == nullptr || !m->supported || !m->valid || m->stale) {
        return;  // omit, do not zero
    }
    appendHelp(out, name, help, "gauge");
    appendValue(out, name, m->value);
}

std::string escapeLabel(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace

std::string buildMetrics(const DeviceState& state, const BridgeInfo& bridge,
                         const DiagnosticsSnapshot& d) {
    std::string out;
    out.reserve(2048);

    appendHelp(out, "heliograph_build_info", "Firmware build information", "gauge");
    out += "heliograph_build_info{version=\"" + escapeLabel(bridge.firmwareVersion) +
           "\",driver=\"" + escapeLabel(state.identity.driverId) + "\",board=\"" +
           escapeLabel(bridge.boardName) + "\"} 1\n";

    appendHelp(out, "heliograph_inverter_online", "1 if the inverter is responding", "gauge");
    appendValue(out, "heliograph_inverter_online",
                static_cast<unsigned long>(state.inverterOnline ? 1 : 0));

    appendHelp(out, "heliograph_data_stale", "1 if the last reading is too old to trust", "gauge");
    appendValue(out, "heliograph_data_stale",
                static_cast<unsigned long>(state.dataStale ? 1 : 0));

    appendGauge(out, state, measurement_id::kAcPowerTotal, "heliograph_inverter_ac_power_watts",
                "Current AC output power");
    appendGauge(out, state, measurement_id::kDcPowerTotal, "heliograph_inverter_dc_power_watts",
                "Current DC input power (derived)");
    appendGauge(out, state, measurement_id::kAcL1Voltage, "heliograph_inverter_ac_voltage_volts",
                "AC voltage on L1");
    appendGauge(out, state, measurement_id::kAcL1Current, "heliograph_inverter_ac_current_amperes",
                "AC current on L1");
    appendGauge(out, state, measurement_id::kAcFrequency, "heliograph_inverter_grid_frequency_hertz",
                "Grid frequency");
    appendGauge(out, state, measurement_id::kEnergyToday, "heliograph_inverter_energy_today_kwh",
                "Energy produced today");
    appendGauge(out, state, measurement_id::kEnergyTotal, "heliograph_inverter_energy_total_kwh",
                "Lifetime energy produced");
    appendGauge(out, state, measurement_id::kTemperature,
                "heliograph_inverter_temperature_celsius", "Inverter temperature");

    appendHelp(out, "heliograph_poll_success_total", "Successful inverter polls", "counter");
    appendValue(out, "heliograph_poll_success_total",
                static_cast<unsigned long>(d.pollSuccessTotal));
    appendHelp(out, "heliograph_poll_failure_total", "Failed inverter polls", "counter");
    appendValue(out, "heliograph_poll_failure_total",
                static_cast<unsigned long>(d.pollFailureTotal));
    appendHelp(out, "heliograph_rs485_checksum_errors_total", "RS485 frames failing checksum",
               "counter");
    appendValue(out, "heliograph_rs485_checksum_errors_total",
                static_cast<unsigned long>(d.checksumErrorTotal));
    appendHelp(out, "heliograph_rs485_timeouts_total", "RS485 read timeouts", "counter");
    appendValue(out, "heliograph_rs485_timeouts_total",
                static_cast<unsigned long>(d.rs485TimeoutTotal));
    appendHelp(out, "heliograph_invalid_frames_total", "Structurally invalid RS485 frames",
               "counter");
    appendValue(out, "heliograph_invalid_frames_total",
                static_cast<unsigned long>(d.invalidFrameTotal));
    appendHelp(out, "heliograph_mqtt_reconnects_total", "MQTT reconnections", "counter");
    appendValue(out, "heliograph_mqtt_reconnects_total",
                static_cast<unsigned long>(d.mqttReconnectTotal));

    if (bridge.wifiConnected) {
        appendHelp(out, "heliograph_wifi_rssi_dbm", "WiFi signal strength", "gauge");
        appendValue(out, "heliograph_wifi_rssi_dbm", static_cast<long>(bridge.wifiRssiDbm));
    }
    appendHelp(out, "heliograph_uptime_seconds", "Bridge uptime", "gauge");
    appendValue(out, "heliograph_uptime_seconds", static_cast<unsigned long>(bridge.uptimeSeconds));
    appendHelp(out, "heliograph_free_heap_bytes", "Free heap", "gauge");
    appendValue(out, "heliograph_free_heap_bytes", static_cast<unsigned long>(bridge.freeHeapBytes));
    appendHelp(out, "heliograph_max_alloc_heap_bytes",
               "Largest allocatable heap block (fragmentation signal)", "gauge");
    appendValue(out, "heliograph_max_alloc_heap_bytes",
                static_cast<unsigned long>(bridge.maxAllocHeapBytes));
    // Omitted until the first sample, like the RSSI gauge above: 0 would read as an
    // exhausted stack to any alerting rule.
    if (d.rs485StackFreeBytes > 0) {
        appendHelp(out, "heliograph_rs485_stack_free_bytes",
                   "rs485Task stack low-water mark", "gauge");
        appendValue(out, "heliograph_rs485_stack_free_bytes",
                    static_cast<unsigned long>(d.rs485StackFreeBytes));
    }
    if (d.loopStackFreeBytes > 0) {
        appendHelp(out, "heliograph_loop_stack_free_bytes",
                   "loopTask stack low-water mark", "gauge");
        appendValue(out, "heliograph_loop_stack_free_bytes",
                    static_cast<unsigned long>(d.loopStackFreeBytes));
    }

    return out;
}

}  // namespace heliograph::prometheus
