#!/usr/bin/env python3
"""Self-check for gen_profiles.py's `byte` key (docs/device-profiles/schema.md).

A profile that uses the key proves the accepted case and nothing else, so every refusal below is
otherwise never executed. Each case parses a shipped profile with one row edited in memory --
nothing is written to the tree -- and a refusal must name the key, so a row that fails for some
other reason does not pass as a refusal of this one.

Run by tools/check_layering.sh.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_profiles
from check_readback_rule import parse


def row(data: dict, rtype: str) -> dict:
    """The first plain numeric row of that type: no sentinel of its own to get in the way."""
    return next(
        r for r in data["register"] if r["type"] == rtype and "invalid" not in r
    )


def edit_row(rtype: str = "u16", **keys):
    return lambda data: row(data, rtype).update(keys)


def flag_row(data: dict) -> None:
    base = row(data, "u16")
    data["register"].append(
        {
            "measurement": "grid.connected",
            "display_name": "Grid Connected",
            "space": base["space"],
            "address": base["address"],
            "type": "u16",
            "bit": 2,
            "byte": "low",
        }
    )


REFUSED = {
    "a value that is not low or high": edit_row(byte="middle"),
    "a value that is not a string": edit_row(byte=1),
    "a signed register": edit_row(byte="low", type="s16"),
    "a 32-bit register": edit_row("u32", byte="high"),
    "a bit row": flag_row,
    "a sentinel wider than a byte": edit_row(byte="low", invalid=0x100),
    "a write row": lambda data: data["write"][0].update(byte="low"),
}


def main() -> int:
    for half, enumerator in gen_profiles.BYTES.items():
        profile = parse(edit_row(byte=half, invalid=0xFF, scale=0.5))
        rows = [r for r in profile["registers"] if r["byte"] is not None]
        assert [r["byte"] for r in rows] == [half], rows
        # generate() wants a default profile, and this is the only one it is given.
        profile["default"] = True
        emitted = gen_profiles.generate(
            [profile],
            gen_profiles.load_vocabulary(),
            gen_profiles.load_command_vocabulary(),
        )
        assert emitted.count(f"RegByte::{enumerator}}}") == 1, (
            f"byte = {half!r} must reach the generated table as RegByte::{enumerator}"
        )
    for what, edit in REFUSED.items():
        try:
            parse(edit)
        except gen_profiles.ProfileError as e:
            assert "byte" in str(e), f"{what}: refused for another reason: {e}"
        else:
            raise AssertionError(f"byte on {what} must be refused")
    print(f"byte rule: OK ({len(REFUSED)} refusals)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
