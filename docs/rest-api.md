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
| GET | `/api/v1/config/backup` | **✔** | Download the whole configuration as a file (`?secrets=true` to include passwords) |
| POST | `/api/v1/config/restore` | **✔** | Apply a backup file (`?dry_run=true` to preview only) |
| POST | `/api/v1/actions/undo-restore` | **✔** | Swap back to the configuration from before the last restore |
| POST | `/api/v1/actions/discover` | **✔** | Start discovery |
| POST | `/api/v1/actions/capture` | **✔** | Record raw RS485 traffic (`?seconds=&baud=&parity=&frames=`) |
| GET | `/api/v1/capture` | — | The last capture, with per-frame hex and checksum verdicts |
| POST | `/api/v1/actions/poll` | **✔** | Force an immediate poll |
| POST | `/api/v1/actions/reboot` | **✔** | Reboot |
| POST | `/api/v1/actions/clear-coredump` | **✔** | Discard a stored crash dump (404 when there is none) |
| POST | `/api/v1/ota` | **✔** | Firmware upload (`?board=` and `?sha256=` optional; both refuse a mismatch) |
| GET | `/api/v1/events` | — | Server-Sent Events (live updates) |
| GET | `/metrics` | — | Prometheus |

Each driver in `/api/v1/drivers` declares its own options, and the web UI renders them
generically from that declaration — a new driver's settings appear with no frontend change.
An option is one of three shapes:

| Shape | Declared as | Rendered as | Refused when |
|---|---|---|---|
| Free text | neither bounds nor `allowed_values` | text field | never |
| Enumerated | `allowed_values` | select | not one of them |
| Bounded number | `min_value` / `max_value` | number field with those limits | not a plain whole number, or outside the range |

The bounded shape exists because a driver's own parser is too late to be the only check. An
address outside the protocol's range used to pass config validation — which only length-checks
option strings — get stored, and then fall back to the driver's default at boot with a single
log line — for two of the three drivers; SunSpec's fallback had no log line at all. On a bus of
identical inverters that default is the address the *first* one already uses, so a typo became
an id collision, and the diagnosis pointed at a duplicate address the configuration does not
contain. `PATCH` refuses it now, before the reboot.

"Plain" is strict on purpose: `" 7"`, `"+7"` and `"007"` all parse, and were then stored
verbatim — which is how `007` slipped past a duplicate-address check that compares strings.
They are refused rather than normalised, because silently rewriting what someone typed is the
substitution these bounds exist to remove.

A value **already stored** when the bounds arrived is a different case: it is healed back to the
option's default on the next `PATCH`, exactly as an unrecognised enumerated value is. Validating
the merged configuration without that would have made a pre-existing value refuse every later
save, including ones touching nothing driver-related.

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
    "modbus_clients": 2,
    "max_devices": 8,
    "devices_configured": 3,
    "device_problems": ["'growatt_modbus' shares the address of a device already added (growatt_modbus-2); give them different addresses"]
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
  },
  "devices": [
    { "id": "growatt_modbus-1", "label": "Schuur", "online": true, "data_valid": true,
      "data_stale": false, "last_successful_poll_seconds_ago": 3, "ac_power_w": 1240.0 },
    { "id": "growatt_modbus-2", "online": false, "data_valid": false, "data_stale": false,
      "last_successful_poll_seconds_ago": null, "ac_power_w": null }
  ],
  "totals": {
    "devices_polled": 2, "devices_answering": 1,
    "ac_power_w": 1240.0, "ac_power_devices": 1,
    "energy_today_kwh": 5.25, "energy_today_devices": 1,
    "energy_total_kwh": 24680.0, "energy_total_devices": 1
  }
}
```

`label` is the name an operator gave the device on the settings page. It is **optional and
additional**: absent when nothing was set, and never a replacement for `id`. `id` is the key —
it addresses `/api/v1/devices/<id>`, the MQTT topic tree and the Modbus unit mapping — so a
client that followed the label would 404 the first time somebody renamed an inverter. Show the
label, address by the id. Renaming is safe precisely because nothing keys on it.

`device` and `measurements` describe the **first** device and always will — that is what existing
clients read; `devices[0]` is built from the same snapshot, so the two cannot contradict each
other within one response. `devices` and `totals` cover every **started** device, and are what
anything presenting itself as the bridge's health must use. Compare them against
`devices_configured`, not against each other: a device that failed to start has no row at all,
and the built-in web UI counts it as not reporting rather than leaving it out of the question.

Four rules, all of which exist because the alternative reads as a fact:

- **A reading counts only while it is valid and fresh.** When a device goes offline the firmware
  marks its measurements stale but keeps them *valid*, so a summary that checked validity alone
  kept a dead inverter's last daylight value in the household total until the next reboot. Both
  `devices[].ac_power_w` and the sums go `null` instead; `last_successful_poll_seconds_ago` is
  what the reading has been replaced by.
- **`devices_answering`** counts devices that are online, hold valid data and are not stale.
  Started is not answering (see below), and neither is a device whose reading has aged out.
- **A sum is `null` when no device reported the channel**, never `0`.
- **Every sum carries its own count** (`ac_power_devices` and friends). A total over two of three
  inverters is indistinguishable from a total over three that had a bad afternoon; the count is
  the difference. A device that reports no value at all does not contribute a zero to the sum.

Per-device rows use `null` for an absent reading for the same reason: "reports no power" and
"reports 0 W" are different answers.

Overnight, therefore, every sum is `null` with a count of `0` — the bridge reports that it does
not know, not that the sun produced 0 W and not yesterday's last reading.

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
| 413 | Body > 4 KB (8 KB on `/config/restore`, which carries a whole configuration) |
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
  "wifi":  { "ssid": "thuis", "password_set": true,
             "ip": "", "gateway": "", "subnet": "", "dns1": "", "dns2": "" },
  "mqtt":  { "host": "10.0.0.5", "port": 1883, "username_set": true, "password_set": true },
  "modbus": { "enabled": true, "port": 502, "unit_id": 1, "write_enabled": false },
  "polling": { "interval_seconds": 10 },
  "serial": { "override": false, "baud_rate": 9600, "parity": "none",
              "data_bits": 8, "stop_bits": 1 },
  "security": { "password_set": true, "read_only_mode": false },
  "driver": { "id": "eversolar_legacy", "auto_detect": false, "label": "Schuur" },
  "logging": { "level": "info" }
}
```

## Addressing

`wifi.ip` empty means DHCP, and that is the whole switch — there is no separate enable flag.
Setting it makes the other four fields meaningful; clearing it must clear them too, or the
`PATCH` is refused rather than storing a half-configuration that reads as if it were in effect.

`GET /api/v1/status` reports both `bridge.ip_mode` (`"dhcp"` or `"static"` — how the address was
**asked for**) and `bridge.ip_address` (what the interface **ended up with**, absent while none
has been assigned). They are separate on purpose: on a bridge whose static address the driver
refused, they disagree, and that disagreement is the only visible sign of it.

Changing any addressing field needs a restart, like the rest of the network settings.

**A wrong static address does not fail loudly.** The association still succeeds — WiFi is layer 2
and addressing is layer 3 — so the bridge joins the network and is simply unreachable. `PATCH`
therefore refuses everything it can see from here: anything that is not four decimal octets, a
mask with a hole in it or not starting at 255, an address naming the network or the broadcast, a
gateway outside the subnet or equal to our own, and leftover fields after the address was
cleared. Two more rules exist because their failure is silent rather than loud:

- a static address with **no DNS server** while an NTP server or MQTT broker is configured by
  name — the stack would resolve nothing, the clock would never sync, and nothing would say so
- a static address with NTP enabled and **no `ntp.server`**, even with `ntp.use_dhcp` on: there
  is no lease to provide one

What no check can know is whether the address is already taken. If a bridge does go missing,
hold BOOT for five seconds to factory-reset and start again from the setup portal.

`PATCH` accepts `"password": "..."` / `"username": "..."` to set either. An omitted field stays
unchanged. Credentials never appear in logs, in SSE, in MQTT, or in Prometheus.

**`null` clears a password, but not a username.** Passwords go through `patchSecret`, which
distinguishes an absent key from an explicit `null` and clears on the latter. Usernames go
through `patchString`, where `null` is indistinguishable from absent and therefore does
nothing — `PATCH {"mqtt":{"username":null}}` returns `200` and leaves the stored value in place.
Send `""` to clear the MQTT username. `security.admin_username` cannot be cleared at all:
`validate()` refuses an empty one.

`devices_configured` is how many devices the **current** configuration asks for; `devices_started`
is how many the firmware built at the last boot. They differ both when something failed and in
the window between saving a new device and restarting. `device_problems` says why for each
missing one — the same sentences the boot log carries, each naming the configuration row
("device 3"), because several rows normally share one driver id. The array is always present,
empty when there is nothing to report, so "no problems" cannot be mistaken for "this firmware
does not report them".

**Started is not answering.** A driver whose `begin()` succeeded counts as started whether or
not the inverter has ever replied — with A and B swapped it never will. Per device,
`/api/v1/devices/<id>` carries `online`, `data_valid`, `data_stale`,
`consecutive_poll_failures` and `last_successful_poll_seconds_ago` (`null` when it has never
answered, which is a different thing from `0`).

`GET /api/v1/status` answers **200 even when nothing is polling**, with an empty device object.
That is the case where these fields matter most, and refusing the payload for lack of a device
to describe hid the explanation behind a "cannot reach the bridge" error.

## `additional_devices` — more than one inverter on the bus

`driver` is device 1. `additional_devices` is a list of devices 2..N, in poll order, each
`{ "driver_id": "...", "options": { ... } }`. Up to eight devices in total. Empty on every
single-inverter install.

```json
{ "additional_devices": [ { "driver_id": "growatt_modbus", "options": { "unit_id": "2" } } ] }
```

The mock driver takes the same `unit_id`, so a fleet can be simulated without hardware — each
instance gets its own device id, topic subtree and Modbus unit, and its solar curve is staggered
so the instances do not all report the same value at once:

```json
{ "driver": { "id": "mock_inverter", "options": { "unit_id": "1" } },
  "additional_devices": [ { "driver_id": "mock_inverter", "options": { "unit_id": "2" } },
                          { "driver_id": "mock_inverter", "options": { "unit_id": "3" } } ] }
```

Sending the array **replaces** it; omitting it leaves it alone. There is no per-element patch:
the list has no stable key to merge on, and an index the caller believes is element 2 may not be
after someone else's edit. `driver_id` may not be empty — for `driver` an empty id means "pick
the highest-priority driver compiled in", but for an extra device it would be a poll slot that
can never be filled.

Adding, removing or retuning a device needs a restart: the drivers and their poll contexts are
built once, at boot.

**Every output carries every device**, each with its own dimension: REST keys them by device id,
MQTT/Home Assistant by topic subtree, **Modbus TCP by unit id** (`modbus.unit_id` plus the index,
same order as `/api/v1/devices`) and **Prometheus by a `device` label**. See
`docs/modbus-register-map.md` and `docs/prometheus.md`; `docs/architecture.md` has the table of
what each one did about backwards compatibility.

## Discovery: what each mode actually probes

| | Serial profiles | Bus addresses | Probes, drivers in this build |
|---|---|---|---|
| **Quick** | the driver's first recommended profile | none — the driver's own default, with no option set | 4, ≈ 8 s |
| **Extended** | all of them, stopping at the first profile that answers | the driver's default, then **1–8** | up to 33, ≈ 35 s |

Quick mode's cost is unchanged — one probe per driver — because it is the mode that runs by
default, sometimes on a bus that is already in service. It also passes **no options at all**, so
a quick candidate never claims an address it did not discover: the wizard prefers a discovered
address over the stored configuration, and a candidate carrying the driver's default would have
quietly proposed unit id 1 to a bridge configured at 7.

The extended numbers are the worst case, on a bus where nothing answers: 16 probes each for the
two Modbus drivers (2 profiles × 8 addresses), one for the PMU driver, at roughly a second of
response timeout apiece. Polling is stopped for the whole run. On a bus with devices it is much
faster — the profile is settled at the first address that answers and the rest are probed at
known-good line settings.

Extended sweeps addresses because a chain of identical inverters is the case the wizard was
worst at: only the unit at the default address could ever be a candidate, and the address is
exactly the field a typo makes invisible. Each candidate carries the `options` it answered at
and an `address`, and the report carries `swept_addresses` — so "nothing else answered" can be
told apart from "nothing else was asked". A device parked outside 1–8 is still configured by
hand; the wizard says which addresses it tried rather than implying the bus is empty.

Why 1–8 and not 1–247: the bridge polls at most eight devices, and probing 247 addresses across
every driver and profile is hours of traffic for addresses nothing could use. A driver's own
default is included on top of the range, as long as the driver would accept it.

**Only an address the device already has is swept.** A driver names it (`addressOptionKey`), and
naming it is a claim about what the option means, not just where it lives. SolaX's `address` is
the address the bridge *hands out* at registration, so it is deliberately not named: sweeping it
would assign nine addresses in a row and leave the inverter on the last one while the report
described the first. A driver that names nothing, or names an option that is not a declared
numeric one, is probed once.

**Traffic without a device is reported, not discarded.** `unidentified_addresses` lists any
address where bytes came back that identified nothing — which on a chain of identical inverters
is what two units left on the same unit id look like: their replies collide into a failed
checksum. That is the one fault a sweep can diagnose that nothing else can, and when it is all
there is, the report says so instead of blaming the wiring.

`candidates` is capped at ten, lowest scores dropped first, with `candidates_omitted` saying how
many. Sixteen candidates at half a kilobyte each would exceed the 8 KB response bound, and a
refused body reads to the wizard as "nothing answered" after a minute of probing.

Two consequences worth knowing:

- **One driver can now produce several candidates**, one per device. The "too close to call"
  rule only compares *different* drivers: three inverters of the same make are not ambiguous
  about which protocol they speak, and treating them as such would have made the sweep defeat
  auto-selection on exactly the bus it was built for. The wizard still configures one device and
  says how many answered.
- **The same serial number at two addresses is reported once**, at the lower address, with the
  other recorded in its evidence. That is one inverter mid-reconfiguration, and configuring it
  twice would collide at boot.

A full extended sweep on a silent bus is dozens of response timeouts back to back, all with
polling stopped. It is a user-requested operation and it is slow; the task watchdog is fed per
probe rather than per run. Note that the "stop at the first profile that answers" saving only
applies to a driver that *does* answer — for every driver that finds nothing, profiles and
addresses multiply in full, and that is where the bulk of the time on a mixed bus goes.

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

## Capturing an unknown device

`POST /api/v1/actions/capture` records raw RS485 traffic without needing a driver that
understands it. Contributor-facing; [docs/adding-a-device.md](adding-a-device.md#6-capturing-an-unknown-device)
is the guide, this is the wire contract.

| Parameter | Default | Range |
|---|---|---|
| `seconds` | 30 | 1–300 |
| `frames` | 64 | 1–256 |
| `baud` | 9600 | 300–921600 |
| `parity` | `none` | `none`, `even`, `odd` |
| `data_bits` | 8 | 5–8 |
| `stop_bits` | 1 | 1–2 |

202 on acceptance; **409** when a capture or a discovery run is already using the bus. The guard
is symmetric — `/actions/discover` refuses while a capture is pending or running too. It has to
be: rs485Task checks discovery first, so a discovery accepted alongside a pending capture would
jump the queue and the capture would then record the tail of the probe run.

The capture runs on the task that owns the bus, **instead of** that cycle's poll — the same
handover discovery uses. That is the concurrency answer: there is no iteration in which the bus
is being listened to and polled, so the two cannot interleave. The bridge transmits nothing for
the whole window.

`GET /api/v1/capture` returns the report, poll it for progress:

```json
{
  "status": "done",
  "line": { "baud_rate": 9600, "parity": "none", "data_bits": 8, "stop_bits": 1,
            "idle_gap_ms": 4 },
  "requested_seconds": 30, "max_frames": 64, "elapsed_ms": 30012,
  "summary": { "frames": 12, "bytes": 143, "modbus_crc_ok": 12, "aa55_frames_ok": 0,
               "truncated": false },
  "frames": [
    { "offset_ms": 0, "gap_before_ms": 0, "length": 8, "modbus_crc_ok": true,
      "aa55_ok": false, "hex": "01 03 00 00 00 0A C5 CD" }
  ]
}
```

**`summary.modbus_crc_ok` is the number to read first.** A capture at the wrong baud rate
produces plenty of bytes and zero valid checksums, and without that count the operator
concludes the device is mute when it is merely being listened to at the wrong speed.

The line settings are echoed in full because on an unidentified device they are a *guess*, and
every number in the report is only meaningful against the guess that produced it.

Frames are cut on `idle_gap_ms` of silence, derived from the baud rate (Modbus t3.5) and
floored at 2 ms. Above 19200 the true gap is finer than the read loop can resolve, so adjacent
frames may merge into one record — the byte stream stays complete and ordered, only the cut
points are approximate. Stated here rather than left to be discovered from confusing output.

The report is bounded at 64 KB and the capture stops when full rather than dropping the oldest:
for a handshake the interesting part is at the beginning, and a ring buffer would reliably
discard exactly that. `summary.truncated` says which happened.

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

## Backup and restore

The backup file is a thin envelope around the exact document the bridge stores in NVS. That
matters more than it sounds: the field list has one point of truth, so a setting added in a
later firmware lands in the backup without anyone remembering to add it, and reading a file
back reuses the same parser the bootloader uses — including its migration chain, so a backup
taken on an older firmware is upgraded on the way in.

```json
{
  "format": "heliograph-config-backup",
  "format_version": 1,
  "firmware_version": "0.13.2",
  "exported_at": "2026-07-26T12:00:00Z",
  "includes_secrets": false,
  "bridge_name": "Shed bridge",
  "config": { "version": 1, "wifi": { "ssid": "HomeNet", "hostname": "shed" }, "...": "..." }
}
```

`format_version` versions the envelope; `config.version` versions the configuration inside it,
and the two are checked separately. A file from a newer firmware is **refused**, not
reinterpreted — the same rule NVS loading follows, and for the same reason: a half-understood
configuration looks plausible.

### Secrets are opt-out by default

`GET /api/v1/config/backup` omits every password. `?secrets=true` includes them, in plain text.

What makes the redacted file usable rather than merely safe is the merge rule: **an absent
password means "keep the one the bridge already has"**. So a redacted backup restored onto a
running bridge is complete. Only a factory-fresh board has nothing to inherit — and a restore
that would leave such a board with no admin password is refused outright (`password_required`),
because the alternative is a bridge on your WiFi that nobody can change and only a five-second
BOOT hold can recover.

A password that is *present but empty* is applied. That is a deliberately empty credential (an
open WiFi network), not a redaction, and the two are told apart by whether the key exists.

### Preview, then apply

`POST /api/v1/config/restore?dry_run=true` returns what would change and applies nothing:

```json
{
  "backup": { "format_version": 1, "firmware_version": "0.13.2",
              "exported_at": "2026-07-26T12:00:00Z", "includes_secrets": false },
  "change_count": 2,
  "reboot_required": true,
  "rollback_exists": true,
  "changes": [
    { "field": "mqtt.host", "before": "old.broker", "after": "new.broker" },
    { "field": "polling.interval_seconds", "before": "10", "after": "30" }
  ]
}
```

The diff is computed against the **merged result**, not against the file, so it tells the truth
about a redacted backup: those credentials show as unchanged, because leaving them alone is
what applying it will do. Password values never appear — any key named `password` or ending in
`_password` renders as `(set)` / `(not set)`, as a rule rather than a list, so a credential
added later is redacted before anyone has to remember it should be.

Nothing is staged on the bridge between the two calls. The apply re-sends the file rather than
confirming a token, so there is no server-side session to expire, to be raced by a second
browser tab, or to apply something other than what was on screen.

`rollback_exists` says whether an undo point is stored **right now**, from an earlier restore —
applying replaces it, so the way back becomes the configuration the bridge has at this moment
rather than the older one. It deliberately does *not* claim that an undo will exist afterwards:
that cannot be known before the copy is attempted, because attempting it is the only thing that
discovers whether it fits. `rollback_stored` in the result answers that, after the fact.

Dropping `?dry_run=true` applies it and reboots when `reboot_required`:

```json
{ "status": "restored", "changed_fields": 2, "reboot_required": true, "rollback_stored": true }
```

### The undo

Immediately before a restore overwrites it, the current configuration is copied to a second NVS
key. `POST /api/v1/actions/undo-restore` **swaps** the two back — pressing it twice returns to
the restored configuration, which is what makes the button honest rather than silently doing
nothing the second time.

Only a restore creates a restore point; an ordinary `PATCH` does not. Writing one on every save
would double the flash wear and, worse, make "the configuration from before the restore" mean
nothing in particular.

`rollback_stored: false` means the copy did not fit. NVS here is 20 KB shared with the WiFi
stack's own calibration data, so a second copy of the configuration is the first thing that will
not fit — and the restore still goes ahead, because losing the safety net is a smaller harm than
refusing the operation the safety net was for.

### Restoring during setup

The setup portal offers the same restore, which is the context that makes the feature worth
having: a factory-reset or replacement board takes a file instead of twenty fields typed by
hand.

It carries the same gate as `/api/v1/provision`, and for the same reason — the portal is **not**
exclusive to first boot. It returns on an already-configured bridge after repeated WiFi failures
(a router reboot reaches that in about two minutes) and its AP is open. So the gate is "does an
admin password exist yet", not "is the portal up": open on a factory-fresh board, authenticated
afterwards.

## Firmware updates

`GET /api/v1/status` carries `bridge.board_id` — a stable slug (`rs485-can`, `relay-1ch`,
`relay-6ch`) matching the PlatformIO environment suffix and the release asset name. The display
name in `board_name` is for people and may be reworded; this one is what a client keys on to ask
for the right image.

`POST /api/v1/ota` takes two optional query parameters:

| Parameter | Effect |
|---|---|
| `board` | Refused with 400 `wrong_board` when it does not match this bridge's `board_id` |
| `sha256` | The image must hash to this, or the upload is refused with `hash_mismatch` **before** the boot partition flips |

Both are optional because a hand-picked `.bin` from the settings page has nothing to compare
against. The dashboard's one-click update supplies both.

`hash_mismatch` is told apart from `write_failed` deliberately: the latter means the flash
refused, the former means the flash did as it was told and the bytes were wrong. The error
message carries both digests, so a corrupted download can be told from the wrong file entirely.

The board check is on metadata, not on the binary — it stops a stale link or a UI bug, not a
client sending whatever it likes. The hash is **integrity, not authenticity**; see
[docs/security.md](security.md).

The check for a newer release runs in the browser, not on the bridge, against the project's
GitHub Pages site. Release assets could not be used: their download redirects to a host that
sends no CORS headers at all, so `fetch()` from a page served off the bridge cannot read them.
Pages sends `access-control-allow-origin: *`, which is also why the web flasher's image is
served from there.

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
