# Copilot instructions for Heliograph

## What this project is

Heliograph is ESP32-S3 firmware that reads solar inverters over RS485 and republishes
their data as MQTT + Home Assistant discovery, Modbus TCP, REST/JSON, and Prometheus.
It targets the **Waveshare ESP32-S3-Relay-1CH** board and is built with **PlatformIO**
using the Arduino framework (pioarduino, Arduino core 3.x / ESP-IDF 5.5.x).

## The single most important rule

**Brand / device knowledge lives only in `src/drivers/<driver>/` and in `profiles/`.**
No output module, transport, state store, or generic header may name a manufacturer,
device family, or model — not even in a comment. `tools/check_layering.sh` enforces
this mechanically. Violating the rule breaks the whole abstraction.

The companion rule: **unknown is never zero.** A value the inverter did not report is
absent / `null` everywhere (REST, MQTT, Modbus, UI). Never substitute 0.

## Architecture layers (strict, top-to-bottom)

```
Physical  → Transport → Driver → State (DeviceContext/DeviceState) → Outputs
```

| Layer | Location | May depend on |
|---|---|---|
| Transport | `src/transport/` | Physical only |
| Driver | `src/drivers/<driver>/` | Transport, protocols, device model |
| State | `src/state/` | Device model only |
| Outputs | `src/outputs/` | State snapshots only (read-only) |
| Protocols | `src/protocols/` | Nothing above Transport |

Outputs read immutable `DeviceState` snapshots. They never talk to a driver directly.

## Adding a Modbus device — no C++ required

1. Write a TOML file in `profiles/<family>/`. Schema: `docs/device-profiles/schema.md`.
2. Run `python3 tools/gen_profiles.py --check` to validate.
3. The build pre-script (`tools/gen_profiles.py`) regenerates
   `src/drivers/growatt_modbus/profiles_generated.cpp` automatically.

See `docs/adding-a-device.md` for the full workflow.

## Build environments

| Environment | Command | Purpose |
|---|---|---|
| `native` | `pio test -e native` | 390+ host tests, no hardware needed |
| `waveshare-eversolar` | `pio run -e waveshare-eversolar` | Production firmware (all drivers) |
| `mock` | `pio run -e mock` | Full output stack with a simulated inverter |

The `native` environment only compiles platform-independent sources. If a source file
needs an Arduino header it must not be in the `native` filter — that is a design error.

## Checks to run before every PR

```bash
pio test -e native                    # host test suite
bash tools/check_layering.sh          # layering invariants — read RESULT: PASS/FAIL at the end
python3 tools/gen_profiles.py --check # profile schema (when touching profiles/)
python3 tools/check_web_js.py         # embedded JS (when touching src/web/)
ruff check tools/                     # Python tooling lint
```

CI runs the same checks plus both firmware builds.

## Code style

- **C++17**. Match the style of the file being edited.
- No heap allocations in hot paths (the poll loop, packet parsers).
- No new dependencies without a concrete reason; pin library versions explicitly.
- Comments explain *why* (protocol constraints, hardware quirks), not *what*.
- All code and commit messages are in **English** (architecture docs are Dutch — leave
  them as-is).

## Key files and directories

| Path | Purpose |
|---|---|
| `src/drivers/` | All brand-specific code |
| `src/drivers/driver_registry.cpp` | Registers every driver |
| `src/drivers/discovery_engine.cpp` | Auto-detects the connected inverter |
| `src/protocols/` | Protocol parsers (PMU AA55, Modbus RTU) |
| `src/state/` | Thread-safe state store; immutable snapshots |
| `src/outputs/` | MQTT, Modbus TCP, REST, Prometheus adapters |
| `src/transport/` | RS485 / UART abstraction |
| `profiles/` | TOML register maps for Modbus devices |
| `tools/gen_profiles.py` | Generates `profiles_generated.cpp` from TOML |
| `tools/check_layering.sh` | Enforces the layering rules |
| `docs/architecture.md` | Detailed architecture and task model |
| `docs/adding-a-device.md` | Step-by-step guide for new device support |
| `partitions_16mb_ota.csv` | Custom partition table (dual-app OTA rollback) |
| `platformio.ini` | All build environments and flags |

## Hardware notes

- **Board**: Waveshare ESP32-S3-Relay-1CH (ESP32-S3-WROOM-1U, 16 MB flash, 8 MB octal PSRAM).
- Use `pioarduino` platform URL, not `platform = espressif32` — the official registry
  still ships Arduino core 2.x. See `docs/decisions.md`.
- OTA uses a dual-app partition scheme (`partitions_16mb_ota.csv`). A watchdog-backed
  bootloader rollback reverts a bad image automatically.
- RS485 A/B connects to the inverter's COM port; the relay output is separate.

## Supported inverter families

| Family | Protocol | Driver directory |
|---|---|---|
| EverSolar / Zeversolar legacy TL | PMU (AA55) over RS485 | `src/drivers/eversolar_legacy/` |
| Growatt SPH hybrid | Modbus RTU | `src/drivers/growatt_modbus/` |
| SolaX X1 series | PMU (AA55) over RS485 | `src/drivers/solax_x1/` |

All drivers are **read-only**. Write support requires hardware-verified register maps
and an explicit write path — neither is enabled today.

## Licensing

MIT. Protocol knowledge must be reimplemented (not copy-pasted) from community
references; credit sources in `LICENSE-THIRD-PARTY.md`.
