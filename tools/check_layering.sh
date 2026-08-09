#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Enforces the invariants that are cheap to check mechanically. It started as the two layering
# rules from docs/architecture.md and has grown one rule per class of mistake that reached the
# tree unnoticed -- each numbered check below carries the story of the one that caused it.
# Run in CI and before every phase hand-off.
set -uo pipefail

cd "$(dirname "$0")/.."
status=0

echo "==> 1. Brand-specific knowledge must live only in src/drivers/"
# Applies to comments too: the canonical model should not explain itself in terms of one
# driver, or the rule rots into "well, it is only a comment".
#
# One exemption, marked in the source with LEGACY-CONFIG-ID: a driver id that was RENAMED still
# has to be recognised when it comes back off flash, so the config migration must name it once.
# That is a dead identifier, not brand knowledge -- no register, no framing, no protocol quirk --
# and the alternative (splicing the string together to dodge this grep) would hide exactly what
# the grep is for. Requiring the marker keeps the exemption per-line and greppable, so it cannot
# quietly widen into "config may talk about brands".
marked=$(grep -rn 'LEGACY-CONFIG-ID' src/ --exclude-dir=drivers 2>/dev/null | wc -l | tr -d ' ')
if [ "$marked" -gt 1 ]; then
    # The exemption is meant to cover exactly one dead identifier. A second marker means either a
    # second rename (write the reason down and raise this number deliberately) or somebody
    # reaching for the marker to silence an unrelated brand-name leak. Either way it is a
    # decision, not something to inherit silently -- an unbounded per-line opt-out would let this
    # rule rot into "config may talk about brands", which is what the grep exists to prevent.
    echo "FAIL: $marked LEGACY-CONFIG-ID markers outside src/drivers/; exactly 1 is expected:"
    grep -rn 'LEGACY-CONFIG-ID' src/ --exclude-dir=drivers
    status=1
fi
# Keep this list in step with profiles/: two vendors were added to the tree while this grep
# still listed only the older ones, so the rule it enforces had a hole exactly where the
# newest brand knowledge was most likely to leak.
#
# It happened again, and the second time proves the first was not bad luck. A comment in the
# canonical model explained the grid sign convention by naming three meter brands -- exactly what
# the note above forbids -- and this grep passed it, because none of the three make inverters and
# none had a profile yet. A reviewer caught it; the gate could not.
#
# So the list now runs AHEAD of profiles/ rather than behind it. Everything below the inverter
# vendors is a brand this project has researched, planned for, or has hardware from, with no
# profile yet -- meters, batteries and network devices included. The cost of a name that never
# arrives is nothing; the cost of one that arrives first in a comment is a rule that quietly
# stopped applying.
if hits=$(grep -rniE 'eversolar|zeversolar|growatt|solax|deye|sunsynk|solis|goodwe|sungrow|huawei|homewizard|shelly|eastron|chint|solarmax|sputnik|omnik|trannergy|aeg|solarcube' \
        src/ --exclude-dir=drivers 2>/dev/null | grep -v 'LEGACY-CONFIG-ID'); then
    echo "FAIL: manufacturer names found outside src/drivers/:"
    echo "$hits"
    status=1
else
    echo "OK"
fi

echo "==> 2. The host-testable core must not depend on Arduino or ESP-IDF"
# These translation units are compiled by env:native. An Arduino include here does not just
# break the build, it means protocol logic has drifted into something untestable.
core_paths=(
    src/device
    src/protocols/byte_order.h
    src/diagnostics/frame_capture.h
    src/diagnostics/frame_capture.cpp
    src/protocols/modbus/modbus_rtu.h
    src/protocols/modbus/modbus_rtu.cpp
    src/protocols/pmu/pmu_protocol.h
    src/protocols/pmu/pmu_protocol.cpp
    src/protocols/pmu/pmu_transaction.h
    src/protocols/pmu/pmu_transaction.cpp
    src/network/rtc_time.h
    src/network/rtc_time.cpp
    src/drivers/eversolar_legacy/eversolar_parser.h
    src/drivers/eversolar_legacy/eversolar_parser.cpp
    src/drivers/solax_x1/solax_parser.h
    src/drivers/solax_x1/solax_parser.cpp
)
if hits=$(grep -rnE '#include[[:space:]]*[<"](Arduino|WiFi|HardwareSerial|esp_|driver/|freertos)' \
        "${core_paths[@]}" 2>/dev/null); then
    echo "FAIL: platform headers in the host-testable core:"
    echo "$hits"
    status=1
else
    echo "OK"
fi

echo "==> 3. Fixtures are in sync with their generator"
if command -v python3 >/dev/null 2>&1; then
    before=$(cat test/fixtures/eversolar_frames.h 2>/dev/null)
    python3 tools/gen_fixtures.py >/dev/null
    if [ "$before" != "$(cat test/fixtures/eversolar_frames.h)" ]; then
        echo "FAIL: test/fixtures/eversolar_frames.h is stale; commit the regenerated file"
        status=1
    else
        echo "OK"
    fi
else
    echo "SKIP: python3 not available"
fi

echo "==> 3b. The coverage matrix is in sync with the profiles"
# Same rule as the fixtures above, for the same reason: a coverage table maintained by hand goes
# wrong in the direction that matters, claiming a channel a profile stopped mapping.
if command -v python3 >/dev/null 2>&1; then
    if python3 tools/gen_coverage.py --check; then
        :
    else
        status=1
    fi
else
    echo "SKIP: python3 not available"
fi

echo "==> 4. Every REST payload builder is reachable from a route"
# A builder with no route is dead code that unit tests happily cover. Three of them shipped
# that way in Phase 7 -- buildDevicePayload, buildMeasurementsPayload and
# buildCapabilitiesPayload were fully tested and unreachable in the firmware.
unrouted=""
for fn in $(grep -oE "^bool build[A-Za-z]+" src/outputs/rest/rest_payloads.cpp | sed 's/^bool //'); do
    if ! grep -q "$fn" src/outputs/rest/rest_api.cpp; then
        unrouted="$unrouted $fn"
    fi
done
if [ -n "$unrouted" ]; then
    echo "FAIL: payload builders with no route:$unrouted"
    status=1
else
    echo "OK"
fi

echo "==> 5. setup() applies TZ before it writes its first stamped log line"
# formatLogTimestamp renders through localtime_r, so a log:: call made while TZ is still unset
# comes out in UTC with nothing marking it as such. It shipped that way in 0.13.0: the config
# line was written eleven lines above the tzset(), which nobody noticed because a COLD start
# has no valid clock there and falls back to an uptime stamp. A warm reset (OTA reboot) keeps
# the clock, so every OTA log opened with one line an hour or two in the past.
#
# Line order in setup() is the whole invariant, hence a positional check rather than a unit
# test: main.cpp is not host-compilable, and the pure formatter is already covered in
# test/test_logging.
setup_start=$(grep -n '^void setup()' src/main.cpp | head -1 | cut -d: -f1)
setup_end=$(grep -n '^void loop()' src/main.cpp | head -1 | cut -d: -f1)
if [ -z "$setup_start" ] || [ -z "$setup_end" ]; then
    echo "FAIL: could not locate setup()/loop() in src/main.cpp; this check needs updating"
    status=1
else
    # Comment lines are stripped first: prose about log:: calls is not a log:: call.
    tz_line=$(awk -v a="$setup_start" -v b="$setup_end" \
        'NR>a && NR<b && $0 !~ /^[[:space:]]*\/\// && /tzset\(\)/ {print NR; exit}' src/main.cpp)
    log_line=$(awk -v a="$setup_start" -v b="$setup_end" \
        'NR>a && NR<b && $0 !~ /^[[:space:]]*\/\// && /log::(trace|debug|info|warn|error)\(/ {print NR; exit}' \
        src/main.cpp)
    if [ -z "$tz_line" ]; then
        echo "FAIL: no tzset() in setup(); every stamped boot line would render in UTC"
        status=1
    elif [ -n "$log_line" ] && [ "$log_line" -lt "$tz_line" ]; then
        echo "FAIL: src/main.cpp:$log_line logs before the tzset() on line $tz_line;"
        echo "      that line renders in UTC after a warm reset, unmarked"
        status=1
    else
        echo "OK"
    fi
fi

echo "==> 6. Every test that is written is also registered"
# A `static void test_x()` with no matching RUN_TEST compiles, reads as coverage, and never
# runs. test_start_is_never_throttled sat that way for a while -- and it is the test asserting
# that the rate limiter can never be the reason a curtailed inverter stays curtailed, which is
# a safety property, not a nicety. Clang says "unused function" only on a fresh compile of that
# one file, so a normal incremental run never shows it.
unregistered=""
for f in test/*/test_main.cpp; do
    for fn in $(grep -oE '^static void (test_[A-Za-z0-9_]+)\(\)' "$f" | awk '{print $3}' |
                sed 's/()$//'); do
        if ! grep -q "RUN_TEST($fn)" "$f"; then
            unregistered="$unregistered $f:$fn"
        fi
    done
done
if [ -n "$unregistered" ]; then
    echo "FAIL: tests defined but never run:"
    for hit in $unregistered; do
        echo "      $hit"
    done
    status=1
else
    echo "OK"
fi

echo "==> 7. A test using std::str*/std::mem* includes <cstring>"
# Twice now a test has used std::strstr with no <cstring>: clang on macOS pulls it in through
# another header, GCC on the CI runner does not. It compiles locally, fails in CI, and the
# fix is one line -- which is exactly the kind of round trip worth spending ten lines to avoid.
missing=""
for f in test/*/test_main.cpp test/support/*.h; do
    [ -f "$f" ] || continue
    if grep -qE 'std::(strstr|strlen|strcmp|strncmp|memcpy|memcmp|memset)\(' "$f" &&
       ! grep -q '#include <cstring>' "$f"; then
        missing="$missing $f"
    fi
done
if [ -n "$missing" ]; then
    echo "FAIL: uses std::str*/std::mem* without including <cstring>:"
    for hit in $missing; do echo "      $hit"; done
    status=1
else
    echo "OK"
fi

echo "==> 8. No comment cites a line number in our own source"
# A "see main.cpp:492" is correct on the day it is written and wrong on the next edit to that
# file -- the reader then follows it to an unrelated line and either trusts the wrong code or
# stops trusting the comments. Two had already rotted by 0.15.0: main.cpp:492 pointed at the
# boot-button handler and not at the deferred-poll seam it meant. Name the function or the
# concept instead; those move with the code.
#
# Only OUR files. A pointer into a pinned library (WebRequest.cpp:114 in rest_api.h) is
# verifiable against a fixed version and does not drift under us, so it is allowed.
#
# Portability note: this deliberately avoids `find -printf` (GNU only -- it silently yields
# nothing on the macOS BSD find every local run uses, which turned the filename alternation
# into an empty group and matched every `{width:02X}` format spec in tools/).
lineref=""
for f in $(git ls-files 'src/*.cpp' 'src/*.h' 'test/*.cpp' 'test/*.h' 'tools/*.py' \
           ':!:src/web/assets/*'); do
    for cited in $(grep -oE '[A-Za-z_][A-Za-z0-9_]*\.(cpp|h|py|sh):[0-9]+' "$f" | sort -u); do
        base=${cited%%:*}
        # Ours only: a pointer into a pinned library is verifiable against a fixed version.
        if git ls-files --error-unmatch "*/$base" >/dev/null 2>&1; then
            lineref="$lineref
      $f cites $cited"
        fi
    done
done
if [ -n "$lineref" ]; then
    echo "FAIL: comment points at a line number in our own source, which will drift:"
    printf '%s\n' "$lineref" | sed '/^[[:space:]]*$/d'
    status=1
else
    echo "OK"
fi

echo "==> 9. Every documentation link points at something the public can actually open"
# A link check that only asks "does this file exist" passes on the author's machine and fails
# for everyone else: an ignored file is present locally and absent from the clone. That is
# exactly how docs/README.md shipped a link to docs/implementation-plan.md, which .gitignore
# keeps out of the repo on purpose (it is a personal tracker). Ask git, not the filesystem.
#
# Directories are checked by whether they CONTAIN a tracked file -- git tracks files, not
# directories, so `git ls-files <dir>` returning anything means the link resolves on GitHub.
# The first version of this check reported .vscode/ as broken for that reason.
if link_report=$(python3 - <<'PYEOF'
import pathlib, re, subprocess, sys

tracked = set(subprocess.run(["git", "ls-files"], capture_output=True, text=True).stdout.split())
tracked_dirs = {str(pathlib.PurePosixPath(t).parent) for t in tracked}
root = pathlib.Path(".").resolve()
bad = []


def slug(heading: str) -> str:
    """GitHub's heading anchor: lowercase, punctuation dropped, EACH space to a hyphen.

    "## 6.5 Recording a driver" -> "65-recording-a-driver": the dot vanishes rather than
    becoming a hyphen, which is exactly the kind of detail nobody gets right by hand.

    One space per hyphen, not one per RUN of spaces. "Recovery - hold BOOT" with an em dash
    leaves two spaces once the dash is dropped, and therefore two hyphens. Collapsing them was
    the first version of this function, and it reported a correct link in README.md as broken --
    a check that cries wolf gets switched off, so the slug rule has to be exact.
    """
    s = heading.strip().lstrip("#").strip().lower()
    s = re.sub(r"[^\w\s-]", "", s)
    return re.sub(r"\s", "-", s)


def anchors_in(path: pathlib.Path) -> set:
    """Heading slugs, skipping fenced code blocks.

    A hash inside a fenced block is a shell comment, not a heading. This project's own docs are
    full of them -- the capture examples start with "# Heliograph driver capture ..." -- and
    counting those as anchors makes the check fail OPEN: a link to a heading that no longer
    exists could match one of these phantoms and be waved through. A checker with false
    negatives is worse than none, because it is trusted. There were 13 such phantoms across two
    documents when this was added.

    The fence marker is built rather than written out. A literal backtick run inside this
    heredoc breaks the SHELL parser -- the heredoc sits in a $(...) substitution, where bash
    still tracks backticks as legacy command substitution even though the delimiter is quoted.
    It cost a run of this script to find out.
    """
    fence = chr(96) * 3
    found = set()
    fenced = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith(fence):
            fenced = not fenced
            continue
        if not fenced and line.startswith("#"):
            found.add(slug(line))
    return found


for doc in sorted(pathlib.Path(".").rglob("*.md")):
    if ".git" in doc.parts or ".pio" in doc.parts:
        continue
    if str(doc) not in tracked:
        continue  # only published documents can mislead a reader
    text = doc.read_text(encoding="utf-8")
    for m in re.finditer(r"\[([^\]]*)\]\(([^)#]+)(#[^)]*)?\)", text):
        target = m.group(2)
        if target.startswith(("http://", "https://", "mailto:")):
            continue
        try:
            rel = str((doc.parent / target).resolve().relative_to(root))
        except ValueError:
            continue  # outside the repo; not ours to judge
        line_no = text[: m.start()].count(chr(10)) + 1
        if rel not in tracked and rel not in tracked_dirs:
            bad.append(f"{doc}:{line_no} -> {target}")
            continue
        # The ANCHOR, not just the file. A link to a real document at a heading that no longer
        # exists lands the reader at the top of it with no sign anything is wrong -- which is
        # worse than a 404, because it looks like it worked. This rule was written checking only
        # the path, and a renamed heading in the very same commit slipped straight through it
        # (found by review, 2026-08-02).
        anchor = (m.group(3) or "")[1:]
        if anchor and rel.endswith(".md") and rel in tracked:
            if anchor not in anchors_in(root / rel):
                bad.append(f"{doc}:{line_no} -> {target} (no such heading)")

if bad:
    print("\n".join(bad))
    sys.exit(1)
PYEOF
    ); then
    echo "OK"
else
    echo "FAIL: documentation links to files that are not in the repository:"
    echo "$link_report"
    status=1
fi

echo "==> 10. The ESP32 platform version is written the way the release tag is written"
# pioarduino zero-pads the middle component: 55.03.39, 55.03.311. That is the release tag, and
# it is also the `version` in the platform's own platform.json, so it is what the download URL
# needs and what `pio pkg list` prints. Drop the zero and you get a string that matches nothing:
# no tag, no package, a 404 for anyone who pastes it into the URL.
#
# It happened. Four occurrences across platformio.ini and docs/decisions.md sat in the tree
# through a full review round, mixed in with correctly written ones in the same paragraph --
# which is why a human diff does not catch this and a grep does.
#
# Deliberately matches the SHAPE (55.<one digit>.<digit>) and not a list of known-bad versions, so
# the next release is covered without editing this rule.
#
# The leading guard is not decoration. Without it the pattern matches the tail of every IPv4
# netmask in the tree -- 255.0.0.0 contains "55.0." -- and the first run of this rule failed on
# five lines of ipv4.h and its tests. A digit or a dot in front means it is part of a longer
# number, not a platform version.
if padding=$(git grep -nE '(^|[^0-9.])55\.[0-9]\.[0-9]' -- ':!:.pio' 2>/dev/null) && [ -n "$padding" ]; then
    echo "FAIL: platform version is missing the zero padding pioarduino's tags use:"
    echo "$padding"
    status=1
else
    echo "OK"
fi

echo "==> 11. A bare-string REST route never swallows another route"
# A bare-string URI gets Type::BackwardCompatible in ESPAsyncWebServer -- `_value == path ||
# path.startsWith(_value + "/")`. NOT the type its own AsyncURIMatcher::prefix() gives you, which
# is looser still (a bare startsWith, so it would match /api/v1/captureXYZ too). So a bare
# "/api/v1/capture" answers /api/v1/capture/driver and every other path beneath it, and whichever
# route was registered FIRST wins.
#
# That is not hypothetical. 0.26.0 shipped GET /api/v1/capture/driver, documented it, and
# rendered a UI page against it. Every driver capture ran correctly on the bus and then answered
# with the PASSIVE report: status "idle", zero frames, no sent/received fields at all. The bridge
# log said "6 frames (3 sent, 3 received), 198 bytes" in the same minute the endpoint said
# nothing had happened. Host tests, four review agents and two automated review passes all missed
# it, because the routing lives in the 97% of rest_api.cpp that is inside `#if defined(ESP32)`.
#
# The file already knew this. The note above /api/v1/devices tells the same story about a
# different route and reaches for AsyncURIMatcher::exact, which is the actual fix -- a route that
# should answer one URL says so. The capture route was added as a bare string three hundred lines
# below that note.
#
# THREE REGISTRATION FORMS, and only one of them is dangerous:
#
#   g_server->on("/x", ...)                       bare string -> matches /x AND /x/anything
#   g_server->on(AsyncURIMatcher::exact("/x"))    exact       -> matches /x only, swallows nothing
#   g_server->on(AsyncURIMatcher::prefix("/x/"))  prefix      -> deliberate, e.g. /api/v1/devices/<id>
#
# So the rule is not "no route is a prefix of another" -- that would condemn the devices routes,
# which are correct by design. It is: a BARE STRING route must not be a prefix of any other
# registered path. Exact routes are listed too, because they are what can be swallowed.
#
# AsyncEventSource paths are collected as well (it is a handler with a URI like any other), which
# is the false negative an automated review pointed out in the first version of this rule.
# stderr folded in: without it a traceback -- a moved file, a syntax slip in the block below --
# goes to the terminal while $route_report stays empty, so the FAIL prints a heading and no
# reason. A check that fails without saying why gets stared at and then ignored.
if route_report=$(python3 - 2>&1 <<'PYEOF'
import re, sys

src = open("src/outputs/rest/rest_api.cpp", encoding="utf-8").read()

# Parsed from the whole file rather than line by line. The first version of this rule grepped
# `g_server->on("..."` on one line and silently missed /api/v1/provision and /api/v1/ota, whose
# URI sits on the continuation line -- a check that reports OK while not looking is the failure
# mode this rule exists to prevent.
#
# The METHOD is captured too, and that is not decoration. canHandle() tests
# `_method.matches(request->method())` BEFORE the URI, so GET /x and POST /x/y never collide.
# Ignoring that would fail the build on a pair this project is very likely to write -- it already
# pairs GET resources with POST siblings all over /api/v1/actions/.
BARE = re.compile(r'->on\(\s*"([^"]+)"\s*,\s*(HTTP_\w+)')
MATCHED = re.compile(r'AsyncURIMatcher::(?:exact|prefix)\(\s*"([^"]+)"\s*\)\s*,\s*(HTTP_\w+)')
EVENTS = re.compile(r'AsyncEventSource\(\s*"([^"]+)"')

bare = set(BARE.findall(src))
every = bare | set(MATCHED.findall(src)) | {(p, "HTTP_GET") for p in EVENTS.findall(src)}

# A rule that stops finding routes must say so rather than pass. Renaming g_server, or moving
# every route to a helper, would otherwise turn this into a green check over an empty set.
if len(every) < 20:
    print(f"      only {len(every)} routes found; this rule has stopped seeing them")
    sys.exit(1)

def overlaps(m1, m2):
    return m1 == m2 or "HTTP_ANY" in (m1, m2)

bad = []
for a, ma in sorted(bare):
    # "/" swallows nothing: reaching a subpath would take startsWith("//").
    if a == "/":
        continue
    for b, mb in sorted(every):
        if b != a and b.startswith(a + "/") and overlaps(ma, mb):
            bad.append(f"      {a} [{ma}]  swallows  {b} [{mb}]")

if bad:
    print("\n".join(bad))
    sys.exit(1)
PYEOF
    ); then
    echo "OK"
else
    echo "FAIL: a bare-string route matches paths that belong to another route."
    echo "      Registration order decides which one answers. Use AsyncURIMatcher::exact():"
    echo "$route_report"
    status=1
fi
# KNOWN LIMIT, stated because a silent one is what got us here: the relay routes are built at
# runtime (`/api/v1/relays/<n>/set`, from a std::string), so no static reading finds them. A bare
# string "/api/v1/relays" added later would swallow all eight and this rule would say OK.

echo
if [ $status -eq 0 ]; then
    echo "RESULT: PASS"
else
    # A single unambiguous verdict on the last line. Earlier versions of this script ended
    # with the last check's "OK", which meant `check_layering.sh | tail -1` reported success
    # while an earlier check was failing. It did exactly that, and hid a real violation for
    # a whole phase.
    echo "RESULT: FAIL"
fi
exit $status
