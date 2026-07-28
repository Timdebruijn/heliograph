#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Every canonical measurement id must appear in measurement_id::kAll.
#
# kAll exists so the firmware can enumerate the topics a removed device could have published,
# and clear the retained Home Assistant discovery configs it left behind. A constant that is
# declared and not listed there is silently un-clearable: the entity stays in Home Assistant
# reporting online forever. Nothing in C++ can reflect over the constants, so this checks it.

import pathlib
import re
import sys


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    text = (root / "src/device/measurement.h").read_text()
    block = re.search(
        r"namespace measurement_id \{(.*?)\}\s*//\s*namespace measurement_id",
        text,
        re.DOTALL,
    )
    if not block:
        print("FAIL: measurement_id namespace not found")
        return 1
    body = block.group(1)
    # `const char *k` and `const char* k` are the same declaration; so is one without `inline`.
    # The strict spelling was the whole guard, and there is no .clang-format to enforce it, so a
    # constant one space away was invisible here and un-clearable in the firmware (review).
    declared = set(
        re.findall(r"(?:inline\s+)?constexpr\s+const\s+char\s*\*\s*(k\w+)\s*=", body)
    )
    declared.discard("kAll")
    listed_block = re.search(r"kAll\[\] = \{(.*?)\};", body, re.DOTALL)
    if not listed_block:
        print("FAIL: measurement_id::kAll not found")
        return 1
    listed = set(re.findall(r"\b(k\w+)\b", listed_block.group(1)))
    missing = sorted(declared - listed)
    extra = sorted(listed - declared)
    if missing:
        print("FAIL: not listed in measurement_id::kAll: " + ", ".join(missing))
    if extra:
        print("FAIL: listed in kAll but not declared: " + ", ".join(extra))
    if missing or extra:
        return 1
    print(f"measurement_id::kAll: OK ({len(listed)} ids)")
    return check_dashboard_labels(root, body)


def check_dashboard_labels(root: pathlib.Path, body: str) -> int:
    """Every canonical id must have a row in the dashboard's MEAS table.

    The dashboard renders one row per measurement the payload carries, looking each id up in
    that table for its label, its group and how many decimals it is worth. An id declared in
    C++ and missing there does not fail anywhere -- it simply never appears on the page, which
    is the bug this whole change was made to fix, arriving again through the back door.
    """
    ids = set(re.findall(r'=\s*"([a-z0-9_.]+)"', body))
    # control.* are SETPOINTS, not readings. They are in kAll so a removed device's retained
    # Home Assistant configs can be cleared, but no driver reports them as a measurement, so
    # there is nothing for the dashboard to show and a row here would be a promise of a tile
    # that can never appear.
    ids = {i for i in ids if not i.startswith("control.")}
    page = (root / "src/web/assets/index_html.h").read_text()
    table = re.search(r"const MEAS=\[(.*?)\n\];", page, re.DOTALL)
    if not table:
        print("FAIL: the dashboard's MEAS table was not found")
        return 1
    labelled = set(re.findall(r"\['([a-z0-9_.]+)'", table.group(1)))
    missing = sorted(ids - labelled)
    unknown = sorted(labelled - ids)
    if missing:
        print("FAIL: no dashboard row for: " + ", ".join(missing))
    if unknown:
        print("FAIL: dashboard row for an id that does not exist: " + ", ".join(unknown))
    if missing or unknown:
        return 1
    print(f"dashboard measurement rows: OK ({len(labelled)} ids)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
