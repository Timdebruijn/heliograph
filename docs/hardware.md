# Hardware — the three supported boards

Heliograph runs on three Waveshare ESP32-S3 boards, one firmware image each. They share the
ESP32-S3, the RS485 pins and the BOOT button, and differ in almost everything else: relays,
RTC, PSRAM, flash size, status LED, and whether the RS485 transceiver needs a direction pin
at all.

`src/boards/*.h` is the authoritative pin record. This document explains the parts a header
cannot: why a value is what it is, what has actually been measured, and what has not.

> **History.** This project spent its first months believing it ran on the
> **ESP32-S3-Relay-1CH**, the board named in the original project brief. The physical boards
> turned out to be the RS485-CAN (spotted 2026-07-22). Nothing ever misbehaved, because the
> RS485 subsystem is pin-identical between the two designs. A first revision of that
> correction also claimed "no RTC chip" on the strength of an incomplete community document —
> wrong: the official schematic shows a **PCF85063AT** with backup supply, and the firmware
> now uses it (clock valid from boot, corrected after every NTP sync).

## At a glance

| | RS485-CAN | Relay-1CH | Relay-6CH |
|---|---|---|---|
| Board id (`board_id`, image name) | `rs485-can` | `relay-1ch` | `relay-6ch` |
| Module | N16R8 | N16R8 | **N8** |
| Flash | 16 MB | 16 MB | **8 MB** |
| PSRAM | 8 MB octal | 8 MB octal | **none** |
| Relays | none | 1 (GPIO47) | 6 (1, 2, 41, 42, 45, 46) |
| RTC (PCF85063AT) | yes (38/39) | yes (38/39) | **no** |
| RS485 direction pin | GPIO21 | GPIO21 | **none — auto-direction** |
| Status LED | no | no | yes (GPIO38) |
| Buzzer | no | no | yes (GPIO21) |
| CAN | yes (15/16, unused) | no | no |

Relays are active-high on both relay boards, and every relay is off at boot — see
[docs/drm.md](drm.md) for what that means for a failsafe.

**GPIO21 is the trap in that table.** On the RS485-CAN and the 1CH it drives the transceiver's
direction; on the 6CH it is the buzzer. Flashing the wrong image onto a 6CH would therefore beep
at every transmission rather than fail visibly, which is why the board id travels in the image
name and in `/api/v1/status`.

## Sources

| Source | Location | Used for |
|---|---|---|
| Wiki (RS485-CAN) | <https://www.waveshare.com/wiki/ESP32-S3-RS485-CAN> | Board overview, interfaces, jumpers |
| Schematic (RS485-CAN, PDF) | <https://files.waveshare.com/wiki/ESP32-S3-RS485-CAN/ESP32-S3-RS485-CAN-Schematic.pdf> | GPIO matrix, RTC (PCF85063AT), transceivers |
| Community board doc | [Sleeper85/esphome-yambms](https://github.com/Sleeper85/esphome-yambms/blob/main/documents/README/Board_Waveshare_ESP32-S3-RS485-CAN.md) | Pin cross-check (note: it omits the RTC) |
| Waveshare support | email, 2026-07-27 | RTC backup battery type (see below) |
| Runtime | this project, production since 2026-07 | RS485 pins, flash size, USB-CDC behaviour |

## Pinout

Shared by all three: **RS485 TX 17, RS485 RX 18** (UART1), **BOOT GPIO0**.

| Function | RS485-CAN | Relay-1CH | Relay-6CH | Status |
|---|---|---|---|---|
| RS485 EN (direction) | **21** | **21** | — | RS485-CAN: documentation + months of runtime. 1CH: Waveshare's `WS_GPIO.h` + schematic, no RS485 traffic driven on one yet. 6CH has no direction GPIO |
| Relay(s) | — | **47** | **1, 2, 41, 42, 45, 46** | 6CH order verified on hardware 2026-07-23. 1CH GPIO47 verified 2026-07-28: switched on demand, indicator LED followed in both directions, REST and Prometheus agreed at both ends |
| RTC SCL / SDA (PCF85063) | **38 / 39** | **38 / 39** | — | official schematic GPIO matrix |
| RTC INT | **40** | not checked | — | RS485-CAN schematic only; unused by this firmware, so nobody has needed the 1CH's |
| Status LED | — | — | **38** | verified lit on hardware 2026-07-23 |
| Buzzer | — | — | **21** | documentation + official demo; not sounded on hardware |
| CAN TX / RX | **15 / 16** | — | — | documented; unused by this firmware |

## The RTC, and the battery that backs it

The RS485-CAN and the Relay-1CH carry a **PCF85063AT** with a backup supply, so the clock is
valid from boot and logs are stamped before the network exists. The firmware reads it at boot
and writes it back after every NTP sync. The Relay-6CH has no RTC: on that board the clock is
unknown until NTP answers.

**Use an ML2032 (rechargeable). Never a CR2032.**

Waveshare support, asked directly (2026-07-27):

> Use the RTC-Battery-B (ML2032). It is a rechargeable 3V cell with the correct SH1.0
> connector. Do not use non-rechargeable cells, as the board's circuit provides a charge
> voltage.

This is a safety point rather than a preference. The board puts a charging voltage on the cell,
and a CR2032 is not built to take one — a non-rechargeable lithium cell under charge can vent or
rupture. The connector is SH1.0, so a cell holder with the wrong plug is also the wrong cell.
Product page: <https://www.waveshare.com/rtc-battery.htm>

Sourced from a support email rather than from the schematic, and recorded as such.

## Board facts

| Fact | Value | Relevance |
|---|---|---|
| USB | native USB-C, no CH340 | `-DARDUINO_USB_CDC_ON_BOOT=1`; attaching USB power-cycles a USB-powered board |
| Power | USB-C or 7–36 V DC terminal | The DC terminal allows powering from the inverter side of the room |
| Isolation (RS485-CAN) | power + optocoupler, RS485 **and** CAN | `SGND` is NOT `GND` — never bridge them |
| Termination | 120 Ω jumper per bus | Fit only when the bridge is physically at the end of the RS485 bus |
| Buttons | BOOT + RESET | RESET reboots. BOOT held ~5 s **while running** factory-resets; held **at power-on** it enters USB download mode instead (GPIO0 strapping) — see Recovery |

## Reading the memory figures

The bridge reports five memory numbers, and they do not all measure the same pool.

| Field | Covers |
|---|---|
| `free_heap_bytes`, `minimum_free_heap_bytes`, `max_alloc_heap_bytes` | **Internal SRAM only** (~320 KB total) |
| `psram_size_bytes`, `psram_free_bytes` | **External PSRAM only** (8 MB on the RS485-CAN and the 1CH; the 6CH has none) |

This is not a naming quirk, it is what the Arduino core does: `ESP.getFreeHeap()`,
`getMinFreeHeap()` and `getMaxAllocHeap()` are all `heap_caps_*(MALLOC_CAP_INTERNAL)`. So a free
heap of ~150 KB is healthy, not alarming — it is 150 KB of 320 KB, not of 8 MB. That reading is
the same on all three boards, PSRAM or not.

The PSRAM pair is `null` in JSON, absent from Prometheus and `0xFFFFFFFF` in the Modbus
registers on a board that has none. That is also how you tell, from the network alone, that
PSRAM failed to initialise on a board that should have it: the fields go absent. Before these
existed there was no way to see that at all (audit, 2026-07-26).

Note that the IDF is configured with `CONFIG_SPIRAM_USE_MALLOC` and a 4096-byte threshold, so
allocations above 4 KB already land in PSRAM through plain `malloc` without any code asking for
it. `psram_free_bytes` moving is normal and expected.

## RS485 direction control

**On the RS485-CAN and the Relay-1CH.** The transceiver's enable pin (GPIO21) is handed to the
UART peripheral as its RTS line:

```c
serial.begin(baudrate, SERIAL_8N1, RX, TX);
serial.setPins(-1, -1, -1, EN);                // 4th argument = RTS pin
serial.setMode(UART_MODE_RS485_HALF_DUPLEX);   // UART drives EN itself, bit-exact
```

With `UART_MODE_RS485_HALF_DUPLEX` the ESP32-S3 UART switches direction on the exact bit
boundary. Toggling the pin from software is explicitly **not** done: it cannot reliably
race the last stop bit. This is configured once in the transport's `begin()` and is
transparent afterwards.

**The Relay-6CH has no direction pin, and that is verified rather than unknown.** Its official
demo transmits and receives with a plain `begin(9600, SERIAL_8N1, RX, TX)` — no `setPins`, no
RS485 mode — so the schematic's `TXDEN'` net is driven by the board's own auto-direction
circuit. The board header sets the direction pin to `-1`, which makes the transport skip RTS
configuration entirely. GPIO21, the direction pin on the other two boards, is this board's
buzzer.

## CAN

The second isolated terminal block is a CAN interface (GPIO15/16, ESP32-S3 TWAI
peripheral). No driver uses it today. It is recorded because battery BMS protocols
commonly speak CAN, which makes this board a natural fit for a future battery-side
source — a deliberate decision for later, not an accident waiting in a header file.

## After a crash — the core dump

A panic writes a full ELF core dump to the `coredump` partition (64 KB, present in both
partition tables; `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` is set in the prebuilt IDF config). It
survives the reboot.

The bridge reads that dump at boot and reports it **without a cable**. `GET /api/v1/diagnostics`
carries:

| Field | What it says |
|---|---|
| `coredump_present` | a valid dump is stored |
| `coredump_task` | the task that faulted |
| `coredump_cause` / `coredump_cause_name` | the Xtensa EXCCAUSE, e.g. `28` / `LoadProhibited` |
| `coredump_fault_address` | the address the faulting access reached for |
| `coredump_backtrace` | up to 16 PCs, innermost first |
| `coredump_backtrace_corrupted` | the IDF's own verdict on whether the stack walk stayed sane |
| `coredump_pc` | one PC — see the warning below |

**Start with the cause and the address.** `LoadProhibited` at `0x00000000` is a null dereference
and needs no ELF, no cable and no symbol lookup to read. Most crashes are answered right there.

**A null `coredump_cause` does not mean the dump is useless.** It means the panic was not a CPU
exception — an abort, a failed assert, a watchdog — and those leave the IDF's exception fields
zeroed. The backtrace is still there and is still the answer. Cause `0` is reported as *no*
cause deliberately: `0` is `IllegalInstruction` on the ISA, but an abort looks identical in this
struct, and naming one after the other would invent a fault that did not happen.

**`coredump_pc` is not the fault.** It is where the panic handler was running, so decoding it
tends to land somewhere in the IDF's own cache or panic code and explain nothing. It is reported
because it is what the summary provides, not because it is the useful field — that is
`coredump_backtrace[0]`.

MQTT carries the cause and the faulting address on the diagnostics topic, but **not** the
backtrace: sixteen addresses that never change, republished every interval, are payload weight
for something nobody reads in Home Assistant. Prometheus exports `heliograph_coredump_present`
as a 0/1 to alert on, and Modbus register 828 carries the same flag.

### Resolving the backtrace

The addresses are offsets into the image that was **running**, so you need that image's ELF.
Every release publishes one per board:

```bash
# The release the bridge was RUNNING when it crashed -- not the newest one.
VERSION=0.21.1
gh release download "v${VERSION}" --pattern "heliograph-*-rs485-can.elf.gz"
gunzip "heliograph-${VERSION}-rs485-can.elf.gz"
xtensa-esp32s3-elf-addr2line -pfiaC \
  -e "heliograph-${VERSION}-rs485-can.elf" 0x420529B7 0x420506CD
```

The toolchain lives at
`~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-addr2line`.

**Rebuilding the tag is not a substitute.** It produces a different layout, and the addresses
then land in unrelated functions that decode perfectly and mean nothing — tried on a real dump
from this bridge, against two candidate tags, and both gave call chains that could not exist.
If the release predates ELF publishing (before 0.21.1), the dump cannot be resolved. Clear it
and wait for one that can.

To pull the dump itself, with a cable:

```bash
python -m esp_coredump --chip esp32s3 --port /dev/tty.usbmodem* info_corefile .pio/build/waveshare-rs485-can/firmware.elf
```

The ELF you pass **must be the image that was running when it crashed**. Keep the build
artifacts from the release you flashed, or rebuild from the exact tag — a dump decoded against
a different binary produces confident nonsense.

Once you have dealt with it, clear the dump so the next one is distinguishable from this one:

```bash
curl -u admin:PASSWORD -X POST http://heliograph.local/api/v1/actions/clear-coredump
```

A new crash overwrites the old one on its own (`CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE` is not
set), so clearing is about answering "is this new?", not about making room.

## Step-debugging over the built-in USB-JTAG

The S3 has a USB-Serial-JTAG peripheral on the chip. No external probe, no extra wiring, and
the same USB-C cable that carries the serial console — JTAG and CDC are separate interfaces on
the one USB device, so `ARDUINO_USB_CDC_ON_BOOT=1` does not get in the way.

```bash
pio debug -e debug-rs485-can --interface=gdb -x .pioinit
```

Or in an editor with the PlatformIO extension, pick the `debug-rs485-can` environment and start
a debug session normally.

`debug-rs485-can` is a separate environment (`build_type = debug`, `debug_tool = esp-builtin`)
so the shipping images stay release builds. Debug drops the optimiser to `-Og`, which changes
timing — and RS485 timing is the last thing that should shift underneath you unannounced. The
image grows by roughly 130 KB, which the 6.25 MB app partition absorbs without noticing.

**The watchdog will reset the board while you sit on a breakpoint.** A halted core stops feeding
the task WDT, and its timeout is 120 s (`src/main.cpp`, `setup()`). For a long inspection,
either work in short hops or temporarily raise the timeout in that call — and put it back before
you commit.

Three places where a breakpoint earns its keep, all of them things serial logging is bad at:

- `Rs485Transport::read` / the driver's frame parser, to inspect a malformed buffer *at the
  moment* it is rejected — logging it changes the timing you are trying to observe.
- `applySerialOverride()` and driver `begin()`, for the boot path that runs once and is over
  before you can attach a console.
- Anything reached from the AsyncTCP task, where a `log::` call competes with the request it is
  trying to describe.

> If a PlatformIO-generated `.vscode/launch.json` predates 2026-07-22 it names
> `waveshare-eversolar`, the environment renamed to `waveshare-rs485-can` in the board refactor,
> and the session will fail on a path that no longer exists. Delete the file and let PlatformIO
> regenerate it; it is gitignored, so nothing in the repo needs changing.

## Recovery — hold BOOT to factory-reset

Holding **BOOT for ~5 seconds while the firmware is running** erases the stored
configuration and reboots into the setup portal. It is the only on-device recovery path on a
headless board: a config wrong enough to lock you out of the web UI (a bad hostname, a wrong
static setup) is otherwise unreachable without USB. The hold is deliberately long so it is
never one accidental brush.

**This carries more weight than it used to.** Provisioning through the setup portal is now
gated on the admin password once one exists, because that portal also returns on an
already-configured board after a few minutes without WiFi and its AP is open (see
[docs/security.md](security.md)). So for a forgotten password this reset — or a USB
`-factory.bin` flash, which wipes the same NVS — is the way back in.

**Verification status differs per board, and this is worth knowing before you rely on it:**

| Board | BOOT pin | Factory reset measured? |
|---|---|---|
| ESP32-S3-Relay-6CH | GPIO0 | **Yes** — hold, LED countdown and release confirmed on hardware 2026-07-23 |
| ESP32-S3-RS485-CAN | GPIO0 | Schematic only — the pin is confirmed, the 5 s reset has not been run on this board |
| ESP32-S3-Relay-1CH | GPIO0 | Schematic only — hardware is on the project since 2026-07-26, the 5 s reset has not been run on it |

The pin is the same GPIO0 on all three and the code path is shared, so there is no reason to
expect a difference — but "no reason to expect" is not a measurement. Running it once on an
RS485-CAN or a 1CH and reporting the result is a genuinely useful contribution.

**Not to be confused with download mode.** BOOT is GPIO0, the SoC's download strapping pin,
so holding it *at power-on or during RESET* drops the board into the USB firmware-download
mode instead — the factory reset only happens when BOOT is held during normal operation. On
boards with a status LED (the Relay-6CH) the LED blinks red while the reset counts down and a
buzzer confirms the wipe; the RS485-CAN has neither, so there the reboot is the only signal.

## Open / to be verified on hardware

1. **BOOT factory reset on the RS485-CAN and the 1CH** — see the table above.
2. **Relay polarity on the Relay-1CH, with a meter.** The relay itself was switched on 2026-07-28
   and the indicator LED followed in both directions, so actuation is confirmed — see the pin
   table above. What has not been done is putting a multimeter on the contacts to confirm which
   way NO/NC sit, which is the check that matters before wiring it to a DRM port. The 6CH's six
   were fully verified on 2026-07-23 (order, polarity, failsafe on power-cut).
3. **GPIO47 on the RS485-CAN** — no documented function there. The firmware does not touch it;
   the safest state for a pin with no known function is untouched hi-Z. It is the relay pin on
   the 1CH, which is why it appears twice in this project's history.

Component detail from the RS485-CAN schematic, for reference: SP3485EN RS485 transceiver behind
a π163E31 isolator, TJA1051T CAN transceiver, PCF85063AT RTC with 32.768 kHz crystal.
