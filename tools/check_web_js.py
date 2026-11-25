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


def check_no_referrer_policy(name: str, source: str) -> int:
    """Refuse a page-wide Referrer-Policy on the bridge's own pages.

    The REST API refuses a write whose Origin is not the bridge's own (src/outputs/rest/
    cross_site.h, CSRF). A `<meta name="referrer" content="no-referrer">` would make the browser
    send `Origin: null` on the page's OWN writes, and every save would answer 403. A per-link
    `rel="noreferrer"` is fine: it applies to that navigation only.
    """
    if re.search(r"<meta[^>]*name\s*=\s*[\"']?referrer", source, re.I):
        print(
            f"{name}: FAIL (a <meta name=referrer> would turn the page's own writes into "
            "Origin: null, which the REST API refuses as cross-site)"
        )
        return 1
    return 0


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    status = 0
    # One scratch directory for the whole run, removed on the way out. The previous form used
    # NamedTemporaryFile(delete=False) and never unlinked, so every local run and every CI run
    # left a .js file behind in the system temp directory.
    with tempfile.TemporaryDirectory(prefix="heliograph-js-") as scratch:
        for name in ASSETS:
            source = (root / name).read_text()
            status |= check_no_code_in_handlers(name, source)
            status |= check_no_referrer_policy(name, source)
            scripts = re.findall(r"<script>(.*?)</script>", source, re.S)
            if not scripts:
                print(f"{name}: FAIL (no <script> blocks found)")
                status = 1
                continue
            path = pathlib.Path(scratch) / (pathlib.Path(name).stem + ".js")
            path.write_text("\n".join(scripts))
            try:
                result = subprocess.run(
                    ["node", "--check", str(path)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
            except FileNotFoundError:
                # Without this the script dies on a bare traceback that names no cause. The
                # check is genuinely unrunnable, so say which tool is missing and fail --
                # skipping silently would report a clean tree that was never parsed.
                print(
                    "FAIL: node is not on PATH; install Node.js to syntax-check the web assets"
                )
                print("RESULT: FAIL")
                return 1
            print(f"{name}: {'OK' if result.returncode == 0 else 'FAIL'}")
            if result.returncode != 0:
                print(result.stderr)
                status = 1
                continue
            if name.endswith("index_html.h"):
                script = path.read_text()
                status |= check_version_compare(script, scratch)
                status |= check_auth_prompt_reentrancy(script, scratch)
                status |= check_auth_fetch_race(script, scratch)
                status |= check_auth_fetch_unreachable(script, scratch)
                status |= check_diag_single_flight(script, scratch)
                status |= check_sse_lifecycle(script, scratch)
                status |= check_state_event_gate(script, scratch)
                status |= check_bridge_clock(script, scratch)
                status |= check_fleet_mode(script, scratch)
                status |= check_live_repaint(script, scratch)
                status |= check_image_board(script, scratch)
                status |= check_history_span(script, scratch)
                status |= check_inverters_single_flight(script, scratch)
                status |= check_guarded_poll(script, scratch)
                status |= check_device_rows_fresh(script, scratch)
                status |= check_asleep(script, scratch)
                status |= check_control_confirm(script, scratch)
                status |= check_new_boot(script, scratch)
                status |= check_address_collision(script, scratch)
    # A final verdict, because the per-check lines are not the last thing printed: a failing
    # check dumps node's stderr after its own FAIL line, and any tail of this output then shows
    # a later check's OK. That is not hypothetical -- a real failure was read as green that way
    # (2026-07-29). check_layering.sh has always ended with a verdict; this now does too.
    print(f"RESULT: {'PASS' if status == 0 else 'FAIL'}")
    return status


# An inline event handler is COMPILED AS JAVASCRIPT, and the browser HTML-decodes the attribute
# value before that happens. So esc() -- which is correct everywhere else on these pages -- does
# not protect a handler: it turns an apostrophe into &#39;, the parser turns it back into an
# apostrophe, and the string the handler was building closes early.
#
# That was live. A device id is driverId + '-' + the serial number the inverter reports over
# RS485, and the only filter on those bytes is "printable ASCII", which includes the apostrophe.
# A device supplying a crafted serial got script into the admin's authenticated session, where
# sessionStorage holds the Basic-auth token.
#
# The fix was to move every interpolated value into a data-* attribute read back with
# getAttribute. This check keeps it that way. It is deliberately shape-based rather than
# taint-based: proving a given value is safe means reading code, and the pages are re-authored
# from a design tool often enough that "someone will notice" is not a control. No exceptions --
# a handler that needs a value takes it from data-*.
# Matches any on-word, MINUS the handful that are not handlers, rather than an allowlist of
# event names. Review showed why: an allowlist has to be exhaustive to work, and the first
# version of it was missing onbeforetoggle, onmessageerror, onanimationcancel, onbeforematch,
# oncontextlost, onrejectionhandled, ontransitionrun and the SVG SMIL handlers -- so the exact
# original vulnerability, written with any of those, passed. HTML keeps adding events; this
# denylist has one entry today (`online=${f.online}` in a template literal) and does not grow
# when the platform does.
#
# A false positive here is a loud CI failure on a pull request; a false negative is script
# running in someone's session. Given the choice, be loud.
NOT_HANDLERS = {"online"}

# The `-` in the lookbehind keeps a custom attribute like data-onclick out; the rest keeps JS
# property assignment out -- `dlg.onclose=()=>{...}` assigns a function object and compiles no
# string, so it is not this defect.
HANDLER_ATTR = re.compile(
    r'(?<![-.\w$])(on[a-z]+)\s*=\s*(?:"([^"]*)"|\'([^\']*)\'|([^\s>]+))',
    re.IGNORECASE,
)

# What makes a handler value UNSAFE is that some of it is computed. `${` and a backtick are how
# this page normally does that; the rest are the ways review demonstrated round it.
#
# This cannot be exhaustive and is not the real defence. A shape check can always be evaded by
# building the same string another way -- review did exactly that with Array.join(). It is here
# to fail fast, in a second, on the form anyone would actually write. The defence that does not
# depend on how the source is spelled is in check_dashboard_layout.py, which renders the page
# with a hostile device serial and asserts no on* attribute in the DOM carries it.
DYNAMIC = re.compile(
    r"\$\{"  # template interpolation, how this page normally builds a value
    r"|`"  # a nested template literal
    r"|['\"]\s*\+|\+\s*['\"]"  # concatenation -- next to a quote, so /search?q=solar+panel
    #                            stays legal: a static URL is not a built value
    r"|\.join\s*\(|\.concat\s*\(|String\.raw"  # the ways review got round the above
)


def check_no_code_in_handlers(name, source):
    hits = []
    for m in HANDLER_ATTR.finditer(source):
        if m.group(1).lower() in NOT_HANDLERS:
            continue
        quoted = m.group(2) if m.group(2) is not None else m.group(3)
        if quoted is None:
            why = "the value is not quoted"
        elif DYNAMIC.search(quoted):
            why = "the value is built rather than literal"
        else:
            continue
        line = source.count("\n", 0, m.start()) + 1
        hits.append((line, why + ": " + m.group(0).strip()[:90]))
    print(
        f"{name}: inline handlers carry no interpolation: {'OK' if not hits else 'FAIL'}"
    )
    for n, text in hits:
        print(f"  {name}:{n}: {text}")
    if hits:
        print(
            "  An on* attribute is compiled as JS after the browser HTML-decodes it, so esc()"
        )
        print(
            "  does not make an interpolated value safe there. Put the value in a data-*"
        )
        print(
            "  attribute and read it with getAttribute in the delegated click handler."
        )
        return 1
    return 0


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
    """Pulls one top-level `function name(...){...}` out by brace matching.

    Keeps an `async` prefix when there is one -- without it the extracted body still contains
    `await` and node refuses to parse it, which would read as "the check is broken" rather than
    "the function is async".
    """
    start = source.find(f"async function {name}(")
    if start < 0:
        start = source.find(f"function {name}(")
    if start < 0:
        return None
    # Walk the PARAMETER LIST to its closing paren first, then take the brace after it.
    # Starting the brace match at the first `{` after the name is wrong for a default argument
    # that is an object: `authFetch(url,opts={})` closed depth on the `}` of `{}` and returned
    # a function cut off at its own signature, which node then reported as a syntax error two
    # lines further down -- reading as "the page is broken" rather than "the extractor is".
    depth = 0
    body_start = None
    for i in range(source.index("(", start), len(source)):
        if source[i] == "(":
            depth += 1
        elif source[i] == ")":
            depth -= 1
            if depth == 0:
                body_start = source.index("{", i)
                break
    if body_start is None:
        return None
    depth = 0
    for i in range(body_start, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start : i + 1]
    return None


def run_node(label, filename, harness, scratch):
    """Run a JS harness under node; print "<label>: OK|FAIL" and return 0/1."""
    path = pathlib.Path(scratch) / filename
    # Failure unless the harness says otherwise. node exits 0 when its event loop drains, and an
    # async harness whose awaited promise never settles drains without reaching its verdict --
    # which read as a pass. Every harness ends in an explicit process.exit().
    path.write_text("process.exitCode = 1;\n" + harness)
    result = subprocess.run(
        ["node", str(path)], capture_output=True, text=True, check=False
    )
    print(f"{label}: {'OK' if result.returncode == 0 else 'FAIL'}")
    if result.returncode != 0:
        print(result.stdout + result.stderr)
        return 1
    return 0


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
    return run_node("version comparison", "version_compare.js", harness, scratch)


def check_fleet_mode(script, scratch):
    """One answer to "is this a fleet?", for the headline and for the chart under it.

    It used to be answered three times in three places, and one of them differed: the sparkline
    picked its source by `devices.length > 1` while the header and the hero used
    devices_configured. A bridge with two inverters configured and one not yet answering then
    charted a single inverter's output under a heading reading "all inverters stacked" -- and
    when the second one started replying, the series switched source mid-curve and drew a step
    that never happened.

    Silent while it lasts, because with one answering device the fleet total and that device's
    own reading are the same number. That is what makes it worth a test rather than a comment.
    """
    body = extract_function(script, "isFleet")
    if body is None:
        print("FAIL: isFleet() not found in index_html.h; this check needs updating")
        return 1
    cases = json.dumps(
        [
            # (label, status payload, expected)
            [
                "one inverter, answering",
                {
                    "bridge": {"devices_configured": 1},
                    "devices": [{}],
                    "totals": {"devices_polled": 1},
                },
                False,
            ],
            [
                "two configured, both polled",
                {
                    "bridge": {"devices_configured": 2},
                    "devices": [{}, {}],
                    "totals": {"devices_polled": 2},
                },
                True,
            ],
            # THE case. Configured for two, only one in the array: still a fleet, because the
            # screen says so.
            [
                "two configured, one started",
                {
                    "bridge": {"devices_configured": 2},
                    "devices": [{}],
                    "totals": {"devices_polled": 1},
                },
                True,
            ],
            [
                "two configured, none answering yet",
                {"bridge": {"devices_configured": 2}, "devices": [], "totals": {}},
                True,
            ],
            # Older or partial payloads must not flip the layout by omission.
            [
                "no configured count, two devices",
                {"bridge": {}, "devices": [{}, {}], "totals": {"devices_polled": 2}},
                True,
            ],
            [
                "no totals, one device",
                {"bridge": {}, "devices": [{}], "totals": {}},
                False,
            ],
            ["empty payload", {}, False],
        ]
    )
    harness = (
        body
        + f"""
let bad = 0;
for (const [label, payload, want] of {cases}) {{
  const got = isFleet(payload);
  if (got !== want) {{ console.error(`${{label}}: isFleet = ${{got}}, want ${{want}}`); bad++; }}
}}
process.exit(bad === 0 ? 0 : 1);
"""
    )
    return run_node("fleet mode", "fleet_mode.js", harness, scratch)


def check_asleep(script, scratch):
    """Grey for an inverter asleep since dusk, red for every way that grey could hide a fault.

    Every evening used to turn the dashboard red. The rule that replaces that alarm must not swallow
    the states that are faults, and a mistake in either direction is silent: too eager hides a dead
    inverter behind a grey dot, too strict brings the nightly red back. Times are local, built with
    the Date constructor, so the cases hold in any timezone the check runs in.
    """
    parts = []
    for fn in ("asleep", "quietSince", "fleetVerdict"):
        body = extract_function(script, fn)
        if body is None:
            print(f"FAIL: {fn}() not found in index_html.h; this check needs updating")
            return 1
        parts.append(body)
    consts = re.search(r"const ASLEEP_BELOW_W=.*?;\n", script)
    if consts is None:
        print(
            "FAIL: ASLEEP_BELOW_W constants not found in index_html.h; this check needs updating"
        )
        return 1
    harness = (
        consts.group(0)
        + "\n".join(parts)
        + """
const at = (d, h, m = 0) => new Date(2026, 10, d, h, m).getTime();
// A row that went quiet at `quiet` with `watts` as its last reading, seen at `now`.
const row = (quiet, now, o = {}) => Object.assign({online:false,
  last_successful_poll_seconds_ago: Math.round((now - quiet) / 1000), last_ac_power_w: 12}, o);
const cases = [
  ['quiet at dusk, seen that evening', at(25,17,40), at(25,22), {}, true],
  ['quiet at dusk, seen at 09:59 next day', at(25,17,40), at(26,9,59), {}, true],
  ['quiet at dusk, not awake by 10:00', at(25,17,40), at(26,10,0), {}, false],
  ['quiet at dusk, seen the next afternoon', at(25,17,40), at(26,14), {}, false],
  ['quiet at 0 W', at(25,17), at(25,23), {last_ac_power_w:0}, true],
  ['quiet at 02:00, seen at 03:00', at(26,2), at(26,3), {}, true],
  ['quiet at full output in the evening', at(25,19), at(25,20), {last_ac_power_w:2100}, false],
  ['quiet while importing hard', at(25,19), at(25,20), {last_ac_power_w:-1500}, false],
  ['quiet at low output at noon', at(25,12), at(25,12,30), {}, false],
  ['quiet at low output at dawn', at(26,7), at(26,7,30), {}, false],
  ['never answered', at(25,17), at(25,22), {last_successful_poll_seconds_ago:null, last_ac_power_w:null}, false],
  ['answered, no power channel', at(25,17), at(25,22), {last_ac_power_w:null}, false],
  ['a reading but no poll time', at(25,17), at(25,22), {last_successful_poll_seconds_ago:null}, false],
  ['field absent (older firmware)', at(25,17), at(25,22), {last_ac_power_w:undefined}, false],
  ['still online', at(25,17), at(25,22), {online:true}, false],
];
let bad = 0;
for (const [label, quiet, now, o, want] of cases) {
  const got = asleep(row(quiet, now, o), now);
  if (got !== want) { console.error(`asleep: ${label} = ${got}, want ${want}`); bad++; }
}
if (asleep(undefined)) { console.error('asleep: no row = true'); bad++; }
const t1 = quietSince(row(at(25,17,40), at(25,22)), at(25,22));
const t2 = quietSince(row(at(25,17,40), at(26,8)), at(26,8));
if (t1 !== 'since 17:40') { console.error(`quietSince same day = ${t1}`); bad++; }
if (t2 !== 'since yesterday 17:40') { console.error(`quietSince next day = ${t2}`); bad++; }
for (const [a, e, z, want] of [[2,2,0,'ok'],[0,2,2,'off'],[1,2,1,'off'],[0,3,2,'bad'],[1,3,0,'bad'],[0,1,0,'bad']]) {
  const got = fleetVerdict(a, e, z);
  if (got !== want) { console.error(`fleetVerdict(${a},${e},${z}) = ${got}, want ${want}`); bad++; }
}
process.exit(bad === 0 ? 0 : 1);
"""
    )
    return run_node("asleep versus fault", "asleep.js", harness, scratch)


def check_control_confirm(script, scratch):
    """A Save that opens a control path asks first; one that only closes does not.

    The access form applies immediately, and before this a tick on "Relays enabled" moved from a
    checkbox to live contacts on an inverter's DRM port with no question asked. The rule has two
    silent failure modes: asking for nothing (the old state) and asking on every save, which
    trains people to click OK. Both directions are cases below.
    """
    body = extract_function(script, "controlConfirmText")
    state = extract_function(script, "controlState")
    roles = re.search(r"const DRM_ROLES=\{.*?\};", script, re.S)
    label = re.search(r"const roleLabel=.*?;\n", script)
    if body is None or state is None or roles is None or label is None:
        print(
            "FAIL: controlConfirmText/DRM_ROLES/roleLabel not found in index_html.h; this check needs updating"
        )
        return 1
    harness = (
        roles.group(0)
        + "\n"
        + label.group(0)
        + body
        + "\n"
        + state
        + """
const off = {ctl:false, iw:false, rl:false, roles:['none','none']};
const S = (o) => Object.assign({}, off, o);
const cases = [
  ['nothing changes', off, off, null],
  ['allow control alone opens nothing', off, S({ctl:true}), null],
  ['relays alone, control off, open nothing', off, S({rl:true, roles:['drm0','none']}), null],
  ['writes alone, control off, open nothing', off, S({iw:true}), null],
  ['inverter writes open', S({ctl:true}), S({ctl:true, iw:true}), 'Inverter writes open'],
  ['control opens what was ticked below it', S({iw:true}), S({ctl:true, iw:true}), 'Inverter writes open'],
  ['relays open, roles already stored', S({ctl:true, roles:['drm0','none']}), S({ctl:true, rl:true, roles:['drm0','none']}), 'Relays open'],
  ['relays open, role set', S({ctl:true}), S({ctl:true, rl:true, roles:['drm0','none']}), 'Relay 1: DRM0 — disconnect'],
  ['wiring rule shown with live roles', S({ctl:true}), S({ctl:true, rl:true, roles:['drm5','none']}), 'NO or NC'],
  ['everything off again', S({ctl:true, iw:true, rl:true, roles:['drm0','drm5']}), off, null],
  ['role change while control is off', off, S({roles:['drm0','none']}), null],
  ['role change while relays are off', S({ctl:true}), S({ctl:true, roles:['drm0','none']}), null],
  ['live role cleared to none', S({ctl:true, rl:true, roles:['drm0','none']}), S({ctl:true, rl:true}), null],
  ['live role drm5 -> drm0', S({ctl:true, rl:true, roles:['drm5','none']}), S({ctl:true, rl:true, roles:['drm0','none']}), 'DRM0'],
  ['saving an unchanged live setup', S({ctl:true, rl:true, roles:['drm0','none']}), S({ctl:true, rl:true, roles:['drm0','none']}), null],
];
let bad = 0;
for (const [label, was, now, want] of cases) {
  const got = controlConfirmText(was, now);
  const ok = want === null ? got === '' : got.includes(want);
  if (!ok) { console.error(`${label}: got ${JSON.stringify(got)}, want ${want === null ? 'no prompt' : JSON.stringify(want)}`); bad++; }
}
for (const r of Object.keys(DRM_ROLES)) if (roleLabel(r).includes('unknown')) { console.error(`no label for ${r}`); bad++; }
// Opening writes beside relays that are already live and unchanged asks about the writes only.
const wOnly = controlConfirmText(S({ctl:true, rl:true, roles:['drm0','none']}), S({ctl:true, iw:true, rl:true, roles:['drm0','none']}));
if (!wOnly.includes('Inverter writes open') || wOnly.includes('Relay 1')) { console.error(`writes beside live relays: ${JSON.stringify(wOnly)}`); bad++; }
// The firmware accepts exactly these (configuration.cpp); a role offered here and refused there is a Save that fails.
if (Object.keys(DRM_ROLES).join(',') !== 'none,drm0,drm1,drm2,drm3,drm4,drm5,drm6,drm7,drm8') { console.error('DRM_ROLES keys changed'); bad++; }
const st = [
  [controlState({security:{read_only_mode:true}}), {ctl:false, iw:false, rl:false, roles:[]}],
  [controlState({security:{read_only_mode:false}, control:{inverter_writes_enabled:true}, relays:{enabled:true, roles:['drm0']}}), {ctl:true, iw:true, rl:true, roles:['drm0']}],
  [controlState({security:{}}), {ctl:false, iw:false, rl:false, roles:[]}],
];
for (const [got, want] of st) if (JSON.stringify(got) !== JSON.stringify(want)) { console.error(`controlState = ${JSON.stringify(got)}, want ${JSON.stringify(want)}`); bad++; }
process.exit(bad === 0 ? 0 : 1);
"""
    )
    return run_node("control confirmation", "control_confirm.js", harness, scratch)


def check_new_boot(script, scratch):
    """A restart is noticed, and the page reloads unless that would eat someone's typing.

    After a restart the old page kept running against the new boot: config, driver list and the
    "waiting for a restart" list all from before, and after an OTA the page's own code from the
    previous firmware. Missing a boot is silent, and so is the opposite mistake -- reloading while
    a form is open throws away what was typed.
    """
    parts = []
    for fn in ("isNewBoot", "newBootAction", "askedForRestart"):
        body = extract_function(script, fn)
        if body is None:
            print(f"FAIL: {fn}() not found in index_html.h; this check needs updating")
            return 1
        parts.append(body)
    harness = (
        "\n".join(parts)
        + """
let bad = 0;
const b = (v, up) => ({firmware_version: v, uptime_seconds: up});
const T = 1_000_000;  // "now" in ms; prev.at is how long before it the last sample came in
const p = (v, up, agoS) => ({v, up, at: T - agoS * 1000});
const boots = [
  ['first sample', null, b('0.37.0', 50), false],
  ['same boot, 5 s later', p('0.37.0', 50, 5), b('0.37.0', 55), false],
  ['same boot, same second', p('0.37.0', 50, 0), b('0.37.0', 50), false],
  ['same boot, slow reply (10 s late)', p('0.37.0', 50, 20), b('0.37.0', 60), false],
  ['restart', p('0.37.0', 5000, 20), b('0.37.0', 12), true],
  ['restart noticed late (tab in background)', p('0.37.0', 20, 600), b('0.37.0', 400), true],
  ['OTA', p('0.37.0', 5000, 20), b('0.38.0', 12), true],
  ['OTA, slow to notice', p('0.37.0', 20, 600), b('0.38.0', 400), true],
  ['older firmware without uptime', p('0.37.0', undefined, 20), b('0.37.0', undefined), false],
  ['null uptime is not a boot', p('0.37.0', 5000, 20), b('0.37.0', null), false],
];
// A restart this page asked for, then lost the bridge: a boot even when the last sample was young.
if (!isNewBoot(p('0.37.0', 10, 30), b('0.37.0', 15), T, true)) { console.error('isNewBoot: asked-for restart after a gap = false'); bad++; }
if (isNewBoot(null, b('0.37.0', 15), T, true)) { console.error('isNewBoot: gap on the first sample = true'); bad++; }
for (const [label, prev, now, want] of boots) {
  const got = isNewBoot(prev, now, T);
  if (got !== want) { console.error(`isNewBoot: ${label} = ${got}, want ${want}`); bad++; }
}
const acts = [
  ['nothing open', false, false, false, 'reload'],
  ['typing, unexpected restart', false, false, true, 'banner'],
  ['typing, but this page asked for it', false, true, true, 'reload'],
  ['typing, new firmware', true, false, true, 'reload'],
];
for (const [label, v, asked, typing, want] of acts) {
  const got = newBootAction(v, asked, typing);
  if (got !== want) { console.error(`newBootAction: ${label} = ${got}, want ${want}`); bad++; }
}
let restarting = 0;
if (askedForRestart(T)) { console.error('askedForRestart with nothing asked = true'); bad++; }
restarting = T - 30_000;
if (!askedForRestart(T)) { console.error('askedForRestart 30 s after the click = false'); bad++; }
restarting = T - 3_600_000;
if (askedForRestart(T)) { console.error('askedForRestart an hour after the click = true'); bad++; }
process.exit(bad === 0 ? 0 : 1);
"""
    )
    return run_node("new boot", "new_boot.js", harness, scratch)


def check_device_rows_fresh(script, scratch):
    """A write to additional_devices starts from the stored rows, not this page's old copy.

    The API replaces the array whole. Built from a config this page read an hour ago, a removal in
    one tab came back from another, and "remove row 1" removed whatever had moved into position 1.
    Both are silent: the save succeeds, with the wrong rows.
    """
    parts = []
    for fn in ("rowsOf", "sameRow", "freshRows", "removeExtraAt"):
        body = extract_function(script, fn)
        if body is None:
            print(f"FAIL: {fn}() not found in index_html.h; this check needs updating")
            return 1
        parts.append(body)
    harness = (
        """
const A = {driver_id:'x', label:'A', options:{unit_id:'2'}};
const B = {driver_id:'x', label:'B', options:{unit_id:'3'}};
const C = {driver_id:'x', label:'C', options:{unit_id:'4'}};
let cfg, stored, patched, alerted, panel, invNextMs;
const getJson = async () => { if (stored === null) throw new Error('timed out'); return JSON.parse(JSON.stringify(stored)); };
const patch = async (body) => { patched = body; return {ok:true}; };
const confirm = () => true;
const alert = (t) => { alerted = t; };
const loadInverters = () => {};
const $ = () => null;
const say = () => {};
const ROWS_UNREAD = 'unread';
"""
        + "\n".join(parts)
        + """
// An async harness that never finishes exits 0 in node. Fail unless it reaches the end.
process.exitCode = 1;
let bad = 0;
const run = async (label, pageRows, storedRows, idx, want) => {
  cfg = {driver:{id:'x'}, additional_devices: pageRows};
  stored = storedRows === null ? null : {driver:{id:'x'}, additional_devices: storedRows};
  patched = undefined; alerted = undefined;
  await removeExtraAt(idx);
  const got = patched ? patched.additional_devices.map(r => r.label).join(',') : 'refused';
  if (got !== want) { console.error(`${label}: got ${got}, want ${want}`); bad++; }
};
(async () => {
  await run('nothing changed elsewhere', [A,B,C], [A,B,C], 1, 'A,C');
  await run('a row before it was removed elsewhere', [A,B,C], [B,C], 1, 'refused');
  await run('a row was added elsewhere after it', [A,B], [A,B,C], 1, 'A,C');
  await run('the row itself was edited elsewhere', [A,B], [A,{...B, label:'B2'}], 1, 'refused');
  await run('the row is gone', [A,B], [A], 1, 'refused');
  await run('the bridge did not answer the read', [A,B,C], null, 1, 'refused');
  process.exit(bad === 0 ? 0 : 1);
})();
"""
    )
    return run_node("device rows fresh", "device_rows.js", harness, scratch)


def check_guarded_poll(script, scratch):
    """A polling loop never has two requests in flight.

    setInterval fires every second regardless; on a weak link a request takes up to its 10 s
    timeout, so the loops behind a capture or a device search stacked ten requests on the bridge.
    """
    body = extract_function(script, "guarded")
    if body is None:
        print("FAIL: guarded() not found in index_html.h; this check needs updating")
        return 1
    # Every polling loop goes through it: one reverted to a bare setInterval passes the harness
    # below and brings the request pile-up back.
    bare = re.findall(r"setInterval\((?:async|poll)", script)
    if bare:
        print(
            f"guarded polling: FAIL\n  {len(bare)} polling loop(s) without guarded(): {bare}"
        )
        return 1
    capture = extract_function(script, "pollCapture")
    if capture is None:
        print(
            "FAIL: pollCapture() not found in index_html.h; this check needs updating"
        )
        return 1
    harness = (
        body
        + capture
        + """
// A tick of an OLD capture loop answering after a new capture started must leave the new one be.
let capTimer = null, rendered = 0, cleared = [];
const clearInterval = (t) => cleared.push(t);
const $ = () => ({});
const say = () => {};
const renderCapture = () => { rendered++; };
let answer;
const getJson = () => new Promise(r => { answer = r; });
let calls = 0, release;
const slow = () => { calls++; return new Promise(r => { release = r; }); };
const tick = guarded(slow);
(async () => {
  let bad = 0;
  const first = tick();
  for (let i = 0; i < 9; i++) tick();          // nine more ticks while the first is out
  if (calls !== 1) { console.error(`ticks while in flight started ${calls} requests, want 1`); bad++; }
  release(); await first;
  tick();
  if (calls !== 2) { console.error(`the tick after an answer started ${calls - 1} more, want 1`); bad++; }
  release();
  // A request that throws must not leave the loop stuck as "busy" for good.
  const boom = guarded(async () => { calls++; throw new Error('x'); });
  await boom().catch(() => {});
  await boom().catch(() => {});
  if (calls !== 4) { console.error('a failed request left the loop stuck'); bad++; }
  capTimer = 'old';
  const late = pollCapture('old');                 // its request goes out...
  capTimer = 'new';                                 // ...a new capture starts...
  answer({status: 'done', summary: {}});            // ...and the old answer lands
  await late;
  if (capTimer !== 'new') { console.error(`a late tick of the old capture stopped the new one (capTimer=${capTimer})`); bad++; }
  if (rendered) { console.error('a late tick of the old capture rendered over the new one'); bad++; }
  process.exit(bad === 0 ? 0 : 1);
})();
"""
    )
    return run_node("guarded polling", "guarded.js", harness, scratch)


def check_inverters_single_flight(script, scratch):
    """A refresh asked for by an action is never dropped, and never runs alongside another.

    The Inverters tab reloads after a save, a removal or an added row (force). One that landed
    while the five-second background pass was out used to return at once: the pass that was
    running had read the list before the save, its paint was the last word, and the row just
    added stayed missing until the next timer.
    """
    body = extract_function(script, "loadInverters")
    held = extract_function(script, "invHeldMs")
    if body is None or held is None:
        print(
            "FAIL: loadInverters()/invHeldMs() not found in index_html.h; this check needs updating"
        )
        return 1
    harness = (
        """
process.exitCode = 1;
let invBusy=false, invNextMs=0, invIds=[], invPass=null;
const invFetchedAt={}, invMissed={}, INV_STALE_MS=15000, devCache={}, measCache={}, capsCache={};
let tab='other', panel=null;
const readerIsBusy=()=>false, paintInverters=()=>{};
let passes=0, inFlight=0, maxInFlight=0, gate=null, failNext=false, failAt=null, list=['a'];
const getJson=async (u)=>{
  if(u==='/api/v1/devices'){
    passes++; inFlight++; maxInFlight=Math.max(maxInFlight,inFlight);
    if(gate) await gate;
    inFlight--;
    if(failNext){failNext=false; throw new Error('timed out');}
    return {devices:list};
  }
  if(failAt&&u.includes('/devices/'+failAt+'/measurements')){failAt=null; throw new Error('timed out');}
  return {measurements:{}};
};
"""
        + body
        + held
        + """
(async () => {
  let bad = 0;
  let open; gate = new Promise(r => { open = r; });
  const background = loadInverters(false);              // the timer's pass, held open
  const timer = loadInverters(false);                    // another timer tick meanwhile
  let forcedDone = false;
  const forced = loadInverters(true).then(() => { forcedDone = true; });
  await new Promise(r => setTimeout(r, 10));
  if (passes !== 1) { console.error(`passes while one was out: ${passes}, want 1`); bad++; }
  if (forcedDone) { console.error('the forced refresh returned before any pass after it ran'); bad++; }
  gate = null; open();
  await background; await timer; await forced;
  if (passes !== 2) { console.error(`forced refresh during a pass ran ${passes - 1} extra, want 1`); bad++; }
  if (maxInFlight !== 1) { console.error(`${maxInFlight} passes ran at once`); bad++; }
  // A pass that fails records no fresh answer, so the card can say its numbers are old.
  invFetchedAt.a = 1;  // an answer from long ago
  failNext = true; await loadInverters(true);
  if (invFetchedAt.a !== 1) { console.error('a failed pass still marked the device as freshly read'); bad++; }
  if (invPass !== null || invBusy) { console.error('a failed pass left the loader marked busy'); bad++; }
  if (invHeldMs('a', 20001) !== 20000) { console.error(`a device the bridge did not answer for reads held ${invHeldMs('a', 20001)} ms, want 20000`); bad++; }
  // Old but never missed is NOT held: another tab was in front, or the pass has not run yet.
  await loadInverters(true);
  invFetchedAt.a = 1;
  if (invHeldMs('a', 60000) !== 0) { console.error('an old copy no pass has failed to refresh reads as held'); bad++; }
  // A pass that fails halfway: the devices before the failure are fresh, the rest are held.
  list = ['a', 'b', 'c'];
  await loadInverters(true);
  for (const id of list) invFetchedAt[id] = 1;
  failAt = 'b'; await loadInverters(true);
  const heldNow = list.map(id => invHeldMs(id, 30000) > 0);
  if (heldNow.join() !== 'false,true,true') { console.error(`after a failure at b, held = ${heldNow}, want false,true,true`); bad++; }
  // A device that left the list leaves no record behind to be read as held if it comes back.
  list = ['a']; await loadInverters(true);
  if ('b' in invFetchedAt || 'b' in invMissed) { console.error('a removed device kept its record'); bad++; }
  process.exit(bad === 0 ? 0 : 1);
})();
"""
    )
    return run_node("inverters single flight", "inv_flight.js", harness, scratch)


def check_history_span(script, scratch):
    """The power curve is headed with the span it covers, not "Today".

    At most 720 samples, one per refresh, is an hour or two of the page being open; a chart opened
    at 14:00 read "Today" at 17:00 while it showed 15:30 onwards.
    """
    parts = []
    for fn in ("record", "histSpan"):
        body = extract_function(script, fn)
        if body is None:
            print(f"FAIL: {fn}() not found in index_html.h; this check needs updating")
            return 1
        parts.append(body)
    harness = (
        "const HMAX=720; let hist={}, histT={};\n"
        + "\n".join(parts)
        + """
let bad = 0;
const want = (got, exp, label) => { if (got !== exp) { console.error(`${label}: ${got}, want ${exp}`); bad++; } };
const T0 = new Date(2026, 10, 25, 14, 5).getTime();
want(histSpan('all', T0), 'Recent', 'no samples');
record('all', 100, T0);
want(histSpan('all', T0 + 60000), 'Recent', 'one sample');
record('all', 120, T0 + 47 * 60000);
want(histSpan('all', T0 + 47 * 60000), 'Since 14:05', 'same day');
want(histSpan('all', T0 + 12 * 3600000), 'Since yesterday 14:05', 'past midnight');
// The cap drops the oldest samples AND their times, so the span is what is still drawn.
hist = {}; histT = {};
for (let i = 0; i < 800; i++) record('all', i, T0 + i * 10000);
want(hist.all.length, 720, 'samples kept');
want(histT.all.length, 720, 'times kept');
want(histT.all[0], T0 + 80 * 10000, 'oldest time kept');
want(histSpan('all', T0 + 799 * 10000), 'Since 14:18', 'start after the cap');
// Values saved by an older page, without times: they start over rather than carry a wrong span.
hist = {all: [1, 2, 3]}; histT = {};
record('all', 4, T0);
want(hist.all.join(), '4', 'untimed history kept');
want(histT.all.length, 1, 'times after an untimed history');
process.exit(bad === 0 ? 0 : 1);
"""
    )
    return run_node("history span", "history_span.js", harness, scratch)


def check_image_board(script, scratch):
    """A hand-picked OTA image is checked against this board by its file name.

    Nothing in the image says which board it was built for, and the wrong one boots on the wrong
    pins with the rollback none the wiser. The page reads the release naming; its list of boards
    must stay the release workflow's, or a new board's images would install unchecked.
    """
    body = extract_function(script, "imageBoard")
    ids = re.search(r"const BOARD_IDS=(\[.*?\]);", script)
    if body is None or ids is None:
        print(
            "FAIL: imageBoard()/BOARD_IDS not found in index_html.h; this check needs updating"
        )
        return 1
    release = (
        pathlib.Path(__file__).resolve().parent.parent / ".github/workflows/release.yml"
    )
    m = re.search(r"^\s*BOARDS:\s*(.+)$", release.read_text(), re.M)
    if m is None:
        print("FAIL: BOARDS not found in release.yml; this check needs updating")
        return 1
    page = json.loads(ids.group(1).replace("'", '"'))
    if sorted(page) != sorted(m.group(1).split()):
        print(
            f"image board: FAIL\n  BOARD_IDS {page} != release.yml BOARDS {m.group(1).split()}"
        )
        return 1
    harness = (
        ids.group(0)
        + "\n"
        + body
        + """
let bad = 0;
const cases = [
  ['heliograph-0.37.0-rs485-can.bin', 'rs485-can'],
  ['heliograph-relay-6ch.bin', 'relay-6ch'],
  ['Heliograph-0.38.0-rc.1-relay-1ch.BIN', 'relay-1ch'],
  ['heliograph-0.37.0-rs485-can-factory.bin', 'factory'],
  ['firmware.bin', null],
  ['heliograph-0.37.0-rs485-can (1).bin', null],
];
for (const [name, want] of cases) {
  const got = imageBoard(name);
  if (got !== want) { console.error(`imageBoard(${name}) = ${got}, want ${want}`); bad++; }
}
process.exit(bad === 0 ? 0 : 1);
"""
    )
    return run_node("image board", "image_board.js", harness, scratch)


def check_live_repaint(script, scratch):
    """The Live tab is rebuilt only when what it shows changed, and the timer holds back under a
    selection.

    It was rebuilt on every refresh, up to once a second, and a rebuild drops a selection: a
    reading could not be copied off the page that exists to show readings. The hold is the
    timer's only (readerIsBusy): a tab switch still draws, or a selection left behind would keep
    the tab on old readings.
    """
    parts = []
    for fn in ("drawIfChanged", "setLive", "readerIsBusy"):
        body = extract_function(script, fn)
        if body is None:
            print(f"FAIL: {fn}() not found in index_html.h; this check needs updating")
            return 1
        parts.append(body)
    harness = (
        """
let drawn = {}, writes = 0, selected = false, tab = 'live';
const pane = { contains: (n) => n === 'inside' };
const el = { set innerHTML(v) { writes++; }, contains: pane.contains };
const $ = () => el;
let anchor = 'inside';
const getSelection = () => ({ isCollapsed: !selected, anchorNode: anchor });
const window = { getSelection };
const document = { activeElement: null, body: {} };
"""
        + "\n".join(parts)
        + """
let bad = 0;
setLive('<b>1</b>'); setLive('<b>1</b>');
if (writes !== 1) { console.error(`identical markup written ${writes} times, want 1`); bad++; }
setLive('<b>2</b>');
if (writes !== 2) { console.error('changed markup was not written'); bad++; }
selected = true;
if (!readerIsBusy()) { console.error('a selection on the tab does not hold the timer back'); bad++; }
anchor = 'elsewhere';
if (readerIsBusy()) { console.error('a selection outside the tab holds it back'); bad++; }
selected = false;
if (readerIsBusy()) { console.error('no selection, no focus, yet busy'); bad++; }
process.exit(bad === 0 ? 0 : 1);
"""
    )
    return run_node("live repaint", "live_repaint.js", harness, scratch)


def check_address_collision(script, scratch):
    """Two inverters answering to the same address are caught, whichever drivers they use.

    There is ONE RS485 bus. The check keyed on driver+address, so it only ever noticed a
    collision between two units of the same make -- while this firmware ships two Modbus
    drivers (modbus_profile and sunspec) whose units share the numbering. The most likely real
    collision walked straight past the card built to report it.

    Both directions matter. A protocol that does not address this way -- AA55 finds a device by
    serial number and declares no unit_id -- must not be dragged into a collision it cannot
    have, or the card cries wolf on a perfectly good bus.
    """
    # addressProblem leans on both of these now: an address is the stored option OR the driver's
    # declared default, and resolving only the first is the blind spot this check exists to keep
    # closed. Pulled in rather than stubbed, so the test exercises the real resolution.
    parts = []
    for fn in ("addressOptionOf", "addressOf", "canShare", "addressProblem"):
        piece = extract_function(script, fn)
        if piece is None:
            print(f"FAIL: {fn}() not found in index_html.h; this check needs updating")
            return 1
        parts.append(piece)
    body = "\n".join(parts)
    harness = (
        """
// Only what addressProblem reads. `sunspec` and `modbus_profile` both declare unit_id;
// `eversolar_legacy` declares none, which is what makes it the false-positive case.
const drivers = {drivers:[
  {id:'modbus_profile', options:[{key:'unit_id', display_name:'Modbus unit id', default_value:'1'}]},
  {id:'sunspec',        options:[{key:'unit_id', display_name:'Modbus unit id', default_value:'1'}]},
  // Declares an address with a default of 16, exactly as the real descriptor does. An empty
  // options list here would have made the fallback untestable.
  {id:'eversolar_legacy', options:[{key:'address', display_name:'Assigned bus address', default_value:'16'}]},
  // The one that answers NO. addressProblem() refuses a SECOND row of it whatever the addresses
  // say, because planDevices() skips that row at boot whatever the addresses say.
  {id:'solax_x1', supports_multiple_devices:false,
   options:[{key:'address', display_name:'Assigned bus address', default_value:'10'}]},
]};
const cfg = {driver:{}, additional_devices:[]};
"""
        + body
        + r"""
let bad = 0;
const check = (label, body, wantCollision) => {
  const got = addressProblem(body);
  const isCollision = !!got && got.includes('same address');
  if (isCollision !== wantCollision) {
    console.error(`${label}: got ${JSON.stringify(got)}, wanted ${wantCollision ? 'a collision' : 'no collision'}`);
    bad++;
  }
};

// THE case this was blind to: the primary stores NOTHING and answers at its declared default,
// which is how a bridge configured through the wizard looks. Typing that number into an extra by
// hand was accepted, and the two then collided on the bus.
check('primary at its default + extra typed to match', {
  driver:{id:'modbus_profile', options:{}},
  additional_devices:[{driver_id:'modbus_profile', options:{unit_id:'1'}}]}, true);

// And across two drivers, since the bus does not care which one is polling.
check('eversolar primary at default 16 + modbus@16', {
  driver:{id:'eversolar_legacy', options:{}},
  additional_devices:[{driver_id:'modbus_profile', options:{unit_id:'16'}}]}, true);

// Two different Modbus drivers, one bus, one address.
check('modbus@2 + sunspec@2', {
  driver:{id:'modbus_profile', options:{unit_id:'2'}},
  additional_devices:[{driver_id:'sunspec', options:{unit_id:'2'}}]}, true);

// Same make, same address -- caught before this change too.
check('modbus@2 + modbus@2', {
  driver:{id:'modbus_profile', options:{unit_id:'2'}},
  additional_devices:[{driver_id:'modbus_profile', options:{unit_id:'2'}}]}, true);

// Distinct addresses are fine, across drivers and within one.
check('modbus@2 + sunspec@3', {
  driver:{id:'modbus_profile', options:{unit_id:'2'}},
  additional_devices:[{driver_id:'sunspec', options:{unit_id:'3'}}]}, false);

// A driver with no address option cannot collide with one that has an address.
check('eversolar + modbus@2', {
  driver:{id:'eversolar_legacy', options:{}},
  additional_devices:[{driver_id:'modbus_profile', options:{unit_id:'2'}}]}, false);

// Two of them, neither storing an address -- so BOTH answer at the declared default of 16, and
// that is a real collision on a real bus. This case expected "no collision" until the check
// learned to resolve defaults, which is exactly the misconfiguration it could not see.
check('eversolar + eversolar, both at the default', {
  driver:{id:'eversolar_legacy', options:{}},
  additional_devices:[{driver_id:'eversolar_legacy', options:{}}]}, true);

// An empty field is not address "": the firmware falls back to the declared default, so two
// blanks are two devices on the SAME default. Also flipped by resolving defaults, and for the
// better -- leaving both blank is a bus where nothing answers, which is worth being told.
check('two blank addresses fall back to the same default', {
  driver:{id:'modbus_profile', options:{unit_id:''}},
  additional_devices:[{driver_id:'sunspec', options:{unit_id:''}}]}, true);

// Whitespace is not a different address.
check('modbus@2 + sunspec@" 2 "', {
  driver:{id:'modbus_profile', options:{unit_id:'2'}},
  additional_devices:[{driver_id:'sunspec', options:{unit_id:' 2 '}}]}, true);

// A driver that polls one device per bridge. Distinct addresses do not save it: planDevices()
// refuses the second row at boot regardless, so saying "no problem" here would be the page
// blessing a row that cannot start.
const excl = (label, body, want) => {
  const got = addressProblem(body);
  const said = !!got && got.includes('one device per bridge');
  if (said !== want) {
    console.error(`${label}: got ${JSON.stringify(got)}, wanted ${want ? 'a refusal' : 'none'}`);
    bad++;
  }
};
excl('two solax rows on different addresses', {
  driver:{id:'solax_x1', options:{address:'10'}},
  additional_devices:[{driver_id:'solax_x1', options:{address:'11'}}]}, true);
// One of it is the ordinary case and must stay allowed, or the driver is unusable.
excl('one solax row', {
  driver:{id:'solax_x1', options:{address:'10'}}, additional_devices:[]}, false);
// And it must not spill onto drivers that can share.
excl('two modbus_profile rows', {
  driver:{id:'modbus_profile', options:{unit_id:'2'}},
  additional_devices:[{driver_id:'modbus_profile', options:{unit_id:'3'}}]}, false);
// A driver that omits the field -- an older bridge, or any driver added before it existed.
// The benefit of the doubt is the documented default.
excl('a driver that says nothing either way', {
  driver:{id:'eversolar_legacy', options:{address:'16'}},
  additional_devices:[{driver_id:'eversolar_legacy', options:{address:'17'}}]}, false);

process.exit(bad === 0 ? 0 : 1);
"""
    )
    return run_node("address collision", "address_collision.js", harness, scratch)


def check_auth_prompt_reentrancy(script, scratch):
    """A second askAuth() while one is open must not disturb what is being typed.

    refresh() runs on a 5 s timer and several tabs call authFetch from it, so a second prompt
    IS requested while the first is still open -- that is normal operation, not an edge case.
    Until 0.15.2 the second call reset the username field and blanked the password field, so
    on the Logs tab the box emptied itself every five seconds and could not be filled in at
    all (reported from hardware 2026-07-26). Asserted here rather than left to review because
    the failure needs a stopwatch and an unauthenticated session to see by hand.
    """
    body = extract_function(script, "askAuth")
    if body is None:
        print("FAIL: askAuth() not found in index_html.h; this check needs updating")
        return 1
    harness = f"""
// Minimal DOM: only what askAuth touches.
const els = {{
  '#authdlg': {{returnValue:'', onclose:null, open:false,
                showModal(){{ if(this.open) throw new Error('InvalidStateError'); this.open=true }},
                close(v){{ this.open=false; this.returnValue=v; this.onclose && this.onclose() }}}},
  '#authpw': {{value:'', attrs:{{}},
               setAttribute(k,v){{ this.attrs[k]=String(v) }}, removeAttribute(k){{ delete this.attrs[k] }}}},
  '#authu': {{value:''}},
  // The error line is hidden by a class, not by an inline style. It needs a real enough
  // classList that a missing method is a failure here rather than a TypeError thrown inside
  // the promise executor -- which is how this check first reported the rename: three
  // assertions failed at once and none of them named the cause.
  '#autherr': {{hidden:true,
                classList:{{add(c){{ els['#autherr'].hidden = c==='hide' }},
                           remove(c){{ if(c==='hide') els['#autherr'].hidden=false }},
                           toggle(c,on){{ if(c==='hide') els['#autherr'].hidden=!!on }}}}}},
}};
const $ = s => els[s];
const store = {{}};
const sessionStorage = {{getItem:k=>k in store?store[k]:null, setItem:(k,v)=>{{store[k]=v}},
                         removeItem:k=>{{delete store[k]}}}};
const btoa = s => Buffer.from(s, 'binary').toString('base64');
let authPrompt = null;
{body}

let bad = 0;
const fail = m => {{ console.error(m); bad++; }};

(async () => {{
  const first = askAuth(false);
  await null;                       // let the prompt open
  if ('aria-describedby' in $('#authpw').attrs) fail('a first sign-in is described by the "not accepted" error');
  // The user starts typing.
  $('#authu').value = 'beheerder';
  $('#authpw').value = 'halfway-typed';

  // The 5 s refresh fires while they are mid-word.
  const second = askAuth(false);
  await null;

  if ($('#authpw').value !== 'halfway-typed') fail('password field was wiped by a second askAuth');
  if ($('#authu').value !== 'beheerder') fail('username field was reset by a second askAuth');
  if (!$('#authdlg').open) fail('dialog was closed by a second askAuth');

  // A third, this time signalling a refusal: allowed to show the error, not to touch the fields.
  askAuth(true);
  await null;
  if ($('#authpw').value !== 'halfway-typed') fail('retry=true wiped the password field');
  if ($('#autherr').hidden) fail('a refusal arriving while the dialog is open said nothing');
  // The field points at the error only while the error shows: a description is read even from a
  // hidden element.
  if ($('#authpw').attrs['aria-describedby'] !== 'autherr') fail('the refusal is not tied to the password field');

  // Finishing the dialog must resolve EVERY waiter, not only the last one.
  $('#authdlg').close('ok');
  const results = await Promise.all([first, second]);
  if (!results.every(r => r === true)) fail('not every caller received the answer: ' + JSON.stringify(results));
  if (store['hg_auth'] !== btoa('beheerder:halfway-typed')) fail('wrong credentials stored: ' + store['hg_auth']);

  // And the guard must release, so a later 401 can prompt again.
  const later = askAuth(false);
  await null;
  if (!$('#authdlg').open) fail('a later askAuth did not reopen the dialog');
  $('#authdlg').close('');
  await later;

  process.exit(bad === 0 ? 0 : 1);
}})();
"""
    return run_node("auth prompt re-entrancy", "auth_prompt.js", harness, scratch)


def check_auth_fetch_unreachable(script, scratch):
    """A request that gets no answer resolves as a failure; it never rejects.

    fetch() rejects when the bridge reboots mid-request, the WiFi drops or the timeout runs out,
    and none of the callers catch it: a save left "Applying…" on screen for good. authFetch turns
    that into the same not-ok response every caller already handles, carrying a reason.
    """
    auth_fetch = extract_function(script, "authFetch")
    if auth_fetch is None:
        print("FAIL: authFetch() not found in index_html.h; this check needs updating")
        return 1
    helpers = []
    for name in ("noAnswer", "unreachable"):
        m = re.search(r"^const " + name + r"=.*?;$", script, re.M | re.S)
        if m is None:
            print(
                f"FAIL: const {name} not found in index_html.h; this check needs updating"
            )
            return 1
        helpers.append(m.group(0))
    harness = (
        """
const store = {hg_auth:'GOOD'};
const sessionStorage = {getItem:k=>k in store?store[k]:null, setItem:(k,v)=>{store[k]=v},
                        removeItem:k=>{delete store[k]}};
const authHeader = () => ({'Authorization':'Basic GOOD'});
const clearAuth = () => {};
const rememberUser = () => {};
const authCancelled = () => ({ok:false,status:0,cancelled:true});
async function askAuth(){ return true; }
"""
        + "\n".join(helpers)
        + "\n"
        + auth_fetch
        + """
let bad = 0;
const fail = m => { console.error(m); bad++; };
(async () => {
  for (const [label, err] of [['dropped', new TypeError('Failed to fetch')],
                              ['timed out', Object.assign(new Error('t'), {name:'TimeoutError'})]]) {
    globalThis.fetch = async () => { throw err; };
    let r;
    try { r = await authFetch('/api/v1/config', {method:'PATCH'}); }
    catch (e) { fail(label + ': authFetch rejected instead of answering: ' + e); continue; }
    if (r.ok) fail(label + ': an unanswered request reported ok');
    if (!r.networkError) fail(label + ': not marked as a network error');
    const d = await r.json();
    if (!(d.error && /No answer from the bridge/.test(d.error.message))) {
      fail(label + ': no reason a caller can show: ' + JSON.stringify(d));
    }
    if (label === 'timed out' && !/timed out/.test(d.error.message)) fail('a timeout is not called one');
    if (!/took effect/.test(d.error.message)) fail(label + ': a lost write does not say to check it');
  }
  // A read that gets no answer changed nothing, so it must not tell anyone to check a change.
  globalThis.fetch = async () => { throw new TypeError('Failed to fetch'); };
  const g = await authFetch('/api/v1/config/backup');
  if (g.ok || !g.networkError) fail('a lost read is not reported as one');
  if (/took effect/.test((await g.json()).error.message)) fail('a lost read talks about a change');
  process.exit(bad === 0 ? 0 : 1);
})();
"""
    )
    return run_node(
        "auth fetch unreachable", "auth_fetch_unreachable.js", harness, scratch
    )


def check_auth_fetch_race(script, scratch):
    """A late 401 must not throw away credentials another caller just got accepted.

    Same root cause as the prompt bug: several tabs call authFetch from one 5 s refresh, so two
    requests can be in flight carrying the same stale credentials. The first 401 prompts, the
    password is accepted and stored -- and the second 401, arriving after, used to clear it and
    prompt again. To the user that is "I just signed in and it forgot".
    """
    parts = []
    for fn in ("authFetch", "askAuth"):
        body = extract_function(script, fn)
        if body is None:
            print(f"FAIL: {fn}() not found in index_html.h; this check needs updating")
            return 1
        parts.append(body)
    harness = (
        """
const store = {};
const sessionStorage = {getItem:k=>k in store?store[k]:null, setItem:(k,v)=>{store[k]=v},
                        removeItem:k=>{delete store[k]}};
const authHeader = () => store['hg_auth'] ? {'Authorization':'Basic '+store['hg_auth']} : {};
const clearAuth = () => { delete store['hg_auth'] };
const rememberUser = () => {};
const authCancelled = () => ({ok:false,status:0,cancelled:true});
const unreachable = e => ({ok:false,status:0,networkError:true});
const btoa = s => Buffer.from(s, 'binary').toString('base64');
let authPrompt = null;
let prompts = 0;

// The dialog is not exercised here; askAuth is replaced so the race is the only variable.
const realAskAuth = null;
"""
        + parts[0]
        + """
async function askAuth(retry){ prompts++; store['hg_auth'] = 'GOOD'; return true; }

// 401 for anything that is not the good token; 200 once it is.
let served = [];
const fetch = async (url, opts) => {
  const auth = (opts.headers||{})['Authorization'] || '';
  served.push(auth);
  return {status: auth === 'Basic GOOD' ? 200 : 401, ok: auth === 'Basic GOOD'};
};

let bad = 0;
const fail = m => { console.error(m); bad++; };

(async () => {
  // Both callers start with the same stale credentials, exactly as two tabs on one refresh do.
  store['hg_auth'] = 'STALE';
  const [a, b] = await Promise.all([authFetch('/api/v1/logs'), authFetch('/api/v1/diagnostics')]);

  if (!a.ok || !b.ok) fail(`both callers should end up authorised, got ${a.status} and ${b.status}`);
  if (store['hg_auth'] !== 'GOOD') fail('the accepted credentials were cleared again: ' + store['hg_auth']);
  if (prompts !== 1) fail(`the user was asked ${prompts} times; once is enough`);

  process.exit(bad === 0 ? 0 : 1);
})();
"""
    )
    return run_node("auth fetch race", "auth_fetch_race.js", harness, scratch)


def check_diag_single_flight(script, scratch):
    """Two callers asking for /diagnostics at once share one request.

    The first refresh starts it without waiting, and the Health tab awaits it when stale, so a
    first load that lands on Health used to send it twice. A failed request must clear the
    in-flight slot too, or the page never asks again.
    """
    body = extract_function(script, "loadDiag")
    if body is None:
        print("FAIL: loadDiag() not found in index_html.h; this check needs updating")
        return 1
    harness = (
        "let diag = null, diagAt = 0, diagP = null, sent = 0, fails = false;\n"
        "const getJson = () => { sent++; return fails ? Promise.reject(new Error('x'))"
        " : Promise.resolve({n: sent}); };\n"
        + body
        + """
let bad = 0;
const fail = m => { console.error(m); bad++; };
(async () => {
  await Promise.all([loadDiag(), loadDiag()]);
  if (sent !== 1) fail(`two concurrent callers sent ${sent} requests, want 1`);
  if (!diag) fail('diag not set after the shared request');
  await loadDiag();
  if (sent !== 2) fail(`a later call should send again, sent ${sent}`);
  fails = true;
  await loadDiag();
  fails = false;
  await loadDiag();
  if (sent !== 4) fail(`a failed request left the slot taken: sent ${sent}`);
  process.exit(bad === 0 ? 0 : 1);
})();
"""
    )
    return run_node(
        "diagnostics single flight", "diag_single_flight.js", harness, scratch
    )


def check_sse_lifecycle(script, scratch):
    """A hidden tab holds no SSE stream, a visible one holds exactly one, and retries back off.

    The bridge serves four SSE clients. A tab left in the background kept its slot and fetched
    /status every second for days; four of them locked every later visitor out of the stream.
    And a retry that fires after the tab already reconnected must not open a second stream.
    """
    parts = []
    for fn in ("connect", "retryConnect", "onVisibility"):
        body = extract_function(script, fn)
        if body is None:
            print(f"FAIL: {fn}() not found in index_html.h; this check needs updating")
            return 1
        parts.append(body)
    harness = (
        """
const document = {hidden: false};
let es = null, lastEvent = 0, sseRetry = 0, refreshes = 0;
const refresh = () => { refreshes++; };
const open = [];
class EventSource {
  constructor(url) { this.closed = false; this.l = {}; open.push(this); }
  addEventListener(k, f) { this.l[k] = f; }
  close() { this.closed = true; }
}
const timers = [];
const setTimeout = (f, ms) => { timers.push({f, ms}); };
const live = () => open.filter(e => !e.closed).length;
"""
        + "\n".join(parts)
        + """
let bad = 0;
const fail = m => { console.error(m); bad++; };

connect();
if (live() !== 1) fail(`visible page should hold one stream, holds ${live()}`);
connect();
if (live() !== 1) fail(`a second connect() opened another stream: ${live()}`);

document.hidden = true; onVisibility();
if (live() !== 0) fail(`hidden tab still holds ${live()} stream(s)`);
connect();
if (live() !== 0) fail('a retry firing while hidden opened a stream');

document.hidden = false; const r0 = refreshes; onVisibility();
if (live() !== 1) fail(`back to visible should hold one stream, holds ${live()}`);
if (refreshes !== r0 + 1) fail('back to visible should refresh at once');

// Errors back off: 5-10 s, then 10-20 s, ... capped at 60 s.
const delays = [];
for (let i = 0; i < 6; i++) {
  open[open.length - 1].onerror();
  delays.push(timers.pop().ms);
  connect();
}
if (live() !== 1) fail(`after retries there should be one stream, holds ${live()}`);
if (!(delays[0] >= 5000 && delays[0] <= 10000)) fail(`first retry ${delays[0]} ms, want 5-10 s`);
if (!(delays[1] >= 10000)) fail(`second retry ${delays[1]} ms did not back off`);
if (delays.some(d => d > 60000)) fail(`a retry exceeded 60 s: ${delays}`);

// A successful open resets the backoff.
open[open.length - 1].l.open();
open[open.length - 1].onerror();
const reset = timers.pop().ms;
if (reset > 10000) fail(`backoff not reset after open: ${reset} ms`);

// An error on a stream closed on purpose (tab hidden) schedules nothing.
connect(); const s = open[open.length - 1];
document.hidden = true; onVisibility(); const t0 = timers.length;
s.onerror();
if (timers.length !== t0) fail('an error on a deliberately closed stream scheduled a retry');

// A constructor that throws still schedules a retry, or the page sits on the fallback poll
// until the next visibility change.
const Real = EventSource;
EventSource = class { constructor() { throw new Error('nope'); } };
document.hidden = false; es = null; const t1 = timers.length;
connect();
if (timers.length !== t1 + 1) fail('a throwing EventSource scheduled no retry');
EventSource = Real;

process.exit(bad === 0 ? 0 : 1);
"""
    )
    return run_node("sse lifecycle", "sse_lifecycle.js", harness, scratch)


def check_state_event_gate(script, scratch):
    """An SSE `state` event refetches /status only when a poll landed or the data is 5 s old.

    The event fires about once a second; the inverter is polled every few seconds. Refetching on
    every event was ~2.4 KB a second of JSON built on the board for nothing new. The gate must
    still refetch on a new poll at once, on the 5 s floor, and on an event with no count at all
    -- older firmware sends none, and a page that went quiet on it would look frozen.
    """
    body = extract_function(script, "stateWantsRefresh")
    if body is None:
        print(
            "FAIL: stateWantsRefresh() not found in index_html.h; this check needs updating"
        )
        return 1
    harness = (
        "let lastFetch = 0, lastPolls = null;\n"
        + body
        + """
let bad = 0;
const fail = m => { console.error(m); bad++; };
const ev = n => JSON.stringify({polls: n});
const want = (data, now, exp, what) => {
  const got = stateWantsRefresh(data, now);
  if (got) lastFetch = now;  // what refresh() does
  if (got !== exp) fail(`${what}: expected ${exp}, got ${got}`);
};

want(ev(7), 1000, true, 'first event');
want(ev(7), 2000, false, 'same poll count, 1 s later');
want(ev(7), 4000, false, 'same poll count, 3 s later');
want(ev(8), 4500, true, 'a poll landed');
want(ev(8), 5500, false, 'same count again');
want(ev(8), 9500, true, '5 s floor since the last fetch');
want('', 9600, true, 'older firmware: no data');
want('', 9700, true, 'older firmware: every event');
want('not json', 9800, true, 'garbage data');
want(ev(8), 9900, true, 'count back after a dataless event');
want(ev(8), 10000, false, 'and then settled again');

process.exit(bad === 0 ? 0 : 1);
"""
    )
    return run_node("state event gate", "state_event_gate.js", harness, scratch)


def check_bridge_clock(script, scratch):
    """The Bridge-tab clock is the last reading plus the time since it arrived.

    /status is no longer fetched every second, so the clock ticks in the page. It must still say
    "not synced" before NTP, and must not turn a reading it cannot parse into "NaN-NaN-NaN".
    """
    body = extract_function(script, "clockText")
    if body is None:
        print("FAIL: clockText() not found in index_html.h; this check needs updating")
        return 1
    harness = (
        "let S = null, sAt = Date.now() - 5000;\n"
        + body
        + """
let bad = 0;
const fail = m => { console.error(m); bad++; };
const is = (want, what) => { const got = clockText(); if (got !== want) fail(`${what}: ${got}`); };
S = {bridge: {time_synced: true, time: '2026-11-06 23:59:58'}};
is('2026-11-07 00:00:03', 'reading plus 5 s, across midnight');
S = {bridge: {time_synced: false, time: '2026-11-06 23:59:58'}};
is('not synced', 'before NTP');
S = {bridge: {time_synced: true, time: 'garbage'}};
is('garbage', 'unparseable reading shown as is');
S = null;
is('not synced', 'no status yet');
process.exit(bad === 0 ? 0 : 1);
"""
    )
    return run_node("bridge clock", "bridge_clock.js", harness, scratch)


if __name__ == "__main__":
    sys.exit(main())
