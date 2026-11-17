# EverSolar active-power control — design note, 2026-11-17

**Nothing here is built.** This is Phase 0 of a phased plan: a design and a list of open
questions, written before any code. Read it as intent, not as description. Each later phase ends
with green checks, a hardware test on the maintainer's TL3000-20, and a stop for the result.

**The plan rests on one unmeasured fact**: whether the inverter lists write function `0x20` in
its own read/write description (`11 01`). Phase 1 measures it at no risk. Phases 2 and 3 do not
start until it has.

Every protocol claim carries one of three labels:

- **documented** — stated in a vendor document;
- **observed** — seen on someone's hardware (ours, or a named community project);
- **inference** — our reasoning, to be checked.

A note on the main source. The Eversolar PMU protocol V1.1 PDF is a scanned image (CCITT fax
streams, with a security handler), so its text cannot be extracted. The **documented** facts
below come from a manual reading of it, made while researching this note's brief. They were not
re-read mechanically in this session. Phase 1 replaces the critical ones with observations. The
PDF says copying is forbidden, so it is not committed here and its text is not quoted: it is
cited, and the facts are reimplemented.

## Goal

Let Home Assistant limit the output of an EverSolar/Zeversolar legacy TL inverter, opt-in and
default closed, without ever putting the inverter's grid-protection settings or firmware at risk.
The test unit is the maintainer's own TL3000-20 on the production bridge.

## What changes against what we wrote before

[`eversolar-protocol.md`](eversolar-protocol.md) says WRITE (`0x12`) and EXECUTE (`0x13`) are
empty, and that the driver is therefore read-only "as a property of what we know". That was true
of our source, `eversolar-monitor`, which leaves both empty. The vendor document does fill them.
So the statement stays correct about the reference, but it no longer describes the protocol.
[`pmu_protocol.h`](../src/protocols/pmu/pmu_protocol.h) and
[`eversolar_driver.h`](../src/drivers/eversolar_legacy/eversolar_driver.h) carry the same
sentence in comments. All three are corrected in the Phase 1 PR, together with an entry in
[`decisions.md`](decisions.md). Not here: this PR is the plan only.

## What is known

### Vendor PMU protocol V1.1 (documented)

- **Framing.** `AA 55 | src(2) | dst(2) | ctrl | func | len | data | sum16 BE`, master `01 00`,
  9600 8N1. This matches the driver byte for byte.
- **Timing.** At least 0.5 s between instructions. The inverter answers within 0.5 s. The master
  retries after 0.5 s and gives up after 3 tries. The driver already waits 1000 ms between frames
  (`kInterFrameGapMs`), so it is inside that rule.
- **Read functions we do not use yet:**
  - `11 00` → `11 80`: read description, a list of the read-only data codes.
  - `11 01` → `11 81`: read/write description, a list of the *writable* data numbers this
    inverter supports.
  - `11 04` → `11 84`: set info, the current setting values: Vpv-Start, T-Start, Vac-Min/Max,
    Fac-Min/Max. **No power-limit field.**
  - `11 42` → `11 C2`: master, slave and logger versions.
  - `11 05`: S/W version, labelled 17K only. We do not send it.
- **Write function table (`0x12`).** All of it is grid protection or identity, except one entry:
  `0x20` "Control Power", data `2` = decrease, data `8` = increase. It is a step, not a
  percentage. No step size, persistence or read-back is stated.
- **Write request and reply.** The request carries two data bytes. The reply carries one byte:
  `0x06` set OK, `0x15` refused, `0x00` checksum or format error.
- **Byte order.** The document says words are sent LSB first. Our driver reads this inverter's
  `11 82` words big-endian, and the values decode correctly (**observed**, on the TL3000-20). The
  community implementations do the same. A 16-bit sum checksum cannot tell the two orders apart,
  and whether a *write* follows the read convention is unknown.

### Inferences on top of that

- The write reply's function code is not stated. The family convention (response = function |
  `0x80`) predicts `12 A0`. **inference**
- A NAK (`0x15`) on `12 20` most likely means power management is switched off on the inverter.
  **inference**, supported by one forum report of exactly that symptom on a TL3680.

### The inverter side (documented)

The Zeversolar *Single Phase Inverter Power Management & EEG Setup Guide* (2016) covers Eversol
TL, Evershine TL and Zeverlution S. It says power management (ripple control 0/30/60/100 %,
active power limitation) must first be enabled on the LCD (`ActiPowFun: ON`), and it gives a
minimum firmware: **Eversol TL 610-01023-06**. For NL and UK the guide says to leave it off,
which explains why it is missing from the normal manual.

**Our unit (observed).** The TL3000-20 identifies as
`1  3000E1.00   TL3000-20    Ever-Solar      XH300060115506193600V610-01023-06` (captured
2026-07-19 on the production bridge, the reply to `11 03`). The suffix `V610-01023-06`
is exactly the minimum. That this suffix *is* the firmware version is **inference**: the TLC8000
capture uses the same shape (`V610-03017-03`). The LCD's Version screen confirms it.

### The family (documented, for SolaX)

SolaX X1 protocol V1.8 lists write `0x12` *wAcPowerLimit* (1 %) and `0x1F` *wPowerManagerEnable*.
So the family gained a percentage limit later. **For EverSolar this is inference only.** Neither
function is in the EverSolar table, so under
[`write-path.md`](device-profiles/write-path.md) both are disqualified, unless the inverter lists
them itself in `11 81`. Even then they stay unproven, because "listed" does not mean "same
meaning as on a SolaX".

### Community (observed where they ran hardware, otherwise their claims)

- `timretout/zeversolar-esphome`: reads only. Its README *claims* that write and execute are
  undocumented, and that firmware updates travel over the same port. Neither is observed.
- `craimasjien/ha-zeversolar-modbus`: has a passive mode that only decodes `11 82` replies to
  another master's polls. Not this work, see [Later](#later-not-this-work).
- `kmbrimble/zeversolar-esphome-gateway` (archived): claims a newer TL3000 generation with an
  external DRED port. Unverified, and possibly not our generation.
- A forum user reports an Eversol TL-3000 limited to 25/50/100 % "via a packet", without bytes.
  Unverified.

## Two routes

| | RS485 write (`12 20`) | DRM/DRED relay |
|---|---|---|
| Exists on our unit? | Unknown. Phase 1 answers it. | Unknown. Needs a physical look for a DRED port. |
| Granularity | Steps of unknown size | DRM0 = off. DRM5–8 = levels, only on a 6CH board and an inverter that honours them. |
| Read-back of the limit | None documented | The relay state is ours. The inverter's response is not readable. |
| Hardware we need | None. The production RS485-CAN board can do it. | A Relay board in place of the RS485-CAN. Same RS485 pins per the schematic, untried on the 1CH. |
| Firmware region | Power management needs an LCD switch the NL guide says to leave off | AS4777 DRM is usually AU firmware. SolaX confirmed that for its own units (2026-11-05). |
| Our side proven? | No write has ever left this driver | Relay side hardware-verified, inverter side never wired |
| Failure mode | A step that persists and leaves the inverter curtailed | Wiring inverts the failsafe (see [`drm.md`](drm.md)) |

**Recommended order:** run Phase 1 and the DRED port check in parallel. Both cost nothing and
risk nothing. Then:

- `0x20` listed → Phase 2.
- Not listed, DRED port present → the DRM track.
- Neither → document "not controllable" and stop.

## Mapping onto the architecture

All brand knowledge stays in `src/drivers/eversolar_legacy/`, per the layering rule. The
function codes, the whitelist and the decoding of description replies live there.
`src/protocols/pmu/` only gains the generic codes `11 01`, `11 04` and `11 42`, and no write
helpers.

### Phase 1: a driver diagnostic query hook

The existing tools do not fit. `RegisterReader` is Modbus-only, and refuses a PMU bus on purpose.
`DriverCaptureRunner` records the driver's own traffic but sends nothing new. So:

- `InverterDriver` gains one optional, brand-free pair: a list of named diagnostic queries and a
  call that runs one of them. The default is empty. EverSolar lists `read_description` (`11 00`),
  `rw_description` (`11 01`), `set_info` (`11 04`) and `versions` (`11 42`). Every one of them is
  a READ.
- A small runner in `src/app/` has the same handover as `RegisterReader`. The web task requests,
  and `rs485Task` runs the query in place of one poll, because that task owns the bus. Never from
  the HTTP handler.
- The result holds the raw TX and RX hex, the parse status, and the documented decoding where we
  have one. The rest is shown raw. A reply that does not arrive is a timeout, not an empty list.
  Unknown is never zero.
- The REST surface is `POST /api/v1/actions/driver-query` plus `GET` for the report, admin-gated
  like the other actions. Reads are not under `security.read_only_mode`, for the same reason
  `RegisterReader` is not: no write path is reachable from here.
- Tests: the fake device answers the four queries with constructed fixtures (generated by
  `tools/gen_fixtures.py`, never hand-edited), plus one test that a query is refused while the
  device is unregistered. The address is volatile, so a query can only go to a registered
  inverter.

### Phase 2: one gated bench write, not a command

A step is not a setpoint. Routing it through `CommandDispatcher` as a new `InverterCommandType`
would also announce it to Home Assistant as soon as the gates open, and a bench tool must not
become an entity. So the bench action is a second kind of diagnostic query, marked `writes`:

- Two actions: `power_step_down` and `power_step_up`.
- Before anything is sent, the runner checks, in this order:
  1. `security.read_only_mode` off (the dispatcher's first gate, reused);
  2. `control.inverter_writes_enabled` on (the dispatcher's second gate, reused);
  3. **new:** the per-device driver option `allow_unverified_writes` on. The key is generic
     (`driver_descriptor.h`), but today only `modbus_profile` and `sunspec` declare it; Phase 2
     adds it to this driver's descriptor;
  4. **new:** the device is registered, and its serial matches the one this device was
     configured with. Registration alone does not prove `0x10` is still the same inverter (see
     the pitfalls in [`eversolar-protocol.md`](eversolar-protocol.md));
  5. **new:** at most one bench write per 30 s **per device, counting attempts**, not
     successes. The dispatcher's own limiter defaults to 1 s and is not reused.

  The dispatcher's capability and range gates have no equivalent here. The whitelist below
  replaces them.
- **A deliberate, temporary exception to the read-back rule.** `write-path.md` serves a write
  only when its setpoint can be read back, with or without the opt-in. The bench path has no
  read-back, and that is the point of it: finding one is what Phase 2 is for. It is acceptable
  only because the bench is not a command (no Home Assistant entity, no MQTT or automation path),
  is admin-only, and is removed or replaced in Phase 3. It must not outlive Phase 3 as-is.
- **Never retried.** A bench write goes out once. After a timeout the inverter's state is
  unknown, so an automatic retry could step twice. The vendor's "retry up to 3 times" applies to
  reads only. A timeout counts against the rate limit like any other attempt.
- **The whitelist (hard requirement), structural rather than by convention.**
  - The driver has exactly one function that builds a `0x12` frame. Its only inputs are an enum
    `{Down, Up}` and an enum `{BigEndian, LittleEndian}`, so no caller can pass a function code
    or a data value. `execute()` keeps returning `Unsupported` for every command type.
  - `pmu::buildRequest()` is generic and shared with the SolaX driver, so "one function" is not
    enough by itself. A new check in `tools/check_layering.sh` fails when a control code
    `0x12` or higher appears anywhere in `src/` outside that one builder.
  - The diagnostic-query runner refuses any frame with control `0x12` or higher unless it came
    from the bench builder, whatever the driver declares about the query.
- **A test checks the whitelist on the wire.** It drives every public entry point against the
  fake device, with a frame recorder on the transport: probe, poll, every diagnostic query, both
  bench actions in both byte orders, and `execute()` for every `InverterCommandType`. It asserts
  an **allow-list**: every transmitted frame is a known `10 xx` or `11 xx` pair, or `12 20` with
  data in `{00 02, 02 00, 00 08, 08 00}`. Anything else fails, `0x13` and above included. The
  test covers reachable paths only. The layering check above is what covers unreachable ones.
- **What is logged per bench write:**
  - the full TX and RX;
  - the reply byte, named (`ACK`, `NAK`, `format error`, or `timeout`);
  - AC power, and the complete raw `11 82` payload, right before and 60 s after.

  The raw payload matters: the TL3000-20 sends an undecoded 8-word tail (words 14–21:
  `0000 0000 FFFF 0000 0000 0000 FF00 0000`, identical in three consecutive replies captured
  2026-08-03, see
  [`eversolar-protocol.md`](eversolar-protocol.md)). If one of those words moves after a step,
  that is the read-back candidate. Word 20 (`FF00`) is the first one to watch. **inference**
- **Byte order** is a parameter of this action only. Big-endian is the default, little-endian the
  alternative. It is not a driver option and it does not persist.

### Phase 3: the read-back rule decides

[`write-path.md`](device-profiles/write-path.md) serves a setpoint only when it can be read back.
Measured AC power is not a read-back: clouds move it too. So a real capability needs all of these
from Phase 2:

1. a step size that repeats;
2. a known way back to 100 %;
3. known persistence, across a night and across a power cycle;
4. an observable read-back: a word in `11 82` or `11 84`, or anything else the inverter reports,
   that tracks **several steps in both directions** and holds across repeated polls. One moved
   word after one step is a lead, not a read-back.

Two traps for the bench session:

- A first step *down* at full sun shows in AC power, if the step is big enough. A first step *up*
  from 100 % is probably a no-op. So the session starts with down.
- A 60 s AC-power delta is noise when the step size is unknown. Step only in stable sun, and
  repeat.

With all four, the candidate is a percentage emulated by stepping, with the read-back as the
canonical `control.active_power_limit`. Without a read-back, the honest outcome is a documented
"not controllable over RS485". Whether an explicitly unverified step control ships anyway is
open question 5.

## Bytes on the wire per phase

The examples use assigned address `0x10`, the driver default (`EversolarOptions::assignedAddress`).
Recompute the checksum for any other address. Each checksum below was recomputed for this note as
the 16-bit sum of the preceding bytes. Phase 1 adds them as unit tests against
`pmu::checksum()`.

| Phase | Purpose | Frame |
|---|---|---|
| 1 | Read description `11 00` | `AA 55 01 00 00 10 11 00 00 01 21` |
| 1 | Read/write description `11 01` | `AA 55 01 00 00 10 11 01 00 01 22` |
| 1 | Set info `11 04` | `AA 55 01 00 00 10 11 04 00 01 25` |
| 1 | Versions `11 42` | `AA 55 01 00 00 10 11 42 00 01 63` |
| 2 | Step down, BE | `AA 55 01 00 00 10 12 20 02 00 02 01 46` |
| 2 | Step down, LE | `AA 55 01 00 00 10 12 20 02 02 00 01 46` |
| 2 | Step up, BE | `AA 55 01 00 00 10 12 20 02 00 08 01 4C` |
| 2 | Step up, LE | `AA 55 01 00 00 10 12 20 02 08 00 01 4C` |
| 2 | Expected ACK (**inference**) | `AA 55 00 10 01 00 12 A0 01 06 01 C9` |
| 2 | Expected NAK (**inference**) | `AA 55 00 10 01 00 12 A0 01 15 01 D8` |

Nothing else is ever sent, apart from the existing registration and poll traffic.

## Risks

| Risk | Mitigation |
|---|---|
| A step persists, and the inverter stays curtailed after the test | Step up straight after. If that fails: power-cycle (AC off, DC off, wait, DC on, AC on), then switch the LCD back to `ActiveP&EEG : OFF`. Whether that LCD switch clears a limit is **inference**. |
| `ActiPowFun: ON` without a ComBox puts the inverter in an unexpected default state | Switch it on and watch AC power for a full sunny day *before* the first bench write. Undocumented either way. |
| The wrong byte order is read as another value | Function `0x20` only, two values per order. A big-endian parser reads `02 00` or `08 00` as `0x0200` or `0x0800`, values the document does not define for `0x20`. We expect a NAK or a `0x00` format error. **inference**: the behaviour is undefined. |
| A timeout leaves the step state unknown, and a second press steps twice | No automatic retry. The rate limit counts attempts. Poll and look at AC power before pressing again. |
| Address `0x10` belongs to another inverter after a restart (multi-inverter bus) | Before each write, compare the serial with the configured one. |
| A firmware or execute path is reached | No `0x13` frame can be built (see the whitelist test). |
| Another owner's inverter is affected | Every gate is default closed. The bench action also needs the per-device opt-in. |
| The address was lost at dawn mid-test | Queries and bench writes are refused while unregistered. Test at midday with stable sun. |
| Phase 1 reads confuse older firmware | Low: these are documented read functions. A unit that does not know one should stay silent, which we record as a timeout. **inference** |

## SolaX X1 (report only, nothing to implement)

[`solax-x1-protocol.md`](solax-x1-protocol.md) concludes that the X1 Mini has no writable power
registers, and that DRM0 or meter emulation are the only control routes. That conclusion rests on
the X1 V1.2 document. V1.8's `0x12` *wAcPowerLimit* would be the family's first documented
percentage limit. In this session, nobody checked whether V1.8 also gives it a read-back. Two
things keep it academic for now:

- The only X1 on hand (an X1-Mini-G1) returned zero bytes on its RJ45.
- The SolaX driver is written against V1.2.

Revisit when someone has an X1 on V1.8-era firmware that actually talks RS485. Then add a note to
the SolaX doc, not before.

## DRM track (parallel, if a DRED port exists)

[`drm.md`](drm.md) needs nothing new to support this, only facts about the inverter:

- whether the port exists and how it is labelled;
- which pins carry DRM0 or DRM5–8;
- whether the line is asserted by closing or by opening the circuit, which decides NO or NC;
- whether NL firmware honours it at all.

The production bridge is an RS485-CAN board without relays. A Relay-1CH has the same RS485
pins (TX 17, RX 18, EN 21), per Waveshare's schematic, so it should be able to take its place and
keep monitoring. That is not yet observed: no RS485 traffic has been driven on a 1CH (see
[`hardware.md`](hardware.md)). That gives DRM0 only, which means off, not
limited. Graded limits need the 6CH and an inverter that implements DRM5–8. Once the port is
photographed, the track becomes a short section in `drm.md` for this family.

## Later, not this work

- **Passive decode of `11 82`.** Listen to an existing ComBox or Zevermanager without
  transmitting, as `craimasjien/ha-zeversolar-modbus` does. That fits the passive-decode plan
  ([`passive-decode-plan-2026-08-07.md`](passive-decode-plan-2026-08-07.md)), not a write path.

## Open questions

1. Is the test inverter's firmware at or above 610-01023-06 on the LCD's Version screen? The ID
   string suggests exactly that version.
2. Does the TL3000-20 have an external DRED/DRM port? A photo of the label and the connector
   settles it.
3. Phase 1 surface: a Health-tab tool, a REST endpoint, or both? **Proposal: REST only.** It is a
   one-off research tool for one inverter. `index_html.h` is the repo's top bug magnet, and a
   Health-tab block can follow once the queries prove useful to other owners.
4. Phase 2 byte order: big-endian first, and little-endian only after a `0x00` format error?
   **Proposal: yes.** A NAK is *not* a reason to try little-endian, because it more likely means
   `ActiPowFun` is off.
5. If `0x20` works but has no read-back: ship it as an explicitly unverified, opt-in step
   control, or stop at "use DRM"? **Proposal: stop.** Shipping a control with no read-back would
   be the first exception to the rule PR #286 made mandatory.
6. Does `decisions.md` already rule on undocumented or relative setpoints? **No.** It covers the
   framework and library choices. The binding rules are in
   [`write-path.md`](device-profiles/write-path.md): an undocumented write target is
   disqualifying, a setpoint needs a read-back, and `verified` is flipped on a bench. They are
   enforced by `tools/gen_profiles.py` and `check_readback_rule.py` for profile rows. The Phase 1
   PR adds the EverSolar entry to `decisions.md`.
7. Phase 2's bench opt-in reuses `allow_unverified_writes` as a driver option on this
   driver, so that the per-device consent means the same thing everywhere. OK, or a dedicated
   `allow_bench_writes`?

## Answers (2026-11-17)

1. **Firmware:** not read yet, because the LCD is out of reach. Phase 1's `versions` query
   (`11 42`) may answer this remotely.
2. **DRED port:** none found on visual inspection. **The DRM track is closed** for this unit,
   unless a photo shows otherwise. That leaves RS485 as the only route.
3. **Surface:** Phase 1 is REST only. The end goal is control from Home Assistant over MQTT.
   That arrives in Phase 3 through the existing command path (dispatcher → MQTT/HA discovery),
   never through the bench tool.
4. **Byte order:** big-endian first, as proposed.
5. **No read-back and no DRM:** the proposal "stop at DRM" no longer has a fallback, so this
   question is re-opened. It is decided after Phase 2, on its data. The one middle ground worth
   naming is **known by construction**. If Phase 2 shows that steps saturate, a reset is
   possible: step up until nothing changes, and the inverter is at 100 %. From there, *k* steps
   down give a known setting. That is still not a read-back. Home Assistant would show what was
   requested, not what the inverter confirms. So it would be a documented exception to the rule
   from PR #286, with its own decision entry. It is not a precedent for profile rows.
6. Answered in the note itself.
7. Open.
