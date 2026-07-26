#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Syntax-checks the JavaScript embedded in the web asset headers with `node --check`.
#
# The pages ship as C++ string literals, so a JS syntax error survives compilation and only
# surfaces as a silently broken page on the device. This extracts every <script> block and
# lets node parse it; run locally before flashing and in CI on every push.

import json
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
                continue
            if name.endswith("index_html.h"):
                status |= check_version_compare(path.read_text(), scratch)
    return status


# The version comparison decides whether anyone is ever told an update exists, and every way
# it can be wrong is quiet: compare as strings and "0.9.0" sorts above "0.14.0", so the page
# nags forever about a downgrade; mishandle the build stamp the firmware appends to its own
# version and it never fires at all. Neither surfaces as an error.
#
# It lives only in the page -- the firmware has no use for it -- so this is where it gets
# tested. The two functions stand alone and reference nothing else on the page, which is what
# makes running them in isolation honest rather than a re-implementation.
VERSION_CASES = [
    # (current, candidate, expected isNewer)
    ("0.9.0", "0.14.0", True),  # the trap: as text, "0.9.0" sorts higher
    ("0.14.0", "0.9.0", False),
    ("0.14.0", "0.14.1", True),
    ("0.14.0", "1.0.0", True),
    ("0.14.0", "0.14.0", False),
    # What the bridge actually reports about itself, stamp and all.
    ("0.14.0 (Jul 26 2026 17:31:45)", "0.15.0", True),
    ("0.14.0 (Jul 26 2026 17:31:45)", "0.14.0", False),
    ("0.14.0 (Jul 26 2026 17:31:45)", "v0.14.0", False),
    # A feed replaced by something else -- an error page, a captive portal -- says nothing
    # rather than something nobody can trust.
    ("0.14.0", "latest", False),
    ("0.14.0", "", False),
    ("", "0.15.0", False),
    ("0.14.0", "<!DOCTYPE html>", False),
    # A four-part scheme is not one we understand; reading it as three would make x.y.z.4 and
    # x.y.z.5 compare equal.
    ("1.2.3.4", "1.2.3.5", False),
]


def extract_function(source, name):
    """Pulls one top-level `function name(...){...}` out by brace matching."""
    start = source.find(f"function {name}(")
    if start < 0:
        return None
    depth = 0
    for i in range(source.index("{", start), len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start : i + 1]
    return None


def check_version_compare(script, scratch):
    parts = []
    for fn in ("semver", "isNewer"):
        body = extract_function(script, fn)
        if body is None:
            print(f"FAIL: {fn}() not found in index_html.h; this check needs updating")
            return 1
        parts.append(body)
    cases = json.dumps(VERSION_CASES)
    harness = (
        "\n".join(parts)
        + f"""
let bad = 0;
for (const [current, candidate, want] of {cases}) {{
  const got = isNewer(current, candidate);
  if (got !== want) {{
    console.error(`isNewer(${{JSON.stringify(current)}}, ${{JSON.stringify(candidate)}}) = ${{got}}, want ${{want}}`);
    bad++;
  }}
}}
process.exit(bad === 0 ? 0 : 1);
"""
    )
    path = pathlib.Path(scratch) / "version_compare.js"
    path.write_text(harness)
    result = subprocess.run(["node", str(path)], capture_output=True, text=True)
    print(f"version comparison: {'OK' if result.returncode == 0 else 'FAIL'}")
    if result.returncode != 0:
        print(result.stdout + result.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
