# The write path: what it can and cannot do

A `[[write]]` row in a device profile describes a writable setpoint register. This document is
the honest boundary around what happens to that row — written down because the limits are not
visible from the schema, and a row that validates cleanly can still be one the firmware will
never dispatch.

Read this before adding a `[[write]]` row.

## The whole path, end to end

```
REST  POST /api/v1/devices/<id>/commands   ─┐
MQTT  <prefix>/command/set                  ├─> CommandQueue ─> rs485Task ─> CommandDispatcher ─> driver execute()
HA    number / button entity ──(MQTT)──────┘        (one in flight)   (owns the bus)   (5 gates)      (FC06)
```

Nothing writes on its own. A command is *submitted*, queued, and executed by the task that owns
the RS485 bus — never by the HTTP or MQTT handler, because a driver's `execute()` is a
multi-second bus transaction and running it from a network callback races the poll loop.

The outcome comes back asynchronously: `GET /api/v1/devices/<id>/commands/<requestId>` over
REST, or a republish on `<prefix>/command/result` over MQTT.

### The five gates, in order

`CommandDispatcher` refuses before the driver is ever reached:

1. **Read-only mode.** `security.read_only_mode` is the global kill switch and defaults to **on**.
2. **Inverter writes enabled.** `control.inverter_writes_enabled`, this path's own switch, off by
   default — so turning read-only mode off for the DRM relays does not open it as well.
3. **Capability.** The driver must advertise the command in `capabilities()`. A profile row is
   advertised only when it carries `verified = true`, or when the device's driver option
   `allow_unverified_writes` is `true` (a per-device opt-in for the bench check itself).
4. **Range.** The value must sit inside the bounds the profile declared.
5. **Rate limit.** One command per interval, so a stuck automation cannot hammer a setpoint.

Then the driver applies its own checks (below), and the register bounds are re-checked there
against the row itself.

## What it can do

- **One 16-bit holding register per command**, written with **FC06** (write single register).
- **The echo is verified.** FC06 makes the device echo the address and value it accepted; a
  mismatch is reported as a protocol error rather than success. A write whose echo was not
  checked is a request, not a setting — on a curtailment register the difference is between an
  inverter that is limited and one everybody believes is limited.
- **Scaling**, via the row's `scale`: `raw = value / scale`.
- **Bounds**, mandatory and enforced twice (dispatcher and driver).
- **Reversal**, because the register is also mapped as a read row — see "A read-back is
  required" below.
- **No pointless writes.** Just before writing, the driver reads the register live (FC03). If it
  already holds the value, the write is skipped and the command still reports `Ok`. Many inverters
  keep such a limit in EEPROM, and an automation that re-sends the same value every minute would
  otherwise wear it for nothing. The live read is used, not the last poll, because that can be a
  cycle stale or overwritten by a second controller. If the read fails, the write goes out anyway.

## What it cannot do

### 32-bit setpoints, and anything needing FC16

`writeFor()` refuses a row whose type spans two registers, and refuses `function =
"write_multiple"` outright. The Modbus client deliberately implements FC06 only: a vendor's
control block usually interleaves the points you want to set with timing and ramp registers you
do not, and a span write cannot express that difference.

Such a row is still worth declaring — it records the research — but it will never dispatch.
`capabilities()` will not advertise it, so it also never appears as a Home Assistant entity.
A device whose only export-limit register is 32-bit is therefore **readable but not
controllable** today, and that is a deliberate stop, not an oversight.

### Mode setpoints: supported, with one hard limit

`set_battery_operating_mode` carries a selection rather than a number — a battery work mode, an
EMS mode, a self-use/time-of-use selector. It works, as a `[[write]]` row that declares its
options:

```toml
[[write]]
command = "set_battery_operating_mode"
display_name = "EMS mode"
space = "holding"
address = 13049
type = "u16"
verified = false
# Every value from the vendor protocol document, with the vendor's own numbering. Gaps are
# normal -- retired modes leave holes -- and they are why the raw value is declared per option
# rather than inferred from position.
options = [
  { value = 0, label = "Self-consumption" },
  { value = 2, label = "Forced" },
  { value = 3, label = "External EMS" },
]
```

- `minimum`, `maximum`, `step` and `unit` are **refused** on such a row: they describe a range,
  and this is a list.
- The option's own `value` is what reaches the register — never its position in the list. A
  device numbering its modes 0, 2, 3 would otherwise be sent 1 for "Forced".
- A row with no `options` is refused by the build, and a driver that somehow publishes an empty
  list advertises nothing: an empty dropdown is not a control.
- Home Assistant gets a `select`. It sends the label a user picked; the generated
  `command_template` maps that label back to the mode number, so no automation has to know the
  numbering.
- The dispatcher checks **membership**, not range. A mode the device never declared is refused
  before the driver is reached — there is no "close enough" for a mode number.

**The hard limit: whole registers only.** Several vendors pack a mode into *part* of a register
alongside unrelated flags (`bitmask = 0x03` in some community maps). FC06 writes all sixteen bits,
so setting a masked field needs read-modify-write, which this path does not do — it would clear
whatever shares the register. The build rejects a `bitmask` key by name rather than ignoring it.
On Deye/Sunsynk, for instance, "Load Limit" (register 244) is a whole-register selector and
mappable; "Priority Load" and "Solar Export" are single bits inside a shared register and are not.

Curtailment does not depend on any of this: on every device surveyed so far the export limit and
the active-power limit are standalone numeric setpoints.

### `offset` and negative `scale`

Both are rejected on a `[[write]]` row by the build, because both produce a row that passes
review and then misbehaves:

- a negative `scale` yields `raw = value / scale` below zero, which `execute()` refuses as out of
  range — a setpoint that rejects every value sent to it;
- an `offset` would need inverting as `(value - offset) / scale`, which the write path does not
  do, so the register would receive a different number than the one requested.

Both remain legal on **read** rows, where a negative scale is the supported way to correct a
device that reports the opposite sign convention.

### A read-back is required

Writing a register does not make its value readable. A row is therefore served only when the
profile **also reads the setpoint back** as the command's canonical measurement, for example
`control.active_power_limit` for `set_active_power_limit_percent`. The pairs are listed in
`READBACK` in `tools/gen_profiles.py`. The requirement holds with or without the
`allow_unverified_writes` opt-in, and the generator refuses `verified = true` on a row without a
read-back.

The reason: the read-back shows what the inverter is actually limited to, as opposed to what it
was last asked for. The two differ whenever a write was refused, a second controller is on the
bus, or the inverter reverted the limit on its own timer, which several do by design. Without the
read-back, the bench check behind `verified` (write, read back, compare) cannot be done, and Home
Assistant shows only the request.

A command with no canonical read-back measurement yet (today: the export limit and battery mode)
stays dormant research. It needs the measurement in `measurement.h` and an entry in `READBACK`
first.

### `supportsWrite` on the descriptor is not per-device

`DriverDescriptor::supportsWrite` answers "can this driver ever write", and for the
profile-driven driver that is now `true`. Whether a *particular* configured device can be written
to depends on its profile having a verified row, and `capabilities()` is the authoritative
answer. Do not read the descriptor flag as a per-device promise; it is a driver-level fact used
for listing drivers in the UI.

## Adding a write row

1. Find the register in a **vendor protocol document**. An undocumented write target is
   disqualifying, not something to try: a wrong write can trip an inverter offline or push it
   outside grid-code limits.
2. Record the unit, the valid range and the step **from that document**, and note the source in a
   comment above the row.
3. Check it is a single 16-bit register and a numeric setpoint. If it is not, see above — declare
   it anyway with the reason, and expect it to stay dormant.
4. Map the same setpoint as a `[[register]]` row with the command's read-back measurement —
   see above. Without it the row is never served.
5. Leave `verified = false`. That is not a formality: it is the flag that keeps unproven research
   out of somebody's inverter.
6. To flip it, on a bench, with the inverter in view: write a value, read the register back,
   and confirm from the inverter's own reported output that it took effect. Then flip the flag in
   the same change as the evidence.

## Why it is this cautious

Writing to a hybrid's holding registers moves real energy, with real money and a real warranty
attached, on the basis of a register map that often starts life as a forum post. Every gate above
exists because the failure mode is not a wrong number on a dashboard — it is a curtailed
inverter, a battery discharged at the wrong time, or an installation outside its grid code.
