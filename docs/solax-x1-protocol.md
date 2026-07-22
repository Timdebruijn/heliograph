# SolaX X1 series — PMU-family RS485 protocol

Driver: `src/drivers/solax_x1/`. Framing shared with the other PMU-family driver:
`src/protocols/pmu/` (AA 55 header, 2+2 byte addressing, control/function codes, 16-bit
sum checksum, response = request function | 0x80).

**STATUS: transcribed, not hardware-confirmed.** Sources: syssi/esphome-solax-x1-mini
(Apache-2.0; hardware-tested against X1 Mini G1/G2/G3; protocol knowledge re-implemented,
no code copied) and the official "SolaxPower Single Phase External Communication Protocol
X1 Series V1.2" document. First hardware target: an X1-1.1-S-D (planned 2026-07).

## Session flow

Line: 9600 8N1. Master = PMU address `01 00`; inverters live at `00 <addr>`.

1. **Discovery** — `10 00` broadcast. Only an *unregistered* inverter answers: `10 80`
   from source `00 00` with its **14 raw serial bytes**. A registered inverter ignores
   this (same asymmetry as the sibling protocol).
2. **Address assignment** — `10 01` with the serial echoed byte-exact + 1 address byte
   (reference convention: `0x0A`). The reference sends this frame with **source `00 00`**,
   not the PMU address; we mirror that. Official flow answers `10 81` with `0x06`; the
   reference never waits for it, so the driver treats the ACK as optional and verifies by
   a status query when it is absent.
3. **Poll** — `11 02` → `11 82` status report. Device info: `11 03` → `11 83`.
4. **No RE_REGISTER (`10 04`) in this driver.** The reference never sends it, and the
   deterministic assigned address makes it unnecessary: a cold start against a
   still-registered inverter is resolved by querying `0x0A` directly. Registration is
   established once and kept through timeouts; recovery is a plain offline query
   (sunrise-incident discipline, 2026-07-21).

## Status report payload (`11 82`)

Lengths per generation: G2 = 50, G1 = 52 (+CT Pgrid), G3 = 56. First 50 bytes are common;
the driver accepts ≥ 50 and decodes only the common prefix. All fields big-endian u16
unless noted.

| Offset | Field | Scale/type |
|---|---|---|
| 0 | temperature | int16, °C |
| 2 | energy today | ×0.1 kWh |
| 4 / 6 | PV1 / PV2 voltage | ×0.1 V |
| 8 / 10 | PV1 / PV2 current | ×0.1 A |
| 12 | AC current | ×0.1 A |
| 14 | AC voltage | ×0.1 V |
| 16 | grid frequency | ×0.01 Hz |
| 18 | AC power | 1 W |
| 20 | (unused) | |
| 22 | energy total | u32 **big-endian**, ×0.1 kWh — endianness UNPROVEN on hardware |
| 26 | runtime | u32 big-endian, hours — same caveat |
| 30 | operating mode | 0 Wait, 1 Check, 2 Normal, 3 Fault, 4 Permanent Fault, 5 Update, 6 Self Test |
| 32–44 | protection thresholds | not decoded (TRACE dump only) |
| 46 | error bitmask | u32 **little-endian** (per the reference — opposite to the rest) |
| 50+ | generation tail | ignored |

DC power is not in the payload; the driver derives it (PV V × I, `derived` flag set).
PV2: the Mini datasheet says one MPPT but the payload carries two field sets — the PV2
channels appear only once PV2 shows real voltage (> 1 V) and then stay.

## Device info payload (`11 83`), 58 bytes

| Offset | Width | Field |
|---|---|---|
| 0 | 1 | device type (raw byte) |
| 1 | 6 | rated power (ASCII) |
| 7 | 5 | firmware version (ASCII) |
| 12 | 14 | module name → model (e.g. "X1-1.1-S-D") |
| 26 | 14 | factory name → manufacturer |
| 40 | 14 | serial number (ASCII) |
| 54 | 4 | rated bus voltage (ASCII) |

Known quirk (reference project): some firmware returns identical serial numbers across
units — do not key multi-device logic on this serial.

## Bring-up runbook (first hardware session)

Same script as the other drivers (see docs/growatt-sph-protocol.md for the long form):

1. Flash the combined build; select driver `solax_x1` (or run discovery — note both PMU
   drivers may answer, the margin rule then asks for a manual confirm; pick `solax_x1`).
2. Log level `trace`; watch `/api/v1/logs` for `SOLAX ...` transaction lines and payload
   hex dumps.
3. Wire RS485 A/B to the inverter's COM port. No reply → swap A/B first, then check the
   connector pinout against the X1 manual.
4. Verify against the inverter display/app: AC power, voltage, energy today. **First
   suspects when something is off:** the two u32 endianness assumptions (energy total,
   runtime) and the AC power scale (1 W assumed, not 0.1).
5. Payload length in the trace tells the generation (50/52/56). Record it here.
6. After validation: update the STATUS lines here and in solax_parser.h / solax_driver.h,
   and promote the driver Experimental → Beta.

## Export control (future, separate mode — not this driver)

The X1 Mini has no writable power registers. Curtailment works via the inverter's
"export control: meter" mode: the inverter then acts as Modbus RTU **master** polling an
SDM230-style meter ~1×/s on the same RS485 port, and regulates output toward the reported
grid power. Implementing that means the bridge emulates the meter (Modbus slave role) and
the RS485 port becomes control-only — monitoring XOR control. Prerequisites before this
runs at anyone's home: verify the inverter's failsafe when the meter goes silent, and a
design round for the new slave role. See the memory/plan notes; reference:
syssi's solax_meter_gateway component.
