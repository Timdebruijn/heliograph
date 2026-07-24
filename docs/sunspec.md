# SunSpec Modbus driver

Driver: `src/drivers/sunspec/`. Driver id `sunspec`.

Unlike every other driver here, this one carries no register map. SunSpec devices describe
their own layout at runtime, so a single driver covers any inverter that implements the
standard — no per-vendor file, no per-model file.

**STATUS: Experimental. Not yet confirmed against any physical device.** Everything below is
implemented and host-tested against a simulated device; nothing has met real hardware. See
[Compatibility](#compatibility) for what that means for your inverter.

## How it works

A SunSpec device places a marker and then a self-describing chain in its holding registers:

```
40000  "SunS"  (0x5375 0x6e53)
40002  model id | length | ...payload...
       model id | length | ...payload...
       0xFFFF                              <- end of chain
```

The driver reads the marker, then walks the chain, recording every block it finds. It reads
the models it understands and reports the rest.

**The whole chain is mapped, not just the usable part.** At startup the log carries the full
inventory:

```
[I] SUNSPEC chain at 40000: 6 model(s): 1, 103, 120, 122, 160, 802
```

That line is the useful one when a device does not work. It says exactly what the inverter
offers, which turns "it didn't work" into a report that can be acted on — and it is what
decides which models are worth implementing next. If you open an issue about an unsupported
SunSpec device, paste it.

## What is read today

**Model 1 (common)** — manufacturer, model, version and serial number. Used for device identity,
so the discovery wizard shows the real device rather than "SunSpec device".

**Models 101 / 102 / 103 (inverter, single / split / three phase)** — these three share one point
layout, verified against the official definitions, so all three are handled identically:

| Reading | SunSpec point | Canonical channel |
|---|---|---|
| AC power | `W` | `ac.power.total` |
| AC voltage | `PhVphA` | `ac.phase_l1.voltage` |
| AC current | `AphA` | `ac.phase_l1.current` |
| Grid frequency | `Hz` | `ac.frequency` |
| Lifetime energy | `WH` (acc32, Wh → kWh) | `energy.total` |
| DC power | `DCW` | `dc.power.total` |
| Temperature | `TmpCab` | `inverter.temperature` |
| Operating state | `St` | status code |

Not read yet: nameplate and settings (120–122), and the storage/battery models (124, 160,
802–804). Those are a separate step — battery semantics vary far more between vendors than
inverter measurements do, so they deserve their own hardware validation rather than being
guessed at. The chain inventory above already reports them when a device has them.

### Two rules that keep a reading honest

**Scale factors.** Every SunSpec value carries its exponent in a *separate* register:
`value = raw × 10^sf`, where `sf` is signed. Reading the raw register alone is not slightly
wrong — it is wrong by an order of magnitude while looking entirely plausible. When a device
does not publish a usable scale factor, the reading is **dropped** rather than published
unscaled, and a factor outside the documented −10..10 range is refused for the same reason.

**Not-implemented sentinels.** SunSpec defines `0xFFFF` for unsigned points and `0x8000` for
signed ones. Those are reported **absent**, never as zero. The distinction is not academic:
`0xFFFF` sitting in a signed point is simply −1, a perfectly valid reading.

Lifetime energy is the one exception where zero is real — a new inverter genuinely has
produced nothing yet — so a zero accumulator is published as a reading.

## Settings

| Option | Default | Notes |
|---|---|---|
| `unit_id` | `1` | Modbus slave address on the RS485 bus, 1–247 |
| `base_address` | `40000` | Where the `SunS` marker lives |

**About the base address.** 40000 covers most devices; **50000** is the other common choice, and
some vendors sit elsewhere entirely. It is a setting rather than a search on purpose: every
extra guess costs a discovery round trip on a shared bus, and you know your device better than
a loop does. If discovery finds nothing, check your inverter's Modbus documentation for its
base register and set it here.

## Connection

Modbus RTU over RS485 today. Wire A/B and ground to the inverter's RS485 terminals as for any
other RS485 device, and check the [general safety notes](../README.md#before-you-start-what-you-are-getting-into)
first.

**Modbus TCP is not supported yet.** This matters, because most SunSpec devices in the wild —
SolarEdge and SMA in particular — are reached over TCP rather than RS485. The transport
abstraction has a TCP type reserved, but no Modbus TCP client exists yet; that is a separate
piece of work.

## Compatibility

Two separate things, deliberately kept apart:

- **Expected to work** — the manufacturer states the device implements SunSpec. That is a
  reason to try, not evidence that it does.
- **Confirmed** — somebody ran this driver against the device and reported what happened.

| Device | Connection | Status |
|---|---|---|
| _(none yet)_ | | |

**Nothing is in the confirmed column yet, and that is the honest state of this driver.**

Vendors that publish SunSpec support, and are therefore worth trying: SMA, Fronius, SolarEdge,
and Huawei (partial). Note that for several of these the SunSpec interface is offered over
**Modbus TCP**, which this driver cannot use yet — so "the vendor supports SunSpec" does not
yet mean "this driver can reach it".

Explicitly **not** SunSpec: Growatt inverters use their own register map, which is why this
project carries a separate vendor driver for them. Devices exist that bridge Growatt to SunSpec
in middleware, which is the clearest evidence the inverters do not speak it themselves.

### Reporting a device

Whatever the outcome, it is worth reporting — **including "it returned nothing"**. A negative
result with the chain inventory in it is real information, and it is how the SolaX findings
elsewhere in these docs came to exist.

Useful to include:

1. Manufacturer, model, and how it is connected.
2. The `SUNSPEC chain at ...` log line (set the log level to `trace` under *Settings* if you
   want the raw transactions too).
3. The `base_address` you used.
4. Whether readings appeared, and whether they matched the inverter's own display or app.

Point 4 matters most: a driver that reports plausible-looking but wrong numbers is worse than
one that reports nothing, and a scale factor applied incorrectly looks entirely plausible.

## Implementation notes

The Modbus read transaction is shared with the vendor Modbus driver
(`src/protocols/modbus/modbus_client`), kept separate from the pure framing codec in
`modbus_rtu.h` so that codec stays transport-free.

Model layouts are currently constants in `sunspec_parser.h` rather than data files. That is
deliberate for now: models 101–103 share a single layout, so "layouts as data" would mean one
table with one consumer, and the profile pipeline exists so contributors can add a *device*
without writing C++ — which SunSpec already does not require. When the storage models land
there will be several layouts, and that is the point at which moving them into the profile
pipeline earns its keep.

Defensive behaviour, because vendors diverge: a chain that never terminates is bounded at 32
entries, one whose lengths walk off the address space stops, and one that simply stops
answering keeps whatever was mapped — several devices do not serve the terminator at all. None
of those fail the device outright.

Read-only. SunSpec does define writable models; enabling one needs a hardware-verified map and
a deliberate write path, and neither exists.

Sources: the official SunSpec model definitions (<https://github.com/sunspec/models>) for every
offset used here, and the SunSpec Alliance information model for the marker, chain structure
and sentinel values.
