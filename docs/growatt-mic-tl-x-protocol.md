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

## The generation question is still open

`docs/growatt-sph-protocol.md` flags a genuine trap: Protocol II describes more than one
register generation, and reading the wrong one gets you nothing — or worse, plausible
nonsense. An earlier draft of this document claimed the question was closed for the MIC
without hardware. It is not, and the claim did not survive review. Here is what is actually
known.

**The map below follows the hardware-exercised source.**
[WouterTuinstra/Homeassistant-Growatt-Local-Modbus](https://github.com/WouterTuinstra/Homeassistant-Growatt-Local-Modbus)
is a widely deployed Home Assistant integration, which means its tables have been run against
a great many real inverters. It defines `INPUT_REGISTERS_120` (input 0–124) for a plain
Protocol II inverter and a separate `INPUT_REGISTERS_120_TL_XH` (3000+) for the hybrid, in
`custom_components/growatt_local/API/device_type/inverter_120.py`. A MIC TL-X is a plain
string inverter, so this profile uses the first table. Every mapped row matches it exactly.

**The vendor CSV corroborates far less than it appears to.** The machine-readable register CSV
([0xAHA/Growatt_ModbusTCP](https://github.com/0xAHA/Growatt_ModbusTCP/tree/main/Protocols))
agrees on registers 0–10 and then diverges: its "First group" puts AC power at 11, frequency
at 13 and temperature at 34, which is an older generation than the one used here. The file
also describes itself as "a representative sample" rather than the complete protocol. It is
supporting evidence for the PV portion of the map and no more.

**The 3000-series is not hybrid-only.** That same CSV marks the whole 3000 range — holding and
input — with the group label `Use for TL-X and TL-XH`. So the vendor considers those registers
applicable to the plain TL-X as well. Splitting them off into a hybrid-only table is the HA
integration's implementation choice, not a statement by Growatt.

**Therefore the profile probes both.** Alongside the mapped 0–124 block it reads 40 registers
at 3000, mapping nothing from them. The only purpose is that the first bring-up dump answers
the question directly: does this unit populate 0–124, 3000+, or both? A block the device
refuses is skipped harmlessly, and the block is declared `probe = true`, which keeps its read
failures out of the RS485 bus counters — so probing a range that does not exist costs nothing
at all. That flag is doing real work: a Modbus device *should* answer an unknown range with an
exception, but it is free to stay silent instead, and an unflagged probe block would then add a
timeout to the metrics on every poll of a perfectly healthy inverter. This is the same tactic
`sph.toml` uses for its own open generation question.

The driver stays **Experimental** until someone confirms the map against a physical unit.

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
- **Pac1 (40–41)** duplicates `ac.power.total` on a single-phase inverter, and neither source
  says whether it is real power (W) or apparent power (VA) — both just call it "output
  power". A second power entity that might silently be VA is a trap. On the bench it should
  track registers 35–36 closely; a persistent offset means VA.
- **IPM and boost temperature** are real, but there is one canonical
  `inverter.temperature` and the inverter temperature is the useful one.

Everything omitted still appears in the driver's raw TRACE dump, so any of it can be promoted
later from bench evidence rather than from a forum post.

Register 93 is declared **signed**, which is a choice rather than a transcription — neither
source states this register's signedness. For every physically possible reading the two decode
identically, and signed additionally survives a sub-zero winter morning on an outdoor unit
instead of reporting 6553 °C. If the device turns out to be unsigned, nothing breaks.

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

## Giving each inverter an address

Every MIC TL-X ships as Modbus address **1**. Two of them on one bus will both answer every
request and their replies collide into garbage, so the addresses have to be set before the
units are chained together. 1, 2, 3 in the order they hang on the wire is as good a scheme as
any.

There are two ways to do it. Neither needs this bridge.

**On the inverter's touch control.** The MIC has a touch-sensitive display with a `Set
Comaddr` menu. The gestures are unusual enough to be worth writing down:

| Gesture | Effect |
|---|---|
| Single touch | Next value / next item |
| Double touch | Enter or confirm |
| Triple touch | Back to the previous menu |
| Hold ~5 s | Confirm the setting (or restore defaults) |

**Or over Modbus itself.** Holding register **30** is `Com Address`, writable, range 1–254,
factory default 1 (Growatt Protocol II). Connect one inverter at a time — while they are all
still on address 1 you cannot address them individually — write the new address, then move to
the next unit and chain them once each has its own.

Heliograph has no write path, so this route needs a generic Modbus tool for the one-time
write (`mbpoll` or similar). `tools/read_modbus.py` in this repo reads only.

Whichever route you take, verify before chaining: with a single inverter on the bus, poll each
address in turn and confirm exactly one answers, at the address you expect.

## Bring-up checklist

1. **Give each inverter a unique Modbus address**, as above, before wiring anything to a
   shared bus.
2. Wire A/B/GND to the RS485 port — see [rs485-bus.md](rs485-bus.md) for topology, ground and
   the 120 Ω termination rule. 9600 8N1 is what the profile declares — if your units are set to
   something else, run **extended** discovery (quick only tries the first profile) and the
   wizard stores the line settings that answered.
3. Configure the driver: `growatt_modbus`, register map `mic_tl_x`, `unit_id` as set in step 1.
   Both are fields in the discovery wizard's confirm step, and in *Settings → Driver*. The
   wizard offers back whatever is already stored, so re-running it does not undo a working
   setup — but on a fresh bridge the map starts unset and the step cannot be completed until
   you pick one. That is deliberate: getting the map wrong is the one mistake here that does
   not announce itself. See the warning below.
4. Set log level to `trace` and read `/api/v1/logs`. The `GROWATT in <addr>: ...` lines are
   the raw block dump.
5. Check the dump against the inverter's own app: PV voltage, AC power, today's energy,
   total energy. All four should match without any arithmetic on your part.

> **The register map is a choice, not a detection.** Probing identifies the *protocol* — a valid
> Modbus reply proves the device speaks Modbus at that address and nothing more. It cannot tell
> a MIC TL-X from an SPH.
>
> On the wrong map, **not one published value is correct.** The two maps overlap only in which
> registers are *fetched* (both read the input block 0–124); they share no mapped address at
> all. This map reads AC power from register 35 and DC power from 1; the SPH map reads them
> from 40 and 116, as a different width and sign, and then looks for battery data in a 1000-block
> a MIC does not implement. So a wrong map yields numbers that are the right shape and the right
> order of magnitude and are all wrong — which is exactly what makes it dangerous.
>
> That also means step 5's test poll cannot confirm the map: it shows an AC power figure, and a
> wrong map produces a believable one. The check that works is step 4 below — the raw block dump
> against the inverter's own app.
6. Confirm the two odd scales specifically — frequency should read ~50.0 Hz (not 5.0 or
   500.0), and operating hours should be plausible for the age of the unit.
7. **Settle the generation question.** Look at what the 3000-block dump did. Three outcomes,
   each actionable: the device refused it (only 0–124 exists — drop the probe block), it
   answered with real values too (both exist — drop the probe block, the map is right
   either way), or 0–124 was refused and 3000 answered (the map needs moving, and this
   document needs correcting).
8. Report the result on the issue tracker either way. A confirmation promotes this driver out
   of Experimental; a mismatch corrects one TOML row and helps the next person more.

## Sources

- Growatt Inverter Modbus RTU Protocol II, machine-readable register CSV —
  <https://github.com/0xAHA/Growatt_ModbusTCP/tree/main/Protocols>
- WouterTuinstra/Homeassistant-Growatt-Local-Modbus, `inverter_120.py` —
  <https://github.com/WouterTuinstra/Homeassistant-Growatt-Local-Modbus>
- Growatt MIC 750–3300TL-X datasheet (single phase, one MPP tracker) —
  <https://www.growatt.tech/wp-content/uploads/shared-files/MIC-7503300TL-X-Datasheet.pdf>
- Eniris device documentation for the Growatt MIC series, for the `Set Comaddr` touch-control
  procedure — <https://docs.eniris.be/Devices/PV-hybrid-and-battery-inverters/Growatt/MIC%20Series/>
- The SPH map and the register-generation split it still has to resolve —
  [docs/growatt-sph-protocol.md](growatt-sph-protocol.md)
