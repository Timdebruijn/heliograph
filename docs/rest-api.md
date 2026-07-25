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

`GET /api/v1/config` **never** returns credential material. Not masked-but-present, but
omitted, with a boolean indicator. This covers every password **and both usernames** — a
username is half of a login pair, so it is treated like the password it accompanies.
Non-credential config (SSID, broker host, topics) stays readable; the UI needs it and it is
not a secret.

`security.admin_username` has no `*_set` companion because it can never be unset: validation
requires it to be non-empty, so the flag would be a constant `true`. Clients that need to
authenticate must ask the user for it rather than read it back; the factory value is `admin`.

```json
{
  "wifi":  { "ssid": "thuis", "password_set": true },
  "mqtt":  { "host": "10.0.0.5", "port": 1883, "username_set": true, "password_set": true },
  "modbus": { "enabled": true, "port": 502, "unit_id": 1, "write_enabled": false },
  "polling": { "interval_seconds": 10 },
  "serial": { "override": false, "baud_rate": 9600, "parity": "none",
              "data_bits": 8, "stop_bits": 1 },
  "security": { "password_set": true, "read_only_mode": false },
  "driver": { "id": "eversolar_legacy", "auto_detect": false },
  "logging": { "level": "info" }
}
```

`PATCH` accepts `"password": "..."` / `"username": "..."` to set either. An omitted field stays
unchanged. Credentials never appear in logs, in SSE, in MQTT, or in Prometheus.

**`null` clears a password, but not a username.** Passwords go through `patchSecret`, which
distinguishes an absent key from an explicit `null` and clears on the latter. Usernames go
through `patchString`, where `null` is indistinguishable from absent and therefore does
nothing — `PATCH {"mqtt":{"username":null}}` returns `200` and leaves the stored value in place.
Send `""` to clear the MQTT username. `security.admin_username` cannot be cleared at all:
`validate()` refuses an empty one.

## `additional_devices` — more than one inverter on the bus

`driver` is device 1. `additional_devices` is a list of devices 2..N, in poll order, each
`{ "driver_id": "...", "options": { ... } }`. Up to eight devices in total. Empty on every
single-inverter install.

```json
{ "additional_devices": [ { "driver_id": "growatt_modbus", "options": { "unit_id": "2" } } ] }
```

Sending the array **replaces** it; omitting it leaves it alone. There is no per-element patch:
the list has no stable key to merge on, and an index the caller believes is element 2 may not be
after someone else's edit. `driver_id` may not be empty — for `driver` an empty id means "pick
the highest-priority driver compiled in", but for an extra device it would be a poll slot that
can never be filled.

Adding, removing or retuning a device needs a restart: the drivers and their poll contexts are
built once, at boot.

**The outputs are not all there yet.** REST, the device list and MQTT/Home Assistant carry every
configured device; **Modbus TCP and Prometheus carry the first one only**, because the register
map holds one device's registers and the metric names have no device label. That is the next
piece of work and is stated in `docs/architecture.md` too.

## `serial` — overriding the line the driver picks

Every driver advertises the serial profiles plausible for its protocol and configures the first
one in `begin()`. `serial.override` replaces that choice, and it exists for one case:
**extended** discovery tries all of a driver's profiles (quick mode only tries the first), so a
device can be found at a profile the driver does not lead with. Saving the driver alone then
meant the next boot went back to the driver's default and the identified inverter fell silent.

`override: false` (the default) means the driver decides — leave it there unless something is
actually wrong. When it is on, the settings are applied immediately after `begin()` — at boot
and again after any discovery run, since discovery reconfigures the line per candidate and
`begin()` then puts it back on the driver's own choice. So the override wins over both the
driver's default and a device profile's own `[serial]` block.

Switching `driver.id` in a request that does **not** itself carry a `serial` block turns the
override off. It was derived from one driver's profile sweep; carried across to another it
forces line settings that were never measured against it. The numbers are kept, so turning it
back on does not mean retyping them.

Bounds when the override is on: `baud_rate` 1200–115200, `data_bits` **8 only**, `stop_bits` 1
or 2, `parity` one of `none` / `even` / `odd`. An unrecognised parity is a `400`, not a silent
fall back to `none`. 7-bit framing is refused rather than accepted: the transport maps a profile
onto the ESP32's `SERIAL_*` constants and handles only the 8-bit ones, so a 7 would land on
`SERIAL_8N1` — losing the parity too — while still reporting success.

There is no `response_timeout_ms` here. Read deadlines are per-driver compile-time constants;
a field on this object would have been a knob that changed nothing.

While the override is off these fields are stored but not validated — they configure nothing.

## Applying changes: `reboot_required`

Most settings — WiFi, MQTT, Modbus, the polling interval, the driver, NTP, the RS485 line
override — are read once at boot, so a `PATCH` stores them but they take effect only after a
restart. A few
(`bridge_name`, `relays.*`, `security.*`, `logging.level`) are applied live.

The `PATCH /api/v1/config` response therefore carries a top-level `reboot_required` boolean so
a client knows whether to call `POST /api/v1/actions/reboot`:

```json
{ "version": 1, "wifi": { ... }, ..., "reboot_required": true }
```

`GET` does not include the field (nothing was changed).

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
