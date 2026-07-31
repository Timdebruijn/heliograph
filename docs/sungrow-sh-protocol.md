# Sungrow SH residential hybrid — Modbus RTU register map

Single-phase hybrid with a battery port. The profile lives in
[`profiles/sungrow/sh_hybrid.toml`](../profiles/sungrow/sh_hybrid.toml).

**STATUS: Experimental — but the best-sourced map in this tree.** The rows come from Sungrow's
own protocol specification, cross-checked register by register against a widely deployed
implementation. Not yet confirmed against a physical inverter by this project.

## Connection

| | |
|---|---|
| Port | RS485 on the inverter's own COM terminal (A1/B1); no WiNet dongle required |
| Line | 9600 8N1, RTU |
| Register space | **Input registers (function 04)** for readings; holding (03) for setpoints |

The vendor document states this directly: communication runs over the RS485 interface on the
inverter *or* the Ethernet interface of a WiNet-S/Logger, so a direct RS485 connection is a
first-class path rather than a workaround.

## Sources

| | What |
|---|---|
| **A** | Sungrow, *Communication Protocol of Residential Hybrid Inverter* **V1.1.9** (PDF) — a vendor specification, the top of the trust order in [adding-a-device.md](adding-a-device.md) |
| **B** | [mkaiser/Sungrow-SHx-Inverter-Modbus-Home-Assistant](https://github.com/mkaiser/Sungrow-SHx-Inverter-Modbus-Home-Assistant), `modbus_sungrow.yaml` |

Source B is unusually useful for cross-checking because every entry carries **both** the protocol
address and the document's own register number in a comment
(`address: 5016 # reg 5017`) — which turns the comparison into something mechanical rather than
a judgement call.

### A note on reading the PDF

No PDF tooling was available, so the document was decompressed and its text operators extracted
directly. That worked for the register tables — addresses, data types and units come out
plainly — but the document mixes fonts, and **some name columns are in a subset font whose
glyph codes did not survive extraction**. Where that happened, the row is identified in this
profile by its address, type and unit from the document, with the *name* taken from source B.
That split is called out per row in the profile comments; it is not a detail to paper over.

## Two traps, and both apply to every row

### The numbering trap

**The vendor document numbers registers from 1. Modbus addresses them from 0.**

```
PDU address = document register number − 1
```

The document's "Total DC power 5017~5018" is PDU **5016**. Source B writes exactly that, and the
relationship was verified mechanically across **all 88** of its addressed entries: the offset is
1 in every single one, no exceptions, in both the 5000 and 13000 ranges.

Every address in the profile is a PDU address. Off by one here reads a neighbouring register that
is usually also a plausible number — the kind of wrong that survives a casual bench check.

### The word-order trap

**This family stores every double-word value LOW WORD FIRST.** Source B declares `swap: word` on
**all 21** of its 32-bit sensors — checked mechanically, no exceptions — and the vendor document
says the same in its type definitions.

So every `u32`/`s32` row in the profile carries `word_order = "low_first"`. An earlier version of
this document and the profile had it on the battery-power row alone, with a comment claiming that
row was "the opposite of every other 32-bit value in this map". That was false, and it cost six
rows a factor of 65536 each — 4 kW of PV would have published as about 262 megawatts.

It was caught in review by checking the source mechanically instead of believing the comment, and
the profile now has a test asserting the convention per family rather than per row. The
frame-decoding test did **not** catch it: that test was written from the profile, so it agreed
with the mistake.

## Mapped registers

PDU addresses, all input registers. `value = raw × scale`.

| PDU | Doc | Type | Scale | Canonical id |
|---|---|---|---|---|
| 5007 | 5008 | s16 | 0.1 | `inverter.temperature` |
| 5010 | 5011 | u16 | 0.1 | `dc.mppt_1.voltage` |
| 5011 | 5012 | u16 | 0.1 | `dc.mppt_1.current` |
| 5012 | 5013 | u16 | 0.1 | `dc.mppt_2.voltage` |
| 5013 | 5014 | u16 | 0.1 | `dc.mppt_2.current` |
| 5016 | 5017 | u32 | 1 | `dc.power.total` |
| 5018 | 5019 | u16 | 0.1 | `ac.phase_l1.voltage` |
| **5213** | **5214** | **s32, low-word-first, scale −1** | | `battery.power` — see below |
| 5241 | 5242 | u16 | 0.01 | `ac.frequency` |
| 13001 | 13002 | u16 | 0.1 | `energy.today` |
| 13002 | 13003 | u32 | 0.1 | `energy.total` |
| 13019 | 13020 | u16 | 0.1 | `battery.voltage` |
| 13022 | 13023 | u16 | **0.1** | `battery.soc` — tenths of a percent |
| 13024 | 13025 | s16 | 0.1 | `battery.temperature` |
| 13026 | 13027 | u32 | 0.1 | `battery.energy_discharged` |
| 13030 | 13031 | s16 | 0.1 | `ac.phase_l1.current` |
| 13033 | 13034 | s32 | 1 | `ac.power.total` |
| 13040 | 13041 | u32 | 0.1 | `battery.energy_charged` |

Three of these are worth stating explicitly, because each is a mistake waiting to happen:

**Battery SoC is in tenths of a percent.** Both sources say so. Read as whole percent, a full
battery publishes as 1000%.

**Two frequency registers exist**, both in the document: 5036 at 0.1 Hz and 5242 at 0.01 Hz. The
finer one is mapped. 0.1 Hz is too coarse to see the grid excursions this channel is worth
watching for.

**Energy comes from the hybrid block, not the common block.** The 5003/5004 pair is named "PV
generation **& battery discharge**" by both sources — it includes energy that came back out of
the battery, so feeding it to an energy dashboard counts stored solar twice. 13002/13003 is PV
generation alone.

## Battery power: the register that needed two new schema features

This is the first `battery.power` row in the tree that could be mapped at all. The Deye map
leaves it out because neither source states the direction; the Solis map leaves it out because
the direction lives in a *separate* register. Sungrow states everything — and then needs two
corrections at once.

Source B records the vendor's own guidance alongside the register:

> In datasheet (1.1.11) it is recommended to use this register instead of 13022
> negative: Battery charging
> positive: Battery discharging

The document's own table confirms the shape — row 31, registers **5214-5215, S32, 1W** — while
the row's *name* is in the subset font that did not extract. So: the document supplies the
address, width and unit; source B supplies the name and the sign convention, and the fact that
the datasheet steers you here rather than to the unsigned alternative at 13022.

Two things follow:

1. **`word_order = "low_first"`.** Source B reads this pair with `swap: word` — the low half sits
   at the *lower* address. Read the default way round, 2 kW decodes as roughly 34 megawatts.
2. **`scale = -1`.** Our canonical convention is positive while *charging* (the SunSpec storage
   convention). This device states the opposite, so the sign is flipped in the profile rather
   than in C++.

Both are corrections a source **states**. That is exactly the line the other two hybrid profiles
could not cross.

Low-word-first support was added to the profile schema for this register. It was previously
documented as "open an issue rather than mapping it wrong", which in practice meant declining to
read the register the manufacturer points you at.

## Not mapped

- **Export power (13010).** Both sources agree. (Load power was in this list until `load.power`
  joined the [vocabulary](device-profiles/canonical-measurements.md); PDU 13007 is mapped now and
  appears in the table above.) Export power is
  signed and bidirectional; our grid channels are two unsigned rails, so splitting it needs
  arithmetic a profile cannot do.
- **Imported/exported energy (13036/13037, 13045/13046).** Agreed, but our `grid.*` channels are
  instantaneous power rather than energy.
- **MPPT 3 (5014/5015) and MPPT 4 (5114/5115).** Real on the larger models and present in both
  sources; `mppts = 2` describes what this profile maps. A three-tracker unit shows its third
  string in the raw dump and gains two rows once somebody confirms the model.
- **The alternative battery power at 13021.** Unsigned, and the register the datasheet steers
  away from.

## Writing

Both setpoints are corroborated by both sources — better standing than the single-sourced write
rows in the Deye and Solis profiles — and both stay dormant until somebody confirms them on
hardware.

| PDU | Doc | Command | Notes |
|---|---|---|---|
| 13073 | 13074 | `set_export_limit_watts` | u16, watts, step 10 |
| 13049 | 13050 | `set_battery_operating_mode` | 0 Self-consumption, 2 Forced, 3 External EMS, 4 VPP |

**The export limit has an enable switch.** On this family the limit is governed by a separate
mode register (13086 in source B's numbering). Writing a limit while that switch is off changes a
stored number and nothing else — which on a bench looks exactly like a write that did not work.
Check the mode register first.

Mode **1 is deliberately absent** from the options list: neither source lists it, and filling the
gap by assuming the numbering is contiguous is the guess that puts a hybrid in an untested state.

## Bring-up checklist

- [ ] Wire A/B to the inverter's RS485 terminal.
- [ ] Select driver `modbus_profile`, profile `sungrow_sh_hybrid`.
- [ ] Set log level `trace`; confirm all three blocks answer.
- [ ] Check each mapped row against the inverter's own display **at that moment**.
- [ ] **Watch battery power through a charge→discharge crossover.** It should read positive
      while charging. If it reads negative, the `scale = -1` is wrong for this firmware; if it
      reads in the tens of megawatts, the word order is.
- [ ] Report back either way.
