#!/usr/bin/env python3
"""Self-check for gen_profiles.py's read-back rule (docs/device-profiles/write-path.md).

No shipped profile sets `verified = true`, so the refusal of a verified row without a read-back
is otherwise never executed. This parses the MIC TL-X profile three ways, with the TOML edited
in memory -- nothing is written to the tree:

  1. as shipped, but verified          -> accepted, has_readback
  2. read-back row removed, verified   -> refused
  3. read-back row removed, unverified -> accepted, has_readback false (dormant, never served)

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
    assert not dormant["has_readback"], "an unverified row without a read-back stays dormant"
    print("read-back rule: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
