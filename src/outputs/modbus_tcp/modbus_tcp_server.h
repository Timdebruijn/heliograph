// SPDX-License-Identifier: MIT
//
// Virtual Modbus TCP server, backed by eModbus (MIT).
//
// Owns the RegisterMap and the lock around it: the poll task re-renders it while eModbus
// workers read it from the AsyncTCP task. RegisterMap itself stays lock-free and pure so it
// remains host-testable; all concurrency lives here.
//
// VERIFIED ON HARDWARE 2026-07-17: serves FC3/FC4 on port 502 to pymodbus 3.14 from a
// Waveshare ESP32-S3-RS485-CAN. The API used here was read from the eModbus sources
// (src/ModbusServerTCPasync.h, src/ModbusServer.h, src/ModbusTypeDefs.h,
// examples/TCPServerAsync), not from memory.
//
// Still untested: FC6/FC16 rejection against a real client, and behaviour under several
// concurrent clients (Phase 9).

#pragma once

#include <cstdint>
#include <mutex>

#include "device/device_state.h"
#include "diagnostics/diagnostics.h"
#include "register_map.h"

namespace heliograph::modbus {

struct ModbusServerConfig {
    bool     enabled           = true;
    uint16_t port              = 502;
    uint8_t  inverterUnitId    = 1;
    uint8_t  diagnosticsUnitId = 250;
    uint8_t  maxClients        = 4;
    uint32_t idleTimeoutMs     = 20000;
    /// Never true in the MVP. Kept as config rather than as an absence of code so that the
    /// day a writable driver exists, the switch already has a documented default of off.
    bool writeEnabled = false;
};

/// Modbus caps a single read at 125 registers.
inline constexpr uint16_t kMaxRegistersPerRead = 125;

class ModbusTcpServer {
public:
    explicit ModbusTcpServer(ModbusServerConfig config = {});
    ~ModbusTcpServer();

    ModbusTcpServer(const ModbusTcpServer&)            = delete;
    ModbusTcpServer& operator=(const ModbusTcpServer&) = delete;

    /// Applies configuration. Must be called before begin(); ignored afterwards.
    ///
    /// The class is non-copyable (it owns a mutex), so the port and unit ids cannot be
    /// supplied by assigning a fresh instance -- which is how they silently stopped being
    /// applied at all: main.cpp constructed a default server and called begin() on it, so
    /// modbus.port and modbus.unit_id in the configuration did nothing, ever.
    bool setConfig(const ModbusServerConfig& config);

    bool begin();
    void stop();
    bool running() const;

    /// Re-renders the register map. Called from the poll task; takes the map lock briefly.
    void refresh(const DeviceState& state, const BridgeInfo& bridge,
                 const DiagnosticsSnapshot& diagnostics, uint64_t nowMs);

    uint16_t activeClients() const;

    /// Optional: lets the server bump modbus client counters.
    void setDiagnostics(Diagnostics* diagnostics) { diagnostics_ = diagnostics; }

    const ModbusServerConfig& config() const { return config_; }

private:
    ModbusServerConfig config_;
    RegisterMap        map_;
    mutable std::mutex mapMutex_;
    Diagnostics*       diagnostics_    = nullptr;
    uint16_t           lastClientCount_ = 0;
    bool               started_        = false;
};

}  // namespace heliograph::modbus
