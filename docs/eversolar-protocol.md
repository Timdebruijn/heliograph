# EverSolar / Zeversolar legacy RS485 protocol

Status: reconstructed from the reference implementation; **since 2026-07-19 partially verified
against a real TL3000-20** (Phase 3, first contact). Hardware findings are marked as such;
two captured frames are stored byte-for-byte in `tools/gen_fixtures.py`, which
verifies itself against the captures. Where something is unknown, that is stated explicitly — no
fields have been invented.

## Origin and license

| | |
|---|---|
| Reference | <https://github.com/solmoller/eversolar-monitor> |
| Commit | `784c2fc2f6b6ed4e70bf7519d0939ec1195a1813` (2026-02-21) |
| License | **MIT** — © 2021 Henrik Møller Jørgensen |
| Original basis | Steve Cliffe `<steve@sjcnet.id.au>`, Eversolar PMU logger |

MIT is permissive; reuse of protocol knowledge and reimplementation in C++ is allowed provided
the copyright notice and license text are retained. Concretely:

- `LICENSE-THIRD-PARTY.md` in the repo root gets the full MIT text of eversolar-monitor.
- Every file in `src/drivers/eversolar_legacy/` gets a header stating that the
  protocol knowledge is derived from eversolar-monitor (MIT) and from Steve Cliffe's PMU logger.
- **No** Perl code is translated or copied; only the protocol is
  reimplemented. The application structure (sqlite, pvoutput, FTP upload) is out of scope.

## Physical layer

From `serial_connect()` (`eversolar.pl:559-576`) — hardcoded, not configurable:

| Parameter | Value |
|---|---|
| Baud rate | **9600** |
| Data bits | 8 |
| Parity | none |
| Stop bits | 1 |
| Handshake | none |

This is the **only** relevant serial profile for this driver. The driver descriptor therefore
specifies exactly one `SerialProfile`: `9600 8N1`. No other baud rates are tried.

## Frame format

From the header comment (`eversolar.pl:102-110`) and `send_request()` (`:395-408`):

```
Offset  Length  Field
------  ------  --------------------------------------------------
0       2       Header: 0xAA 0x55
2       2       Source address
4       2       Destination address
6       1       Control code
7       1       Function code
8       1       Data length N
9       N       Data
9+N     2       Checksum (uint16, big-endian)
------  ------  --------------------------------------------------
Total: 11 + N bytes
```

### Addressing

The PMU (our bridge) is the master. From `send_request()`:

```perl
@tmp_packet = ( 0xAA, 0x55, OUR_ADDRESS, 0x00, 0x00, $destination_address, ... );
#                           ^^^^^^^^^^^^^^^^^  ^^^^^^^^^^^^^^^^^^^^^^^^^^
#                           bron = 0x01 0x00   bestemming = 0x00 <addr>
```

| | Value |
|---|---|
| PMU source address | `0x01 0x00` (`OUR_ADDRESS = 0x01`, `eversolar.pl:268`) |
| Broadcast destination | `0x00 0x00` |
| Inverter destination | `0x00 <assigned address>` |
| First assigned address | `0x10` (`START_INVERTER_ADDRESS`, `:267`) |

The PMU assigns addresses itself, incrementing from `0x10`. This is therefore **not** a fixed
slave address like in Modbus: the address is the result of a registration procedure.

### Checksum

From `send_request()` (`:403-406`) and `validate_checksum()` (`:750-760`):

```
checksum = (sum of all bytes from offset 0 through 8+N) mod 65536
```

A **simple 16-bit sum over all preceding bytes, including the 0xAA 0x55 header**,
written big-endian. No CRC-16, no Modbus CRC.

The reference adds one validation (`:759`):

```perl
return ( $csum > 0 ) && ( $csum == ( ( $packet[$len-2] << 8 ) + $packet[$len-1] ) );
```

> "packets with all zeroes pass checksum test, but are invalid"

A frame of all zeros has checksum 0 and would therefore appear valid — precisely the case that
would otherwise be published as a "valid zero reading".

**We deliberately do not adopt this `csum > 0` guard.** It only exists because the reference
never checks the `AA 55` header anywhere. We do check it, and as a result the byte sum of a
valid frame is always at least `0xAA + 0x55 = 255`: an all-zero frame already fails earlier, on
a stronger ground (`BadHeader`).

Worse, the guard is slightly wrong: with a real header the sum can still come out to exactly
65536 and truncate to 0. That is then a correct checksum, and the guard would incorrectly
reject such a frame — roughly 1 in 65536 frames. Both cases are captured as a test in
`test_eversolar_checksum`.

## Control and function codes

From `%CTRL_FUNC_CODES` (`eversolar.pl:190-223`):

| Control | Function | Name | Request → Response |
|---|---|---|---|
| `0x10` REGISTER | `0x00` | Offline query | `10 00` → `10 80` |
| `0x10` REGISTER | `0x01` | Send register address | `10 01` → `10 81` |
| `0x10` REGISTER | `0x04` | Re-register | `10 04` → *(no response)* |
| `0x11` READ | `0x00` | Query description | `11 00` → `11 80` |
| `0x11` READ | `0x02` | Query normal info | `11 02` → `11 82` |
| `0x11` READ | `0x03` | Query inverter ID | `11 03` → `11 83` |
| `0x12` WRITE | — | *empty in reference* | — |
| `0x13` EXECUTE | — | *empty in reference* | — |

Responses set bit 7 of the function code (`0x02` → `0x82`).

**`0x12` WRITE and `0x13` EXECUTE are empty in the reference.** There is therefore no known
write operation for this inverter at all. This fits seamlessly with the read-only MVP: the
EverSolar driver returns `Unsupported` for every command, and that is not a temporary
limitation but a property of what we know about this protocol.

## Registration procedure

All registration traffic goes to broadcast (`0x00`).

```
1. RE_REGISTER (10 04) → broadcast, repeated 8x, no response expected
   eversolar.pl:582-594. The comment says "3 times as per protocol specs",
   the code sends 8. All inverters forget their registration.

2. OFFLINE_QUERY (10 00) → broadcast
   Response (10 80): serial number as ASCII bytes, variable length.
   eversolar.pl:602-608

3. SEND_REGISTER_ADDRESS (10 01) → broadcast
   Data = <serial-number bytes> + <1 byte address to assign>
   Response (10 81): 1 byte ACK. Expected: 0x06 (ASCII ACK).
   eversolar.pl:610-620

4. QUERY_INVERTER_ID (11 03) → <assigned address>
   Response (11 83): ASCII identification string.
   eversolar.pl:625-628
```

After that, `QUERY_NORMAL_INFO` (`11 02`) is sent periodically to the assigned address.
The reference repeats steps 2-4 every minute to find new inverters (`:1037-1040`).

**All four steps are read-only.** Nothing is configured in the inverter except for
a volatile bus address, which the inverter itself forgets when it loses power. This procedure
is therefore safe as a probe (see `docs/architecture.md`, discovery).

## Measurement data — `QUERY_NORMAL_INFO` (`11 02` → `11 82`)

The payload is an array of **16-bit big-endian words** (`parse_packet()`, `:766-777`):

```perl
$data[$j++] = ( $packet[$i] << 8 ) + $packet[$i+1];
```

The indices below are therefore **word indices**, not byte offsets.

### Layout with 1 string (14 words = 28 bytes payload)

`eversolar.pl:226-241`

| Word | Field | Type | Scale | Unit | Measurement ID |
|---|---|---|---|---|---|
| 0 | TEMP | **int16** | ÷10 | °C | `inverter.temperature` |
| 1 | E_TODAY | uint16 | ÷100 | kWh | `energy.today` |
| 2 | VPV | uint16 | ÷10 | V | `dc.mppt_1.voltage` |
| 3 | IPV | uint16 | ÷10 | A | `dc.mppt_1.current` |
| 4 | IAC | uint16 | ÷10 | A | `ac.phase_l1.current` |
| 5 | VAC | uint16 | ÷10 | V | `ac.phase_l1.voltage` |
| 6 | FREQUENCY | uint16 | ÷100 | Hz | `ac.frequency` |
| 7 | PAC | uint16 | **×1** | W | `ac.power.total` |
| 8 | IMPEDANCE | uint16 | ? | ? | *not published, see below* |
| 9 | E_TOTAL_HI | uint16 | — | — | *high word of energy.total* |
| 10 | E_TOTAL_LO | uint16 | ÷10 | kWh | `energy.total` |
| 11 | NA_2 | uint16 | ? | ? | *unknown, not published* |
| 12 | HOURS_UP | uint16 | ×1 | h | `inverter.operating_hours` |
| 13 | OP_MODE | uint16 | — | — | `status_code` |

### Layout with 2 strings (16 words = 32 bytes payload)

`eversolar.pl:244-261`

| Word | Field | Measurement ID |
|---|---|---|
| 0 | TEMP | `inverter.temperature` |
| 1 | E_TODAY | `energy.today` |
| 2 | VPV | `dc.mppt_1.voltage` |
| 3 | VPV2 | `dc.mppt_2.voltage` |
| 4 | IPV | `dc.mppt_1.current` |
| 5 | IPV2 | `dc.mppt_2.current` |
| 6 | IAC | `ac.phase_l1.current` |
| 7 | VAC | `ac.phase_l1.voltage` |
| 8 | FREQUENCY | `ac.frequency` |
| 9 | PAC | `ac.power.total` |
| 10 | NA_0 | *unknown* |
| 11 | E_TOTAL_HI | *high word* |
| 12 | E_TOTAL_LO | `energy.total` |
| 13 | NA_2 | *unknown* |
| 14 | HOURS_UP | `inverter.operating_hours` |
| 15 | OP_MODE | `status_code` |

Note: the order of `IAC`/`VAC` and the position of `PAC` differ between the two layouts.
A wrong layout choice therefore produces plausible-looking but completely wrong values.

### Layout auto-detection — and the hypothesis behind it

The reference lets the user choose the layout via an ini option (`strings = 1` or `2`,
`eversolar.ini:8`) and `die`s on any other value. We instead derive the layout from the
payload length:

| Payload length | Words | Layout |
|---|---|---|
| 28 bytes | 14 | 1 string |
| 32 bytes | 16 | 2 strings |
| otherwise | — | **discard frame**, do not publish data |

**This is a hypothesis, not an established fact.** It follows from the two index maps of 14
and 16 entries in the reference. But there is counter-evidence: if the length unambiguously
determines the layout, why is it then a config option?

Two explanations, and we don't yet know which one is correct:

1. The author didn't think of it. Plausible: the reference validates almost nothing — no
   header check, no length check, and with a wrong `strings=` Perl silently indexes
   out of bounds (`undef` → 0) instead of failing.
2. The length is not distinguishing and only the interpretation differs. In that case the
   detection is broken.

#### What the TL3000-20 actually sends — hypothesis refuted (2026-07-19)

The real 1-string TL3000-20 sends **44 bytes**: the 14 single-string words followed by an
8-word tail of zeros and `0xFFFF`, meaning unknown. Not 28. The values in the first 14
words have been verified against reality (38.6°C, 346.4 V × 2.0 A DC ≈ 655 W AC,
50.03 Hz, 35,445.9 kWh lifetime — mutually consistent and physically plausible; frame stored
as `kRespNormalInfoCaptured` in the fixtures).

That resolves the question about the config option in the reference: `eversolar.pl` indexes
words and ignores everything after them, so the payload length was never relevant to the
author — and therefore never decisive either. Explanation 2 from the previous revision of this
document was the correct one.

`Auto` now accepts both 28 and 44 as single (44 is hardware truth, not a guess), 32 as dual, and
rejects everything else. Whether 2-string firmware really sends 32 (or 44, or something else)
remains **unvalidatable** on this hardware; the dual path remains constructed. `LayoutSelection`
remains the escape hatch.

#### Escape hatch

Because the hypothesis is unproven, `decodeNormalInfo()` has a `LayoutSelection`:

| Selection | Behavior |
|---|---|
| `Auto` | 28 → single, 32 → dual, otherwise `UnknownLayout` (default) |
| `ForceSingleString` | interprets as 1 string; requires `len >= 28` |
| `ForceDualString` | interprets as 2 strings; requires `len >= 32` |

A forced layout only requires that the payload is long enough, not that the length matches
exactly. This way a device that contradicts the hypothesis remains readable without a firmware
change. A too-short payload gives `LayoutMismatch` — the buffer is never read out of bounds.

This is exposed as a generic driver option, not as a field in the config model:

```json
{ "driver": { "id": "eversolar_legacy", "options": { "layout": "auto" } } }
```

The driver declares the key `layout` with allowed values `auto|single|dual` in its
descriptor; `validateDriverOptions()` validates against that and the web interface renders it
generically. This keeps `Configuration` free of brand-specific fields. In line with §19 "manual
configuration is always possible". See `docs/architecture.md`.

### Scale factors and signedness

- **TEMP is signed.** `eversolar.pl:1059-1061`: `if ($data >= 0x8000) { $data -= 0x10000 }`,
  with comment "Temperature is signed, -0.1 = 0xFFFF". All other fields are treated as
  unsigned.
- **PAC has no scale factor** — the raw word is watts. (`:1058`, `:1097`)
- **E_TODAY is ÷100**, while **E_TOTAL is ÷10**. Different resolutions, easy to get wrong.

### Confirmed bug in the reference: `energy.total`

`eversolar.pl:1057`:

```perl
my $e_total = $data[E_TOTAL] / 10 + $data[NA_1] * 65535 / 10;
```

The high word is multiplied by **65535**. The correct value is **65536** (`1 << 16`). The
correct calculation is a plain uint32:

```
energy.total [kWh] = ((E_TOTAL_HI << 16) | E_TOTAL_LO) / 10.0
```

**Consequence for hardware validation (Phase 3/§42):** our value systematically deviates from
eversolar-monitor by `E_TOTAL_HI × 0.1 kWh`. At a total around 18,452.7 kWh,
`E_TOTAL_HI = 2`, so our reading comes out **0.2 kWh higher**. This is not a bug in our
implementation, and the acceptance criterion "energy exactly within scale factor" must be
adjusted for this: expect a deviation of exactly `HI × 0.1 kWh`, and verify that the
difference is exactly that value.

That `NA_1`/`E_TOTAL` together form a uint32 is a reverse-engineering conclusion of the
original author (comment `:256`: "NA_1 is actually upper bytes of total production"),
not a documented specification. Confidence: **high but to be confirmed** — it can only be
confirmed on hardware once `E_TOTAL_LO` rolls over past `0xFFFF`, or by checking that
`HI` matches the cumulative total on the inverter's display.

### `IMPEDANCE` and `NA_*`

`IMPEDANCE` (word 8, only in the 1-string layout) is logged by the reference but
without a unit or scale factor; presumably the insulation resistance in mΩ or Ω. `NA_0` and
`NA_2` are entirely unknown.

For these fields the rule from the assignment applies: **unknown values are not published as
zero.** They are logged at TRACE level in Phase 3 so that their meaning can potentially still
be determined, but they do not appear in the canonical measurement model as long as unit and
scale are unknown.

## Status codes (`OP_MODE`) — two codes measured, rest unknown

The reference reads `OP_MODE` and publishes it as a raw number, without a value table.
No table is invented here — but two codes have now been **measured** on a real
TL3000-20 (day/night cycle via HA history, 2026-07-19 through 2026-07-22):

| Code | Meaning (measured) | Evidence |
|---|---|---|
| 1 | Grid-connected (normal) | Full production days show code 1; independently confirmed by the calibrated Zeversolar 2000s capture (ha-zeversolar-modbus) |
| 0 | Standby (not feeding) | Four independent events: dusk on July 19/20/21 (last minutes before shutdown show code 0) and dawn on July 22 at 06:01 (four minutes of code 0, then code 1 at first production) |

Mapping in `eversolar_parser.cpp::opModeText()`; every other code honestly remains
`"Unknown (<n>)"` until it has actually been observed (candidate: an error code during a
grid fault).

Approach:
- The MVP publishes `status_code` as a raw uint16 and sets `status_text` to `"Unknown (<n>)"`.
- In Phase 3, the value is logged over a full day (night → startup → production →
  shutdown). The actually occurring codes follow from that sequence.
- Only after that is a mapping added, with the measurement as the source.

The same applies to `error_code`: **the protocol has no separate error code field** that
is read out in the reference. `DeviceState::errorCode` therefore stays at 0 with
`supported = false`; no fictitious field is filled in.

## Response validation

The reference validates minimally (`send_request()`, `:456-467`): only checksum +
control/function code. Source and destination address are not checked, and
`$len = length($response) - 11` assumes there is exactly one frame in the buffer.

Our parser validates more strictly:

| Check | Rule |
|---|---|
| Minimum length | `>= 11` bytes |
| Maximum length | `11 + 255`; buffer capped at 300 bytes |
| Header | `0xAA 0x55` |
| Destination | must be `0x01 0x00` (= us) |
| Source | addressed queries: `0x00 <assigned address>`. **Registration broadcasts: also `0x00 0x00`** — see below |
| Data length | `N` must match the actual frame length |
| Checksum | sum over `[0 .. 8+N]`, and `> 0` |
| Control/function | must be the expected response code |
| Payload length | per function: 28/32/**44** for `11 82` (see layout section) |

**Hardware correction (2026-07-19):** an unregistered inverter answers the offline query from
source `00 00` — it doesn't have an address yet; that is precisely what the query is there to
arrange. The original assumption (answer from the not-yet-assigned address) was a constructed
guess, and the reference couldn't tell us because it doesn't check source addresses at all.
The registration steps therefore accept both sources; the ACK source (`10 81`) has not yet
been observed on hardware (this device registered in one go via the `00 00` path). Addressed
queries (`11 02`, `11 03`) remain strictly on the assigned address — confirmed on hardware:
the normal-info response comes cleanly from `00 10`.

If any check fails, the frame is discarded and `invalid_frame_total` /
`checksum_error_total` is incremented. **No** partial or corrected data is published.

## Timing

The reference does `sleep 1` after each request and then blindly reads 256 bytes
(`:430-446`). That is crude but proves that the inverter responds well within a second.

Our transport layer does it properly: read until the frame is complete (length follows from
byte 8), with a response timeout of **1000 ms** and **3** retries. These values are set in the
driver's `SerialProfile` and are configurable.

## What we still don't know

| Unknown | Impact | Approach |
|---|---|---|
| Does 2-string firmware really send 32 bytes? | Auto-detection could guess wrong | Not validatable on this hardware; `LayoutSelection` as escape hatch |
| Meaning of `OP_MODE` codes | `status_text` remains "Unknown (n)" | Log over a full day, Phase 3 |
| Meaning of `IMPEDANCE`, `NA_0`, `NA_2` | Not published | TRACE log, Phase 3 |
| Format of `QUERY_DESCRIPTION` (`11 00`) | Model info possibly derivable from this | Try it out in Phase 3 (read-only, safe) |
| Format of `QUERY_INVERTER_ID` (`11 03`) | ASCII, structure unknown | Log raw first, then parse |
| Confirmation of uint32 `energy.total` | 0.1 kWh precision | Compare with display |
| Behavior at night | Timeouts vs. error frames | Phase 9 |

For none of these points is code written in Phase 2 that hardcodes an assumption.
