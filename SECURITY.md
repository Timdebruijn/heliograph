# Security policy

Heliograph is firmware for a small board that sits on a home network next to a solar
inverter. It reads the inverter over RS485 and serves the result on that network: a web
UI and REST API (port 80), Prometheus metrics (`/metrics`, port 80), Modbus TCP (port 502),
and MQTT as a client of your broker. When its owner switches on *Allow control* it can also
send commands to the inverter. The people running it are usually not the people who
installed their inverter, and a flaw here can reach equipment that is hard to reset, so
please report problems privately.

## Reporting a vulnerability

Use GitHub's **private vulnerability reporting** on this repository
(Security › Report a vulnerability). Please do not open a public issue or pull request for
a security problem.

Useful to include:

- the firmware version (web UI, or `firmware_version` in `GET /api/v1/status`) and the board
  (RS485-CAN, Relay-1CH or Relay-6CH);
- the inverter and the protocol or driver in use, if it matters;
- what an attacker needs: network position (same LAN, setup access point, a broker client),
  the admin password, physical access;
- the smallest request, packet or file that shows it.

This is a one-person project. Reports are read as soon as there is time, with no promised
response time. A confirmed fix ships in a new release, and the advisory credits you unless
you ask otherwise.

## Supported versions

Heliograph is pre-1.0. Only the latest release and `main` get fixes; older releases are not
patched, so update. Updates are delivered as OTA firmware images, see below.

## Scope and known limits

The threat model is other devices on the same LAN, not someone holding the board. The
following is expected behaviour, documented in [docs/security.md](docs/security.md), and is
not a vulnerability on its own.

**Network services and who may use them**

| Service | Port | Authentication |
|---|---|---|
| Web UI, REST `GET`, SSE (`/api/v1/events`), `/metrics` | TCP 80 | None. Read endpoints are open by design, and carry live data, the inverter's model and serial number, and the configuration without secrets |
| REST writes (`PATCH`/`POST`: settings, actions, commands, relays, OTA, backup, restore) | TCP 80 | HTTP Basic with the admin password. Refused outright while no password is set |
| Modbus TCP server | TCP 502 (configurable), on by default | None, as the protocol has none. Read-only: write function codes are answered with an exception and cannot be switched on |
| MQTT | outbound to your broker, off until configured | Whatever the broker enforces. The bridge does not authenticate the messages it receives |
| mDNS | UDP 5353 | `<hostname>.local` and an `_http._tcp` record on port 80 |
| Setup access point | open WiFi, DNS on UDP 53 | Open by design; see below |

- **No transport encryption.** The web UI and REST API are plain HTTP, so the admin password
  (HTTP Basic) and everything else travels readable. MQTT has no TLS option in the firmware
  either: the broker password and the command topics are in clear. Put a reverse proxy in
  front of the web UI if you need HTTPS, and keep the bridge and the broker on a network you
  trust.
- **Modbus TCP and the read endpoints are unauthenticated by design.** Anyone on the LAN can
  read measurements. Reading is not control.
- **Commands over MQTT are only as protected as the broker.** Once control is allowed,
  whoever may publish to the bridge's `command/set`, `relay/<n>/set` or `drm/set` topics can
  steer it. Restrict those topics with broker ACLs.
- **The setup access point is open, and it comes back.** It runs on a factory-fresh board
  and again on a configured one after repeated WiFi failures. Provisioning and restore over
  it are open only while no admin password exists; once one is set they require it. Anyone
  in radio range can still use the unauthenticated read endpoints, and a password typed over
  that network crosses the air in HTTP Basic.
- **Credentials are stored unencrypted.** WiFi, MQTT and admin passwords are in plain text in
  the board's NVS flash, and a copy of the previous configuration is kept there too. Flash
  encryption is not enabled. Anyone who can read the flash over USB can read them. Passwords
  are never returned by the REST API, MQTT or Prometheus; the one exception is a
  configuration backup downloaded with `secrets=true`, which is opt-in and admin-gated.
- **Firmware updates are integrity-checked, not signed.** An upload needs the admin password.
  The firmware checks the ESP32 image magic byte before the first byte reaches flash, refuses
  an image built for another board, verifies a SHA-256 when the caller supplies one, and the
  bootloader rolls back an image that does not come up healthy. The dashboard's "update
  available" check runs in your browser against the project's GitHub Pages site and passes
  the digest from the same feed, so it catches corruption, not a malicious host. Secure boot
  and signed images are not enabled. Anyone holding the admin password can flash any image
  that boots.
- **Control is opt-in, and off by default.** `security.read_only_mode` is on out of the box
  (the web UI shows it inverted as *Allow control*) and refuses every inverter command and
  relay move. Under it each path has its own switch, also off: `relays.enabled` for the DRM
  contacts on relay boards, `control.inverter_writes_enabled` for inverter setpoints; and a
  setpoint that is not yet verified on hardware needs a per-device
  `allow_unverified_writes` as well. With every gate open, what is possible is what the
  inverter's protocol allows: asserting a DRM mode (DRM0 switches the inverter off),
  limiting its output, connecting or disconnecting it, or writing a register on a device whose
  map has not been validated on hardware. Anyone with the admin password can open the gates.
- **Discovery of EverSolar and SolaX inverters transmits.** Those protocols find a device by
  assigning it a bus address, so discovery sends a re-register broadcast and an address
  assignment even with *Allow control* off. A second inverter of that family on the same
  RS485 line will forget its address and re-register.
- **Brute-force protection is per source address.** Five wrong admin passwords lock an address
  out with a growing delay, and twenty a minute across all addresses lock out every sign-in
  for 60 s. Behind a reverse proxy the "address" is the proxy. The counters live in RAM.
- **Denial of service on the LAN** (flooding port 80 or 502, holding the four SSE slots) can
  degrade the web UI and outputs. RS485 polling runs in its own task and is designed to keep
  going.

**In scope**: anything that goes beyond the above, for example reaching a write, a reboot,
an OTA or a secret without the admin password; bypassing *Allow control* or the per-path
gates; getting a credential out of the REST API, MQTT or metrics; a crafted Modbus, MQTT,
HTTP or inverter-side frame that crashes or corrupts the board; a way to make the OTA path
accept something its checks should refuse; and the release workflow and published images.

**Out of scope**: attacks that need physical access to the board, a compromised broker or
reverse proxy, the owner's own admin password, and the unauthenticated or unencrypted
behaviour listed above.
