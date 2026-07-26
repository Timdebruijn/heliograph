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
#include <vector>

#include "device/device_state.h"
#include "diagnostics/diagnostics.h"
#include "register_map.h"

namespace heliograph::modbus {

struct ModbusServerConfig {
    bool     enabled           = true;
    uint16_t port              = 502;
    /// Device 1. Devices 2..N follow at inverterUnitId + 1, + 2, and so on.
    ///
    /// One unit id per inverter is what the Modbus specification's Unit Identifier is for --
    /// addressing a device on a serial sub-network behind a gateway -- so a client needs no
    /// vendor-specific address arithmetic, only the unit id field it already has. The register
    /// map is identical at every unit; it simply repeats. Device 1 stays exactly where it has
    /// always been, so an existing client keeps working untouched (#36).
    uint8_t  inverterUnitId    = 1;
    uint8_t  diagnosticsUnitId = 250;
    /// How many inverters to serve, i.e. how many consecutive unit ids to claim. Set from the
    /// number of devices that started, before begin().
    uint8_t  deviceCount       = 1;
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

    /// Re-renders the register maps, one per device, in configuration order: `devices[i]` is
    /// served at `inverterUnitId + i`. Called from the poll task; takes the map lock briefly.
    ///
    /// Extra entries beyond servedDevices() are ignored rather than served at a unit id no
    /// worker is registered for -- the workers are bound at begin() and cannot grow after.
    void refresh(const std::vector<const DeviceState*>& devices, const BridgeInfo& bridge,
                 const DiagnosticsSnapshot& diagnostics, uint64_t nowMs);

    /// How many inverters this server actually serves. Lower than the configured count when
    /// the unit ids would run past 247 or collide with the diagnostics unit; begin() logs it.
    uint8_t servedDevices() const { return servedDevices_; }

    /// The unit id device `index` is served at, or 0 when it is not served.
    uint8_t unitIdFor(size_t index) const;

    uint16_t activeClients() const;

    /// Optional: lets the server bump modbus client counters.
    void setDiagnostics(Diagnostics* diagnostics) { diagnostics_ = diagnostics; }

    const ModbusServerConfig& config() const { return config_; }

private:
    /// How many devices can be served without running past unit id 247 or over the diagnostics
    /// unit. Pure, so the rule is host-testable; begin() applies it.
    uint8_t serveableDevices() const;

    ModbusServerConfig config_;
    /// One per served device, index i at unit id inverterUnitId + i. Allocated at begin(), when
    /// the count is known: 900 registers is 1.8 KB apiece, and eight of those reserved up front
    /// on a board with one inverter is 14 KB of heap for nothing.
    std::vector<RegisterMap> maps_;
    mutable std::mutex       mapMutex_;
    Diagnostics*             diagnostics_     = nullptr;
    uint16_t                 lastClientCount_ = 0;
    uint8_t                  servedDevices_   = 0;
    bool                     started_         = false;
};

}  // namespace heliograph::modbus
