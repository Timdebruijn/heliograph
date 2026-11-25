#!/usr/bin/env python3
"""Self-check for gen_profiles.py's read-back rule (docs/device-profiles/write-path.md).

No shipped profile sets `verified = true`, so the refusal of a verified row without a read-back
is otherwise never executed. This parses the MIC TL-X profile three ways, with the TOML edited
in memory -- nothing is written to the tree:

  1. as shipped, but verified          -> accepted, has_readback
  2. read-back row removed, verified   -> refused
  3. read-back row removed, unverified -> accepted, has_readback false (dormant, never served)
  4. read-back from another register (address, space, scale or type), verified -> refused:
     a reading of a different register does not witness the one being written
  5. a range that does not fit the register (s16 overflow) or starts below zero -> refused

Run by tools/check_layering.sh.
"""

from __future__ import annotations

import copy
import sys
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_profiles

PROFILE = gen_profiles.ROOT / "profiles" / "growatt" / "mic_tl_x.toml"
READBACK = gen_profiles.READBACK["set_active_power_limit_percent"]


def parse(edit) -> dict:
    with PROFILE.open("rb") as f:
        data = tomllib.load(f)
    edit(data)

    class _Shim:  # hands parse_profile the edited table instead of the file's
        @staticmethod
        def load(_f):
            return copy.deepcopy(data)

    real, gen_profiles.tomllib = gen_profiles.tomllib, _Shim
    try:
        commands = gen_profiles.load_command_vocabulary()
        enums = gen_profiles.load_enum_command_names()
        return gen_profiles.parse_profile(
            PROFILE,
            gen_profiles.load_vocabulary(),
            commands,
            {n for n, e in commands.items() if e in enums},
        )
    finally:
        gen_profiles.tomllib = real


def verified(data: dict) -> None:
    data["write"][0]["verified"] = True


def no_readback(data: dict) -> None:
    data["register"] = [r for r in data["register"] if r["measurement"] != READBACK]


def readback_row(data: dict) -> dict:
    return next(r for r in data["register"] if r["measurement"] == READBACK)


def refused(edit, needle: str, why: str) -> None:
    try:
        parse(edit)
    except gen_profiles.ProfileError as e:
        assert needle in str(e), f"{why}: refused for the wrong reason: {e}"
    else:
        raise AssertionError(why)


def main() -> int:
    ok = parse(verified)["writes"][0]["has_readback"]
    assert ok, "a verified row WITH a read-back must be accepted and flagged"
    try:
        parse(lambda d: (no_readback(d), verified(d)))
    except gen_profiles.ProfileError as e:
        assert "read-back" in str(e), e
    else:
        raise AssertionError("a verified row WITHOUT a read-back must be refused")
    dormant = parse(no_readback)["writes"][0]
    assert not dormant["has_readback"], (
        "an unverified row without a read-back stays dormant"
    )

    # 4. The read-back must be the written register, read the same way.
    def moved(key, value):
        def edit(d):
            readback_row(d)[key] = value
            verified(d)

        return edit

    write = tomllib.load(PROFILE.open("rb"))["write"][0]
    for key, value in (
        ("address", write["address"] + 1),
        ("space", "input"),
        ("scale", 0.1),
        ("type", "s16" if write["type"] == "u16" else "u16"),
    ):
        refused(
            moved(key, value),
            "read-back",
            f"a read-back with a different {key} must not count as one",
        )

    # 5. The range must fit the register and start at zero or above.
    def s16_overflow(d):
        d["write"][0]["type"] = "s16"
        d["write"][0]["scale"] = 0.001  # 100 % / 0.001 = 100000, past 32767
        readback_row(d)["type"] = "s16"
        readback_row(d)["scale"] = 0.001

    refused(s16_overflow, "does not fit", "an s16 range past 32767 must be refused")

    def negative_minimum(d):
        d["write"][0]["minimum"] = -10

    refused(
        negative_minimum, "minimum must be >= 0", "a negative minimum must be refused"
    )
    print("read-back rule: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
