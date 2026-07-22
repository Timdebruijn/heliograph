# Architecture

## Layering

```
┌──────────────────────────────────────────────────────────────┐
│ Physical            RS485 (MVP) │ RS232 │ CAN │ TCP (later)  │
└───────────────────────────────┬──────────────────────────────┘
                                │
┌───────────────────────────────▼──────────────────────────────┐
│ Transport      Transport (abstract)                          │
│                Rs485Transport   MockTransport                │
│                UART-config, framing help, timeouts, bus lock │
└───────────────────────────────┬──────────────────────────────┘
                                │  bytes in/out — no protocol knowledge
┌───────────────────────────────▼──────────────────────────────┐
│ Driver         InverterDriver (abstract)                     │
│                EversolarDriver  MockDriver                   │
│                [later] GrowattDriver, SolaxDriver, ...       │
└───────────────────────────────┬──────────────────────────────┘
                                │  writes only after a fully valid poll
┌───────────────────────────────▼──────────────────────────────┐
│ State          DeviceContext → DeviceState (immutable snap)  │
│                StateStore (thread-safe), DeviceManager       │
└───────────────────────────────┬──────────────────────────────┘
                                │  reads snapshots — read-only
┌───────────────────────────────▼──────────────────────────────┐
│ Outputs        MQTT │ Modbus TCP │ REST │ Web │ Prometheus   │
└──────────────────────────────────────────────────────────────┘
```

The hard rule: **brand-specific knowledge exists exclusively in `src/drivers/<driver>/`.**
No output module knows the word "EverSolar", except in one place: the string in
`DeviceIdentity::manufacturer`, which comes from the driver and is passed through as data.

This is mechanically enforceable and is checked by `tools/check_layering.sh`:

```
1. Brand names do not appear outside src/drivers/  — not even in comments
2. The host-testable core does not include any Arduino/ESP-IDF headers
3. The fixtures are in sync with their generator
```

**This check must be run in full.** Its last line is `RESULT: PASS` or
`RESULT: FAIL`. Earlier versions ended with the "OK" of the final sub-check, so that
`check_layering.sh | tail -1` reported success while an earlier check had failed — that is
exactly what happened during Phase 7, and it hid a real violation for an entire phase.

Rule 1 deliberately also applies to comments. The moment the canonical model starts explaining
itself in terms of one driver ("the EverSolar doesn't have this field"), the rule dilutes into
"ah, it's just a comment" and becomes unenforceable. `errorCodeSupported` therefore documents
*why* the flag exists, not which inverter needed it.

## Data flow and task model

```
        ┌──────────────┐   uart1 (excl.)   ┌──────────────┐
        │ rs485Task    │ ◄───── mutex ────►│  RS485 bus   │
        │ core 1 pr 5  │                   └──────────────┘
        │ 8192 B       │
        └──────┬───────┘
               │ publish(snapshot)   ← only writer
        ┌──────▼───────┐
        │  StateStore  │  shared_ptr<const DeviceState>, mutex
        └──────┬───────┘
               │ snapshot()          ← only readers
     ┌─────────┼─────────┬─────────────┬────────────┐
     ▼         ▼         ▼             ▼            ▼
  rs485Task AsyncTCP  AsyncTCP     loopTask     eModbus
  (MQTT     (REST)    (web/SSE)    core 1       (Modbus TCP)
  publish*) pr 3      pr 3         pr 1         pr 3
```

\* MQTT publishing runs on `rs485Task` itself: `MqttOutput::loop()` builds the payloads and
`publish()` only enqueues into espMqttClient's outbox. The library's own `mqttclient` task
drives the socket. See "MQTT task model" below.

### Tasks

| Task | Core | Prio | Stack | Responsible for | WDT |
|---|---|---|---|---|---|
| `rs485Task` | 1 | 5 | 8192 | UART1, active driver, poll cycle, discovery, and pushing snapshots to the outputs (`MqttOutput::loop()`, Modbus refresh, SSE notify). 8192 since the stack-canary crash of 2026-07-19 (TRACE path through newlib vsnprintf); see main.cpp | ✔ |
| `mqttclient` (library) | 1 | 1 | 5120 | espMqttClient's internal task: socket TX/RX, keepalive, draining the outbox, and running the `onMessage`/`onDisconnect` callbacks | ✖ |
| `loopTask` (Arduino) | 1 | 1 | 8192 | Wifi supervision, mDNS, diagnostics, relay safety | ✔ |
| `async_tcp` (library) | 0 | 3 | 8192 | REST, web, SSE — managed by AsyncTCP | ✖ |
| `eModbus server` (library) | 0 | 3 | 4096 | Modbus TCP clients | ✖ |

Two of our own tasks, three library tasks. Deliberately minimal.

Rationale for the core split: wifi/lwIP runs on core 0 by default. By pinning `rs485Task` to
core 1 **at priority 5**, network load cannot disturb RS485 timing — exactly the acceptance
criterion "Modbus client errors do not interrupt inverter polling". The `mqttclient` task also
lives on core 1 (the library's default constructor pins it there at priority 1), but it cannot
preempt the higher-priority `rs485Task`; the MQTT payload building that runs on `rs485Task`
itself is microseconds of pure CPU against a protocol with second-scale timeouts.

Watchdog: `esp_task_wdt` with a 120 s timeout (an extended discovery scan legitimately runs
many back-to-back 3 s transactions between feeds); `rs485Task` and `loopTask` register with
it, the library tasks do not. `rs485Task` calls `esp_task_wdt_reset()` every cycle, even when
the poll fails — an unreachable inverter must never cause a reset.

### MQTT task model and thread safety

VERIFIED 2026-07-22 against the vendored espMqttClient 1.7.x sources (not the online docs,
which are silent on this): on ESP32 every `publish()`/`subscribe()` takes a FreeRTOS mutex
around the outbox (`EMC_SEMAPHORE_TAKE` in `MqttClient.h`/`.cpp`) and the connection state is
`std::atomic`. Calling `publish()` from a different task than the library's own is therefore
safe by design — the library exists to be used exactly this way: its `mqttclient` task pumps
the socket while the application enqueues from wherever it runs.

The flip side: the `onMessage`/`onDisconnect` callbacks run on the `mqttclient` task, so
anything they touch is cross-task state. `MqttOutput` keeps those callbacks down to atomics
(`relayAckRequested_`, `resyncRequested_`, `lastDisconnectReason_`) and the mutex-guarded
`Diagnostics::setLastError()`; the state owned by `loop()` (throttle, discovery bookkeeping)
is only ever mutated on the task that calls `loop()`.

### Shared resources and locking

| Resource | Owner | Protection |
|---|---|---|
| UART1 | `rs485Task` | `SemaphoreHandle_t busMutex` in `Rs485Transport` |
| `DeviceState` | `StateStore` | mutex + `shared_ptr<const>` snapshot |
| NVS config | `ConfigurationStore` | mutex; writes only from `loopTask` |
| Diagnostics counters | `Diagnostics` | `std::atomic<uint32_t>` |

A snapshot is immutable: readers get a `shared_ptr<const DeviceState>` and can hold onto it as
long as they want without the mutex. The writer builds a new `DeviceState` and swaps the
pointer under the mutex. No copy cost per reader, no tearing.

**Output modules have no path whatsoever to the UART.** They only see `StateStore`.

## Interfaces

### Transport

```cpp
enum class TransportType { Rs485, Rs232, Can, Tcp, Mock };

struct SerialProfile {
    uint32_t      baudRate          = 9600;
    SerialParity  parity            = SerialParity::None;
    uint8_t       dataBits          = 8;
    uint8_t       stopBits          = 1;
    uint32_t      responseTimeoutMs = 1000;
    uint8_t       retries           = 3;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual TransportType type() const = 0;
    virtual bool     configure(const SerialProfile& profile) = 0;
    virtual void     flushInput() = 0;
    virtual size_t   write(const uint8_t* data, size_t len) = 0;
    // Reads up to `len` bytes or until timeout. Frame detection is up to the driver.
    virtual size_t   read(uint8_t* buf, size_t len, uint32_t timeoutMs) = 0;
    virtual bool     lock(uint32_t timeoutMs) = 0;
    virtual void     unlock() = 0;
    virtual const TransportStats& stats() const = 0;
};
```

The transport layer has **no** knowledge of EverSolar framing. It provides bytes and timeouts.
The driver determines when a frame is complete (for EverSolar: read header → length byte →
read the rest).

`Rs485Transport::configure()` does exactly what the Waveshare demo proves:

```cpp
uart_.begin(profile.baudRate, toArduinoConfig(profile), board::kRs485Rx, board::kRs485Tx);
uart_.setPins(-1, -1, -1, board::kRs485De);         // GPIO21 as RTS
uart_.setMode(UART_MODE_RS485_HALF_DUPLEX);         // UART drives DE/RE itself
```

### InverterDriver

```cpp
class InverterDriver {
public:
    virtual ~InverterDriver() = default;
    virtual const DriverDescriptor& descriptor() const = 0;
    virtual bool                 begin(Transport& transport) = 0;
    virtual ProbeResult          probe() = 0;
    virtual PollResult           poll(DeviceState& state) = 0;
    virtual DeviceIdentity       identity() const = 0;
    virtual InverterCapabilities capabilities() const = 0;
    virtual CommandResult        execute(const InverterCommand& command) = 0;
};
```

Per the project brief. `EversolarDriver::execute()` **always** returns
`CommandResult::Unsupported` — not as a temporary MVP limitation but because control codes
`0x12` WRITE and `0x13` EXECUTE are empty in the reference: there is no known write operation
for this protocol at all.

```cpp
enum class PollResult { Ok, Timeout, ChecksumError, InvalidFrame, NotRegistered, TransportError };
```

`poll()` may **only** touch `state` on `Ok`. On any other outcome the previous state remains
unchanged — this way a half-filled measurement never occurs.

### DriverDescriptor

```cpp
enum class DriverSupportLevel { Experimental, Beta, Stable, Deprecated };

struct DriverDescriptor {
    std::string                id;
    std::string                displayName;
    std::string                manufacturer;
    std::string                protocol;
    std::string                description;
    std::vector<TransportType> supportedTransports;
    std::vector<SerialProfile> recommendedSerialProfiles;
    DriverSupportLevel         supportLevel;
    int                        probePriority;
    bool                       supportsAutoDetection;
    bool                       supportsMultipleDevices;
    bool                       supportsRead;
    bool                       supportsWrite;
};
```

EverSolar:

```cpp
{
  .id = "eversolar_legacy",
  .displayName = "Ever-Solar / Zeversolar (legacy PMU)",
  .manufacturer = "Ever-Solar",
  .protocol = "EverSolar PMU RS485",
  .supportedTransports = { TransportType::Rs485, TransportType::Mock },
  .recommendedSerialProfiles = { { 9600, SerialParity::None, 8, 1, 1000, 3 } },  // only one known
  .supportLevel = DriverSupportLevel::Experimental,   // → Beta after Phase 3, Stable after Phase 9
  .probePriority = 10,
  .supportsAutoDetection = true,
  .supportsMultipleDevices = true,    // protocol supports it; MVP allows 1
  .supportsRead = true,
  .supportsWrite = false,             // 0x12/0x13 empty in the protocol
}
```

Only **one** serial profile: 9600 8N1 is hardcoded in the reference. We do not try other
combinations — that would be blind brute-forcing.

### DriverRegistry

```cpp
using DriverFactory = std::function<std::unique_ptr<InverterDriver>(Transport&)>;

class DriverRegistry {
public:
    void registerDriver(const DriverDescriptor& d, DriverFactory factory);
    std::vector<DriverDescriptor> availableDrivers() const;
    std::unique_ptr<InverterDriver> create(const std::string& driverId, Transport& t);
};
```

Registration is compile-time conditional:

```cpp
void registerBuiltinDrivers(DriverRegistry& r) {
#if ENABLE_DRIVER_EVERSOLAR
    r.registerDriver(eversolar::descriptor(), eversolar::factory);
#endif
#if ENABLE_DRIVER_MOCK
    r.registerDriver(mock::descriptor(), mock::factory);
#endif
}
```

Adding a new driver = a folder, a `descriptor.cpp`, an `#if` block, fixtures + tests.
**Zero changes** in MQTT, REST, Modbus, or web.

## Canonical data model

### Measurement

```cpp
struct Measurement {
    std::string     id;            // "ac.power.total"
    std::string     displayName;
    MeasurementType type;
    Unit            unit;
    double          value;
    bool            supported;     // whether the driver supports this field at all
    bool            valid;         // last poll produced a valid value
    bool            stale;         // value is too old
    bool            derived;       // calculated, not measured (e.g. dc.power.total)
    uint64_t        timestampMs;
};
```

`derived` is an addition to the model from the project brief. Reason: with EverSolar,
`dc.power.total` is `VPV × IPV` and not a measurement. Without that flag, a consumer would
suggest a precision that isn't there.

The three flags are strictly distinct:

| `supported` | `valid` | `stale` | Meaning |
|---|---|---|---|
| false | — | — | Driver never provides this field → **do not publish** |
| true | false | — | Supported, but never yet read validly → NaN/null |
| true | true | true | Value known but outdated → publish with the stale flag |
| true | true | false | Current measurement |

### Measurement IDs filled by the EverSolar driver

| ID | Source |
|---|---|
| `ac.power.total` | `PAC` |
| `ac.phase_l1.voltage` | `VAC` |
| `ac.phase_l1.current` | `IAC` |
| `ac.frequency` | `FREQUENCY` |
| `dc.mppt_1.voltage` | `VPV` |
| `dc.mppt_1.current` | `IPV` |
| `dc.mppt_1.power` | *derived* |
| `dc.mppt_2.*` | only with 32-byte payload |
| `dc.power.total` | *derived* |
| `energy.today` | `E_TODAY` ÷100 |
| `energy.total` | uint32(`HI`,`LO`) ÷10 |
| `inverter.temperature` | `TEMP`, **signed** |
| `inverter.operating_hours` | `HOURS_UP` |

Not filled (`supported = false`): all `ac.phase_l2/l3.*`, `battery.*`, `grid.*`,
`ac.phase_l1.power`. These **do not** appear in MQTT, not in HA, and are NaN in Modbus.

### DeviceState

```cpp
struct DeviceState {
    bool     bridgeOnline;
    bool     inverterOnline;
    bool     dataValid;
    bool     dataStale;
    uint64_t lastPollAttemptMs;
    uint64_t lastSuccessfulPollMs;
    uint32_t consecutiveFailures;
    DeviceIdentity        identity;
    InverterCapabilities  capabilities;
    std::vector<Measurement> measurements;
    uint16_t    statusCode;
    uint16_t    errorCode;
    bool        errorCodeSupported;   // EverSolar: false
    std::string statusText;
};
```

### Stale and offline logic

Per the project brief:

| Event | Effect |
|---|---|
| 1 failed poll | Previous data remains valid, `consecutiveFailures = 1` |
| 3 failed polls | `dataStale = true`, values remain visible |
| 10 failed polls (± 100 s) | `inverterOnline = false`, `dataValid = false` |
| Valid poll | Everything recovers, `consecutiveFailures = 0` |

Back-off: 10 s → 20 s → 40 s → 60 s (max). After `inverterOnline = false`, the driver keeps
trying the registration procedure every 60 s. **Never** a reboot, never unbounded logging: a
repeated error is logged once at WARN and then counted at DEBUG.

This is exactly the night scenario: the inverter disappears from the bus, the bridge stays
online, MQTT/REST/Modbus keep responding with `inverter_online: false`, and in the morning the
inverter registers itself again on its own.

### DeviceManager

```cpp
using DeviceId = std::string;

class DeviceManager {
public:
    std::vector<DeviceId> devices() const;
    std::shared_ptr<const DeviceState> state(const DeviceId& id) const;
};
```

The MVP allows a maximum of one active physical device (`MAX_ACTIVE_DEVICES = 1`), but no
interface is a singleton. `DeviceId` for the MVP = `"<driverId>-<serialNumber>"`, e.g.
`eversolar_legacy-XH300060115506193600V610`. REST and MQTT already use that ID in their paths,
so adding more devices later requires no API change.

## Capability model

```cpp
enum class InverterCapability { ReadAcPower, /* ... */ SynchronizeTime };

struct NumericCapability {
    bool   supported;
    bool   writable;
    double minimum, maximum, step;
    Unit   unit;
};

struct InverterCapabilities {
    std::bitset<64> read;
    std::bitset<64> write;
    std::map<InverterCommandType, NumericCapability> numeric;
    uint8_t phaseCount;   // EverSolar: 1
    uint8_t mpptCount;    // EverSolar: 1 or 2, from frame length
    bool    hasBattery;   // EverSolar: false
};
```

EverSolar sets: `ReadAcPower, ReadAcVoltage, ReadAcCurrent, ReadGridFrequency, ReadDcVoltage,
ReadDcCurrent, ReadDcPower(derived), ReadEnergyToday, ReadEnergyTotal, ReadTemperature,
ReadOperatingHours, ReadStatus`. `write` is empty. `ReadErrors` is **not** set.

Capabilities are the only gate to the outputs:

```
capabilities.write.none()  →  no HA control entities
                              no writable Modbus registers (FC6/16 → exception)
                              no REST control endpoints
                              no MQTT command topics
```

That is not an `if (driverId == "eversolar")` check but follows from the bitset. A future
Growatt driver sets write bits and the control entities appear automatically — without the
output modules being touched.

## Command model and dispatcher

Fully implemented in the MVP, including tests; only the EverSolar driver returns `Unsupported`.

```
MQTT / REST / Modbus / HA
          │
          ▼
   CommandDispatcher
     1. global read-only mode?          → Rejected(ReadOnlyMode)
     2. capabilities.write[type]?        → Rejected(NotSupported)
     3. range/step validation            → Rejected(OutOfRange)
     4. rate limit (1/s, burst 3)        → Rejected(RateLimited)
     5. driver->execute(command)
          │
          ▼
       Driver → Unsupported (EverSolar)
```

```cpp
enum class CommandResult { Ok, Unsupported, Rejected, ReadOnlyMode,
                           OutOfRange, RateLimited, DriverError, Timeout };
```

The dispatcher is host-tested in Phase 4: every command type against a read-only mock must
return `ReadOnlyMode`, and against a write-capable mock with an invalid value, `OutOfRange`.
This makes the read-only guarantee a testable contract instead of a promise.

## Discovery model

```cpp
struct ProbeResult {
    bool        responded;
    bool        checksumValid;
    int         confidenceScore;     // 0-100
    std::string detectedManufacturer;
    std::string detectedModel;
    std::string serialNumber;
    std::string firmwareVersion;
    std::vector<std::string> evidence;
};
```

### Safe EverSolar probe

The registration procedure happens to be an excellent probe: it is entirely read-only, and the
only thing it "changes" is a volatile bus address that the inverter forgets on power loss.

| Step | Frame | Evidence on success | Score |
|---|---|---|---|
| 1 | `10 04` re-register (broadcast) | — (no response expected) | 0 |
| 2 | `10 00` offline query | Response with valid checksum | +40 |
| 3 | — | Payload is printable ASCII (serial number) | +25 |
| 4 | `10 01` register address | ACK = exact `0x06` | +20 |
| 5 | `11 03` query inverter ID | ASCII identification string | +10 |
| 6 | `11 02` query normal info | Payload exactly 28 or 32 bytes | +5 |

Maximum 100. A second identical round is mandatory: if the results diverge, the score is
halved.

Automatic selection is only allowed if **all** conditions hold:

- exactly one driver ≥ threshold (default **80**);
- checksum valid;
- two consecutive probes gave the same serial number;
- the second-highest candidate scores ≥ 20 points lower.

Otherwise: show candidates in the web interface, user confirms. In the MVP, de facto only ever
one real driver is registered, so the second condition is trivial — but the logic is tested
with a mock registry containing two competing drivers.

### Forbidden during discovery — enforced

Writing, Modbus FC5/6/15/16, broadcast writes, start/stop, power limits, address changes,
setting time, factory reset, firmware update, brute-forcing.

This is not only documented but enforced: the discovery engine calls **exclusively**
`driver->probe()`, never `execute()`. `probe()` receives a `ProbeContext` with a `readOnly`
transport wrapper that refuses write actions outside the driver's whitelist.

Modes: **Quick** (only drivers with `supportsAutoDetection`, recommended profile, 1 round),
**Extended** (all profiles, multiple rounds — can only be started manually), **Manual**.

There is no scanning across Modbus ID 1..247.

## Mock driver

`mock_inverter` proves the acceptance criterion "the mock driver works without any change to
the outputs".

- Simulates a 3-phase hybrid with 2 MPPTs and a battery — deliberately **different** from
  EverSolar, so it's visible that the outputs assume nothing about EverSolar.
- Generates a day curve (sine based on uptime), so the web interface feels alive.
- Configurable failure modes: `--fail-checksum`, `--timeout`, `--night` for Phase 9.
- Two variants: `mock_readonly` (write bits empty) and `mock_writable` (write bits set) to test
  the dispatcher.
- Runs on `MockTransport`, so also in the `native` host tests.

## Driver-specific settings

A driver must **not** get its own field in `Configuration`. The moment that happens
(`eversolar_layout`), a second driver costs a change in the config struct, the validator, the
serializer, and the web form — exactly the coupling the architecture is meant to prevent.

Instead, the driver declares its own options:

```cpp
struct DriverOption {
    std::string key;            // "layout"
    std::string displayName;
    std::string description;
    std::string defaultValue;
    std::vector<std::string> allowedValues;  // empty = free text
};
// in DriverDescriptor:
std::vector<DriverOption> options;
```

And the configuration carries them along opaquely:

```cpp
struct DriverSettings {
    std::string   id;       // empty = highest probePriority that was compiled in
    bool          autoDetect = false;
    DriverOptions options;  // std::map<std::string, std::string>
};
```

`validateDriverOptions(descriptor, values, error)` checks against what the driver declares.
Unknown keys are **rejected**, not ignored: a silently ignored setting is worse than a refused
one, because the user thinks it was applied.

Added benefit: the options are self-describing, so the web interface can render them
generically. A new driver appears with its settings in the UI without a single line of
frontend work.

Also, `driver.id` has **no** brand name as its default. Empty means "the application chooses":
the registry is already sorted by `probePriority`, so `main.cpp` can choose without knowing
*what* it's choosing.

## Project structure

The structure from the project brief is adopted, with three deviations:

| Deviation | Reason |
|---|---|
| `src/outputs/raw_tcp/` contains **only** `raw_serial_bridge.h` in the MVP (interface, no impl) | Per §31: lock down interfaces and bus locking, defer implementation |
| `docs/decisions.md` added | Framework/library choices belong recorded |
| `LICENSE-THIRD-PARTY.md` added | MIT obligation for eversolar-monitor + LGPL components |

The separation transport / drivers / state / commands / outputs / network / config /
diagnostics remains exactly as requested.

## Security model (summary)

Elaborated in `docs/security.md`. Core:

| Topic | MVP |
|---|---|
| Global read-only mode | **On**, cannot be disabled (no driver can write) |
| Modbus writing | Off; FC6/16 → exception 0x01 |
| Raw TCP bridge | Off (not implemented) |
| REST GET | Unsecured (local network) |
| REST PATCH/POST | HTTP Basic auth required |
| Web configuration | Same auth |
| OTA | Same auth + firmware magic check |
| MQTT | Optional user/pass |
| Secrets | Never in logs/REST/MQTT/web; password fields return `"***"` or are omitted |
| Rate limiting | 1 req/s on `/actions/*`, body limit 4 KB |

Modbus TCP has no encryption, no authentication, and no authorization. This is stated
explicitly in the README and `docs/security.md`: only expose it on a trusted or filtered
network.

## Test strategy

`env:native` runs on the host, without an ESP32. The protocol core (`eversolar_protocol.cpp`,
`eversolar_parser.cpp`), the measurement model, the register map, and the dispatcher have
**no** Arduino include whatsoever — that is a hard design requirement, not a side effect.

| Suite | Covers |
|---|---|
| `test_eversolar_checksum` | Sum checksum, all-zero rejection, overflow |
| `test_eversolar_parser` | 28/32-byte layouts, signed TEMP, uint32 energy.total, scale factors, partial frames, too-short/too-long frames, wrong checksum, wrong address, wrong function number |
| `test_driver_registry` | Registration, compile-time exclusion, unknown ID |
| `test_discovery` | Confidence scoring, tied scores → no auto-selection, inconsistent probes |
| `test_register_map` | float32 word order, NaN sentinels, validity bitmap |
| `test_measurements` | supported/valid/stale matrix, capability filtering |
| `test_commands` | Read-only rejection per command type, range validation, rate limiting |

Fixtures: `test/fixtures/` with frames from the reference, supplemented in Phase 3 with **real
recordings** from the TL3000-20, plus deliberately corrupted frames and a night scenario.

The `MockTransport` plays back recorded frames, so a full poll cycle runs without hardware.

**Honesty note:** the fixtures that can be created now are *constructed* according to the
protocol as derived from the Perl code — they prove that the parser does what we think the
protocol is, not that this is the real protocol. Only Phase 3 (real hardware) can prove that.
That distinction is recorded in `test/fixtures/README.md`.
