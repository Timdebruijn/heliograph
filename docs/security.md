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
| Secrets in logs/REST/MQTT/Prometheus | Never. `serializeConfig()` omits every password **and both usernames** — the MQTT one and `security.admin_username` (not masked, absent); `serializeConfigForStorage()` is the only one that writes them |
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
data and the inverter's model and serial number, plus everything `GET /config` carries: your
WiFi SSID and hostname, the MQTT host, port, base topic, discovery prefix and QoS, the Modbus
port and unit ids, the bridge name, the poll interval, the NTP server and timezone, the log
level, the driver id **and its options**, the relay roles, the `*_set` booleans saying which
credentials exist — and `security.read_only_mode`, which tells a stranger whether the relay/DRM
write gate is currently open. The Modbus TCP server on port 502 is reachable from that AP too,
and Modbus has no authentication at all.

None of it is a secret — no password is served anywhere (see the table above) — and none of it
grants control. But "an open AP that appears for a few minutes when your WiFi drops" is a
disclosure surface worth knowing about.

**`security.admin_username` used to be in that list and no longer is.** It is half of a login
that has no brute-force protection, and serving it turned guessing the credentials into guessing
only the password. It is now omitted from `GET /api/v1/config` exactly as `mqtt.username` always
was.

Be clear-eyed about the size of this. Almost every install keeps the factory `admin`, and for
those an attacker guesses right on the first try either way — the search space only shrinks for
someone who renamed the account. That is also exactly who pays the cost below. The change is
consistency with how the broker credential is already treated, not a meaningful second factor,
and on a device whose Modbus port has no authentication at all it would be silly to claim more.

It is defence in depth rather than a hole closed: HTTP Basic over plain HTTP puts the password
itself on the wire, so a listener who catches someone signing in has had both halves all along.
What goes away is the much lower bar of one unauthenticated GET — no timing, no waiting for an
admin to appear, and on a bridge nobody ever signs into, that GET was the *only* way to get the
username.

> **If you change the admin username, write it down.** There is no way to read it back — that
> is the whole point of the change — and it is not in the logs, the API, MQTT or Prometheus. A
> forgotten username costs a factory reset (BOOT held ~5 s), which erases WiFi, MQTT, the driver
> selection, relay roles and the timezone along with it. Forgetting the *password* always cost
> that; forgetting the username now does too.

The everyday cost is smaller: the sign-in dialog has a username field defaulting to `admin`, and
the settings field behaves like the other credential fields — blank means keep. The dialog
remembers the last username the bridge actually accepted, but only for that tab and that origin,
so `http://heliograph.local` and `http://192.168.1.50` each ask once, and a second tab asks
again.

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
