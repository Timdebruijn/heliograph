// SPDX-License-Identifier: MIT

#include "rest_payloads.h"

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

DeviceSummary summariseDevice(const DeviceState& state, const std::string& deviceId,
                              uint64_t nowMs) {
    DeviceSummary s;
    s.id        = deviceId;
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
    double    acTotal = 0.0, todayTotal = 0.0, lifetimeTotal = 0.0;
    unsigned  acCount = 0, todayCount = 0, lifetimeCount = 0, answering = 0;
    for (const auto& f : fleet) {
        JsonObject o    = devices.add<JsonObject>();
        o["id"]         = f.id;
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
        if (f.hasAcPower) {
            o["ac_power_w"] = f.acPowerW;
        } else {
            o["ac_power_w"] = nullptr;
        }
        if (f.online && f.dataValid && !f.dataStale) {
            ++answering;
        }
        if (f.hasAcPower) {
            acTotal += f.acPowerW;
            ++acCount;
        }
        if (f.hasEnergyToday) {
            todayTotal += f.energyTodayKwh;
            ++todayCount;
        }
        if (f.hasEnergyTotal) {
            lifetimeTotal += f.energyTotalKwh;
            ++lifetimeCount;
        }
    }
    JsonObject t = doc["totals"].to<JsonObject>();
    // "Answering" is the strict reading: started, online, holding data that is valid and not
    // stale. A device that started and never returned a byte is not answering, and neither is
    // one whose last reading has aged out.
    t["devices_answering"] = answering;
    t["devices_polled"]    = static_cast<unsigned>(fleet.size());
    // A total over none is unknown, not zero. The count travels with it so a client can say
    // "2 of 3 inverters" instead of implying the sum covers everything.
    const auto total = [&t](const char* key, unsigned count, double sum) {
        if (count != 0) {
            t[key] = sum;
        } else {
            t[key] = nullptr;
        }
    };
    total("ac_power_w", acCount, acTotal);
    t["ac_power_devices"] = acCount;
    total("energy_today_kwh", todayCount, todayTotal);
    t["energy_today_devices"] = todayCount;
    total("energy_total_kwh", lifetimeCount, lifetimeTotal);
    t["energy_total_devices"] = lifetimeCount;

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
                        const DriverDescriptor* driver, uint64_t nowMs, std::string& out,
                        size_t maxBytes) {
    JsonDocument doc;
    doc["id"] = deviceId;

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
    doc["uptime_seconds"]          = bridge.uptimeSeconds;
    doc["firmware_version"]        = bridge.firmwareVersion;
    doc["board"]                   = bridge.boardName;
    doc["free_heap_bytes"]         = bridge.freeHeapBytes;
    doc["minimum_free_heap_bytes"] = bridge.minFreeHeapBytes;
    doc["max_alloc_heap_bytes"]    = bridge.maxAllocHeapBytes;

    // Absent, not zero, when the board has none -- and 0 is a legitimate reading for
    // psram_free_bytes on a board that HAS PSRAM and has exhausted it, so the two must not
    // collapse onto the same value. Reported at all because the three heap figures above are
    // MALLOC_CAP_INTERNAL and say nothing about it (audit, 2026-07-26).
    if (bridge.psramSizeBytes > 0) {
        doc["psram_size_bytes"] = bridge.psramSizeBytes;
        doc["psram_free_bytes"] = bridge.psramFreeBytes;
    } else {
        doc["psram_size_bytes"] = nullptr;
        doc["psram_free_bytes"] = nullptr;
    }
    doc["reset_reason"]            = bridge.resetReason;
    doc["ota_image_state"]         = bridge.otaImageState;

    // Absent, not zero, when no dump is stored: task "" at PC 0 is not a fact about anything,
    // and `coredump_present` false already carries the whole message. The partition and the
    // IDF support have existed since the OTA layout was designed; nothing read them until now.
    doc["coredump_present"] = bridge.coredumpPresent;
    if (bridge.coredumpPresent && !bridge.coredumpTask.empty()) {
        doc["coredump_task"] = bridge.coredumpTask;  // std::string: copied into the document
    } else {
        doc["coredump_task"] = nullptr;
    }
    if (bridge.coredumpPresent) {
        doc["coredump_pc"] = bridge.coredumpPc;
    } else {
        doc["coredump_pc"] = nullptr;
    }
    doc["wifi_connected"]          = bridge.wifiConnected;
    if (bridge.wifiConnected) {
        doc["wifi_rssi_dbm"] = bridge.wifiRssiDbm;
    } else {
        doc["wifi_rssi_dbm"] = nullptr;
    }
    doc["mqtt_connected"]                  = bridge.mqttConnected;
    json_util::addClockFields(doc.as<JsonObject>(), bridge);
    doc["poll_success_total"]              = d.pollSuccessTotal;
    doc["poll_failure_total"]              = d.pollFailureTotal;
    doc["consecutive_poll_failures"]       = d.consecutivePollFailures;
    doc["checksum_error_total"]            = d.checksumErrorTotal;
    doc["rs485_timeout_total"]             = d.rs485TimeoutTotal;
    doc["invalid_frame_total"]             = d.invalidFrameTotal;
    doc["wifi_reconnect_total"]            = d.wifiReconnectTotal;
    doc["mqtt_reconnect_total"]            = d.mqttReconnectTotal;
    doc["modbus_client_connections_total"] = d.modbusClientConnections;
    doc["rest_requests_total"]             = d.restRequestTotal;
    doc["mqtt_publish_failure_total"] = d.mqttPublishFailureTotal;
    doc["last_successful_poll_ms"]         = d.lastSuccessfulPollMs;
    // Null until the first sample, not 0: a monitoring rule on "stack headroom == 0" must
    // not fire during the first seconds after boot.
    if (d.rs485StackFreeBytes > 0) {
        doc["rs485_stack_free_bytes"] = d.rs485StackFreeBytes;
    } else {
        doc["rs485_stack_free_bytes"] = nullptr;
    }
    if (d.loopStackFreeBytes > 0) {
        doc["loop_stack_free_bytes"] = d.loopStackFreeBytes;
    } else {
        doc["loop_stack_free_bytes"] = nullptr;
    }
    doc["last_error"]                      = d.lastError;
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

}  // namespace heliograph::rest
