# Heliograph

**Read your solar inverter over RS485 and publish it locally — Home Assistant, MQTT, Modbus TCP,
REST and Prometheus, all at once. One ESP32 board, roughly €20–30, no cloud anywhere in the
path.**

![The built-in dashboard, reading an EverSolar the manufacturer stopped supporting in 2019](docs/images/live.png)

> **Before anything else, check [the inverter table](#does-it-work-with-my-inverter).** Most of
> it says *never met a device*. That is not modesty — it is the only number on this page that
> decides whether this is worth your evening.

---

## Why it exists

The portal was shut down, the app stopped working, or the dongle is no longer supported. The
inverter still produces perfectly; you just cannot see it any more. Heliograph exists because of
an EverSolar that outlived its own monitoring portal.

Reading the inverter over its own RS485 port removes the vendor from the path: no account, no
portal, no dongle firmware that can decide to stop answering. **It does not make you immune to a
vendor changing what that port does.** Nothing can promise that, and anyone who does is guessing.

Four things people keep running into, and where this lands on each:

| What happens | Where this helps |
|---|---|
| A firmware update quietly removes local access. Reported on Solax, SolarEdge, Huawei, Deye, Solarman sticks, APsystems, Enphase. | RS485 is a different door from the dongle or the API. If the vendor closes that door too, this stops working as well. |
| The cloud is slow, rate-limited, or locks you out. One Growatt owner: *"I have been banned for 24 hours."* | No account to ban. Polls every 10 s by default. |
| The vendor disappears. Omnik went bankrupt; `solaredge_modbus` was archived in May 2026 with no successor. | MIT, self-hosted, no server to switch off. |
| Readings are silently wrong, and nobody can tell you whether the register map is right. | Every map says how far it has been proven. See below. |

---

## What it does

Reads the inverter over RS485 and serves the same decoded data four ways at the same time:
**MQTT** with Home Assistant discovery, a **Modbus TCP server**, a **REST API**, and
**Prometheus**. It has its own dashboard on the board, so there is nothing else to install to
see whether it works.

Polling is **every 10 seconds by default**, configurable from 1 to 3600. On the one bridge where
it has been measured over months — an EverSolar on RS485 — a poll takes about 103 ms. A Modbus
profile reading several blocks has never been timed against hardware. Either way it is a poll
and not a live feed.

It speaks more than one wire protocol, which is unusual on an ESP32. Three of them are not Modbus
at all — the AA55/PMU framing shared by EverSolar/Zeversolar and SolaX, and SolarMax's MaxTalk
ASCII. Alongside those it reads SunSpec and plain Modbus RTU with eleven device profiles. A
discovery mode probes an unknown RS485 bus and works out which family is on it.

### Turning the inverter down

Reading is half of it. Negative prices and feed-in limits are why people want the other half, and
a survey of what exists found no open-source project doing it at all.

Two routes are built. **Over the protocol**, a setpoint — a power limit, or connect and
disconnect. **Over a dry contact**, the relay boards close the DRM terminals many inverters carry
for exactly this, which is often the only control an inverter that cannot be written to still
offers.

Both are gated, deliberately, and the gates are the part worth knowing before you count on
either:

- `security.read_only_mode` is **on by default** and refuses every command before any driver sees
  it. Anyone with the admin password can turn it off.
- A Modbus profile will not write a row that is not marked `verified`, and no shipped profile has
  one. Verified means a register checked against a device, not against a second PDF.
- SunSpec's write path has a different gate — the inverter must publish SunSpec model 123 — and
  no SunSpec device has ever answered this firmware.
- The relays switch, measured on the bench. **Nothing has ever been wired to an inverter's DRM
  port.** If you are first, [`docs/drm.md`](docs/drm.md) is written for you.

The path is built and tested; what is missing is a verified register and somebody's hardware to
prove it against. That is the current state, not the design.

![Finding and naming what is on the bus](docs/images/inverters.png)

---

## Does it work with my inverter?

Probably not yet, and nobody should tell you otherwise from a README.

This table does not count how many inverters are *supported*. It counts how many have been seen
working, which is three.

An inverter is reached in one of two ways. A **driver** is hand-written code for one protocol
family. A **profile** is a Modbus register map written as data — a table of addresses, scales and
units — read by the one Modbus driver. The two are graded separately, because they fail
differently: a driver gets the framing wrong, a profile gets the meaning of register 33079 wrong.

**Drivers**

| Driver | Status | Has a real device ever answered it? |
|---|---|---|
| EverSolar / Zeversolar **TL** series (PMU framing, not Modbus) | **Stable**, for that model | Yes — a TL3000-20, the author's own, in production |
| Same driver, Zeversolar **TLC** series (three-phase) | **Beta** | Yes — one TLC8000, on a contributor's hardware, not the author's |
| Modbus profile driver | Experimental | Yes — a Deye SG05, through a profile its owner wrote (below) |
| SunSpec | Experimental | No |
| SolarMax (MaxTalk ASCII, not Modbus) | Experimental | No |
| SolaX X1 | Experimental | One field attempt. It returned zero bytes. |

The TLC row carries two caveats the word Beta does not: which of the two per-phase current
registers is L2 rests on the layout rather than on a measurement, because both read the same
value in the only capture that exists, and the operating-hours word has never been compared with
anything. Beta also lives in prose only — in the code the driver is one driver, so the REST API
and the dashboard report `stable` for a TLC8000 too.

**Modbus profiles** — Deye, GoodWe, Growatt (×3), Huawei, Sofar (×2), Solis (×2), Sungrow

| | |
|---|---|
| Status | Experimental, all eleven |
| Have any of the shipped profiles met a device? | **No. Zero of eleven.** |

Those eleven are where most inverter brands live, and the shipped maps are the part of this
project with the least evidence behind them. The driver that reads them has met real hardware,
though — with a map somebody wrote for their own inverter.

**Experimental** means implemented from documentation. It may simply not work. If you run it,
you are the first.
**Beta** means a real inverter has answered it, and it has not yet run unattended long enough to
be called anything more.
**Stable** means validated and soak-tested on hardware. The label belongs to a driver *and* an
inverter, not to a driver alone.

What that looks like in practice, from [the thread asking for testers](https://community.home-assistant.io/t/heliograph-esp32-firmware-that-speaks-your-inverters-native-protocol-mqtt-ha-modbus-tcp-rest-prometheus-looking-for-testers/1022820/3): the
bundled Deye profile is single-phase, so on a three-phase Deye SG05 it read nothing. Its owner —
who wrote that they don't have *"the biggest experience with programming"* — wrote a profile for
the SG05 instead, built the firmware, flashed it, and got *"a pretty stable readout"*. That is
what adding an inverter as data is for: a table of registers, not a driver. Writes were not
tried, and that profile has not yet come back into this repository.

### How a register map earns its grade

A map copied out of a vendor PDF is not a map a device has answered. Vendor documentation is
sometimes only partly accurate, and a reading that is valid but wrong is the failure that goes
unnoticed for months.

So each profile names its sources in its own header, row by row where it matters, and says which
rows rest on one source rather than two. Two agreeing sources is still not a device confirming —
it is a better guess, and it is written down as one. The annotation is prose and it is uneven:
Huawei and GoodWe are single-sourced and say so, while GoodWe annotates none of its eighteen rows
individually. Eleven profiles, 197 read registers, 8 write rows, nothing verified against
hardware. [`docs/drivers/coverage.md`](docs/drivers/coverage.md) has it row by row.

No inverter firmware version is named as tested anywhere in this project. Do not read that
absence as "all of them".

---

## Who this is for

- You have an inverter with an RS485 port, and a vendor cloud you would rather not depend on.
- You run Home Assistant, Prometheus, or something that speaks Modbus TCP.
- You can live with a project where your inverter has probably never been tried.

## Who it is not for

- Anyone whose inverter has no RS485 connection. Some are WiFi-only, and there is nothing here
  for them.
- Anyone who needs curtailment working **this week**. It is built; nothing has proven it on an
  inverter yet.
- Anyone who needs authentication on the data outputs.

---

## Where it stops

- **Eleven of eleven Modbus profiles have never met a device.** Nor has SunSpec, nor SolarMax.
- **No control path has ever reached a device**, by either route. The gates above are all still
  shut, and the relay measurements cover one board of the three: the Relay-6CH was measured for
  order, polarity, failsafe and boot state, while the 1CH switches and its polarity has not been
  put on a meter.
- **Finding an EverSolar or SolaX is not a read-only operation.** That family is discovered by
  being given a bus address, so discovery transmits by design and `read_only_mode` does not
  cover it. Nothing is configured and the address is forgotten on power loss.
  [`docs/security.md`](docs/security.md) is the authority on all of this.
- **Modbus TCP carries 39 of the 44 channels.** `battery.power`, `battery.current`,
  `battery.temperature` and both battery energy counters have no register.
- **No authentication on Modbus TCP, on REST reads, or on `/metrics`.** Where there is
  authentication it is HTTP Basic over plain HTTP, so the admin password travels in clear — and
  that password is what turns the read-only flag off. Stored credentials are unencrypted, the
  setup access point is open and comes back after a WiFi failure, and OTA images are unsigned.
  Keep it on a network you trust; [`docs/security.md`](docs/security.md) has the full list.
- **A wrong-but-valid static IP can lock you out** ([#69](https://github.com/Timdebruijn/heliograph/issues/69)).
  The ways back are holding BOOT to factory-reset, or re-flashing over USB. The BOOT reset has
  been measured on the Relay-6CH and not on the RS485-CAN, where it also gives no feedback.
- **Several inverters on one bus is unconfirmed on hardware.** It is implemented across every
  output and has only ever been run against simulated fleets
  ([#82](https://github.com/Timdebruijn/heliograph/issues/82)).
- **One maintainer, pre-1.0.** Expect breaking changes.

---

## What else exists

If you have a Raspberry Pi, [SolarAssistant](https://solar-assistant.io) covers 66 vendors across
RS232, RS485 and CAN, with a cable per protocol. It costs €59 software-only, or €159–204 with
hardware, one licence per installation. It is mature and it is broad, and for most people with a
Pi it is the sensible answer today.

What a survey of the ESP32 projects did not turn up: anything combining several non-Modbus
inverter protocols on one board, anything serving a Modbus TCP server with decoded inverter data,
any open-source DRM curtailment at all, or any project grading its register maps by how many
sources agree. The ESP32 projects found each speak one family — OpenDTU (Hoymiles),
OpenInverterGateway (Growatt), SolaxModbusGateway, sofar2mqtt. "Not found" is the honest limit of
that search.

---

## What has been measured

1,055 host tests run with no hardware attached. They prove the code does what its author
intended; they do not prove an inverter will answer it. For that, the table above is the only
evidence this project has.

A production bridge ran 16.4 days without a restart on 0.27.4, ending with 145 KB of free heap
against 136 KB a minute after the flash — it did not shrink. The v0.29.0 image for that board is
1.57 MB, in a 6.25 MB slot.

![What the bridge thinks is wrong, when something is](docs/images/health.png)

One mention outside the author's own bench: a blog post by Tim Retout (10 October 2026) about
getting a Zeversolar Zeverlution 3680 into Home Assistant, which builds its own ESPHome component
and credits the protocol work — *"credit to eversolar-monitor, Heliograph and others"*. That is a
one-clause mention among several projects, and it is the whole of it. This project's own EverSolar
work was in turn reconstructed from eversolar-monitor, which is named first.

---

## Where to start

Everything practical is in [`docs/`](docs/README.md), grouped by what you are trying to do.

| | |
|---|---|
| Buy a board and wire it up, start to finish | [getting-started.md](docs/getting-started.md) |
| Read this before touching the RS485 wiring | [rs485-bus.md](docs/rs485-bus.md) |
| Check what your inverter's profile rests on | [drivers/coverage.md](docs/drivers/coverage.md) |
| Connect Home Assistant, MQTT, Prometheus, Modbus | [mqtt.md](docs/mqtt.md) · [prometheus.md](docs/prometheus.md) · [modbus-register-map.md](docs/modbus-register-map.md) |
| Turn an inverter down over its DRM input — the demand-response contacts many inverters carry | [drm.md](docs/drm.md) |
| Add your own inverter | [adding-a-device.md](docs/adding-a-device.md) |

**If your inverter is not in the table: plug it in anyway and say what happened.** A report that
it returned nothing is worth as much as one that it worked — that is how the SolaX row above got
its evidence, and it is the single most useful thing anyone can contribute right now.

## Licence

MIT — see [LICENSE](LICENSE), with third-party notices in
[LICENSE-THIRD-PARTY.md](LICENSE-THIRD-PARTY.md). Protocol knowledge was re-implemented from
community documentation; see the per-protocol documents for sources.
