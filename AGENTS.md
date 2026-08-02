# Heliograph — Agent Instructions

Heliograph is ESP32-S3 firmware that reads solar inverters over RS485 and republishes
their data as MQTT + Home Assistant discovery, Modbus TCP, REST/JSON, and Prometheus.
It targets the **Waveshare ESP32-S3-RS485-CAN** board and is built with **PlatformIO**
using the Arduino framework (pioarduino, Arduino core 3.x / ESP-IDF 5.5.x).

---

## The two rules everything else depends on

1. **Brand / device knowledge lives only in `src/drivers/<driver>/` and in `profiles/`.**
   No output module, transport, state store, or generic header may name a manufacturer,
   device family, or model — not even in a comment. `tools/check_layering.sh` enforces
   this mechanically. Violating it breaks the whole abstraction.

2. **Unknown is never zero.** A value the inverter did not report is absent / `null`
   everywhere (REST, MQTT, Modbus, UI). Never substitute 0.

---

## Architecture

```
Physical  →  Transport  →  Driver  →  State (DeviceContext/DeviceState)  →  Outputs
```

| Layer | Location | May depend on |
|---|---|---|
| Transport | `src/transport/` | Physical only |
| Driver | `src/drivers/<driver>/` | Transport, protocols, device model |
| State | `src/device/`, `src/state/` | Device model only |
| Outputs | `src/outputs/` | State snapshots only (read-only) |
| Protocols | `src/protocols/` | Nothing above Transport |

Outputs read immutable `DeviceState` snapshots via `StateStore::snapshot()`. They never
talk to a driver directly.

---

## Key files and directories

| Path | Purpose |
|---|---|
| `src/drivers/` | All brand-specific code |
| `src/drivers/driver_registry.cpp` | Registers every driver |
| `src/drivers/discovery_engine.cpp` | Auto-detects the connected inverter |
| `src/protocols/` | Protocol parsers (PMU AA55, Modbus RTU) |
| `src/device/` | `DeviceContext`, `DeviceState`, and measurement models |
| `src/state/` | Thread-safe state store; immutable snapshots |
| `src/outputs/` | MQTT, Modbus TCP, REST, Prometheus adapters |
| `src/transport/` | RS485 / UART abstraction |
| `profiles/` | TOML register maps for Modbus devices |
| `tools/gen_profiles.py` | Generates `profiles_generated.cpp` from TOML |
| `tools/check_layering.sh` | Enforces the layering rules |
| `docs/architecture.md` | Detailed architecture and task model |
| `docs/adding-a-device.md` | Step-by-step guide for new device support |
| `platformio.ini` | All build environments and flags |

---

## Checks to run before every PR

Run these and confirm they pass before committing:

```bash
pio test -e native                    # ~930 host tests, no hardware needed
bash tools/check_layering.sh          # layering invariants — read RESULT: PASS/FAIL at the end
python3 tools/gen_profiles.py --check # profile schema (when touching profiles/)
python3 tools/check_web_js.py         # embedded JS (when touching src/web/)
python3 tools/build_web.py            # strip + gzip web pages (when touching src/web/)
ruff check tools/                     # Python tooling lint
```

`check_layering.sh` prints every rule it checks. **Always read the final `RESULT:` line**;
an earlier check may have failed even if the last sub-check printed "OK".

CI runs the same checks plus a firmware build for each of the four firmware environments
(`waveshare-rs485-can`, `waveshare-relay-1ch`, `waveshare-relay-6ch`, `mock`).
---

## Build environments

| Environment | Command | Purpose |
|---|---|---|
| `native` | `pio test -e native` | Host tests, no hardware needed |
| `waveshare-rs485-can` | `pio run -e waveshare-rs485-can` | Production firmware |
| `mock` | `pio run -e mock` | Full output stack with a simulated inverter |

The `native` environment only compiles platform-independent sources. A source file that
needs an Arduino header must not be in the `native` filter — that is a design error.

---

## Adding a Modbus device — no C++ required

1. Write a TOML file in `profiles/<family>/`. Schema: `docs/device-profiles/schema.md`.
2. Validate with `python3 tools/gen_profiles.py --check`.
3. The build pre-script regenerates `src/drivers/modbus_profile/profiles_generated.cpp`
   automatically on next build.

See `docs/adding-a-device.md` for the full workflow, including how to research a register
map and what the support-level progression means.

---

## Code style

- **C++17**. Match the style of the file being edited.
- No heap allocations in hot paths (the poll loop, packet parsers).
- No new dependencies without a concrete reason; pin library versions explicitly.
- Comments explain *why* (protocol constraints, hardware quirks), not *what*.
- All code and commit messages are in **English**. Architecture docs in `docs/` are in
  Dutch — leave them as-is.

---

## Supported inverter families

| Family | Protocol | Driver |
|---|---|---|
| EverSolar / Zeversolar legacy TL | PMU (AA55) over RS485 | `src/drivers/eversolar_legacy/` |
| Growatt SPH hybrid, MIC TL-X | Modbus RTU | `src/drivers/modbus_profile/` |
| SolaX X1 series | PMU (AA55) over RS485 | `src/drivers/solax_x1/` |
| SunSpec-capable devices | Modbus RTU | `src/drivers/sunspec/` |

Do not describe this firmware as read-only. Two drivers (`modbus_profile`, `sunspec`)
declare `supportsWrite` and implement `execute()`. What prevents writes today is a chain
of gates: no profile ships a `verified` write row; SunSpec additionally requires the
device to publish model 123; and `security.read_only_mode` is **on by default** and
blocks the whole path regardless. Describe the gates, not a blanket limitation.

---

## Web UI

`src/web/assets/*.h` is what you edit; the device does not serve it directly. The build
strips comments and gzips the result into `src/web/assets/generated/` (not committed).

To preview locally without hardware:

```bash
python3 tools/preview_web.py   # serves on http://127.0.0.1:8000
```

---

## Licensing

MIT. Protocol knowledge must be reimplemented from community references — do not
copy-paste. Credit all sources in `LICENSE-THIRD-PARTY.md`.
