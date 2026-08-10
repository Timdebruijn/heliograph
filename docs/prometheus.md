# Prometheus metrics

Heliograph exposes its state in the Prometheus text exposition format at **`GET /metrics`**.
Nothing needs enabling: if the firmware was built with Prometheus support (all release images
are), the endpoint is live as soon as the bridge is on the network.

```bash
curl http://heliograph-a1b2c3.local/metrics
```

The endpoint is also readable by anything that speaks the same format — **Zabbix**, **Checkmk**
and **Telegraf** can all scrape it directly, and Grafana can graph it through any of them.

## How to scrape it

Add the bridge as a normal static target. It is a small, cheap endpoint — a few kilobytes of
text — but there is no point scraping faster than the inverter is polled: the default poll
interval is 10 seconds, so anything under that just repeats a sample.

```yaml
scrape_configs:
  - job_name: heliograph
    scrape_interval: 30s
    static_configs:
      - targets: ["heliograph-a1b2c3.local:80"]
```

Prefer the IP address if your Prometheus host cannot resolve mDNS `.local` names — many
containers cannot.

### Response codes

| Code | Meaning |
|---|---|
| `200` | Metrics, `Content-Type: text/plain; version=0.0.4` |
| `503` | No device configured yet — body is `# no device configured` |

The `503` is deliberate: a freshly provisioned bridge with no driver selected has nothing to
report, and reporting zeroes would be worse than reporting nothing. Prometheus marks the
target down, which is exactly what it is.

### Authentication

**There is none on `/metrics`.** Every mutating endpoint in the REST API is admin-gated, but
this one is a read-only scrape target and is deliberately left open so a scraper needs no
credentials.

That means anyone on your network can read your production figures, your firmware version and
your board type. On a home LAN that is normally fine. If it is not, put the bridge on a
segregated VLAN and let only the scraper reach it — the firmware has no per-endpoint access
control to do it for you. See [security.md](security.md).

## Several inverters: the `device` label

Every inverter series carries a **`device`** label holding the device id — the same string
`/api/v1/devices` serves, e.g. `modbus_profile-2`. So one bus of three inverters is three series
per metric:

```
heliograph_inverter_ac_power_watts{device="modbus_profile-1"} 1240.000
heliograph_inverter_ac_power_watts{device="modbus_profile-2"} 980.500
heliograph_inverter_ac_power_watts{device="modbus_profile-3"} 1105.000
```

**Bridge-wide series carry no `device` label** — uptime, heap, WiFi, the RS485 and poll
counters, the relay and DRM series. They are not per device: the counters live in one place for
the whole bus, so labelling them would invent a distinction the firmware does not make.

### Upgrading from 0.19.x or earlier

`heliograph_inverter_ac_voltage_volts` and `heliograph_inverter_ac_current_amperes` **gained a
`phase` label**. The metric names did not change, but a series with a new label is a new series:
an existing panel keeps its history and stops receiving points, and the new series starts empty
beside it.

Selectors keep working — `heliograph_inverter_ac_voltage_volts{device="x"}` still matches, now
returning one series per phase. What needs a look is anything that assumed a single series, such
as a panel with no legend format or an alert on the bare metric. `max by (device)` or
`{phase="l1"}` restores the old meaning.

Nothing else was renamed. Everything else in this release is new metrics that were simply not
exported before.

### If you already have a dashboard

**The first device is labelled too.** That is a breaking change and it was chosen deliberately:
leaving device 1 unlabelled would have been back-compatible, but `sum by (device)` would then
produce a blank label forever, and an asymmetry that has to be explained every time is worse
than a one-off edit. (MQTT made the opposite call for its topics, because there the cost of
changing was a user's whole Home Assistant history.)

What actually breaks, and what does not:

| Query | Before | After |
|---|---|---|
| `heliograph_inverter_ac_power_watts` | one series | one series **per inverter** — a graph gains lines |
| `sum(heliograph_inverter_ac_power_watts)` | the one inverter | the whole bus — usually what you wanted |
| a recording rule expecting a single series | fine | **fix it**: add `sum(...)` or `{device="…"}` |
| `heliograph_build_info` | one series | one **per inverter**, and it gains `device` too — a `group_left` join on it now fans out |
| any bridge-wide series | unchanged | unchanged |

With one inverter the only visible change is that a label appears, and existing panels keep
working. One thing to expect at the upgrade itself, on any number of inverters: adding a label
**changes the series identity**, so Prometheus ends the old series and starts a new one. Gauges
graph straight across the gap, but `rate()`, `increase()` or `resets()` over the boundary — and
an alert with a long `for:` — will see a discontinuity once.

### Matching it up with Home Assistant

The `device` label matches `/api/v1/devices`, and matches the MQTT subtree for devices 2..N.
It does **not** match what Home Assistant displays: device 1 keeps the bridge-scoped MQTT topics,
so its id appears nowhere in MQTT, and HA names the devices after the model (`… - Growatt MIC
TL-X`, `… #2`, `… #3`). Lining a Grafana panel up with an HA entity is a short lookup rather than
a string match.

## What is exported

Every series is prefixed `heliograph_`. Base units are in the names, counters end in `_total`.

### Build and health

| Metric | Type | Notes |
|---|---|---|
| `heliograph_build_info` | gauge | Always `1`. One per inverter. Labels `device`, `version`, `driver`, `board` |
| `heliograph_inverter_online` | gauge | `1` when the inverter is answering, `0` when it is not |
| `heliograph_data_stale` | gauge | `1` when the last reading is too old to trust |

### Inverter readings

All gauges, all carrying `device`, and all **omitted entirely when the value is unknown** (see
[Missing values](#missing-values-are-missing-not-zero)) — per device, so a bus where one
inverter reports temperature and another does not gives one series, not two with a fabricated
one. A metric no inverter reports is absent entirely, header included.

Two of these carry a second label besides `device`. A phase and an MPPT string are dimensions of
the same quantity, not different quantities, so they are labels rather than three metric names —
`sum by (device) (heliograph_inverter_ac_power_watts)` works, and would not if L1, L2 and L3 were
`..._l1_volts` and friends.

**Whole inverter**

| Metric | Unit |
|---|---|
| `heliograph_inverter_ac_power_watts` | W |
| `heliograph_inverter_dc_power_watts` | W (whole array, derived) |
| `heliograph_inverter_grid_frequency_hertz` | Hz |
| `heliograph_inverter_energy_today_kwh` | kWh |
| `heliograph_inverter_energy_total_kwh` | kWh, lifetime |
| `heliograph_inverter_operating_hours` | h, lifetime |
| `heliograph_inverter_temperature_celsius` | °C |

**Per phase** — extra label `phase="l1"`, `"l2"`, `"l3"`

| Metric | Unit |
|---|---|
| `heliograph_inverter_ac_voltage_volts` | V |
| `heliograph_inverter_ac_current_amperes` | A |
| `heliograph_inverter_ac_phase_power_watts` | W |

`ac_phase_power_watts` is a separate family from `ac_power_watts` on purpose. Parts and their
sum in one family is how a `sum()` silently double-counts.

**Per MPPT string** — extra label `string="1"`, `"2"`

| Metric | Unit |
|---|---|
| `heliograph_inverter_mppt_voltage_volts` | V |
| `heliograph_inverter_mppt_current_amperes` | A |
| `heliograph_inverter_mppt_power_watts` | W |

**Battery** — a separate prefix, because a battery is its own thing that happens to be reported
through the inverter. A query for the inverter's temperature should not have to exclude the
battery's.

| Metric | Unit |
|---|---|
| `heliograph_battery_state_of_charge_percent` | % |
| `heliograph_battery_power_watts` | W, positive charging |
| `heliograph_battery_charge_power_watts` | W |
| `heliograph_battery_discharge_power_watts` | W |
| `heliograph_battery_voltage_volts` | V |
| `heliograph_battery_current_amperes` | A |
| `heliograph_battery_temperature_celsius` | °C |
| `heliograph_battery_energy_charged_kwh` | kWh, lifetime |
| `heliograph_battery_energy_discharged_kwh` | kWh, lifetime |

**Grid meter**

| Metric | Unit |
|---|---|
| `heliograph_grid_import_power_watts` | W |
| `heliograph_grid_export_power_watts` | W |
| `heliograph_grid_power_watts` | Net grid flow. **Signed**: positive means importing, negative exporting. Present only when a device publishes `grid.power`. |
| `heliograph_load_power_watts` | W, what the house is drawing |

Which of these appear depends on the inverter: a driver only reports what its device actually
provides, so a single-phase inverter has no three-phase series and an inverter without a
temperature sensor has no temperature series. That is not a fault.

### Communication counters

| Metric | Type | What it counts |
|---|---|---|
| `heliograph_poll_success_total` | counter | Successful polls |
| `heliograph_poll_failure_total` | counter | Failed polls |
| `heliograph_rs485_checksum_errors_total` | counter | Frames that failed their checksum |
| `heliograph_rs485_timeouts_total` | counter | Reads that timed out |
| `heliograph_invalid_frames_total` | counter | Structurally invalid frames |
| `heliograph_mqtt_reconnects_total` | counter | MQTT reconnections |
| `heliograph_wifi_reconnects_total` | counter | WiFi reconnections |
| `heliograph_modbus_client_connections_total` | counter | Modbus TCP connections accepted |
| `heliograph_modbus_clients` | gauge | Modbus TCP clients connected right now |

**None of these carry a `device` label, and on a bus with several inverters that is a real
limitation, not just a naming choice.** The counters live in one place for the whole bus, so
when one of three inverters starts corrupting frames the checksum counter climbs and all three
look equally guilty. What *is* per device is
`heliograph_inverter_online{device="…"}` — so a unit that drops out entirely is identifiable; a
unit with intermittent, sub-timeout faults is not. Per-device counters would mean changing the
diagnostics model, which is tracked separately.

The distinction matters when something is wrong, and it is three-way rather than two:

- **Timeouts** — nothing came back at all. Wiring, a swapped A/B, a wrong address, or an
  inverter that is simply asleep.
- **Checksum errors** — bytes came back corrupted. This is the one that indicts the *wire*: a
  missing ground, no termination (or termination in three places instead of two), a long stub,
  a run alongside the inverter's own output cabling. See [rs485-bus.md](rs485-bus.md).
- **Invalid frames** — an intact frame that was not what we asked for: a neighbour on a
  multidrop bus answering, or a device quirk. Addressing, not cabling.

A quiet night on a solar inverter produces timeouts and no checksum errors, and that is normal:
the inverter powers down after dark. That asymmetry is exactly why the alert below watches
checksum errors rather than timeouts.

> **Upgrading from a release before per-transaction counting?** `heliograph_rs485_timeouts_total`
> steps up by roughly 2–3× on the same hardware, with nothing wrong. A dark inverter used to add
> one timeout per poll; it now adds one per read, which is one per register block on a Modbus
> profile. On EverSolar and SolaX it is worse: after three consecutive timeouts the recovery
> probe runs on every poll, so a night goes from ~1 to ~2 timeouts per poll — and a bridge that
> reboots after dark, which used to report a *cold-start* night as zero timeouts, now reports
> two per poll. Rescale any threshold you built on the raw counter. The checksum counter climbs
> too, but only on a bus that was already faulty. Nothing else in this file changed meaning.

These three count **transactions, not polls**. A Growatt poll reads each register block as its
own Modbus transaction; the PMU drivers may re-register before they query. So one poll can add
several errors, and on a healthy bus adds none.

That distinction took two fixes to get there:

- Before the release carrying this note, a Modbus driver could not raise the checksum counter
  **at all**: the shared read transaction folded CRC failures into a generic protocol error, so
  on a Growatt or SunSpec install the metric was structurally always zero and the alert could
  never fire. The PMU drivers (EverSolar, SolaX) were unaffected.
- And on every driver it only counted when a poll failed **outright**, because the counters were
  derived from the poll's verdict — and a poll succeeds as soon as *one* block decodes. Each
  driver now tallies what the wire did per transaction, and the poll verdict no longer gates it.

  Concretely, on a two-block profile polling every 10 s: at a 3% frame-corruption rate the old
  counter moved about **once every three hours** — never enough for `rate(...[15m]) > 0` to hold
  for the alert's `for:` window, so it would never have fired. The new counter moves ~22 times
  an hour and the alert fires reliably. At heavier corruption the old counter did eventually
  work: at 30% it caught roughly 32 an hour, because both blocks failing at once stops being
  rare. So this fix does not make a *failing* bus visible sooner; it makes a *degrading* one
  visible at all, and the crossover is somewhere around 15%.

> **Two under-counts remain.**
>
> - Line noise that damages a length or byte-count field can make the frame un-parseable rather
>   than merely wrong, and that surfaces as a **timeout** rather than a checksum error. (Not
>   always: a byte count damaged *downwards* shortens the frame, the CRC is then checked over
>   the wrong span, and it does land in the checksum counter.)
> - A poll can fail with all three counters flat. A device that answers every range with an
>   exception, or a SunSpec device advertising a model this driver cannot decode, is a
>   configuration fault, not a wire fault — `heliograph_poll_failure_total` and `last_error` are
>   what carry it.
>
> So: treat a non-zero value as a definite problem, and a **zero as "no proof either way"**. The
> `trace` log is the ground truth: a corrupt block says so on the line it happens.

Two things are deliberately **not** errors in any of the three counters, because both prove the
wiring and the addressing are fine:

- An **exception reply** — the device answering "I do not have that register".
- A **probe block** failing in any way. A profile may declare a block that maps nothing and
  exists only to discover which register generation a model speaks (see
  [device-profiles/schema.md](device-profiles/schema.md)). A device is allowed to answer an
  unknown range with silence rather than an exception, and counting that would put a permanent
  slope on the metric of a perfectly healthy installation — 360 timeouts an hour at the default
  poll interval.

### Bridge health

| Metric | Type | Notes |
|---|---|---|
| `heliograph_uptime_seconds` | gauge | Seconds since boot |
| `heliograph_free_heap_bytes` | gauge | Free heap |
| `heliograph_max_alloc_heap_bytes` | gauge | Largest single allocatable block |
| `heliograph_psram_size_bytes` | gauge | Total external PSRAM — **absent on a board without it** |
| `heliograph_psram_free_bytes` | gauge | Free external PSRAM — **absent on a board without it** |
| `heliograph_coredump_present` | gauge | `1` when a crash dump is waiting in flash — **always emitted**, `0` is a fact |
| `heliograph_mqtt_publish_failures_total` | counter | Publishes the MQTT client refused (link down, or outbox full) |
| `heliograph_wifi_rssi_dbm` | gauge | Only present while WiFi is connected |
| `heliograph_rs485_stack_free_bytes` | gauge | Only after the first sample |
| `heliograph_loop_stack_free_bytes` | gauge | Only after the first sample |
| `heliograph_time_synced` | gauge | `1` once the clock has been set from NTP |
| `heliograph_ntp_last_sync_timestamp_seconds` | gauge | Unix time of the last sync; absent until there has been one |

### Relays and curtailment

Only on relay boards. A board without relays exports **none** of these — not zeroes — so a
monitoring-only bridge never grows a panel for hardware it does not have.

| Metric | Type | Notes |
|---|---|---|
| `heliograph_relays_enabled` | gauge | `1` when the relay feature is enabled in the configuration |
| `heliograph_relay_energised` | gauge | `1` per energised relay, label `relay` |
| `heliograph_drm_mode` | gauge | Always `1`; the `mode` label carries the active mode |

The **`relay` label is 0-based**, deliberately: it is the same index as the MQTT topic
(`<base>/<id>/relay/0/state`) and the REST route (`/api/v1/relays/0/set`), so a series in a
dashboard and the topic that switched it line up. The web UI numbers relays from 1 for humans;
the machine interfaces all agree on 0.

`heliograph_drm_mode` follows the standard enum-as-label pattern: exactly one series exists at a
time and its value is always `1`, so `heliograph_drm_mode` in a graph shows *which* mode is
active over time. It is **absent entirely when no DRM roles are configured** — with no roles
there is no curtailment vocabulary to report, and inventing a `normal` would claim a model the
operator never set up.

`relays_enabled` is the configuration flag, not permission to move: a relay also needs
`security.read_only_mode` off before anything actuates. See [drm.md](drm.md).

This is what makes curtailment reviewable after the fact. Graphing
`heliograph_relay_energised` beside `heliograph_inverter_ac_power_watts` shows the contact
closing and the production dropping in the same picture — which is the evidence you want when
deciding whether a curtailment rule is behaving.

The three heap gauges are **internal SRAM only**. Arduino's `ESP.getFreeHeap()` and friends are
`heap_caps_*(MALLOC_CAP_INTERNAL)`, so on the RS485-CAN and Relay-1CH — which carry 8 MB of
external PSRAM — they describe roughly 300 KB of the RAM that exists. `heliograph_psram_*`
covers the rest, and is **omitted entirely** on the Relay-6CH, which has none. Omitted rather
than zero, for the same reason as the RSSI and stack gauges: a flat 0 reads as exhaustion to an
alerting rule. Their absence is also the only way to tell from the network that PSRAM failed to
initialise on a board that should have it.

`max_alloc_heap_bytes` is the fragmentation signal, and it is the more useful of the two heap
numbers on a device meant to run for months: free heap can look healthy while no allocation of
any size still fits. A steady free heap with a falling max-alloc is fragmentation.

The two stack gauges are low-water marks — the smallest amount of stack that task has ever had
left. They only ever go down. If either approaches zero the firmware is close to a stack
overflow, which is how one was caught during development.

## Missing values are missing, not zero

This is the one rule worth understanding before you build a dashboard on this.

**A reading that is unknown, unsupported or stale is left out of the response entirely.** It is
not exported as `0`.

The reason is that Prometheus handles a genuine gap correctly — the series simply has no sample
for that scrape, graphs show a break, and `avg_over_time` ignores it. A zero would be recorded
as a real measurement of nothing, dragging every average down and making "the inverter is
asleep" indistinguishable from "the inverter is producing nothing while awake".

The practical consequence: **do not alert on the absence of a series alone.** At night an
inverter stops answering and its measurement series stop with it. Use `heliograph_inverter_online`
to tell "not answering" from "answering with zero output".

The same rule is why `heliograph_wifi_rssi_dbm` disappears rather than reporting `0` when WiFi
drops: 0 dBm would read as a perfect signal.

## Cardinality

`heliograph_build_info` carries `device`, `version`, `driver` and `board` labels. That is the
complete set — **the inverter's serial number is deliberately not a label**. Serial numbers are
high-cardinality by definition, and putting one in a label would multiply every series by the
number of devices a scraper has ever seen, which is how a small Prometheus turns into a large
one. The serial is available over the REST API instead.

## Example output

An abbreviated real scrape from a production bridge. Note that this one is an RS485-CAN — a
monitoring-only board — which is exactly why there are **no relay or DRM series in it**: on a
board without relays those are omitted rather than reported as zeroes.

> The example below predates the per-device `device="…"` label described above, which
> every device-scoped series carries. Read it for the metric NAMES, not the exact lines.

```
# HELP heliograph_build_info Firmware build information
# TYPE heliograph_build_info gauge
heliograph_build_info{version="0.10.1 (Jul 24 2026 08:31:49)",driver="eversolar_legacy",board="Waveshare ESP32-S3-RS485-CAN"} 1
# HELP heliograph_inverter_online 1 if the inverter is responding
# TYPE heliograph_inverter_online gauge
heliograph_inverter_online 1
# HELP heliograph_data_stale 1 if the last reading is too old to trust
# TYPE heliograph_data_stale gauge
heliograph_data_stale 0
# HELP heliograph_inverter_ac_power_watts Current AC output power
# TYPE heliograph_inverter_ac_power_watts gauge
heliograph_inverter_ac_power_watts 507.000
# HELP heliograph_inverter_energy_today_kwh Energy produced today
# TYPE heliograph_inverter_energy_today_kwh gauge
heliograph_inverter_energy_today_kwh 1.510
# HELP heliograph_inverter_energy_total_kwh Lifetime energy produced
# TYPE heliograph_inverter_energy_total_kwh gauge
heliograph_inverter_energy_total_kwh 35505.800
# HELP heliograph_poll_success_total Successful inverter polls
# TYPE heliograph_poll_success_total counter
heliograph_poll_success_total 275
# HELP heliograph_rs485_timeouts_total RS485 read timeouts
# TYPE heliograph_rs485_timeouts_total counter
heliograph_rs485_timeouts_total 1
# HELP heliograph_wifi_rssi_dbm WiFi signal strength
# TYPE heliograph_wifi_rssi_dbm gauge
heliograph_wifi_rssi_dbm -52
# HELP heliograph_max_alloc_heap_bytes Largest allocatable heap block (fragmentation signal)
# TYPE heliograph_max_alloc_heap_bytes gauge
heliograph_max_alloc_heap_bytes 110580
```

(Several series are left out above for length — a full response carries the complete set
listed earlier.)

## Useful queries

Current output, and today's yield:

```promql
heliograph_inverter_ac_power_watts
heliograph_inverter_energy_today_kwh
```

Inverter efficiency, when the driver reports both sides:

```promql
heliograph_inverter_ac_power_watts / heliograph_inverter_dc_power_watts
```

Poll failure rate over the last hour — the honest health signal for the RS485 link:

```promql
rate(heliograph_poll_failure_total[1h])
  / (rate(heliograph_poll_success_total[1h]) + rate(heliograph_poll_failure_total[1h]))
```

Line noise rather than a dead link:

```promql
rate(heliograph_rs485_checksum_errors_total[15m]) > 0
```

Since the counter moves per transaction this fires while the bridge is still delivering data —
which is the point, but it does mean a *rising* rate on a bus that looks healthy in Home
Assistant is now the expected shape of an early warning, not a contradiction. Corrupt
transactions per poll attempt reads more usefully than a bare rate:

```promql
rate(heliograph_rs485_checksum_errors_total[1h])
  / (rate(heliograph_poll_success_total[1h]) + rate(heliograph_poll_failure_total[1h]))
```

Note this is per **poll**, not per transaction — the number of transactions is not exported, so
on a three-block profile the value ranges 0–3 rather than 0–1. Dividing by successful polls
alone would be worse than imprecise: on the failing bus the query exists to describe, that
denominator stops advancing and the expression goes to `+Inf`.

The bridge rebooted:

```promql
resets(heliograph_uptime_seconds[1h]) > 0
```

How much production a curtailment cost, by pairing the contact with the output it suppressed:

```promql
heliograph_inverter_ac_power_watts and on() heliograph_relay_energised{relay="0"} == 1
```

Which DRM mode was active over the last day — one series per mode that occurred:

```promql
heliograph_drm_mode
```

## Alerting

Two rules worth having, and both need care about night-time.

```yaml
groups:
  - name: heliograph
    rules:
      # The bridge itself is unreachable. Not the same as the inverter being asleep.
      - alert: HeliographDown
        expr: up{job="heliograph"} == 0
        for: 10m
        annotations:
          summary: "Heliograph is not answering scrapes"

      # Corrupted frames mean the wiring or the electrical environment, not darkness.
      # Timeouts are deliberately NOT alerted on: every night produces them.
      - alert: HeliographLineErrors
        expr: rate(heliograph_rs485_checksum_errors_total[15m]) > 0
        for: 15m
        annotations:
          summary: "RS485 frames are arriving corrupted — check ground, termination and cable routing"

      # Fragmentation: free heap can look fine while nothing sizeable still fits.
      - alert: HeliographHeapFragmented
        expr: heliograph_max_alloc_heap_bytes < 20000
        for: 30m
        annotations:
          summary: "Largest allocatable heap block is shrinking"
```

Do **not** alert on `heliograph_inverter_online == 0` without a time-of-day condition: it is
`0` every night, on every solar installation, by design.

## Other integrations

Prometheus is one of several outputs and carries no state the others lack. If you want
long-term energy statistics inside Home Assistant, the MQTT integration is the better route —
see [mqtt.md](mqtt.md). For a machine-readable snapshot in JSON, use
[the REST API](rest-api.md). For building or industrial tooling, there is
[Modbus TCP](modbus-register-map.md).
