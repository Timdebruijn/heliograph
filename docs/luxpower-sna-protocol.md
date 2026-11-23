# LuxPower SNA hybrid — Modbus RTU register map

Single-phase hybrid with a low-voltage battery, sold as the SNA 3–6K. The profile lives in
[`profiles/luxpower/sna.toml`](../profiles/luxpower/sna.toml).

**STATUS: Experimental. No LuxPower device has answered this map, and nobody on this project
owns one.** Every mapped row is carried by at least two independent sources that agree on
address, width and scale. Whether a given unit answers this firmware *at all* is a separate and
larger question than the map — read the next section first.

Each claim below is labelled: **documented** (the vendor's protocol document says it),
**observed** (another project reports it from a real unit; this project has not seen it), or
**hypothesis** (inferred, and what a tester would settle).

## Does it answer plain Modbus RTU?

These inverters are normally read through a WiFi dongle, and newer dongles ship with a variant
that no longer offers a local protocol. The RS485 bus behind the dongle is what makes the
inverter reachable without it. There are two different things on that bus:

| | The vendor's frame | Plain Modbus RTU |
|---|---|---|
| Frame | address, function, **10-byte serial number**, register, count, CRC | address, function, register, count, CRC |
| Byte order in a 16-bit value | low byte first | high byte first |
| Where | **documented** — it is the only frame the vendor document describes | **observed** by two projects, documented nowhere |
| This firmware | cannot speak it | speaks it |

The vendor document is titled a Modbus RTU protocol and is not one a standard master can use:
the serial number sits inside every request. A bridge that speaks that frame is a codec, not a
register map, and is out of reach of a profile.

What this profile rests on is that the inverter **also** answers standard frames:

- **observed** — an SNA polled with ESPHome's `modbus_controller`, which can send nothing but
  standard Modbus RTU, on the RJ45 port described below (source B). A second user in that
  project's issue tracker reports the same behaviour on a 6000XP.
- **observed** — the same family (18kPV, 12kPV and relatives) polled with pymodbus through an
  RS485 adapter or a Modbus TCP↔RTU gateway, which the project calls its tested setup (source
  A). Different models from this profile's, the same register map.
- **observed, against** — an LXP-3600 ACS whose RJ45 ports answered nothing at all; only the
  dongle's own port did, in the vendor frame
  ([openlux issue 7](https://github.com/f-garofalo/openlux/issues/7)). That project also
  records the SNA-12K as having RS485 on the dongle port only.

So the honest answer is **yes for some models on one port, no for others, and unconfirmed for
the SNA firmware any particular reader owns** (**hypothesis**). If a unit stays silent at the
settings below, no option in this firmware will change that: it needs the vendor frame.

## Connection

| | | |
|---|---|---|
| Port | On the SNA, the RJ45 socket the manual's CT section describes. Pin 7 = B, pin 8 = A — the second RS485 pair, which source B's author calls the bus for third-party connections | **observed** (B, with the manual's pin table) |
| Do not use | Pins 5 and 6 of that socket are the current-transformer input. The manual's table shows a first RS485 pair on pins 1–4; asked about it, source B's author answers "you have to use pin 7 and 8" | **observed** (B) |
| Line | 19200 baud, 8 data bits, no parity, 1 stop bit | **documented**, and used by A and B |
| Unit id | 1. Stored in holding register 15, range 0–150 | **documented** range; 1 is **observed** (A, B) |
| Function codes | 04 for readings, 03 for settings, 06 to write one register | **documented** for the vendor frame; **observed** on plain frames (A all three, B all three) |
| Read size | At most 40 registers, and only whole groups: 0–39, 40–79, 80–119 … A read that crosses a group is not allowed | **documented**; a longer read refused with exception 03 is **observed** on an LXP-3600's dongle port |
| Polling | No faster than once a second | **documented** |
| The dongle | With the adapter polling every 5 s the dongle lost its link to the inverter and regained it when the adapter was unplugged | **observed** (B's issue 3, two users) |

The dongle's own socket is an HDMI-shaped connector with RS485 on pins 14 (B) and 15 (A). It
speaks the vendor frame, so it is of no use to this firmware. It also carries 5 V on pin 18.

## Sources

| | Project | File | Read at |
|---|---|---|---|
| **A** | [joyfulhouse/pylxpweb](https://github.com/joyfulhouse/pylxpweb), the register layer of [joyfulhouse/eg4_web_monitor](https://github.com/joyfulhouse/eg4_web_monitor) | `src/pylxpweb/registers/inverter_input.py`, `inverter_holding.py`; `docs/CONFIGURATION.md` in the integration | `39d6ed4` (2026-11-23) and `91cf5da` (2026-11-13). MIT, active |
| **B** | [nicolaERTT/ESP32-luxpower](https://github.com/nicolaERTT/ESP32-luxpower) | `esp-luxpower.yaml` | `fbece56` (2025-12-22). No licence file; one author, a few dozen rows |
| **C** | [celsworth/lxp-bridge](https://github.com/celsworth/lxp-bridge) | `src/lxp/packet.rs`, `doc/LXP_REGISTERS.txt` | `6d6fcee` (2025-03-20). MIT, quiet since |
| **D** | The vendor protocol document | "EG4 18KPV-12LV Modbus RTU Protocol", read as the text copy in [jaredmauch/eg4-bridge](https://github.com/jaredmauch/eg4-bridge) `doc/`; and the Lux Power "Register Mapping Table" (pages 8–19 of a 41-page document) carried in source B | `2fb7900` (2026-09-29) and `fbece56` |

Neither copy of D is redistributed here. Nothing was copied from any source; register numbers
and scales are facts, and the comments are this project's own.

Independence, checked rather than assumed: A, B and C are different projects by different
authors in three languages. C states that it was reverse-engineered, with no vendor document.
A states that it was cross-checked against D, so A and D are not independent of each other on
paper — what A adds is that it runs against real units over plain Modbus RTU.
[ant0nkr/luxpower-ha-integration](https://github.com/ant0nkr/luxpower-ha-integration)
(`66d1315`, 2026-11-11) is a transcription of D and is counted as D. jaredmauch/eg4-bridge is a
fork of C and is counted as C.

### Why a dongle integration's map is usable here

C and ant0nkr's integration talk to the dongle over TCP, not to the bus. Their requests wrap
the vendor frame, and the register number and count inside it are the same numbers a Modbus
read carries. So the **register map** transfers; the byte order does not. Both decode a 16-bit
value low byte first because the vendor frame does. On plain Modbus RTU the bytes of a word
arrive in the standard order, and A and B decode them that way without a swap.

The **word** order is the same in both worlds: a 32-bit value has its low word at the lower
address. D names the pairs `L` then `H`, A declares it, C parses a little-endian `u32`. Every
32-bit row in the profile is therefore `word_order = "low_first"`.

## Mapped registers

Input registers unless marked. `value = raw × scale`.

| Register | Type | Scale | Canonical id | Sources | Notes |
|---|---|---|---|---|---|
| 0 | u16 | | device status | B, C, ant0nkr | thirteen codes, each named alike by two tables |
| 1 | u16 | 0.1 | `dc.mppt_1.voltage` | A B C D | |
| 2 | u16 | 0.1 | `dc.mppt_2.voltage` | A B C D | |
| 4 | u16 | 0.1 | `battery.voltage` | A B C D | |
| 7 | u16 | 1 | `dc.mppt_1.power` | A B C D | |
| 8 | u16 | 1 | `dc.mppt_2.power` | A B C D | |
| 10 | u16 | 1 | `battery.charge_power` | A B C D | power into the battery |
| 11 | u16 | 1 | `battery.discharge_power` | A B C D | power out of the battery |
| 12 | u16 | 0.1 | `ac.phase_l1.voltage` | A B C D | grid voltage |
| 15 | u16 | 0.01 | `ac.frequency` | A B C D | |
| 26 | u16 | 1 | `grid.export_power` | A C D | |
| 27 | u16 | 1 | `grid.import_power` | A B C D | |
| 50 | u32, low first | 0.1 | `battery.energy_charged` | A C D | kWh, lifetime |
| 52 | u32, low first | 0.1 | `battery.energy_discharged` | A C D | kWh, lifetime |
| 64 | **s16** | 1 | `inverter.temperature` | A C D | signedness from A |
| 67 | **s16** | 1 | `battery.temperature` | A B C D | signedness from B |
| holding 60 | u16 | 1 | `control.active_power_limit` | C D, A's dumps | % — see [Control](#control) |

`dc.power.total` is not a row: with both strings mapped, the driver publishes their sum and
marks it derived.

## What is not mapped, and why

### State of charge — agreed by everyone, and not expressible

Input register **5** holds state of charge in its **low byte** and state of health in its
**high byte** (A, B, C and D all say so). A profile row reads a whole word or a single bit, so
there is no way to say "the low byte". Mapped as a word, a healthy battery at 80 % reads 25680.

This is the gap that matters most: the dashboard's battery card is driven by `battery.soc`, so
a unit on this profile shows battery voltage, temperature and both power rails, and no charge
level. It is a limit of the profile schema and needs a byte selector on a read row. It is not
in this change.

### AC power — two rails

**16** is the inverter's output power and **17** the power it draws to charge from the grid,
both unsigned. `ac.power.total` is one signed channel. Mapping 16 alone would publish 0 W while
the inverter pulls kilowatts into the battery.

### Generation — per string only

Today's energy sits at 28, 29 and 30 and the lifetime totals at 40–45, one per string. The
whole-array figure is a sum, which a profile cannot compute. 31 and 46 are the grid-side
inverter output, which is not what the array produced on a unit that feeds a backup output.

### Battery current — the scale is disputed

Input **98**, signed in every source and with the direction stated by none. B, C and D scale
it at 0.01 A; A scales it at 0.1 A, and says of the neighbouring BMS current limits that 0.1 A
was found by experiment against the document's 0.01 A. A factor of ten and an unknown sign:
left out, and read in the third block so a tester's dump shows it.

### PV current — a conflict

A places the string currents at 72–74. The Lux Power copy of D places the grid self-test
registers at 72–75. Neither is mapped.

### House load

D lists a load register at input **170**, valid "when working in on-grid mode". The LXP-3600
report has it reading 0 in one test and refused with exception 03 in a later one. **24** is the
backup output, a different quantity.

### A third string

Registers 3 and 9 are the third string on the larger models. The profile is scoped to two
trackers. D's note on 9 reads "PV3 power (total PV power, obtained by adding PV1, 2, 3)", which
does not say whether 9 is the third string or the total.

### Status codes left out

17 is "bypass" in one table and "standby" in another; 128 is "PV off-grid" in one and "PV power
unstable" in another; 25 and 96 appear in one table each. They are published as `Unknown (n)`.

## Control

What users of these inverters ask for is AC charging on and off, and the charge and discharge
rates. The registers are known. All but one cannot be declared today.

| Holding | Vendor name | Unit, range | Sources | Why there is no `[[write]]` row |
|---|---|---|---|---|
| 21, bit 7 | `FuncEn.ACChargeEn` | flag | A C D | One bit of a word that also holds anti-islanding, the standby switch and feed-in enable. The write path writes whole registers and does not read-modify-write |
| 64 | `ChargePowerPercentCMD` | %, 0–100 | A C D | The charge-limit command is in watts and has no read-back measurement |
| 65 | `DischgPowerPercentCMD` | %, 0–100 | A C D | As 64 |
| 66 | `ACChgPowerCMD` | **disputed**: %, 0–100 in D; hundreds of watts in A | A C D | The sources disagree on the unit |
| 67 | `ACChgSOCLimit` | %, 0–100 | A C D | Stops AC charging only; no command means that, and no read-back |
| 103 | `MaxBackFlow` | **disputed**: %, 0–100 in D and C; a power in A | A C D | The export-limit command is in watts, has no read-back, and the sources disagree on the unit |
| 105 | `EOD`, discharge cut-off | %, 10–90 | A B C D | `set_minimum_soc` has no read-back measurement. B writes this register over RS485 |
| 125 | off-grid discharge cut-off | % | A B C | As 105 |
| **60** | `ActivePowerPercentCMD` | %, 0–100 | C D, A's dumps | **Declared** — see below |

A row is only served when the profile reads its setpoint back, and the read-back vocabulary
has one entry today, for the active-power limit
([write-path.md](device-profiles/write-path.md#a-read-back-is-required)). Extending it is its
own change.

### The one declared row

Holding **60** limits the inverter's active power as a percentage. It is declared as
`set_active_power_limit_percent`, 0–100, `verified = false`, and read back as
`control.active_power_limit`. It is a single 16-bit register written with function 06, which
is what A uses for every setting and what B uses for 105 and 125.

Three things are not known, and all three are why it stays unverified:

- **Whether 60 is obeyed on its own** (**hypothesis**). D lists a separate enable,
  `uFunctionEn2.ActPowerCMDEn`, bit 6 of holding 179, and does not say how the two relate.
- **What it limits on an SNA.** A unit feeding its backup output is not exporting; whether the
  limit applies to that output, to grid feed-in, or to both is undocumented.
- **One source disagrees about the register.** A's holding table labels 60 as a reactive-power
  mode with a range of 0–4. D, C and the vendor-portal parameter dumps kept in A's own
  repository (an SNA 12K and two LXP-EU 12K among them) all name it the active-power percentage,
  reading 100. The table looks like the outlier. It is still a reason to read 60 before
  believing it: a unit at factory settings should show **100**, not a number from 0 to 4.

Nothing reaches the inverter unless the control switches are opened and the device opts in to
unverified rows. Until someone has done that on a bench, this row is research.

## What a tester needs to send back

An issue with the following is worth more than any amount of further reading. A report that
nothing answered is as useful as one that everything did.

- [ ] Model, and the firmware code from the inverter's display.
- [ ] Which socket and which pins, and whether the WiFi dongle was plugged in.
- [ ] Driver `modbus_profile`, profile `luxpower_sna`, log level `trace`. **Did any block
      answer?** If all six time out, say so — that settles the frame question for that model.
- [ ] The raw TRACE dump of one full poll, all six blocks.
- [ ] Each mapped reading next to the inverter's own display **at the same moment**.
- [ ] Input **5** raw, with the state of charge and state of health the display shows.
- [ ] Input **98** raw while charging and again while discharging, with the display's battery
      current both times. That settles the scale and the sign together.
- [ ] Input **9** on a two-string unit: zero, or the sum of 7 and 8?
- [ ] Whether the dongle keeps reporting to the vendor portal while the bridge polls.
- [ ] Holding **60** and **179** as read, before anything is written.

Only for someone willing to test the limit, with the inverter in view: write 50 to the
active-power limit, confirm 60 reads back 50, watch whether the output follows, and write 100
again. Report all three observations, including "it read back and nothing changed".
