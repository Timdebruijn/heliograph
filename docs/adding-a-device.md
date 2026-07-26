# Adding a device

This guide is for contributors who want the bridge to support their inverter or battery.
For a whole class of devices that is **a TOML file, not C++**: you research which Modbus
registers mean what, write a profile, and the build does the rest.

It has three parts: figuring out what kind of device you have, researching its register
map (the actual work), and turning that into a profile the build accepts.

## 1. What kind of device do I have?

**Modbus RTU over RS485** — the majority of modern inverters and hybrids (Growatt,
Deye, Sofar, Solis, many others). The device answers read requests for numbered
registers. → You can add it with a **device profile**. Continue below.

**A proprietary handshake protocol** — the device needs a registration/addressing
sequence before it talks, or uses non-Modbus framing (example in this repo: the
EverSolar/Zeversolar PMU protocol, `AA55` frames with a multi-step registration dance).
→ That needs a **codec**: C++ per protocol family, because sequencing logic does not fit
in a data file. Read [§5](#5-handshake-protocols-codecs) and open an issue first — the
protocol research is the same, but the implementation path is different.

Not sure? Two hints: if a vendor datasheet or community project mentions "Modbus",
"holding/input registers" or "function code 03/04", it is Modbus. If the official
dongle/monitoring software must "search" or "pair" before data flows, expect a
handshake.

## 2. Researching a Modbus device

Goal: for every value you want, learn its **register address**, **register space**
(input vs holding), **data type** (16/32-bit, signed?), **scale factor** and **unit**.

### 2a. Find existing documentation first

In rough order of reliability:

1. **The vendor's Modbus/RS485 protocol PDF.** Sometimes public, often obtainable by
   asking support, occasionally floating around community forums. Note the *protocol
   version* on the title page — vendors ship multiple register generations under one
   brand (Growatt's older SPH map vs the newer TL-X map is exactly this).
2. **SunSpec.** Some inverters (Fronius, SMA, SolarEdge, Huawei, …) implement the
   SunSpec information models — a standardized, self-describing register layout. If your
   device is SunSpec-certified, the register map is effectively public documentation.
3. **Community integrations that ran on real hardware.** Home Assistant integrations,
   ESPHome configs, Node-RED flows, OpenWB/evcc sources. These encode maps someone
   verified against a live device — but check *which model generation* they tested.
   The SPH profile in this repo started as a transcription of one of these
   (`profiles/growatt/sph.toml` documents its sources in comments; do the same).

If maps disagree — they will — record both candidates and settle it on hardware (2b).

### 2b. Probe the device

You need: a USB-RS485 adapter (a few euros), two wires to the inverter's RS485/COM
port, and [`mbpoll`](https://github.com/epsilonrt/mbpoll) (or `modpoll`). **Reads are
non-destructive**; do not write anything (`mbpoll` writes when given data arguments —
only ever use it in read mode here).

```console
# 20 input registers (function 04) from address 0, unit id 1, 9600 8N1:
$ mbpoll -m rtu -a 1 -b 9600 -P none -t 3 -r 0 -c 20 -1 /dev/ttyUSB0

# The same range as holding registers (function 03):
$ mbpoll -m rtu -a 1 -b 9600 -P none -t 4 -r 0 -c 20 -1 /dev/ttyUSB0
```

Notes that save an afternoon:

- **No reply at all?** Try the other common baud rate (9600 ↔ 115200), unit id 1 vs 3,
  and check A/B wiring polarity (swapping the two wires is harmless and fixes silence
  more often than anything else).
- **Off-by-one:** some tools number registers from 1, protocol addresses start at 0.
  `mbpoll -r` uses 1-based PDU numbering by default (`-0` switches to 0-based) — our
  profiles always use **0-based protocol addresses**.
- **"Illegal data address" exceptions** just mean that range does not exist on this
  firmware; probe elsewhere. The bridge handles this the same way (skipped block).
- If a datalogger/dongle is attached, unplug it while probing — two masters on one
  RTU bus corrupt each other's frames.

### 2c. Identify the values

With the device running (ideally: sun on the panels, battery moving), compare raw
registers against the **inverter's own display or app**:

- **Find a known value.** Display says 3.47 kW → look for `34700` (scale 0.1 W),
  `3470` (scale 1 W) or a 32-bit pair decoding to one of those. Grid voltage ≈ 230 V →
  `2300` at scale 0.1 is unmistakable.
- **32-bit values** occupy two consecutive registers, almost always **high word
  first**: `value = reg[n] * 65536 + reg[n+1]`. If a power reading looks absurdly huge
  or jumps wildly, you are probably reading one half of a pair, or the word order is
  swapped (the profile format currently supports high-word-first only — if your device
  is genuinely low-word-first, open an issue).
- **Signed values:** anything that can flow both ways (battery power, grid
  import/export) or go below zero (temperature). A raw value near 65535 that "should"
  be small and negative is a signed 16-bit (`65535` = −1). Use `s16`/`s32`.
- **Watch it change.** Poll the same range at different output levels and different
  times of day. A register that tracks the display through change after change is
  confirmed; a register that happens to match once is a coincidence.
- **Record confidence per register** (high/medium/low + source) — it goes into the
  profile comments verbatim.

## 3. Writing the profile

1. Copy [`profiles/_template.toml`](../profiles/_template.toml) to
   `profiles/<family>/<your_device>.toml`. Field reference:
   [device-profiles/schema.md](device-profiles/schema.md); allowed measurement ids:
   [device-profiles/canonical-measurements.md](device-profiles/canonical-measurements.md).
2. Declare **wide read blocks** during bring-up (e.g. the whole 0–124 base range), not
   just the registers you mapped: the driver TRACE-dumps every raw block, which is how
   you verify and correct the map later. Narrow them once confirmed.
3. Map **only registers you have at least medium confidence in**. The project rule is
   *never invent a reading*: an unmapped channel shows up as absent, which is honest; a
   wrongly mapped one shows up as authoritative data, which is worse. Leave the shaky
   rows as comments (`# candidate: reg 1042 = grid power? scale unclear`) until proven.
4. Validate and run the host tests:

   ```console
   $ python3 tools/gen_profiles.py --check
   $ pio test -e native
   ```

   The generator rejects unknown measurement ids, unknown units, registers outside the
   declared blocks, duplicate mappings, out-of-range blocks, and a missing/ambiguous
   default profile — with the file and entry named in the message.

## 4. Testing against the real device

1. Build and flash (`pio run -e waveshare-rs485-can`, or the env for your board — the image contains all
   drivers), or OTA-upload the `.bin` if a bridge is already installed.
2. Select the driver and, if not the default, your profile in the bridge web UI
   (driver option `profile = <your id>`).
3. Set log level to `trace` and watch `/api/v1/logs`: the `GROWATT in <addr>: ...`
   lines are the raw register dump. Verify each mapped register against the device
   display **at that moment**.
4. Check the published values: `/api/v1/status`, MQTT, Home Assistant. Watch a full
   day if you can — sunrise, full sun, and (for hybrids) charge→discharge crossover,
   where sign conventions reveal themselves. `battery.power` must be **positive while
   charging** (our convention; negate-on-map is not supported yet, so if your device
   reports it inverted, note it and open an issue).

### Before opening the PR

- [ ] Every mapped value matches the device display (within scale rounding).
- [ ] Values you cannot read are absent, not mapped-and-wrong. Nothing publishes 0 for
      "unknown".
- [ ] `python3 tools/gen_profiles.py --check` and `pio test -e native` pass.
- [ ] Profile comments state your device model + firmware, sources per register, and
      confidence. Uncertain candidates are comments, not mappings.
- [ ] `docs/` protocol notes updated if you learned something structural (register
      generations, quirks) — see `docs/growatt-sph-protocol.md` for the level of detail
      that has paid off.

## 5. Handshake protocols (codecs)

If your device is not plain Modbus, the register-research above still applies in
spirit, but the implementation is a C++ protocol codec plus a driver. The path that
worked for the EverSolar driver:

1. **Capture real traffic first.** A passive RS485 tap while the official
   software/dongle talks to the device, or replaying a community implementation's
   sequence. Decode frame by frame before writing any code
   (`tools/decode_eversolar.py` is the working example of such a decoder).

   **The bridge can do the tap itself** — see [§6](#6-capturing-an-unknown-device) below.
   It is already wired to the bus, so you need no USB-RS485 adapter and no laptop within
   cable reach of the inverter.
2. Read `docs/eversolar-protocol.md` and `src/drivers/eversolar_legacy/` as the
   reference structure: framing/checksum in a parser (host-tested), sequencing in the
   driver, brand knowledge nowhere else.
3. Open an issue early with your captures. Protocol sequencing has failure modes that
   only show on real hardware over days (our sunrise-recovery saga is the cautionary
   tale), so plan for a soak-test phase.

## 6. Capturing an unknown device

When the discovery wizard finishes and names nothing, it offers **"Record the raw bus"**.
That is this feature, and it needs no working driver — which is the point, because the
devices worth capturing are exactly the ones nothing here can talk to yet.

It listens. The bridge transmits nothing for the whole window; on a protocol you do not
understand, a stray write is the one thing that could actually disturb the device.

**Make the other end talk while it runs.** A passive tap records nothing on a quiet bus.
Start the vendor app, plug in the monitoring dongle, press whatever makes it poll — or
just wait, if something already polls on a cycle.

### Read the checksum count first

The report leads with how many records carried a valid checksum, and that number answers
the question that actually blocks you:

| What you see | What it means |
|---|---|
| Bytes, and valid checksums | Right line speed. Go decode. |
| Bytes, **no** valid checksums | Almost always the **wrong baud rate** — try the next one. Failing that, a protocol that is neither Modbus RTU nor AA55, in which case the hex is still exactly what you want. |
| No bytes at all | Either nothing spoke while it ran, or the wiring is wrong. A/B swapped is the usual one, then termination. |

The device's baud rate is part of what you do not know yet, so guess and iterate. 9600 is
by far the most common; then 19200, then 115200.

### What comes out

A text file you can attach to an issue directly:

```
# Heliograph raw RS485 capture
# line: 9600 baud, none parity, 8 data bits, 1 stop bits
# frame boundaries cut on 4 ms of silence
# 12 frames, 143 bytes, 12 valid Modbus CRC, 0 valid AA55
#
# time_ms  gap_ms  len  checksum  bytes
       0       0    8  modbus    01 03 00 00 00 0A C5 CD
      41      33   25  modbus    01 03 14 00 00 09 C4 ... 
```

**Write down what the device is and what was talking to it while this ran.** That context
is the one part nobody can recover from the bytes, and it is the difference between a
useful capture and a wall of hex.

### One honest limit: framing above 19200

There is no request/response structure to lean on the way a driver has, so records are cut
on an idle gap — the Modbus t3.5 rule, 3.5 character times of silence. At 9600 and 19200
that gap is 4 ms and 2 ms, comfortably wider than the read loop's resolution.

**At 38400 and above the real gap is under a millisecond**, finer than anything polling a
UART can observe, so adjacent frames may land in one record. The byte stream is still
complete and in order; only the cut points are approximate. A merged record shows up as a
failed checksum rather than as silence — so on a fast line, judge the capture by the hex,
not by the checksum count.

### Bounds worth knowing

- Polling stops for the whole window, and the bus is held exclusively.
- Maximum 300 seconds per run, 256 records, 256 bytes per record — and **12 KB of captured
  bytes in total**, which is the bound that actually bites first. The other two are a product,
  and a product of two independently chosen limits bounds nothing: 256 × 256 would render to
  about 220 KB of hex, which no board here can hold.
- **Full means stopped, not "oldest dropped".** For a handshake the interesting part is the
  registration dance at the beginning, and a ring buffer would reliably discard exactly
  that. A truncated capture says so.

## Why writes are dormant

Writing to a hybrid's holding registers moves real energy with real money and real
warranty attached, on the basis of a register map that — see above — starts life as a
forum post. So the schema treats writes as *research to record*, not behavior to enable:

- Every register is **read-only by default**. A `[[write]]` row (see
  [schema.md](device-profiles/schema.md)) documents a writable setpoint register with
  mandatory min/max bounds — include them in your PR when your protocol PDF documents
  them, marked `verified = false`.
- Nothing acts on such a row until it is `verified = true` (confirmed against the real
  device, on a bench, by someone watching the inverter respond) **and** the driver has
  grown a write path in C++. Both gates are deliberate; a data file that could make an
  unreviewed device writable is not a feature but a liability.
