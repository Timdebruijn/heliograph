# SolarMax MaxTalk — protocol notes and a driver plan, 2026-08-09

**Nothing here is built.** No driver, no codec, no profile. This records what the MaxTalk
protocol is, how it would fit this firmware, and — more usefully — **which parts are agreed by two
independent sources and which rest on one**.

**Nobody here owns a SolarMax.** Everything below is desk research. Two agreeing sources are not a
device agreeing, which is the rule this project already applies to every Modbus register map, and
it applies here with more force because there is no inverter on any desk to check against.

## Why this brand

Sputnik Engineering, the Swiss maker of SolarMax, went bankrupt, leaving owners and service
providers without support. In 2008 it had been the fifth largest inverter supplier in the world.
The hardware kept working; the company behind the monitoring did not.

That is the case this firmware exists for, and it is the same shape as its only Stable driver —
an abandoned family with a serial port and no vendor left to ask.

## The protocol

ASCII over RS485. **19200 baud, 8 data bits, no parity, 1 stop bit.**

```
request:  {FB;05;36|64:CAC;KHR;KDY;KMT;KYR;KT0;KLD;KLM;KLY|0D34}
reply:    {05;FB;59|64:CAC=1F3E;KHR=26D6;KDY=8E;KMT=8D;KYR=221;KT0=18C7;...|154C}
```

The shape is `{source;destination;length|64:payload|checksum}`:

| Field | |
|---|---|
| source | two hex digits. The querying computer is `FB` |
| destination | two hex digits. The inverter's address, set in its own display menu |
| length | two hex digits, the whole frame's length |
| `\|64:` | a literal that appears in every frame from both sources. **Its meaning is not established** |
| payload | query codes separated by `;`. In a reply each code carries `=` and a hex value |
| checksum | four hex digits: the ASCII values of every preceding character, summed |

Framing is **brace-delimited**, which is worth saying plainly because it removes an entire class of
problem: a reader can find a frame boundary by looking for `}`. There is no inter-character timing
rule to get wrong, no length field that has to be trusted before the frame is complete, and no
silent-interval heuristic. Compare the Modbus RTU path, where frame boundaries are found by timing.

### The one detail most likely to be got wrong

The length field is not the payload length. One implementation computes it as
`9 + len("|64:...|") + 5` — that is, the payload span plus the fixed header and trailer. A frame
with the wrong length is ignored rather than rejected, so this fails silently and looks like a
device that is not answering.

### What two sources agree on, and what one says

**Agreed by both** the 2009 reverse-engineering writeup and a working implementation: the frame
shape, the `FB` computer address, 19200 8N1, the checksum being a plain sum of ASCII values
rendered as four hex digits, and a shared vocabulary of query codes.

**One source only:** every scaling factor below. They come from one implementation and have not
been cross-checked. **This is the part that must not be trusted before hardware confirms it.**

## Codes, and how they map onto the canonical model

| Code | Meaning | Scale | Canonical id |
|---|---|---|---|
| `PAC` | AC power | **÷2** | `ac.power.total` |
| `PDC` | DC power | **÷2** | `dc.power.total` |
| `UL1` `UL2` `UL3` | AC voltage per phase | ÷10 | `ac.phase_lN.voltage` |
| `IL1` `IL2` `IL3` | AC current per phase | ÷100 | `ac.phase_lN.current` |
| `TNF` | grid frequency | ÷100 | `ac.frequency` |
| `TKK` | inverter temperature | raw | `inverter.temperature` |
| `UDC` | DC voltage | raw | `dc.mppt_1.voltage` |
| `IDC` | DC current | ÷100 | `dc.mppt_1.current` |
| `UD01`–`UD03` | per-string voltage | ÷10 | `dc.mppt_1..3.voltage` |
| `ID01`–`ID03` | per-string current | ÷100 | `dc.mppt_1..3.current` |
| `KDY` | energy today | ÷10 | `energy.today` |
| `KT0` | energy total | raw or ÷10 | `energy.total` |
| `KMT` `KLM` `KYR` `KLY` `KHR` | month / last month / year / last year / hours | mixed | none — no canonical id |
| `SYS` | system status | code lookup | `statusCode` / `statusText` |
| `SAL` | system alarms | code lookup | `errorCode` |
| `TYP` `SWV` `ADR` | type, firmware, address | — | `DeviceIdentity` |

**`PAC` divided by two is the entry to check first.** A power reading wrong by a factor of two is
plausible in both directions — it will not look absurd on a dashboard, and it will quietly corrupt
every energy total derived from it. This is the same hazard the profile schema records for word
order: wrong in one direction is obvious, wrong in the other is invisible.

**Three strings, and this firmware publishes five.** The per-string codes stop at `UD03`/`ID03`, so
a SolarMax maps comfortably inside the existing tracker vocabulary with room left over.

**Five energy codes have nowhere to go.** Month, last month, year, last year and operating hours
are real readings with no canonical id. That is a vocabulary question, not a driver question, and
it should be answered deliberately rather than by inventing ids in a driver.

## Two things that make this easier than the driver we already have

**There is no registration handshake.** The inverter's address is set in its own menu; a querying
device simply addresses it. The AA55 family this firmware already supports does the opposite — a
device has to be *assigned* a bus address before it answers, which is why discovery for that family
cannot be read-only and why a sunrise-recovery bug was expensive to find. A MaxTalk probe sends a
question and receives an answer, changing nothing.

**That makes discovery genuinely read-only** and lets it sit inside the existing discovery rules
without an exception.

**One request returns many values.** A single frame can carry a dozen codes and get a dozen answers,
so a full poll is one transaction rather than a dozen. On a shared half-duplex bus that matters.

## Profile, codec, or both

The profile schema draws the line already: protocol logic and framing are a codec in C++; a
mapping of addresses to measurements is a table. It also says outright that a genuinely different
register-map family *"would get its own table-driven driver and reuse this same profile pipeline"*.

MaxTalk is not Modbus, so it cannot be an existing profile. But its content is still a table —
just keyed by a **code string** rather than a register number.

| | Approach | Consequence |
|---|---|---|
| **a** | hand-written codec and driver, like the existing legacy family | brand knowledge in C++; a second SolarMax series means a code change |
| **b** | codec in C++ for framing, **plus a table-driven driver reusing the profile pipeline** | rows become `code = "PAC"` instead of `address = 3000`; a second series is a TOML |
| **c** | codec only, no table, everything hardcoded | fastest to a first reading, worst to extend |

**b is the one the schema anticipated**, and it splits exactly where the existing Modbus path
splits: framing in C++, mapping in data. SolarMax shipped several series — S, MT, C, P, TS — and a
table means the second one costs a file rather than a release of driver changes.

It is also the more honest place for the unverified scaling factors: a profile carries a `status`
field and can ship as `experimental` with its sources named, which is precisely the state this
information is in.

## What it would look like here

- **Transport**: the existing RS485 transport, unchanged. 19200 8N1 is an ordinary serial profile.
- **Codec**: brace framing, ASCII hex parsing, checksum. Pure, no Arduino dependency, so it belongs
  in the host-testable core beside the Modbus RTU codec and is fully unit-testable from captured
  frames.
- **Driver**: builds a query from the codes its profile maps, parses the reply, fills the canonical
  set. Read-only.
- **Discovery**: a probe is one query for `TYP` and `SWV`. Read-only, no state change, no address
  assignment — it fits the existing rules with no exception requested.

**Read-only, and that is not a limitation to apologise for.** No write or control command appears
in either source. The primary goal — Home Assistant steering an inverter — does not require every
driver to be writable, and a driver that declares no write capability is refused by the dispatcher's
capability gate without any special handling.

## What hardware has to settle before this ships

Nothing here can be validated remotely, and the list is short but not optional:

1. **`PAC`'s divisor.** Compare against the inverter's own display. Everything else is a scale
   error you would notice; this one you would not.
2. **Whether `KT0` is total energy**, and its divisor. The two sources are least specific here.
3. **The `|64:` literal** — whether it is fixed or a device/port selector that varies by series.
4. **The length-field formula**, against a real device that ignores malformed frames silently.
5. **`SYS` and `SAL` code tables**, which neither source enumerates.

## Open questions

**Which series this actually covers.** The sources name S-series explicitly and reference C, MT and
TS elsewhere. Whether the code vocabulary is constant across them is unknown, and it is the
question option **b** above is designed to absorb.

**Where the five orphan energy codes go**, if anywhere.

**Who tests it.** There is no SolarMax here. Unlike the profile queue — where at least the devices
exist somewhere in reach — this one needs a stranger with abandoned hardware and a reason to help.
That is a real constraint on scheduling it, not a detail.

## Out of scope

- Omnik and Trannergy, which are a network case and not this shape at all
- Any write or control path; none is documented
- Building the codec before item 1 above has been checked against a display

## Sources

- [MaxTalk protocol reverse-engineering writeup, 2009](https://2007.blog.dest-unreach.be/2009/04/15/solarmax-maxtalk-protocol-reverse-engineered/) — frame format, checksum, addressing, line settings
- [borsti87/solarmax-inverters](https://github.com/borsti87/solarmax-inverters) — a working implementation; the scaling table above comes from it
- [Ardexa's SolarMax connection notes](https://docs.ardexa.com/knowledge/configure/plugins/solar-inverter-plugins/solarmax-inverters/solarmax) — RS485 pinout: pin 7 is A/485+, pin 8 is B/485−; C-series and several accessories need 12–15 V DC supplied
- [Renewable Energy World on the Sputnik bankruptcy](https://www.renewableenergyworld.com/solar/when-an-inverter-manufacturer-goes-bankrupt-facing-the-impact-from-different-angles/)
