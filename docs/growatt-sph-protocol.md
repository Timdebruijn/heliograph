# Growatt SPH (hybrid) — Modbus RTU register map

Status: **documented from community sources, NOT yet validated on hardware.** Every register
below is copied from published maps and open-source integrations, cross-checked where possible,
and marked with its confidence. Nothing here has been confirmed against a real SPH inverter by
this project. Treat it the way `docs/eversolar-protocol.md` treated its early constructed
frames: a starting hypothesis, to be pinned down the moment the hardware is on the bus.

The lesson from Phase 3 applies in full: a register map that reads plausibly can still be wrong
in the field (Eversolar's source address and payload length both were). No SPH driver ships
above **Alpha** until the read values match the inverter's own display and a write has been
seen to take effect and then reverted.

## Transport

- **Physical:** RS485 on the inverter's SYS/COM port (chosen 2026-07-20).
- **Line settings:** 9600 8N1 is the SPH default; some units run 115200. To confirm on site.
- **Protocol:** standard Modbus RTU — this project's `src/protocols/modbus/` codec, no
  brand-specific framing. Unit id is configurable on the inverter (default 1).
- **Two register spaces:** input registers (function 0x04, read-only measurements) and holding
  registers (function 0x03 read / 0x06 / 0x10 write, configuration and control).

## Reading — input registers (0x04)

Multi-register values are 32-bit, high word first, and (per the Growatt Protocol II convention)
scaled ×0.1 unless noted. **Scaling is NOT confirmed from the community map** and must be
checked against the inverter display — this is the single most likely thing to be wrong.

| Quantity | Register(s) | Canonical id | Scale (assumed) | Confidence |
|---|---|---|---|---|
| PV total power | 116–117 | `dc.power.total` | ×0.1 W | medium |
| AC / grid power | 40–41 | `ac.power.total` | ×0.1 W | low (sign/meaning unclear) |
| Battery power | 1009–1010 | `battery.power` | ×0.1 W | medium — labelled "discharge power"; sign convention TBD |
| Battery SoC | 1014 | `battery.soc` | ×1 % | high |
| Battery voltage | 1013 | `battery.voltage` | ×0.1 V | medium |
| Battery temperature | 1040 | `battery.temperature` | ×0.1 °C | medium |
| Energy today / total | not in source | `energy.today` / `energy.total` | — | to find (Protocol II doc) |

The battery-power sign is the first thing to nail down: our canonical convention is
positive = charging (see `measurement.h`). The SPH register is labelled "discharge power", so it
may need negating, or there may be separate charge/discharge registers to combine.

## Writing — holding registers (0x03 / 0x06)

**Battery control on the SPH is not a single "mode" write.** Priority is driven by time slots,
and the obvious mode register is firmware-dependent:

| Function | Register(s) | Values | Confidence / caveat |
|---|---|---|---|
| Priority mode | 1044 | 0=Load, 1=Battery, 2=Grid | **UNRELIABLE**: read-only on some firmware/profiles, writable on others. Do not rely on it as the control surface. |
| AC charge enable | 1092 | 0=disable, 1=enable | medium — lets the battery charge from grid |
| Discharge power rate | 1070 | 0–100 % | medium |
| Discharge stop SoC | 1071 | 0–100 % | medium |
| Battery-First time slots | 1017–1025 | start/end/enable per slot (3 slots) | medium — this is the real control surface |
| Grid-First time slots | 1080–1088 | start/end/enable per slot (3 slots) | medium |

Consequence for the driver: "charge the battery now" is expressed as configuring a Battery-First
slot (and/or AC-charge enable), not as flipping register 1044. The command model already has the
right shape for this — `SetBatteryOperatingMode`, `SetBatteryChargeLimit`, `SetMinimumSoc` — but
the mapping from a clean command to SPH time-slot registers is real driver work and must be
proven on hardware before it is trusted.

## Two register generations — resolve this FIRST

The authoritative Growatt Modbus RTU Protocol II V1.39 (machine-readable register CSV from
0xAHA/Growatt_ModbusTCP) shows the SPH register space is split by generation:

- **"Second group" (~125-1044, the older SPH):** the map at the top of this document
  (battery SoC 1014, etc.), from the hardware-tested Node-RED source.
- **"TL-X / TL-XH group" (3000+, newer):** the official V1.39 puts battery telemetry and
  control here — SoC **3002**, voltage 3003, charge current 3004, power **3005/3006** (32-bit),
  today/total discharge energy 3008/3009 and 3015-3017.

Which one an SPH6000 answers depends on model age and firmware, and the community sources
disagree. **The driver reads both ranges and TRACE-dumps them**, so the first bring-up settles
it in one look: whichever generation returns real values is the one this inverter speaks. Only
then are the read mappings and the control registers below trusted. Writing to the wrong
generation's registers on a live battery inverter is exactly the risk that keeps control gated.

## Control registers (writable holding, from V1.39 — confirm generation before use)

3000-series (TL-X/TL-XH generation). Single 16-bit holding-register writes (fn 0x06), each
reversible, each mapping cleanly onto the canonical command model:

| Control | Register | Range | Canonical command |
|---|---|---|---|
| Inverter active-power limit | **3** | 0-100 % (255 = off) | `SetActivePowerLimitPercent` |
| Battery-First charge power rate | **3047** | 0-100 % | `SetBatteryChargeLimit` (as %) |
| Battery-First stop-charge SoC | **3048** | 0-100 % | `SetMaximumSoc` |
| Grid-First discharge power rate | **3036** | 0-100 % | `SetBatteryDischargeLimit` (as %) |
| Grid-First stop-discharge SoC | **3037** | 0-100 % | `SetMinimumSoc` |
| AC (grid) charge enable | **3049** | 0/1 | `SetBatteryOperatingMode` component |

Register **3 (active-power limit)** is in the common base group, generation-independent, and
the safest first write to test: it curtails inverter output 0-100 %, is trivially reversed
(set back to 100), and does not depend on the 1000-vs-3000 battery question. It is the natural
first control to prove the write path on hardware.

Battery *mode* (Battery First / Grid First / Load First) is driven by time-slot registers
(3038-3056), not a single mode write — a later, more involved mapping. The percentage/SoC
writes above are the useful, low-risk first control surface.

Every write goes through the existing CommandDispatcher (kill switch on by default, rate limit,
range check) and must be write-verified (write → read back → confirm) before being reported
as applied.

## Sources

- Growatt Inverter Modbus RTU Protocol II (official), e.g. V1.39 2024-04-16 —
  <https://shop.frankensolar.ca/content/documentation/Growatt/AppNote_Growatt_WIT-Modbus-RTU-Protocol-II-V1.39-English-20240416_(frankensolar).pdf>
- 8none1/growatt_sph_nodered register map (hardware-tested, Node-RED) —
  <https://github.com/8none1/growatt_sph_nodered/blob/main/registers.md>
- 0xAHA/Growatt_ModbusTCP — machine-readable V1.39 register CSV and protocol spreadsheets —
  <https://github.com/0xAHA/Growatt_ModbusTCP/tree/main/Protocols>
- Forum, MQTT control of a Growatt SPH6000 via Solar Assistant —
  <https://diysolarforum.com/threads/mqtt-to-growatt-sph-6000-via-solar-assistant.102792/>
- bobbesnl/ModbusGrowatt_HomeAssistant (SPH read **and** write) —
  <https://github.com/bobbesnl/ModbusGrowatt_HomeAssistant>
- grott protocol docs — <https://github.com/johanmeijer/grott>
- SunSpec Energy Storage model (Model 120), for the canonical measurement shape —
  <https://sunspec.org/sunspec-energy-storage-model-description/>

## Bring-up procedure (first hardware session)

1. Flash the standard `waveshare-eversolar` build (historic name — it carries every hardware
   driver, this one included), join it to WiFi via the setup wizard.
2. **Settings → Logging → Level → `trace`** (applied immediately, no restart). The raw
   register dumps are TRACE-only; with the default `info` level the logs endpoint will show
   no register data at all — the response's `level` field tells you which level is active.
3. Wire RS485 to the SYS/COM port, run the **extended** discovery scan (quick tries only
   9600; extended also tries 115200).
4. `curl -u admin "http://<bridge>/api/v1/logs?limit=60"` — the `GROWATT in <reg>: ...` lines
   are the raw blocks. Whichever range (1000-series vs 3000-series) carries real values
   settles the register generation.
5. Compare SoC / battery voltage / power against the inverter display; correct
   `profiles/growatt/sph.toml` accordingly (the register map is a build-time-generated TOML
   profile since 2026-07-21 — see docs/adding-a-device.md) and rebuild. Only then trust the
   values — and only then start on the write path.

## Open questions to resolve on hardware

1. Line speed (9600 vs 115200) and unit id on the colleague's unit.
2. Scaling of every measurement, against the inverter display.
3. Battery-power sign, and whether charge/discharge are one register or two.
4. Whether register 1044 is writable on this firmware, or whether control must go entirely
   through the time-slot registers.
5. Energy today/total register numbers (not in the community map used here).
