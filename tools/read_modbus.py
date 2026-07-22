#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reads the Heliograph Modbus TCP register map.

Doubles as the reference for how a client is expected to handle this map: check the schema
version, read the validity bitmap, and treat NaN as "unknown" rather than as zero.

    pip install pymodbus
    python3 tools/read_modbus.py heliograph.local
"""

from __future__ import annotations

import argparse
import math
import struct
import sys

try:
    from pymodbus.client import ModbusTcpClient
except ImportError:
    sys.exit("pymodbus is required: pip install pymodbus")

SCHEMA_VERSION = 1

# Keep in step with src/outputs/modbus_tcp/register_map.h.
VALIDITY_BITMAP = 610
FLOATS = [
    ("ac.power.total", 10, "W", 0),
    ("ac.phase_l1.voltage", 12, "V", 1),
    ("ac.phase_l1.current", 14, "A", 2),
    ("ac.frequency", 16, "Hz", 3),
    ("dc.power.total", 20, "W", 4),
    ("dc.mppt_1.voltage", 22, "V", 5),
    ("dc.mppt_1.current", 24, "A", 6),
    ("inverter.temperature", 30, "degC", 7),
    ("energy.today", 40, "kWh", 8),
    ("energy.total", 42, "kWh", 9),
]
STRINGS = [
    ("manufacturer", 700, 16),
    ("model", 716, 16),
    ("serial", 732, 16),
    ("driver", 756, 8),
]


def u16(regs: list[int], base: int, addr: int) -> int:
    return regs[addr - base]


def u32(regs: list[int], base: int, addr: int) -> int:
    """High word first — the map's documented word order."""
    return (regs[addr - base] << 16) | regs[addr - base + 1]


def f32(regs: list[int], base: int, addr: int) -> float:
    return struct.unpack(
        ">f", struct.pack(">HH", regs[addr - base], regs[addr - base + 1])
    )[0]


def text(regs: list[int], base: int, addr: int, count: int) -> str:
    raw = b"".join(struct.pack(">H", regs[addr - base + i]) for i in range(count))
    return raw.split(b"\x00", 1)[0].decode("ascii", "replace")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=502)
    ap.add_argument("--unit", type=int, default=1)
    args = ap.parse_args()

    client = ModbusTcpClient(args.host, port=args.port)
    if not client.connect():
        return f"cannot connect to {args.host}:{args.port}"

    # pymodbus renamed the unit-id argument from `slave` to `device_id` (3.9+), so try the
    # current name and fall back. Verified against pymodbus 3.14.
    import inspect

    unit_kw = (
        "device_id"
        if "device_id" in inspect.signature(client.read_input_registers).parameters
        else "slave"
    )

    def read(addr: int, count: int) -> list[int]:
        rr = client.read_input_registers(
            address=addr, count=count, **{unit_kw: args.unit}
        )
        if rr.isError():
            raise SystemExit(f"modbus error reading {addr}+{count}: {rr}")
        return rr.registers

    core = read(0, 64)
    version = u32(core, 0, 0)
    if version != SCHEMA_VERSION:
        print(
            f"WARNING: register map schema is v{version}, this script knows v{SCHEMA_VERSION}."
        )
        print("Field positions may have moved; not guessing.")
        return 1

    bitmap_regs = read(VALIDITY_BITMAP, 8)

    def valid(bit: int) -> bool:
        return bool(bitmap_regs[bit // 16] & (1 << (bit % 16)))

    print(f"bridge online   : {bool(u16(core, 0, 2))}")
    print(f"inverter online : {bool(u16(core, 0, 3))}")
    print(f"data valid      : {bool(u16(core, 0, 4))}")
    print(f"data stale      : {bool(u16(core, 0, 5))}")
    print(f"uptime          : {u32(core, 0, 62)} s")

    since = u32(core, 0, 50)
    print(f"last valid poll : {'never' if since == 0xFFFFFFFF else f'{since} s ago'}")
    print()

    identity = read(700, 64)
    for name, addr, count in STRINGS:
        value = text(identity, 700, addr, count)
        print(f"{name:16}: {value or '(unknown)'}")
    print()

    caps = read(600, 24)
    print(f"phases          : {u16(caps, 600, 620)}")
    print(f"mppts           : {u16(caps, 600, 621)}")
    print(f"battery         : {bool(u16(caps, 600, 622))}")
    print(f"read-only       : {bool(u16(caps, 600, 623))}")
    print()

    for name, addr, unit, bit in FLOATS:
        value = f32(core, 0, addr)
        # The two signals must agree; if they ever do not, the firmware has a bug.
        if math.isnan(value) != (not valid(bit)):
            print(f"{name:22}: INCONSISTENT (NaN={math.isnan(value)} bit={valid(bit)})")
        elif math.isnan(value):
            # Not zero. Unknown: unsupported by this driver, or stale.
            print(f"{name:22}: unknown")
        else:
            print(f"{name:22}: {value:10.2f} {unit}")

    status_valid = valid(11)
    print()
    print(f"status code     : {u16(core, 0, 6) if status_valid else 'unknown'}")
    err = u16(core, 0, 7)
    # 0xFFFF means the driver has no error code field at all -- not "no fault".
    print(
        f"error code      : {'not reported by this protocol' if err == 0xFFFF else err}"
    )

    client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
