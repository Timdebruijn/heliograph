# Solis (Ginlong) RHI hybrid — Modbus RTU register map

Single-phase hybrid with a battery port. The profile lives in
[`profiles/solis/rhi_hybrid.toml`](../profiles/solis/rhi_hybrid.toml).

**STATUS: Experimental.** Every mapped row is corroborated by two independent implementations
that agree on address, width and scale. Nothing here has been confirmed against a physical
inverter by this project.

## Connection

| | |
|---|---|
| Port | RS485 on the inverter's COM terminal |
| Line | 9600 8N1 |
| Register space | **Input registers (function 04)** for readings, in the 33000 range; **holding (function 03)** in the 43000 range for setpoints |
| Writes | Function code not established by either source — see [Writing](#writing) |

## Sources

| | Project | File |
|---|---|---|
| **A** | [wills106/homeassistant-solax-modbus](https://github.com/wills106/homeassistant-solax-modbus) | `custom_components/solax_modbus/plugin_solis.py` |
| **B** | [StephanJoubert/home_assistant_solarman](https://github.com/StephanJoubert/home_assistant_solarman) | `custom_components/solarman/inverter_definitions/solis_hybrid.yaml` |

Independence was checked rather than assumed: different projects, different authors, different
file formats, and neither is a generated copy of the other. (Worth checking every time — the Deye
map's apparent third source turned out to be a byte-identical copy of its second.)

Source B scopes itself in its own header to the **RHI-(3-6)K-48ES-5G** and cites the register
document it was transcribed from. Source A covers the hybrid family plus several string
generations, tagging each entity with `allowedtypes=HYBRID`.

### The word-order trap

Source B lists a 32-bit value's registers **low word first**: `registers: [33030, 33029]`. Its
parser shifts the *first* entry by 0 and the next by 16, so the high word sits at the **lower**
address — which is the ordinary high-word-first convention this schema uses (`address = 33029,
type = "u32"`).

Read that list as an address order instead, and every 32-bit value in the map comes out wrong by
a factor of 65536. Lifetime generation would read in the hundreds of millions of kWh, which is
obvious; PV power would read plausibly wrong, which is not.

## Mapped registers

All input registers. `value = raw × scale`.

| Register | Type | Scale | Canonical id | Notes |
|---|---|---|---|---|
| 33029 | u32 | 1 | `energy.total` | kWh |
| 33035 | u16 | 0.1 | `energy.today` | kWh |
| 33049 | u16 | 0.1 | `dc.mppt_1.voltage` | |
| 33050 | u16 | 0.1 | `dc.mppt_1.current` | |
| 33051 | u16 | 0.1 | `dc.mppt_2.voltage` | |
| 33052 | u16 | 0.1 | `dc.mppt_2.current` | |
| 33057 | u32 | 1 | `dc.power.total` | whole-array PV power — no arithmetic needed, unlike Deye |
| 33073 | u16 | 0.1 | `ac.phase_l1.voltage` | |
| 33076 | u16 | 0.1 | `ac.phase_l1.current` | |
| 33079 | **s32** | 1 | `ac.power.total` | negative while importing to charge |
| 33093 | **s16** | 0.1 | `inverter.temperature` | signedness from source B |
| 33094 | u16 | 0.01 | `ac.frequency` | |
| 33133 | u16 | 0.1 | `battery.voltage` | |
| 33139 | u16 | 1 | `battery.soc` | % |
| 33161 | u32 | 1 | `battery.energy_charged` | kWh, lifetime |
| 33165 | u32 | 1 | `battery.energy_discharged` | kWh, lifetime |

## What the sources would not agree on

### Battery power — a conflict, not a gap

Both sources place a signed 32-bit watt value at **33149**. They disagree about whether to
believe the sign:

- **Source A refuses to.** It derives charge versus discharge from register **33135** (the
  charge-direction word, `0` = charging, `1` = discharging) and calls `abs()` on the magnitude.
  Solis got its own pair of value functions in that project specifically for this; the generic
  ones use a signed value directly.
- **Source B publishes the signed value as-is.**

Code that works whether or not a register is genuinely signed tells you its author did not know
either. And our canonical `battery.power` is one signed channel — deriving it here needs the
magnitude combined with a *separate* direction register, which is arithmetic across two registers
and something a profile deliberately cannot express.

So this is not a "settle it on the bench and add a row" item like the Deye equivalent: even with
the convention known, this schema cannot say it. What the bench session should record is what
33149 and 33135 do **together**, because that decides whether a direction-aware read is worth
building at all.

`battery.current` (33134) is signed in both sources with the direction unstated by either — the
same trap, and left out for the same reason.

### Meter power — a factor of a thousand apart

Source B reads **33257** as meter active power in watts at scale 1. Source A reads the same
address as one of three per-phase values at scale **0.001**. That is a conflict, not a rounding
difference, so neither reading is mapped and `grid.import_power`/`grid.export_power` stay empty.
The cumulative import/export *energy* registers (33169/33173) do agree — but our canonical grid
channels are instantaneous power.

### House load — mapped since `load.power` was added

Both sources carry house load (33147) and backup load (33148) and agree on both. House load is
**mapped** as `load.power`, which joined the
[vocabulary](device-profiles/canonical-measurements.md) on 2026-07-30; this section said it was
blocked on that id until the id existed. Backup load stays unmapped — it is a different quantity
(what the backup port carries, not what the house draws) and has no canonical id.

### Strings 3 and 4

33053–33056 appear in source A only; source B's file is scoped to the two-tracker RHI. Right for
a model this profile may not be describing is not the same as right.

## Writing

Both setpoints below come from **source A only**. Source B reads the storage mode's input-register
mirror at 33132 and never touches the 43000 range, so nothing corroborates the addresses, the
bounds, or the mode numbering. Both are declared with `verified = false`.

| Register | Command | Range | Notes |
|---|---|---|---|
| 43074 | `set_export_limit_watts` | 0–9900 W, step 100 | raw is hundreds of watts, so `scale = 100` |
| 43110 | `set_battery_operating_mode` | 13 modes | complete register values, not a masked field |

The export limit is the one worth attention: its bounds are real fixed numbers rather than a
per-model runtime value, which is exactly what made the equivalent Deye row unmappable. It is a
plain FC06 single-register row, so `verified` is its **only** gate — flip that and it goes live.

**Establish the write function code first.** Neither source shows what a Solis accepts for a
write, and this firmware sends FC06 only. An inverter that wants FC16 will refuse; one that
silently accepts a malformed write is worse.

The storage-mode numbering (1, 3, 17, 33, 35, 37, 41, 43, 49, 51, 64, 96, 98) is clearly
bit-composed — self-use, timed charge/discharge, backup and feed-in each contribute — but the
values are quoted exactly as source A lists them rather than reconstructed from that structure. A
mode number assembled by reasoning is precisely the guess that leaves a battery in a state nobody
has tested.

## Bring-up checklist

- [ ] Wire A/B to the COM terminal; unplug any vendor datalogger sharing that port.
- [ ] Select driver `modbus_profile`, profile `solis_rhi_hybrid`.
- [ ] Set log level `trace`; confirm all four blocks answer rather than time out.
- [ ] Check each mapped row against the inverter's own display **at that moment**.
- [ ] Watch **33149 together with 33135** through a charge→discharge crossover and write down
      both. That is the observation this map is missing.
- [ ] Report back either way — a map that turned out wrong is as useful as one that worked.
