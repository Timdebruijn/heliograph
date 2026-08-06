# MQTT design

Client: **espMqttClient 1.7.3** (MIT, non-blocking, QoS 0/1/2, LWT, auto-reconnect).
MQTT can optionally be disabled. If MQTT goes away, polling, Modbus TCP, REST, and the
web interface keep working fully — that is an acceptance criterion and is tested in Phase 9 by
hard-shutting-down the broker.

`<bridge_id>` = `heliograph-<last 3 bytes of MAC in hex>`, e.g. `heliograph-a1b2c3`.
Configurable; defaults to being derived from the MAC so that two bridges never collide.

## Topics

| Topic | Retained | QoS | Content |
|---|---|---|---|
| `heliograph/<bridge_id>/availability` | ✔ | 1 | `online` / `offline` |
| `heliograph/<bridge_id>/state` | ✔ | 0 | Measurements + status (JSON) |
| `heliograph/<bridge_id>/diagnostics` | ✔ | 0 | Bridge diagnostics (JSON) |
| `heliograph/<bridge_id>/identity` | ✔ | 1 | Device identity (JSON) |
| `heliograph/<bridge_id>/capabilities` | ✔ | 1 | Capabilities (JSON) |
| `heliograph/<bridge_id>/relay/<n>/state` | ✔ | 1 | `ON` / `OFF` — relay boards only |
| `heliograph/<bridge_id>/relay/<n>/set` | — | 1 | Command topic (subscribed) — relay boards only |

### More than one inverter

The **first** device publishes on the topics above, unchanged from when a bridge polled only
one. That is deliberate: moving it would change every Home Assistant entity's `unique_id` and
cost an existing install its recorder history for a feature it is not using.

Devices 2..N get their own subtree, keyed by device id:

| Topic | Retained | QoS | Payload |
|---|---|---|---|
| `heliograph/<bridge_id>/device/<device_id>/state` | ✔ | 0 | Measurements + status (JSON) |
| `heliograph/<bridge_id>/device/<device_id>/identity` | ✔ | 1 | Device identity (JSON) |
| `heliograph/<bridge_id>/device/<device_id>/capabilities` | ✔ | 1 | Capabilities (JSON) |

`availability`, `diagnostics` and the relay/DRM topics stay bridge-scoped — they describe the
bridge, not any inverter. Availability tracking the bridge is also why a sleeping inverter does
not make its entities unavailable at night.

Each device gets its own Home Assistant device block, its own `unique_id`s and its own retained
discovery config topics. Devices beyond the first also carry their address in the HA device name
(`… #2`), because identical inverters report an identical model and no serial number.

`<device_id>` is the id REST uses, frozen at boot: the driver id plus the configured address,
e.g. `modbus_profile-2`. It is **not** the serial number even for drivers that report one — the
serial arrives during the first poll, long after the id is needed.

### Removing or re-addressing a device

The bridge clears up after the devices that publish on `…/device/<device_id>/`. It remembers
which devices it announced and on which topic tree, and on the first connection after a restart
it publishes empty retained payloads for any that no longer owns that tree — the device's own
`state`, `identity` and `capabilities`, and every Home Assistant discovery config it could have
announced there. Home Assistant drops those entities.

Two things follow from *could have*: the topics are enumerated from the canonical measurement
ids rather than derived from the device's readings, because by then the device is gone and so
are its readings; and clearing a topic that was never used is harmless.

It matters because of how availability works here. Availability is bridge-scoped, deliberately,
so a sleeping inverter does not make its entities unavailable every night — which means an
orphaned entity does **not** go unavailable either. It reports *online* forever, showing the
last value it ever read, and a template or automation summing your inverters keeps counting it.

Two edits trigger it, and for devices 2..N they are the same edit: changing an address counts as
removing the device, because the address is part of the device id. The other is promotion —
moving a device into the `driver` slot, which leaves the per-device tree it used to publish on
with no owner.

**The first device's own tree is never cleared, and that is the point.** The bridge-scoped
topics and `unique_id`s always belong to whatever is in the `driver` slot, so re-addressing or
replacing the first inverter hands the new one those entities and their recorder history rather
than orphaning them. A single-inverter bridge therefore never orphans anything by changing its
address. What can be left behind is narrower: if the replacement publishes *fewer* measurements
than its predecessor, the discovery configs for the difference stay retained. Delete those
entities in Home Assistant, or clear them with `mosquitto_pub -r -n -t <topic>`.

The clearing runs once per boot and only after the broker is connected, and every publish is
checked — a clear that the client refused is retried, and until it succeeds the device stays in
the bookkeeping rather than being recorded as cleared. Not covered:

- **A boot where a configured device did not start.** Nothing is cleared that boot: a device
  skipped for a duplicate address or a driver that is not compiled in is still configured, and
  its id cannot be known without its driver. Fix the configuration and reboot. The log says so.
- **A changed base topic or discovery prefix**, which the bridge reports rather than clears —
  see the section below for what is actually left behind and how to remove it.
- **A factory reset**, which erases the bookkeeping along with everything else, and a
  bookkeeping entry that cannot be read back.
- **A bridge that ran a build from before this existed.** Its orphans have to be cleared by
  hand (`mosquitto_pub -r -n -t <topic>`) or deleted in Home Assistant.

### Changing the base topic or the discovery prefix

The bridge **names what it left behind and does not delete it.** The bookkeeping records the
topic tree each announcement went to, so on the first boot after a change the log says which
prefix the old payloads are under. Clearing them is a one-line job you run; the firmware does not
do it for you, for the reason at the end of this section.

The two settings fail differently, and it is worth knowing which one you changed.

**Changing only `mqtt.base_topic` costs you nothing in Home Assistant.** A discovery config's
topic is `<discovery_prefix>/<component>/<unique_id>/config`, and `unique_id` is derived from the
bridge id and the device id — the base topic appears nowhere in it. So every config is rewritten
*in place* with a `state_topic` pointing at the new tree, and the entities follow, history
intact. What stays behind is the old `state`, `identity` and `capabilities` payloads: retained
bytes on your broker that nothing reads any more. Litter, not zombies.

**Changing `mqtt.discovery_prefix` is the one that leaves entities behind.** The old configs are
at a path the bridge no longer writes to, so Home Assistant keeps reading them, pointing at a
`state_topic` that is no longer updated. Availability is bridge-scoped by design, so those
entities never go unavailable — they report *online* forever with their last value, and any
template or automation summing your inverters keeps counting them.

To see what is there, and then remove it:

```bash
mosquitto_sub -h <broker> -v -t 'OLD_PREFIX/#' --retained-only -W 2
mosquitto_pub -h <broker> -r -n -t 'OLD_PREFIX/sensor/<unique_id>/config'
```

An empty retained payload (`-r -n`) is what deletes a retained topic, and for a discovery config
it is also what tells Home Assistant to drop the entity. Deleting the entities in Home Assistant
works too, but leaves the retained configs on the broker to come back on the next restart.

**Why the firmware does not sweep this itself.** Getting the distinction above wrong deletes a
working device from someone's dashboard while nobody is watching. The bridge would have to be
certain which of the two settings moved and which configs are consequently live — and a
single-character typo in a prefix, corrected a minute later, would have it clearing a tree it is
about to use again. No comparable project does this in firmware either: ESPHome ships
`esphome clean-mqtt` and Tasmota points at its Device Manager, both host-side tools a human runs
deliberately. What the bridge can do that an external script cannot is know where its own topics
used to be, so it says so. See issue #41.

> **Which device is "first" comes from the configuration, not from boot order.** It is the
> `driver` entry. If it fails to start, no device takes over the bridge-scoped topics — that is
> deliberate, so a bad boot cannot transplant one inverter's history onto another.

**Last Will and Testament:** topic `availability`, payload `offline`, retained, QoS 1. On a
clean shutdown, the bridge publishes `offline` itself.

Important distinction: `availability` is about the **bridge**, not the inverter. An
inverter that's off at night doesn't make the bridge offline — that would make all
entities `unavailable` in Home Assistant and ruin the history. The inverter status lives in
`state` as `inverter_online`.

The bridge subscribes to **three** things:

| Topic | Gated by |
|---|---|
| `relay/<n>/set` (relay boards only) | `security.read_only_mode` off **and** `relays.enabled` on |
| `drm/set` (relay boards only) | the same two gates |
| `<device>/command/set` | `security.read_only_mode` off, then the driver's own capability check |

Relay commands pass the RelayController's gates, and the acknowledged state follows on
`relay/<n>/state`, so a refused command visibly snaps the Home Assistant switch back.

**This section used to say the relay topics were the only subscription, and that there were no
command topics for the inverter because the active driver had no write capabilities.** Both
stopped being true when the command queue landed: the subscription is unconditional wherever a
handler is wired, which it always is. What is still true is that no shipped register map has a
`verified` write row, so a command on that topic ends in `unsupported` rather than a register
write — and `read_only_mode` refuses the whole path before a driver is reached. See
[docs/security.md](security.md#what-can-reach-the-inverter) for which drivers can write and under
what conditions; the honest summary is "gated", not "absent".

The outcome of a submitted command is published on `<device>/command/result`.

## Publishing strategy

- **Publish-on-change** with deadband, to avoid broker spam: power ≥ 5 W, voltage ≥ 0.5 V,
  current ≥ 0.1 A, frequency ≥ 0.02 Hz, temperature ≥ 0.2 °C; energy and status on every change.
- **Periodic forced refresh** every 60 s, even without a change. Fixed, not configurable: it is
  a compile-time constant in `publish_policy.h`, and nothing in the settings or the config file
  changes it. (This line said "configurable" and there has never been a setting for it.)
- `identity` and `capabilities` only on change or after (re)connection.
- JSON document is bounded: `JsonDocument` with explicit capacity checking; exceeding it
  logs an error and does not publish — never a truncated JSON message.

## `state` — example

Only `supported` measurements appear. The TL3000-20 has no L2/L3 and no battery, so
those fields simply do not exist in the payload:

```json
{
  "bridge_online": true,
  "inverter_online": true,
  "data_valid": true,
  "data_stale": false,
  "driver_id": "eversolar_legacy",
  "manufacturer": "Ever-Solar",
  "model": "TL3000-20",
  "serial_number": "XH300060115506193600V610",
  "last_successful_poll_ms": 1752670000000,
  "measurements": {
    "ac.power.total":       { "value": 1842.0, "unit": "W",   "valid": true, "stale": false },
    "ac.phase_l1.voltage":  { "value": 233.4,  "unit": "V",   "valid": true, "stale": false },
    "ac.phase_l1.current":  { "value": 7.9,    "unit": "A",   "valid": true, "stale": false },
    "ac.frequency":         { "value": 49.98,  "unit": "Hz",  "valid": true, "stale": false },
    "dc.mppt_1.voltage":    { "value": 341.2,  "unit": "V",   "valid": true, "stale": false },
    "dc.mppt_1.current":    { "value": 5.6,    "unit": "A",   "valid": true, "stale": false },
    "dc.power.total":       { "value": 1910.7, "unit": "W",   "valid": true, "stale": false, "derived": true },
    "energy.today":         { "value": 8.42,   "unit": "kWh", "valid": true, "stale": false },
    "energy.total":         { "value": 18452.7,"unit": "kWh", "valid": true, "stale": false },
    "inverter.temperature": { "value": 41.3,   "unit": "°C",  "valid": true, "stale": false },
    "inverter.operating_hours": { "value": 31204, "unit": "h", "valid": true, "stale": false }
  },
  "status_code": 1,
  "status_text": "Unknown (1)",
  "error_code": null
}
```

Two deliberate choices relative to the example in the assignment:

1. **`status_text` is `"Unknown (1)"`, not `"Normal"`.** The meaning of `OP_MODE` is
   documented nowhere — not in the reference implementation, not anywhere else. No
   table is being invented. Once Phase 3 has logged the codes over a full day, a real
   mapping will follow.
2. **`error_code` is `null`, not `0`.** The protocol has no readable error code field. `0`
   would mean "no error" and we don't know that.

On a nighttime outage:

```json
{
  "bridge_online": true,
  "inverter_online": false,
  "data_valid": false,
  "data_stale": true,
  "measurements": { "...": { "value": null, "valid": false, "stale": true } }
}
```

`value: null` — not zero. A zero would end up in Home Assistant statistics as "0 W
produced" and distort the daily curve.

## Home Assistant MQTT Discovery

Prefix `homeassistant/` (configurable). Entities are generated **exclusively** from
`state.measurements` + `capabilities` — the discovery module contains no brand-specific
rule at all and has no table of EverSolar fields. A mapping from `MeasurementType`/`Unit` to
`device_class`/`state_class` is sufficient.

| Measurement | device_class | state_class | Unit |
|---|---|---|---|
| `ac.power.total`, `dc.power.total` | `power` | `measurement` | W |
| `*.voltage` | `voltage` | `measurement` | V |
| `*.current` | `current` | `measurement` | A |
| `ac.frequency` | `frequency` | `measurement` | Hz |
| `inverter.temperature` | `temperature` | `measurement` | °C |
| `energy.today`, `energy.total` | `energy` | `total_increasing` | kWh |
| `inverter.operating_hours` | `duration` | `total_increasing` | h |
| `wifi_rssi` | `signal_strength` | `measurement` | dBm |

Discovery payload per measurement:

```json
{
  "unique_id": "heliograph-a1b2c3_ac_power_total",
  "object_id": "heliograph_ac_power_total",
  "name": "AC Power",
  "state_topic": "heliograph/heliograph-a1b2c3/state",
  "value_template": "{{ value_json.measurements['ac.power.total'].value }}",
  "availability_topic": "heliograph/heliograph-a1b2c3/availability",
  "device_class": "power",
  "state_class": "measurement",
  "unit_of_measurement": "W",
  "device": {
    "identifiers": ["heliograph-a1b2c3_inverter"],
    "name": "Heliograph – EverSolar",
    "manufacturer": "Ever-Solar",
    "model": "TL3000-20",
    "serial_number": "XH300060115506193600V610",
    "via_device": "heliograph-a1b2c3"
  }
}
```

`value_template` yields `None` for `value: null` → Home Assistant marks the entity
`unknown` instead of 0. That is exactly what's wanted at night.

### Two devices

- **Bridge** (`heliograph-a1b2c3`): manufacturer "Heliograph open-source project", model
  "Waveshare ESP32-S3-RS485-CAN", firmware version. Carries the diagnostic entities (RSSI,
  uptime, heap, poll counter).
- **Inverter** (`heliograph-a1b2c3_inverter`): manufacturer/model/serial number from
  `DeviceIdentity`, with `via_device` pointing to the bridge. Carries the measurements.

This keeps the HA device page correct if a second inverter is added later.

### No control entities

`capabilities.write` is empty → no `number`, `switch`, or `select` entities are
created. This is a loop over the write bitset, not a driver check.

## The `supported` field: two ways to say "not available"

| Way | Meaning | Behavior |
|---|---|---|
| Not declaring the channel at all | The device doesn't have this (a string inverter has no battery) | Nothing published |
| `declareUnsupported()` | The device does have it, but this protocol/firmware doesn't provide it | Nothing published |

Both are treated identically by every output. The difference is intent: the second keeps a
fixed schema visible so a later driver version can populate the channel without the shape
of the measurement set changing.

This is not cosmetic. Without `declareUnsupported()`, `Measurement::supported` would always
be `true` — a flag that MQTT, Modbus, and discovery dutifully check and that nothing could
ever set to `false`. That's not safety but a trap: the first one to actually set it false
discovers that half the outputs ignored it. Now the behavior is enforced by tests.

## Reconnect

Exponential back-off 1 s → 2 s → 4 s → … → max 60 s. After connecting: availability `online`,
then identity/capabilities/discovery, then state. The poll cycle keeps running in the
meantime — `mqttTask` and `rs485Task` share nothing but the `StateStore`.

## Changing the base topic or the discovery prefix

Both need a restart, and both leave the previous topic tree behind on the broker. **The bridge
does not clear it**, and the two fields do not strand the same things.

### `mqtt.base_topic`

The Home Assistant discovery configs do **not** move. Their topics are built from the bridge id,
not from the base topic, so the next announcement overwrites them in place with `state_topic`
pointing at the new tree. Your entities keep working and keep their history.

What is stranded is the retained data under the old tree — `state`, `identity`, `capabilities`,
`diagnostics`, `availability`, and the same three for every extra device. Invisible in Home
Assistant, clutter on the broker.

### `mqtt.discovery_prefix`

Here the configs really do move. The old ones stay retained, so Home Assistant keeps a full set
of stale entities beside the new ones — reporting online forever with their last value, because
availability is bridge-scoped and nothing publishes to the old tree any more.

**Deleting the device in Home Assistant does not fix this.** The retained config is still on the
broker, so it is rediscovered on the next Home Assistant restart. It has to be cleared at the
broker.

### Clearing it

The bridge logs the old and new prefixes when it notices the change, so the tree to clear is
named in the boot log rather than left for you to work out.

Retained messages are deleted by publishing an empty payload to the same topic with the retain
flag set. With mosquitto's tools, for a base topic that moved:

```bash
mosquitto_sub -h BROKER -t 'OLD_BASE/BRIDGE_ID/#' -v --retained-only -W 2 \
  | cut -d' ' -f1 \
  | xargs -I{} mosquitto_pub -h BROKER -t {} -r -n
```

For a discovery prefix that moved, the entities live under several component types
(`sensor`, `binary_sensor`, `switch`, `select`) and under two id shapes — `BRIDGE_ID` for the
first device and the bridge's own entities, `BRIDGE_ID_DEVICEID` for every other. **MQTT has no
wildcard within a topic level** — `+` matches exactly one whole level and `#` the rest, so
`BRIDGE_ID*` matches nothing. Subscribe to the whole prefix and filter:

```bash
mosquitto_sub -h BROKER -t 'OLD_PREFIX/#' -v --retained-only -W 2 \
  | grep "BRIDGE_ID" \
  | cut -d' ' -f1 \
  | xargs -I{} mosquitto_pub -h BROKER -t {} -r -n
```

Two things about `--retained-only`: it exits as soon as a **non**-retained message arrives, which
on an orphaned tree never happens (nothing publishes there any more) but will cut the listing
short if you point it at a live one. And `-W 2` is the safety net — without a timeout the client
waits forever on a quiet tree.

Check what would be deleted before deleting it: drop the `xargs` line and read the list first.

### Why the firmware does not do it for you

An automatic sweep on boot has to get the distinction above exactly right, and the failure mode
of getting it wrong is deleting the discovery configs that were just correctly rewritten —
turning broker clutter into a device that vanishes from someone's dashboard, unattended. No
comparable project does this in firmware either: ESPHome ships `esphome clean-mqtt` and Tasmota
points at Tasmota Device Manager, both host-side tools a human runs deliberately.

Tracked in [#41](https://github.com/Timdebruijn/heliograph/issues/41).
