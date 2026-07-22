# REST API design

Server: **ESP32Async/ESPAsyncWebServer 3.11.2** (LGPL-3.0) + AsyncTCP 3.4.10.
JSON: ArduinoJson 7.4.3. All handlers are non-blocking and only read a
`shared_ptr<const DeviceState>` snapshot — a slow or failing REST client cannot affect
the RS485 poll.

Versioning is in the path: `/api/v1/`. Breaking changes → `/api/v2/`.

## Endpoints

| Method | Path | Auth | Description |
|---|---|---|---|
| GET | `/api/v1/status` | — | Summary of bridge + device |
| GET | `/api/v1/devices` | — | List device IDs |
| GET | `/api/v1/devices/<id>` | — | Single device |
| GET | `/api/v1/devices/<id>/measurements` | — | All measurements |
| GET | `/api/v1/devices/<id>/capabilities` | — | Capabilities |
| GET | `/api/v1/diagnostics` | — | Diagnostics |
| GET | `/api/v1/drivers` | — | Registered drivers + descriptors |
| GET | `/api/v1/config` | — | Config **without secrets** |
| PATCH | `/api/v1/config` | **✔** | Change config |
| POST | `/api/v1/actions/discover` | **✔** | Start discovery |
| POST | `/api/v1/actions/poll` | **✔** | Force an immediate poll |
| POST | `/api/v1/actions/reboot` | **✔** | Reboot |
| POST | `/api/v1/ota` | **✔** | Firmware upload |
| GET | `/api/v1/events` | — | Server-Sent Events (live updates) |
| GET | `/metrics` | — | Prometheus |

`/api/v1/drivers` is not in the assignment but is needed for the discovery wizard: it
must be able to show the available drivers and their support level without hardcoding
them in the frontend.

There are **no control endpoints** (`/actions/set-power-limit` etc.). Those only appear
once a driver has write capabilities.

## `GET /api/v1/status`

```json
{
  "bridge": {
    "id": "heliograph-a1b2c3",
    "firmware_version": "0.1.0",
    "uptime_seconds": 86400,
    "wifi_rssi_dbm": -57,
    "wifi_connected": true,
    "mqtt_connected": true,
    "modbus_clients": 2
  },
  "device": {
    "id": "eversolar_legacy-XH300060115506193600V610",
    "driver_id": "eversolar_legacy",
    "support_level": "experimental",
    "manufacturer": "Ever-Solar",
    "model": "TL3000-20",
    "online": true,
    "data_valid": true,
    "data_stale": false,
    "last_successful_poll_seconds_ago": 4
  },
  "measurements": {
    "ac.power.total": { "value": 1842.0, "unit": "W", "valid": true, "stale": false }
  }
}
```

## Error format

Uniform, for every error:

```json
{
  "error": {
    "code": "device_not_found",
    "message": "No device with id 'foo'",
    "request_id": "a3f1"
  }
}
```

| HTTP | When |
|---|---|
| 400 | Invalid JSON, unknown config field, value out of range |
| 401 | Auth missing/incorrect on a secured endpoint |
| 404 | Unknown device or path |
| 409 | Discovery already in progress; RS485 bus busy |
| 413 | Body > 4 KB |
| 429 | Rate limit (1 req/s on `/actions/*`) |
| 503 | No valid data yet (cold start) |

Never an HTTP 200 with an error message in the body.

## Secrets

`GET /api/v1/config` **never** returns passwords. Not masked-but-present, but
omitted, with a boolean indicator:

```json
{
  "wifi":  { "ssid": "thuis", "password_set": true },
  "mqtt":  { "host": "10.0.0.5", "port": 1883, "username": "solar", "password_set": true },
  "modbus": { "enabled": true, "port": 502, "unit_id": 1, "write_enabled": false },
  "polling": { "interval_seconds": 10 },
  "driver": { "id": "eversolar_legacy", "auto_detect": false },
  "rs485": { "baud_rate": 9600, "parity": "none", "data_bits": 8, "stop_bits": 1 },
  "logging": { "level": "info" }
}
```

`PATCH` accepts `"password": "..."` to set it and `"password": null` to clear it. An
omitted field stays unchanged.

Passwords never appear in logs, in SSE, in MQTT, or in Prometheus.

## Auth

HTTP Basic over unencrypted HTTP — the device runs on a trusted LAN, and TLS on an
ESP32 with an async web server isn't worth the complexity here. This is stated explicitly in
`docs/security.md` as a limitation.

Default username `admin`; the password **must** be set during provisioning —
no hardcoded default. Without a password, all mutating endpoints refuse with 401.

## SSE

`GET /api/v1/events` sends on every state change (max 1×/s):

```
event: state
data: {"ac.power.total":1842.0,"inverter_online":true,"data_stale":false}
```

Maximum 4 concurrent SSE clients; beyond that, 503. Bounded to protect the heap.
If SSE goes away, the web interface polls `/api/v1/status` every 5 s — SSE is an
optimization, not a dependency.

## Prometheus

```
# HELP heliograph_inverter_ac_power_watts Current AC output power
# TYPE heliograph_inverter_ac_power_watts gauge
heliograph_inverter_ac_power_watts 1842
heliograph_inverter_online 1
heliograph_inverter_energy_today_kwh 8.42
heliograph_inverter_energy_total_kwh 18452.7
heliograph_poll_success_total 4200
heliograph_poll_failure_total 12
heliograph_rs485_checksum_errors_total 2
heliograph_rs485_timeouts_total 4
heliograph_wifi_rssi_dbm -57
heliograph_uptime_seconds 86400
heliograph_build_info{version="0.1.0",driver="eversolar_legacy"} 1
```

Rules: lowercase, snake_case, base unit in the name, counters end in `_total`. The serial
number is **not** a label (high cardinality); it appears in `build_info` only if truly
needed — the recommendation is to leave it out. Invalid measurements are **omitted**,
not published as 0; Prometheus correctly handles a missing sample.

Can be disabled at compile time with `-DENABLE_PROMETHEUS=0`.
