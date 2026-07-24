#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate C++ driver profile tables from TOML device profiles.

Device profiles live in profiles/<family>/*.toml. Each file describes one register-map
profile for a table-driven driver (today: growatt_modbus). This script validates them
against the canonical measurement vocabulary in src/device/measurement.h and emits
src/drivers/growatt_modbus/profiles_generated.cpp — constexpr tables, zero runtime
parsing on the ESP32.

Runs in two modes:
  - PlatformIO pre-build script (extra_scripts = pre:tools/gen_profiles.py): regenerates
    before every build, so a broken profile fails the BUILD, never the device at 3 AM.
  - Standalone: `python3 tools/gen_profiles.py` validates and regenerates;
    `--check` validates without writing; `--list-measurements` prints the vocabulary.

Requires Python 3.11+ (tomllib is stdlib since 3.11 — deliberately no third-party
dependency). PlatformIO's penv ships far newer than that.

The output file is generated, never edited, and not committed: contributors edit the
TOML, the build does the rest. See docs/adding-a-device.md.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

if sys.version_info < (3, 11):
    sys.stderr.write(
        "gen_profiles.py needs Python >= 3.11 (tomllib); found %s\n"
        % sys.version.split()[0]
    )
    sys.exit(1)

import tomllib


def set_root(root: Path) -> None:
    """Project root and everything derived from it. Not simply `__file__`: under
    PlatformIO, SCons exec()s this script without __file__, so the pre-script branch
    at the bottom sets it from the build environment's PROJECT_DIR instead."""
    global ROOT, MEASUREMENT_H, COMMAND_CPP, PROFILES_DIR, OUTPUT
    ROOT = root
    MEASUREMENT_H = ROOT / "src" / "device" / "measurement.h"
    COMMAND_CPP = ROOT / "src" / "device" / "command.cpp"
    PROFILES_DIR = ROOT / "profiles"
    OUTPUT = ROOT / "src" / "drivers" / "growatt_modbus" / "profiles_generated.cpp"


try:
    set_root(Path(__file__).resolve().parent.parent)
except NameError:
    set_root(Path.cwd())  # provisional; the PlatformIO branch below overrides this

# Mirrors GrowattDriver::kMaxBlocks (growatt_driver.h). The driver's scratch buffer holds
# this many blocks; a profile asking for more would silently drop reads.
MAX_BLOCKS = 8
# Modbus read limit: at most 125 registers per transaction.
MAX_BLOCK_COUNT = 125

# Unit symbol -> (C++ Unit enumerator, C++ MeasurementType enumerator).
# Must stay in sync with unitSymbol() in src/device/measurement.cpp. The measurement type
# is derived from the unit so contributors never touch internal enums.
UNITS: dict[str, tuple[str, str]] = {
    "W": ("Watt", "Power"),
    "V": ("Volt", "Voltage"),
    "A": ("Ampere", "Current"),
    "Hz": ("Hertz", "Frequency"),
    "°C": ("Celsius", "Temperature"),
    "C": ("Celsius", "Temperature"),  # ASCII convenience alias
    "kWh": ("KilowattHour", "Energy"),
    "h": ("Hour", "Duration"),
    "%": ("Percent", "Ratio"),
    "dBm": ("Decibel", "SignalStrength"),
    "s": ("Second", "Duration"),
}

# Register data type -> (words, signed). 32-bit values are high word first (the Modbus
# convention Growatt uses); a device with swapped word order needs decoder support first.
TYPES: dict[str, tuple[int, bool]] = {
    "u16": (1, False),
    "s16": (1, True),
    "u32": (2, False),
    "s32": (2, True),
}

SPACES = {"input": "Input", "holding": "Holding"}

PARITIES = {"none": "None", "even": "Even", "odd": "Odd"}

# Commands that are not numeric setpoints (no value, no min/max) and therefore cannot be
# expressed as a [[write]] register row. They need driver-level semantics (what value means
# "start"?) — a schema extension when a first device requires one, not a guess now.
NON_NUMERIC_COMMANDS = {"start", "stop", "synchronize_time"}

ID_RE = re.compile(r"^[a-z][a-z0-9_]*$")


def load_vocabulary() -> dict[str, str]:
    """Canonical measurement id -> C++ constant name, parsed from measurement.h.

    Parsing the header instead of duplicating the list means the vocabulary has exactly
    one source of truth; a renamed constant shows up here immediately.
    """
    text = MEASUREMENT_H.read_text(encoding="utf-8")
    block = re.search(r"namespace measurement_id \{(.*?)\}", text, re.DOTALL)
    if not block:
        raise SystemExit(f"could not find namespace measurement_id in {MEASUREMENT_H}")
    pairs = re.findall(
        r'inline constexpr const char\*\s+(k\w+)\s*=\s*"([^"]+)"', block.group(1)
    )
    if not pairs:
        raise SystemExit(f"no measurement ids parsed from {MEASUREMENT_H}")
    return {mid: name for name, mid in pairs}


def load_command_vocabulary() -> dict[str, str]:
    """Canonical command name -> C++ InverterCommandType enumerator, parsed from
    commandTypeName() in command.cpp — the same single-source-of-truth trick as the
    measurement vocabulary. Non-numeric commands are excluded (see NON_NUMERIC_COMMANDS)."""
    text = COMMAND_CPP.read_text(encoding="utf-8")
    pairs = re.findall(r'case InverterCommandType::(\w+):\s*return "([^"]+)";', text)
    if not pairs:
        raise SystemExit(f"no command names parsed from {COMMAND_CPP}")
    return {
        name: enum
        for enum, name in pairs
        if name not in NON_NUMERIC_COMMANDS and enum != "_Count"
    }


class ProfileError(Exception):
    pass


def _require(table: dict, key: str, kind: type, where: str):
    if key not in table:
        raise ProfileError(f"{where}: missing required key '{key}'")
    value = table[key]
    # bool is a subclass of int in Python; keep the two apart in the schema.
    if kind is int and isinstance(value, bool):
        raise ProfileError(f"{where}: '{key}' must be an integer, got a boolean")
    if not isinstance(value, kind):
        raise ProfileError(
            f"{where}: '{key}' must be {kind.__name__}, got {type(value).__name__}"
        )
    return value


def parse_profile(
    path: Path, vocabulary: dict[str, str], commands: dict[str, str]
) -> dict:
    with path.open("rb") as f:
        data = tomllib.load(f)
    where = path.relative_to(ROOT)

    meta = _require(data, "profile", dict, f"{where}")
    driver = _require(meta, "driver", str, f"{where} [profile]")
    if driver != "growatt_modbus":
        raise ProfileError(
            f"{where}: unknown driver '{driver}' "
            f"(only 'growatt_modbus' is table-driven today)"
        )

    pid = _require(meta, "id", str, f"{where} [profile]")
    if not ID_RE.match(pid):
        raise ProfileError(f"{where}: profile id '{pid}' must match {ID_RE.pattern}")
    display = _require(meta, "display_name", str, f"{where} [profile]")
    if not display:
        raise ProfileError(f"{where}: display_name must not be empty")
    phases = _require(meta, "phases", int, f"{where} [profile]")
    if not 1 <= phases <= 3:
        raise ProfileError(f"{where}: phases must be 1-3, got {phases}")
    mppts = _require(meta, "mppts", int, f"{where} [profile]")
    if not 0 <= mppts <= 8:
        raise ProfileError(f"{where}: mppts must be 0-8, got {mppts}")
    battery = _require(meta, "battery", bool, f"{where} [profile]")
    default = bool(meta.get("default", False))

    transports = meta.get("transports", ["rtu"])
    if (
        not isinstance(transports, list)
        or not transports
        or any(t not in ("rtu", "tcp") for t in transports)
    ):
        raise ProfileError(
            f"{where}: transports must be a non-empty list drawn from ['rtu', 'tcp']"
        )

    serial = data.get("serial")
    parsed_serial = None
    if serial is not None:
        sw = f"{where} [serial]"
        baud = _require(serial, "baud", int, sw)
        if baud not in (2400, 4800, 9600, 19200, 38400, 57600, 115200):
            raise ProfileError(f"{sw}: baud {baud} is not a standard rate")
        parity = serial.get("parity", "none")
        if parity not in PARITIES:
            raise ProfileError(f"{sw}: parity must be one of {sorted(PARITIES)}")
        stop_bits = serial.get("stop_bits", 1)
        if isinstance(stop_bits, bool) or stop_bits not in (1, 2):
            raise ProfileError(f"{sw}: stop_bits must be 1 or 2")
        parsed_serial = {"baud": baud, "parity": parity, "stop_bits": stop_bits}

    tcp = data.get("tcp")
    tcp_port = 0
    if tcp is not None:
        if "tcp" not in transports:
            raise ProfileError(f"{where}: [tcp] section without 'tcp' in transports")
        tcp_port = tcp.get("port", 502)
        if (
            isinstance(tcp_port, bool)
            or not isinstance(tcp_port, int)
            or not 1 <= tcp_port <= 65535
        ):
            raise ProfileError(f"{where} [tcp]: port must be 1-65535")
    elif "tcp" in transports:
        tcp_port = 502  # Modbus TCP default

    blocks = data.get("block", [])
    if not blocks:
        raise ProfileError(f"{where}: at least one [[block]] is required")
    if len(blocks) > MAX_BLOCKS:
        raise ProfileError(
            f"{where}: {len(blocks)} blocks, driver supports at most {MAX_BLOCKS}"
        )
    parsed_blocks = []
    for i, b in enumerate(blocks):
        bw = f"{where} [[block]] #{i + 1}"
        space = _require(b, "space", str, bw)
        if space not in SPACES:
            raise ProfileError(f"{bw}: space must be one of {sorted(SPACES)}")
        start = _require(b, "start", int, bw)
        count = _require(b, "count", int, bw)
        if not 0 <= start <= 0xFFFF:
            raise ProfileError(f"{bw}: start must be 0-65535")
        if not 1 <= count <= MAX_BLOCK_COUNT:
            raise ProfileError(
                f"{bw}: count must be 1-{MAX_BLOCK_COUNT} (Modbus read limit)"
            )
        if start + count > 0x10000:
            raise ProfileError(f"{bw}: start+count exceeds the register address space")
        parsed_blocks.append({"space": space, "start": start, "count": count})

    def covered(space: str, address: int) -> bool:
        return any(
            b["space"] == space and b["start"] <= address < b["start"] + b["count"]
            for b in parsed_blocks
        )

    registers = data.get("register", [])
    if not registers:
        raise ProfileError(f"{where}: at least one [[register]] is required")
    seen_measurements: set[str] = set()
    parsed_regs = []
    for i, r in enumerate(registers):
        rw = f"{where} [[register]] #{i + 1}"
        mid = _require(r, "measurement", str, rw)
        if mid not in vocabulary:
            known = ", ".join(sorted(vocabulary))
            raise ProfileError(
                f"{rw}: unknown measurement '{mid}'.\n"
                f"  Canonical ids: {known}\n"
                f"  (see docs/device-profiles/canonical-measurements.md)"
            )
        if mid in seen_measurements:
            raise ProfileError(f"{rw}: measurement '{mid}' mapped twice")
        seen_measurements.add(mid)
        name = _require(r, "display_name", str, rw)
        if not name:
            raise ProfileError(f"{rw}: display_name must not be empty")
        space = _require(r, "space", str, rw)
        if space not in SPACES:
            raise ProfileError(f"{rw}: space must be one of {sorted(SPACES)}")
        address = _require(r, "address", int, rw)
        rtype = _require(r, "type", str, rw)
        if rtype not in TYPES:
            raise ProfileError(f"{rw}: type must be one of {sorted(TYPES)}")
        words, _signed = TYPES[rtype]
        for a in range(address, address + words):
            if not covered(space, a):
                raise ProfileError(
                    f"{rw}: register {a} ({space}) is not inside any declared [[block]] "
                    f"-- the driver would never read it (a {rtype} needs {words} "
                    f"consecutive registers)"
                )
        scale = r.get("scale", 1.0)
        if isinstance(scale, bool) or not isinstance(scale, (int, float)):
            raise ProfileError(f"{rw}: scale must be a number")
        if scale == 0:
            raise ProfileError(f"{rw}: scale must not be 0 (every reading would be 0)")
        unit = _require(r, "unit", str, rw)
        if unit not in UNITS:
            raise ProfileError(f"{rw}: unknown unit '{unit}'; known: {sorted(UNITS)}")
        parsed_regs.append(
            {
                "measurement": mid,
                "display_name": name,
                "space": space,
                "address": address,
                "type": rtype,
                "scale": float(scale),
                "unit": unit,
            }
        )

    # [[write]] rows: read-only is the default — a register is writable only when declared
    # here, and even then it stays dormant (see WriteMapping in growatt_registers.h).
    writes = data.get("write", [])
    seen_commands: set[str] = set()
    parsed_writes = []
    for i, wr in enumerate(writes):
        ww = f"{where} [[write]] #{i + 1}"
        cmd = _require(wr, "command", str, ww)
        if cmd not in commands:
            raise ProfileError(
                f"{ww}: unknown command '{cmd}'.\n"
                f"  Numeric setpoint commands: {', '.join(sorted(commands))}"
            )
        if cmd in seen_commands:
            raise ProfileError(f"{ww}: command '{cmd}' mapped twice")
        seen_commands.add(cmd)
        name = _require(wr, "display_name", str, ww)
        if not name:
            raise ProfileError(f"{ww}: display_name must not be empty")
        space = _require(wr, "space", str, ww)
        if space != "holding":
            raise ProfileError(
                f"{ww}: writes go to holding registers; space must be "
                f"'holding' (input registers are read-only by definition)"
            )
        address = _require(wr, "address", int, ww)
        if not 0 <= address <= 0xFFFF:
            raise ProfileError(f"{ww}: address must be 0-65535")
        rtype = _require(wr, "type", str, ww)
        if rtype not in TYPES:
            raise ProfileError(f"{ww}: type must be one of {sorted(TYPES)}")
        words, _signed = TYPES[rtype]
        function = wr.get(
            "function", "write_single" if words == 1 else "write_multiple"
        )
        if function not in ("write_single", "write_multiple"):
            raise ProfileError(f"{ww}: function must be write_single or write_multiple")
        if function == "write_single" and words != 1:
            raise ProfileError(
                f"{ww}: a {rtype} spans 2 registers and needs "
                f'function = "write_multiple" (FC 0x10)'
            )
        scale = wr.get("scale", 1.0)
        if isinstance(scale, bool) or not isinstance(scale, (int, float)) or scale == 0:
            raise ProfileError(f"{ww}: scale must be a non-zero number")
        unit = _require(wr, "unit", str, ww)
        if unit not in UNITS:
            raise ProfileError(f"{ww}: unknown unit '{unit}'; known: {sorted(UNITS)}")
        minimum = wr.get("minimum")
        maximum = wr.get("maximum")
        for key, val in (("minimum", minimum), ("maximum", maximum)):
            if val is None:
                raise ProfileError(
                    f"{ww}: '{key}' is required for a write register -- "
                    f"the dispatcher refuses unbounded writes"
                )
            if isinstance(val, bool) or not isinstance(val, (int, float)):
                raise ProfileError(f"{ww}: '{key}' must be a number")
        if not minimum < maximum:
            raise ProfileError(f"{ww}: minimum must be < maximum")
        step = wr.get("step", 1.0)
        if isinstance(step, bool) or not isinstance(step, (int, float)) or step <= 0:
            raise ProfileError(f"{ww}: step must be a positive number")
        verified = wr.get("verified", False)
        if not isinstance(verified, bool):
            raise ProfileError(f"{ww}: verified must be a boolean")
        parsed_writes.append(
            {
                "command": cmd,
                "display_name": name,
                "address": address,
                "type": rtype,
                "function": function,
                "scale": float(scale),
                "unit": unit,
                "minimum": float(minimum),
                "maximum": float(maximum),
                "step": float(step),
                "verified": verified,
            }
        )

    return {
        "path": where,
        "id": pid,
        "display_name": display,
        "default": default,
        "phases": phases,
        "mppts": mppts,
        "battery": battery,
        "transports": transports,
        "serial": parsed_serial,
        "tcp_port": tcp_port,
        "blocks": parsed_blocks,
        "registers": parsed_regs,
        "writes": parsed_writes,
    }


def cpp_symbol(pid: str) -> str:
    return "".join(part.capitalize() for part in pid.split("_"))


def cpp_string(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def cpp_float(x: float) -> str:
    text = repr(x)
    return text if ("." in text or "e" in text or "inf" in text) else text + ".0"


def generate(
    profiles: list[dict], vocabulary: dict[str, str], commands: dict[str, str]
) -> str:
    lines: list[str] = []
    w = lines.append
    w("// SPDX-License-Identifier: MIT")
    w("//")
    w("// GENERATED FILE -- DO NOT EDIT.")
    w("// Emitted by tools/gen_profiles.py from profiles/growatt/*.toml. To change a")
    w("// register map, edit the TOML and rebuild; this file is regenerated pre-build")
    w("// and is not committed. See docs/adding-a-device.md.")
    w("")
    w("#include <cstring>")
    w("")
    w('#include "drivers/growatt_modbus/growatt_registers.h"')
    w("")
    w("namespace heliograph::growatt {")
    w("namespace {")
    for p in profiles:
        sym = cpp_symbol(p["id"])
        w("")
        w(f"// --- {p['id']}: {p['display_name']} (from {p['path']}) ---")
        w(f"constexpr RegisterMapping k{sym}Mappings[] = {{")
        for r in p["registers"]:
            words, signed = TYPES[r["type"]]
            unit_enum, mtype = UNITS[r["unit"]]
            const = vocabulary[r["measurement"]]
            w(
                f"    {{measurement_id::{const}, MeasurementType::{mtype}, "
                f"Unit::{unit_enum}, {cpp_string(r['display_name'])},"
            )
            w(
                f"     RegSpace::{SPACES[r['space']]}, {r['address']}, {words}, "
                f"{cpp_float(r['scale'])}, {'true' if signed else 'false'}}},"
            )
        w("};")
        w(f"constexpr RegBlock k{sym}Blocks[] = {{")
        for b in p["blocks"]:
            w(f"    {{RegSpace::{SPACES[b['space']]}, {b['start']}, {b['count']}}},")
        w("};")
        if p["writes"]:
            w(f"constexpr WriteMapping k{sym}Writes[] = {{")
            for wr in p["writes"]:
                words, _signed = TYPES[wr["type"]]
                unit_enum, _mtype = UNITS[wr["unit"]]
                multiple = "true" if wr["function"] == "write_multiple" else "false"
                w(
                    f"    {{InverterCommandType::{commands[wr['command']]}, "
                    f"{cpp_string(wr['display_name'])},"
                )
                w(
                    f"     RegSpace::Holding, {wr['address']}, {words}, {multiple}, "
                    f"{cpp_float(wr['scale'])},"
                )
                w(
                    f"     {cpp_float(wr['minimum'])}, {cpp_float(wr['maximum'])}, "
                    f"{cpp_float(wr['step'])}, Unit::{unit_enum}, "
                    f"{'true' if wr['verified'] else 'false'}}},"
                )
            w("};")
    w("")
    w("constexpr GrowattProfile kProfiles[] = {")
    for p in profiles:
        sym = cpp_symbol(p["id"])
        w(
            f"    {{{cpp_string(p['id'])}, {cpp_string(p['display_name'])}, "
            f"{'true' if p['battery'] else 'false'}, {p['phases']}, {p['mppts']},"
        )
        w(f"     k{sym}Blocks, sizeof(k{sym}Blocks) / sizeof(k{sym}Blocks[0]),")
        w(f"     k{sym}Mappings, sizeof(k{sym}Mappings) / sizeof(k{sym}Mappings[0]),")
        if p["writes"]:
            w(f"     k{sym}Writes, sizeof(k{sym}Writes) / sizeof(k{sym}Writes[0]),")
        else:
            w("     nullptr, 0,")
        rtu = "true" if "rtu" in p["transports"] else "false"
        tcp = "true" if "tcp" in p["transports"] else "false"
        w(
            f"     /*supportsRtu=*/{rtu}, /*supportsTcp=*/{tcp}, "
            f"/*tcpPort=*/{p['tcp_port']},"
        )
        if p["serial"]:
            s = p["serial"]
            w("     /*hasSerial=*/true,")
            w(
                f"     SerialProfile{{{s['baud']}, SerialParity::{PARITIES[s['parity']]}, "
                f"8, {s['stop_bits']}, 1000, 3}}}},"
            )
        else:
            w("     /*hasSerial=*/false, SerialProfile{}},")
    w("};")
    w("")
    w("}  // namespace")
    w("")
    w("const GrowattProfile* findProfile(const char* id) {")
    w("    if (id == nullptr) {")
    w("        return nullptr;")
    w("    }")
    w("    for (const GrowattProfile& p : kProfiles) {")
    w("        if (std::strcmp(p.id, id) == 0) {")
    w("            return &p;")
    w("        }")
    w("    }")
    w("    return nullptr;")
    w("}")
    w("")
    default_index = next(i for i, p in enumerate(profiles) if p["default"])
    w(f"// [profile] default = true in {profiles[default_index]['path']}.")
    w(
        f"const GrowattProfile& defaultProfile() {{ return kProfiles[{default_index}]; }}"
    )
    w("")
    w("size_t profileCount() { return sizeof(kProfiles) / sizeof(kProfiles[0]); }")
    w("")
    w("const GrowattProfile& profileAt(size_t index) {")
    w("    return kProfiles[index < profileCount() ? index : 0];")
    w("}")
    w("")
    w("}  // namespace heliograph::growatt")
    w("")
    return "\n".join(lines)


def run(check_only: bool = False) -> int:
    vocabulary = load_vocabulary()
    commands = load_command_vocabulary()
    paths = sorted(
        p for p in PROFILES_DIR.rglob("*.toml") if not p.name.startswith("_")
    )
    profiles: list[dict] = []
    errors: list[str] = []
    for path in paths:
        try:
            profiles.append(parse_profile(path, vocabulary, commands))
        except ProfileError as e:
            errors.append(str(e))
        except tomllib.TOMLDecodeError as e:
            errors.append(f"{path.relative_to(ROOT)}: TOML syntax error: {e}")

    if not errors:
        ids = [p["id"] for p in profiles]
        for dup in {i for i in ids if ids.count(i) > 1}:
            errors.append(f"profile id '{dup}' is defined in more than one file")
        defaults = [p for p in profiles if p["default"]]
        if not profiles:
            errors.append(
                f"no profiles found under {PROFILES_DIR.relative_to(ROOT)}/ "
                f"-- the growatt driver needs at least one"
            )
        elif len(defaults) != 1:
            errors.append(
                f"exactly one profile must set `default = true`; found {len(defaults)} "
                f"({', '.join(p['id'] for p in defaults) or 'none'})"
            )

    if errors:
        sys.stderr.write("gen_profiles.py: device profile validation FAILED\n\n")
        for e in errors:
            sys.stderr.write(f"  * {e}\n")
        sys.stderr.write("\n")
        return 1

    content = generate(profiles, vocabulary, commands)
    if check_only:
        print(
            f"gen_profiles.py: {len(profiles)} profile(s) valid: "
            + ", ".join(p["id"] for p in profiles)
        )
        return 0
    # Write only on change so an untouched profile never dirties mtimes and triggers
    # a needless rebuild of the driver.
    if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != content:
        OUTPUT.write_text(content, encoding="utf-8")
        print(
            f"gen_profiles.py: wrote {OUTPUT.relative_to(ROOT)} "
            f"({len(profiles)} profile(s): {', '.join(p['id'] for p in profiles)})"
        )
    return 0


def main(argv: list[str]) -> int:
    if "--list-measurements" in argv:
        for mid, const in sorted(load_vocabulary().items()):
            print(f"{mid:32} ({const})")
        return 0
    if "--list-commands" in argv:
        for cmd, enum in sorted(load_command_vocabulary().items()):
            print(f"{cmd:36} ({enum})")
        return 0
    return run(check_only="--check" in argv)


# Under PlatformIO (extra_scripts) SCons provides Import(); standalone it does not.
try:
    Import("env")  # type: ignore[name-defined]  # noqa: F821
    set_root(Path(env["PROJECT_DIR"]))  # type: ignore[name-defined]  # noqa: F821
    if run() != 0:
        env.Exit(1)  # type: ignore[name-defined]  # noqa: F821
except NameError:
    if __name__ == "__main__":
        sys.exit(main(sys.argv[1:]))
