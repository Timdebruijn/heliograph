# Growatt MIC TL-X — Modbus RTU register map

The MIC TL-X is Growatt's small single-phase string inverter: one MPPT tracker, one phase,
600–3300 W depending on the model. It speaks Growatt's Modbus RTU **Protocol II** over its
RS485 port.

The profile lives in `profiles/growatt/mic_tl_x.toml`. Everything below explains how that
file was arrived at and what still needs proving on hardware.

## One profile for the whole range

A profile describes a **register layout**, not a model number. A MIC 600TL-X and a MIC
3300TL-X differ only in power rating, and the rating appears nowhere in the register map.
One `mic_tl_x` profile therefore covers the whole range, exactly as `sph` covers the whole
SPH 3–6 kW range.

Holding register **44** reports the actual tracker and phase count of the connected unit, so
a variant that ever deviates from "1 tracker, 1 phase" is visible in the raw dump rather than
being silently mis-decoded.

## The generation question, settled without hardware

`docs/growatt-sph-protocol.md` flags a genuine trap: Protocol II describes two register
generations, and reading the wrong one gets you nothing (or worse, plausible nonsense). For
the SPH that question is still open and can only be closed on the bench.

For the MIC TL-X it is closed already. The 3000-series input registers belong to the
**TL-XH**, the hybrid variant with a battery. The plain **TL-X** string inverter lives in the
"first group" at input registers **0–124**. Two independent sources keep the two maps
strictly apart and agree on which is which:

- The machine-readable Protocol II register CSV
  ([0xAHA/Growatt_ModbusTCP](https://github.com/0xAHA/Growatt_ModbusTCP/tree/main/Protocols),
  `growatt_modbus_rtu_v139_registers.csv`) defines the "First group" input registers 0–124
  with exactly this layout.
- [WouterTuinstra/Homeassistant-Growatt-Local-Modbus](https://github.com/WouterTuinstra/Homeassistant-Growatt-Local-Modbus),
  a widely deployed and therefore heavily hardware-exercised Home Assistant integration,
  defines `INPUT_REGISTERS_120` (0–124) and `INPUT_REGISTERS_120_TL_XH` (3000+) as two
  separate tables in
  `custom_components/growatt_local/API/device_type/inverter_120.py`.

That two sources of different kinds — a vendor spec and a working implementation — agree
register for register is the strongest evidence available short of the inverter itself. It is
still not the inverter itself, which is why the driver stays Experimental until someone
confirms it.

## Read map (input registers, function 04)

The default scale in Protocol II is ÷10. Two rows deviate and are the easiest to get wrong:

| Reg | Meaning | Type | Scale | Published as |
|---|---|---|---|---|
| 0 | Inverter status | u16 | — | (raw dump only) |
| 1–2 | Total PV power | u32 | ÷10 W | `dc.power.total` |
| 3 | PV1 voltage | u16 | ÷10 V | `dc.mppt_1.voltage` |
| 4 | PV1 current | u16 | ÷10 A | `dc.mppt_1.current` |
| 5–6 | PV1 power | u32 | ÷10 W | `dc.mppt_1.power` |
| 7–10 | PV2 | — | — | not mapped (single-tracker hardware) |
| 35–36 | AC output power | u32 | ÷10 W | `ac.power.total` |
| 37 | Grid frequency | u16 | **÷100** Hz | `ac.frequency` |
| 38 | Grid voltage L1 | u16 | ÷10 V | `ac.phase_l1.voltage` |
| 39 | Grid current L1 | u16 | ÷10 A | `ac.phase_l1.current` |
| 40–41 | Pac1 | u32 | ÷10 | not mapped (see below) |
| 53–54 | Energy today | u32 | ÷10 kWh | `energy.today` |
| 55–56 | Energy total | u32 | ÷10 kWh | `energy.total` |
| 57–58 | Work time total | u32 | **÷7200** → h | `inverter.operating_hours` |
| 93 | Inverter temperature | s16 | ÷10 °C | `inverter.temperature` |
| 94 / 95 | IPM / boost temperature | u16 | ÷10 °C | not mapped |
| 98 / 99 | P-bus / N-bus voltage | u16 | ÷10 V | not mapped |
| 101 | Output percentage | u16 | — | not mapped |
| 104 / 105 | Derating mode / fault code | u16 | — | not mapped |

Deliberate omissions, all for the same reason — a channel that might be wrong is worse than a
channel that is absent:

- **PV2 (7–10)** exists in the layout but not on this hardware. Mapping it would publish a
  permanent zero, and this project does not publish unknowns as zero.
- **Pac1 (40–41)** duplicates `ac.power.total` on a single-phase inverter, and protocol
  revisions disagree on whether it is real power (W) or apparent power (VA). A second power
  entity that might silently be VA is a trap.
- **IPM and boost temperature** are real, but there is one canonical
  `inverter.temperature` and the inverter temperature is the useful one.

Everything omitted still appears in the driver's raw TRACE dump, so any of it can be promoted
later from bench evidence rather than from a forum post.

Register 93 is declared **signed** although the protocol table calls it unsigned. For every
physically possible reading the two decode identically, and signed additionally survives a
sub-zero winter morning on an outdoor unit instead of reporting 6553 °C.

## Identity (holding registers, function 03)

Read as a block during bring-up so the raw dump can be checked against the sticker on the
unit:

| Reg | Meaning |
|---|---|
| 3 | Output power limit, % — the writable setpoint below |
| 9–14 | Firmware version (string) |
| 23–27 | Serial number (string) |
| 28–29 | Model code (packed nibbles) |
| 43 | Device type code |
| 44 | Number of trackers and phases |
| 88 | Modbus protocol version |

## Curtailment: holding register 3

**Holding register 3 is the inverter's active power limit, 0–100 %, writable with function
code 06.**

This matters well beyond the MIC. It is the safest write this project has found on any
device so far:

- a single 16-bit holding register, one word, no multi-register transaction;
- trivially reversed by writing 100;
- it touches no grid-protection or safety setting, unlike the SPH's battery and time-slot
  registers which sit next to exactly that;
- it needs no DRM wiring and no relay board, so it works on the plain RS485-CAN board.

It is declared in the profile as a `[[write]]` row against the canonical
`set_active_power_limit_percent` command — and it is **dormant**. Two independent gates stand
between that row and a byte on the bus:

1. `verified = false` in the profile. Only a bench session flips this.
2. The driver has no write path at all yet; `execute()` returns `Unsupported`, and the
   descriptor declares `supportsWrite = false`.

Both are asserted in `test/test_growatt_driver/test_main.cpp`, so neither can be dropped by
accident.

One note for whoever does the bench session: some protocol revisions describe **255** as
"limit disabled". The declared bound stops at 100 rather than allowing 255, because a
percentage value that silently means "off" is a footgun. If the bench confirms 255 is needed,
it gets its own explicit handling rather than a widened range.

## Bring-up checklist

1. **Give each inverter a unique Modbus address.** All units ship as address 1, so two on one
   bus collide. Set this via ShineBus (or the manufacturer's app) before wiring anything to a
   shared bus.
2. Wire A/B/GND to the RS485 port. 9600 8N1 is what the profile declares.
3. Configure the driver: `growatt_modbus`, profile `mic_tl_x`, `unit_id` as set in step 1.
4. Set log level to `trace` and read `/api/v1/logs`. The `GROWATT in <addr>: ...` lines are
   the raw block dump.
5. Check the dump against the inverter's own app: PV voltage, AC power, today's energy,
   total energy. All four should match without any arithmetic on your part.
6. Confirm the two odd scales specifically — frequency should read ~50.0 Hz (not 5.0 or
   500.0), and operating hours should be plausible for the age of the unit.
7. Report the result on the issue tracker either way. A confirmation promotes this driver out
   of Experimental; a mismatch corrects one TOML row and helps the next person more.

## Sources

- Growatt Inverter Modbus RTU Protocol II, machine-readable register CSV —
  <https://github.com/0xAHA/Growatt_ModbusTCP/tree/main/Protocols>
- WouterTuinstra/Homeassistant-Growatt-Local-Modbus, `inverter_120.py` —
  <https://github.com/WouterTuinstra/Homeassistant-Growatt-Local-Modbus>
- Growatt MIC 750–3300TL-X datasheet (single phase, one MPP tracker) —
  <https://www.growatt.tech/wp-content/uploads/shared-files/MIC-7503300TL-X-Datasheet.pdf>
- The SPH map and the register-generation split it still has to resolve —
  [docs/growatt-sph-protocol.md](growatt-sph-protocol.md)
