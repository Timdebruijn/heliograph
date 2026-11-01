<div align="center">

# Heliograph

### Your solar inverter, on your own network. No cloud, no account, one ~€25 board.

[![Latest release](https://img.shields.io/github/v/release/Timdebruijn/heliograph)](https://github.com/Timdebruijn/heliograph/releases)
[![CI](https://github.com/Timdebruijn/heliograph/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Timdebruijn/heliograph/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/tour-dark.gif">
  <img src="docs/images/tour-light.gif" alt="Tour of the Heliograph dashboard: Live, Inverters, Integrations, Health and Bridge tabs" width="900">
</picture>

<sub>The dashboard that runs on the board itself, shown here with its built-in demo fleet: three inverters, one hybrid with a battery, one that never answers.</sub>

**[Flash it from your browser →](https://timdebruijn.github.io/heliograph/)** · [Getting started](docs/getting-started.md) · [Docs](docs/README.md)

</div>

> **Honest status:** pre-1.0 and one maintainer. One inverter family is Stable today. Read
> [Will it work with my inverter?](#will-it-work-with-my-inverter) before you order a board.

---

## Your inverter works fine. Everything around it doesn't.

- The portal shut down, the app stopped updating, or the Wi-Fi stick *"just stops sending data"*.
- The cloud refreshes every five minutes, rate-limits you, or locks you out. One Growatt owner:
  *"I have been banned for 24 hours."*
- Negative prices keep growing. Dutch negative-price hours went 85 → 316 → 458 → roughly 570
  from 2022 to 2025 ([Selectra](https://selectra.nl/energie/vergelijken/stroomprijs/negatieve-stroomprijs)),
  and *salderen* ends on 1 January 2027
  ([Keuze.nl](https://www.keuze.nl/nieuws/salderingsregeling-zonnepanelen-definitief-van-de-baan-per-2027-dit-zijn-de-gevolgen)).
  The commercial "switch off when it's negative" offers sit behind an energy contract and a vendor cloud.

**Heliograph takes the vendor out of the path.** Wire three conductors (A, B, GND) from a Waveshare
ESP32-S3 board to your inverter's RS485 port and flash it from a browser tab. If your inverter's
protocol is supported, it is now on your network, decoded once and served everywhere at the same time.

## What you get

| | |
|---|---|
| **MQTT + Home Assistant** | Auto-discovery. Each inverter is its own HA device, named by you. [mqtt.md](docs/mqtt.md) |
| **Modbus TCP server** | A documented register map with one unit id per inverter. Heliograph is the only master on the RS485 bus, so Home Assistant, evcc and a script can all read without colliding. [modbus-register-map.md](docs/modbus-register-map.md) |
| **REST API + live events** | JSON for scripts and Node-RED, a server-sent event stream, config backup and restore. [rest-api.md](docs/rest-api.md) |
| **Prometheus** | `/metrics` with 53 metrics and a `device=` label per inverter. Unknown is absent, not zero. [prometheus.md](docs/prometheus.md) |
| **A dashboard on the board** | Live view, bus diagnostics, health counters, one-click firmware update. Nothing else to install. |

Polls every 10 s by default (1–3600). Up to 8 inverters share one bus.

## Why people pick it

**Your old inverter is not e-waste.** Not everything speaks Modbus. Heliograph also speaks the
EverSolar/Zeversolar (AA55/PMU) and SolarMax (MaxTalk) protocols, next to SunSpec and Modbus
register maps. It exists because of an EverSolar that outlived its own monitoring portal. Vendor
support for it ended in 2019, and it now runs in production on the author's roof.

**A new inverter is a data file, not firmware.** Modbus register maps are TOML: addresses,
scales, units. A tester whose three-phase Deye SG05 didn't match the bundled profile wrote his own,
and got *"a pretty stable readout"*. He also wrote that he doesn't have *"the biggest experience
with programming"*.
([HA community thread](https://community.home-assistant.io/t/heliograph-esp32-firmware-that-speaks-your-inverters-native-protocol-mqtt-ha-modbus-tcp-rest-prometheus-looking-for-testers/1022820/3) ·
[adding-a-device.md](docs/adding-a-device.md))

**Nothing to install.** Browser flasher, a captive-portal Wi-Fi setup, a discovery wizard that
proposes a driver, and over-the-air updates from the dashboard. No toolchain, no YAML.

**It tells you what's wrong.** When an inverter doesn't answer, the dashboard ranks the likely
causes (two units on one address, A and B swapped, termination in the wrong place) and offers a
one-click test for each. The Health tab names every counter the way a person would ask for it.

**Safe by default.** Read-only mode is on out of the box and refuses every command and relay move
before a driver sees it. Finishing setup requires an admin password. Passwords never show up in
logs, REST, MQTT or metrics. Reads are unauthenticated, so keep it on a network you trust; see
[security.md](docs/security.md).

## Built to turn your inverter down

**Not yet proven on an inverter. Today this does not save you money.** No control command has
reached an inverter yet, and the relays have never been wired to a real DRM port. The path is
built and tested; what it needs is someone with the hardware to be first.

Two routes are built:

- **A DRM dry contact.** Relay boards drive the DRM0–DRM8 lines of the AS/NZS 4777 demand-response
  port (usually an RJ45 marked DRM/DRED), on inverters that have one. Relay energised means DRM
  asserted, so a dead bridge leaves your inverter producing. The relays are measured on the bench.
- **A protocol setpoint.** A power limit or connect/disconnect written over Modbus or SunSpec. It
  stays shut until the device proves it: a register verified on hardware, or SunSpec model 123.

**Who decides when to curtail? You do.** Heliograph is the local actuator and the data source. The
price logic lives in Home Assistant, evcc or Node-RED, next to your dynamic tariff. No contract, no
cloud. Read [drm.md](docs/drm.md) first.

---

## Will it work with my inverter?

You need an inverter with an **RS485 port**. Wi-Fi-only inverters are out, and so is Hoymiles
(use [OpenDTU](https://github.com/tbnobody/OpenDTU)).

| Inverter / protocol | Grade | Has a real device answered it? |
|---|---|---|
| EverSolar / Zeversolar **TL** series | **Stable** | Yes. A TL3000-20 in production, the author's own |
| Zeversolar **TLC** series | **Beta** | Yes. One TLC8000, on a contributor's hardware |
| Modbus register maps: Deye, GoodWe, Growatt (×3), Huawei, Sofar (×2), Solis (×2), Sungrow | Experimental | The driver has read a Deye SG05 through its owner's map. None of the 11 shipped maps has met a device |
| SunSpec | Experimental | No |
| SolarMax (MaxTalk) | Experimental | No |
| SolaX X1 | Experimental | One field attempt. Zero bytes came back |

- **Stable:** validated and soak-tested on hardware, for that driver *and* that inverter.
- **Beta:** a real inverter has answered, but it hasn't yet run unattended long enough to be
  called more.
- **Experimental:** nothing shipped has been confirmed on hardware. It may simply not work.

**Own one of the Experimental ones? You can be the first.** Plug it in and say what happened.
"It returned nothing" is worth as much as "it works"; that is how the SolaX row got its evidence.
Every map cites its sources row by row in [coverage.md](docs/drivers/coverage.md).

**Is it for you?** Yes, if you have an RS485 inverter, run Home Assistant, Prometheus or anything
that speaks Modbus TCP, and would rather not depend on a vendor cloud. Not yet, if you need
curtailment working this week or authentication on the data outputs.

## Quick start

1. **Get a board.** A Waveshare ESP32-S3 RS485 board, about €20–30; three are supported
   ([hardware.md](docs/hardware.md)). Pick a relay variant if you want DRM.
2. **Flash it** from the **[web flasher](https://timdebruijn.github.io/heliograph/)** in Chrome or Edge.
3. **Connect.** Join the `Heliograph-Setup` Wi-Fi, choose your network, set an admin password.
4. **Wire A, B and GND** to the inverter. Read [rs485-bus.md](docs/rs485-bus.md) first;
   swapping A and B is the most common fix.
5. **Find your inverter** with the discovery wizard. Add your MQTT broker and Home Assistant picks
   it up.

Full walkthrough: [getting-started.md](docs/getting-started.md).

## How it compares

| | Heliograph | Vendor cloud + stick | HA integrations (ha-solarman, solax-modbus) | ESPHome modbus_controller |
|---|---|---|---|---|
| Cloud-free | Yes | No | Yes | Yes |
| Outputs | MQTT/HA, Modbus TCP, REST, Prometheus, dashboard | App | Home Assistant | HA, MQTT |
| Add an inverter | TOML map | — | Per integration | Write the YAML |
| Hardware-proven breadth | **Narrow, growing** | Own brand | **Broad** | What you write |

The HA integrations above have been proven on far more devices than Heliograph has. Heliograph's
bet is a different one: every output at once, protocols the vendors abandoned, and a local curtailment
path on a €25 board.

With a Raspberry Pi and a hybrid or off-grid system, [SolarAssistant](https://solar-assistant.io)
is mature and broad, from €59. Frank Energie and Zonneplan curtail at negative prices, but only on
their own contract and through their cloud. evcc is a consumer of Heliograph's data, not a rival.

## Engineering you can check

- More than 1,000 host tests, static analysis and a tag-must-match-version gate on every release.
- A production bridge ran 16.4 days without a restart, and its free heap did not shrink.
- One-click updates from the dashboard check the target board and a SHA-256 before the partition flips.
- A monthly dependency report, and a release roughly every two days since July 2026.

Host tests prove the code does what its author intended. They do not prove your inverter will
answer; the table above is the only evidence for that.

<details>
<summary><b>Where it stops</b>: the full list of limits</summary>

- **Eleven of eleven Modbus maps have never met a device.** Nor have SunSpec or SolarMax.
- **No control path has ever reached a device**, by either route. The Relay-6CH was measured for
  order, polarity, failsafe and boot state. The 1CH switches, but its polarity has not been put on a meter.
- **Finding an EverSolar or SolaX is not a read-only operation.** That family is discovered by being
  given a bus address, so discovery transmits by design and `read_only_mode` does not cover it.
- **Modbus TCP carries 39 of the 44 channels.** Battery power, current, temperature and both
  battery energy counters have no register.
- **No authentication on Modbus TCP, REST reads or `/metrics`.** Admin auth is HTTP Basic over plain
  HTTP. Stored credentials are unencrypted, the setup access point is open and comes back after a
  Wi-Fi failure, and OTA images are unsigned (the SHA-256 checks integrity, not origin).
- **A wrong-but-valid static IP can lock you out** ([#69](https://github.com/Timdebruijn/heliograph/issues/69)).
  Hold BOOT to factory-reset, or re-flash over USB.
- **Several inverters on one bus has only run against simulated fleets**
  ([#82](https://github.com/Timdebruijn/heliograph/issues/82)).
- **No inverter firmware version is named as tested.** Don't read that as "all of them".
- **One maintainer, pre-1.0.** Expect breaking changes.

[security.md](docs/security.md) is the authority on the security side.

</details>

<details>
<summary><b>Screenshots from a real bridge</b></summary>

![The live tab on the author's bridge, reading an EverSolar the manufacturer stopped supporting in 2019](docs/images/live.png)
![The Health tab: what the bridge thinks is wrong, when something is](docs/images/health.png)

</details>

## Documentation

| I want to… | Read |
|---|---|
| Set it up from start to finish | [getting-started.md](docs/getting-started.md) · [hardware.md](docs/hardware.md) · [rs485-bus.md](docs/rs485-bus.md) |
| Connect my stack | [mqtt.md](docs/mqtt.md) · [rest-api.md](docs/rest-api.md) · [prometheus.md](docs/prometheus.md) · [modbus-register-map.md](docs/modbus-register-map.md) |
| Turn an inverter down | [drm.md](docs/drm.md) |
| Add my own inverter | [adding-a-device.md](docs/adding-a-device.md) · [drivers/coverage.md](docs/drivers/coverage.md) |
| Understand the security model | [security.md](docs/security.md) |

Everything else is in the [docs index](docs/README.md).

## Join in

**Plug your inverter in and tell us what happened.** That is the single most useful contribution
right now: it is how an Experimental row becomes a Beta one. If Heliograph is useful to you, a
star helps other people with an orphaned inverter find it.

## Licence

MIT. See [LICENSE](LICENSE), with third-party notices in [LICENSE-THIRD-PARTY.md](LICENSE-THIRD-PARTY.md).
Protocol knowledge was re-implemented from community documentation; the per-protocol documents
name their sources.
