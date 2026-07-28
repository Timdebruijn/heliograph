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
                script = path.read_text()
                status |= check_version_compare(script, scratch)
                status |= check_auth_prompt_reentrancy(script, scratch)
                status |= check_auth_fetch_race(script, scratch)
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
  '#authpw': {{value:''}}, '#authu': {{value:''}},
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
    path = pathlib.Path(scratch) / "auth_prompt.js"
    path.write_text(harness)
    result = subprocess.run(["node", str(path)], capture_output=True, text=True)
    print(f"auth prompt re-entrancy: {'OK' if result.returncode == 0 else 'FAIL'}")
    if result.returncode != 0:
        print(result.stdout + result.stderr)
        return 1
    return 0


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
    path = pathlib.Path(scratch) / "auth_fetch_race.js"
    path.write_text(harness)
    result = subprocess.run(["node", str(path)], capture_output=True, text=True)
    print(f"auth fetch race: {'OK' if result.returncode == 0 else 'FAIL'}")
    if result.returncode != 0:
        print(result.stdout + result.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
