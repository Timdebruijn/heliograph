# REST API-ontwerp

Server: **ESP32Async/ESPAsyncWebServer 3.11.2** (LGPL-3.0) + AsyncTCP 3.4.10.
JSON: ArduinoJson 7.4.3. Alle handlers zijn non-blocking en lezen uitsluitend een
`shared_ptr<const DeviceState>`-snapshot — een trage of falende REST-client kan de RS485-poll
niet raken.

Versionering in het pad: `/api/v1/`. Breaking changes → `/api/v2/`.

## Endpoints

| Methode | Pad | Auth | Omschrijving |
|---|---|---|---|
| GET | `/api/v1/status` | — | Samenvatting bridge + device |
| GET | `/api/v1/devices` | — | Lijst device-ID's |
| GET | `/api/v1/devices/<id>` | — | Eén device |
| GET | `/api/v1/devices/<id>/measurements` | — | Alle metingen |
| GET | `/api/v1/devices/<id>/capabilities` | — | Capabilities |
| GET | `/api/v1/diagnostics` | — | Diagnostiek |
| GET | `/api/v1/drivers` | — | Geregistreerde drivers + descriptors |
| GET | `/api/v1/config` | — | Config **zonder secrets** |
| PATCH | `/api/v1/config` | **✔** | Config wijzigen |
| POST | `/api/v1/actions/discover` | **✔** | Discovery starten |
| POST | `/api/v1/actions/poll` | **✔** | Directe poll forceren |
| POST | `/api/v1/actions/reboot` | **✔** | Herstart |
| POST | `/api/v1/ota` | **✔** | Firmware-upload |
| GET | `/api/v1/events` | — | Server-Sent Events (live updates) |
| GET | `/metrics` | — | Prometheus |

`/api/v1/drivers` staat niet in de opdracht maar is nodig voor de discoverywizard: die moet de
beschikbare drivers en hun supportniveau kunnen tonen zonder ze hard te coderen in de frontend.

Er zijn **geen control-endpoints** (`/actions/set-power-limit` e.d.). Die verschijnen pas
wanneer een driver write-capabilities heeft.

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

## Foutformaat

Uniform, voor elke fout:

```json
{
  "error": {
    "code": "device_not_found",
    "message": "No device with id 'foo'",
    "request_id": "a3f1"
  }
}
```

| HTTP | Wanneer |
|---|---|
| 400 | Ongeldige JSON, onbekend configveld, waarde buiten bereik |
| 401 | Auth ontbreekt/onjuist op beveiligd endpoint |
| 404 | Onbekend device of pad |
| 409 | Discovery al bezig; RS485-bus bezet |
| 413 | Body > 4 KB |
| 429 | Rate limit (1 req/s op `/actions/*`) |
| 503 | Nog geen geldige data (koude start) |

Nooit een HTTP 200 met een foutmelding in de body.

## Secrets

`GET /api/v1/config` retourneert **nooit** wachtwoorden. Niet gemaskeerd-maar-aanwezig, maar
weggelaten, met een booleaanse indicatie:

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

`PATCH` accepteert `"password": "..."` om te zetten en `"password": null` om te wissen. Een
weggelaten veld blijft ongewijzigd.

Wachtwoorden komen niet in logs, niet in SSE, niet in MQTT, niet in Prometheus.

## Auth

HTTP Basic over onversleuteld HTTP — het apparaat draait op een vertrouwd LAN en TLS op een
ESP32 met async webserver is de complexiteit hier niet waard. Dit staat expliciet in
`docs/security.md` als beperking.

Standaard gebruikersnaam `admin`; wachtwoord wordt bij provisioning **verplicht** ingesteld —
geen hardcoded default. Zonder wachtwoord weigeren alle muterende endpoints met 401.

## SSE

`GET /api/v1/events` stuurt bij elke state-wijziging (max 1×/s):

```
event: state
data: {"ac.power.total":1842.0,"inverter_online":true,"data_stale":false}
```

Maximaal 4 gelijktijdige SSE-clients; daarboven 503. Begrensd om de heap te beschermen.
Valt SSE weg, dan pollt de webinterface `/api/v1/status` elke 5 s — SSE is een optimalisatie,
geen afhankelijkheid.

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

Regels: lowercase, snake_case, basiseenheid in de naam, counters op `_total`. Het serienummer
is **geen** label (high cardinality); het staat in `build_info` alleen als het echt nodig is —
voorstel is het weg te laten. Ongeldige metingen worden **weggelaten**, niet als 0
gepubliceerd; Prometheus gaat correct om met een ontbrekende sample.

Compile-time uit te schakelen met `-DENABLE_PROMETHEUS=0`.
