# Solar Assistant — MQTT source

Status: **structure and control topics verified from the official docs; exact per-metric
battery topic names must be confirmed against the live broker.** Same discipline as the Growatt
register map: the shape is known, the last-mile detail is read off the real system, not guessed.

## What this is

A second kind of *source* for the bridge, beside the RS485 protocol drivers. Solar Assistant
(commercial, runs on a Pi) is the Modbus master on the inverter and exposes everything over its
own MQTT broker — read **and** control. Where the colleague already runs it, our bridge does
not touch the RS485 bus (that would be a second Modbus master — a collision). Instead it
subscribes to Solar Assistant's topics, maps them to our canonical `DeviceState`, and maps our
canonical commands to Solar Assistant's `/set` topics.

## Why the bridge sits in front instead of using Solar Assistant's own HA discovery

The migration path. The colleague wants to start via our solution now (Solar Assistant staying
master) and later switch to the bridge being the Modbus master, as a config change. That switch
is only seamless if HA sees the **same entities either way** — same `entity_id`, same
`unique_id` — so long-term statistics and the Energy dashboard survive. If Solar Assistant
published to HA directly, switching to our own Growatt driver later would present *different*
entities and break history (the exact entity-migration / phantom-statistics pain from
2026-07-19). By republishing Solar Assistant's data under the bridge's own stable identity now,
mode B → mode A is a source swap underneath unchanged outputs.

Design rule that follows: the Solar Assistant source and the Growatt Modbus driver **must map
onto the identical canonical measurement ids**, so their discovery output is byte-for-byte the
same. That equivalence is the feature; guard it with a test.

## Topic structure (verified — solar-assistant.io/help/integration/mqtt)

- **State:** `solar_assistant/inverter_1/<metric>/state`
- **Control:** `solar_assistant/inverter_1/<setting>/set`
- **Writes are DISABLED by default** in Solar Assistant and must be enabled in its config. A
  silent no-op on control until the colleague turns this on — surface it in our UI, do not let
  a command look accepted when Solar Assistant is ignoring it.
- Solar Assistant also offers its own HA auto-discovery (optional); we do not rely on it, we
  consume the raw topics and publish our own discovery.

## Control topics (verified examples)

| Setting | Topic | Value |
|---|---|---|
| Output source priority | `.../output_source_priority/set` | string, e.g. "Utility first", "Solar first" |
| Charger source priority | `.../charger_source_priority/set` | string |
| Max grid charge current | `.../max_grid_charge_current/set` | integer, A |
| Capacity setpoint | `.../capacity_point_1/set` | integer, % |
| Shutdown battery voltage | `.../shutdown_battery_voltage/set` | float, V |

These map onto our command model: charger/output priority → `SetBatteryOperatingMode`, grid
charge current / capacity → `SetBatteryChargeLimit` / `SetMinimumSoc`. The exact string values
for priority modes are device-specific and come from the live broker.

## To confirm on the live broker (colleague, one command)

`mosquitto_sub -h <SA-ip> -v -t 'solar_assistant/#'` dumps every topic and payload. From that
we pin down, for this SPH6000:

1. The exact state topic names for: battery SoC, battery power (and its sign), battery voltage,
   battery temperature, grid power, PV power, load power, energy today/total.
2. Whether battery values live under `inverter_1`, a `total` namespace, or a `battery_1`
   namespace (varies by setup).
3. The exact accepted string values for the priority `/set` topics.
4. That MQTT control has actually been enabled (else writes are silently dropped).

## Sources

- Solar Assistant MQTT integration docs — <https://solar-assistant.io/help/integration/mqtt>
- JJSlabbert/Solar-Assistant-MQTT-client (Python examples, `Gen_Reading_all_messages.py`) —
  <https://github.com/JJSlabbert/Solar-Assistant-MQTT-client>
- Forum, SPH6000 over Solar Assistant MQTT —
  <https://diysolarforum.com/threads/mqtt-to-growatt-sph-6000-via-solar-assistant.102792/>
