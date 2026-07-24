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

Add the bridge as a normal static target. It is a small, cheap endpoint (~3.5 kB of text), but
there is no point scraping faster than the inverter is polled — the default poll interval is
10 seconds, so anything under that just repeats a sample.

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

## What is exported

Every series is prefixed `heliograph_`. Base units are in the names, counters end in `_total`.

### Build and health

| Metric | Type | Notes |
|---|---|---|
| `heliograph_build_info` | gauge | Always `1`. Carries labels `version`, `driver`, `board` |
| `heliograph_inverter_online` | gauge | `1` when the inverter is answering, `0` when it is not |
| `heliograph_data_stale` | gauge | `1` when the last reading is too old to trust |

### Inverter readings

All gauges, and all **omitted entirely when the value is unknown** (see
[Missing values](#missing-values-are-missing-not-zero)).

| Metric | Unit |
|---|---|
| `heliograph_inverter_ac_power_watts` | W |
| `heliograph_inverter_dc_power_watts` | W (derived) |
| `heliograph_inverter_ac_voltage_volts` | V (L1) |
| `heliograph_inverter_ac_current_amperes` | A (L1) |
| `heliograph_inverter_grid_frequency_hertz` | Hz |
| `heliograph_inverter_energy_today_kwh` | kWh |
| `heliograph_inverter_energy_total_kwh` | kWh, lifetime |
| `heliograph_inverter_temperature_celsius` | °C |

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

The distinction matters when something is wrong. **Timeouts** mean nothing came back — wiring,
a swapped A/B, or an inverter that is asleep. **Checksum errors and invalid frames** mean bytes
did come back but were corrupted — usually electrical: a missing ground, no termination, or a
cable run alongside something noisy. A quiet night on a solar inverter produces timeouts and no
checksum errors, and that is normal: the inverter powers down after dark.

### Bridge health

| Metric | Type | Notes |
|---|---|---|
| `heliograph_uptime_seconds` | gauge | Seconds since boot |
| `heliograph_free_heap_bytes` | gauge | Free heap |
| `heliograph_max_alloc_heap_bytes` | gauge | Largest single allocatable block |
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

`heliograph_build_info` carries `version`, `driver` and `board` labels. That is the complete
set — **the inverter's serial number is deliberately not a label**. Serial numbers are
high-cardinality by definition, and putting one in a label would multiply every series by the
number of devices a scraper has ever seen, which is how a small Prometheus turns into a large
one. The serial is available over the REST API instead.

## Example output

A real scrape from a production bridge:

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

(Abbreviated — the full response is about 3.5 kB.)

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
