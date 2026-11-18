# EverSolar fault and status decoding — design note, 2026-11-18

**Nothing here is built.** This is Phase 0: a design and open questions, written before any
code. Phase 1 ships only what is documented and safe; Phase 2 decodes error bits once a real
fault frame exists.

Every claim carries one label:

- **documented** — stated in a vendor document;
- **observed** — seen in real frames from our TL3000-20, or from a named community project;
- **hypothesis** — our reasoning, to be checked. A hypothesis is never shipped as a decoded value.

The Eversolar PMU protocol V1.1 PDF is a scanned image whose text cannot be extracted (see
[the control plan](eversolar-control-plan-2026-11-17.md)). Facts marked documented from it come
from the brief's manual reading of Tables 3-6 and 3-7 and data codes `0x78`–`0x7F`. The PDF is
not committed and not quoted.

## Goal

Show in Home Assistant, REST and Prometheus *that* the inverter is in a fault state, and record
the raw frames that will later tell us *which* fault. Nothing about the fault bits is decoded
until a faulted inverter has shown us what they look like.

## What is new since the brief: the tail is self-described

The brief treats the order of the tail (words 14–21 of the `11 82` reply) as a hypothesis. The
control plan's Phase 1 run (2026-11-18) settles most of it.

**Observed.** The TL3000-20's read description (`11 00`) lists 22 data codes:

```
00 0D 01 04 41 42 43 44 45 47 48 49 4A 4C 78 79 7A 7B 7C 7D 7E 7F
```

and its healthy normal-info reply (`11 82`) carries exactly 22 words (44 bytes). Laid side by
side with the word map the driver already uses
([eversolar-protocol.md](eversolar-protocol.md#layout-with-1-string-14-words--28-bytes-payload)):

| Word | Code | Known as | Word | Code | Known as |
|---|---|---|---|---|---|
| 0 | `00` | TEMP | 11 | `49` | NA_2 |
| 1 | `0D` | E_TODAY | 12 | `4A` | HOURS_UP |
| 2 | `01` | VPV | 13 | `4C` | **OP_MODE** |
| 3 | `04` | IPV | 14 | `78` | tail |
| 4 | `41` | IAC | 15 | `79` | tail |
| 5 | `42` | VAC | 16 | `7A` | tail |
| 6 | `43` | FREQUENCY | 17 | `7B` | tail |
| 7 | `44` | PAC | 18 | `7C` | tail |
| 8 | `45` | IMPEDANCE | 19 | `7D` | tail |
| 9 | `47` | E_TOTAL_HI | 20 | `7E` | tail |
| 10 | `48` | E_TOTAL_LO | 21 | `7F` | tail |

**Hypothesis, now a strong one:** the reply is positional, one word per listed code, in list
order. The count matches exactly, and nothing in the known part contradicts it. With the
documented meaning of `0x78`–`0x7F`, the tail reads:

| Word | Code | Field (documented) | Unit | Healthy value (observed) |
|---|---|---|---|---|
| 14 | `78` | GV fault value (grid voltage at fault) | 0.1 V | `0000` |
| 15 | `79` | GF fault value (grid frequency at fault) | 0.01 Hz | `0000` |
| 16 | `7A` | GZ fault value (grid impedance at fault) | 0.001 Ω | `FFFF` |
| 17 | `7B` | Tmp fault value (temperature at fault) | 0.1 °C | `0000` |
| 18 | `7C` | PV1 fault value (PV voltage at fault) | 0.1 V | `0000` |
| 19 | `7D` | GFCI fault value (residual current at fault) | 0.001 A | `0000` |
| 20 | `7E` | Error message H | bitmask | `FF00` |
| 21 | `7F` | Error message L | bitmask | `0000` |

This moves the brief's question from "which words are the error words" to "what do the bits in
words 20 and 21 mean on this firmware". The conflict stays exactly as the brief puts it. Read as
the V1.1 bitmask, `FF00 0000` sets bits 31–24 on a healthy inverter. timretout/zeversolar-esphome
sees the same `FF00 0000` on a Zeverlution 2000S and 3680. H1–H3 from the brief remain open, and
Phase 1 does not decode a single bit.

The `11 00` codes are **measurement** codes. The writable numbers `40 41 44`–`49` from `11 01`
are a separate setting table: `0x44` there is not PAC.

## What exists in the repo (observed in the code)

- **Status:** `DeviceState` has `statusCode`, `statusText`, `errorCode` (uint32) and their
  `…Supported` flags (`src/device/device_state.h`). `writeDeviceStatus` publishes them to MQTT and
  REST as `null` unless supported and fresh (`src/outputs/json_util.h`). HA gets one diagnostic
  status sensor showing `status_text`. Prometheus publishes no device status.
- **EverSolar today:** `statusCode` is the raw mode word. `opModeText()` maps only 0 and 1, the
  two **observed** values. Everything else is `Unknown (n)`. `errorCodeSupported = false`. Words
  14–21 are never read.
- **No generic alarm model.** The nearest things are `status_*`/`error_code` above, the profile
  `[status_register]` value table, and `MeasurementType::Flag`, which becomes an HA
  `binary_sensor`. Discovery already sets a device class per flag id
  (`grid_connected` → `connectivity`). No driver produces a flag yet.
- **`error_code` means "0 is healthy".** SolaX puts its error bits there, and Modbus TCP clamps it
  to 16 bits. EverSolar's healthy `0xFF000000` would read as an error in both. **So the tail words
  do not go into `errorCode`.**
- **Capture:** the passive capture and the driver capture are RAM-only and started by hand. The
  breadcrumbs are 16 bytes in `RTC_NOINIT`. They survive a panic, watchdog or software reset, but
  not a power loss.

## Phase 1: what ships (documented and safe)

1. **Mode text.** `opModeText()` gains 2 → `Fault` and 3 → `Permanent fault` (documented,
   Table 3-6). 0 and 1 keep their observed texts. Anything else stays `Unknown (n)`. This leaves
   the old rule in `eversolar-protocol.md`, which allowed texts only for observed codes. The brief
   allows documented ones. The doc records which codes are documented and which are observed.
2. **Problem flag.** A new `Flag` measurement, `inverter.fault`, is true when mode is 2 or 3 and
   false when mode is 0 or 1. On an unknown mode it is absent, never false. Discovery gives it
   `device_class: problem`, the same way `grid_connected` gets `connectivity`.
3. **Error words, raw.** Words 20 and 21 are parsed and kept, but not decoded. They are published
   as two hex strings in the REST device payload and in the fault log (point 5). Not in
   `error_code`, for the reason above.
4. **Baseline flag.** `inverter.error_words_changed` (a `Flag`) is true when words 20–21 differ
   from a baseline. The baseline is learned at runtime, per device, from the first mode-1 frame
   after the device registers. It is not hard-coded to `FF00 0000`. It is RAM only and
   re-learned after every boot. Until a baseline exists, the flag is absent.
   *Known limit:* a warning bit already set at boot becomes the baseline. Point 5 also records
   the baseline frame, so that case stays visible.
5. **Fault log.** A record is made for:
   - every change of the mode word into a value other than 0 or 1, so an unknown mode is
     recorded as well as 2 and 3 (`status_changed`);
   - every change of words 20–21 (`error_words_changed`);
   - the baseline frame, once per distinct baseline per device (`baseline`).

   Each record is one log line plus a ring entry: time, mode, and the full raw `11 82` frame in
   hex. Reading the log adds words 14–19 decoded with V1.1 scaling and marked `unverified`; only
   the raw frame is stored. The ring holds N entries and is readable over
   `GET /api/v1/fault-frames`. The log, the driver hook that feeds it and the endpoint are
   generic: brand names stay inside the driver directory. N and persistence: see question 3.
6. **Fault values.** They are recorded only in the fault log, not published as measurements. Six
   HA entities that sit at "unknown" except during a rare fault would be noise. If you want them
   as entities after all: question 5.
7. **Prometheus.** A `heliograph_device_status_code{device=…}` gauge, absent when the status is
   unsupported or stale, and `inverter.fault` through the existing measurement path.
8. **Tests.**
   - Replace the generator's constructed `11 00` fixture with today's observed reply.
   - The real healthy capture stays as it is.
   - Constructed frames for mode 2 and 3 with changed words 20–21. They come from
     `tools/gen_fixtures.py` and are labelled CONSTRUCTED.
   - The stale "zeros and `0xFFFF`" comments in `eversolar_parser.h` and `gen_fixtures.py` get
     corrected.

Not in Phase 1: any bit decode, any LCD-name table in code, dual-MPPT or three-phase tails. The
dual-MPPT layout has seven tail values according to timretout (hypothesis), and our only
dual-MPPT data is constructed. Phase 1 reads the tail only from a 44-byte single-MPPT reply. Other
layouts keep today's behaviour.

## Phase 2: decode, once a real fault frame exists

The first test (below) is meant to produce the frame. Then:

- choose between H1, H2 and H3 on the frame itself;
- add the LCD table with a verified mark per code that has been observed;
- update `eversolar-protocol.md`.

## LCD error codes (documented, for Phase 2)

Checked against the gold-coast-solar-power-solutions summary of the Zeverlution manual
(2016-08-23). That page covers only the Zeverlution, so it is family evidence for the TL3000-20,
not unit evidence.

| Code | Message | Type | Code | Message | Type |
|---|---|---|---|---|---|
| 1 | SCI fault | permanent | 33 | Fac fault | resumable |
| 2 | EEPROM R/W fault | permanent | 34 | Vac fault | resumable |
| 3 | Relay-check fault | permanent | 35 | No utility | resumable |
| 4 | DC injection high | permanent | 36 | Residual current | resumable |
| 5 | *(no description)* | permanent | 37 | PV overvoltage | resumable |
| 6 | High DC bus | both | 38 | ISO fault | resumable |
| 8 | AC HCT fault | permanent | 40 | Over temperature | resumable |
| 9 | GFCI fault | permanent | 41–45 | Vac / Fac / residual current / DC injection / Fac+Vac differ for M-S | resumable |
| 10 | Device fault | — | 48 | Average voltage over 10 minutes | resumable |

Corrections to the brief: **11** (M-S version unmatched) and **39** (fan lock) are not on that
page, so they need another source before Phase 2 uses them. **5** is on it, in the permanent
fault row, without a description.

## Hardware test: the first fault frame

As in the brief: in daylight, switch off the inverter's AC breaker. The inverter should show
code 35, "No Utility". Leave it off for 1–2 minutes, then switch it back on. Expected: mode 2 and
words 20–21 away from the baseline. Two things to record beside the frames:

- Does the inverter keep answering RS485 with the AC off? This is open. If it falls silent, the
  bridge marks the device offline, and that is a result too.
- Does the **bridge** stay powered? If it hangs on the same group, the ring is lost with it unless
  it is persisted (question 3).

The same frames also show the mode word during the fault. That is the first observation of mode
2 on this unit, and it is what moves `Fault` from documented to observed.

## Open questions

1. **Generic alarm model.** There is none (see above). Proposal: status text and error code stay
   as they are, plus two `Flag` measurements (`inverter.fault`, `inverter.error_words_changed`).
   No new generic model for one driver. OK?
2. **Status as an HA enum?** Proposal: **no**, keep the existing `status_text` sensor. An HA
   `enum` sensor needs a fixed `options` list, and HA rejects any value outside it. `Unknown (n)`
   is exactly such a value.
3. **Fault log size and persistence.** Proposal: N = 8, in `RTC_NOINIT` like the breadcrumbs. That
   survives a crash or OTA restart for about 600 bytes, but not a power loss. It is enough as long
   as the bridge has its own power during the AC-off test. Is it powered separately from the
   inverter's AC group? If not, it needs NVS, written only on a record and rate-limited.
4. **Problem binary sensor in Phase 1?** Proposal: **yes**, `inverter.fault` from the mode word
   (documented), never from the error bits.
5. **Fault values as entities?** Proposal: **no**, only in the fault log (Phase 1, point 6). This
   deviates from the brief, which publishes them while mode is 2 or 3.

## Answers (2026-11-18)

All five proposals are accepted. The maintainer set one condition above the rest: **the Home
Assistant integration must keep working.** For Phase 1 that means:

- The change is additive. No existing entity, unique id, topic or state key changes.
- The texts for mode 0 and 1 stay byte-identical, because automations may match on them.
- Home Assistant gets two new binary sensors and nothing else.
- An absent value is published as `null`. Home Assistant then shows "unknown", never "off" or
  "OK".

Refinements made while turning the note into a build brief:

- **Baseline records.** The bridge restarts on every deploy, several times a day. One baseline
  record per boot would fill an 8-slot ring with baselines and push out the fault frames. A
  baseline is therefore stored once per distinct value per device.
- **Persistence (question 3).** If the bridge shares the inverter's AC group, it is off during
  the AC-off test and records nothing, whatever the storage. So `RTC_NOINIT` is enough, and the
  bridge's power source is a thing to check before the test, not a storage decision.
- **One record per frame.** When the mode and the error words move in the same reply, that is
  one `status_changed` record. Two copies of one frame would only cost a slot.
- **Frame size.** A stored frame is up to 80 bytes, so a reply behind an echo of the request
  (66 bytes on a half-duplex bus) keeps its checksum.
