# Deye / Sunsynk SUN-xK-SG — Modbus RTU register map

The Deye SUN-xK-SG is a single-phase hybrid inverter with a battery port; Sunsynk sells the same
hardware under its own badge, and every implementation treats them as one register map. The
profile lives in [`profiles/deye/sun_xk_sg.toml`](../profiles/deye/sun_xk_sg.toml).

**STATUS: Experimental.** Every mapped row is corroborated by two independent implementations
that agree. Nothing here has been confirmed against a physical inverter by this project.

## Connection

| | |
|---|---|
| Port | RS485 on the inverter's own terminal block (marked BMS/RS485/Modbus depending on revision) |
| Line | 9600 8N1 |
| Unit id | 1 by default |
| Register space | **Holding registers (function 03)** for everything — there are no input registers in this map |
| Writes | **function 16** (write multiple), even for a single register — see [Writing](#writing) |

## Sources

Two projects, read directly rather than summarised. Both are maintained, both have real users on
real hardware, and they were checked against each other row by row.

| | Project | File |
|---|---|---|
| **A** | [kellerza/sunsynk](https://github.com/kellerza/sunsynk) | `src/sunsynk/definitions/single_phase.py` |
| **B** | [kbialek/deye-inverter-mqtt](https://github.com/kbialek/deye-inverter-mqtt) | `src/deye_sensors_deye_hybrid.py` |

Two traps worth recording, because both would have quietly corrupted this map:

**Source A's factor sign is not a negation.** `Sensor(190, "Battery power", WATT, -1)` does not
mean "negate this". `reg_to_value()` calls `unpack_value(signed=self.factor < 0)` and then
multiplies by `abs(self.factor)` — a negative factor declares the register **signed**. Reading it
as a negation would invert every signed row in the file.

**A third source that is not one.** `StephanJoubert/home_assistant_solarman`'s
`inverter_definitions/deye_hybrid.yaml` is byte-identical to source B's generated
`ha_definitions/deye_hybrid_ha.yaml` (same MD5). It is source B in another format. Counting it
would have turned one implementation into two and defeated the whole point of cross-checking.

## The double-assignment in source A

Source A's `single_phase.py` claims four registers twice. Its AUX/generator-port block takes
181–186 for generator voltages and currents, while its battery and PV blocks take:

| Register | Battery/PV block says | AUX block also says |
|---|---|---|
| 182 | Battery temperature | Generator L2 voltage |
| 183 | Battery voltage | Generator L3 voltage |
| 184 | Battery SOC | Generator L1 current |
| 186 | PV1 power | Generator L3 current |

Source B has no AUX block and assigns exactly the battery/PV meanings. The tie is therefore broken
by an independent implementation, and it breaks against the AUX reading — which looks like a
three-phase map pasted into the single-phase file. **The profile follows battery/PV.**

If a bring-up dump shows a plausible generator voltage at 182 instead of a battery temperature,
this table is where to start.

## Mapped registers

All holding. `value = raw × scale + offset`.

| Register | Type | Scale | Offset | Canonical id | Notes |
|---|---|---|---|---|---|
| 96–97 | u32 | 0.1 | | `energy.total` | lifetime production, kWh |
| 108 | u16 | 0.1 | | `energy.today` | kWh |
| 109 | u16 | 0.1 | | `dc.mppt_1.voltage` | |
| 110 | u16 | 0.1 | | `dc.mppt_1.current` | |
| 111 | u16 | 0.1 | | `dc.mppt_2.voltage` | |
| 112 | u16 | 0.1 | | `dc.mppt_2.current` | |
| 150 | u16 | 0.1 | | `ac.phase_l1.voltage` | grid voltage |
| 164 | s16 | 0.01 | | `ac.phase_l1.current` | |
| 175 | s16 | 1 | | `ac.power.total` | inverter output, **not** grid flow |
| 182 | u16 | 0.1 | **−100** | `battery.temperature` | biased register — see below |
| 183 | u16 | 0.01 | | `battery.voltage` | |
| 184 | u16 | 1 | | `battery.soc` | % |
| 186 | u16 | 1 | | `dc.mppt_1.power` | |
| 187 | u16 | 1 | | `dc.mppt_2.power` | |

### The biased temperature

Register 182 never goes negative on the wire: it reads **1000 for 0 °C**. Both sources apply the
same correction — B as an explicit `offset = -100.0`, A through its `TempSensor` class, whose
decode is `(raw × factor) − 100`. Two independent implementations, one identical arithmetic.

This is the register that made `offset` part of the profile schema. Without it the only options
were publishing 100 °C or publishing no temperature, and temperature is the channel that explains
a derating inverter.

## Open questions — settle these on the first bench session

The profile reads three wide blocks so the raw TRACE dump answers all of these in one visit. Set
log level to `trace` and read `/api/v1/logs`; the `MODBUS unit <n> hold <addr>: ...` lines are the
dump.

**1. Which way does battery power point?** Registers 190 (power) and 191 (current) are agreed by
both sources as signed, at scale 1 and 0.01. Neither says which direction positive means. Our
convention is **positive = charging**. Watch 190 while the battery charges: if it reads negative,
the row needs `scale = -1` (and 191 `scale = -0.01`). Until then both are unmapped — a confidently
inverted battery graph is worse than an absent one.

**2. Is 192 the grid frequency or the load frequency?** Source A maps grid frequency to register
79 and calls 192 the load frequency; source B has no register 79 at all and publishes 192 as the
AC frequency. On a hybrid that can run off-grid these are different quantities. Read 79 and 192
together while grid-connected — if 79 answers at all and both read ~50 Hz, run the inverter
off-grid briefly and see which one follows the load.

**3. Which of 90 and 91 is the inverter temperature?** Both sources agree they are temperatures
with the same `0.1 / −100` decoding. They disagree about which is which: A says 90 is the DC
transformer and 91 the radiator; B calls 90 "DC Temperature" while naming its own topic
`radiator_temp`, and calls 91 "AC Temperature". There is one canonical `inverter.temperature`.
Load the inverter hard and watch which climbs first and furthest — that is the heatsink.

## Writing

**Both sources write with function 16**, even for a single register. Source A says so outright
("Sunsynk support function code 0x10"); source B's frame builder emits `01 10` unconditionally.
Neither ever sends FC06.

This firmware's write path is FC06-only ([write-path.md](device-profiles/write-path.md)), so the
one declared setpoint is dormant for two independent reasons: `verified = false`, and
`function = "write_multiple"` which the driver refuses outright. Making it live needs FC16 support
in the Modbus client — not a flipped flag.

| Register | What | Values | Source |
|---|---|---|---|
| 244 | Load Handling | 0 Allow Export, 1 Essentials Only, 2 Zero Export | **A only** |

That row rests on a single source, which is recorded in the profile rather than smoothed over.

### What cannot be a setpoint here

**Bit fields.** "Priority Load" (243), "Solar Export" (247) and "Use Timer" (248) are single bits
inside shared registers. Both FC06 and FC16 write whole registers, so setting one bit needs
read-modify-write; writing the whole register would clear its neighbours. The profile generator
rejects a `bitmask` key by name for this reason.

**Max Sell power (245)**, the export limit — the setpoint most people actually want. Source A
bounds it by the inverter's rated power, read at runtime from registers 16–17, so the maximum
differs per model across a 3–12 kW family. A `[[write]]` row needs a static maximum, and inventing
one would refuse legitimate setpoints on a large unit or accept impossible ones on a small one.
It needs a per-model profile or a schema that can bound a setpoint by another register.

## Bring-up checklist

- [ ] Wire A/B to the inverter's RS485 terminal; unplug any vendor dongle sharing that port.
- [ ] Select driver `modbus_profile`, profile `deye_sun_xk_sg`, `unit_id` 1.
- [ ] Set log level `trace`; confirm the three blocks answer rather than time out.
- [ ] Check each mapped row against the inverter's own display **at that moment**.
- [ ] Answer the three open questions above and record what you saw.
- [ ] Report back on the issue tracker either way — a map that turned out wrong is as useful to
      the next person as one that worked.
