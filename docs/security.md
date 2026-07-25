# Security model

This device belongs on a trusted local network. The threat model is "other devices on
the same LAN", not "someone holding the PCB in their hands".

## What is enforced

| Topic | Status |
|---|---|
| Global read-only mode | On by default; a deliberate opt-out in *Settings → Security*. While on, every inverter command and every relay move is refused. Turning it off unlocks the relay/DRM contacts — no driver in this build can write to an inverter, so nothing reaches the inverter either way |
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

**The setup AP is open**, and it is not limited to first boot: the bridge raises it again on
an already-configured device after repeated WiFi failures (a router reboot reaches that in
about two minutes), because without a reset button that portal is the only way back in.

Provisioning over it is therefore gated on whether a credential exists yet, not on whether
the portal happens to be up. On a factory-fresh device `/api/v1/provision` is open — there is
nothing to protect and no password to present. Once an admin password is set, the same
endpoint requires it even while the portal is running. It was previously open in both cases,
which meant anyone in radio range of a bridge whose WiFi had dropped could overwrite its
configuration, including the admin password and the relay gates.

`/api/v1/wifi/scan` carries the same gate, and the setup page asks for the admin password when
the bridge already has one.

**What the open AP still exposes is read access, and that is not nothing.** The web server
listens on every interface, so anyone joining the failure-portal AP reaches the same
unauthenticated read endpoints a LAN user does: `/api/v1/status`, `/devices`, `/diagnostics`,
`/discovery`, `/drivers`, `/config` (GET) and `/metrics`. Between them that is live production
data, the inverter's model and serial number, your WiFi SSID and hostname, the MQTT host, port
and base topic, the relay roles, and `security.admin_username`. The Modbus TCP server on port
502 is reachable from that AP too, and Modbus has no authentication at all.

None of it is a secret — no password is served anywhere (see the table above) — and none of it
grants control. But "an open AP that appears for a few minutes when your WiFi drops" is a
disclosure surface worth knowing about, and `admin_username` in particular hands out half of a
login that has no brute-force protection. Closing that one is tracked separately.

**The gate has a cost of its own: authenticating over that AP puts the admin password on the
air.** HTTP Basic is base64, not encryption, so a passive listener in radio range of the open
setup AP can read it — and then use it on your LAN. The same is true of the normal web UI over
plain HTTP, but the setup AP is the case where the network itself is open by design. If that
matters to you, do the recovery with the board on a cable-fed network, or factory-reset and
re-provision instead of authenticating over the air.

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
