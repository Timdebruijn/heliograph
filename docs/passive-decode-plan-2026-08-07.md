# Passive decode — design note, 2026-08-07

**Nothing here is built.** This is a plan, written while the reasoning was fresh, so it does not
have to be derived again. It describes turning a passive bus capture into a readable transaction
list. Read it as intent, not as description.

**One number in it is unmeasured**, and the design depends on it. See [What to do
first](#what-to-do-first) — that step comes before any code.

## Why

The bridge can already record a bus it does not drive. What it hands back is a list of frames
with timings and a checksum verdict, and nothing that says what any of them *mean*: which unit,
which function code, which registers, and which reply belongs to which request.

The case this is for is a Modbus RTU bus with somebody else's master on it — a vendor dongle
polling inverters. Listening tells you which registers that dongle actually reads, without the
bridge putting a single byte on a live bus. For confirming an `experimental` register map that
is the safest tool available, and it is safer than the address sweep discovery already declines
to widen.

Not applicable to a device whose vendor dongle does not speak RS485 at all. A field session on
the SolaX X1-Mini's RJ45 returned **zero bytes**; there is nothing there to listen to, and its
route is HTTP to the dongle. Recorded because the opposite was briefly assumed.

## What already exists

| | Where |
|---|---|
| Passive capture that transmits nothing | `src/app/capture_runner.cpp` |
| Frame recorder with direction | `src/diagnostics/bus_tap.{h,cpp}` |
| Gap-based frame delimiting | `CutReason::{IdleGap, DirectionChange, ByteCap}`, host-tested |
| REST entry and report | `POST /api/v1/actions/capture` (`mode=passive`), `GET /api/v1/capture` |
| Precedent for a host-side decoder | `tools/decode_eversolar.py` |

The capture side is done. This note is only about what happens to the bytes afterwards.

## The report this consumes

Per frame, from `buildCapturePayload()`:

```json
{ "offset_ms": 1204, "gap_before_ms": 47, "length": 8,
  "modbus_crc_ok": true, "aa55_ok": false, "hex": "01 03 0B B8 00 08 45 8C" }
```

The CRC verdict is already there, so frame boundaries do not have to be guessed.

## The problem: passive capture has no direction

A *driver* capture carries `direction`, because the bridge knows what it sent itself. A
*passive* capture does not, and correctly so — the bridge transmits nothing, so every frame
arrives on RX. **A foreign master's request and its slave's reply are indistinguishable by
direction.** That is a property of listening, not a gap in the report, and pairing them is the
decoder's actual job.

## How to pair them

Modbus RTU offers three signals, and together they are enough.

**Shape.** A read request (FC 03/04) is exactly 8 bytes: `unit, fc, addr_hi, addr_lo, count_hi,
count_lo, crc_lo, crc_hi`. A reply is `unit, fc, bytecount, data…, crc`, where
`bytecount == 2 × count` from the request.

**Identity.** Unit id and function code must match between the two.

**Silence.** `gap_before_ms` decides: between request and reply sits the device's response time
(milliseconds); between transactions sits the master's poll cycle (hundreds of ms to seconds).

A pair is therefore: an 8-byte frame with a valid CRC, followed by a frame with the same unit and
FC whose `bytecount` matches, separated by a gap smaller than the one that follows.

**Where this fails, and each must appear in the output with a reason rather than be dropped:**

- a request with no reply
- an exception reply (FC with bit 7 set) — recognised as such, not as a malformed answer
- a frame cut by `ByteCap` — flagged, never half-decoded
- two masters, or devices whose traffic overlaps — no pair invented

## What it emits

A transaction list. What happened, without interpretation:

```
t=1204ms  unit 1  FC03  holding 3000..3007   ->  8 registers   [2305 0500 ...]
t=1251ms  unit 2  FC04  input   0..124       ->  125 registers
t=2198ms  unit 1  FC03  holding 3000..3007   ->  (no reply)
```

**A profile skeleton was considered and dropped.** A capture can observe addresses; it cannot
know a `measurement`, a `scale` or a `unit`. Emitting a skeleton would present guesses in the
shape of a profile, which is exactly what the schema's `experimental` status exists to prevent.
It can be added later if the transaction list proves itself.

## Where it lives, and what the layering check says

`tools/decode_modbus_capture.py`, beside `tools/decode_eversolar.py`.

Checked rather than assumed: of the eleven rules in `tools/check_layering.sh`, **`tools/*.py`
appears in exactly one** — rule 8, "no comment cites a line number in our own source". So the
decoder may name a function but must not pin one to a line number.

(Stated precisely, because an earlier draft of this paragraph got it wrong and claimed the rule
had caught this file. It cannot have: rule 8 scans `src/`, `test/` and `tools/*.py`, and **not**
`docs/`. Checked by putting a line reference in a markdown file and watching the rule pass. So the
constraint binds the decoder script, which lives in `tools/`; it does not bind this note. Naming a
function rather than a line is still the right habit here — the line drifts either way — but that
is a convention, not something the build enforces.)

Rule 1 (brand knowledge only in `src/drivers/`) does not reach `tools/`, and would not fire
anyway: function codes 03 and 04 are the Modbus standard, not brand knowledge. That is the same
reason `src/protocols/modbus/` is allowed to exist.

## Testing

The existing shape for `tools/`: `ruff check` and `ruff format --check` in CI, plus a `--check`
mode like `gen_profiles.py` and `gen_coverage.py` already carry — a stored sample report in, the
expected transaction list beside it, non-zero exit on a difference. No hardware, no firmware
build: JSON in, text out.

What the tests must pin, the last two being where decoders usually go quiet:

- a normal pair is matched
- a request without a reply appears as its own line, with a reason
- an exception reply is recognised as one
- a `ByteCap` frame is flagged rather than decoded
- overlapping devices produce no false pair

## What to do first

**Run one thirty-second passive capture on the real bus before fixing the heuristic.**

The pairing rests on the gap between request and reply being well below the gap between
transactions. That follows from the protocol and the baud rate, but it has **not been measured
against the vendor dongle it is meant to read**. If that dongle polls faster than assumed, the
distinction collapses and pairing has to work on shape alone — a different decoder.

One capture answers it. Building first would mean fixing a heuristic on a derived number.

## Out of scope

- The profile skeleton (dropped above)
- Decoding PMU/AA55 — `tools/decode_eversolar.py` already reads those frames
- Any change to `src/`
- A structural no-transmit guarantee in the transport, which is a separate decision
- Writing anything into `profiles/`
