# Architectuur

## Gelaagdheid

```
┌──────────────────────────────────────────────────────────────┐
│ Physical            RS485 (MVP) │ RS232 │ CAN │ TCP (later)  │
└───────────────────────────────┬──────────────────────────────┘
                                │
┌───────────────────────────────▼──────────────────────────────┐
│ Transport      Transport (abstract)                          │
│                Rs485Transport   MockTransport                │
│                UART-config, framing-hulp, timeouts, buslock  │
└───────────────────────────────┬──────────────────────────────┘
                                │  bytes in/uit — géén protocolkennis
┌───────────────────────────────▼──────────────────────────────┐
│ Driver         InverterDriver (abstract)                     │
│                EversolarDriver  MockDriver                   │
│                [later] GrowattDriver, SolaxDriver, ...       │
└───────────────────────────────┬──────────────────────────────┘
                                │  schrijft alleen ná volledig geldige poll
┌───────────────────────────────▼──────────────────────────────┐
│ State          DeviceContext → DeviceState (immutable snap)  │
│                StateStore (thread-safe), DeviceManager       │
└───────────────────────────────┬──────────────────────────────┘
                                │  snapshots lezen — read-only
┌───────────────────────────────▼──────────────────────────────┐
│ Outputs        MQTT │ Modbus TCP │ REST │ Web │ Prometheus   │
└──────────────────────────────────────────────────────────────┘
```

De harde regel: **merkspecifieke kennis bestaat uitsluitend in `src/drivers/<driver>/`.**
Geen enkele outputmodule kent het woord "EverSolar", op één plek na: de string in
`DeviceIdentity::manufacturer`, die uit de driver komt en als data wordt doorgegeven.

Dit is mechanisch afdwingbaar en wordt gecontroleerd door `tools/check_layering.sh`:

```
1. Merknamen komen niet voor buiten src/drivers/  — ook niet in commentaar
2. De host-testbare kern includeert geen Arduino/ESP-IDF headers
3. De fixtures zijn in sync met hun generator
```

**Deze check moet volledig gedraaid worden.** Zijn laatste regel is `RESULT: PASS` of
`RESULT: FAIL`. Eerdere versies eindigden met de "OK" van de láátste deelcheck, waardoor
`check_layering.sh | tail -1` succes rapporteerde terwijl een eerdere check faalde — dat is
precies wat er tijdens Fase 7 gebeurde, en het verborg een echte overtreding een fase lang.

Regel 1 geldt bewust ook voor commentaar. Zodra het canonieke model zichzelf gaat uitleggen
in termen van één driver ("de EverSolar heeft dit veld niet"), verwatert de regel tot "ach,
het is maar een comment" en is hij niet meer te handhaven. `errorCodeSupported` documenteert
daarom *waarom* de vlag bestaat, niet welke omvormer hem nodig had.

## Datastroom en taakmodel

```
        ┌──────────────┐   uart1 (excl.)   ┌──────────────┐
        │ rs485Task    │ ◄───── mutex ────►│  RS485 bus   │
        │ core 1 pr 5  │                   └──────────────┘
        │ 8192 B       │
        └──────┬───────┘
               │ publish(snapshot)   ← enige schrijver
        ┌──────▼───────┐
        │  StateStore  │  shared_ptr<const DeviceState>, mutex
        └──────┬───────┘
               │ snapshot()          ← alleen lezers
     ┌─────────┼─────────┬─────────────┬────────────┐
     ▼         ▼         ▼             ▼            ▼
  mqttTask  AsyncTCP  AsyncTCP     loopTask     eModbus
  core 0    (REST)    (web/SSE)    core 1       (Modbus TCP)
  pr 3      pr 3      pr 3         pr 1         pr 3
```

### Taken

| Taak | Core | Prio | Stack | Verantwoordelijk voor | WDT |
|---|---|---|---|---|---|
| `rs485Task` | 1 | 5 | 8192 | UART1, actieve driver, pollcyclus, discovery. 8192 sinds de stack-canary-crash van 2026-07-19 (TRACE-pad door newlib vsnprintf); zie main.cpp | ✔ |
| `mqttTask` | 0 | 3 | 4096 | espMqttClient, publiceren, HA-discovery | ✔ |
| `loopTask` (Arduino) | 1 | 1 | 8192 | Wifi-supervisie, mDNS, diagnostics, relais-safety | ✔ |
| `async_tcp` (library) | 0 | 3 | 8192 | REST, web, SSE — AsyncTCP-beheerd | ✖ |
| `eModbus server` (library) | 0 | 3 | 4096 | Modbus TCP-clients | ✖ |

Drie eigen taken, twee library-taken. Bewust minimaal.

Onderbouwing van de core-verdeling: wifi/lwIP draait standaard op core 0. Door `rs485Task` op
core 1 te pinnen kan netwerkdrukte de RS485-timing niet verstoren — precies het
acceptatiecriterium "Modbus-clientfouten onderbreken inverterpolling niet". Omgekeerd zit
`mqttTask` bij het netwerk op core 0.

Watchdog: `esp_task_wdt` met 30 s timeout; `rs485Task`, `mqttTask` en `loopTask` melden zich
aan. `rs485Task` doet een `esp_task_wdt_reset()` per cyclus, óók wanneer de poll faalt — een
onbereikbare omvormer mag nooit een reset veroorzaken.

### Gedeelde resources en locking

| Resource | Eigenaar | Bescherming |
|---|---|---|
| UART1 | `rs485Task` | `SemaphoreHandle_t busMutex` in `Rs485Transport` |
| `DeviceState` | `StateStore` | mutex + `shared_ptr<const>`-snapshot |
| NVS-config | `ConfigurationStore` | mutex; schrijven alleen vanuit `loopTask` |
| Diagnostics-tellers | `Diagnostics` | `std::atomic<uint32_t>` |

Een snapshot is immutable: lezers krijgen een `shared_ptr<const DeviceState>` en houden die zo
lang ze willen vast zonder de mutex. De schrijver bouwt een nieuwe `DeviceState` en wisselt de
pointer om onder mutex. Geen kopieerkosten per lezer, geen tearing.

**Outputmodules hebben geen enkel pad naar de UART.** Ze zien alleen `StateStore`.

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
    // Leest tot `len` bytes of tot timeout. Frame-detectie is aan de driver.
    virtual size_t   read(uint8_t* buf, size_t len, uint32_t timeoutMs) = 0;
    virtual bool     lock(uint32_t timeoutMs) = 0;
    virtual void     unlock() = 0;
    virtual const TransportStats& stats() const = 0;
};
```

De transportlaag kent **geen** framing van EverSolar. Ze levert bytes en timeouts. De driver
bepaalt wanneer een frame compleet is (voor EverSolar: header lezen → lengtebyte → rest lezen).

`Rs485Transport::configure()` doet precies wat de Waveshare-demo bewijst:

```cpp
uart_.begin(profile.baudRate, toArduinoConfig(profile), board::kRs485Rx, board::kRs485Tx);
uart_.setPins(-1, -1, -1, board::kRs485De);         // GPIO21 als RTS
uart_.setMode(UART_MODE_RS485_HALF_DUPLEX);         // UART stuurt DE/RE zelf
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

Conform de opdracht. `EversolarDriver::execute()` geeft **altijd**
`CommandResult::Unsupported` — niet als tijdelijke MVP-beperking maar omdat control code
`0x12` WRITE en `0x13` EXECUTE in de referentie leeg zijn: er is geen enkele bekende
schrijfoperatie voor dit protocol.

```cpp
enum class PollResult { Ok, Timeout, ChecksumError, InvalidFrame, NotRegistered, TransportError };
```

`poll()` mag `state` **alleen** aanraken bij `Ok`. Bij elke andere uitkomst blijft de vorige
state ongewijzigd — zo ontstaat nooit een half ingevulde meting.

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
  .recommendedSerialProfiles = { { 9600, SerialParity::None, 8, 1, 1000, 3 } },  // enige bekende
  .supportLevel = DriverSupportLevel::Experimental,   // → Beta na Fase 3, Stable na Fase 9
  .probePriority = 10,
  .supportsAutoDetection = true,
  .supportsMultipleDevices = true,    // protocol ondersteunt het; MVP staat 1 toe
  .supportsRead = true,
  .supportsWrite = false,             // 0x12/0x13 leeg in het protocol
}
```

Slechts **één** serieel profiel: 9600 8N1 is in de referentie hardcoded. We proberen geen
andere combinaties — dat zou blind brute-forcen zijn.

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

Registratie is compile-time-conditioneel:

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

Een nieuwe driver toevoegen = een map, een `descriptor.cpp`, een `#if`-blok, fixtures + tests.
**Nul wijzigingen** in MQTT, REST, Modbus of web.

## Canoniek datamodel

### Measurement

```cpp
struct Measurement {
    std::string     id;            // "ac.power.total"
    std::string     displayName;
    MeasurementType type;
    Unit            unit;
    double          value;
    bool            supported;     // driver kan dit veld überhaupt
    bool            valid;         // laatste poll leverde een geldige waarde
    bool            stale;         // waarde is te oud
    bool            derived;       // berekend, niet gemeten (bv. dc.power.total)
    uint64_t        timestampMs;
};
```

`derived` is een toevoeging op het model uit de opdracht. Reden: `dc.power.total` is bij
EverSolar `VPV × IPV` en geen meting. Zonder die vlag zou een consument een precisie
suggereren die er niet is.

De drie vlaggen zijn strikt onderscheiden:

| `supported` | `valid` | `stale` | Betekenis |
|---|---|---|---|
| false | — | — | Driver levert dit veld nooit → **niet publiceren** |
| true | false | — | Ondersteund, maar nog nooit/niet geldig gelezen → NaN/null |
| true | true | true | Waarde bekend maar verouderd → publiceren mét stale-vlag |
| true | true | false | Actuele meting |

### Meet-ID's die de EverSolar-driver vult

| ID | Bron |
|---|---|
| `ac.power.total` | `PAC` |
| `ac.phase_l1.voltage` | `VAC` |
| `ac.phase_l1.current` | `IAC` |
| `ac.frequency` | `FREQUENCY` |
| `dc.mppt_1.voltage` | `VPV` |
| `dc.mppt_1.current` | `IPV` |
| `dc.mppt_1.power` | *derived* |
| `dc.mppt_2.*` | alleen bij 32-byte payload |
| `dc.power.total` | *derived* |
| `energy.today` | `E_TODAY` ÷100 |
| `energy.total` | uint32(`HI`,`LO`) ÷10 |
| `inverter.temperature` | `TEMP`, **signed** |
| `inverter.operating_hours` | `HOURS_UP` |

Niet gevuld (`supported = false`): alle `ac.phase_l2/l3.*`, `battery.*`, `grid.*`,
`ac.phase_l1.power`. Deze verschijnen **niet** in MQTT, niet in HA en zijn NaN in Modbus.

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

### Stale- en offline-logica

Conform de opdracht:

| Gebeurtenis | Effect |
|---|---|
| 1 mislukte poll | Vorige data blijft geldig, `consecutiveFailures = 1` |
| 3 mislukte polls | `dataStale = true`, waarden blijven zichtbaar |
| 10 mislukte polls (± 100 s) | `inverterOnline = false`, `dataValid = false` |
| Geldige poll | Alles herstelt, `consecutiveFailures = 0` |

Back-off: 10 s → 20 s → 40 s → 60 s (max). Na `inverterOnline = false` blijft de driver elke
60 s de registratieprocedure proberen. **Nooit** een reboot, nooit onbeperkte logging: een
herhaalde fout wordt één keer op WARN gelogd en daarna op DEBUG geteld.

Dit is precies het nachtscenario: de omvormer verdwijnt van de bus, de bridge blijft online,
MQTT/REST/Modbus blijven antwoorden met `inverter_online: false`, en 's ochtends registreert de
omvormer zich vanzelf weer.

### DeviceManager

```cpp
using DeviceId = std::string;

class DeviceManager {
public:
    std::vector<DeviceId> devices() const;
    std::shared_ptr<const DeviceState> state(const DeviceId& id) const;
};
```

De MVP staat maximaal één actief fysiek device toe (`MAX_ACTIVE_DEVICES = 1`), maar geen enkele
interface is singleton. `DeviceId` voor de MVP = `"<driverId>-<serienummer>"`, bijv.
`eversolar_legacy-XH300060115506193600V610`. REST en MQTT gebruiken die ID al in hun paden, dus
meerdere devices later vergt geen API-wijziging.

## Capability-model

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
    uint8_t mpptCount;    // EverSolar: 1 of 2, uit framelengte
    bool    hasBattery;   // EverSolar: false
};
```

EverSolar zet: `ReadAcPower, ReadAcVoltage, ReadAcCurrent, ReadGridFrequency, ReadDcVoltage,
ReadDcCurrent, ReadDcPower(derived), ReadEnergyToday, ReadEnergyTotal, ReadTemperature,
ReadOperatingHours, ReadStatus`. `write` is leeg. `ReadErrors` staat **niet** aan.

Capabilities zijn de enige poort naar de outputs:

```
capabilities.write.none()  →  geen HA-bedieningsentities
                              geen schrijfbare Modbus-registers (FC6/16 → exception)
                              geen REST control-endpoints
                              geen MQTT command-topics
```

Dat is geen `if (driverId == "eversolar")`-check maar volgt uit de bitset. Een toekomstige
Growatt-driver zet write-bits en de bedieningsentities verschijnen vanzelf — zonder dat de
outputmodules zijn aangeraakt.

## Commandmodel en dispatcher

Volledig geïmplementeerd in de MVP, inclusief tests; alleen levert de EverSolar-driver
`Unsupported`.

```
MQTT / REST / Modbus / HA
          │
          ▼
   CommandDispatcher
     1. globale read-only-modus?        → Rejected(ReadOnlyMode)
     2. capabilities.write[type]?       → Rejected(NotSupported)
     3. bereik/step-validatie           → Rejected(OutOfRange)
     4. rate limit (1/s, burst 3)       → Rejected(RateLimited)
     5. driver->execute(command)
          │
          ▼
       Driver → Unsupported (EverSolar)
```

```cpp
enum class CommandResult { Ok, Unsupported, Rejected, ReadOnlyMode,
                           OutOfRange, RateLimited, DriverError, Timeout };
```

De dispatcher wordt in Fase 4 host-getest: elk commandotype tegen een read-only mock moet
`ReadOnlyMode` geven, en tegen een write-capable mock met foute waarde `OutOfRange`. Zo is de
read-only-garantie een testbaar contract in plaats van een belofte.

## Discovery-model

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

### Veilige EverSolar-probe

De registratieprocedure is toevallig een uitstekende probe: hij is volledig read-only, en het
enige dat hij "wijzigt" is een vluchtig busadres dat de omvormer bij spanningsverlies vergeet.

| Stap | Frame | Bewijs bij succes | Score |
|---|---|---|---|
| 1 | `10 04` re-register (broadcast) | — (geen response verwacht) | 0 |
| 2 | `10 00` offline query | Respons met geldige checksum | +40 |
| 3 | — | Payload is printbare ASCII (serienummer) | +25 |
| 4 | `10 01` register address | ACK = exact `0x06` | +20 |
| 5 | `11 03` query inverter ID | ASCII-identificatiestring | +10 |
| 6 | `11 02` query normal info | Payload exact 28 of 32 bytes | +5 |

Maximaal 100. Een tweede identieke ronde is verplicht: wijken de resultaten af, dan wordt de
score gehalveerd.

Automatisch selecteren mag alleen als **alle** voorwaarden gelden:

- precies één driver ≥ drempel (standaard **80**);
- checksum geldig;
- twee opeenvolgende probes gaven hetzelfde serienummer;
- de op één na hoogste kandidaat scoort ≥ 20 punten lager.

Anders: kandidaten tonen in de webinterface, gebruiker bevestigt. In de MVP is de facto altijd
maar één echte driver geregistreerd, dus de tweede voorwaarde is triviaal — maar de logica
wordt met een mock-registry met twee concurrerende drivers wél getest.

### Verboden tijdens discovery — afgedwongen

Schrijven, Modbus FC5/6/15/16, broadcast-writes, start/stop, vermogenslimieten, adreswijziging,
tijd zetten, factory reset, firmware-update, brute-force.

Dit wordt niet alleen gedocumenteerd maar afgedwongen: de discovery-engine roept **uitsluitend**
`driver->probe()` aan, nooit `execute()`. `probe()` krijgt een `ProbeContext` met een
`readOnly`-transportwrapper die schrijfacties buiten de whitelist van de driver weigert.

Modi: **Quick** (alleen drivers met `supportsAutoDetection`, aanbevolen profiel, 1 ronde),
**Extended** (alle profielen, meerdere rondes — alleen handmatig te starten), **Manual**.

Er wordt niet over Modbus-ID 1..247 gescand.

## Mockdriver

`mock_inverter` bewijst het acceptatiecriterium "de mockdriver werkt zonder wijziging van
outputs".

- Simuleert een 3-fasige hybride met 2 MPPT's en batterij — bewust **anders** dan EverSolar,
  zodat zichtbaar wordt dat de outputs niets over EverSolar aannemen.
- Genereert een dagcurve (sinus op basis van uptime), zodat de webinterface leeft.
- Configureerbare faalmodi: `--fail-checksum`, `--timeout`, `--night` voor Fase 9.
- Twee varianten: `mock_readonly` (write-bits leeg) en `mock_writable` (write-bits gezet) om de
  dispatcher te testen.
- Draait op `MockTransport`, dus ook in de `native` host-tests.

## Driverspecifieke instellingen

Een driver mag géén eigen veld in `Configuration` krijgen. Zodra dat gebeurt (`eversolar_layout`)
kost een tweede driver een wijziging in de config-struct, de validator, de serializer én het
webformulier — precies de koppeling die de architectuur moet voorkomen.

In plaats daarvan declareert de driver zijn opties zelf:

```cpp
struct DriverOption {
    std::string key;            // "layout"
    std::string displayName;
    std::string description;
    std::string defaultValue;
    std::vector<std::string> allowedValues;  // leeg = vrije tekst
};
// in DriverDescriptor:
std::vector<DriverOption> options;
```

En de configuratie draagt ze ondoorzichtig mee:

```cpp
struct DriverSettings {
    std::string   id;       // leeg = hoogste probePriority die is meegecompileerd
    bool          autoDetect = false;
    DriverOptions options;  // std::map<std::string, std::string>
};
```

`validateDriverOptions(descriptor, values, error)` toetst tegen wat de driver declareert.
Onbekende sleutels worden **afgewezen**, niet genegeerd: een stilzwijgend genegeerde instelling
is erger dan een geweigerde, want de gebruiker denkt dat hij is toegepast.

Bijkomend voordeel: de opties zijn zelfbeschrijvend, dus de webinterface kan ze generiek
renderen. Een nieuwe driver verschijnt met zijn instellingen in de UI zonder één regel
frontend-werk.

Ook `driver.id` heeft géén merknaam als default. Leeg betekent "de applicatie kiest": de
registry is al gesorteerd op `probePriority`, dus `main.cpp` kan kiezen zonder te weten wát
het kiest.

## Projectstructuur

De structuur uit de opdracht wordt overgenomen, met drie afwijkingen:

| Afwijking | Reden |
|---|---|
| `src/outputs/raw_tcp/` bevat in de MVP **alleen** `raw_serial_bridge.h` (interface, geen impl) | Conform §31: interfaces en buslocking vastleggen, implementatie uitstellen |
| `docs/decisions.md` toegevoegd | Framework-/librarykeuze hoort vastgelegd |
| `LICENSE-THIRD-PARTY.md` toegevoegd | MIT-verplichting eversolar-monitor + LGPL-componenten |

De scheiding transport / drivers / state / commands / outputs / network / config / diagnostics
blijft exact zoals gevraagd.

## Security-model (samenvatting)

Uitgewerkt in `docs/security.md`. Kern:

| Onderwerp | MVP |
|---|---|
| Globale read-only-modus | **Aan**, niet uitschakelbaar (geen driver kan schrijven) |
| Modbus schrijven | Uit; FC6/16 → exception 0x01 |
| Raw TCP-bridge | Uit (niet geïmplementeerd) |
| REST GET | Onbeveiligd (lokaal netwerk) |
| REST PATCH/POST | HTTP Basic-auth verplicht |
| Webconfiguratie | Zelfde auth |
| OTA | Zelfde auth + firmware-magic-check |
| MQTT | Optionele user/pass |
| Secrets | Nooit in logs/REST/MQTT/web; wachtwoordvelden retourneren `"***"` of worden weggelaten |
| Rate limiting | 1 req/s op `/actions/*`, body-limiet 4 KB |

Modbus TCP heeft geen encryptie, geen authenticatie en geen autorisatie. Dat wordt in README en
`docs/security.md` expliciet vermeld: alleen op een vertrouwd of gefilterd netwerk aanbieden.

## Teststrategie

`env:native` draait op de host, zonder ESP32. De protocolkern (`eversolar_protocol.cpp`,
`eversolar_parser.cpp`), het meetmodel, de registermap en de dispatcher hebben **geen enkele**
Arduino-include — dat is een harde ontwerpeis, geen bijvangst.

| Suite | Dekt |
|---|---|
| `test_eversolar_checksum` | Som-checksum, all-zero-verwerping, overflow |
| `test_eversolar_parser` | 28/32-byte-layouts, signed TEMP, uint32 energy.total, schaalfactoren, gedeeltelijke frames, te korte/lange frames, foute checksum, fout adres, fout functienummer |
| `test_driver_registry` | Registratie, compile-time-uitsluiting, onbekende ID |
| `test_discovery` | Confidence-scoring, gelijke scores → geen autoselectie, inconsistente probes |
| `test_register_map` | float32-word-order, NaN-sentinels, validity bitmap |
| `test_measurements` | supported/valid/stale-matrix, capability-filtering |
| `test_commands` | Read-only-afwijzing per commandotype, bereikvalidatie, rate limiting |

Fixtures: `test/fixtures/` met frames uit de referentie, in Fase 3 aangevuld met **echte
opnames** van de TL3000-20, plus bewust beschadigde frames en een nachtscenario.

De `MockTransport` speelt opgenomen frames af, zodat een volledige pollcyclus zonder hardware
draait.

**Eerlijkheidsnotitie:** de fixtures die nu gemaakt kunnen worden zijn *geconstrueerd* volgens
het protocol zoals uit de Perl-code afgeleid — ze bewijzen dat de parser doet wat wij denken
dat het protocol is, niet dat dat het echte protocol is. Alleen Fase 3 (echte hardware) kan dat
bewijzen. Dat onderscheid wordt in `test/fixtures/README.md` vastgelegd.
