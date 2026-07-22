# Securitymodel

Dit apparaat hoort op een vertrouwd lokaal netwerk. Het dreigingsmodel is "andere apparaten op
hetzelfde LAN", niet "iemand met de printplaat in handen".

## Wat is afgedwongen

| Onderwerp | Stand |
|---|---|
| Globale read-onlymodus | Aan, en niet uit te zetten: geen enkele driver kan schrijven |
| Modbus schrijven | Uit; FC6/FC16 → exception 0x01. `write_enabled=true` wordt door de config-validatie geweigerd |
| Raw TCP-bridge | Niet geïmplementeerd |
| REST GET | Onbeveiligd (lokaal netwerk) |
| REST PATCH/POST | HTTP Basic verplicht; zonder ingesteld wachtwoord **geweigerd**, niet opengelaten |
| OTA | Zelfde auth + firmware-magic-check (0xE9) vóór de eerste byte flash raakt |
| Setup afronden | Weigert zonder adminwachtwoord |
| Secrets in logs/REST/MQTT/Prometheus | Nooit. `serializeConfig()` laat wachtwoorden wég (niet gemaskeerd); `serializeConfigForStorage()` is de enige die ze schrijft |
| Rate limiting | 1 req/s op `/actions/*` |
| Request size | 4096 bytes, geweigerd met 413 |
| Stringlengtes | Begrensd in `validate()`; SSID 32 en PSK 64 zijn de 802.11/WPA2-limieten |
| Hardcoded credentials | Geen. Geverifieerd door de firmware-image op strings te scannen |

## Bekende beperkingen — expliciet

**Modbus TCP heeft geen encryptie, geen authenticatie en geen autorisatie.** Dat is het
protocol, niet onze implementatie. Bied het alleen aan op een vertrouwd of gefilterd netwerk.

**HTTP Basic over onversleuteld HTTP.** Het adminwachtwoord gaat base64-gecodeerd (dus
leesbaar) over het netwerk. TLS op een ESP32 met een async webserver is de complexiteit hier
niet waard; wie dat wel wil, zet er een reverse proxy voor.

**NVS is niet versleuteld.** De opgeslagen configuratie bevat het wifi- en MQTT-wachtwoord in
klare tekst. Wie de flash over USB kan uitlezen, leest ze. Flash-encryptie zou dit oplossen
maar maakt OTA en herstel aanzienlijk complexer; gegeven het dreigingsmodel (LAN, geen fysieke
toegang) is dat niet gedaan. Weet het.

**Het setup-AP is open.** Het venster is klein (tot
de eerste succesvolle verbinding) maar het is een venster: iemand binnen wifi-bereik op dat
moment kan de bridge configureren.

**Geen brute-force-bescherming op HTTP Basic.** Rate limiting zit op `/actions/*`, niet op de
auth zelf.

## Wat een aanvaller op het LAN kan

| Kan | Kan niet |
|---|---|
| Alle metingen lezen (REST, Modbus, Prometheus) | De omvormer aansturen — geen enkele driver kan schrijven |
| De configuratie lezen **zonder secrets** | Wachtwoorden uitlezen via de API |
| Het toestel DoS'en met verkeer | De RS485-polling verstoren (aparte core, aparte taak) |
| — | Instellingen wijzigen, OTA of reboot zonder adminwachtwoord |
