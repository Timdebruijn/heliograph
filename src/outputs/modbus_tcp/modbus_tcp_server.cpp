// SPDX-License-Identifier: MIT
//
// eModbus wiring. See modbus_tcp_server.h for the "not yet compiled" caveat.

#include "modbus_tcp_server.h"

#include <algorithm>

// Everything that is not eModbus lives here, once, for both builds: the unit-id mapping is the
// part worth testing and the host build is where it gets tested.
namespace heliograph::modbus {

ModbusTcpServer::ModbusTcpServer(ModbusServerConfig config) { setConfig(config); }

void ModbusTcpServer::refresh(const std::vector<const DeviceState*>& devices,
                              const BridgeInfo& bridge, const DiagnosticsSnapshot& diagnostics,
                              uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mapMutex_);
    for (size_t i = 0; i < maps_.size() && i < devices.size(); ++i) {
        if (devices[i] != nullptr) {
            maps_[i].update(*devices[i], bridge, diagnostics, nowMs);
        }
    }
}

bool ModbusTcpServer::setConfig(const ModbusServerConfig& config) {
    if (started_) {
        return false;  // workers are already registered against the old unit ids
    }
    // Under the map lock, because this REALLOCATES. rs485Task is created in setup() and starts
    // calling refresh() -- which writes maps_[i] -- before loop() ever reaches startOutputs()
    // and this call. It preempts loop() too, being the higher priority of the two, so an
    // unlocked assign() here frees a buffer another task may be writing into: a heap
    // write-after-free that shows up once, as an unreproducible panic. On main this was a plain
    // POD assignment and the hazard did not exist; the per-device maps created it (review).
    std::lock_guard<std::mutex> lock(mapMutex_);
    config_        = config;
    // Sized here rather than in begin() so the mapping is settled -- and host-testable --
    // before any TCP stack is involved.
    //
    // One map per CONFIGURED device, not per started one, and at least one always: the
    // diagnostics unit reads maps_[0], and a bridge with nothing polling is exactly when
    // someone scrapes it for uptime and heap.
    servedDevices_ = serveableDevices();
    maps_.assign(servedDevices_ > 0 ? servedDevices_ : 1, RegisterMap{});
    return true;
}

bool ModbusTcpServer::readUnit(uint8_t unitId, uint16_t address, uint16_t count,
                               uint16_t* out) const {
    if (count == 0 || count > kMaxRegistersPerRead) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mapMutex_);
    const size_t index = mapIndexFor(unitId);
    if (index >= maps_.size()) {
        return false;
    }
    return maps_[index].read(address, count, out);
}

size_t ModbusTcpServer::mapIndexFor(uint8_t unitId) const {
    // The diagnostics unit, and the two ids a client sends when it has no unit field to set,
    // all read device 1's map. 0 and 255 are what the MB/TCP spec calls "unused" for a server
    // that is not a gateway, and plenty of clients still send them -- Home Assistant's Modbus
    // integration defaults to 0. Answering them with device 1 costs nothing and is the
    // difference between a copied-in configuration working and returning an exception.
    if (unitId == config_.diagnosticsUnitId || unitId == 0 || unitId == 255) {
        return 0;
    }
    if (unitId < config_.inverterUnitId) {
        return kNoMap;
    }
    return static_cast<size_t>(unitId - config_.inverterUnitId);
}

uint8_t ModbusTcpServer::serveableDevices() const {
    uint8_t served = 0;
    for (uint8_t i = 0; i < config_.deviceCount; ++i) {
        const int unit = static_cast<int>(config_.inverterUnitId) + i;
        // Consecutive or not at all. Skipping a taken id and carrying on would leave a hole a
        // client cannot derive: the whole point of unit-id-per-device is that a client works
        // out device N's address by adding, so the run has to be unbroken.
        if (unit > 247 || unit == static_cast<int>(config_.diagnosticsUnitId)) {
            break;  // 247 is the last valid Modbus slave address
        }
        served = static_cast<uint8_t>(i + 1);
    }
    return served;
}

uint8_t ModbusTcpServer::unitIdFor(size_t index) const {
    if (index >= servedDevices_) {
        return 0;
    }
    return static_cast<uint8_t>(config_.inverterUnitId + index);
}

}  // namespace heliograph::modbus

#if defined(ESP32)

#include <ModbusServerTCPasync.h>

namespace heliograph::modbus {
namespace {

// One server instance for the process. eModbus keeps its own task and client list; there is
// no reason to have two, and the ESP32 has one TCP stack anyway.
ModbusServerTCPasync g_server;

}  // namespace

ModbusTcpServer::~ModbusTcpServer() { stop(); }

uint16_t ModbusTcpServer::activeClients() const {
    return started_ ? g_server.activeClients() : 0;
}

bool ModbusTcpServer::running() const { return started_ && g_server.isRunning(); }

bool ModbusTcpServer::begin() {
    if (!config_.enabled || started_) {
        return started_;
    }

    // FC3 and FC4 serve identical content. Measurements belong in input registers, but many
    // clients (PLCs, some EVCC setups) only speak FC3, and refusing them buys nothing.
    auto readWorker = [this](ModbusMessage request) -> ModbusMessage {
        ModbusMessage response;
        uint16_t      address = 0;
        uint16_t      words   = 0;
        request.get(2, address);
        request.get(4, words);

        if (words == 0 || words > kMaxRegistersPerRead) {
            response.setError(request.getServerID(), request.getFunctionCode(),
                              ILLEGAL_DATA_VALUE);
            return response;
        }

        // Which inverter, and the read itself, both in readUnit() -- the same code path a host
        // test exercises, rather than arithmetic that only runs on hardware.
        uint16_t values[kMaxRegistersPerRead];
        if (!readUnit(request.getServerID(), address, words, values)) {
            // Out of range. Note the official eModbus example sets this error and then falls
            // through into building a normal response anyway; returning here is deliberate.
            response.setError(request.getServerID(), request.getFunctionCode(),
                              ILLEGAL_DATA_ADDRESS);
            return response;
        }

        response.add(request.getServerID(), request.getFunctionCode(),
                     static_cast<uint8_t>(words * 2));
        for (uint16_t i = 0; i < words; ++i) {
            response.add(values[i]);
        }
        return response;
    };

    // Writing is refused at the protocol level, independent of any driver. The MVP has no
    // writable driver at all, so this is belt and braces -- but a client must get a correct
    // exception rather than silence or, worse, a success it did not earn.
    auto writeRejectWorker = [](ModbusMessage request) -> ModbusMessage {
        ModbusMessage response;
        response.setError(request.getServerID(), request.getFunctionCode(), ILLEGAL_FUNCTION);
        return response;
    };

    std::vector<uint8_t> units;
    units.reserve(static_cast<size_t>(servedDevices_) + 3);
    for (uint8_t i = 0; i < servedDevices_; ++i) {
        units.push_back(static_cast<uint8_t>(config_.inverterUnitId + i));
    }
    units.push_back(config_.diagnosticsUnitId);
    // A client with no unit-id field of its own sends 0 or 255 -- the MB/TCP spec calls the
    // byte unused for a server that is not a gateway, and Home Assistant's Modbus integration
    // defaults to 0. Both read device 1, so a configuration copied from a single-inverter
    // example works instead of returning an exception. Skipped when they collide with a real
    // unit, which cannot happen for 0 (validate() forbids unit_id 0) but can for 255.
    for (const uint8_t alias : {static_cast<uint8_t>(0), static_cast<uint8_t>(255)}) {
        if (std::find(units.begin(), units.end(), alias) == units.end()) {
            units.push_back(alias);
        }
    }

    for (const uint8_t unitId : units) {
        g_server.registerWorker(unitId, READ_HOLD_REGISTER, readWorker);
        g_server.registerWorker(unitId, READ_INPUT_REGISTER, readWorker);
        if (!config_.writeEnabled) {
            g_server.registerWorker(unitId, WRITE_HOLD_REGISTER, writeRejectWorker);
            g_server.registerWorker(unitId, WRITE_MULT_REGISTERS, writeRejectWorker);
        }
    }

    // coreID 0: keep Modbus with the network stack so that a burst of clients cannot disturb
    // RS485 timing on core 1.
    started_ = g_server.start(config_.port, config_.maxClients, config_.idleTimeoutMs, 0);
    return started_;
}

void ModbusTcpServer::stop() {
    if (started_) {
        g_server.stop();
        started_ = false;
    }
}

}  // namespace heliograph::modbus

#else  // !ESP32

// Host builds compile the register map only; there is no TCP stack to bind to. The map is
// where the logic worth testing lives, and it is tested in test_register_map.

namespace heliograph::modbus {

ModbusTcpServer::~ModbusTcpServer() = default;

uint16_t ModbusTcpServer::activeClients() const { return 0; }
bool     ModbusTcpServer::running() const { return false; }
bool     ModbusTcpServer::begin() { return false; }
void     ModbusTcpServer::stop() { started_ = false; }

}  // namespace heliograph::modbus

#endif
