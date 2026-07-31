# GoodWe ET / EH / BT / BH — Modbus RTU register map

Residential hybrid with a battery port. The profile lives in
[`profiles/goodwe/et_hybrid.toml`](../profiles/goodwe/et_hybrid.toml).

**STATUS: Experimental, single-sourced, read-only.** Not confirmed against a physical inverter by
this project.

## Connection

| | |
|---|---|
| Port | RS485 on the inverter's COM terminal |
| Line | 9600 8N1 |
| **Unit id** | **247** — see below |
| Register space | Holding registers (function 03) |

### The unit id will catch you out

GoodWe ships this family at Modbus address **247**, not 1. The driver's `unit_id` option defaults
to 1, so it has to be set explicitly. Get it wrong and the inverter simply never answers — which
on a fresh install looks exactly like a wiring fault, and sends people to check A/B polarity for
an hour.

The profile cannot express this: there is no per-profile default unit id in the schema today. If
this trips up more than one vendor it is worth adding.

## Source, and why there is only one

| | What |
|---|---|
| **A** | [marcelblijleven/goodwe](https://github.com/marcelblijleven/goodwe), `goodwe/et.py` and `goodwe/sensor.py` |

GoodWe's own *Modbus Protocol Hybrid ET/EH/BT/BH* PDF **was** obtained, and could not be read.
Its text is entirely in subset fonts: extraction yields glyph indices, not a single readable word.
The Sungrow document worked because it mixed fonts and happened to leave its register tables in a
plain one; this one does not.

So this map has the same standing as the [Huawei](huawei-sun2000-protocol.md) one — a single
careful transcription rather than two agreeing sources. The bring-up checklist asks for the
cross-check that could not be done here.

### Scaling lives in the classes

The source library encodes scale in its sensor *classes* rather than per row: `Voltage` and
`Current` divide by 10, `Frequency` by 100, `Energy4` by 10, `Temp` by 10. Those divisors appear
in the profile as multipliers. A row's class is therefore the only place its scale is written
down — read `sensor.py` alongside `et.py`, not `et.py` alone.

## Sentinels

GoodWe uses the same convention Huawei does: an unavailable reading comes back as a fixed pattern
rather than an exception, and the library maps `0xFFFF`, `0xFFFFFFFF` and `0x7FFF` to "no value".
Every row that can go unavailable carries the sentinel for its width, so a profile pointed at an
inverter with no battery leaves those channels absent instead of publishing a phantom one.

**One known limitation.** The library treats *both* `0x7FFF` **and** `0xFFFF` (−1) as "no value"
on temperature registers, and this schema declares one sentinel per row. The profile declares the
dangerous one: `0x7FFF` would otherwise publish 3276.7 °C, while `0xFFFF` decodes as −0.1 °C —
wrong, but harmless-looking rather than alarming. If a bench session shows `0xFFFF` appearing in
practice, that is the moment to consider multi-sentinel support.

## Mapped registers

All holding. `value = raw × scale`, sentinel checked first.

| Register | Type | Scale | Sentinel | Canonical id |
|---|---|---|---|---|
| 35103 | u16 | 0.1 | `0xFFFF` | `dc.mppt_1.voltage` |
| 35104 | u16 | 0.1 | `0xFFFF` | `dc.mppt_1.current` |
| 35105 | u32 | 1 | `0xFFFFFFFF` | `dc.mppt_1.power` |
| 35107 | u16 | 0.1 | `0xFFFF` | `dc.mppt_2.voltage` |
| 35108 | u16 | 0.1 | `0xFFFF` | `dc.mppt_2.current` |
| 35109 | u32 | 1 | `0xFFFFFFFF` | `dc.mppt_2.power` |
| 35121 | u16 | 0.1 | `0xFFFF` | `ac.phase_l1.voltage` |
| 35122 | u16 | 0.1 | `0xFFFF` | `ac.phase_l1.current` |
| 35123 | s16 | 0.01 | — | `ac.frequency` |
| 35138 | s16 | 1 | — | `ac.power.total` — the inverter's output |
| 35176 | s16 | 0.1 | `0x7FFF` | `inverter.temperature` |
| 35180 | u16 | 0.1 | `0xFFFF` | `battery.voltage` |
| 35191 | u32 | 0.1 | `0xFFFFFFFF` | `energy.total` |
| 35193 | u32 | 0.1 | `0xFFFFFFFF` | `energy.today` |
| 35206 | u32 | 0.1 | `0xFFFFFFFF` | `battery.energy_charged` |
| 35209 | u32 | 0.1 | `0xFFFFFFFF` | `battery.energy_discharged` |
| 37003 | s16 | 0.1 | `0x7FFF` | `battery.temperature` |
| 37007 | u16 | 1 | `0xFFFF` | `battery.soc` |

### Which register is "AC power"

35138 and 35140 sit next to each other and are both signed watt values. The source tags 35138
`Kind.AC` — the inverter's own output — and 35140 `Kind.GRID` — the flow at the meter. On a hybrid
those are different quantities, and that tag is the only thing distinguishing them.

`ac.power.total` maps 35138. 35140 stays out: it is a single signed bidirectional value where our
canonical grid channels are two separate unsigned rails.

## Not mapped

**`battery.power` (35182) and `battery.current` (35181).** Signed in the source, which does not
say which direction positive means — it passes both straight through.

This is the **fourth hybrid in a row** where battery power is held back:

| Profile | Why |
|---|---|
| Deye | both sources silent on the direction |
| Solis | direction in a separate register; combining is arithmetic a profile cannot do |
| Huawei | undocumented in the one source read |
| GoodWe | undocumented in the one source read |
| *Sungrow* | *mapped* — the datasheet states it, so `scale = -1` corrects it |

The older `sph` profile maps it too, at register 1009, with a comment recording that its sign is
unconfirmed — noted here so this table is not read as saying no other profile has the channel.

**House consumption.** The source computes it from several registers rather than reading one, and
a profile does no arithmetic. `load.power` exists in the vocabulary now, so the id is no longer
the obstacle — the arithmetic is, and that stays out of a data file.

## Writing: nothing, deliberately

There are **no `[[write]]` rows**, and that is a sourcing decision rather than an oversight.

The publicly circulating GoodWe hybrid protocol document is titled **"Read Only"**, and the source
library exposes no write targets for this family that a profile could point at. The project rule
is that an undocumented write target is disqualifying rather than something to try, so nothing is
declared — not even dormant.

What would change that: a vendor write-capable protocol document, or a second implementation that
writes to this family on real hardware. Either turns "no source" into research worth recording.

## Bring-up checklist

- [ ] Wire A/B to the COM terminal.
- [ ] Select driver `modbus_profile`, profile `goodwe_et_hybrid`, and **set `unit_id` to 247**.
- [ ] Set log level `trace`; confirm both blocks answer.
- [ ] Check each mapped row against the inverter's own display **at that moment**.
- [ ] Check a channel that should be unavailable — it must be absent, not a large number.
- [ ] Cross-check against GoodWe's own protocol document if you can read a copy; this profile has
      not been.
- [ ] Report back either way.
