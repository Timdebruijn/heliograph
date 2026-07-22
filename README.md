# Heliograph

> A heliograph signalled messages with sunlight. This one translates what your solar
> inverter is saying into languages the rest of your network speaks.

Firmware for the **Waveshare ESP32-S3-Relay-1CH** that reads solar inverters and hybrid
systems over RS485 and republishes everything in the formats your tooling already speaks:
**MQTT + Home Assistant discovery, Modbus TCP, REST/JSON, Prometheus**. One box next to
the inverter, every integration for free.

Built for devices whose vendor tooling is dead, cloud-bound or closed — starting with a
2009 EverSolar that outlived its own monitoring portal.

## Supported devices

| Device family | Protocol | Status |
|---|---|---|
| EverSolar / Zeversolar legacy (TL series) | PMU (AA55) over RS485 | **Beta** — running in production, multi-day soak toward Stable |
| Growatt SPH hybrid (3–6 kW) | Modbus RTU | Experimental — register map transcribed, hardware validation in progress |
| SolaX X1 series (X1 Mini G1/G2/G3) | PMU (AA55) over RS485 | Experimental — awaiting first hardware session |

All drivers are **read-only**. Writing setpoints to somebody's inverter requires a
hardware-verified register map and a deliberate driver-level write path; neither is
enabled today (see `docs/device-profiles/schema.md` for how write support is staged).

**Adding a Modbus device is a data file, not C++** — see
[docs/adding-a-device.md](docs/adding-a-device.md).

## Quickstart

1. **Flash** the firmware — pick whichever suits you:

   - **Browser (easiest):** open the
     [web installer](https://timdebruijn.github.io/heliograph/) in Chrome or Edge,
     plug the board in over USB-C, click Connect. Nothing to install.
   - **esptool:** download `heliograph-<version>-factory.bin` from the
     [latest release](https://github.com/Timdebruijn/heliograph/releases/latest) and:

     ```bash
     esptool.py --chip esp32s3 write_flash 0x0 heliograph-<version>-factory.bin
     ```

   - **From source:** `pio run -e waveshare-eversolar`, then flash
     `.pio/build/waveshare-eversolar/firmware.factory.bin` the same way.

   Already running Heliograph? Update over the air instead: *Settings → Firmware
   update* with the release's `heliograph-<version>.bin` — settings survive an OTA,
   a factory flash erases them.

2. **Join the setup network** `Heliograph-Setup-XXXX` that the board broadcasts on first
   boot. The setup page opens by itself (captive portal); pick your WiFi, set an admin
   password, save.

3. **Open the dashboard** at the address the setup page shows
   (`http://heliograph-xxxxxx.local`). Wire RS485 A/B to the inverter's COM port and run
   the discovery wizard from the *Discovery* tab — it identifies the device and selects
   the driver.

4. **Integrations**: MQTT + Home Assistant discovery are configured under *Settings*;
   Modbus TCP listens on port 502; Prometheus scrapes `/metrics`; the JSON API lives
   under `/api/v1/` ([docs/rest-api.md](docs/rest-api.md)). Zabbix and checkmk can
   consume `/metrics` directly.

## Design in one paragraph

A strict layering — Transport → Driver → canonical measurement model → output adapters —
keeps brand knowledge confined to `src/drivers/<driver>/`. Outputs only ever see canonical
measurements and capabilities, which is why a new driver gets every integration for free.
The protocol core is platform-independent and host-tested (`pio test -e native`, no
hardware required); unknown values are published as *null/absent, never zero*; OTA updates
are guarded by a watchdog-backed bootloader rollback that has already earned its keep.
Details: [docs/architecture.md](docs/architecture.md).

## Development

```bash
pio test -e native          # 390+ host tests, no hardware needed
./tools/check_layering.sh   # architectural invariants
pio check -e native         # static analysis (cppcheck)
ruff check tools/           # lint for the Python tooling
pio run -e waveshare-eversolar
```

The `mock` environment runs the full output stack against a simulated inverter — useful
for UI and integration work without an RS485 bus.

## License

MIT — see [LICENSE](LICENSE). Protocol knowledge was re-implemented from community
references; credits and third-party licenses in
[LICENSE-THIRD-PARTY.md](LICENSE-THIRD-PARTY.md).
