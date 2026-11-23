#!/usr/bin/env python3
"""Self-check for gen_profiles.py's unknown-key refusal (docs/device-profiles/schema.md).

Every shipped profile spells its keys correctly, so the refusal is otherwise never executed --
and a refusal that quietly stopped firing would look exactly like a tree full of valid
profiles. This parses the MIC TL-X profile with the TOML edited in memory (nothing is written to
the tree), first grown to carry every section the schema has, then once per section with one
misspelt key:

  1. every section present, spelt correctly      -> accepted
  2. one misspelt key in one section, per section -> refused, naming that key
  3. a key refused by name (bitmask, offset, ...) -> still refused with its own reason

The template is parsed too: it is skipped by --check, and it is the file people copy.

Run by tools/check_layering.sh.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_profiles
from check_readback_rule import parse


def full(data: dict) -> None:
    """Add the sections the shipped profile does not use, so each one has a row to misspell."""
    data["profile"]["transports"] = ["rtu", "tcp"]
    data["tcp"] = {"port": 502}
    data["status_register"] = {"space": "input", "address": 0, "values": {"1": "On"}}
    data["register"].append(
        {
            "measurement": "grid.connected",
            "display_name": "Grid Connected",
            "space": "holding",
            "address": 40,
            "type": "u16",
            "bit": 2,
        }
    )
    data["write"].append(
        {
            "command": "set_battery_operating_mode",
            "display_name": "Mode",
            "space": "holding",
            "address": 41,
            "type": "u16",
            "options": [{"value": 0, "label": "Auto"}],
        }
    )


# Section -> (the table to put a key in, a misspelling of one of its keys).
MISSPELT = {
    "top level": (lambda d: d, "registers"),
    "[profile]": (lambda d: d["profile"], "phase"),
    "[serial]": (lambda d: d["serial"], "baudrate"),
    "[tcp]": (lambda d: d["tcp"], "Port"),
    "[[block]]": (lambda d: d["block"][0], "probed"),
    "[[register]]": (lambda d: d["register"][0], "scal"),
    "[[register]] bit row": (lambda d: d["register"][-1], "bits"),
    "[status_register]": (lambda d: d["status_register"], "value"),
    "[[write]]": (lambda d: d["write"][0], "minimun"),
    "[[write]] option": (lambda d: d["write"][-1]["options"][0], "name"),
}

# Keys that are recognised and refused with their own reason. They must not be swallowed by the
# generic message, which would say "unknown" about a key the author had a reason to write.
NAMED = {
    "bitmask on a read row": (lambda d: d["register"][0], "bitmask", "bitmask is not"),
    "scale on a bit row": (lambda d: d["register"][-1], "scale", "takes no scale"),
    "offset on a write row": (lambda d: d["write"][0], "offset", "offset is not"),
    "bit on a write row": (lambda d: d["write"][0], "bit", "bit/bitmask is not"),
}


def refusal(table, key: str) -> str:
    def edit(data: dict) -> None:
        full(data)
        table(data)[key] = 1

    try:
        parse(edit)
    except gen_profiles.ProfileError as e:
        return str(e)
    return ""


def main() -> int:
    parsed = parse(full)
    assert parsed["status_register"] and parsed["writes"][-1]["options"], parsed
    for section, (table, key) in MISSPELT.items():
        message = refusal(table, key)
        assert f"unknown key '{key}'" in message, (
            f"{section}: '{key}' must be refused by name, got: {message or 'accepted'}"
        )
    assert "did you mean 'scale'?" in refusal(*MISSPELT["[[register]]"])
    for case, (table, key, reason) in NAMED.items():
        message = refusal(table, key)
        assert reason in message and "unknown key" not in message, (
            f"{case}: must keep its own message, got: {message or 'accepted'}"
        )

    commands = gen_profiles.load_command_vocabulary()
    enums = gen_profiles.load_enum_command_names()
    gen_profiles.parse_profile(
        gen_profiles.PROFILES_DIR / "_template.toml",
        gen_profiles.load_vocabulary(),
        commands,
        {n for n, e in commands.items() if e in enums},
    )
    print("unknown-key refusal: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
