# Solis (Ginlong) S6-EH3P hybrid — Modbus RTU register map

**Experimental**, like every register map here. Two independent integrations agree on every row
below. **No S6 has answered this file.**

## Which models

The profile is `solis_s6_eh3p_hybrid`, written for the **S6-EH3P(5–10)K-H** three-phase hybrid
family — four MPPT strings, high-voltage battery. It was built because a neighbour has an
**S6-EH3P8K-H** and the existing `solis_rhi_hybrid` profile could not read it.

**This is not the RHI map with phases bolted on.** `solis/rhi_hybrid.toml` is scoped to the
RHI-(3–6)K-48ES-5G: single phase, two trackers. The two families share the 33xxx input-register
*space*, and that is exactly the kind of resemblance that invites a wrong assumption — same space
is not same device. Every row here was re-derived.

Neither source names the 8K specifically. Source B's own tested list names `S6-EH3P10K-H-EU` and
`S6-EH3P(12-20)K-H`; Eniris, who integrate these commercially, list `S6-EH3P(5-10)K-H-EU`, the
bracket the 8K falls in. Same series, same battery class, one size up — strong, not proof.

## Connection

| | |
|---|---|
| Port | the **16-pin COM connector** at the bottom — a different connector from the one the WiFi/LAN datalogger stick uses |
| Signals | RS485 IN A → A/TX+, RS485 IN B → B/TX− |
| Unit id | **1** by default, changed from the **inverter's own display** (manual ch. 7.3.2) |
| Function code | 04 (input registers) for everything mapped here |
| Line | 9600 8N1 is the family default this profile ships |

Two things worth knowing before ordering parts.

**You do not need installer rights to read.** No source documents a menu enable, a password or an
installer unlock for Modbus, and the address is changed on the inverter itself rather than in the
vendor app. That matters to anyone whose installer has locked them out of their own installation.

**The baud rate is the least certain line here.** No hybrid-specific source states it. The 9600
8N1 figure that circulates in forums comes from the **S6-GR1P string** inverters, which are a
different map entirely — one of their own threads says so plainly: *"The standard inverter has
different registers."* If nothing answers, vary the rate first.

## Sources

Read as source code, at the commit named, rather than as documentation:

- **A** — `wills106/homeassistant-solax-modbus` @ `5845a931b`, `plugin_solis.py`.
  Declares `register`, `register_type`, `register_data_type` and `scale` per entity, and tags
  each with `allowedtypes` **X1** or **X3**.
- **B** — `Pho3niX90/solis_modbus` @ `011e31196`, `sensor_data/hybrid_sensors.py`.
  The integration behind `solis-modbus.readthedocs.io`. Declares a register list and a
  `multiplier`, and a `data_type` on some rows.

Different authors, different repositories, different data shapes. Neither is generated from the
other.

### How the phase split is known

Not guessed. In source A, register 33073 appears **twice**: as "Inverter Voltage" tagged `X1`,
and as "Inverter Voltage L1" tagged `X3`. 33076 has the same pair. 33074/33075/33077/33078 are
X3-only L2 and L3 entities. One map serves both topologies, and the same register is explicitly
L1 on a three-phase unit. Source B labels 33073–33075 and 33076–33078 as A/B/C phase
independently.

### Two traps in source B

Both would have shipped silent nonsense.

**A multiplier of `0` does not mean zero.** The decode path treats 0 and 1 alike
(`if multiplier == 0 or multiplier == 1`), and the bounds helpers spell the same rule as
`self.multiplier or 1` — 0 is falsy in Python. Copying a 0 into a scale zeroes the channel.

**Model-specific overrides.** `S6-EH3P10K-H-ZP` and Zonneplan-badged units apply **0.01** instead
of the default to registers 33142 and 33161–33168. Two of those — 33161 and 33165 — are mapped
below. A plain `-H` uses the defaults; a `-ZP` or Zonneplan owner must check those two rows.

## Mapped registers

All function 04, all corroborated by both sources.

| Register | Channel | Type | Scale | Unit |
|---|---|---|---|---|
| `33029` | `energy.total` | u32 | 1 | kWh |
| `33035` | `energy.today` | u16 | 0.1 | kWh |
| `33049` | `dc.mppt_1.voltage` | u16 | 0.1 | V |
| `33050` | `dc.mppt_1.current` | u16 | 0.1 | A |
| `33051` | `dc.mppt_2.voltage` | u16 | 0.1 | V |
| `33052` | `dc.mppt_2.current` | u16 | 0.1 | A |
| `33053` | `dc.mppt_3.voltage` | u16 | 0.1 | V |
| `33054` | `dc.mppt_3.current` | u16 | 0.1 | A |
| `33055` | `dc.mppt_4.voltage` | u16 | 0.1 | V |
| `33056` | `dc.mppt_4.current` | u16 | 0.1 | A |
| `33057` | `dc.power.total` | u32 | 1 | W |
| `33073` | `ac.phase_l1.voltage` | u16 | 0.1 | V |
| `33074` | `ac.phase_l2.voltage` | u16 | 0.1 | V |
| `33075` | `ac.phase_l3.voltage` | u16 | 0.1 | V |
| `33076` | `ac.phase_l1.current` | u16 | 0.1 | A |
| `33077` | `ac.phase_l2.current` | u16 | 0.1 | A |
| `33078` | `ac.phase_l3.current` | u16 | 0.1 | A |
| `33079` | `ac.power.total` | s32 | 1 | W |
| `33093` | `inverter.temperature` | s16 | 0.1 | °C |
| `33094` | `ac.frequency` | u16 | 0.01 | Hz |
| `33133` | `battery.voltage` | u16 | 0.1 | V |
| `33139` | `battery.soc` | u16 | 1 | % |
| `33147` | `load.power` | u16 | 1 | W |
| `33161` | `battery.energy_charged` | u32 | 1 | kWh |
| `33165` | `battery.energy_discharged` | u32 | 1 | kWh |

On signedness the two sources are not symmetrical, and the table above flattens that. A declares
`register_data_type` on most rows; B declares `data_type` on only a few. Where both declare they
agree (33029 u32, 33093 s16). Where B is silent, A's declaration stands alone and B falls back to
its own default — agreement by one statement and a default, not by two statements.

## What is left out, and why

### `grid.power` — one source states the sign, the other does not

Both agree on the pair (33263/33264), on s32, and on raw watts. Source A presents kW via
`scale 0.001` and B presents W via `scale 1`: the same raw value in different clothes, not a
conflict.

What differs is the convention. **B states it in its derived-sensor code** — it negates the pair,
with the comment *"Meter total active power is positive when exporting"*. **A is silent.** One
signed value, no note.

So the convention is single-sourced, and the two-source rule keeps the row out. If B is right,
canonical `grid.power` (positive = importing) wants `scale = -1`. That is a one-line change and
**the most valuable row still missing**, because zero-export control cannot be built without a
grid figure. One bench reading settles it.

### `battery.power` and `battery.current` — the direction lives in another register

`battery.power` (33149/33150) is absent for a structural reason, not a disagreement. **Both**
sources consult the direction register at **33135** to decide the sign: A returns the value on
charge and its absolute value on discharge; B negates when the flag says charging. Neither treats
the 32-bit pair as self-sufficient, and this schema maps one register to one measurement.

`battery.current` (33134) is absent for the same reason, and it is the tempting one: both sources
decode it as `s16`, so it *looks* self-describing. Neither states which direction is positive, and
both ship 33135 alongside it. The sibling RHI map reaches the same conclusion in words worth
repeating — *a confidently inverted battery graph is worse than an absent one*.

`battery.soc` and `battery.voltage` say what they mean without a convention to get wrong.

### Per-phase grid-port power

33512, 33515 and 33518 exist in source B, phase C included. The row stays out because it is
single-sourced — A carries no grid-port per-phase power — and because the **AC grid port is not
the inverter's AC output**, which is what `ac.phase_*` means here. Conflating them would put the
wrong quantity behind a familiar name.

### `battery.temperature`

33043, source B only. One source is research, not a map.

## Writing — none, and not for the usual reason

Every other profile here carries dormant `[[write]]` rows behind a `verified` flag. This one
carries none at all, because of something worse than an unverified address.

**Eniris, who integrate Solis hybrids commercially, state that older Solis firmware implemented
control by writing to FLASH.** Flash has finite write endurance. A control loop on the wrong
firmware wears the inverter out. Newer firmware exposes curtailment registers that avoid it, and
they warn that external control depends on the firmware version.

The 44100–44112 dispatch block in source B is the right target — export limit 44104, power target
44106/44107, failsafe interval 44101, gated on register 34502 reading `0xAA55` — once someone has
a unit in front of them, knows its firmware level, and has confirmed that the function code this
firmware sends (**FC06 only**) is one the inverter accepts.

## A trap in Solis's own documentation

Solis numbers registers inconsistently (`33072` vs `3072`), and the written number is sometimes
one above the address to send — one reader found active power *documented* as 3005–6 while the
address to send is 3004. The numbers in this map are the ones both integrations put on the wire.
Do not "correct" them against a Solis PDF without first checking which convention that PDF uses.

## Bring-up checklist

- [ ] Wire A/B to the 16-pin COM connector. Check the plug seats fully — a neighbouring Solis
      thread lost days to a connector that looked seated and stopped 2.5 mm short.
- [ ] Before connecting a reader: A to 0V should read about **3.3 V**, B about **1.6 V**. The
      port is 3.3 V logic.
- [ ] Select driver `modbus_profile`, profile `solis_s6_eh3p_hybrid`.
- [ ] Set log level `trace`; confirm all seven blocks answer rather than time out.
- [ ] Check each mapped row against the inverter's own display **at that moment**.
- [ ] **The one observation this map is missing:** after dark, switch on a heavy load and read
      33263/33264. Positive or negative decides whether `grid.power` wants `scale = -1`.
- [ ] While you are there, watch **33134 and 33135 together** across a charge→discharge
      crossover. That decides whether battery current and power can ever be mapped.
- [ ] Report back either way — a map that turned out wrong is as useful as one that worked.
