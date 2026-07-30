# Huawei SUN2000 — Modbus RTU register map

Residential string inverter, optionally with a LUNA2000 battery. The profile lives in
[`profiles/huawei/sun2000.toml`](../profiles/huawei/sun2000.toml).

**STATUS: Experimental, and single-sourced** — see the caveat below. Not confirmed against a
physical inverter by this project.

**Not SunSpec.** Huawei implements its own register map, so the generic
[`sunspec` driver](sunspec.md) does not apply. (An earlier note in this repo grouped Huawei with
SolarEdge and Fronius; that was wrong.)

## Connection

| | |
|---|---|
| Port | RS485A1 / RS485B1 on the inverter's COM port |
| Line | 9600 8N1 |
| Register space | **Holding registers (function 03)** throughout — there is no input-register map |

Huawei is unusually particular about being polled: the documented behaviour is one request at a
time with a pause between them, and an inverter asked for too much too quickly stops answering
for a while. The profile therefore declares four narrow blocks rather than two wide sweeps.

## Source, and the caveat

| | What |
|---|---|
| **A** | [wlcrs/huawei-solar-lib](https://github.com/wlcrs/huawei-solar-lib), `register_definitions/` and `registers.py` — the library behind the [wlcrs/huawei_solar](https://github.com/wlcrs/huawei_solar) Home Assistant integration |

**One source.** Huawei's own *Solar Inverter Modbus Interface Definitions* exists and is what the
library transcribes — it is the natural second — but no machine-readable copy was obtained for
this profile the way Sungrow's was. So unlike the Sungrow map, **nothing here has been checked
against the vendor's own numbering**.

That is a real difference in standing, not a formality. Treat every row as one careful
transcription rather than two agreeing ones, and check the bring-up dump against the inverter's
display before trusting any of it.

### Gain is a divisor

The library expresses scaling as a **gain it divides by**: `value = raw / gain`. This schema
multiplies, so every gain appears in the profile as its reciprocal — gain 10 is `scale = 0.1`,
gain 1000 is `scale = 0.001`.

Getting that backwards is a factor of 100 on a current reading, which is exactly the kind of wrong
that still looks plausible on a dashboard.

## The sentinel

Huawei does **not** answer an unavailable reading with an exception or a zero. It answers with a
fixed pattern per register width:

| Width | Sentinel |
|---|---|
| U16 | `0xFFFF` |
| I16 | `0x7FFF` |
| U32 | `0xFFFFFFFF` |
| I32 | `0x7FFFFFFF` |

The source library turns exactly those into "no value". Decoded as numbers instead, they are not
obviously wrong:

- an inverter that has shut down for the night publishes **3276.7 °C**;
- a SUN2000 with **no LUNA2000 attached** publishes **6553.5 %** state of charge — all night,
  every night, because the battery range answers with sentinels rather than refusing.

Both have the shape of a real reading, which is what makes them dangerous. This is the register
family that put `invalid` into the profile schema: a matching register is left **undeclared**,
the same outcome as a block that was never read.

The comparison is exact, deliberately. `0x7FFE` is a perfectly good temperature reading sitting
one below the sentinel, and a guard written as "greater than or equal" would eat it. There is a
test for precisely that.

## Mapped registers

All holding. `value = raw × scale`, with the sentinel checked first.

| Register | Type | Scale | Sentinel | Canonical id |
|---|---|---|---|---|
| 32016 | s16 | 0.1 | `0x7FFF` | `dc.mppt_1.voltage` |
| 32017 | s16 | 0.01 | `0x7FFF` | `dc.mppt_1.current` |
| 32018 | s16 | 0.1 | `0x7FFF` | `dc.mppt_2.voltage` |
| 32019 | s16 | 0.01 | `0x7FFF` | `dc.mppt_2.current` |
| 32064 | s32 | 1 | `0x7FFFFFFF` | `dc.power.total` (input power) |
| 32069 | u16 | 0.1 | `0xFFFF` | `ac.phase_l1.voltage` |
| 32072 | s32 | **0.001** | `0x7FFFFFFF` | `ac.phase_l1.current` — milliamp resolution |
| 32080 | s32 | 1 | `0x7FFFFFFF` | `ac.power.total` |
| 32085 | u16 | 0.01 | `0xFFFF` | `ac.frequency` |
| 32087 | s16 | 0.1 | `0x7FFF` | `inverter.temperature` |
| 32106 | u32 | 0.01 | `0xFFFFFFFF` | `energy.total` |
| 32114 | u32 | 0.01 | `0xFFFFFFFF` | `energy.today` |
| 37760 | u16 | 0.1 | `0xFFFF` | `battery.soc` |
| 37763 | u16 | 0.1 | `0xFFFF` | `battery.voltage` |
| 37780 | u32 | 0.01 | `0xFFFFFFFF` | `battery.energy_charged` |
| 37782 | u32 | 0.01 | `0xFFFFFFFF` | `battery.energy_discharged` |

Phase A only. Three-phase SUN2000 models report B and C alongside; `phases = 1` describes what
this profile maps, and a three-phase unit shows the others in the raw dump.

## Not mapped

**`battery.power` (37765, I32, watts).** The register is there and it is signed, and neither the
library nor the Home Assistant integration built on it states which direction positive means —
both pass the signed value straight through.

This is the **third hybrid in a row** where battery power is held back, and each time for a
different reason:

| Profile | Why battery power is absent |
|---|---|
| Deye | both sources silent on the direction |
| Solis | direction lives in a *separate* register; combining them is arithmetic a profile cannot do |
| Huawei | undocumented in the one source read |
| *Sungrow* | *mapped* — the datasheet states the direction, so `scale = -1` corrects it |

(The older `sph` profile also maps it, at register 1009, with its own comment recording that the
sign convention is unconfirmed — so it is the exception that proves the rule rather than a
counter-example.)

Huawei's own interface document states the convention. Settle it there, or on the bench through a
charge→discharge crossover.

**The fixed-watt export limit (40126, U32).** Writable, and the natural companion to the
percentage limit below — but 32 bits means two registers and this firmware's write path is FC06
single-register only, so declaring it would produce a row that validates and can never dispatch.

**Meter registers (37113 onward)** and the built-in/external energy-meter blocks: single signed
bidirectional values, where our canonical grid channels are two unsigned rails.

## Writing

| Register | Command | Range | Notes |
|---|---|---|---|
| 40125 | `set_active_power_limit_percent` | 0–100 %, step 0.1 | s16, gain 10 → `scale = 0.1`; 50.0 % is raw 500 |

The standard curtailment control on this family, and a single 16-bit register — the one shape this
firmware's write path can serve. Dormant until `verified = true`.

Two things to establish on the bench before that flag moves:

1. **Does the inverter accept FC06 for it?** This firmware sends nothing else.
2. **Does a control-mode register have to select percentage derating first?** The library exposes
   one at 47415. A limit written while the mode says otherwise is a stored number that changes
   nothing — which looks exactly like a failed write.

## Bring-up checklist

- [ ] Wire A/B to RS485A1/RS485B1 on the COM port.
- [ ] Select driver `modbus_profile`, profile `huawei_sun2000`.
- [ ] Set log level `trace`; confirm all four blocks answer. If reads start failing after a while,
      suspect poll rate before wiring — this family throttles.
- [ ] Check each mapped row against the inverter's own display **at that moment**.
- [ ] **Check a channel that should be unavailable** — an inverter with no battery, or any
      register at night. It must be absent in `/api/v1/status`, not a large number.
- [ ] Cross-check the map against Huawei's own interface document if you have it; this profile has
      not been.
- [ ] Report back either way.
