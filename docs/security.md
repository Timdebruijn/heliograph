# Security model

This device belongs on a trusted local network. The threat model is "other devices on
the same LAN", not "someone holding the PCB in their hands".

## What is enforced

| Topic | Status |
|---|---|
| Global read-only mode | On, and cannot be turned off: no driver can write |
| Modbus writes | Off; FC6/FC16 → exception 0x01. `write_enabled=true` is rejected by config validation |
| Raw TCP bridge | Not implemented |
| REST GET | Unsecured (local network) |
| REST PATCH/POST | HTTP Basic required; **rejected** without a configured password, not left open |
| OTA | Same auth + firmware magic check (0xE9) before the first byte hits flash |
| Completing setup | Refuses without an admin password |
| Secrets in logs/REST/MQTT/Prometheus | Never. `serializeConfig()` omits every password **and the MQTT username** (not masked, absent); `serializeConfigForStorage()` is the only one that writes them |
| Rate limiting | 1 req/s on `/actions/*` |
| Request size | 4096 bytes, rejected with 413 |
| String lengths | Bounded in `validate()`; SSID 32 and PSK 64 are the 802.11/WPA2 limits |
| Hardcoded credentials | None. Verified by scanning the firmware image for strings |

## Known limitations — explicit

**Modbus TCP has no encryption, no authentication and no authorization.** That is the
protocol, not our implementation. Only offer it on a trusted or filtered network.

**HTTP Basic over unencrypted HTTP.** The admin password travels base64-encoded (i.e.
readable) over the network. TLS on an ESP32 with an async web server isn't worth the
complexity here; anyone who wants it should put a reverse proxy in front.

**NVS is not encrypted.** The stored configuration contains the wifi and MQTT password in
plain text. Anyone who can read the flash over USB can read them. Flash encryption would solve
this but makes OTA and recovery considerably more complex; given the threat model (LAN, no
physical access) this has not been done. Be aware of it.

**The setup AP is open.** The window is small (until
the first successful connection) but it is a window: anyone within wifi range at that
moment can configure the bridge.

**No brute-force protection on HTTP Basic.** Rate limiting is on `/actions/*`, not on the
auth itself.

**OTA images are not cryptographically signed.** The upload is gated by the admin password
and checked for the ESP32 image magic (`0xE9`) before any byte reaches flash, and a bad image
is rolled back by the bootloader — but the magic check only rejects a wrong *file* (a
filesystem image, an HTML error page a proxy substituted), not a malicious *firmware*. Anyone
with the admin password can flash anything that boots. Signed OTA (secure boot v2 + a signed
app) is the mitigation; it is not enabled because it complicates key management, recovery and
the open-source build, and the threat model is a trusted LAN with no physical access. Keep the
admin password strong and the network trusted.

## What an attacker on the LAN can do

| Can | Cannot |
|---|---|
| Read all measurements (REST, Modbus, Prometheus) | Control the inverter — no driver can write |
| Read the configuration **without secrets** | Read passwords via the API |
| DoS the device with traffic | Disrupt the RS485 polling (separate core, separate task) |
| — | Change settings, OTA, or reboot without the admin password |
