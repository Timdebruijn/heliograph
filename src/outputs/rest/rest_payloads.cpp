// SPDX-License-Identifier: MIT

#include "rest_payloads.h"

#include <cstdio>

#include <ArduinoJson.h>

#include "outputs/json_util.h"
#include "relays/drm.h"

namespace heliograph::rest {
namespace {

using json_util::addOptional;
using json_util::finish;
using json_util::writeDeviceStatus;
using json_util::writeMeasurement;

}  // namespace

bool buildErrorPayload(const ApiError& error, const std::string& requestId, std::string& out) {
    JsonDocument doc;
    JsonObject   e = doc["error"].to<JsonObject>();
    e["code"]      = error.code;
    e["message"]   = error.message;
    if (!requestId.empty()) {
        e["request_id"] = requestId;
    }
    return finish(doc, out, 512);
}

bool buildProvisionPayload(const std::string& hostname, std::string& out, size_t maxBytes) {
    JsonDocument doc;
    doc["status"]    = "saved";
    doc["rebooting"] = true;
    doc["hostname"]  = hostname;
    return finish(doc, out, maxBytes);
}

const std::string& displayName(const DeviceSummary& device) {
    return device.label.empty() ? device.id : device.label;
}

FleetTotals totalsFor(const std::vector<DeviceSummary>& fleet) {
    FleetTotals t;
    t.polled = static_cast<unsigned>(fleet.size());
    for (const auto& f : fleet) {
        if (f.online && f.dataValid && !f.dataStale) {
            ++t.answering;
        }
        if (f.hasAcPower) {
            t.acPowerW += f.acPowerW;
            ++t.acCount;
        }
        if (f.hasEnergyToday) {
            t.energyTodayKwh += f.energyTodayKwh;
            ++t.todayCount;
        }
        if (f.hasEnergyTotal) {
            t.energyTotalKwh += f.energyTotalKwh;
            ++t.lifetimeCount;
        }
    }
    return t;
}

DeviceSummary summariseDevice(const DeviceState& state, const std::string& deviceId,
                              uint64_t nowMs) {
    DeviceSummary s;
    s.id        = deviceId;
    s.label     = state.label;
    s.online    = state.inverterOnline;
    s.dataValid = state.dataValid;
    s.dataStale = state.dataStale;
    if (state.lastSuccessfulPollMs != 0) {
        s.everPolled = true;
        // Guarded like MeasurementSet::updateStaleness does the same subtraction: unsigned, and
        // a caller passing a clock older than the snapshot would otherwise get ~584 million
        // years "ago". Not reachable from the one call site today; this is a public function.
        s.lastPollSecondsAgo = nowMs > state.lastSuccessfulPollMs
                                   ? static_cast<uint32_t>((nowMs - state.lastSuccessfulPollMs) / 1000)
                                   : 0;
    }
    const auto take = [&state](const char* id, bool& has, double& value) {
        const Measurement* m = state.measurements.find(id);
        // valid AND fresh, the same rule writeMeasurement, the MQTT payloads and the Modbus
        // register map all use. Two traps, both of which produce a number nobody questions:
        // an unread channel holds 0.0, and markAllStale() leaves `valid` true when a device
        // goes offline -- so without !stale a dead inverter's last daylight reading was summed
        // into the Dashboard total forever. At 03:00 the bridge reported watts (review).
        if (m != nullptr && m->supported && m->valid && !m->stale) {
            has   = true;
            value = m->value;
        }
    };
    take(measurement_id::kAcPowerTotal, s.hasAcPower, s.acPowerW);
    take(measurement_id::kEnergyToday, s.hasEnergyToday, s.energyTodayKwh);
    take(measurement_id::kEnergyTotal, s.hasEnergyTotal, s.energyTotalKwh);
    // L1 specifically, not "the AC voltage": a three-phase inverter has three, and picking one
    // to stand for all of them would be a quiet lie. L1 is the phase every supported driver
    // reports, and the Device tab remains the place to see all of them.
    take(measurement_id::kAcL1Voltage, s.hasAcVoltage, s.acVoltageV);
    take(measurement_id::kTemperature, s.hasTemperature, s.temperatureC);
    take(measurement_id::kBatterySoc, s.hasBatterySoc, s.batterySocPct);
    take(measurement_id::kBatteryPower, s.hasBatteryPower, s.batteryPowerW);
    return s;
}

bool buildStatusPayload(const DeviceState& state, const std::string& deviceId,
                        const BridgeInfo& bridge, const DiagnosticsSnapshot& diagnostics,
                        const DriverDescriptor* driver, uint64_t nowMs,
                        const std::vector<DeviceSummary>& fleet, std::string& out,
                        size_t maxBytes) {
    JsonDocument doc;

    JsonObject b            = doc["bridge"].to<JsonObject>();
    b["id"]                 = bridge.bridgeId;
    b["name"]               = bridge.name;
    b["firmware_version"]   = bridge.firmwareVersion;
    // The board as a slug, not only as a display name. The update check needs to ask for the
    // image built for THIS board: all three start with the same magic byte, so nothing
    // downstream could otherwise tell a Relay-6CH image from an RS485-CAN one, and the wrong
    // one runs happily on the wrong pins.
    b["board_id"]           = bridge.boardId;
    // What the interface HAS, and how it was asked for -- see BridgeInfo. Empty while the
    // portal is up and nothing has been assigned yet, so it is omitted rather than reported
    // as "" (the house rule: absent means "no answer", never a blank that reads like one).
    if (!bridge.ipAddress.empty()) {
        b["ip_address"] = bridge.ipAddress;
    }
    b["ip_mode"] = bridge.staticIp ? "static" : "dhcp";
    // A firmware constant, not configuration: the settings page needs it to stop offering an
    // "Add a device" button past the point the bridge would refuse the save.
    b["max_devices"]        = static_cast<unsigned>(kMaxDevices);
    b["devices_configured"] = static_cast<unsigned>(bridge.devicesConfigured);
    // How many actually got built at boot. The difference from devices_configured is the
    // whole point: it is what any tab can show without walking the device list.
    b["devices_started"]    = static_cast<unsigned>(bridge.devicesStarted);
    // Always emitted, empty or not: "no problems" and "this firmware does not report problems"
    // must not look the same to a client.
    JsonArray problems      = b["device_problems"].to<JsonArray>();
    for (const auto& p : bridge.deviceProblems) {
        problems.add(p);
    }
    b["uptime_seconds"]     = bridge.uptimeSeconds;
    b["wifi_connected"]     = bridge.wifiConnected;
    if (bridge.wifiConnected) {
        b["wifi_rssi_dbm"] = bridge.wifiRssiDbm;
    } else {
        b["wifi_rssi_dbm"] = nullptr;  // 0 dBm would read as a perfect signal
    }
    b["mqtt_connected"]    = bridge.mqttConnected;
    b["modbus_listening"]  = bridge.modbusListening;
    b["modbus_clients"]    = bridge.modbusClients;
    json_util::addClockFields(b, bridge);
    b["free_heap_bytes"]   = bridge.freeHeapBytes;

    // Only on boards that have relays: absent, not an empty list, per the house rule that
    // hardware which does not exist is never reported as a zero-ish value.
    if (bridge.relayCount > 0) {
        b["relays_enabled"] = bridge.relaysEnabled;
        JsonArray relays    = b["relays"].to<JsonArray>();
        for (uint8_t i = 0; i < bridge.relayCount; ++i) {
            relays.add(((bridge.relayMask >> i) & 1) != 0);
        }
        // Derived DRM mode, only when roles make one meaningful.
        std::vector<std::string> roles = bridge.relayRoles;
        roles.resize(bridge.relayCount, "none");
        if (!drm::optionsFor(roles).empty()) {
            b["drm_mode"] = drm::modeFrom(roles, bridge.relayMask);
        }
    }

    // Onboard indicators, only on boards that carry them (house rule: absent, not a
    // false-ish default, when the hardware is not there).
    if (bridge.hasBootButton) {
        b["boot_button_pressed"] = bridge.bootButtonPressed;
    }
    if (bridge.hasStatusLed) {
        b["status_led"] = bridge.statusLedColor;
    }

    JsonObject d = doc["device"].to<JsonObject>();
    d["id"]      = deviceId;  // the registered id, not identity.deviceId() -- see the header
    addOptional(d, "label", state.label);
    addOptional(d, "driver_id", state.identity.driverId);
    if (driver != nullptr) {
        d["support_level"] = supportLevelName(driver->supportLevel);
    }
    addOptional(d, "manufacturer", state.identity.manufacturer);
    addOptional(d, "model", state.identity.model);
    addOptional(d, "serial_number", state.identity.serialNumber);
    d["online"]     = state.inverterOnline;
    d["data_valid"] = state.dataValid;
    d["data_stale"] = state.dataStale;
    d["consecutive_poll_failures"] = state.consecutiveFailures;
    if (state.lastSuccessfulPollMs == 0) {
        // Never polled successfully. 0 would read as "just now".
        d["last_successful_poll_seconds_ago"] = nullptr;
    } else {
        d["last_successful_poll_seconds_ago"] =
            static_cast<uint32_t>((nowMs - state.lastSuccessfulPollMs) / 1000);
    }

    JsonObject measurements = doc["measurements"].to<JsonObject>();
    for (const auto& m : state.measurements.all()) {
        if (!m.supported) {
            continue;
        }
        writeMeasurement(measurements[m.id].to<JsonObject>(), m);
    }

    writeDeviceStatus(doc.as<JsonObject>(), state);

    // Every polled device, and the totals over them. The `device` block above is the first
    // device and stays that way for compatibility, but nothing that presents itself as the
    // bridge's health may be built from it: on a bus of three, one dead inverter left the
    // header indicator green and the Dashboard reporting a healthy day (#38).
    JsonArray devices = doc["devices"].to<JsonArray>();
    // Summed by the shared helper, not here: the log heartbeat reports the same fleet and must
    // not carry its own copy of what "answering" means (#74).
    const FleetTotals totals = totalsFor(fleet);
    for (const auto& f : fleet) {
        JsonObject o    = devices.add<JsonObject>();
        o["id"]         = f.id;
        addOptional(o, "label", f.label);
        o["online"]     = f.online;
        o["data_valid"] = f.dataValid;
        o["data_stale"] = f.dataStale;
        // 0 means "not served over Modbus" -- Modbus disabled, or past the end of the run.
        if (f.modbusUnitId != 0) {
            o["modbus_unit_id"] = f.modbusUnitId;
        } else {
            o["modbus_unit_id"] = nullptr;
        }
        if (f.everPolled) {
            o["last_successful_poll_seconds_ago"] = f.lastPollSecondsAgo;
        } else {
            o["last_successful_poll_seconds_ago"] = nullptr;  // 0 would read as "just now"
        }
        // Absent channels stay null rather than 0: "this inverter reports no power" and
        // "this inverter reports 0 W" are different answers and look identical as a number.
        // Same absent-is-null rule for every channel below: a driver that does not report
        // temperature and one reporting 0 degrees are different answers.
        if (f.hasEnergyToday) {
            o["energy_today_kwh"] = f.energyTodayKwh;
        } else {
            o["energy_today_kwh"] = nullptr;
        }
        if (f.hasAcVoltage) {
            o["ac_voltage_v"] = f.acVoltageV;
        } else {
            o["ac_voltage_v"] = nullptr;
        }
        if (f.hasTemperature) {
            o["temperature_c"] = f.temperatureC;
        } else {
            o["temperature_c"] = nullptr;
        }
        if (f.hasBatterySoc) {
            o["battery_soc_pct"] = f.batterySocPct;
        } else {
            o["battery_soc_pct"] = nullptr;
        }
        if (f.hasBatteryPower) {
            o["battery_power_w"] = f.batteryPowerW;
        } else {
            o["battery_power_w"] = nullptr;
        }
        if (f.hasAcPower) {
            o["ac_power_w"] = f.acPowerW;
        } else {
            o["ac_power_w"] = nullptr;
        }
    }
    JsonObject t = doc["totals"].to<JsonObject>();
    // "Answering" is the strict reading: started, online, holding data that is valid and not
    // stale. A device that started and never returned a byte is not answering, and neither is
    // one whose last reading has aged out.
    t["devices_answering"] = totals.answering;
    t["devices_polled"]    = totals.polled;
    // A total over none is unknown, not zero. The count travels with it so a client can say
    // "2 of 3 inverters" instead of implying the sum covers everything.
    const auto total = [&t](const char* key, unsigned count, double sum) {
        if (count != 0) {
            t[key] = sum;
        } else {
            t[key] = nullptr;
        }
    };
    total("ac_power_w", totals.acCount, totals.acPowerW);
    t["ac_power_devices"] = totals.acCount;
    total("energy_today_kwh", totals.todayCount, totals.energyTodayKwh);
    t["energy_today_devices"] = totals.todayCount;
    total("energy_total_kwh", totals.lifetimeCount, totals.energyTotalKwh);
    t["energy_total_devices"] = totals.lifetimeCount;

    doc["poll_success_total"] = diagnostics.pollSuccessTotal;
    doc["poll_failure_total"] = diagnostics.pollFailureTotal;
    return finish(doc, out, maxBytes);
}

bool buildDevicesPayload(const std::vector<std::string>& deviceIds, std::string& out,
                         size_t maxBytes) {
    JsonDocument doc;
    JsonArray    arr = doc["devices"].to<JsonArray>();
    for (const auto& id : deviceIds) {
        arr.add(id);
    }
    return finish(doc, out, maxBytes);
}

bool buildDevicePayload(const DeviceState& state, const std::string& deviceId,
                        const DriverDescriptor* driver, int configSlot, uint64_t nowMs,
                        std::string& out, size_t maxBytes) {
    JsonDocument doc;
    JsonObject   root = doc.to<JsonObject>();
    root["id"]        = deviceId;
    // Which configured row started this device: 0 is the primary driver, 1..N are
    // additional_devices[0..N-1]. Only setup() can answer it -- rows get refused and the
    // survivors are not the configuration minus a suffix -- and a client that has to guess gets
    // it wrong, which is how a Remove button ended up on a working inverter. Omitted rather than
    // sent as -1 when unknown: absent is not a slot.
    if (configSlot >= 0) {
        root["config_slot"] = configSlot;
    }
    // Outside `identity`, deliberately: identity is what the DEVICE reported, and a name the
    // operator typed does not belong in it.
    addOptional(root, "label", state.label);

    JsonObject identity = doc["identity"].to<JsonObject>();
    addOptional(identity, "manufacturer", state.identity.manufacturer);
    addOptional(identity, "model", state.identity.model);
    addOptional(identity, "serial_number", state.identity.serialNumber);
    addOptional(identity, "firmware_version", state.identity.firmwareVersion);
    addOptional(identity, "hardware_version", state.identity.hardwareVersion);
    addOptional(identity, "protocol_name", state.identity.protocolName);
    addOptional(identity, "driver_id", state.identity.driverId);

    if (driver != nullptr) {
        JsonObject drv     = doc["driver"].to<JsonObject>();
        drv["id"]          = driver->id;
        drv["display_name"] = driver->displayName;
        drv["protocol"]    = driver->protocol;
        drv["support_level"] = supportLevelName(driver->supportLevel);
        drv["supports_write"] = driver->supportsWrite;
    }

    doc["online"]     = state.inverterOnline;
    doc["data_valid"] = state.dataValid;
    doc["data_stale"] = state.dataStale;
    // The same two fields the status payload has always carried for the first device. Started
    // is not answering: a driver whose begin() succeeded is in the device list whether or not
    // the inverter has ever replied, and with A and B swapped it never will.
    doc["consecutive_poll_failures"] = state.consecutiveFailures;
    if (state.lastSuccessfulPollMs == 0) {
        doc["last_successful_poll_seconds_ago"] = nullptr;  // 0 would read as "just now"
    } else {
        doc["last_successful_poll_seconds_ago"] =
            static_cast<uint32_t>((nowMs - state.lastSuccessfulPollMs) / 1000);
    }
    return finish(doc, out, maxBytes);
}

bool buildMeasurementsPayload(const DeviceState& state, std::string& out, size_t maxBytes) {
    JsonDocument doc;
    JsonObject   measurements = doc["measurements"].to<JsonObject>();
    for (const auto& m : state.measurements.all()) {
        if (!m.supported) {
            continue;
        }
        JsonObject entry = measurements[m.id].to<JsonObject>();
        entry["display_name"] = m.displayName;
        writeMeasurement(entry, m);
        entry["timestamp_ms"] = m.timestampMs;
    }
    return finish(doc, out, maxBytes);
}

bool buildDiagnosticsPayload(const DiagnosticsSnapshot& d, const BridgeInfo& bridge,
                             std::string& out, size_t maxBytes) {
    JsonDocument doc;
    json_util::writeDiagnostics(doc.to<JsonObject>(), d, bridge, bridge.boardName,
                                bridge.mqttConnected);
    // REST only. The backtrace is the one part of a crash dump worth sixteen numbers, and it is
    // also the part nobody wants republished over MQTT every interval when it never changes.
    // Someone fetching /api/v1/diagnostics is asking on purpose.
    //
    // Absent, not an empty array, when there is nothing to report: [] would read as "the stack
    // walk found no frames", which is a different statement from "no dump is stored".
    // Reset breadcrumbs, REST-only like the backtrace below and for the same reason:
    // someone fetching /api/v1/diagnostics is asking on purpose, and a reset history never
    // changes between reboots -- republishing it over MQTT every interval says nothing new.
    // bootCount 0 = not wired (host default); absent rather than a fabricated "0 boots".
    if (bridge.bootCount > 0) {
        doc["boot_count"] = bridge.bootCount;
        if (bridge.breadcrumbsCold) {
            // Cold start: first boot ever, or power was lost. No previous life to report --
            // null, not 0, because "it had been up 0 ms" is a different (false) statement.
            doc["previous_uptime_ms"] = nullptr;
            doc["previous_firmware"]  = nullptr;
        } else {
            // How the previous life ENDED is this boot's reset_reason, already in this
            // payload -- these two fields say how long it ran and what it was running.
            doc["previous_uptime_ms"] = bridge.previousUptimeMs;
            char v[16];
            // Cast rather than switch to %lu: uint32_t is `unsigned long` on xtensa and
            // `unsigned int` on the host, so either bare format string is wrong on one of the
            // two builds. Each component is masked to a byte, so narrowing to unsigned is exact.
            snprintf(v, sizeof v, "%u.%u.%u",
                     static_cast<unsigned>((bridge.previousFirmware >> 16) & 0xFF),
                     static_cast<unsigned>((bridge.previousFirmware >> 8) & 0xFF),
                     static_cast<unsigned>(bridge.previousFirmware & 0xFF));
            doc["previous_firmware"] = v;
        }
    }
    if (bridge.coredumpPresent && !bridge.coredumpBacktrace.empty()) {
        JsonArray bt = doc["coredump_backtrace"].to<JsonArray>();
        for (const uint32_t pc : bridge.coredumpBacktrace) {
            bt.add(pc);
        }
        // The IDF's own verdict on the stack walk. A corrupted backtrace is still worth showing
        // -- the innermost frame or two are usually right -- but it must be shown as suspect.
        doc["coredump_backtrace_corrupted"] = bridge.coredumpBacktraceCorrupted;
    } else {
        doc["coredump_backtrace"]           = nullptr;
        doc["coredump_backtrace_corrupted"] = nullptr;
    }
    return finish(doc, out, maxBytes);
}

bool buildDiscoveryPayload(const DiscoveryReport& report, uint64_t nowMs, std::string& out,
                           size_t maxBytes) {
    JsonDocument doc;
    doc["status"] = discoveryStatusName(report.status);
    doc["mode"]   = report.mode == DiscoveryMode::Extended ? "extended" : "quick";
    doc["busy"]   = report.status == DiscoveryStatus::Requested ||
                  report.status == DiscoveryStatus::Running;

    if (report.startedMs > 0) {
        const uint64_t until =
            report.finishedMs > 0 ? report.finishedMs : (nowMs > report.startedMs ? nowMs : report.startedMs);
        doc["elapsed_ms"] = until - report.startedMs;
    }
    if (!report.error.empty()) {
        doc["error"] = report.error;
    }

    if (report.status == DiscoveryStatus::Done) {
        doc["auto_selected"] = report.outcome.autoSelected;
        // Always present, even when nothing was chosen: the wizard shows this verbatim so the
        // user can judge an ambiguous result themselves rather than be told "failed".
        doc["reason"] = report.outcome.reason;
        if (report.outcome.autoSelected) {
            doc["selected_driver_id"] = report.outcome.selectedDriverId;
        } else {
            doc["selected_driver_id"] = nullptr;
        }

        // Bounded. A sweep of eight addresses across two Modbus drivers can produce sixteen
        // candidates at roughly half a kilobyte each, and the response bound is 8 KB -- past
        // which finish() refuses the whole body and the wizard shows "nothing answered" after a
        // minute of probing. Ten is well inside the bound and more than a bridge can poll.
        constexpr size_t kMaxCandidates = 10;
        size_t           omitted        = 0;
        if (report.outcome.candidates.size() > kMaxCandidates) {
            omitted = report.outcome.candidates.size() - kMaxCandidates;
        }
        // Never silently: the lowest-scoring ones are dropped, and the client is told how many.
        doc["candidates_omitted"] = static_cast<unsigned>(omitted);

        JsonArray candidates = doc["candidates"].to<JsonArray>();
        size_t    written    = 0;
        for (const auto& c : report.outcome.candidates) {
            if (++written > kMaxCandidates) {
                break;
            }
            JsonObject e            = candidates.add<JsonObject>();
            e["driver_id"]          = c.descriptor.id;
            e["display_name"]       = c.descriptor.displayName;
            e["manufacturer"]       = c.descriptor.manufacturer;
            e["protocol"]           = c.descriptor.protocol;
            e["support_level"]      = supportLevelName(c.descriptor.supportLevel);
            e["confidence"]         = c.probe.confidenceScore;
            e["consistent"]         = c.consistent;
            e["responded"]          = c.probe.responded;
            e["checksum_valid"]     = c.probe.checksumValid;
            addOptional(e, "detected_manufacturer", c.probe.detectedManufacturer);
            addOptional(e, "detected_model", c.probe.detectedModel);
            addOptional(e, "serial_number", c.probe.serialNumber);
            addOptional(e, "firmware_version", c.probe.firmwareVersion);
            {
                // What it answered at, not what the driver recommends first -- with a working
                // profile sweep those are no longer the same thing.
                const auto& p        = c.matchedProfile;
                JsonObject  profile  = e["serial_profile"].to<JsonObject>();
                profile["baud_rate"] = p.baudRate;
                profile["parity"]    = parityName(p.parity);
                profile["data_bits"] = p.dataBits;
                profile["stop_bits"] = p.stopBits;
                profile["response_timeout_ms"] = p.responseTimeoutMs;
            }
            // The options it answered at -- the bus address, in practice. Handed over as a map
            // so the wizard can apply them verbatim rather than re-deriving an address from a
            // driver id, and `address` separately because that is the one the UI shows.
            JsonObject options = e["options"].to<JsonObject>();
            for (const auto& [key, value] : c.matchedOptions) {
                options[key] = value;
            }
            if (c.address().empty()) {
                e["address"] = nullptr;  // this protocol has no address the user picks
            } else {
                e["address"] = c.address();
            }
            JsonArray evidence = e["evidence"].to<JsonArray>();
            for (const auto& line : c.probe.evidence) {
                evidence.add(line);
            }
        }
        // Addresses that produced traffic without identifying anything -- two devices sharing
        // one unit id, in practice. Not candidates: they are not devices.
        JsonArray unidentified = doc["unidentified_addresses"].to<JsonArray>();
        for (const auto& u : report.outcome.unidentified) {
            JsonObject e  = unidentified.add<JsonObject>();
            e["driver_id"] = u.driverId;
            e["address"]   = u.address;
            e["note"]      = u.note;
        }
        JsonObject selectedOptions = doc["selected_options"].to<JsonObject>();
        for (const auto& [key, value] : report.outcome.selectedOptions) {
            selectedOptions[key] = value;
        }
        // Always present, empty in Quick mode: "nothing else answered" and "nothing else was
        // asked" are different results and a client must be able to tell them apart.
        JsonArray swept = doc["swept_addresses"].to<JsonArray>();
        for (const int address : report.outcome.sweptAddresses) {
            swept.add(address);
        }
    }
    return finish(doc, out, maxBytes);
}

bool buildDriversPayload(const std::vector<DriverDescriptor>& drivers, std::string& out,
                         size_t maxBytes) {
    JsonDocument doc;
    JsonArray    arr = doc["drivers"].to<JsonArray>();
    for (const auto& d : drivers) {
        JsonObject e         = arr.add<JsonObject>();
        e["id"]              = d.id;
        e["display_name"]    = d.displayName;
        e["manufacturer"]    = d.manufacturer;
        e["protocol"]        = d.protocol;
        e["support_level"]   = supportLevelName(d.supportLevel);
        e["supports_read"]   = d.supportsRead;
        e["supports_write"]  = d.supportsWrite;
        e["auto_detection"]  = d.supportsAutoDetection;
        // Whether a second row of this driver can run at all. planDevices() refuses one that
        // cannot, at boot, after the restart the owner performed to apply the change -- so
        // without this field the page could only find out by trying. It guessed instead.
        e["supports_multiple_devices"] = d.supportsMultipleDevices;
        JsonArray profiles   = e["serial_profiles"].to<JsonArray>();
        for (const auto& p : d.recommendedSerialProfiles) {
            JsonObject pr = profiles.add<JsonObject>();
            pr["baud_rate"] = p.baudRate;
            pr["parity"]    = parityName(p.parity);
            pr["data_bits"] = p.dataBits;
            pr["stop_bits"] = p.stopBits;
        }
        // Driver-declared settings. The web form renders these generically, so a new driver's
        // options show up in the UI without a line of frontend work.
        JsonArray options = e["options"].to<JsonArray>();
        for (const auto& o : d.options) {
            JsonObject oo        = options.add<JsonObject>();
            oo["key"]            = o.key;
            oo["display_name"]   = o.displayName;
            oo["description"]    = o.description;
            oo["default_value"]  = o.defaultValue;
            // Emitted only for a numeric option, so a client can tell "bounded number" from
            // "free text" without guessing from the key name.
            if (o.isNumeric()) {
                oo["min_value"] = o.minValue;
                oo["max_value"] = o.maxValue;
            }
            JsonArray allowed    = oo["allowed_values"].to<JsonArray>();
            for (const auto& v : o.allowedValues) {
                allowed.add(v);
            }
            // Only when every value has one. A short list would pair labels with the wrong
            // values from the first gap onward, and the failure looks like working software:
            // the page renders, the names read plausibly, and the map somebody picked is not
            // the map they were shown. Absent labels just fall back to the raw ids.
            if (o.allowedLabels.size() == o.allowedValues.size() && !o.allowedLabels.empty()) {
                JsonArray labels = oo["allowed_labels"].to<JsonArray>();
                for (const auto& l : o.allowedLabels) {
                    labels.add(l);
                }
            }
        }
    }
    return finish(doc, out, maxBytes);
}

bool buildLogsPayload(const std::vector<std::string>& lines, uint32_t totalLines,
                      const std::string& level, std::string& out, size_t maxBytes) {
    JsonDocument doc;
    // `total` versus the returned count is how a reader spots that the ring wrapped and lines
    // were lost, rather than assuming a quiet bus.
    doc["total"]    = totalLines;
    doc["returned"] = lines.size();
    doc["level"]    = level;
    JsonArray arr   = doc["lines"].to<JsonArray>();
    for (const auto& l : lines) {
        arr.add(l);
    }
    return finish(doc, out, maxBytes);
}

bool buildCapturePayload(const CaptureReport& report, uint64_t nowMs, std::string& out,
                         size_t maxBytes) {
    JsonDocument doc;
    doc["status"] = captureStatusName(report.status);
    if (!report.error.empty()) {
        doc["error"] = report.error;
    }
    // The line it actually listened at, echoed in full. On an unidentified device this is a
    // guess, and every number below is only meaningful against the guess that produced it --
    // a report that omitted it would be uninterpretable a day later.
    JsonObject line   = doc["line"].to<JsonObject>();
    line["baud_rate"] = report.profile.baudRate;
    line["parity"]    = parityName(report.profile.parity);
    line["data_bits"] = report.profile.dataBits;
    line["stop_bits"] = report.profile.stopBits;
    line["idle_gap_ms"] = report.idleGapMs;

    doc["requested_seconds"] = report.config.durationMs / 1000;
    doc["max_frames"]        = report.config.maxFrames;
    if (report.startedMs != 0) {
        const uint64_t end = report.finishedMs != 0 ? report.finishedMs : nowMs;
        doc["elapsed_ms"]  = end - report.startedMs;
    }

    JsonObject summary       = doc["summary"].to<JsonObject>();
    summary["frames"]        = report.frames.size();
    summary["bytes"]         = report.totalBytes;
    // THE number to read first. A capture at the wrong baud rate produces plenty of bytes and
    // zero valid checksums, and without this the operator concludes the device is mute.
    summary["modbus_crc_ok"] = report.modbusFrames;
    summary["aa55_frames_ok"] = report.pmuFrames;
    summary["truncated"]     = report.truncated;

    JsonArray frames = doc["frames"].to<JsonArray>();
    for (const auto& f : report.frames) {
        JsonObject e        = frames.add<JsonObject>();
        e["offset_ms"]      = f.offsetMs;
        e["gap_before_ms"]  = f.gapBeforeMs;
        e["length"]         = f.bytes.size();
        e["modbus_crc_ok"]  = f.modbusCrcValid;
        e["aa55_ok"]        = f.pmuFrameValid;
        // Uppercase, space-separated: the form every protocol document and every decoder
        // example in docs/ uses, so a captured frame can be compared against one by eye.
        std::string hex;
        hex.reserve(f.bytes.size() * 3);
        for (size_t i = 0; i < f.bytes.size(); ++i) {
            static const char kDigits[] = "0123456789ABCDEF";
            if (i != 0) hex.push_back(' ');
            hex.push_back(kDigits[f.bytes[i] >> 4]);
            hex.push_back(kDigits[f.bytes[i] & 0x0F]);
        }
        e["hex"] = hex;
    }
    return finish(doc, out, maxBytes);
}

bool buildRestorePreviewPayload(const BackupContents& backup,
                                const std::vector<ConfigDiffEntry>& diff, bool rebootRequired,
                                bool rollbackExists, std::string& out, size_t maxBytes) {
    JsonDocument doc;
    // Describe the FILE first: which bridge and which firmware wrote it is half of deciding
    // whether to apply it, and it is the half a diff of field values cannot show.
    JsonObject source          = doc["backup"].to<JsonObject>();
    source["format_version"]   = backup.formatVersion;
    source["includes_secrets"] = backup.includesSecrets;
    // Absent, not empty string: an unsynced bridge writes no timestamp, and "" rendered in a
    // date column reads as a bug rather than as "this file is undated".
    if (!backup.firmwareVersion.empty()) source["firmware_version"] = backup.firmwareVersion;
    if (!backup.exportedAt.empty()) source["exported_at"] = backup.exportedAt;

    doc["change_count"]       = diff.size();
    doc["reboot_required"]    = rebootRequired;
    doc["rollback_exists"] = rollbackExists;
    JsonArray changes         = doc["changes"].to<JsonArray>();
    for (const auto& entry : diff) {
        JsonObject e = changes.add<JsonObject>();
        e["field"]   = entry.field;
        e["before"]  = entry.before;
        e["after"]   = entry.after;
    }
    return finish(doc, out, maxBytes);
}

bool buildRestoreResultPayload(size_t changedFields, bool rebootRequired, bool rollbackStored,
                               std::string& out, size_t maxBytes) {
    JsonDocument doc;
    doc["status"]          = "restored";
    doc["changed_fields"]  = changedFields;
    doc["reboot_required"] = rebootRequired;
    doc["rollback_stored"] = rollbackStored;
    return finish(doc, out, maxBytes);
}

}  // namespace heliograph::rest
