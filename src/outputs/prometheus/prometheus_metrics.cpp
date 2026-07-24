// SPDX-License-Identifier: MIT

#include "prometheus_metrics.h"

#include <cstdio>
#include <vector>

#include "relays/drm.h"

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
    appendHelp(out, "heliograph_wifi_reconnects_total", "WiFi reconnections", "counter");
    appendValue(out, "heliograph_wifi_reconnects_total",
                static_cast<unsigned long>(d.wifiReconnectTotal));
    appendHelp(out, "heliograph_modbus_client_connections_total",
               "Modbus TCP client connections accepted", "counter");
    appendValue(out, "heliograph_modbus_client_connections_total",
                static_cast<unsigned long>(d.modbusClientConnections));
    appendHelp(out, "heliograph_modbus_clients", "Modbus TCP clients connected right now",
               "gauge");
    appendValue(out, "heliograph_modbus_clients", static_cast<unsigned long>(bridge.modbusClients));

    // Clock. The timestamp is omitted rather than exported as 0 while the clock is unset: a
    // zero here is 1970, and any "last sync was long ago" rule would fire permanently on a
    // device that has simply never synced.
    appendHelp(out, "heliograph_time_synced", "1 if the system clock has been set from NTP",
               "gauge");
    appendValue(out, "heliograph_time_synced",
                static_cast<unsigned long>(bridge.timeSynced ? 1 : 0));
    if (bridge.timeSynced && bridge.lastNtpSyncEpoch > 0) {
        appendHelp(out, "heliograph_ntp_last_sync_timestamp_seconds",
                   "Unix time of the last successful NTP sync", "gauge");
        appendValue(out, "heliograph_ntp_last_sync_timestamp_seconds",
                    static_cast<unsigned long>(bridge.lastNtpSyncEpoch));
    }

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

    // Relays and DRM last, on boards that have them. Absent entirely on a board without
    // relays, like every other output -- hardware that does not exist is never reported as a
    // zero. Kept at the end so a raw curl reads in the same order the documentation groups it.
    //
    // The relay label is the SAME index the MQTT topic and the REST route use (0-based), so a
    // dashboard series and the topic that switched it line up. The web UI numbers them from 1
    // for humans; machine interfaces agree on 0.
    if (bridge.relayCount > 0) {
        appendHelp(out, "heliograph_relays_enabled",
                   "1 if the relay feature is enabled in the configuration", "gauge");
        appendValue(out, "heliograph_relays_enabled",
                    static_cast<unsigned long>(bridge.relaysEnabled ? 1 : 0));

        // One HELP/TYPE for the whole family, then one line per relay. Repeating the header
        // per series is a parse error in strict parsers (pinned by a test).
        appendHelp(out, "heliograph_relay_energised",
                   "1 if the relay coil is energised (DRM line asserted)", "gauge");
        for (uint8_t i = 0; i < bridge.relayCount; ++i) {
            const bool on = ((bridge.relayMask >> i) & 1) != 0;
            out += "heliograph_relay_energised{relay=\"" + std::to_string(i) + "\"} ";
            out += on ? "1\n" : "0\n";
        }

        // Only when the configured roles make a mode meaningful, matching the REST payload and
        // the Home Assistant select: with no roles assigned there is no DRM vocabulary to
        // report, and an invented "normal" would claim a curtailment model that is not set up.
        std::vector<std::string> roles = bridge.relayRoles;
        roles.resize(bridge.relayCount, "none");
        if (!drm::optionsFor(roles).empty()) {
            appendHelp(out, "heliograph_drm_mode",
                       "Active DRM mode; the mode label carries the value", "gauge");
            out += "heliograph_drm_mode{mode=\"" +
                   escapeLabel(drm::modeFrom(roles, bridge.relayMask)) + "\"} 1\n";
        }
    }

    return out;
}

}  // namespace heliograph::prometheus
