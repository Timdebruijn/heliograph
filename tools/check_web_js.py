#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Syntax-checks the JavaScript embedded in the web asset headers with `node --check`.
#
# The pages ship as C++ string literals, so a JS syntax error survives compilation and only
# surfaces as a silently broken page on the device. This extracts every <script> block and
# lets node parse it; run locally before flashing and in CI on every push.

import pathlib
import re
import subprocess
import sys
import tempfile

ASSETS = [
    "src/web/assets/index_html.h",
    "src/web/assets/setup_html.h",
]


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    status = 0
    for name in ASSETS:
        source = (root / name).read_text()
        scripts = re.findall(r"<script>(.*?)</script>", source, re.S)
        if not scripts:
            print(f"{name}: FAIL (no <script> blocks found)")
            status = 1
            continue
        with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as handle:
            handle.write("\n".join(scripts))
            path = handle.name
        result = subprocess.run(
            ["node", "--check", path], capture_output=True, text=True
        )
        print(f"{name}: {'OK' if result.returncode == 0 else 'FAIL'}")
        if result.returncode != 0:
            print(result.stderr)
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
