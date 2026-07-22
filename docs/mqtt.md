# MQTT-ontwerp

Client: **espMqttClient 1.7.3** (MIT, non-blocking, QoS 0/1/2, LWT, auto-reconnect).
MQTT is optioneel uitschakelbaar. Valt MQTT weg, dan blijven polling, Modbus TCP, REST en de
webinterface volledig werken — dat is een acceptatiecriterium en wordt in Fase 9 getest door de
broker hard af te sluiten.

`<bridge_id>` = `heliograph-<laatste 3 bytes MAC in hex>`, bijv. `heliograph-a1b2c3`.
Configureerbaar; standaard afgeleid van de MAC zodat twee bridges nooit botsen.

## Topics

| Topic | Retained | QoS | Inhoud |
|---|---|---|---|
| `heliograph/<bridge_id>/availability` | ✔ | 1 | `online` / `offline` |
| `heliograph/<bridge_id>/state` | ✔ | 0 | Metingen + status (JSON) |
| `heliograph/<bridge_id>/diagnostics` | ✔ | 0 | Bridge-diagnostiek (JSON) |
| `heliograph/<bridge_id>/identity` | ✔ | 1 | Device-identiteit (JSON) |
| `heliograph/<bridge_id>/capabilities` | ✔ | 1 | Capabilities (JSON) |

**Last Will and Testament:** topic `availability`, payload `offline`, retained, QoS 1. Bij een
nette afsluiting publiceert de bridge zelf `offline`.

Belangrijk onderscheid: `availability` gaat over de **bridge**, niet over de omvormer. Een
omvormer die 's nachts uit staat maakt de bridge niet offline — dat zou in Home Assistant alle
entities `unavailable` maken en de historie verpesten. De omvormerstatus zit in `state` als
`inverter_online`.

Er zijn **geen command-topics.** De actieve driver heeft geen write-capabilities, dus abonneert
de bridge zich nergens op. Dat is geen configuratie maar een gevolg van
`capabilities.write.none()`.

## Publicatiestrategie

- **Publish-on-change** met deadband, om broker-spam te voorkomen: vermogen ≥ 5 W verschil,
  spanning ≥ 0,5 V, energie bij elke wijziging, status bij elke wijziging.
- **Periodieke forced refresh** elke 60 s (configureerbaar), ook zonder wijziging.
- `identity` en `capabilities` alleen bij verandering of na (her)verbinding.
- JSON-document begrensd: `JsonDocument` met expliciete capaciteitscontrole; overschrijding
  logt een fout en publiceert niet — nooit een afgekapt JSON-bericht.

## `state` — voorbeeld

Alleen `supported` metingen verschijnen. De TL3000-20 heeft geen L2/L3 en geen batterij, dus
die velden bestaan simpelweg niet in de payload:

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

Twee bewuste keuzes t.o.v. het voorbeeld in de opdracht:

1. **`status_text` is `"Unknown (1)"`, niet `"Normal"`.** De betekenis van `OP_MODE` is
   nergens gedocumenteerd — niet in de referentie-implementatie, niet elders. Er wordt geen
   tabel verzonnen. Zodra Fase 3 de codes over een etmaal heeft gelogd, komt er een echte
   mapping.
2. **`error_code` is `null`, niet `0`.** Het protocol kent geen uitleesbaar foutcodeveld. `0`
   zou "geen fout" betekenen en dat weten we niet.

Bij een nachtelijke uitval:

```json
{
  "bridge_online": true,
  "inverter_online": false,
  "data_valid": false,
  "data_stale": true,
  "measurements": { "...": { "value": null, "valid": false, "stale": true } }
}
```

`value: null` — geen nul. Een nul zou in Home Assistant als "0 W geproduceerd" in de statistiek
belanden en de dagcurve vervalsen.

## Home Assistant MQTT Discovery

Prefix `homeassistant/` (configureerbaar). Entities worden **uitsluitend** gegenereerd uit
`state.measurements` + `capabilities` — de discovery-module bevat geen enkele merkspecifieke
regel en heeft geen tabel met EverSolar-velden. Een mapping `MeasurementType`/`Unit` →
`device_class`/`state_class` volstaat.

| Meting | device_class | state_class | Eenheid |
|---|---|---|---|
| `ac.power.total`, `dc.power.total` | `power` | `measurement` | W |
| `*.voltage` | `voltage` | `measurement` | V |
| `*.current` | `current` | `measurement` | A |
| `ac.frequency` | `frequency` | `measurement` | Hz |
| `inverter.temperature` | `temperature` | `measurement` | °C |
| `energy.today`, `energy.total` | `energy` | `total_increasing` | kWh |
| `inverter.operating_hours` | `duration` | `total_increasing` | h |
| `wifi_rssi` | `signal_strength` | `measurement` | dBm |

Discovery-payload per meting:

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

`value_template` levert `None` bij `value: null` → Home Assistant markeert de entity
`unknown` in plaats van 0. Dat is precies gewenst 's nachts.

### Twee devices

- **Bridge** (`heliograph-a1b2c3`): manufacturer "Heliograph open-source project", model
  "Waveshare ESP32-S3-Relay-1CH", firmwareversie. Draagt de diagnostische entities (RSSI,
  uptime, heap, pollteller).
- **Inverter** (`heliograph-a1b2c3_inverter`): manufacturer/model/serienummer uit
  `DeviceIdentity`, met `via_device` naar de bridge. Draagt de metingen.

Zo blijft de HA-devicepagina kloppen als er later een tweede omvormer bijkomt.

### Geen bedieningsentities

`capabilities.write` is leeg → er worden geen `number`-, `switch`- of `select`-entities
aangemaakt. Dit is een lus over de write-bitset, geen driver-check.

## Het `supported`-veld: twee manieren om "niet beschikbaar" te zeggen

| Manier | Betekenis | Gedrag |
|---|---|---|
| Kanaal helemaal niet declareren | Het apparaat heeft dit niet (een string-omvormer heeft geen batterij) | Niets gepubliceerd |
| `declareUnsupported()` | Het apparaat heeft het wél, maar dit protocol/deze firmware levert het niet | Niets gepubliceerd |

Beide worden door élke output identiek behandeld. Het verschil is intentie: de tweede houdt een
vast schema zichtbaar zodat een latere driverversie het kanaal kan invullen zonder dat de vorm
van de measurement-set verandert.

Dit is niet cosmetisch. Zonder `declareUnsupported()` zou `Measurement::supported` altijd `true`
zijn — een vlag die MQTT, Modbus en discovery netjes controleren en die niets ooit op `false`
kan zetten. Dat is geen veiligheid maar een val: de eerste die hem wél op false zet, ontdekt
dat de helft van de outputs het negeerde. Nu is het gedrag afgedwongen door tests.

## Reconnect

Exponentiële back-off 1 s → 2 s → 4 s → … → max 60 s. Na verbinding: availability `online`,
daarna identity/capabilities/discovery, dan state. De pollcyclus draait ondertussen gewoon
door — `mqttTask` en `rs485Task` delen niets behalve de `StateStore`.
