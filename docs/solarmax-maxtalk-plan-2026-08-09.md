# SolarMax MaxTalk — protocol notes and a driver plan, 2026-08-09

**Nothing here is built.** No driver, no codec, no profile. This records what the MaxTalk
protocol is, how it would fit this firmware, and — more usefully — **which parts are agreed by two
independent sources, which rest on one, and which the two sources actively disagree on**.

**Nobody here owns a SolarMax.** Everything below is desk research. Two agreeing sources are not a
device agreeing, which is the rule this project already applies to every Modbus register map, and
it applies here with more force because there is no inverter on any desk to check against.

## Why this brand

Sputnik Engineering, the Swiss maker of SolarMax, went bankrupt in 2014, leaving owners and
service providers without support. The hardware kept working; the company behind the monitoring
did not.

That is the case this firmware exists for, and it is the same shape as its only Stable driver —
an abandoned family with a serial port and no vendor left to ask.

## The protocol

ASCII over RS485. **19200 baud, 8 data bits, no parity, 1 stop bit.**

```
reply (quoted from captured traffic):
{05;FB;59|64:CAC=1F3E;KHR=26D6;KDY=8E;KMT=8D;KYR=221;KT0=18C7;KLD=78;KLM=F8;KLY=A8F|154C}

request (RECONSTRUCTED here, not quoted — see below):
{FB;05;36|64:CAC;KHR;KDY;KMT;KYR;KT0;KLD;KLM;KLY|0D34}
```

The reply is a byte-for-byte quote. **The request is not**: no request frame carrying those codes
appears in any source, so it was built from the documented rules to match the reply. Its length and
checksum were recomputed and both check out, but it is a worked example, not evidence. Anyone
implementing this should treat it as a test vector to reproduce, not as observed traffic.

The shape is `{source;destination;length|64:payload|checksum}`:

| Field | |
|---|---|
| source | two hex digits. The querying computer is `FB` |
| destination | two hex digits. The inverter's address, set in its own display menu |
| length | two hex digits, the whole frame's length including braces |
| `\|64:` | a literal that appears in every frame from both sources. **Its meaning is not established** |
| payload | query codes separated by `;`. In a reply each code carries `=` and a hex value |
| checksum | four hex digits: the ASCII values of every preceding character, summed, **truncated to 16 bits** |

Framing is **brace-delimited**, which removes a class of problem: a reader finds a frame boundary
by looking for `}`, with no dependence on the length field being right first.

### The length field, which is where a silent failure would come from

The length is not the payload length — it covers the whole frame including the braces. Both
sources compute the same value, one of them as `9 + len("|64:...|") + 5`. A frame with a wrong
length is ignored rather than rejected, so getting this wrong looks exactly like a device that is
not answering.

## What the sources agree on, disagree on, and each say alone

Two independent sources: a 2009 reverse-engineering writeup **together with the working Perl script
it publishes**, and a separate Python implementation. The Perl script matters — it carries its own
conversion factors, and reading only the blog prose understates how much is corroborated.

**Agreed by both:** the frame shape, the `FB` computer address, 19200 8N1, the checksum algorithm
including the 16-bit truncation, the length-field computation, and the scaling for `PAC`, `KDY`,
`UL1`, `IL1` and `IDC`.

**Contested — the two sources disagree:** `UDC`. The Perl script scales it by the same factor it
uses for AC voltage, implying ÷10; the Python implementation leaves it raw. **This is the first
thing to check against hardware**, because it is the only value where the sources actively
conflict rather than merely being silent.

**One source only:** `UL2`, `UL3`, `IL2`, `IL3` and `PDC`. These appear in the Python
implementation and nowhere in the Perl script, so they are uncorroborated — not wrong, just
unchecked.

## Codes, and how they map onto the canonical model

Ids below are the real constants from the canonical model. Where a code has per-phase or
per-string variants, each variant is its own id — there is no wildcard form.

| Code | Meaning | Scale | Sources | Canonical id |
|---|---|---|---|---|
| `PAC` | AC power | ÷2 | **both** | `ac.power.total` |
| `PDC` | DC power | ÷2 | one | `dc.power.total` |
| `UL1` | AC voltage L1 | ÷10 | **both** | `ac.phase_l1.voltage` |
| `UL2` `UL3` | AC voltage L2, L3 | ÷10 | one | `ac.phase_l2.voltage`, `ac.phase_l3.voltage` |
| `IL1` | AC current L1 | ÷100 | **both** | `ac.phase_l1.current` |
| `IL2` `IL3` | AC current L2, L3 | ÷100 | one | `ac.phase_l2.current`, `ac.phase_l3.current` |
| `TNF` | grid frequency | ÷100 | one | `ac.frequency` |
| `TKK` | inverter temperature | raw | one | `inverter.temperature` |
| `UDC` | DC voltage | **contested: raw vs ÷10** | conflict | `dc.mppt_1.voltage` |
| `IDC` | DC current | ÷100 | **both** | `dc.mppt_1.current` |
| `UD01` `UD02` `UD03` | per-string voltage | ÷10 | one | `dc.mppt_1.voltage`, `dc.mppt_2.voltage`, `dc.mppt_3.voltage` |
| `ID01` `ID02` `ID03` | per-string current | ÷100 | one | `dc.mppt_1.current`, `dc.mppt_2.current`, `dc.mppt_3.current` |
| `KDY` | energy today | ÷10 | **both** | `energy.today` |
| `KT0` | energy total | raw | **both** | `energy.total` |
| `KHR` | operating hours | raw | one | `inverter.operating_hours` |
| `KMT` `KLM` `KYR` `KLY` | month, last month, year, last year | mixed | one | **none — no canonical id** |
| `SYS` | system status | code lookup | one | `statusCode` / `statusText` |
| `SAL` | system alarms | code lookup | one | `errorCode` |
| `TYP` `SWV` `ADR` | type, firmware, address | — | one | `DeviceIdentity` |

**`UDC` is the entry to check first**, and it is the only one where the evidence conflicts. A DC
voltage wrong by a factor of ten is obvious on a dashboard, which is the good case; the risk is
picking one source's answer silently, which is what an earlier draft of this note did.

**`UDC` and `UD01` may be the same measurement.** Both land on `dc.mppt_1.voltage` above. On a
single-string inverter that is likely one value under two names; on a multi-string one it may be an
array total. Nothing in the sources settles it, and a driver must not publish both into one id.

**Three strings, and this firmware publishes five.** The per-string codes stop at `UD03`/`ID03`, so
a SolarMax maps comfortably inside the existing tracker vocabulary with room left over.

**Four energy codes have nowhere to go**, not five: month, last month, year and last year. Hours
(`KHR`) does have a home — `inverter.operating_hours` is a live channel that two shipping drivers
already publish, with a Modbus register, a Prometheus metric and a dashboard row. The four
remaining ones are a vocabulary question, not a driver question, and should be answered
deliberately rather than by inventing ids inside a driver.

## What makes this easier than the driver we already have

**There is no registration handshake.** The inverter's address is set in its own menu; a querying
device simply addresses it. The AA55 family this firmware already supports does the opposite — a
device has to be *assigned* a bus address before it answers, which is why discovery for that family
cannot be read-only. A MaxTalk probe sends a question and receives an answer, changing nothing.

**That makes discovery genuinely read-only** and lets it sit inside the existing discovery rules
without an exception.

**One request returns many values.** A single frame can carry a dozen codes and get a dozen answers,
so a full poll is one transaction rather than a dozen. On a shared half-duplex bus that matters.

An earlier draft also claimed brace framing was an advantage over "the Modbus RTU path, where frame
boundaries are found by timing". **That is not how this codebase finds them** — the Modbus codec
computes the exact expected reply length from the request and reads until that parses or the
transaction deadline passes. The framing here is still simple, but it is not simpler than what is
already in the tree, and the comparison was flattering rather than true.

## Profile, codec, or both

The profile schema draws a line: protocol logic and framing are a codec in C++; a mapping of
addresses to measurements is a table.

MaxTalk is not Modbus, so it cannot be an existing profile. But its content is still a table — just
keyed by a **code string** rather than a register number.

| | Approach | Consequence |
|---|---|---|
| **a** | hand-written codec and driver, like the existing legacy family | brand knowledge in C++; a second SolarMax series means a code change |
| **b** | codec in C++ for framing, **plus a table-driven driver reusing the profile pipeline** | rows become `code = "PAC"` instead of `address = 3000`; a second series is a TOML |
| **c** | codec only, no table, everything hardcoded | fastest to a first reading, worst to extend |

**b, but as an extension of the schema rather than something it already covers.** The schema says a
genuinely different *register-map* protocol family would get its own table-driven driver and reuse
the pipeline. MaxTalk is not register-map — it is code-keyed — so applying that sentence here is an
extrapolation, and reusing the pipeline for it would be a deliberate widening of what a profile
means. Worth doing, in my reading: SolarMax shipped several series, and a table makes the second
one a file rather than driver changes. But it is a decision to take, not a precedent to cite.

It is also the more honest home for the unverified scaling: a profile carries a `status` field and
can ship as `experimental` with its sources named, which is precisely the state this information
is in.

## What it would look like here

- **Transport**: the existing RS485 transport, unchanged. 19200 8N1 is an ordinary serial profile.
- **Codec**: brace framing, ASCII hex parsing, checksum. Pure, no Arduino dependency, so it belongs
  in the host-testable core beside the Modbus RTU codec and is fully unit-testable from the frames
  above.
- **Driver**: builds a query from the codes its profile maps, parses the reply, fills the canonical
  set. Read-only.
- **Discovery**: a probe is one query for `TYP` and `SWV`. Read-only, no state change, no address
  assignment — it fits the existing rules with no exception requested.

**Read-only, and that is not a limitation to apologise for.** No write or control command appears
in either source. The primary goal — Home Assistant steering an inverter — does not require every
driver to be writable, and a driver that declares no write capability is refused by the dispatcher's
capability gate with no special handling.

## What hardware has to settle before this ships

Nothing here can be validated remotely, and the list is short but not optional:

1. **`UDC`'s divisor**, where the two sources conflict outright.
2. **Whether `UDC` and `UD01` are the same measurement**, since both map to one canonical id.
3. **The five single-sourced scalings** — `PDC`, `UL2`, `UL3`, `IL2`, `IL3` — against the display.
4. **The `\|64:` literal**: fixed, or a device/port selector that varies by series.
5. **`SYS` and `SAL` code tables**, which neither source enumerates.

## Open questions

**Which series this actually covers.** The sources name S-series explicitly and reference C, MT and
TS elsewhere. Whether the code vocabulary is constant across them is unknown, and it is the
question option **b** above is designed to absorb.

**Where the four orphan energy codes go**, if anywhere.

**Who tests it.** There is no SolarMax here. Unlike the profile queue — where at least the devices
exist somewhere in reach — this one needs a stranger with abandoned hardware and a reason to help.
That is a real constraint on scheduling it, not a detail.

## Out of scope

- Omnik and Trannergy, which are a network case and not this shape at all
- Any write or control path; none is documented
- Building the codec before items 1 and 2 above have been checked against a display

## Sources

- [MaxTalk protocol reverse-engineering writeup, 2009](https://2007.blog.dest-unreach.be/2009/04/15/solarmax-maxtalk-protocol-reverse-engineered/) — frame format, checksum, addressing, line settings. **The Perl script it publishes is part of this source** and carries its own conversion factors; the corroboration above depends on reading it, not only the prose.
- [borsti87/solarmax-inverters](https://github.com/borsti87/solarmax-inverters) — a working Python implementation; the second opinion on every scaling factor.
- [Ardexa's SolarMax connection notes](https://docs.ardexa.com/knowledge/configure/plugins/solar-inverter-plugins/solarmax-inverters/solarmax) — RS485 pinout: pin 7 is A/485+, pin 8 is B/485−; C-series and several accessories need 12–15 V DC supplied.
- The Sputnik bankruptcy is widely reported, including by Renewable Energy World. **That article returned HTTP 403 to every attempt to read it**, so the date above was confirmed from other coverage rather than from it, and no wording is quoted from it here.
