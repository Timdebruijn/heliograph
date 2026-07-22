// SPDX-License-Identifier: MIT
//
// eModbus wiring. See modbus_tcp_server.h for the "not yet compiled" caveat.

#include "modbus_tcp_server.h"

#if defined(ESP32)

#include <ModbusServerTCPasync.h>

namespace heliograph::modbus {
namespace {

// One server instance for the process. eModbus keeps its own task and client list; there is
// no reason to have two, and the ESP32 has one TCP stack anyway.
ModbusServerTCPasync g_server;

}  // namespace

ModbusTcpServer::ModbusTcpServer(ModbusServerConfig config) : config_(config) {}

ModbusTcpServer::~ModbusTcpServer() { stop(); }

void ModbusTcpServer::refresh(const DeviceState& state, const BridgeInfo& bridge,
                              const DiagnosticsSnapshot& diagnostics, uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mapMutex_);
    map_.update(state, bridge, diagnostics, nowMs);
}

bool ModbusTcpServer::setConfig(const ModbusServerConfig& config) {
    if (started_) {
        return false;  // workers are already registered against the old unit ids
    }
    config_ = config;
    return true;
}

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

        uint16_t values[kMaxRegistersPerRead];
        bool     ok = false;
        {
            std::lock_guard<std::mutex> lock(mapMutex_);
            ok = map_.read(address, words, values);
        }
        if (!ok) {
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

    for (const uint8_t unitId : {config_.inverterUnitId, config_.diagnosticsUnitId}) {
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

ModbusTcpServer::ModbusTcpServer(ModbusServerConfig config) : config_(config) {}
ModbusTcpServer::~ModbusTcpServer() = default;

void ModbusTcpServer::refresh(const DeviceState& state, const BridgeInfo& bridge,
                              const DiagnosticsSnapshot& diagnostics, uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mapMutex_);
    map_.update(state, bridge, diagnostics, nowMs);
}

bool ModbusTcpServer::setConfig(const ModbusServerConfig& config) {
    if (started_) {
        return false;
    }
    config_ = config;
    return true;
}
uint16_t ModbusTcpServer::activeClients() const { return 0; }
bool     ModbusTcpServer::running() const { return false; }
bool     ModbusTcpServer::begin() { return false; }
void     ModbusTcpServer::stop() { started_ = false; }

}  // namespace heliograph::modbus

#endif
