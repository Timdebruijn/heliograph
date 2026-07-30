# Device profile schema

A device profile is one TOML file in `profiles/<vendor>/` describing how to read one
device family over a register-map protocol. At build time `tools/gen_profiles.py`
validates every profile and generates the C++ tables the driver polls from
(`src/drivers/modbus_profile/profiles_generated.cpp` — generated, git-ignored, never
edited). A broken profile fails the **build** with a validation message; nothing invalid
can reach a running device.

Start from [`profiles/_template.toml`](../../profiles/_template.toml). The research
workflow (how to find registers for your device) is in
[adding-a-device.md](../adding-a-device.md).

Validate without building:

```console
$ python3 tools/gen_profiles.py --check
gen_profiles.py: 1 profile(s) valid: sph
```

Files whose name starts with `_` (like the template) are skipped.

## `[profile]` — one per file

| Key | Type | Required | Meaning |
|---|---|---|---|
| `driver` | string | yes | The C++ driver that consumes this profile. Only `"modbus_profile"` is table-driven today; see [Scope](#scope) for what qualifies. |
| `id` | string | yes | Stable lowercase identifier (`[a-z][a-z0-9_]*`), unique across all profiles. Users select it with the driver's `profile` option; treat it as API, never rename it. |
| `display_name` | string | yes | Human-readable model name, e.g. `"Growatt SPH (3-6 kW)"`. Becomes the reported model identity. |
| `manufacturer` | string | yes | The vendor, e.g. `"Growatt"`. One driver serves every brand, so the profile is the only thing that knows this; it is what Home Assistant shows as the device's maker. |
| `status` | string | no (`"experimental"`) | How far **this map** has been proven: `experimental`, `beta`, `stable` or `deprecated`. See [Status](#status). |
| `default` | bool | no (false) | Profile used when the `profile` option is unset. Exactly **one** profile per driver must set this. |
| `phases` | int | yes | AC phases, 1–3. |
| `mppts` | int | yes | MPPT/string inputs, 0–8. |
| `battery` | bool | yes | `true` for hybrids with an attached battery; drives the `ReadBatteryState` capability and battery discovery entities. |
| `transports` | array | no (`["rtu"]`) | Which transports the device family supports: `"rtu"` and/or `"tcp"`. Declaring `"tcp"` is schema-forward: the bridge has no Modbus TCP *client* transport yet, so a TCP-only profile cannot be polled today. |

## Status

The rungs mirror the driver support levels, because "how much should I trust this" has one
answer shape whether it is asked about code or about a table:

| | |
|---|---|
| `experimental` | Transcribed from a vendor document or a mature open-source map. Has never met the device. |
| `beta` | Confirmed against real hardware, not yet run long enough to trust unattended. |
| `stable` | Validated and soak-tested. |
| `deprecated` | Superseded or known wrong. Kept so a stored configuration still resolves to something instead of silently falling back to another family's map. |

**This is the profile's own property and is never inherited from the driver.** One driver reads
every table here, so its `DriverSupportLevel` can only ever describe the least-proven profile in
the build. Without a per-profile answer, the first map confirmed on hardware would promote the
driver and carry every unconfirmed map up with it — a Huawei table that has never met a Huawei,
wearing the same badge as one somebody watched all day. A test asserts that relationship rather
than today's values, so it keeps holding after a promotion.

The default is the **lowest** rung on purpose: a profile that forgets to declare a status must
understate what we know, never overstate it. Declare it anyway — a default nobody reads is a
default nobody revisits.

What promotes a profile is a **register-by-register comparison against the device's own display,
at the same moment**, reported on the issue tracker. Agreement between two written sources is
not confirmation from a device; the word-order defect that cost six Sungrow rows a factor of
65536 sat behind two agreeing sources.

The status reaches the user: it is shown in the profile dropdown next to the model name, emitted
as `allowed_labels` on the driver's `profile` option in `GET /api/v1/drivers`, and carried in
the Status column of [the coverage matrix](../drivers/coverage.md).

## `[serial]` — optional

The RS485 line settings this device family actually ships with. Omit when unsure: the
driver descriptor's generic candidates (which discovery tries) then apply.

| Key | Type | Required | Meaning |
|---|---|---|---|
| `baud` | int | yes | One of the standard rates (2400–115200). |
| `parity` | string | no (`"none"`) | `"none"`, `"even"` or `"odd"`. |
| `stop_bits` | int | no (1) | 1 or 2. |

## `[tcp]` — optional, requires `"tcp"` in `transports`

| Key | Type | Required | Meaning |
|---|---|---|---|
| `port` | int | no (502) | Modbus TCP port. |

## `[[block]]` — 1 to 8 per file

The contiguous register ranges the driver reads each poll cycle, one Modbus transaction
per block.

| Key | Type | Required | Meaning |
|---|---|---|---|
| `space` | string | yes | `"input"` (function 04) or `"holding"` (function 03). |
| `start` | int | yes | First register, 0–65535. |
| `count` | int | yes | Registers in the block, 1–125 (the Modbus per-read limit). |
| `probe` | bool | no (`false`) | This block exists to answer a question, not to feed a measurement. See below. |

Rules enforced by the build:

- at most **8** blocks (the driver's scratch-buffer limit);
- `start + count` must stay inside the 16-bit register address space;
- every mapped register must be covered by a block (including the second word of a
  32-bit value);
- a mapped register may **not** live only inside a `probe` block.

A block the device refuses with a Modbus exception is skipped at runtime, not fatal —
deliberately, so a profile may probe ranges that only exist on some firmware generations
and the TRACE dump shows which ones this unit actually has.

### `probe = true`

Mark a block that maps nothing and exists only so the raw TRACE dump answers a question — most
often "which register generation does this model speak?". Two things follow:

- its read failures are **excluded from the RS485 bus counters** (`heliograph_rs485_*_total`);
- mapping a measurement into it becomes a build error, because the exclusion would then hide
  real bus errors on a range the profile depends on.

The exclusion is the point. A Modbus device *should* answer an unknown range with an exception,
which was never counted as a bus error — but nothing forces it to, and a unit that answers with
silence instead would otherwise add a timeout to the metrics on **every poll**, forever, on an
installation with nothing wrong with it. A probe block's silence is a fact about the register
map, not about the wire. See [prometheus.md](../prometheus.md).

## `[[register]]` — 1 or more per file

One canonical measurement fed by one register (or register pair).
Decoded as `value = raw * scale + offset`, after sign extension for `s16`/`s32`.

| Key | Type | Required | Meaning |
|---|---|---|---|
| `measurement` | string | yes | Canonical id from [canonical-measurements.md](canonical-measurements.md). Each id may be mapped at most once per profile. |
| `display_name` | string | yes | Human name for dashboards/Home Assistant. |
| `space` | string | yes | `"input"` or `"holding"`. |
| `address` | int | yes | First register. A 32-bit type also reads `address + 1`; the **high word comes first** by default (what nearly every Modbus inverter does). Set `word_order` when a source says otherwise. |
| `type` | string | yes | `u16`, `s16`, `u32`, `s32`. `s*` is two's-complement signed — use it for anything that can be negative (power that can flow both ways, temperatures). |
| `scale` | number | no (1.0) | Multiplier for the raw integer. A device reporting tenths uses `0.1`. Must not be 0. **May be negative** — see below. |
| `offset` | number | no (0.0) | Added after scaling. For registers that store a *biased* value so it never goes negative on the wire: several vendors report `1000` for 0 °C, which is `scale = 0.1, offset = -100`. |
| `word_order` | string | no (`"high_first"`) | 32-bit values only. `"low_first"` when the device stores the LOW half at the lower address. Refused on a 16-bit row and on write rows. |
| `invalid` | int | no | A raw value the device uses to mean "not available" (e.g. `0x7FFF`, `0xFFFF`). A matching register is left **undeclared** rather than published. Compared before sign extension; must fit the register width. Refused on write rows. |
| `unit` | string | yes | One of `W` `V` `A` `Hz` `°C` (or `C`) `kWh` `h` `%` `dBm` `s`. The measurement *type* (Power, Voltage, …) is derived from the unit, so you never touch internal enums. |

### Sentinels: when a device says "unknown" in numbers

Some vendors do not answer an unavailable reading with an exception or a zero — they answer with
a fixed pattern, one per register width. Decoded as a number that is not obviously wrong: an
inverter asleep for the night reports 3276.7 °C, and a hybrid map pointed at an inverter with no
battery reports 6553.5 % state of charge, all night, every night.

`invalid` names that pattern. A register holding it is skipped — exactly as a register whose
block was never read is skipped, and with the same consequences: a channel that has never been
seen stays undeclared, and one that WAS read successfully on an earlier poll keeps its last value
until the normal staleness window expires, after which every output publishes null. The rule is
the one that applies everywhere here: **missing is not zero**, and a channel we cannot read is
absent rather than invented.

Take the value from documentation, not from a hunch about what looks like a sentinel. A real
reading can sit right next to one (`0x7FFE` is a perfectly good temperature), and the comparison
is exact for that reason.

### Correcting a sign convention

A **negative `scale`** negates the reading, which is how a device that reports the opposite sign
convention to ours gets corrected in the profile. The canonical convention is
`battery.power` positive while *charging* (see
[canonical-measurements.md](canonical-measurements.md)); a device reporting positive while
discharging maps with `scale = -1`.

Only do this when a source *states* the direction. If the register is documented merely as
"battery power" with no sign convention given, leave the row out and settle it on the bench —
getting it backwards produces a dashboard that is confidently inverted, which is worse than a
missing channel.

Neither `offset` nor a negative `scale` is accepted on a `[[write]]` row: the write path computes
`raw = value / scale`, refuses a negative raw, and does not invert an offset. The build rejects
both rather than emitting a row that passes review and then silently refuses or mis-writes every
value.

## `[[write]]` — optional: writable setpoint registers

**Read-only is the default.** A register is writable only when declared here — and even then
it is *dormant* until the row carries `verified = true`, meaning somebody wrote it on real
hardware, read it back, and confirmed the device acted on it. No row in this repository sets
that today. The section exists so write-register research can be recorded, reviewed and
bounds-checked long before anything acts on it.

The driver's write path itself is implemented (FC06, one holding register, echo verified).
**What it cannot do — 32-bit setpoints, FC16, enum modes like a battery work mode — is in
[write-path.md](write-path.md), and a row it cannot serve is refused rather than approximated.**
Read that before adding a `[[write]]` row, or you may write a row that validates and can never
be dispatched.

| Key | Type | Required | Meaning |
|---|---|---|---|
| `command` | string | yes | Canonical numeric setpoint this register implements — one of the ids from `python3 tools/gen_profiles.py --list-commands` (e.g. `set_export_limit_watts`, `set_battery_charge_limit_watts`). One row per command. |
| `display_name` | string | yes | Human name. |
| `space` | string | yes | Must be `"holding"` — Modbus writes target holding registers; input registers are read-only by definition. |
| `address` | int | yes | First register. Does *not* need to be inside a read `[[block]]` (write-only registers exist). |
| `type` | string | yes | `u16`, `s16`, `u32`, `s32`. Raw value = `value / scale`. |
| `function` | string | no (derived) | `"write_single"` (FC 06) or `"write_multiple"` (FC 16). Defaults to FC 06 for one word, FC 16 for two. **The driver serves FC 06 only**: a row set to `"write_multiple"` validates and is then refused at dispatch, so setting it is a way to *record* that a firmware demands FC 16 — deliberately dormant, not a way to enable it. See [write-path.md](write-path.md). |
| `scale` | number | no (1.0) | Same semantics as read registers. |
| `unit` | string | yes (numeric rows) | Same set as read registers. Refused on a mode row — a selection has no unit. |
| `minimum` / `maximum` | number | **yes** (numeric rows) | Bounds in canonical units. Mandatory — the dispatcher refuses unbounded writes, so the schema refuses unbounded rows. Refused on a mode row. |
| `options` | array | **yes** (mode rows) | The selectable modes: `[{ value = 0, label = "Self-consumption" }, …]`, with the vendor's own numbering. Only for `set_battery_operating_mode`; refused on a numeric row, and required on a mode one. At most 16. |
| `step` | number | no (1) | Setpoint granularity. Refused on a mode row, like `minimum`/`maximum`/`unit`: all four describe a range, and a mode row is a list. |
| `verified` | bool | no (**false**) | `true` only after the row is confirmed on real hardware. An unverified row is documentation, never a capability. |

Value-less commands (`start`, `stop`, `synchronize_time`) cannot be expressed as a write row —
"which value means start?" is driver semantics, not a register mapping. If a first device needs
one, that is a schema extension to design then, not to guess now.

Mode setpoints (`set_battery_operating_mode`) **can** be expressed, by declaring `options`
instead of bounds — see [write-path.md](write-path.md#mode-setpoints-supported-with-one-hard-limit),
including the one thing they cannot do: a mode packed into part of a shared register.

## What a profile can NOT express

By design. Being honest about the boundary saves contributors wasted effort:

- **Protocol logic.** Handshakes, registration sequences, session state, non-Modbus
  framing — that is a *codec*, written in C++ per protocol family (see
  `src/drivers/eversolar_legacy/` for what that looks like). A profile only maps
  registers of an existing codec.
- **Computed values.** No arithmetic between registers (e.g. power = V × I). If a
  device needs a derived channel, that is a small driver change — open an issue.
- **Acting on writes.** A `[[write]]` row *records* a writable register; it cannot
  *enable* writing. That requires `verified = true` plus a driver write path — see the
  `[[write]]` section above.
- ~~**Word-order variants.**~~ Supported since a vendor datasheet turned up specifying
  low-word-first for a register that same datasheet recommends using — see `word_order`
  above. High-word-first remains the default and is what nearly every device does. Getting
  this wrong is not subtle in one direction and invisible in the other: 2 kW read the wrong
  way round is about 34 MW, while a large value read the wrong way round can land near zero.

## Scope

`driver = "modbus_profile"` today. That driver is the generic consumer for Modbus-RTU
register-map devices, whatever the badge on the front: the brand lives in the profile's
`manufacturer` field, not in the driver. A genuinely different register-map protocol family
would get its own table-driven driver and reuse this same profile pipeline.

The id was `growatt_modbus` up to config version 1, when every profile it served was one
vendor's map. Stored configurations are migrated on load
(`src/config/configuration_store.cpp`); profile ids themselves never changed.
