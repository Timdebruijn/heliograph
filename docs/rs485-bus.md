# Wiring the RS485 bus

Everything this bridge reads over RS485 depends on three wires being right. This page covers
the physical side: topology, termination, ground, and what to do when the bus is silent or
noisy. Per-inverter details (which terminal, which address menu) live in that inverter's own
protocol document.

If you are connecting **one** inverter and it works, you can stop after "The three wires".
The rest matters once you chain several devices onto one bus.

## The three wires

| Bridge | Inverter | Notes |
|---|---|---|
| A / A+ / D+ | RS485 A | Data pair, one half |
| B / B− / D− | RS485 B | Data pair, other half |
| GND / SGND | GND / COM | Signal reference — not optional |

**Ground is not optional.** RS485 is differential, which makes people assume two wires are
enough. They are not: the receiver only rejects common-mode noise within a limited range, and
two devices on different mains phases or with metres of cable between them can drift outside
it. The failure is not a clean "no data" — it is intermittent corruption that looks like a
software bug. Connect the ground.

**A and B polarity is not standardised.** Different vendors label the same conductor
differently. Swapped A/B is harmless: you get silence, not damage. If a correctly wired bus
stays silent, swap A and B before suspecting anything else.

**On the Waveshare ESP32-S3-RS485-CAN board, `SGND` is isolated from `GND` — never bridge
them.** That isolation is the reason the board survives an inverter on a different earth. See
[hardware.md](hardware.md).

### Cable

Use one twisted pair for A/B plus a third conductor (or the shield) for ground. Ordinary
CAT5e/CAT6 is ideal and is what most people have lying around: take one pair (for example
blue/blue-white) for A and B, and a conductor from another pair for ground. Do **not** split
A and B across two different pairs — the twist is what makes the noise rejection work.

Keep the run away from PV DC strings and inverter output cabling where you can. An inverter is
an electrically hostile neighbour.

## Topology: a chain, not a star

RS485 is a **daisy chain**. Each device connects to the next; the cable enters a device and
leaves it toward the following one.

```
[Bridge] ---- [Inverter 1] ---- [Inverter 2] ---- [Inverter 3]
   ^                                                    ^
 bus end                                             bus end
```

What you must not build is a star — several separate cables radiating from one point to one
device each. It appears to work on a short bench run and then fails intermittently once the
cable gets longer, because every unterminated stub reflects the signal back onto the bus.

If a device sits slightly off the main run, keep that stub as short as you can. Centimetres are
fine; a metre is asking for trouble.

The bridge does not have to be at one end. It can sit anywhere in the chain — it is just
another device on the wire, and the one that happens to do the talking.

## Termination: 120 Ω, at exactly two places

A terminating resistor of 120 Ω goes across A and B at each of the **two physical ends** of the
bus. Not at every device. Not at three places. Two.

The reason: 120 Ω matches the characteristic impedance of twisted-pair cable, so a signal
arriving at the end of the cable is absorbed instead of reflected back along it. Reflections
collide with the data still arriving and corrupt frames.

Getting the count wrong fails in both directions:

- **Too few** (none at all): reflections. Symptom is corrupted frames — checksum errors,
  not timeouts.
- **Too many** (one per device): the parallel resistance drops far below what the transmitter
  can drive. Three 120 Ω resistors in parallel is 40 Ω. The signal amplitude collapses and the
  bus goes quiet or unreliable across the board.

In the diagram above, the two ends are the **bridge** and **inverter 3**. Inverters 1 and 2 sit
in the middle and get no termination.

### On the Waveshare boards

Both supported RS485 boards have the resistor fitted already, switchable:

- **ESP32-S3-RS485-CAN** — a 120 Ω jumper per bus. Fit it only when the bridge is at a
  physical end of the chain.
- **ESP32-S3-Relay-1CH** — 120 Ω as R23 on header H1, same rule.

On the inverter side, many inverters have a DIP switch or jumper for their own termination.
Check the manual of the unit at the far end of the chain and enable it there — and make sure
the inverters in the middle have theirs **off**.

### When you can skip it

At 9600 baud over a few metres, an unterminated bus usually works fine. The signal rise time
is long compared to the propagation delay, so reflections settle before they matter. If your
whole run is a couple of metres to a single inverter, termination is not what is wrong.

Termination becomes necessary as the bus gets longer, as the baud rate rises, and as devices
are added. The honest rule: wire it up, look at the diagnostics, and fit termination if you
see corruption.

## Giving each device an address

Every device on a shared bus needs a **unique Modbus address**. Nearly all inverters ship as
address 1, so two units straight out of the box will both answer at once, and their replies
collide into garbage.

Set the addresses **before** you chain anything together. The conventional scheme is simply
1, 2, 3 in the order they hang on the wire.

How you set it is per manufacturer. For Growatt (including the MIC TL-X), see
[growatt-mic-tl-x-protocol.md](growatt-mic-tl-x-protocol.md#giving-each-inverter-an-address).

## When it does not work

The bridge distinguishes three failure modes, and they point at different causes. All are
visible in the logs at `trace` level, in `/api/v1/diagnostics`, and as Prometheus counters
(see [prometheus.md](prometheus.md)).

**Timeouts — nothing came back.**

- A and B swapped (try swapping them; it costs nothing)
- Wrong address, or the device is still on its factory default while you are asking for
  another
- Wrong baud rate or framing
- Wrong port on the inverter — some units have several RJ45 sockets and only one is RS485
- The port is occupied by the manufacturer's own WiFi dongle
- A break in the chain: every device past the break goes silent, which is a useful clue about
  where to look

**Invalid frames — an intact frame arrived that was not an answer to us.** Usually addressing:
another device on the bus replied, or this one answered with fewer registers than asked for.
Look at the addresses before you look at the cable.

**Checksum errors — bytes came back, corrupted.** This is an electrical problem, not a
configuration one:

- Missing ground
- Termination missing, or too much of it
- A and B split across different twisted pairs
- Cable routed alongside PV DC or inverter output cabling
- A star topology, or a long stub

A night of timeouts is normal and expected: the inverter powers down when the sun does. That
is why the alerting examples in [prometheus.md](prometheus.md) watch checksum errors rather
than timeouts.

One caveat worth knowing before you trust the counters over your own eyes: noise that *drops*
bytes — which is what a missing ground most often does — leaves a frame too short to parse, and
that surfaces as a **timeout**, not a checksum error. So a bus can be electrically bad and still
show nothing but timeouts. If a "sleeping inverter" is silent at a time it should not be, treat
the cable as a suspect anyway. See the note in [prometheus.md](prometheus.md) for the other
place these counters under-report.
