# Device profile schema

A device profile is one TOML file in `profiles/<family>/` describing how to read one
device family over a register-map protocol. At build time `tools/gen_profiles.py`
validates every profile and generates the C++ tables the driver polls from
(`src/drivers/growatt_modbus/profiles_generated.cpp` — generated, git-ignored, never
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
| `driver` | string | yes | The C++ driver that consumes this profile. Only `"growatt_modbus"` is table-driven today; see [Scope](#scope) for what qualifies. |
| `id` | string | yes | Stable lowercase identifier (`[a-z][a-z0-9_]*`), unique across all profiles. Users select it with the driver's `profile` option; treat it as API, never rename it. |
| `display_name` | string | yes | Human-readable model name, e.g. `"Growatt SPH (3-6 kW)"`. Becomes the reported model identity. |
| `default` | bool | no (false) | Profile used when the `profile` option is unset. Exactly **one** profile per driver must set this. |
| `phases` | int | yes | AC phases, 1–3. |
| `mppts` | int | yes | MPPT/string inputs, 0–8. |
| `battery` | bool | yes | `true` for hybrids with an attached battery; drives the `ReadBatteryState` capability and battery discovery entities. |
| `transports` | array | no (`["rtu"]`) | Which transports the device family supports: `"rtu"` and/or `"tcp"`. Declaring `"tcp"` is schema-forward: the bridge has no Modbus TCP *client* transport yet, so a TCP-only profile cannot be polled today. |

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

Rules enforced by the build:

- at most **8** blocks (the driver's scratch-buffer limit);
- `start + count` must stay inside the 16-bit register address space;
- every mapped register must be covered by a block (including the second word of a
  32-bit value).

A block the device refuses with a Modbus exception is skipped at runtime, not fatal —
deliberately, so a profile may probe ranges that only exist on some firmware generations
and the TRACE dump shows which ones this unit actually has.

## `[[register]]` — 1 or more per file

One canonical measurement fed by one register (or register pair).
Decoded as `value = raw * scale`, after sign extension for `s16`/`s32`.

| Key | Type | Required | Meaning |
|---|---|---|---|
| `measurement` | string | yes | Canonical id from [canonical-measurements.md](canonical-measurements.md). Each id may be mapped at most once per profile. |
| `display_name` | string | yes | Human name for dashboards/Home Assistant. |
| `space` | string | yes | `"input"` or `"holding"`. |
| `address` | int | yes | First register. A 32-bit type also reads `address + 1`; the **high word comes first** (the convention Growatt and most Modbus inverters use — see word order caveat in [adding-a-device.md](../adding-a-device.md)). |
| `type` | string | yes | `u16`, `s16`, `u32`, `s32`. `s*` is two's-complement signed — use it for anything that can be negative (power that can flow both ways, temperatures). |
| `scale` | number | no (1.0) | Multiplier for the raw integer. A device reporting tenths uses `0.1`. Must not be 0. |
| `unit` | string | yes | One of `W` `V` `A` `Hz` `°C` (or `C`) `kWh` `h` `%` `dBm` `s`. The measurement *type* (Power, Voltage, …) is derived from the unit, so you never touch internal enums. |

## `[[write]]` — optional: writable setpoint registers

**Read-only is the default.** A register is writable only when declared here — and even
then it is *dormant*: two independent gates stand between a `[[write]]` row and a byte on
the bus. The row must carry `verified = true` (confirmed against the real device), and
the driver must implement a write path (none does today; `execute()` returns
Unsupported). The section exists so write-register research can be recorded, reviewed
and bounds-checked long before writing is ever enabled.

| Key | Type | Required | Meaning |
|---|---|---|---|
| `command` | string | yes | Canonical numeric setpoint this register implements — one of the ids from `python3 tools/gen_profiles.py --list-commands` (e.g. `set_export_limit_watts`, `set_battery_charge_limit_watts`). One row per command. |
| `display_name` | string | yes | Human name. |
| `space` | string | yes | Must be `"holding"` — Modbus writes target holding registers; input registers are read-only by definition. |
| `address` | int | yes | First register. Does *not* need to be inside a read `[[block]]` (write-only registers exist). |
| `type` | string | yes | `u16`, `s16`, `u32`, `s32`. Raw value = `value / scale`. |
| `function` | string | no (derived) | `"write_single"` (FC 06) or `"write_multiple"` (FC 16). Defaults to FC 06 for one word, FC 16 for two; set explicitly when a firmware demands FC 16 for single registers. |
| `scale` | number | no (1.0) | Same semantics as read registers. |
| `unit` | string | yes | Same set as read registers. |
| `minimum` / `maximum` | number | **yes** | Bounds in canonical units. Mandatory — the dispatcher refuses unbounded writes, so the schema refuses unbounded rows. |
| `step` | number | no (1) | Setpoint granularity. |
| `verified` | bool | no (**false**) | `true` only after the row is confirmed on real hardware. An unverified row is documentation, never a capability. |

Non-numeric commands (`start`, `stop`, `synchronize_time`) cannot be expressed as a
write row — "which value means start?" is driver semantics, not a register mapping. If a
first device needs one, that is a schema extension to design then, not to guess now.

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
- **Word-order variants.** 32-bit values are high-word-first. A device that is
  low-word-first needs decoder support first — open an issue rather than mapping it
  wrong.

## Scope

`driver = "growatt_modbus"` today. The Growatt driver is the generic consumer for
Modbus-RTU register-map devices; a genuinely different register-map protocol family
would get its own table-driven driver and reuse this same profile pipeline.
