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

**Last Will and Testament:** topic `availability`, payload `offline`, retained, QoS 1. On a
clean shutdown, the bridge publishes `offline` itself.

Important distinction: `availability` is about the **bridge**, not the inverter. An
inverter that's off at night doesn't make the bridge offline — that would make all
entities `unavailable` in Home Assistant and ruin the history. The inverter status lives in
`state` as `inverter_online`.

There are **no command topics.** The active driver has no write capabilities, so the
bridge doesn't subscribe to anything. That is not a configuration choice but a consequence of
`capabilities.write.none()`.

## Publishing strategy

- **Publish-on-change** with deadband, to avoid broker spam: power ≥ 5 W difference,
  voltage ≥ 0.5 V, energy on every change, status on every change.
- **Periodic forced refresh** every 60 s (configurable), even without a change.
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
  "Waveshare ESP32-S3-Relay-1CH", firmware version. Carries the diagnostic entities (RSSI,
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
