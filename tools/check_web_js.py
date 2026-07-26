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
    # One scratch directory for the whole run, removed on the way out. The previous form used
    # NamedTemporaryFile(delete=False) and never unlinked, so every local run and every CI run
    # left a .js file behind in the system temp directory.
    with tempfile.TemporaryDirectory(prefix="heliograph-js-") as scratch:
        for name in ASSETS:
            source = (root / name).read_text()
            scripts = re.findall(r"<script>(.*?)</script>", source, re.S)
            if not scripts:
                print(f"{name}: FAIL (no <script> blocks found)")
                status = 1
                continue
            path = pathlib.Path(scratch) / (pathlib.Path(name).stem + ".js")
            path.write_text("\n".join(scripts))
            try:
                result = subprocess.run(
                    ["node", "--check", str(path)], capture_output=True, text=True
                )
            except FileNotFoundError:
                # Without this the script dies on a bare traceback that names no cause. The
                # check is genuinely unrunnable, so say which tool is missing and fail --
                # skipping silently would report a clean tree that was never parsed.
                print(
                    "FAIL: node is not on PATH; install Node.js to syntax-check the web assets"
                )
                return 1
            print(f"{name}: {'OK' if result.returncode == 0 else 'FAIL'}")
            if result.returncode != 0:
                print(result.stderr)
                status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
