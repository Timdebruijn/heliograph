# Heliograph — Agent Instructions

Heliograph is ESP32-S3 firmware that reads solar inverters over RS485 and republishes
their data as MQTT + Home Assistant discovery, Modbus TCP, REST/JSON, and Prometheus.
It targets the **Waveshare ESP32-S3-RS485-CAN** board, plus the **Relay-1CH** and **Relay-6CH**
boards for DRM relay curtailment, and is built with **PlatformIO** using the Arduino framework
(pioarduino, Arduino core 3.x / ESP-IDF 5.5.x).

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
| Outputs | `src/outputs/` | Read state snapshots; commands go to the dispatcher, never to a driver |
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
| `src/protocols/` | Protocol framing (PMU AA55, Modbus RTU, MaxTalk) |
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
pio test -e native                      # host tests, no hardware needed
bash tools/check_layering.sh            # layering invariants (and the read-back rule) — read RESULT: at the end
python3 tools/gen_profiles.py --check   # profile schema (when touching profiles/)
python3 tools/check_measurement_ids.py  # measurement ids vs the Prometheus gauges
bash tools/check_version.sh --self-test # version-tag check (when touching src/main.cpp versions)
python3 tools/check_web_js.py           # embedded JS (when touching src/web/)
python3 tools/build_web.py              # strip + gzip web pages (when touching src/web/)
python3 tools/check_dashboard_layout.py # the page in headless Chrome against tools/demo_fleet.js (src/web/)
ruff check tools/ && ruff format --check tools/   # Python tooling lint + format
```

`check_layering.sh` prints every rule it checks. **Always read the final `RESULT:` line**;
an earlier check may have failed even if the last sub-check printed "OK".

CI runs the same checks, cppcheck (`pio check -e native`), and a firmware build for each of the
four firmware environments (`waveshare-rs485-can`, `waveshare-relay-1ch`, `waveshare-relay-6ch`,
`mock`).

---

## Build environments

| Environment | Command | Purpose |
|---|---|---|
| `native` | `pio test -e native` | Host tests, no hardware needed |
| `waveshare-rs485-can` | `pio run -e waveshare-rs485-can` | Production firmware |
| `waveshare-relay-1ch` | `pio run -e waveshare-relay-1ch` | Relay-1CH board (DRM) |
| `waveshare-relay-6ch` | `pio run -e waveshare-relay-6ch` | Relay-6CH board (DRM) |
| `debug-rs485-can` | `pio run -e debug-rs485-can` | RS485-CAN with a debug build, for the built-in JTAG |
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
- Generated files are never edited by hand: `test/fixtures/eversolar_frames.h` comes from
  `tools/gen_fixtures.py`; `profiles_generated.cpp` and `src/web/assets/generated/` are built and
  gitignored. Change the generator or its source instead.
- All code, commit messages and docs (`docs/`, README) are in **English**.

---

## Supported inverter families

| Family | Protocol | Driver |
|---|---|---|
| EverSolar / Zeversolar legacy TL | PMU (AA55) over RS485 | `src/drivers/eversolar_legacy/` |
| Modbus devices described by a TOML profile in `profiles/`: Growatt, Deye, GoodWe, Huawei, Sofar, Solis, Sungrow | Modbus RTU | `src/drivers/modbus_profile/` |
| SolaX X1 series | PMU (AA55) over RS485 | `src/drivers/solax_x1/` |
| SolarMax | MaxTalk over RS485 | `src/drivers/solarmax/` |
| SunSpec-capable devices | Modbus RTU | `src/drivers/sunspec/` |

The profile list grows by adding TOML, so read `profiles/` rather than this table for the current
set. Each profile and driver carries its own support level; most are `experimental` until a real
device has confirmed them.

Do not describe this firmware as read-only. It monitors by default and **controls on opt-in**.
Two drivers (`modbus_profile`, `sunspec`) declare `supportsWrite` and implement `execute()`, and
the DRM relays can move. What holds is a chain of gates, all closed by default:
`security.read_only_mode` (the UI's *Allow control*, inverted) refuses everything; under it each
path has its own switch, `relays.enabled` for DRM and `control.inverter_writes_enabled` for
setpoints; and a setpoint is served only when its profile reads it back and the row is
`verified = true` or the device's `allow_unverified_writes` option is on (SunSpec: always the
opt-in, plus model 123). Describe the gates, not a blanket limitation.

---

## Web UI

`src/web/assets/*.h` is what you edit; the device does not serve it directly. The build
strips comments and gzips the result into `src/web/assets/generated/` (not committed).

To preview locally without hardware:

```bash
python3 tools/preview_web.py   # serves on http://127.0.0.1:8000 with tools/demo_fleet.js
```

The page is one document; tabs are buttons that call `goTab()`, with no URL per tab. User feedback
goes through `say()`, which also announces result messages to screen readers via the two live
regions at the top of `<body>`. Use it rather than writing into a message element directly.

---

## Licensing

MIT. Protocol knowledge must be reimplemented from community references — do not
copy-paste. Credit all sources in `LICENSE-THIRD-PARTY.md`.
