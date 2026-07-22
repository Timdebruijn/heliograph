# Hardware — Waveshare ESP32-S3-RS485-CAN

Status: **pins verified** by the board documentation and by months of runtime on the real
hardware. Component-level details (transceiver and isolator part numbers) are pending a
read of this board's schematic and are explicitly marked as such below.

> **History.** This project spent its first months believing it ran on the Waveshare
> **ESP32-S3-Relay-1CH**, the board named in the original project brief; the earlier
> revision of this document verified that board's schematic in detail. The physical boards
> turned out to be the RS485-CAN (spotted 2026-07-22). Nothing ever misbehaved because the
> RS485 subsystem is pin-identical between the two designs. Consequence of the
> correction: there is **no relay** (the GPIO47 safety clamp was removed — the safest
> state for a pin with no known function is untouched hi-Z). A first revision of this
> correction also claimed "no RTC chip" on the strength of an incomplete community
> document — wrong: the official schematic shows a **PCF85063AT** with backup supply,
> and the firmware now uses it (clock valid from boot, corrected after every NTP sync).

## Sources

| Source | Location | Used for |
|---|---|---|
| Wiki | <https://www.waveshare.com/wiki/ESP32-S3-RS485-CAN> | Board overview, interfaces, jumpers |
| Schematic (PDF) | <https://files.waveshare.com/wiki/ESP32-S3-RS485-CAN/ESP32-S3-RS485-CAN-Schematic.pdf> | GPIO matrix, RTC (PCF85063AT), transceivers |
| Community board doc | [Sleeper85/esphome-yambms](https://github.com/Sleeper85/esphome-yambms/blob/main/documents/README/Board_Waveshare_ESP32-S3-RS485-CAN.md) | Pin assignments cross-check (note: it omits the RTC) |
| Runtime | this project, production since 2026-07 | RS485 pins, flash size, USB-CDC behaviour |

This is one of three supported boards; the board headers in `src/boards/` are the
authoritative pin record for all of them (Relay-1CH: 1 relay + RTC; Relay-6CH: 6 relays,
8 MB flash, RS485 direction pin still unverified — see the header).

## Pinout

| Function | GPIO | Status |
|---|---|---|
| RS485 TX (UART1) | **17** | verified (documentation + runtime) |
| RS485 RX (UART1) | **18** | verified (documentation + runtime) |
| RS485 EN (direction) | **21** | verified (documentation + runtime) |
| CAN TX | **15** | documented; unused by this firmware |
| CAN RX | **16** | documented; unused by this firmware |
| RTC SCL (PCF85063) | **38** | verified (official schematic GPIO matrix) |
| RTC SDA (PCF85063) | **39** | verified (official schematic GPIO matrix) |
| RTC INT | **40** | schematic; unused by this firmware |
| BOOT button | — | present on the board; GPIO not measured yet, so not a constant |

## Board facts

| Fact | Value | Relevance |
|---|---|---|
| Flash | **16 MB** | Dual-app OTA partitions + headroom (`partitions_16mb_ota.csv`) |
| PSRAM | 8 MB | `-DBOARD_HAS_PSRAM` |
| USB | native USB-C, no CH340 | `-DARDUINO_USB_CDC_ON_BOOT=1`; attaching USB power-cycles a USB-powered board |
| Power | USB-C or 7–36 V DC terminal | The DC terminal allows powering from the inverter side of the room |
| Isolation | power + optocoupler, RS485 **and** CAN | `SGND` is NOT `GND` — never bridge them |
| Termination | 120 Ω jumper per bus | Fit only when the bridge is physically at the end of the RS485 bus |
| Buttons | BOOT + RESET | RESET reboots; BOOT is the future hold-to-factory-reset candidate (backlog) |

## RS485 direction control

The transceiver's enable pin (GPIO21) is handed to the UART peripheral as its RTS line:

```c
serial.begin(baudrate, SERIAL_8N1, RX, TX);
serial.setPins(-1, -1, -1, EN);                // 4th argument = RTS pin
serial.setMode(UART_MODE_RS485_HALF_DUPLEX);   // UART drives EN itself, bit-exact
```

With `UART_MODE_RS485_HALF_DUPLEX` the ESP32-S3 UART switches direction on the exact bit
boundary. Toggling the pin from software is explicitly **not** done: it cannot reliably
race the last stop bit. This is configured once in the transport's `begin()` and is
transparent afterwards.

## CAN

The second isolated terminal block is a CAN interface (GPIO15/16, ESP32-S3 TWAI
peripheral). No driver uses it today. It is recorded because battery BMS protocols
commonly speak CAN, which makes this board a natural fit for a future battery-side
source — a deliberate decision for later, not an accident waiting in a header file.

## Open / to be verified on hardware or schematic

1. **BOOT button GPIO** — needed for the hold-BOOT-at-boot factory-reset recovery path
   (backlog). Measure on real hardware; ESP32-S3 convention (GPIO0) is convention, not
   evidence.
2. **GPIO47** — no documented function on this board. The firmware no longer touches it.
3. **Relay-6CH relay polarity** — the community configuration drives HIGH to energise;
   confirm on the physical board before the relays ever touch a DRM port. (The RS485
   direction question is resolved: the official demo transmits and receives with a plain
   `begin()`, so the board's `TXD1EN` net is driven by its own auto-direction circuit and
   the header correctly declares no direction GPIO.)

Component detail from the schematic, for reference: SP3485EN RS485 transceiver behind a
π163E31 isolator, TJA1051T CAN transceiver, PCF85063AT RTC with 32.768 kHz crystal.
