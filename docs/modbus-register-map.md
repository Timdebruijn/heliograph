# Modbus TCP register map (design)

This is a **virtual** Modbus TCP server with its own register map. The EverSolar TL3000-20
does not speak Modbus itself; the bridge decodes the manufacturer-specific protocol and publishes
the result here. So there is no relation whatsoever to registers "in the inverter" — this map is
entirely specific to this project.

Schema version: **1** (register 0-1). Increment on every breaking change.

## Conventions

| Topic | Choice |
|---|---|
| Addressing in source code | 0-based |
| Documentation | 0-based raw + 3xxxx/4xxxx notation |
| Float | IEEE-754 `float32`, 2 registers |
| Word order | **high word first** (big-endian, ABCD) |
| Byte order | big-endian (Modbus standard) |
| Measurements | **input registers** (FC4) |
| Port | 502 (configurable) |
| Unit ID inverter | 1 (configurable) |
| Unit ID diagnostics | 250 (configurable) |

FC3 (Read Holding Registers) mirrors the same content as FC4, so clients that can only do FC3
(many PLCs, some EVCC configurations) also work. FC6/FC16 are disabled and
return exception **0x01 (Illegal Function)**, in line with the read-only MVP.

## Invalid values — never zero

The project brief prohibits plausible-looking zero values for unknown fields. Therefore:

| Type | Value for unsupported / invalid / stale |
|---|---|
| `float32` | **NaN** (`0x7FC00000`) |
| `uint16` / `uint32` / `int32` | **all-ones** (`0xFFFF`, `0xFFFFFFFF`) as sentinel |

In addition there is a **validity bitmap** (600-699). A client that cannot handle NaN —
and there are quite a few, e.g. some Node-RED nodes — reads the bitmap first and then ignores
the registers in question. Both mechanisms are always consistent.

`0` therefore **always** means a genuinely measured zero (for example 0 W at night), never "unknown".

## Block layout

| Range | Content |
|---|---|
| 0-99 | Core: schema, status, main measurements |
| 100-199 | AC phases |
| 200-299 | DC/MPPT channels |
| 300-399 | Battery (empty for EverSolar) |
| 400-499 | Grid meter (empty for EverSolar) |
| 500-599 | Status and errors |
| 600-699 | Capabilities + validity bitmap |
| 700-799 | Identity strings |
| 800-899 | Bridge diagnostics |

## Core block (0-99)

| Raw | 3xxxx | Regs | Type | Meaning | EverSolar |
|---|---|---|---|---|---|
| 0 | 30001 | 2 | uint32 | Register map schema version | 1 |
| 2 | 30003 | 1 | uint16 | Bridge online (1/0) | ✔ |
| 3 | 30004 | 1 | uint16 | Inverter online (1/0) | ✔ |
| 4 | 30005 | 1 | uint16 | Data valid (1/0) | ✔ |
| 5 | 30006 | 1 | uint16 | Data stale (1/0) | ✔ |
| 6 | 30007 | 1 | uint16 | Status code (raw `OP_MODE`) | ✔ |
| 7 | 30008 | 1 | uint16 | Error code | ✖ `0xFFFF` |
| 10 | 30011 | 2 | float32 | Total AC power (W) | ✔ `PAC` |
| 12 | 30013 | 2 | float32 | AC voltage L1 (V) | ✔ `VAC` |
| 14 | 30015 | 2 | float32 | AC current L1 (A) | ✔ `IAC` |
| 16 | 30017 | 2 | float32 | Grid frequency (Hz) | ✔ `FREQUENCY` |
| 20 | 30021 | 2 | float32 | Total DC power (W) | ✔ *calculated* |
| 22 | 30023 | 2 | float32 | DC voltage MPPT1 (V) | ✔ `VPV` |
| 24 | 30025 | 2 | float32 | DC current MPPT1 (A) | ✔ `IPV` |
| 30 | 30031 | 2 | float32 | Inverter temperature (°C) | ✔ `TEMP` |
| 40 | 30041 | 2 | float32 | Energy today (kWh) | ✔ `E_TODAY` |
| 42 | 30043 | 2 | float32 | Total energy (kWh) | ✔ `E_TOTAL` uint32 |
| 44 | 30045 | 2 | uint32 | Operating hours (h) | ✔ `HOURS_UP` |
| 50 | 30051 | 2 | uint32 | Seconds since last valid poll | ✔ |
| 52 | 30053 | 2 | uint32 | Successful polls | ✔ |
| 54 | 30055 | 2 | uint32 | Failed polls | ✔ |
| 56 | 30057 | 2 | uint32 | Checksum errors | ✔ |
| 58 | 30059 | 2 | uint32 | RS485 timeouts | ✔ |
| 60 | 30061 | 2 | int32 | Wifi RSSI (dBm) | ✔ |
| 62 | 30063 | 2 | uint32 | Bridge uptime (s) | ✔ |

Deviation from the proposal in the project brief: **none**. The map was adopted as proposed.

Two remarks:

- **Register 20 (total DC power)** is not provided by the inverter. It is
  `VPV × IPV` (+ `VPV2 × IPV2` with 2 strings). That is a derived value, not a measurement. In the
  internal model it carries a `derived` flag; **Modbus cannot express that distinction**
  and publishes it as a plain valid value. Anyone who needs the derived status must
  use REST or MQTT. This register is therefore deliberately slightly less honest than the rest of
  the map — the alternative (omitting it) costs more than it delivers, since virtually every
  consumer expects DC power.
- **Register 7 (error code)** stays `0xFFFF`. The EverSolar protocol has no readable
  error code field (see `docs/eversolar-protocol.md`). A zero here would suggest "no error".

## AC phases (100-199)

20 registers per phase, starting at 100. The TL3000-20 is single-phase; only L1 is populated.

| Offset in block | Regs | Type | Meaning |
|---|---|---|---|
| +0 | 2 | float32 | Voltage (V) |
| +2 | 2 | float32 | Current (A) |
| +4 | 2 | float32 | Power (W) |

| Phase | Base | EverSolar |
|---|---|---|
| L1 | 100 | voltage+current ✔, power NaN |
| L2 | 120 | everything NaN |
| L3 | 140 | everything NaN |

## DC/MPPT (200-299)

20 registers per MPPT, starting at 200.

| Offset | Regs | Type | Meaning |
|---|---|---|---|
| +0 | 2 | float32 | Voltage (V) |
| +2 | 2 | float32 | Current (A) |
| +4 | 2 | float32 | Power (W, derived) |

| MPPT | Base | EverSolar |
|---|---|---|
| 1 | 200 | ✔ |
| 2 | 220 | ✔ *only with 2-string layout*, otherwise NaN |

## Battery (300-399) and grid meter (400-499)

Fully reserved. For EverSolar everything is NaN / bitmap bit 0. Populated once a driver with
battery or meter support is added — without changing the map.

## Status and errors (500-599)

| Raw | Regs | Type | Meaning |
|---|---|---|---|
| 500 | 1 | uint16 | Status code (raw, = reg 6) |
| 501 | 1 | uint16 | Error code (= reg 7) |
| 502 | 1 | uint16 | Consecutive failed polls |
| 510 | 16 | string | `status_text`, ASCII, null-padded (32 characters) |

## Capabilities + validity bitmap (600-699)

| Raw | Regs | Type | Meaning |
|---|---|---|---|
| 600 | 4 | bitmap64 | `InverterCapability` read bits |
| 604 | 4 | bitmap64 | `InverterCapability` write bits (MVP: all 0) |
| 610 | 8 | bitmap128 | **Validity bitmap** of the core measurements |
| 620 | 1 | uint16 | Number of AC phases (EverSolar: 1) |
| 621 | 1 | uint16 | Number of MPPTs (EverSolar: 1 or 2, from frame length) |
| 622 | 1 | uint16 | Battery present (0/1) |
| 623 | 1 | uint16 | Driver read-only (MVP: 1) |

The bit indices are listed in the table further down and are fixed in `ValidityBit` in
`src/outputs/modbus_tcp/register_map.h`. That order is part of the schema version and must not
change within version 1.

Register 623 = 1 tells a client up front that writing is pointless. That is more useful than
just an exception on FC6.

## Identity strings (700-799)

ASCII, null-padded, 2 characters per register, big-endian.

| Raw | Regs | Chars | Field |
|---|---|---|---|
| 700 | 16 | 32 | Manufacturer |
| 716 | 16 | 32 | Model |
| 732 | 16 | 32 | Serial number |
| 748 | 8 | 16 | Inverter firmware version |
| 756 | 8 | 16 | Driver ID |
| 764 | 8 | 16 | Bridge firmware version |

Unknown strings are entirely `0x00` — an empty string here is unambiguously "unknown",
there is no possible confusion with a valid value.

## Bridge diagnostics (800-899, also on Unit ID 250)

| Raw | Regs | Type | Meaning |
|---|---|---|---|
| 800 | 2 | uint32 | Uptime (s) |
| 802 | 2 | uint32 | Free heap (bytes) |
| 804 | 2 | uint32 | Minimum free heap (bytes) |
| 806 | 1 | uint16 | Reset reason (`esp_reset_reason_t`) |
| 807 | 1 | uint16 | Wifi RSSI (int16) |
| 810 | 2 | uint32 | Wifi reconnects |
| 812 | 2 | uint32 | MQTT reconnects |
| 814 | 2 | uint32 | Modbus client connections |
| 816 | 2 | uint32 | REST requests |
| 818 | 2 | uint32 | Invalid frames |
| 820 | 3 | uint16[3] | Firmware version major/minor/patch |
| 850 | 1 | uint16 | Relay count — `0xFFFF` = no relay hardware on this board |
| 851 | 1 | uint16 | Relay state bitmask (bit *i* = relay *i* energised); `0xFFFF` without hardware |

## Examples

### mbpoll

```bash
# AC power (float32, high word first) — raw 10, mbpoll is 1-based
mbpoll -m tcp -a 1 -t 4 -r 11 -c 2 -0 heliograph.local
# All core registers
mbpoll -m tcp -a 1 -t 4 -r 1 -c 64 -0 heliograph.local
```

A complete, working client is in [`tools/read_modbus.py`](../tools/read_modbus.py) —
it checks the schema version, reads the validity bitmap and handles NaN correctly.

### Python (pymodbus 3.x)

**Note the API changes.** This example has been verified against **pymodbus 3.14** (2026-07):

- `slave=` is now called `device_id=`;
- `pymodbus.payload` (`BinaryPayloadDecoder`) **no longer exists** — use
  `client.convert_from_registers()`.

Older examples found online still use the removed API.

```python
from pymodbus.client import ModbusTcpClient
from pymodbus.client.mixin import ModbusClientMixin

c = ModbusTcpClient("heliograph.local", port=502)
c.connect()

rr = c.read_input_registers(address=10, count=2, device_id=1)
watts = c.convert_from_registers(
    rr.registers, ModbusClientMixin.DATATYPE.FLOAT32, word_order="big"
)
# NaN means unknown: not supported by this driver, or stale. Never 0.
print("unknown" if watts != watts else f"{watts:.1f} W")
c.close()
```

Anyone who wants to be version-independent can decode it themselves — two registers, high word first:

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

MQTT discovery is the simpler route for Home Assistant; this Modbus config is intended for
those who already use Modbus.

### EVCC (custom meter)

There is **no** EVCC template for this custom-built map — it has to be done manually:

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

Use `modbus-read`: FC `InputRegister`, address `10`, quantity `2`, unit ID `1`. Then add
a `function` node that converts two words to float32 (high word first) **and catches NaN** —
`Buffer.from` + `readFloatBE`, then `if (Number.isNaN(v)) return null;`.

## Validity bitmap — bit indices (schema v1)

Fixed in `ValidityBit` in `src/outputs/modbus_tcp/register_map.h`. Within schema version 1
these **must not** change; only append at the end.

| Bit | Measurement | Bit | Measurement |
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
| 11 | status code | 26 | `battery.charge_power` |
| 12 | error code | 27 | `battery.discharge_power` |
| 13 | `ac.phase_l1.power` | 28 | `grid.import_power` |
| 14 | `ac.phase_l2.voltage` | 29 | `grid.export_power` |

Bit `n` is in register `610 + n/16`, at bit position `n % 16`.

**Guarantee:** the bitmap and the NaN sentinel never contradict each other. A bit is 1 if and
only if the corresponding float register is not NaN. This is enforced by
`test_validity_bitmap_and_nan_always_agree`.

## Stale data does not count as a measurement

When `data_stale = 1`, the bridge publishes the measurements as **NaN**, not as the last
known value. The last value does remain in the internal model and is visible via REST/MQTT with a
`stale` flag — but Modbus has no way to indicate "this number is old", so a
consumer would treat it as current. Unknown is therefore the more honest answer here.

## Open items

- Whether `energy.today` remains usable as `total_increasing` around midnight (reset to 0)
  depends on the inverter's behavior — to be determined in Phase 9.
- The eModbus server wiring (`modbus_tcp_server.cpp`) has **not been compiled yet**; only the
  register map has been tested.

Registers 850-851 are **read-only observation** of the bridge relays (DRM contacts).
Relay control never goes through Modbus: the unauthenticated Modbus surface only
observes, and commands go through the admin-gated REST/MQTT paths and their gates.
