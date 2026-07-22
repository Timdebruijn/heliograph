# Hardware — Waveshare ESP32-S3-Relay-1CH

Status: **verified** against the official schematic and official demo. Not a single pin in
this document is guessed.

## Sources

| Source | Location | Used for |
|---|---|---|
| Wiki | <https://www.waveshare.com/wiki/ESP32-S3-Relay-1CH> | Board overview, jumper, FAQ |
| Schematic (PDF) | <https://files.waveshare.com/wiki/ESP32-S3-Relay-1CH/ESP32-S3-Relay-1CH-schematic.pdf> | Netlists, chip selection, isolation |
| Demo (ZIP) | <https://files.waveshare.com/wiki/ESP32-S3-Relay-1CH/ESP32-S3-Relay-1CH-Demo.zip> | `WS_GPIO.h`, `WS_RS485.cpp`, `I2C_Driver.h` |

The wiki itself contains **no** pin table. All pins below come from the demo source code and
are cross-checked against the schematic.

## Pinout

| Function | GPIO | Source (primary) | Source (confirmation) |
|---|---|---|---|
| RS485 TX (UART1) | **17** | `WS_GPIO.h: #define TXD1 17` | Schematic net `TXD1` → isolator → SP3485 `DI` |
| RS485 RX (UART1) | **18** | `WS_GPIO.h: #define RXD1 18` | Schematic net `RXD1` ← isolator ← SP3485 `RO` |
| RS485 DE/RE (direction) | **21** | `WS_GPIO.h: #define TXD1EN 21` | Schematic net `RS485_EN` → SP3485 `RE`(2)+`DE`(3) |
| Relay CH1 | **47** | `WS_GPIO.h: #define GPIO_PIN_CH1 47` | Schematic block `Relay` / `CH1` |
| I2C SCL (RTC) | **38** | `I2C_Driver.h: #define I2C_SCL_PIN 38` | Schematic `GPIO38` |
| I2C SDA (RTC) | **39** | `I2C_Driver.h: #define I2C_SDA_PIN 39` | Schematic `IO39` |

## Components (from schematic)

| Component | Type | Relevance |
|---|---|---|
| Module | ESP32-S3-**WROOM-1U** (chip `ESP32-S3R8`) | 8 MB PSRAM embedded |
| Flash | `W25Q128JVSI` | **16 MB** external → board is functionally N16R8 |
| RS485 transceiver | `SP3485EN` | `RE`(pin 2) + `DE`(pin 3) tied together on net `RS485_EN` |
| Digital isolator | `π131M31` | 3 channels: TXD, RS485_EN (outgoing), RXD (incoming) |
| Isolated power supply | `B0505S-1WR3` | Galvanic isolation on the RS485 side (`SGND` ≠ `GND`) |
| Termination | `R23 = 120R` + header `H1` | Jumper-selectable, see §Termination |
| RTC | `PCF85063AT` @ I2C `0x51` | Optional, not needed for MVP |
| Protection | `SMF12CA`, `SM712`, `BSMD1206-050` | TVS / surge / resettable fuses on RS485 |
| USB | direct `USB_N`/`USB_P` → `D_N`/`D_P` | **No** CH340/CH343 → native USB CDC |

## Critical correction: RS485 direction control is NOT passively automatic

The assignment assumed "automatic hardware-based RS485 direction control" and assumed that
software-driven DE/RE control could be omitted. The schematic and the demo disprove that:

- `SP3485EN` has separate `DE`/`RE` pins, tied together to the net `RS485_EN`.
- `RS485_EN` is driven via the isolator by **GPIO21**.
- There is no auto-direction circuit (no TX detection on DI, no RC timing).

The official demo solves this by having the **UART hardware** control the direction:

```c
// ws-demo/Arduino/examples/MAIN_WIFI_AP/WS_RS485.cpp
lidarSerial.begin(Baudrate, SERIAL_8N1, RXD1, TXD1);
lidarSerial.setPins(-1, -1, -1, TXD1EN);          // 4th argument = RTS pin
lidarSerial.setMode(UART_MODE_RS485_HALF_DUPLEX); // UART drives RTS itself, bit-exact
```

The wiki claim "automatic direction control" is therefore correct in the sense that **no
software toggle per byte** is needed — but only because the ESP32-S3 UART peripheral in
`UART_MODE_RS485_HALF_DUPLEX` switches the RTS line itself at the correct bit timing.

**Consequence for the transport layer:** GPIO21 must be configured as RTS and the UART must be
explicitly set to RS485 half-duplex mode. This is configured exactly once at `begin()`, after
which it is transparent. Manually toggling in software is explicitly **not** desired (too
slow, races with the last stop bit).

## Relay — safety rules for the MVP

The relay is on GPIO47 and is **out of scope** for the MVP.

- ESP32-S3 GPIOs are input/hi-Z after reset; the relay is therefore not energized.
- The firmware sets GPIO47 to `OUTPUT` + `LOW` as the first action in `setup()`, before wifi,
  RS485, or any other initialization.
- No output adapter (MQTT/REST/Modbus/web) gets a path to this pin.
- On every failure path the pin stays `LOW`; there is no code that ever sets it `HIGH`.

Rationale for `LOW` = off: `WS_Relay.cpp` defines `Relay_Open()` as `digitalWrite(HIGH)` and
`Relay_Closs()` [sic] as `digitalWrite(LOW)`. Active-high, in other words.

## Termination (120 Ω)

Header `H1` (3-pin) switches `R23` (120 Ω) across the RS485 line. Place the jumper in the
120R position **only** when the bridge is physically at the end of the RS485 bus. The wiki
FAQ explicitly names missing termination as a cause of unreliable communication.

## Consequences for the build

| Fact | Consequence |
|---|---|
| 16 MB flash | Room for dual-app OTA partitions + LittleFS. No drivers need to be left out. |
| 8 MB PSRAM (octal) | Requires `-DBOARD_HAS_PSRAM` and octal PSRAM config. |
| Native USB-CDC | Requires `-DARDUINO_USB_CDC_ON_BOOT=1` for serial logging over USB-C. |
| Isolated RS485 | Do not connect `SGND` to `GND`; do not undermine the isolation when wiring. |

## Open / to be validated on hardware

1. **Boot button GPIO** — the FAQ mentions BOOT+RESET, but the schematic's text extraction did
   not unambiguously reveal the BOOT pin. ESP32-S3 uses GPIO0 by default, but this is **not
   verified** and is therefore not yet recorded as a pin constant. This is needed for the
   provisioning reset (§29) — to be confirmed in Phase 3 on real hardware.
2. **RS485 TX/RX status LEDs** — mentioned in the product description; the driving circuit
   cannot be found in the demo and is presumably hardware-based on the transceiver lines. No
   firmware action needed.
