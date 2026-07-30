# ESP32-S3 hardware and tooling audit

Audit date: **2026-07-26**, against `v0.13.2` (`96a2629`).

> **Status: acted on.** Every quick win and both medium items are implemented — see the
> plan at the bottom for what landed and what deliberately did not. One recommendation in
> this document (L1) was **withdrawn after its justification turned out to be wrong**; the
> correction is in §6 and is left visible rather than edited away.

Scope: are the ESP32-S3-specific facilities that firmware projects commonly leave on the table
actually being used here? Every claim below is backed by a `path:line` reference that was read
during the audit, not recalled. Where the answer turned out to be "already handled", that is
stated rather than dressed up as a finding — four of the eight points are in that category.

Toolchain context: pioarduino `platform-espressif32` stable, Arduino core 3.3.9 / ESP-IDF 5.5.x
(`platformio.ini:118`). The IDF options quoted below come from the prebuilt config that ships
with `framework-arduinoespressif32-libs`, per memory type — for the boards with octal PSRAM the
effective file is `esp32s3/qio_opi/include/sdkconfig.h`, not the package-root `sdkconfig`. That
distinction matters: the two disagree about PSRAM mode.

## Verdict at a glance

| # | Topic | Verdict |
|---|---|---|
| 1 | RS485 direction control | **Already correct** — hardware half-duplex mode, not a software toggle |
| 2 | Crash diagnostics | Partition **and** IDF support present; nothing reads the coredump, and no exception decoding in `pio monitor` |
| 3 | Watchdogs | **Well covered** for both application tasks; two library-owned tasks are unwatched |
| 4 | USB-JTAG debugging | Hardware and platform support present and unused; the local launch config points at an env deleted in Fase A2 |
| 5 | Randomness | No weak RNG, because **nothing generates a secret at all**; the real exposure is already documented |
| 6 | PSRAM | Enabled and implicitly used; **completely invisible in diagnostics**, and no history buffer uses it |
| 7 | Timing | **Already correct** — `esp_timer` everywhere it matters; `millis()` survives only where it is harmless |
| 8 | NVS / OTA rollback | **Verified as claimed** |

---

## 1. RS485 direction control (DE/RE)

### Status

Hardware mode, on every board that has a direction pin. `Rs485Transport::configure()` hands the
DE/RE GPIO to the UART as its RTS line and switches the peripheral into RS485 half-duplex:

- [`src/transport/rs485_transport.cpp:58-65`](../src/transport/rs485_transport.cpp#L58) —
  `uart_.setPins(-1, -1, -1, board::kRs485De)` followed by
  `uart_.setMode(UART_MODE_RS485_HALF_DUPLEX)`, both return-checked.
- [`src/transport/rs485_transport.cpp:82`](../src/transport/rs485_transport.cpp#L82) —
  `uart_.flush()` after every write, so RTS cannot drop while the tail of the frame is still
  shifting out.

There is **no** `digitalWrite` in the transport at all, and no `delayMicroseconds` around the
turnaround. The pin values are per board:

| Board | `kRs485De` | Evidence |
|---|---|---|
| RS485-CAN | 21 | [`waveshare_esp32_s3_rs485_can.h:30`](../src/boards/waveshare_esp32_s3_rs485_can.h#L30) |
| Relay-1CH | 21 | [`waveshare_esp32_s3_relay_1ch.h:30`](../src/boards/waveshare_esp32_s3_relay_1ch.h#L30) |
| Relay-6CH | **-1** | [`waveshare_esp32_s3_relay_6ch.h:41`](../src/boards/waveshare_esp32_s3_relay_6ch.h#L41) |

The `-1` on the 6CH is not a gap. The comment above it records that the board's SP485E has an
auto-direction circuit driven by the schematic's `TXDEN'` net, verified against the official
demo, and that GPIO21 — the direction pin on the other two boards — is the 6CH's buzzer. The
transport skips RTS configuration entirely in that case
([`rs485_transport.cpp:58`](../src/transport/rs485_transport.cpp#L58) guards on `>= 0`).

### Risk / missed win

None on this point. The failure mode the question asks about — a software toggle racing the last
stop bit, which gets worse as the baud rate rises — cannot occur here: the peripheral flips
direction on the bit boundary. This matters concretely, because the Growatt descriptor offers
115200 as a discovery profile (`modbus_profile/descriptor.cpp` (then `growatt_modbus/descriptor.cpp:43`)),
which is exactly where a software toggle would start truncating frames.

### Recommendation

No change. One thing worth **not** doing: do not "improve" the 6CH by inventing a direction pin.
The board header explains why a guessed pin is worse than none.

Residual item, unrelated to the toggle itself: auto-direction transceivers have a turnaround
delay of their own, so if the 6CH ever runs at 115200 on a long bus it is worth capturing a
scope trace before trusting it. That is hardware validation, not a code change.

---

## 2. Crash diagnostics

### Status

Better than the question assumes on both halves.

**A coredump partition exists in both tables**, 64 KB each:

- [`partitions_16mb_ota.csv:14`](../partitions_16mb_ota.csv#L14) — `coredump, data, coredump, 0xFF0000, 0x10000`
- [`partitions_8mb_ota.csv:15`](../partitions_8mb_ota.csv#L15) — `coredump, data, coredump, 0x7F0000, 0x10000`

**And the IDF side is enabled**, so those partitions are actually written on a panic —
from `esp32s3/qio_opi/include/sdkconfig.h`:

```
CONFIG_ESP_COREDUMP_ENABLE=y
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y
CONFIG_ESP_COREDUMP_CHECK_BOOT=y
CONFIG_ESP_COREDUMP_MAX_TASKS_NUM=64
```

So no partition change and no sdkconfig change is needed. That part of the brief is already done.

What is **not** there:

- **Nothing in the firmware ever reads the coredump.** Outside the two partition CSVs the word
  appeared nowhere in the tracked tree. There is no
  `esp_core_dump_image_check()`, no summary in the diagnostics payload, no REST route. A crash
  writes a full ELF dump to flash and the operator has no way to learn it exists without
  attaching a cable and running `espcoredump.py` by hand.
- **No exception decoding in `pio monitor`.** `platformio.ini` sets `monitor_speed = 115200`
  ([`platformio.ini:123`](../platformio.ini#L123)) and no `monitor_filters` at all. A panic
  therefore prints a raw backtrace of hex addresses. The repo does recommend an IDE-side decoder
  (`Jason2866.esp-decoder` in [`.vscode/extensions.json`](../.vscode/extensions.json)), but that
  only helps someone using that editor — it does nothing for `pio device monitor` on the command
  line or in CI.

### Risk / missed win

`ota_image_state` already tells you an image is rolling back, and the task watchdog panics on a
hang (§3) — but neither tells you **why**. Right now the answer to "it rebooted at 03:00, what
happened" is `reset_reason` as an integer and nothing else. A decoded backtrace is the difference
between a diagnosable night-time crash and a shrug. The dump is already being written; the cost
of the missing half is purely that nobody can get at it.

### Recommendation

1. Add `monitor_filters = esp32_exception_decoder, time` to `[esp32common]`. One line, decodes
   backtraces against the ELF automatically, and stamps every console line.
2. Surface coredump *presence* in diagnostics: `esp_core_dump_image_check()` returning `ESP_OK`
   means a dump is waiting. Report it as a boolean plus the summary
   (`esp_core_dump_get_summary()` gives the faulting task name and PC) in
   `GET /api/v1/diagnostics`, and clear it with `esp_core_dump_image_erase()` behind the admin
   gate. That turns "it rebooted" into "it panicked in `rs485` at `0x420...`" from a browser.
3. Document the retrieval command in `docs/hardware.md`.

---

## 3. Watchdogs

### Status

All three watchdogs are on, and the application tasks are explicitly covered.

From the prebuilt IDF config:

```
CONFIG_ESP_INT_WDT=y                      # interrupt WDT, 300 ms
CONFIG_ESP_INT_WDT_TIMEOUT_MS=300
CONFIG_ESP_INT_WDT_CHECK_CPU1=y
CONFIG_ESP_TASK_WDT_EN=y
CONFIG_ESP_TASK_WDT_PANIC=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=5           # default, overridden in firmware
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y
CONFIG_BOOTLOADER_WDT_ENABLE=y            # RTC WDT, 9000 ms
```

The firmware reconfigures the task WDT and registers both application tasks at
[`src/main.cpp:1123-1132`](../src/main.cpp#L1123):

```cpp
esp_task_wdt_config_t wdtConfig = {
    .timeout_ms    = 120000,
    .idle_core_mask = 1 << 0,
    .trigger_panic = true,
};
esp_task_wdt_reconfigure(&wdtConfig);
enableLoopWDT();
if (rs485Handle != nullptr) {
    esp_task_wdt_add(rs485Handle);
}
```

On the specific question — **is the task that parses potentially malformed AA55/Modbus data
separately watched?** Yes. There is no separate parser task; parsing happens inside `rs485Task`,
which is created pinned to core 1 at [`src/main.cpp:1114`](../src/main.cpp#L1114) and added to
the task WDT at line 1131. `trigger_panic = true` means a hang there resets the device, and on an
unconfirmed OTA image that reset is what triggers the rollback.

Feeding is deliberate rather than incidental: the discovery run feeds **per probe**, not once per
sweep ([`src/main.cpp:767`](../src/main.cpp#L767), with the reasoning in
[`discovery_engine.h:111-114`](../src/drivers/discovery_engine.h#L111) and
[`discovery_runner.h:61-64`](../src/app/discovery_runner.h#L61)).

### Risk / missed win

Two real observations, neither a defect:

**Two library-owned tasks are unwatched.** `esp_task_wdt_add` is called exactly once
([`src/main.cpp:1131`](../src/main.cpp#L1131)). The AsyncTCP task that runs the web server and the
task espMqttClient creates for itself are not registered. A hang in either is invisible to the
task WDT: the web UI and MQTT would go dead while polling, Modbus TCP and the LED carry on, and
nothing resets. Worth knowing; **not** worth blindly fixing, because those tasks may legitimately
block for reasons the firmware does not control, and a spurious panic on a healthy device is a
worse outcome than a dead web UI.

**The 120 s timeout is global.** `esp_task_wdt_reconfigure` applies to every watched task,
including the core-0 idle task that `idle_core_mask` keeps. So idle starvation on core 0 — which
the IDF default would have caught in 5 s — now takes two minutes. The comment at
[`main.cpp:1119-1122`](../src/main.cpp#L1119) justifies the widening for discovery, and that
justification is sound, but the side effect on idle coverage is not stated there.

### Recommendation

1. **Low effort, real value:** add a liveness heartbeat for the two unwatched subsystems rather
   than a watchdog registration. `MqttOutput` and `RestApi` already have a `loop()`/notify path;
   record a last-serviced timestamp for each in `Diagnostics` and expose the age. That makes a
   stalled AsyncTCP visible in Prometheus without risking a panic on a healthy device.
2. Consider narrowing the task WDT to the two application tasks and leaving the idle tasks on a
   shorter timeout. The API does not support per-task timeouts, so this means dropping
   `idle_core_mask` and accepting no idle coverage, or keeping today's trade-off. Given the
   discovery path genuinely needs the headroom, **keeping today's behaviour and documenting the
   trade-off in the comment is the better answer.**
3. Add a note to the comment at `main.cpp:1119` naming the idle-coverage side effect.

---

## 4. Debug tooling (USB-JTAG)

### Status

The S3's built-in USB-Serial-JTAG is available and completely unused.

- The board manifest already declares it: `esp32-s3-devkitc-1.json` carries
  `"debug": {"default_tool": "esp-builtin", "onboard_tools": ["esp-builtin"], "openocd_target": "esp32s3.cfg"}`.
  So **no `debug_tool` line is required** — PlatformIO would pick `esp-builtin` on its own.
- `platformio.ini` sets no `debug_tool`, no `debug_init_break`, and no `build_type`
  (grep across the file returns only `monitor_speed` at line 123). Every environment therefore
  builds `release`, without the `-Og` and full symbols a debug session wants.
- Only `.vscode/extensions.json` is tracked ([`git ls-files .vscode/`](../.vscode/)); the rest of
  `.vscode/` is gitignored by `.gitignore:18-19`.
- The locally present, PlatformIO-generated `.vscode/launch.json` points at
  `projectEnvName: "waveshare-eversolar"` and an executable under
  `.pio/build/waveshare-eversolar/firmware.elf`. **That environment no longer exists** — it was
  renamed to `waveshare-rs485-can` in Fase A2. A `PIO Debug` launch would fail on a stale path.

Worth stating for anyone who tries this: on these boards the same USB-C connector carries both
the CDC console and the JTAG interface (they are separate interfaces on the one USB peripheral),
so `ARDUINO_USB_CDC_ON_BOOT=1` at [`platformio.ini:133`](../platformio.ini#L133) does not conflict
with debugging.

### Risk / missed win

Every bug in this project so far has been found by log archaeology — the 4 KB stack overflow, the
UTC boot line, the reconnect counter. Several of those would have taken minutes with a breakpoint
and a watch expression instead of a reflash-and-wait cycle. The single most valuable use is the
one thing serial logging is worst at: inspecting a corrupt RS485 frame buffer *at the moment* the
parser rejects it, without the act of logging changing the timing.

### Recommendation

1. Add a `[env:debug-rs485-can]` inheriting `waveshare-rs485-can` with `build_type = debug` and
   `debug_tool = esp-builtin` (explicit, even though it is the default, because it documents the
   intent). Keep it separate so the shipping envs stay `release`.
2. Delete the stale local `.vscode/launch.json` and let PlatformIO regenerate it against the new
   env name. Nothing in the repo needs changing — it is gitignored — but it is a trap for the next
   person, so mention it in the docs.
3. Document the flow in `docs/hardware.md`: which USB port, how to set a breakpoint in
   `rs485Task`, and the caveat that halting the core stops the watchdog feed, so a debug session
   will trip the task WDT unless it is temporarily disabled.

---

## 5. Security and randomness

### Status

The honest answer to "does credential generation use `esp_random()` or something weaker" is:
**neither, because the firmware never generates a credential.** A grep for `esp_random`,
`esp_fill_random`, `random(`, `rand()`, `srand`, `mbedtls_ctr_drbg` and `getentropy` across `src/`
returns nothing.

What exists instead:

- **HTTP Basic with an operator-set password.** `security.adminPassword`
  ([`configuration.h:117`](../src/config/configuration.h#L117)) is never serialised outward
  ([`configuration.cpp:404`](../src/config/configuration.cpp#L404) publishes only
  `password_set`), and mutations are refused outright when it is empty — an unprovisioned device
  is locked, not open.
- **No session tokens, no cookies, no nonces.** Every request re-presents the password.
- **The setup AP is open**: `WiFi.softAP(apSsid_.c_str())` with no passphrase
  ([`wifi_manager.cpp:73`](../src/network/wifi_manager.cpp#L73)), SSID derived from the MAC
  ([`provisioning_policy.cpp:48`](../src/network/provisioning_policy.cpp#L48)).

None of this is undocumented. `docs/security.md` already states the open AP (line 37), HTTP Basic
over plain HTTP putting the password on the air (lines 28, 96-99), unencrypted NVS (line 32), no
brute-force protection (line 103) and unsigned OTA images (line 106), each with reasoning.

### Risk / missed win

No weak-RNG vulnerability exists today, because there is no RNG use to be weak. The finding is
about the future: **the moment a session token, a CSRF nonce, a pairing code or a generated
initial password is introduced, the obvious reach is `random()` from the Arduino API, which on
ESP32 is a seeded PRNG and wrong for this.** That is a footgun waiting rather than a hole open.

One point worth knowing if this ever gets built: `esp_random()` is only a true hardware RNG when
the RF subsystem is active or the bootloader's entropy is still valid. During early boot with
WiFi down — precisely when a first-boot password would be generated — the guarantee is weaker.
`esp_fill_random()` after `WiFi.begin()` is the safe window.

### Recommendation

1. **No code change now.** Do not add randomness that nothing consumes.
2. Add one line to `docs/security.md` recording the decision: *if* a token is ever introduced it
   must come from `esp_random()`/`esp_fill_random()`, never `random()`, and not before the RF
   subsystem is up. Cheap insurance against the reflex reach for `random()`.
3. Optional and separate from this audit: WPA2 on the setup AP with a per-device passphrase
   derived from the MAC would close the "password on the air" window at lines 96-99 of
   `security.md`. That is a deliberate UX trade (the passphrase has to reach the user somehow),
   so it belongs in its own discussion, not in a hardware-utilisation sweep.

---

## 6. PSRAM

### Status

Present, enabled, implicitly used for large allocations — and invisible.

**Configuration.** The RS485-CAN, Relay-1CH and mock envs build with
`board_build.arduino.memory_type = qio_opi` and `-DBOARD_HAS_PSRAM`
([`platformio.ini:184,190`](../platformio.ini#L184) (RS485-CAN), `:204,207` (Relay-1CH), `:233,238` (mock)). The effective IDF
config for that memory type is `esp32s3/qio_opi/include/sdkconfig.h`:

```
#define CONFIG_SPIRAM 1
#define CONFIG_SPIRAM_MODE_OCT 1
#define CONFIG_SPIRAM_USE_MALLOC 1
#define CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 4096
```

Note this contradicts the package-root `sdkconfig`, which says `CONFIG_SPIRAM_MODE_QUAD`. The
per-memory-type file is the one that applies; reading only the root file would give the wrong
answer. The Relay-6CH uses `qio_qspi` and deliberately omits `BOARD_HAS_PSRAM`
([`platformio.ini:221`](../platformio.ini#L221)) because the module is an N8 with no PSRAM.

**`SPIRAM_USE_MALLOC` with a 4096-byte threshold means PSRAM is already in use** — every
allocation above 4 KB goes there through plain `malloc`, with no code change. So "PSRAM sits
unused" would be wrong.

**But no code uses it deliberately.** A grep for `ps_malloc`, `heap_caps_malloc`,
`MALLOC_CAP_SPIRAM`, `psramFound` and `ESP.getPsram` across `src/` returns nothing.

**And — the finding that matters — PSRAM is absent from every diagnostic surface.** The three heap
figures reported at [`src/main.cpp:249-251`](../src/main.cpp#L249) are
`ESP.getFreeHeap()`, `ESP.getMinFreeHeap()` and `ESP.getMaxAllocHeap()`. In Arduino core 3.3.9
(`cores/esp32/Esp.cpp:164-174`) all three are `heap_caps_*(MALLOC_CAP_INTERNAL)` — **internal SRAM
only**. So the ~150 KB the production bridge reports says nothing whatsoever about the 8 MB of
PSRAM, and there is no way to tell from a running device whether PSRAM initialised at all.

**No measurement history exists.** `StateStore` holds a single `current_` handle
([`state_store.h:39-42`](../src/state/state_store.h#L39)). The only ring buffer in the firmware is
the 64-line log ring ([`log_buffer.h:27-30`](../src/diagnostics/log_buffer.h#L27)).

### Risk / missed win

Two distinct things:

1. **Observability gap, live today.** If PSRAM silently failed to train on a board, nothing would
   report it — not REST, not Prometheus, not the Modbus diagnostics block. The bridge would run
   on internal RAM with a quietly reduced ceiling. This is the one item in this section that is a
   present-tense defect in the monitoring, not a future opportunity.
2. **A possible capability, on a weaker basis than first stated.** Every REST query sees only
   *now*, and with 8 MB available a ring of per-device samples would be nearly free: 24 hours at
   60-second resolution is roughly 2.8 MB, well beyond anything internal SRAM could hold.

   ⚠️ **The original version of this paragraph also claimed a device-side ring would stop
   Prometheus scrape gaps from being permanent data loss. That is wrong, and the claim is
   withdrawn.** Prometheus records one sample per series per scrape, at scrape time. The text
   exposition format does allow an optional per-sample timestamp, but it cannot carry a *series*
   of historical points for one metric in a single scrape, and our exporter emits none anyway
   (`prometheus_metrics.cpp`, `appendValue`). So a missed scrape stays missed no matter what the
   device remembers. Device-side history would serve REST and a dashboard curve — a feature —
   and would do nothing for the monitoring gap it was justified by.

### Recommendation

1. **Quick win, do this first:** add `psram_size_bytes` and `psram_free_bytes` to `BridgeInfo` from
   `ESP.getPsramSize()` / `ESP.getFreePsram()` (both guarded by `psramFound()` in the core, so
   they return 0 safely on the 6CH), and carry them into the diagnostics payload, the Prometheus
   output and the Modbus diagnostics registers. Then confirm against real hardware that the
   RS485-CAN reports ~8 MB and the Relay-6CH reports 0.
2. **Medium:** a PSRAM-backed sample ring behind an interface, feeding a `?since=` parameter on
   the measurements route. Design constraints worth fixing up front: allocate explicitly with
   `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` rather than relying on the 4 KB threshold; the ring
   must be a bounded, pre-allocated block, not a growing container; and it must degrade to
   "history disabled" on a board without PSRAM rather than fail to boot.
3. Do **not** move the log ring to PSRAM. It is 16 KB, it is touched from a panic path, and PSRAM
   is not accessible in every crash context.

---

## 7. Timing

### Status

`esp_timer` is already the monotonic clock everywhere a protocol deadline depends on it, and the
reasoning is recorded at each site:

- [`src/main.cpp:168-175`](../src/main.cpp#L168) — `nowMs()` is
  `esp_timer_get_time() / 1000`, explicitly chosen over `millis()` because the 32-bit wrap at
  49.7 days is a scheduled outage on a device that runs for months.
- [`src/transport/rs485_transport.cpp:135-138`](../src/transport/rs485_transport.cpp#L135) —
  `Rs485Transport::nowMs()`, same, because drivers build absolute transaction deadlines from it.
- [`src/network/time_manager.cpp:32-35`](../src/network/time_manager.cpp#L32) — uptime stamps.

Those feed the transaction deadlines in
[`modbus_client.cpp:32,36`](../src/protocols/modbus/modbus_client.cpp#L32) and the per-driver
`kTransactionDeadlineMs`.

`millis()` and `delay()` survive in exactly one place:
[`rs485_transport.cpp:98,103,110`](../src/transport/rs485_transport.cpp#L98) — the read poll loop,
which waits for bytes with `while (millis() - start < timeoutMs) { ... delay(1); }`. The
subtraction form is deliberate and wrap-safe (comment at lines 95-97).

### Risk / missed win

The question implies frame-gap timing might be at risk. It is not, and the reason is structural:
**this Modbus implementation does not use t3.5 inter-frame gap detection at all.** Framing is
length-and-CRC based — `parseReadResponse` is called repeatedly on the accumulating buffer and
succeeds when a complete, CRC-valid frame is present
([`modbus_client.cpp:41-58`](../src/protocols/modbus/modbus_client.cpp#L41)), with a transaction
deadline as the only clock involved. So the 1 ms granularity of `delay(1)` cannot corrupt a frame;
it can only delay noticing one.

That delay is worth quantifying honestly. At 9600 baud a character is ~1.04 ms, so 1 ms polling is
well matched. At 115200 — offered by the Growatt profile — a character is ~87 µs, so the poll
granularity is ~11 character times. The cost is added latency per read chunk and a few extra
scheduler wakeups, not data loss.

### Recommendation

1. No change to the clock sources. This point is already solved better than most projects manage.
2. Optional, low priority: replace the `delay(1)` poll with a blocking UART read that yields on a
   queue (`uart_read_bytes` with a tick timeout), which removes the busy-poll granularity
   entirely. Measurable benefit only at 115200 with several devices on the bus, so this is worth
   doing **after** the multi-device Growatt bring-up gives a real workload to measure, not before.
3. If a protocol is ever added that genuinely needs t3.5 gap detection, that is the moment to
   reach for the UART's hardware `rx_timeout` threshold, which counts in bit periods — not to
   tighten the software poll.

---

## 8. NVS and OTA rollback

Verification, as requested, not guesswork. The README's claims hold.

### Settings persistence

NVS via the Arduino `Preferences` wrapper, in
[`src/config/nvs_backend.cpp`](../src/config/nvs_backend.cpp):

- read: `prefs.begin(namespace_, /*readOnly=*/true)` then `prefs.getString(...)` (lines 17-25)
- write: `prefs.begin(namespace_, /*readOnly=*/false)` then `prefs.putString(...)` (lines 34-38)
- erase: lines 46-47

Each operation opens and closes its own handle rather than holding one open. The `nvs` partition
is declared at `partitions_16mb_ota.csv:9` / `partitions_8mb_ota.csv:10` (0x5000 = 20 KB).
The host build gets a stub (`nvs_backend.cpp:59-63`), which is why the configuration store is
testable in `-e native`.

### OTA and rollback

Upload through the core's `Update.h`, rollback through the IDF's OTA ops
([`src/ota/ota_manager.cpp`](../src/ota/ota_manager.cpp)):

- `Update.begin(size, U_FLASH)` / `Update.write` / `Update.end(true)` — lines 81, 116, 131
- image state read via `esp_ota_get_running_partition()` + `esp_ota_get_state_partition()` —
  lines 58-60, mapped to the `ota_image_state` string at lines 64-69
- **the rollback is cancelled by** `esp_ota_mark_app_valid_cancel_rollback()` at line 151

The gating is the part worth confirming, because it is where this went wrong once before. The
comment at lines 51-53 records that an earlier version confirmed the image too early, so "the
rollback we thought we had was being cancelled before one existed". The current gate is
`shouldConfirmHealthyBoot(wifiConnected, uptimeMs, alreadyConfirmed, ...)`
([`ota_manager.h:64`](../src/ota/ota_manager.h#L64)), called from `loop()` and deliberately **not**
gated on a successful inverter poll — an inverter is absent every night. Dual app partitions back
this: `app0`/`app1` at `partitions_16mb_ota.csv:11-12`.

This chain was exercised end to end on hardware in this session's 0.13.1 and 0.13.2 flashes:
`ota_image_state` read `valid` about 30 s after each boot.

### Recommendation

No change. The one adjacent gap is that OTA images are unsigned, which `docs/security.md:106-110`
already records as a known, reasoned position.

---

## Prioritised plan

### Quick wins — hours, low risk  ✅ all implemented

| # | Item | Impact | Test |
|---|---|---|---|
| Q1 | `monitor_filters = esp32_exception_decoder, time` in `[esp32common]` | Every future panic prints a symbolised backtrace instead of hex | Not unit-testable. Validate by forcing a panic on a bench board (`abort()` behind a debug-only REST route, or pull the RS485 task into a deliberate hang) and confirming the decoded frames |
| Q2 | Report PSRAM in diagnostics: `psram_size_bytes` / `psram_free_bytes` through `BridgeInfo` → REST, Prometheus, Modbus diagnostics block | Closes a live monitoring blind spot over 8 MB of RAM; proves PSRAM actually trains on each board | Host tests in `-e native` for the payload builders and the register map (both already fully host-tested — add the two fields to the existing diagnostics tests). Hardware: RS485-CAN must report ~8 MB, Relay-6CH must report 0 |
| Q3 | `[env:debug-rs485-can]` with `build_type = debug` + `debug_tool = esp-builtin`; document the flow and the watchdog caveat in `docs/hardware.md` | Makes step-debugging a one-command operation instead of a research project | CI: the new env must appear in the build matrix or be explicitly excluded — decide which. Hardware: one breakpoint hit in `rs485Task` |
| Q4 | Document the RNG rule in `docs/security.md`; note the idle-coverage side effect at `main.cpp:1119` | Prevents the reflex `random()` reach and an incorrect reading of the WDT config | Docs only |

Q1 and Q4 are effectively free. Q2 is the one with real present-tense value.

### Medium — a day or two each  ✅ both implemented (M2 in a different shape — see below)

| # | Item | Impact | Test |
|---|---|---|---|
| M1 | Coredump surfacing: `esp_core_dump_image_check()` + `esp_core_dump_get_summary()` in diagnostics, admin-gated erase route, retrieval documented | Turns an unexplained night-time reboot into a named faulting task and PC, without a cable | The IDF calls are not host-compilable, so keep the presentation pure: a `CoredumpSummary` struct formatted by a host-tested payload builder, with the IDF call behind the same `#if defined(ESP32)` split the OTA manager already uses. Hardware: force a panic, reboot, confirm the summary appears and that erasing clears it |
| M2 | ~~Liveness heartbeats~~ → **MQTT publish-failure counter** | Implementing the heartbeats showed both were the wrong shape. **The web server needs none:** a successful `/metrics` scrape is itself proof the AsyncTCP task is alive, so the field would be true exactly whenever you could read it. **MQTT needed something better:** `publish()` returns 0 when the client refuses a message (link down, outbox full), and eleven of twelve call sites discarded that — so an undelivered message looked delivered, while `mqtt_connected` stayed true. Now counted and exported | Two host tests. Hardware: block the broker and confirm the counter climbs while polling continues |

### Larger — L1 withdrawn, L2 deliberately deferred

| # | Item | Impact | Test |
|---|---|---|---|
| L1 | ~~PSRAM-backed measurement history ring~~ | **Withdrawn.** Its stated benefit — closing Prometheus scrape gaps — does not exist (see §6). What remains is a dashboard feature: a device-side curve without Home Assistant. Worth doing if that is wanted for its own sake, on its own design pass, and not justified by monitoring |
| L2 | Blocking UART read replacing the `delay(1)` poll | Latency only, and only at 115200 with several devices | **Not done, on purpose.** It touches `Rs485Transport::read`, the one path that talks to a live inverter, for a benefit nobody can currently measure. The current loop returns as soon as any byte arrives and the caller reassembles frames; a naive blocking read would wait for a full buffer and stall on every short reply. Revisit when the Growatt bus exists to measure against |

### Explicitly not recommended

- **Changing the RS485 direction handling.** It is already the hardware path, on the boards that
  have a direction pin, with the 6CH's `-1` a verified fact rather than a gap.
- **Replacing `esp_timer` or adding `micros()` anywhere.** The clock story is right.
- **Adding a coredump partition or an sdkconfig coredump option.** Both already exist.
- **Adding `esp_random()` calls.** Nothing consumes randomness; adding an unused RNG is noise.
- **Moving the log ring to PSRAM.** It is touched from panic paths where PSRAM may be unreachable.

## What this audit found that was already right

Four of the eight points needed no work at all: RS485 hardware half-duplex (§1), watchdog coverage
of both application tasks including the parser path (§3), `esp_timer` as the monotonic clock (§7),
and the NVS/OTA-rollback implementation the README describes (§8). Two more were half-done in the
place people usually forget — the coredump partition and its IDF support both exist (§2), and
PSRAM is configured and implicitly carrying large allocations (§6).

The genuinely open items are narrower than the brief assumed: nothing can **read** the coredump,
nothing **reports** PSRAM, and the built-in JTAG has never been wired into the workflow.
