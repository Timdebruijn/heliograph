# Third-party licenses and protocol provenance

This project is MIT licensed (see [LICENSE](LICENSE)). It builds on the work of others in
two distinct ways: **libraries** that are compiled into the firmware, and **protocol
knowledge** that was re-implemented from reference implementations and documents. No code
was copied from the protocol references; register layouts, frame formats and observed
device behaviour are facts, but the credit matters.

## Protocol knowledge (re-implemented, no code copied)

### eversolar-monitor — MIT

The EverSolar/Zeversolar PMU protocol implementation (`src/protocols/pmu/`,
`src/drivers/eversolar_legacy/`) is derived from the protocol knowledge in
[solmoller/eversolar-monitor](https://github.com/solmoller/eversolar-monitor)
(commit 784c2fc), itself based on the Eversolar PMU logger by Steve Cliffe.
Its license:

```
MIT License

Copyright (c) 2021 Henrik Møller Jørgensen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### esphome-solax-x1-mini — Apache-2.0

The SolaX X1 payload layouts and registration conventions (`src/drivers/solax_x1/`) are
derived from [syssi/esphome-solax-x1-mini](https://github.com/syssi/esphome-solax-x1-mini)
(Apache-2.0, hardware-tested against X1 Mini G1/G2/G3) and the official "SolaxPower
Single Phase External Communication Protocol X1 Series V1.2" document.

### Growatt register maps — community sources

The Growatt SPH register profile (`profiles/growatt/sph.toml`) was transcribed from
community register maps, primarily
[8none1/growatt_sph_nodered](https://github.com/8none1/growatt_sph_nodered), pending
verification against real hardware. Sources per register are annotated in the profile.

## Libraries compiled into the firmware

| Library | License (as published upstream) |
|---|---|
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | MIT |
| [espMqttClient](https://github.com/bertmelis/espMqttClient) | MIT |
| [eModbus](https://github.com/eModbus/eModbus) | MIT |
| [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) (ESP32Async fork) | LGPL-3.0 |
| [AsyncTCP](https://github.com/ESP32Async/AsyncTCP) (ESP32Async fork) | LGPL-3.0 |
| [Arduino core for ESP32](https://github.com/espressif/arduino-esp32) + ESP-IDF components | LGPL-2.1 / Apache-2.0 |
| [Unity](https://github.com/ThrowTheSwitch/Unity) (tests only, not in firmware) | MIT |

The LGPL-licensed components remain under their own licenses; this project links them
unmodified. Consult each project's repository for the authoritative license text.
