#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate C++ driver profile tables from TOML device profiles.

Device profiles live in profiles/<family>/*.toml. Each file describes one register-map
profile for a table-driven driver (today: modbus_profile). This script validates them
against the canonical measurement vocabulary in src/device/measurement.h and emits
src/drivers/modbus_profile/profiles_generated.cpp — constexpr tables, zero runtime
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
    OUTPUT = ROOT / "src" / "drivers" / "modbus_profile" / "profiles_generated.cpp"


try:
    set_root(Path(__file__).resolve().parent.parent)
except NameError:
    set_root(Path.cwd())  # provisional; the PlatformIO branch below overrides this

# Mirrors ModbusProfileDriver::kMaxBlocks (modbus_profile_driver.h). The driver's scratch buffer holds
# this many blocks; a profile asking for more would silently drop reads.
MAX_BLOCKS = 8
# Modbus read limit: at most 125 registers per transaction.
MAX_BLOCK_COUNT = 125
# Selectable modes per enum setpoint. Not a protocol limit -- a sanity bound, because the option
# table is emitted into flash and a Home Assistant dropdown of forty modes is not a control.
MAX_ENUM_OPTIONS = 16

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

# Register data type -> (words, signed). 32-bit values are high word first (what nearly every
# Modbus inverter does); a device with swapped word order needs decoder support first.
TYPES: dict[str, tuple[int, bool]] = {
    "u16": (1, False),
    "s16": (1, True),
    "u32": (2, False),
    "s32": (2, True),
}

SPACES = {"input": "Input", "holding": "Holding"}

# 32-bit word order. High-word-first is what nearly every Modbus inverter does and stays the
# default; "low_first" exists because at least one vendor datasheet specifies it for a register
# that same datasheet recommends using.
WORD_ORDERS = {"high_first": "false", "low_first": "true"}

PARITIES = {"none": "None", "even": "Even", "odd": "Odd"}

# How far a register MAP has been proven, mapping to ProfileStatus in profile_tables.h. The
# rungs deliberately mirror DriverSupportLevel: the question "how much should I trust this"
# has one answer shape, whether it is asked about code or about a table.
#
#   experimental  transcribed from documentation or a community map; never met the device
#   beta          confirmed against real hardware, not yet run long enough to trust unattended
#   stable        validated and soak-tested
#   deprecated    superseded or known wrong; kept so stored configs still resolve
PROFILE_STATUSES = {
    "experimental": "Experimental",
    "beta": "Beta",
    "stable": "Stable",
    "deprecated": "Deprecated",
}

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
    # Bounded by the namespace's own closing comment, not by the first `}` in it: kAll's
    # initializer brace comes first, so the loose form silently stopped parsing there and a
    # constant declared after kAll would have been invisible here while check_measurement_ids.py
    # still saw it -- a profile mapping it would be rejected as "unknown measurement".
    block = re.search(
        r"namespace measurement_id \{(.*?)\}\s*//\s*namespace measurement_id",
        text,
        re.DOTALL,
    )
    if not block:
        raise SystemExit(f"could not find namespace measurement_id in {MEASUREMENT_H}")
    pairs = re.findall(
        r'(?:inline\s+)?constexpr\s+const\s+char\s*\*\s*(k\w+)\s*=\s*"([^"]+)"',
        block.group(1),
    )
    if not pairs:
        raise SystemExit(f"no measurement ids parsed from {MEASUREMENT_H}")
    return {mid: name for name, mid in pairs}


def load_enum_command_names() -> set[str]:
    """Command names that carry an enum rather than a number, parsed from
    commandTakesEnumValue() in command.cpp.

    Read from the source for the same reason the other two vocabularies are: a command that
    changes shape in C++ and not here would be validated as the wrong kind of row -- accepted
    with min/max bounds it cannot use, and then refused at runtime for want of a mode list.
    """
    text = COMMAND_CPP.read_text(encoding="utf-8")
    body = re.search(
        r"bool commandTakesEnumValue\(InverterCommandType type\) \{(.*?)\n\}",
        text,
        re.DOTALL,
    )
    if not body:
        raise SystemExit(f"could not find commandTakesEnumValue in {COMMAND_CPP}")
    enums = set(re.findall(r"InverterCommandType::(\w+)", body.group(1)))
    if not enums:
        raise SystemExit(f"no enum command types parsed from {COMMAND_CPP}")
    return enums


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
    path: Path,
    vocabulary: dict[str, str],
    commands: dict[str, str],
    enum_command_names: set[str],
) -> dict:
    with path.open("rb") as f:
        data = tomllib.load(f)
    where = path.relative_to(ROOT)

    meta = _require(data, "profile", dict, f"{where}")
    driver = _require(meta, "driver", str, f"{where} [profile]")
    if driver != "modbus_profile":
        raise ProfileError(
            f"{where}: unknown driver '{driver}' "
            f"(only 'modbus_profile' is table-driven today)"
        )

    pid = _require(meta, "id", str, f"{where} [profile]")
    if not ID_RE.match(pid):
        raise ProfileError(f"{where}: profile id '{pid}' must match {ID_RE.pattern}")
    display = _require(meta, "display_name", str, f"{where} [profile]")
    if not display:
        raise ProfileError(f"{where}: display_name must not be empty")
    check_text(display, f"{where} [profile]", "display_name")
    # Required, not optional-with-a-default. One driver now serves several vendors, so the
    # profile is the only thing that knows the brand -- and an empty manufacturer would show up
    # in Home Assistant as a device with no maker, which reads as a bug in the bridge.
    manufacturer = _require(meta, "manufacturer", str, f"{where} [profile]")
    if not manufacturer:
        raise ProfileError(f"{where}: manufacturer must not be empty")
    check_text(manufacturer, f"{where} [profile]", "manufacturer")
    phases = _require(meta, "phases", int, f"{where} [profile]")
    if not 1 <= phases <= 3:
        raise ProfileError(f"{where}: phases must be 1-3, got {phases}")
    mppts = _require(meta, "mppts", int, f"{where} [profile]")
    if not 0 <= mppts <= 8:
        raise ProfileError(f"{where}: mppts must be 0-8, got {mppts}")
    battery = _require(meta, "battery", bool, f"{where} [profile]")
    default = bool(meta.get("default", False))

    # How far this MAP has been proven -- a property of the register table, not of the driver
    # that reads it. The driver is one piece of code serving every vendor here, so its own
    # support level can only ever describe the least-proven profile in the build. Left that way,
    # the first profile confirmed on hardware would promote the driver and carry every
    # unconfirmed map up with it: a Huawei table that has never met a Huawei, labelled beta.
    #
    # Optional, defaulting to the LOWEST rung on purpose. A forgotten field must understate what
    # we know, never overstate it.
    status = meta.get("status", "experimental")
    if status not in PROFILE_STATUSES:
        raise ProfileError(
            f"{where}: status must be one of {sorted(PROFILE_STATUSES)}, got {status!r}"
        )

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
        probe = b.get("probe", False)
        if not isinstance(probe, bool):
            raise ProfileError(f"{bw}: probe must be true or false")
        parsed_blocks.append(
            {"space": space, "start": start, "count": count, "probe": probe}
        )

    def covered(space: str, address: int, exclude_probe: bool = False) -> bool:
        return any(
            b["space"] == space
            and b["start"] <= address < b["start"] + b["count"]
            and not (exclude_probe and b["probe"])
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
        check_text(name, rw, "display_name")
        space = _require(r, "space", str, rw)
        if space not in SPACES:
            raise ProfileError(f"{rw}: space must be one of {sorted(SPACES)}")
        address = _require(r, "address", int, rw)
        rtype = _require(r, "type", str, rw)
        if rtype not in TYPES:
            raise ProfileError(f"{rw}: type must be one of {sorted(TYPES)}")
        words, _signed = TYPES[rtype]
        for a in range(address, address + words):
            if covered(space, a, exclude_probe=True):
                continue
            if covered(space, a):
                raise ProfileError(
                    f"{rw}: register {a} ({space}) is only inside a probe block. A probe "
                    f"block maps nothing and its read failures are kept out of the RS485 bus "
                    f"counters, so mapping a measurement into one would hide real bus errors "
                    f"on a range this profile actually depends on"
                )
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
        check_finite(float(scale), rw, "scale")
        unit = _require(r, "unit", str, rw)
        if unit not in UNITS:
            raise ProfileError(f"{rw}: unknown unit '{unit}'; known: {sorted(UNITS)}")
        # value = raw * scale + offset. Zero for almost every register; not zero for the ones
        # that store a biased temperature so it never goes negative on the wire.
        offset = r.get("offset", 0.0)
        if isinstance(offset, bool) or not isinstance(offset, (int, float)):
            raise ProfileError(f"{rw}: offset must be a number")
        check_finite(float(offset), rw, "offset")
        # A raw value meaning "not available". Some vendors answer an unavailable channel with a
        # sentinel rather than an exception, and decoding that as a number publishes a sleeping
        # inverter at 3276.7 degrees.
        invalid = r.get("invalid")
        if invalid is not None:
            if isinstance(invalid, bool) or not isinstance(invalid, int):
                raise ProfileError(f"{rw}: invalid must be an integer raw value")
            # Zero is refused outright. It is a legitimate reading almost everywhere in this
            # domain -- AC power at night, battery current at rest, a string in the dark -- so a
            # sentinel of 0 would permanently and silently suppress real zeroes, which is the one
            # value this project is most careful never to fabricate OR discard.
            if invalid == 0:
                raise ProfileError(
                    f"{rw}: invalid must not be 0 -- zero is a real reading on almost every "
                    f"channel, and a sentinel of 0 would suppress it forever"
                )
            limit = 0xFFFF if words == 1 else 0xFFFFFFFF
            if not 0 <= invalid <= limit:
                raise ProfileError(
                    f"{rw}: invalid must fit the register width (0-{limit} for '{rtype}')"
                )
        word_order = r.get("word_order", "high_first")
        if word_order not in WORD_ORDERS:
            raise ProfileError(f"{rw}: word_order must be one of {sorted(WORD_ORDERS)}")
        # Refused rather than ignored on a 16-bit row: a word order on a single register is a
        # sign the author believes the value spans two, and silently accepting it would leave
        # that misunderstanding in the file looking honoured.
        if word_order != "high_first" and words != 2:
            raise ProfileError(
                f"{rw}: word_order applies to 32-bit values only; '{rtype}' is one register"
            )
        parsed_regs.append(
            {
                "measurement": mid,
                "display_name": name,
                "space": space,
                "address": address,
                "type": rtype,
                "scale": float(scale),
                "offset": float(offset),
                "word_order": word_order,
                "invalid": invalid,
                "unit": unit,
            }
        )

    # [[write]] rows: read-only is the default — a register is writable only when declared
    # here, and even then it stays dormant (see WriteMapping in profile_tables.h).
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
        check_text(name, ww, "display_name")
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
        check_finite(float(scale), ww, "scale")
        # Read rows may carry a negative scale -- that is how a device reporting the opposite sign
        # convention to ours gets corrected. A WRITE row may not: execute() computes
        # raw = value / scale and refuses anything below zero as out of range, so a negative scale
        # here produces a row that passes every check and then rejects every value sent to it.
        if scale < 0:
            raise ProfileError(
                f"{ww}: scale must be positive on a write row -- the write path computes "
                f"raw = value / scale and refuses a negative raw, so a negative scale makes "
                f"the row silently undispatchable (negative scale IS allowed on read rows)"
            )
        # Same reason, one step further: the inverse of raw * scale + offset is
        # (value - offset) / scale, which the write path does not implement. Accepting the key
        # here would write a value the device reads back as something else.
        if "offset" in wr:
            raise ProfileError(
                f"{ww}: offset is not supported on a write row (read rows only) -- the write "
                f"path does not invert it, so the register would receive the wrong value"
            )
        if "word_order" in wr:
            raise ProfileError(
                f"{ww}: word_order is not supported on a write row -- the write path sends a "
                f"single 16-bit register (FC06), so there is no word order to choose"
            )
        if "invalid" in wr:
            raise ProfileError(
                f"{ww}: invalid is a READ concept -- it marks a value the device reports as "
                f"unavailable, and there is nothing to skip when writing"
            )
        # Part of a register is not something FC06 can set: it writes all sixteen bits, so a
        # masked field would clear whatever shares the register with it. Several vendors do pack a
        # mode in beside unrelated flags, so the key is recognised and refused by name rather than
        # ignored -- an ignored `bitmask` would produce a row that looks masked and writes whole.
        if "bitmask" in wr:
            raise ProfileError(
                f"{ww}: bitmask is not supported -- FC06 writes the whole register, so setting "
                f"part of one needs read-modify-write, which the write path does not do. A "
                f"masked field would clear its neighbours in the same register"
            )
        verified = wr.get("verified", False)
        if not isinstance(verified, bool):
            raise ProfileError(f"{ww}: verified must be a boolean")

        row = {
            "command": cmd,
            "display_name": name,
            "address": address,
            "type": rtype,
            "function": function,
            "scale": float(scale),
            "verified": verified,
        }

        if cmd in enum_command_names:
            # A mode setpoint. Bounds, a step and a unit describe a range, and this is a list --
            # accepting them would mean publishing a range the device does not have.
            for key in ("minimum", "maximum", "step", "unit"):
                if key in wr:
                    raise ProfileError(
                        f"{ww}: '{key}' does not apply to '{cmd}', which selects from a list "
                        f"rather than moving along a range -- declare `options` instead"
                    )
            if words != 1:
                raise ProfileError(
                    f"{ww}: a mode setpoint must be a single register (u16/s16); "
                    f"'{rtype}' spans {words}"
                )
            options = wr.get("options")
            if not isinstance(options, list) or not options:
                raise ProfileError(
                    f"{ww}: '{cmd}' needs a non-empty `options` list, e.g.\n"
                    f'  options = [{{ value = 0, label = "Self-consumption" }}, '
                    f'{{ value = 2, label = "Forced" }}]\n'
                    f"  A mode write with no declared modes is refused at runtime: nothing can "
                    f"range-check it and Home Assistant would show an empty dropdown"
                )
            if len(options) > MAX_ENUM_OPTIONS:
                raise ProfileError(
                    f"{ww}: {len(options)} options, at most {MAX_ENUM_OPTIONS} are supported"
                )
            parsed_options = []
            seen_values: set[int] = set()
            seen_labels: set[str] = set()
            for j, opt in enumerate(options):
                ow = f"{ww} options[{j}]"
                if not isinstance(opt, dict):
                    raise ProfileError(
                        f"{ow}: each option must be a table with value + label"
                    )
                value = _require(opt, "value", int, ow)
                if not 0 <= value <= 0xFFFF:
                    raise ProfileError(f"{ow}: value must be 0-65535")
                if value in seen_values:
                    raise ProfileError(f"{ow}: value {value} declared twice")
                seen_values.add(value)
                label = _require(opt, "label", str, ow)
                if not label:
                    raise ProfileError(f"{ow}: label must not be empty")
                check_text(label, ow, "label")
                # Home Assistant identifies a select option BY its label, so two options sharing
                # one would give the user a dropdown where one entry silently shadows the other.
                if label in seen_labels:
                    raise ProfileError(f"{ow}: label '{label}' declared twice")
                seen_labels.add(label)
                parsed_options.append({"value": value, "label": label})
            row.update(
                {
                    "unit": None,
                    "minimum": 0.0,
                    "maximum": 0.0,
                    "step": 0.0,
                    "options": parsed_options,
                }
            )
        else:
            if "options" in wr:
                raise ProfileError(
                    f"{ww}: `options` applies only to a mode setpoint; '{cmd}' takes a number, "
                    f"so declare minimum/maximum/step instead"
                )
            unit = _require(wr, "unit", str, ww)
            if unit not in UNITS:
                raise ProfileError(
                    f"{ww}: unknown unit '{unit}'; known: {sorted(UNITS)}"
                )
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
                check_finite(float(val), ww, key)
            if not minimum < maximum:
                raise ProfileError(f"{ww}: minimum must be < maximum")
            step = wr.get("step", 1.0)
            if (
                isinstance(step, bool)
                or not isinstance(step, (int, float))
                or step <= 0
            ):
                raise ProfileError(f"{ww}: step must be a positive number")
            check_finite(float(step), ww, "step")
            row.update(
                {
                    "unit": unit,
                    "minimum": float(minimum),
                    "maximum": float(maximum),
                    "step": float(step),
                    "options": [],
                }
            )

        parsed_writes.append(row)

    return {
        "path": where,
        "id": pid,
        "display_name": display,
        "manufacturer": manufacturer,
        "status": status,
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


def check_text(value: str, where: str, key: str) -> str:
    """Refuse text that cannot survive the trip into a C++ string literal.

    TOML basic strings decode escapes, so `display_name = "Roof\\nArray"` arrives here holding a
    real newline -- which inside a "..." literal is an unterminated string and a compiler error
    two steps removed from the file that caused it. A NUL is worse: it compiles, and then
    truncates the label at runtime with nothing to show for it.

    Rejected rather than escaped. A control character in a human-readable name is a typo every
    time, and the error belongs here, where the file and the key can be named.
    """
    for ch in value:
        if ord(ch) < 0x20 or ord(ch) == 0x7F:
            raise ProfileError(
                f"{where}: '{key}' contains a control character (0x{ord(ch):02X}); "
                f"names are single-line text"
            )
    return value


def check_finite(value: float, where: str, key: str) -> float:
    """TOML has `nan`, `inf` and `-inf` as legal float literals, and tomllib hands them straight
    over. They pass every bound check written as a comparison -- nan compares false against
    everything -- and then cpp_float() emits `nan.0` or a bare `inf`, neither of which is a C++
    float literal. The build fails somewhere unrecognisable instead of here."""
    if value != value or value in (float("inf"), float("-inf")):
        raise ProfileError(f"{where}: '{key}' must be a finite number")
    return value


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
    w("// Emitted by tools/gen_profiles.py from profiles/*/*.toml. To change a")
    w("// register map, edit the TOML and rebuild; this file is regenerated pre-build")
    w("// and is not committed. See docs/adding-a-device.md.")
    w("")
    w("#include <cstring>")
    w("")
    w('#include "drivers/modbus_profile/profile_tables.h"')
    w("")
    w("namespace heliograph::profile {")
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
                f"{cpp_float(r['scale'])}, {'true' if signed else 'false'}, "
                f"{cpp_float(r['offset'])}, {WORD_ORDERS[r['word_order']]}, "
                f"{'true' if r['invalid'] is not None else 'false'}, "
                f"{r['invalid'] if r['invalid'] is not None else 0}}},"
            )
        w("};")
        w(f"constexpr RegBlock k{sym}Blocks[] = {{")
        for b in p["blocks"]:
            probe = "true" if b["probe"] else "false"
            w(
                f"    {{RegSpace::{SPACES[b['space']]}, {b['start']}, "
                f"{b['count']}, {probe}}},"
            )
        w("};")
        if p["writes"]:
            # Option tables first: each write row points at its own, and a table has to exist
            # before the row that references it.
            for wr in p["writes"]:
                if not wr["options"]:
                    continue
                osym = f"k{sym}{cpp_symbol(wr['command'])}Options"
                w(f"constexpr EnumOption {osym}[] = {{")
                for opt in wr["options"]:
                    w(f"    {{{opt['value']}, {cpp_string(opt['label'])}}},")
                w("};")
            w(f"constexpr WriteMapping k{sym}Writes[] = {{")
            for wr in p["writes"]:
                words, _signed = TYPES[wr["type"]]
                # A mode row has no unit: it selects from a list rather than measuring anything.
                unit_enum = "None" if wr["unit"] is None else UNITS[wr["unit"]][0]
                multiple = "true" if wr["function"] == "write_multiple" else "false"
                w(
                    f"    {{InverterCommandType::{commands[wr['command']]}, "
                    f"{cpp_string(wr['display_name'])},"
                )
                w(
                    f"     RegSpace::Holding, {wr['address']}, {words}, {multiple}, "
                    f"{cpp_float(wr['scale'])},"
                )
                if wr["options"]:
                    osym = f"k{sym}{cpp_symbol(wr['command'])}Options"
                    w(
                        f"     {cpp_float(wr['minimum'])}, {cpp_float(wr['maximum'])}, "
                        f"{cpp_float(wr['step'])}, Unit::{unit_enum}, "
                        f"{'true' if wr['verified'] else 'false'},"
                    )
                    w(f"     {osym}, sizeof({osym}) / sizeof({osym}[0])}},")
                else:
                    w(
                        f"     {cpp_float(wr['minimum'])}, {cpp_float(wr['maximum'])}, "
                        f"{cpp_float(wr['step'])}, Unit::{unit_enum}, "
                        f"{'true' if wr['verified'] else 'false'}}},"
                    )
            w("};")
    w("")
    w("constexpr DeviceProfile kProfiles[] = {")
    for p in profiles:
        sym = cpp_symbol(p["id"])
        w(
            f"    {{{cpp_string(p['id'])}, {cpp_string(p['display_name'])}, "
            f"{cpp_string(p['manufacturer'])},"
        )
        w(f"     {'true' if p['battery'] else 'false'}, {p['phases']}, {p['mppts']},")
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
        status = f"ProfileStatus::{PROFILE_STATUSES[p['status']]}"
        if p["serial"]:
            s = p["serial"]
            w("     /*hasSerial=*/true,")
            w(
                f"     SerialProfile{{{s['baud']}, SerialParity::{PARITIES[s['parity']]}, "
                f"8, {s['stop_bits']}, 1000, 3}},"
            )
        else:
            w("     /*hasSerial=*/false, SerialProfile{},")
        w(f"     {status}}},")
    w("};")
    w("")
    w("}  // namespace")
    w("")
    w("const DeviceProfile* findProfile(const char* id) {")
    w("    if (id == nullptr) {")
    w("        return nullptr;")
    w("    }")
    w("    for (const DeviceProfile& p : kProfiles) {")
    w("        if (std::strcmp(p.id, id) == 0) {")
    w("            return &p;")
    w("        }")
    w("    }")
    w("    return nullptr;")
    w("}")
    w("")
    default_index = next(i for i, p in enumerate(profiles) if p["default"])
    w(f"// [profile] default = true in {profiles[default_index]['path']}.")
    w(f"const DeviceProfile& defaultProfile() {{ return kProfiles[{default_index}]; }}")
    w("")
    w("size_t profileCount() { return sizeof(kProfiles) / sizeof(kProfiles[0]); }")
    w("")
    w("const DeviceProfile& profileAt(size_t index) {")
    w("    return kProfiles[index < profileCount() ? index : 0];")
    w("}")
    w("")
    w("}  // namespace heliograph::profile")
    w("")
    return "\n".join(lines)


def run(check_only: bool = False) -> int:
    vocabulary = load_vocabulary()
    commands = load_command_vocabulary()
    # commandTakesEnumValue names C++ ENUMERATORS; a profile names commands. Translate through
    # the command vocabulary rather than comparing the two spellings, which do not match.
    enum_enumerators = load_enum_command_names()
    enum_commands = {n for n, enum in commands.items() if enum in enum_enumerators}
    paths = sorted(
        p for p in PROFILES_DIR.rglob("*.toml") if not p.name.startswith("_")
    )
    profiles: list[dict] = []
    errors: list[str] = []
    for path in paths:
        try:
            profiles.append(parse_profile(path, vocabulary, commands, enum_commands))
        except ProfileError as e:
            errors.append(str(e))
        except tomllib.TOMLDecodeError as e:
            errors.append(f"{path.relative_to(ROOT)}: TOML syntax error: {e}")

    if not errors:
        ids = [p["id"] for p in profiles]
        for dup in {i for i in ids if ids.count(i) > 1}:
            errors.append(f"profile id '{dup}' is defined in more than one file")
        # Two DIFFERENT ids can still collapse to one C++ identifier: cpp_symbol() splits on
        # underscores and capitalises, so `a_b` and `a__b` both become `AB`. The check above
        # compares the ids as written and sees no duplicate, and the collision then surfaces as a
        # redefinition error deep in a generated file -- which is exactly the sort of failure this
        # script exists to turn into a sentence naming the file that caused it.
        symbols: dict[str, str] = {}
        for p in profiles:
            sym = cpp_symbol(p["id"])
            if sym in symbols and symbols[sym] != p["id"]:
                errors.append(
                    f"profile ids '{symbols[sym]}' and '{p['id']}' both generate the C++ "
                    f"symbol '{sym}'; give one of them a distinct id"
                )
            symbols[sym] = p["id"]
        defaults = [p for p in profiles if p["default"]]
        if not profiles:
            errors.append(
                f"no profiles found under {PROFILES_DIR.relative_to(ROOT)}/ "
                f"-- the profile-driven driver needs at least one"
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
