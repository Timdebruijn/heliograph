# SolarMax — MaxTalk over RS485

**Experimental, and unusually so: no SolarMax has ever answered this driver.** The protocol was
implemented from two independent published sources that agree on the framing. Several scaling
factors rest on only one of them, and one is left unmapped because the two disagree. Read
[What is not published, and why](#what-is-not-published-and-why) before trusting a number.

Sputnik Engineering, which made these inverters, went bankrupt in 2014 and its monitoring portal
went with it. The hardware kept working. This driver exists so those units can still report
something without a vendor.

## Wiring

Ordinary RS485: A, B and ground, chained rather than starred, 120 Ω at each end of the run. See
[rs485-bus.md](rs485-bus.md) — none of that is specific to this family.

On the inverter's own connector the published pinout is **pin 7 = A (485+)** and **pin 8 = B
(485−)**. Some models and the separate accessories need 12–15 V DC supplied on the same connector
to power their communication side; the inverter's manual is the authority on which.

## Line settings and addressing

| | |
|---|---|
| Baud | 19200 |
| Format | 8 data bits, no parity, 1 stop bit |
| Address | set in the inverter's own display menu, 0–255 |

**There is no registration handshake.** Nothing is assigned over the wire: the inverter answers at
whatever address its operator configured, and the driver simply asks. Two consequences worth
having:

- **Discovery is genuinely read-only here.** A probe sends a question and changes no state, unlike
  the AA55 family where a device must be assigned an address before it answers at all.
- **Several inverters on one bus is ordinary**, not an exception — give each one its own address
  in its menu, and set the matching `address` option per device here. A duplicate in the menu is a
  duplicate on the wire, and this protocol has no way to detect that for you.

## The protocol, briefly

ASCII, brace-delimited, request and response:

```
request  {FB;05;36|64:CAC;KHR;KDY;KMT;KYR;KT0;KLD;KLM;KLY|0D34}
reply    {05;FB;59|64:CAC=1F3E;KHR=26D6;KDY=8E;KMT=8D;KYR=221;...|154C}
```

`{sender;recipient;length|64:payload|checksum}`. The querying computer is `FB`. Length counts the
whole frame including both braces. The checksum is every character after the opening brace up to
and including the `|` before it, summed and reduced to 16 bits.

Two properties that make this family pleasant to read:

- **A frame ends at `}`**, so nothing depends on inter-character timing or on trusting the length
  field before the frame is complete.
- **One request carries many codes and gets many answers**, so a full poll is a single transaction
  rather than one per channel — which matters on a shared half-duplex line.

The framing lives in `src/protocols/maxtalk/` and names no brand; the mapping below is in
`src/drivers/solarmax/`.

## What is published

| Code | Channel | Divisor | Sources |
|---|---|---|---|
| `PAC` | `ac.power.total` | 2 | **both** |
| `PDC` | `dc.power.total` | 2 | one |
| `UL1` `UL2` `UL3` | `ac.phase_lN.voltage` | 10 | `UL1` **both**, the others one |
| `IL1` `IL2` `IL3` | `ac.phase_lN.current` | 100 | `IL1` **both**, the others one |
| `TNF` | `ac.frequency` | 100 | one |
| `TKK` | `inverter.temperature` | 1 | one |
| `UD01`–`UD03` | `dc.mppt_1..3.voltage` | 10 | one |
| `ID01`–`ID03` | `dc.mppt_1..3.current` | 100 | one |
| `KDY` | `energy.today` | 10 | **both** |
| `KT0` | `energy.total` | 1 | **both** |
| `KHR` | `inverter.operating_hours` | 1 | one |
| `SYS` | status code | — | one |
| `SAL` | error code | — | one |

The phase and string counts are **not declared in advance**: they follow what the device actually
answers. Asking a single-phase unit for `UL2` simply gets no `UL2` back, and the channel then
reads as absent rather than zero.

The status code is reported as a number with no words attached. Neither source enumerates the
status table, and inventing labels for it would be a guess wearing a label.

## What is not published, and why

**`UDC` — DC voltage — is deliberately unmapped.** The two sources conflict: one scales it like AC
voltage (divide by ten), the other leaves it raw. Picking a winner is exactly what this project's
two-source rule exists to prevent, and a DC voltage wrong by a factor of ten is a number an
operator would believe. `IDC` is left out with it, because alone it would be a current with no
voltage beside it — and it may simply be another name for `ID01` on a single-string unit.

On a device that reports per-string values (`UD01`/`ID01`) nothing is lost. On one that reports
only the totals, **DC voltage and current will be missing entirely.** That is the honest outcome
rather than a plausible wrong one, and it is the first thing a hardware session should settle.

**Four energy codes are not asked for at all**: `KMT`, `KLM`, `KYR`, `KLY` — this month, last
month, this year, last year. There is no canonical id for them, and a code with nowhere to go is
payload spent for nothing on a shared bus. Giving them a home is a vocabulary decision rather
than a driver one; until it is taken, the driver does not query them.

**Nothing is writable.** Neither source documents a write or control command for this protocol, so
the driver declares no write capability at all and the dispatcher refuses any command before it
reaches the device.

## What a hardware session should check, in order

1. **`UDC`'s divisor**, against the inverter's own display. It is the only conflict.
2. **Whether `UDC` and `UD01` are the same measurement.**
3. **The single-sourced divisors** — `PDC`, `UL2`, `UL3`, `IL2`, `IL3`, `TNF`, `TKK`, `KHR`.
4. **The `|64:` literal**: fixed across the range, or a selector that varies by series.
5. **The `SYS` and `SAL` code tables**, which neither source enumerates.

If you have one of these inverters, that list is the whole contribution — five answers, and this
driver stops being guesswork.

## Which models

The sources name the S-series explicitly and refer to C, MT and TS elsewhere. Whether the code
vocabulary is constant across the range is **unknown**. A model that answers a query with a
correct checksum is speaking this protocol; whether its numbers mean what the table above says is
the open question.

The reasoning behind the design, and which claim came from which source, is recorded in
[solarmax-maxtalk-plan-2026-08-09.md](solarmax-maxtalk-plan-2026-08-09.md).
