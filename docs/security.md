# Security model

This device belongs on a trusted local network. The threat model is "other devices on
the same LAN", not "someone holding the PCB in their hands".

## What is enforced

| Topic | Status |
|---|---|
| Global read-only mode | On by default; a deliberate opt-out in *Settings → Security*. While on, every inverter command and every relay move is refused. **This is the protection that actually holds** — see [What can reach the inverter](#what-can-reach-the-inverter) below, because it is not true that no driver is capable of writing |
| Modbus writes | Off; FC6/FC16 → exception 0x01. `write_enabled=true` is rejected by config validation |
| Raw TCP bridge | Not implemented |
| REST GET | Unsecured (local network) |
| REST PATCH/POST | HTTP Basic required; **rejected** without a configured password, not left open |
| OTA | Same auth + firmware magic check (0xE9) before the first byte hits flash |
| Completing setup | Refuses without an admin password |
| Secrets in logs/REST/MQTT/Prometheus | Never. `serializeConfig()` omits every password **and both usernames** — the MQTT one and `security.admin_username` (not masked, absent); `serializeConfigForStorage()` is the only one that writes them |
| Configuration backup | Passwords omitted unless the operator ticks a box. Admin-gated even redacted. See below |
| Rate limiting | 1 req/s on `/actions/*` |
| Request size | 4096 bytes, rejected with 413 |
| String lengths | Bounded in `validate()`; SSID 32 and PSK 64 are the 802.11/WPA2 limits |
| Hardcoded credentials | None. Verified by scanning the firmware image for strings |

## What can reach the inverter

This section exists because the sentence it replaces was wrong, and wrong in the direction that
matters here: it said no driver in this build can write to an inverter. Two of the four can.

| Driver | Can it write? |
|---|---|
| `eversolar_legacy` | No. `supportsWrite = false`, and the protocol defines no writes at all |
| `solax_x1` | No. `supportsWrite = false` |
| `modbus_profile` | **Declares it and implements it** — `execute()` writes one holding register over FC06 and verifies the device's echo. But every `[[write]]` row in every shipped profile is `verified = false`, and the driver refuses an unverified row, so **no profile writes today** |
| `sunspec` | **Declares it and implements it, and its gate is a device property rather than a review flag.** `execute()` writes the power-limit registers and the connect/disconnect register. It requires that the inverter published SunSpec model 123 and that the block was read. Attach a SunSpec inverter that implements model 123, turn read-only mode off, send the command, and it will write |

**So what actually protects the inverter with the shipped configuration is
`security.read_only_mode`, which is `true` by default and refuses every command before a driver
is reached.** Not an absence of capability. If you are relying on this bridge being unable to
act on your inverter — for a rented installation, a warranty condition, or a site where you are
not the only one with LAN access — rely on that flag, keep it on, and note that it is reachable
by anyone who has the admin password.

The relay/DRM outputs are governed by the same flag plus their own `relays.enabled`, and are
covered in [docs/drm.md](drm.md).

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

**Nothing in the firmware generates a secret, and there is no random number generator in the
tree at all.** The admin password is set by the operator, every request re-presents it, and
there are no session tokens, cookies or nonces to mint. That is a deliberate absence, not an
oversight — a 2026-07-26 audit confirmed no `esp_random`, `random()`, `rand()` or
`mbedtls_ctr_drbg` call anywhere in `src/`.

It is recorded here because the day that changes, the obvious reach is the wrong one. If a
session token, a CSRF nonce, a pairing code or a generated first-boot password is ever added:

- Use `esp_random()` / `esp_fill_random()`. **Never Arduino's `random()`**, which on ESP32 is a
  seeded PRNG and predictable.
- Do it after the RF subsystem is up. `esp_random()` is only a true hardware RNG while WiFi or
  Bluetooth is active or the bootloader's entropy is still valid; during early boot with the
  radio down — exactly when a first-boot password would be minted — the guarantee is weaker.
- Remember that a generated secret has to reach the user somehow, and every channel this
  bridge has (the open setup AP, plain HTTP, the serial console) is one of the exposures listed
  above.

**A configuration backup with `?secrets=true` is a plaintext credential file.** It holds the
WiFi password, the MQTT password and the admin password exactly as NVS stores them. This is the
one place the firmware will hand a password back to anyone, which is why it is opt-in, off by
default, admin-gated, and labelled in the UI with what actually happens to such a file: it lands
in a downloads folder, syncs to whatever cloud drive is watching that folder, and is the obvious
thing to attach to a bug report. Treat it like a copy of the flash, because that is what it is.

The redacted form — the default — carries no password at all, and the keys are removed rather
than emptied, so nothing round-trips back in as a deliberate "clear this password". It is still
admin-gated: it contains no secret, but it does put every other setting in one convenient
document, and unlike `GET /api/v1/config` nothing needs it except a human who is already signed
in.

Restoring cannot leave the bridge without an admin password. A redacted backup keeps whatever
the bridge already has; on a factory-fresh board there is nothing to keep, and that restore is
refused rather than producing a bridge on your WiFi that nobody can reconfigure.

**The rollback copy holds credentials too.** `config.prev` is a full copy of the previous
configuration, secrets included, in the same unencrypted NVS. A factory reset clears the whole
namespace and takes it along — which is what keeps "erases everything, including passwords"
true.

**The update check is integrity-checked, not authenticated.** The dashboard can offer a newer
release and install it in one click. The release feed publishes a SHA-256 per image; the browser
passes it to the bridge with the upload, and the firmware hashes what it actually wrote and
refuses before the boot partition flips.

Be clear about what that buys. It catches a truncated download, a proxy that mangled the body, a
mangled upload across your LAN, a half-written flash. It does **not** prove the image is one this
project published: the hash travels beside the binary on the same host, so anyone able to replace
one can replace the other. It is a checksum, not a signature.

Two smaller guards ride along. The upload carries the board slug and the firmware refuses an
image built for a different board — all three images start with the same magic byte, so nothing
else in the chain could tell them apart, and the wrong one boots happily on the wrong pins
without the rollback net noticing. And SubtleCrypto is deliberately not used in the page: it
only exists in a secure context, and this dashboard is served over plain HTTP, so a check there
would silently not run. The firmware is the right place for it anyway — it also covers the hop
the page cannot see.

**The bridge itself makes no outbound connection for this.** The check runs in your browser,
against the project's GitHub Pages site. Turning it off in *Settings → Firmware release* stops
the background request; the "check now" button still works, because that is a deliberate act.

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
| Read all measurements (REST, Modbus, Prometheus) | Control the inverter — refused while read-only mode is on, which is the default |
| Read the configuration **without secrets** | Read passwords via the API |
| DoS the device with traffic | Disrupt the RS485 polling (separate core, separate task) |
| — | Change settings, OTA, or reboot without the admin password |
