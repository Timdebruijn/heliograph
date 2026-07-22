# Modbus TCP-registermap (ontwerp)

Dit is een **virtuele** Modbus TCP-server met een eigen registermap. De EverSolar TL3000-20
spreekt zelf geen Modbus; de bridge decodeert het fabrikant-specifieke protocol en publiceert
het resultaat hier. Er is dus geen enkele relatie met registers "in de omvormer" — deze map is
volledig van dit project.

Schemaversie: **1** (register 0-1). Ophogen bij elke breaking change.

## Conventies

| Onderwerp | Keuze |
|---|---|
| Adressering in broncode | 0-based |
| Documentatie | 0-based raw + 3xxxx/4xxxx-notatie |
| Float | IEEE-754 `float32`, 2 registers |
| Word order | **high word first** (big-endian, ABCD) |
| Byte order | big-endian (Modbus-standaard) |
| Metingen | **input registers** (FC4) |
| Poort | 502 (configureerbaar) |
| Unit-ID inverter | 1 (configureerbaar) |
| Unit-ID diagnostics | 250 (configureerbaar) |

FC3 (Read Holding Registers) spiegelt dezelfde inhoud als FC4, zodat clients die alleen FC3
kunnen (veel PLC's, sommige EVCC-configuraties) ook werken. FC6/FC16 zijn uitgeschakeld en
retourneren exception **0x01 (Illegal Function)**, conform de read-only MVP.

## Ongeldige waarden — geen nul

De opdracht verbiedt geloofwaardige nulwaarden voor onbekende velden. Daarom:

| Type | Waarde bij niet-ondersteund / ongeldig / stale |
|---|---|
| `float32` | **NaN** (`0x7FC00000`) |
| `uint16` / `uint32` / `int32` | **all-ones** (`0xFFFF`, `0xFFFFFFFF`) als sentinel |

Daarnaast is er een **validity bitmap** (600-699). Een client die NaN niet kan verwerken —
en dat zijn er nogal wat, o.a. sommige Node-RED-nodes — leest eerst de bitmap en negeert dan
de betreffende registers. Beide mechanismen zijn altijd consistent.

`0` betekent dus **altijd** een echt gemeten nul (bijvoorbeeld 0 W 's nachts), nooit "onbekend".

## Blokindeling

| Bereik | Inhoud |
|---|---|
| 0-99 | Kern: schema, status, hoofdmetingen |
| 100-199 | AC-fasen |
| 200-299 | DC/MPPT-kanalen |
| 300-399 | Batterij (leeg bij EverSolar) |
| 400-499 | Grid meter (leeg bij EverSolar) |
| 500-599 | Status en errors |
| 600-699 | Capabilities + validity bitmap |
| 700-799 | Identity strings |
| 800-899 | Bridge diagnostics |

## Kernblok (0-99)

| Raw | 3xxxx | Regs | Type | Betekenis | EverSolar |
|---|---|---|---|---|---|
| 0 | 30001 | 2 | uint32 | Registermap-schemaversie | 1 |
| 2 | 30003 | 1 | uint16 | Bridge online (1/0) | ✔ |
| 3 | 30004 | 1 | uint16 | Inverter online (1/0) | ✔ |
| 4 | 30005 | 1 | uint16 | Data geldig (1/0) | ✔ |
| 5 | 30006 | 1 | uint16 | Data stale (1/0) | ✔ |
| 6 | 30007 | 1 | uint16 | Statuscode (ruwe `OP_MODE`) | ✔ |
| 7 | 30008 | 1 | uint16 | Foutcode | ✖ `0xFFFF` |
| 10 | 30011 | 2 | float32 | Totaal AC-vermogen (W) | ✔ `PAC` |
| 12 | 30013 | 2 | float32 | AC-spanning L1 (V) | ✔ `VAC` |
| 14 | 30015 | 2 | float32 | AC-stroom L1 (A) | ✔ `IAC` |
| 16 | 30017 | 2 | float32 | Netfrequentie (Hz) | ✔ `FREQUENCY` |
| 20 | 30021 | 2 | float32 | Totaal DC-vermogen (W) | ✔ *berekend* |
| 22 | 30023 | 2 | float32 | DC-spanning MPPT1 (V) | ✔ `VPV` |
| 24 | 30025 | 2 | float32 | DC-stroom MPPT1 (A) | ✔ `IPV` |
| 30 | 30031 | 2 | float32 | Invertertemperatuur (°C) | ✔ `TEMP` |
| 40 | 30041 | 2 | float32 | Energie vandaag (kWh) | ✔ `E_TODAY` |
| 42 | 30043 | 2 | float32 | Totale energie (kWh) | ✔ `E_TOTAL` uint32 |
| 44 | 30045 | 2 | uint32 | Bedrijfsuren (h) | ✔ `HOURS_UP` |
| 50 | 30051 | 2 | uint32 | Seconden sinds geldige poll | ✔ |
| 52 | 30053 | 2 | uint32 | Geslaagde polls | ✔ |
| 54 | 30055 | 2 | uint32 | Mislukte polls | ✔ |
| 56 | 30057 | 2 | uint32 | Checksumfouten | ✔ |
| 58 | 30059 | 2 | uint32 | RS485-timeouts | ✔ |
| 60 | 30061 | 2 | int32 | Wifi RSSI (dBm) | ✔ |
| 62 | 30063 | 2 | uint32 | Bridge-uptime (s) | ✔ |

Afwijking van het voorstel in de opdracht: **geen**. De map is overgenomen zoals voorgesteld.

Twee opmerkingen:

- **Register 20 (DC-vermogen totaal)** wordt niet door de omvormer geleverd. Het is
  `VPV × IPV` (+ `VPV2 × IPV2` bij 2 strings). Dat is een afgeleide, geen meting. In het
  interne model draagt het een `derived`-vlag; **Modbus kan dat onderscheid niet uitdrukken**
  en publiceert het als een gewone geldige waarde. Wie de afgeleide status nodig heeft, moet
  REST of MQTT gebruiken. Dit register is dus bewust iets minder eerlijk dan de rest van de
  map — het alternatief (weglaten) kost meer dan het oplevert, want vrijwel elke consument
  verwacht DC-vermogen.
- **Register 7 (foutcode)** blijft `0xFFFF`. Het EverSolar-protocol kent geen uitleesbaar
  foutcodeveld (zie `docs/eversolar-protocol.md`). Een nul zou hier "geen fout" suggereren.

## AC-fasen (100-199)

Per fase 20 registers, vanaf 100. De TL3000-20 is 1-fasig; alleen L1 is gevuld.

| Offset in blok | Regs | Type | Betekenis |
|---|---|---|---|
| +0 | 2 | float32 | Spanning (V) |
| +2 | 2 | float32 | Stroom (A) |
| +4 | 2 | float32 | Vermogen (W) |

| Fase | Basis | EverSolar |
|---|---|---|
| L1 | 100 | spanning+stroom ✔, vermogen NaN |
| L2 | 120 | alles NaN |
| L3 | 140 | alles NaN |

## DC/MPPT (200-299)

Per MPPT 20 registers, vanaf 200.

| Offset | Regs | Type | Betekenis |
|---|---|---|---|
| +0 | 2 | float32 | Spanning (V) |
| +2 | 2 | float32 | Stroom (A) |
| +4 | 2 | float32 | Vermogen (W, afgeleid) |

| MPPT | Basis | EverSolar |
|---|---|---|
| 1 | 200 | ✔ |
| 2 | 220 | ✔ *alleen bij 2-string-layout*, anders NaN |

## Batterij (300-399) en grid meter (400-499)

Volledig gereserveerd. Bij EverSolar alles NaN / bitmap-bit 0. Ingevuld zodra een driver met
batterij- of meterondersteuning wordt toegevoegd — zonder wijziging aan de map.

## Status en errors (500-599)

| Raw | Regs | Type | Betekenis |
|---|---|---|---|
| 500 | 1 | uint16 | Statuscode (ruw, = reg 6) |
| 501 | 1 | uint16 | Foutcode (= reg 7) |
| 502 | 1 | uint16 | Opeenvolgende mislukte polls |
| 510 | 16 | string | `status_text`, ASCII, null-gepadded (32 tekens) |

## Capabilities + validity bitmap (600-699)

| Raw | Regs | Type | Betekenis |
|---|---|---|---|
| 600 | 4 | bitmap64 | `InverterCapability` read-bits |
| 604 | 4 | bitmap64 | `InverterCapability` write-bits (MVP: alles 0) |
| 610 | 8 | bitmap128 | **Validity bitmap** van de kernmetingen |
| 620 | 1 | uint16 | Aantal AC-fasen (EverSolar: 1) |
| 621 | 1 | uint16 | Aantal MPPT's (EverSolar: 1 of 2, uit framelengte) |
| 622 | 1 | uint16 | Batterij aanwezig (0/1) |
| 623 | 1 | uint16 | Driver read-only (MVP: 1) |

De bit-indices staan in de tabel verderop en zijn vastgelegd in `ValidityBit` in
`src/outputs/modbus_tcp/register_map.h`. Die volgorde is onderdeel van de schemaversie en mag
binnen versie 1 niet wijzigen.

Register 623 = 1 vertelt een client op voorhand dat schrijven zinloos is. Dat is nuttiger dan
alleen een exception op FC6.

## Identity strings (700-799)

ASCII, null-gepadded, 2 tekens per register, big-endian.

| Raw | Regs | Tekens | Veld |
|---|---|---|---|
| 700 | 16 | 32 | Manufacturer |
| 716 | 16 | 32 | Model |
| 732 | 16 | 32 | Serienummer |
| 748 | 8 | 16 | Firmwareversie inverter |
| 756 | 8 | 16 | Driver-ID |
| 764 | 8 | 16 | Bridge-firmwareversie |

Onbekende strings zijn volledig `0x00` — een lege string is hier ondubbelzinnig "onbekend",
er is geen verwarring met een geldige waarde mogelijk.

## Bridge diagnostics (800-899, ook op Unit-ID 250)

| Raw | Regs | Type | Betekenis |
|---|---|---|---|
| 800 | 2 | uint32 | Uptime (s) |
| 802 | 2 | uint32 | Free heap (bytes) |
| 804 | 2 | uint32 | Minimum free heap (bytes) |
| 806 | 1 | uint16 | Reset reason (`esp_reset_reason_t`) |
| 807 | 1 | uint16 | Wifi RSSI (int16) |
| 810 | 2 | uint32 | Wifi reconnects |
| 812 | 2 | uint32 | MQTT reconnects |
| 814 | 2 | uint32 | Modbus-clientverbindingen |
| 816 | 2 | uint32 | REST-requests |
| 818 | 2 | uint32 | Ongeldige frames |
| 820 | 3 | uint16[3] | Firmwareversie major/minor/patch |

## Voorbeelden

### mbpoll

```bash
# AC-vermogen (float32, high word first) — raw 10, mbpoll is 1-based
mbpoll -m tcp -a 1 -t 4 -r 11 -c 2 -0 heliograph.local
# Alle kernregisters
mbpoll -m tcp -a 1 -t 4 -r 1 -c 64 -0 heliograph.local
```

Een volledige, werkende client staat in [`tools/read_modbus.py`](../tools/read_modbus.py) —
die controleert de schemaversie, leest de validity bitmap en behandelt NaN correct.

### Python (pymodbus 3.x)

**Let op de API-wijzigingen.** Dit voorbeeld is geverifieerd tegen **pymodbus 3.14** (2026-07):

- `slave=` heet nu `device_id=`;
- `pymodbus.payload` (`BinaryPayloadDecoder`) **bestaat niet meer** — gebruik
  `client.convert_from_registers()`.

Oudere voorbeelden op internet gebruiken nog de verwijderde API.

```python
from pymodbus.client import ModbusTcpClient
from pymodbus.client.mixin import ModbusClientMixin

c = ModbusTcpClient("heliograph.local", port=502)
c.connect()

rr = c.read_input_registers(address=10, count=2, device_id=1)
watts = c.convert_from_registers(
    rr.registers, ModbusClientMixin.DATATYPE.FLOAT32, word_order="big"
)
# NaN betekent onbekend: niet ondersteund door deze driver, of stale. Nooit 0.
print("onbekend" if watts != watts else f"{watts:.1f} W")
c.close()
```

Wie versie-onafhankelijk wil zijn, decodeert zelf — twee registers, high word first:

```python
import struct
watts = struct.unpack(">f", struct.pack(">HH", *rr.registers))[0]
```

### Home Assistant (`configuration.yaml`)

```yaml
modbus:
  - name: heliograph
    type: tcp
    host: heliograph.local
    port: 502
    sensors:
      - name: Solar AC Power
        slave: 1
        address: 10
        input_type: input
        data_type: float32
        swap: false          # high word first
        unit_of_measurement: W
        device_class: power
        state_class: measurement
      - name: Solar Energy Total
        slave: 1
        address: 42
        input_type: input
        data_type: float32
        swap: false
        unit_of_measurement: kWh
        device_class: energy
        state_class: total_increasing
```

MQTT-discovery is voor Home Assistant de eenvoudiger route; deze Modbus-config is bedoeld voor
wie Modbus al gebruikt.

### EVCC (custom meter)

Er bestaat **geen** EVCC-template voor deze zelfgemaakte map — die moet handmatig:

```yaml
meters:
  - name: pv1
    type: custom
    power:
      source: modbus
      uri: heliograph.local:502
      id: 1
      register:
        address: 10
        type: input
        decode: float32
```

### Node-RED

Gebruik `modbus-read`: FC `InputRegister`, address `10`, quantity `2`, unit-id `1`. Zet daarna
een `function`-node die twee words naar float32 omzet (high word first) **en NaN afvangt** —
`Buffer.from` + `readFloatBE`, dan `if (Number.isNaN(v)) return null;`.

## Validity bitmap — bit-indices (schema v1)

Vastgelegd in `ValidityBit` in `src/outputs/modbus_tcp/register_map.h`. Binnen schemaversie 1
mogen deze **niet** wijzigen; alleen achteraan aanvullen.

| Bit | Meting | Bit | Meting |
|---|---|---|---|
| 0 | `ac.power.total` | 15 | `ac.phase_l2.current` |
| 1 | `ac.phase_l1.voltage` | 16 | `ac.phase_l2.power` |
| 2 | `ac.phase_l1.current` | 17 | `ac.phase_l3.voltage` |
| 3 | `ac.frequency` | 18 | `ac.phase_l3.current` |
| 4 | `dc.power.total` | 19 | `ac.phase_l3.power` |
| 5 | `dc.mppt_1.voltage` | 20 | `dc.mppt_1.power` |
| 6 | `dc.mppt_1.current` | 21 | `dc.mppt_2.voltage` |
| 7 | `inverter.temperature` | 22 | `dc.mppt_2.current` |
| 8 | `energy.today` | 23 | `dc.mppt_2.power` |
| 9 | `energy.total` | 24 | `battery.soc` |
| 10 | `inverter.operating_hours` | 25 | `battery.voltage` |
| 11 | statuscode | 26 | `battery.charge_power` |
| 12 | foutcode | 27 | `battery.discharge_power` |
| 13 | `ac.phase_l1.power` | 28 | `grid.import_power` |
| 14 | `ac.phase_l2.voltage` | 29 | `grid.export_power` |

Bit `n` staat in register `610 + n/16`, op bitpositie `n % 16`.

**Garantie:** de bitmap en de NaN-sentinel spreken elkaar nooit tegen. Een bit is 1 dan en
slechts dan als het bijbehorende float-register geen NaN is. Dit wordt afgedwongen door
`test_validity_bitmap_and_nan_always_agree`.

## Stale data telt niet als meting

Wanneer `data_stale = 1` publiceert de bridge de metingen als **NaN**, niet als de laatst
bekende waarde. De laatste waarde blijft wel in het interne model en is via REST/MQTT met een
`stale`-vlag zichtbaar — maar Modbus kent geen manier om "dit getal is oud" mee te geven, dus
zou een consument het als actueel behandelen. Onbekend is dan het eerlijkere antwoord.

## Openstaand

- Of `energy.today` als `total_increasing` bruikbaar blijft rond middernacht (reset naar 0)
  hangt af van het gedrag van de omvormer — vast te stellen in Fase 9.
- De eModbus-serverbedrading (`modbus_tcp_server.cpp`) is **nog niet gecompileerd**; alleen de
  registermap is getest.
