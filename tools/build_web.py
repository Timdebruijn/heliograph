#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Turns the commented, readable web assets in src/web/assets/*.h into what the device actually
# serves: comments removed, gzip-compressed, emitted as a PROGMEM byte array.
#
# WHY THIS EXISTS
#
# The web UI is authored as one raw string literal per page, comments and all, and that is
# deliberate -- those comments are the record of which failure each line prevents, and they
# belong next to the line. But they ship, on every page load, out of a flash chip that holds
# two OTA slots.
#
# WHAT IT DOES NOT DO
#
# No minifier, no renaming, no AST. Whole-line comments and blank lines only, plus the leading
# indent. Anything cleverer would need to understand JS strings, regexes and template literals
# to stay correct, and a web UI that silently breaks in one browser is a far worse trade than
# the few kB it would win. gzip recovers most of what a minifier would anyway -- it is very
# good at repeated `document.querySelector` -- so the clever half is not worth owning.
#
# The output is NOT committed. Same rule as tools/gen_profiles.py: the authored header is the
# source of truth, and a stale generated copy in the tree is exactly the drift that a build
# step exists to prevent. platformio.ini runs this before every build.
#
# USAGE
#
#     ./tools/build_web.py        # writes src/web/assets/generated/*.h and prints the sizes

from __future__ import annotations

import contextlib
import gzip
import pathlib
import re
import shutil
import sys

ROOT = pathlib.Path.cwd()

# One header per page. The generated symbol keeps the `Gz` suffix so a route that still
# references the plain literal fails to compile rather than silently serving the wrong bytes.
PAGES = [
    ("index_html.h", "kIndexHtmlGz"),
    ("setup_html.h", "kSetupHtmlGz"),
]

RAW = re.compile(r'R"HTML\((.*?)\)HTML"', re.S)


def find_chrome() -> str | None:
    """A Chrome or Chromium binary to render with, or None.

    Lives here because this is the module every page-rendering tool already imports; it
    existed twice, in check_dashboard_layout and make_screenshots, each with its own browser
    list -- the drift that invites is two tools disagreeing about which browser they found.
    Returning None rather than raising keeps the softer contract; a caller that cannot
    continue without a browser raises at its own call site, with its own message.
    """
    for name in (
        "google-chrome",
        "google-chrome-stable",
        "chromium",
        "chromium-browser",
    ):
        found = shutil.which(name)
        if found:
            return found
    mac = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    return mac if pathlib.Path(mac).exists() else None


def set_root(root: pathlib.Path) -> None:
    """Project root. Not simply `__file__`: under PlatformIO, SCons exec()s this script
    without __file__, so the pre-script branch at the bottom sets it from the build
    environment's PROJECT_DIR instead. Same shape as tools/gen_profiles.py."""
    global ROOT
    ROOT = root


# NameError is the SCons case: exec()'d without __file__, so ROOT stays the provisional cwd
# and the PlatformIO branch below overrides it.
with contextlib.suppress(NameError):
    set_root(pathlib.Path(__file__).resolve().parent.parent)


def assets() -> pathlib.Path:
    return ROOT / "src" / "web" / "assets"


def served_page(page: str = "index_html.h") -> str:
    """The page as the DEVICE SERVES IT: the literal, with the comments taken out.

    The one function every other tool should call. check_dashboard_layout.py, make_screenshots.py
    and preview_web.py each spelled out `strip_comments(extract(assets() / "index_html.h"))`,
    which is three places to keep in step with a pipeline that has changed twice already. A
    screenshot, a layout assertion and a preview of anything other than these bytes is a picture
    of something no browser receives.
    """
    return strip_comments(extract(assets() / page))


def inject_before_script(html: str, *scripts: str) -> str:
    """Splices <script> blocks in AHEAD of the page's own.

    Ahead, not after, and that ordering is the whole point: the injected stub replaces
    window.fetch, and a page that has already fired its first request answers it from the
    network instead -- which on a CI runner means a layout check that hangs, and on a desk means
    a preview looking for a bridge that is not there.

    Written once because it was written twice, in the check and in the preview server, with the
    same reasoning restated in both.
    """
    at = html.find("<script>")
    if at < 0:
        raise SystemExit(
            "no <script> in the page; the caller's assumptions need updating"
        )
    for s in scripts:
        # A `</script>` anywhere in the injected source -- including inside a JS string --
        # closes the block early, and the rest of it lands in the document as text. The page
        # then renders, wrongly, with no error anywhere. Refusing beats emitting that.
        if "</script>" in s:
            raise SystemExit("injected script contains </script> and would close early")
    block = "".join(f"<script>{s}</script>\n" for s in scripts)
    return html[:at] + block + html[at:]


def extract(path: pathlib.Path) -> str:
    """The raw string literal's contents, i.e. exactly the bytes the device serves today."""
    m = RAW.search(path.read_text(encoding="utf-8"))
    if m is None:
        raise SystemExit(f'{path.name}: no R"HTML(...)HTML" literal found')
    return m.group(1)


def strip_comments(src: str) -> str:
    """Drop whole-line comments, blank lines and leading indent.

    Only lines whose FIRST non-space character opens a comment are removed, so a trailing
    comment after code is left alone -- finding the end of one needs to know whether the `//`
    is inside a string, which is exactly the parsing this refuses to do.

    Nothing is touched between an unbalanced backtick and its partner. The page is full of
    multi-line template literals that emit HTML, and inside one a line beginning with `//` is
    not a comment -- it is a protocol-relative URL, or a string, or the start of a regex, and
    deleting it produces a page that is syntactically fine and visibly wrong. Their indent is
    left alone too: it is part of the string's value.

    Backtick parity is a crude tracker, and deliberately so. It is counted only over lines
    that survive comment removal, so the backticks inside the comment prose above them never
    reach it -- but a backtick inside a '...' or "..." string on a kept line would still fool
    it. That failure mode is conservative in the right direction: it makes the stripper skip
    lines it could have stripped, never strip lines it should have kept.

    The two block-comment forms are tracked separately. Conflating them (one flag closed by
    either `*/` or `-->`) lets an unterminated HTML comment swallow the rest of the file.
    """
    out: list[str] = []
    block: str | None = None  # 'c' for /* */, 'h' for <!-- -->
    in_literal = False
    for line in src.split("\n"):
        if in_literal:
            # Verbatim, indent included: these bytes are string content, not source.
            out.append(line)
            if line.count("`") % 2 == 1:
                in_literal = False
            continue
        t = line.strip()
        if block == "c":
            if "*/" in t:
                block = None
            continue
        if block == "h":
            if "-->" in t:
                block = None
            continue
        if t.startswith("/*"):
            if "*/" not in t:
                block = "c"
            continue
        if t.startswith("<!--"):
            if "-->" not in t:
                block = "h"
            continue
        if t.startswith("//"):
            continue
        if not t:
            continue
        out.append(t)
        if t.count("`") % 2 == 1:
            in_literal = True
    if block is not None:
        raise SystemExit(
            "unterminated block comment -- refusing to emit a truncated page"
        )
    if in_literal:
        raise SystemExit(
            "unterminated template literal -- refusing to emit a damaged page"
        )
    return "\n".join(out) + "\n"


def sanity(original: str, stripped: str) -> None:
    """Cheap invariants. A stripped page that is subtly broken is worse than a large one.

    Not a substitute for tools/check_web_js.py, which runs the page's JS through node; this
    only catches the structural damage a line-based filter can do.
    """
    for tag in ("<script>", "</script>", "</body>", "</html>", "<style>", "</style>"):
        if original.count(tag) != stripped.count(tag):
            raise SystemExit(f"structural change: {tag} count differs")
    # A page that lost more than 70% of its bytes has lost code, not comments.
    if len(stripped) < len(original) * 0.30:
        raise SystemExit(
            f"stripped page is {len(stripped)} of {len(original)} bytes -- that is not comments"
        )


def as_c_array(name: str, data: bytes) -> str:
    rows = [
        "    " + ", ".join(f"0x{b:02x}" for b in data[i : i + 16]) + ","
        for i in range(0, len(data), 16)
    ]
    body = "\n".join(rows)
    return (
        f"inline const uint8_t {name}[] PROGMEM = {{\n{body}\n}};\n"
        f"inline constexpr size_t {name}Len = {len(data)};\n"
    )


def build(page: str, gz_symbol: str) -> tuple[int, int, int, str]:
    src = extract(assets() / page)
    stripped = strip_comments(src)
    sanity(src, stripped)
    # mtime=0 so identical input gives identical output. The generated file is not committed,
    # but a stable byte stream keeps an untouched page from dirtying the object it feeds.
    blob = gzip.compress(stripped.encode("utf-8"), compresslevel=9, mtime=0)
    header = f"""// SPDX-License-Identifier: MIT
//
// GENERATED by tools/build_web.py from {page} -- do not edit, do not commit.
// {len(src)} bytes as authored, {len(stripped)} with comments removed, {len(blob)} gzipped.
//
// Served with `Content-Encoding: gzip`. The identity copy is deliberately NOT kept as a
// fallback: every browser that can run this page accepts gzip, and keeping both spends the
// whole saving twice -- in flash, in both OTA slots.

#pragma once

#include <pgmspace.h>

#include <cstddef>
#include <cstdint>

namespace heliograph::web {{

{as_c_array(gz_symbol, blob)}
}}  // namespace heliograph::web
"""
    return len(src), len(stripped), len(blob), header


def self_test() -> None:
    """The template-literal guard, stated as the cases it exists for.

    It runs on every invocation because there is no Python test harness in this repo and a
    guard nobody executes is a comment. It costs microseconds.
    """
    # A line that opens a template literal keeps everything up to the closing backtick --
    # indent included, and `//` inside it is a URL, not a comment.
    kept = strip_comments("  const a = `<a href=\n  //host/x\n  >`;\n")
    assert "//host/x" in kept, kept
    assert "  //host/x" in kept, "indent inside a literal is string content"
    # A blank line inside a literal is content too.
    assert strip_comments("x=`a\n\nb`;\n") == "x=`a\n\nb`;\n"
    # Outside one, all three still go.
    assert strip_comments("// gone\n\n  kept();\n") == "kept();\n"
    # An unclosed literal means the tracker lost the thread; emitting would ship a page that
    # is missing whatever came after it.
    try:
        strip_comments("x=`open\n")
    except SystemExit:
        pass
    else:
        raise AssertionError("unterminated literal was not caught")


def run() -> int:
    self_test()
    out_dir = assets() / "generated"
    out_dir.mkdir(parents=True, exist_ok=True)
    before = after = 0

    for page, gz_symbol in PAGES:
        raw, stripped, packed, header = build(page, gz_symbol)
        before += raw
        after += packed
        pct = (1 - packed / raw) * 100
        print(
            f"build_web.py: {page:15} {raw:6} -> {stripped:6} -> {packed:5} gzipped (-{pct:.0f}%)"
        )

        # Write only on change, so an untouched page never dirties an mtime and triggers a
        # needless rebuild of the REST layer.
        target = out_dir / page.replace(".h", "_gz.h")
        if not target.exists() or target.read_text(encoding="utf-8") != header:
            target.write_text(header, encoding="utf-8")

    print(
        f"build_web.py: {before} bytes of page served as {after} ({before - after} saved)"
    )
    return 0


# Under PlatformIO (extra_scripts) SCons provides Import(); standalone it does not.
try:
    Import("env")  # type: ignore[name-defined]
    set_root(pathlib.Path(env["PROJECT_DIR"]))  # type: ignore[name-defined]
    if run() != 0:
        env.Exit(1)  # type: ignore[name-defined]
except NameError:
    if __name__ == "__main__":
        sys.exit(run())
