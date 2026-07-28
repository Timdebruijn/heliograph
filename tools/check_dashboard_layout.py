#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Renders the Live tab in headless Chrome and asserts its LAYOUT and its battery semantics.
#
# WHY A BROWSER
#
# The rest of the suite asserts what the page SAYS. 0.18.0 shipped a fleet strip that said all
# the right things and broke every row onto a second line, and nothing caught it; 0.19.1 shipped
# one that fitted and scrolled sideways anyway. Both are questions only a layout engine can
# answer, so this one boots a real one.
#
# WHAT IT RENDERS
#
# The page as the DEVICE SERVES IT: pulled out of the raw string literal and put through
# tools/build_web.py's stripper, which is what gzips into flash. Rendering the authored source
# instead would leave the one transformation between source and screen untested -- and that
# stripper deletes lines for a living.
#
# The fleet comes from tools/demo_fleet.js: three inverters, one of which never replied. No
# bridge on this desk has that fleet, which is the point.
#
# No browser is installed by this script on purpose. It fails loudly when it cannot find one, so
# a runner image that stops shipping Chrome shows up as a red check rather than as a layout
# check that quietly stopped rendering anything.

import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import build_web  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Phone, small tablet, laptop, desktop. The narrow ones are where a row cannot fit and the
# layout has to hold up by wrapping rather than by overflowing -- they are what catches the
# 0.18.0 class of bug. 1000 and 1200 guard the opposite failure, the one Tim reported after
# 0.19.1: something that only misbehaves when there is room to spare.
WIDTHS = (390, 700, 780, 1000, 1200)

# Battery states, and what each must be able to say for itself. Rendered from the same page at
# one width, because these are semantics and not layout.
#
# Direction is carried by an ARROW as well as a colour, so it survives a reader who cannot tell
# the red from the green, and the sign of the number never has to be decoded. Asserting only the
# colour would let a future change drop the arrow and leave the meaning in a hue.
BATTERY_CASES = [
    # (label, soc, power, must contain, must NOT contain)
    ("discharging", 68, -1240, ["↑", "var(--ok)", 'title="discharging"'], ["-1240"]),
    ("charging", 41, 1500, ["↓", "var(--bad)", 'title="charging"'], []),
    # A trickle that rounds away must not claim a direction next to a zero.
    ("idle", 55, 0.4, ["idle"], ["charging at 0 W", "discharging at 0 W"]),
]

# Runs after the page has painted. Verdict goes in document.title, which --dump-dom gives back.
#
# Scoped in an IIFE, and not for tidiness: the page has its own top-level say(), and a bare
# `const say` here is a SyntaxError that kills the whole script -- which reports as "the page
# did not run" and points at everything except the collision.
ASSERT_JS = r"""
(function(){
const fail = [];
const say = m => fail.push(m);

// How many LINES an element's text occupies. Not getClientRects().length: an element holding
// several inline children yields one rect per child, so a pill that reads as one line counts as
// two. Rects that overlap vertically are the same line.
function lines(el){
  const rects=[...el.getClientRects()].filter(r=>r.width>0&&r.height>0);
  const rows=[];
  for(const r of rects.sort((a,b)=>a.top-b.top)){
    const row=rows.find(x=>Math.min(x.bottom,r.bottom)-Math.max(x.top,r.top) > r.height*0.5);
    if(row){ row.top=Math.min(row.top,r.top); row.bottom=Math.max(row.bottom,r.bottom); }
    else rows.push({top:r.top,bottom:r.bottom});
  }
  return rows.length;
}

function check(){
  const rows=[...document.querySelectorAll('.fleetrow')];
  if(rows.length!==3){ say('expected 3 fleet rows, rendered '+rows.length); return true; }

  // THE original complaint, and the one thing that must hold at every width: the page itself
  // may not scroll sideways. Asserted on the document rather than on the strip, because "I have
  // to scroll right to read it" is a property of the page, not of whichever element caused it.
  const over = document.documentElement.scrollWidth - document.documentElement.clientWidth;
  if(over > 1) say('the page scrolls sideways by '+over+'px');

  for(const row of rows){
    // A row may WRAP -- it is a flex container and that is how it survives a phone. What it may
    // not do is break a single value onto two lines: "1 840" over two lines is the 0.18.0 bug
    // wearing different markup.
    for(const v of row.querySelectorAll('.num b, .name b')){
      if(lines(v)>1) say('a value wraps onto '+lines(v)+' lines: "'+v.textContent.trim()+'"');
    }
    if(row.scrollWidth > row.clientWidth + 1){
      say('a fleet row overflows its own box: '+row.scrollWidth+' > '+row.clientWidth);
    }
  }

  // The inverter that never answered has the most to say and the least to show. It must be
  // marked as such rather than rendered as a row of dashes indistinguishable from a zero.
  const silent=rows.find(r=>r.textContent.includes('Dak achter'));
  if(!silent) say('the never-answering inverter is not on screen');
  else{
    if(!silent.classList.contains('bad')) say('the never-answering inverter is not marked');
    if(!silent.textContent.includes('never answered')){
      say('the never-answering inverter does not say so: "'+silent.textContent.trim().slice(0,60)+'"');
    }
  }

  // The legend became load-bearing the moment the word came out of the cell: it is the only
  // place the arrows are explained. A battery on screen without it is undecodable.
  const card=[...document.querySelectorAll('.card')].find(c=>c.querySelector('.soc'));
  if(!card) say('no battery card for a fleet that reports one');
  else if(!card.textContent.includes('power going into the battery')){
    say('the battery renders without the legend that explains its arrows');
  }
  return true;
}

// The page fetches before it paints. Poll rather than guess a delay: a fixed timeout that is
// slightly too short reports "nothing rendered" for a page that was about to.
let tries=0;
const tick=setInterval(()=>{
  if(document.querySelector('.fleetrow') || ++tries>60){
    clearInterval(tick);
    try{ check(); }catch(e){ say('the assertions threw: '+e.message); }
    document.title = fail.length ? 'LAYOUT-FAIL '+fail.join(' || ') : 'LAYOUT-OK';
  }
}, 25);
})();
"""


def find_chrome() -> str | None:
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


def build_page(stripped: str, stub: str, battery: str, extra_js: str) -> str:
    """The served page with the stub attached, and the assertions after it.

    The stub goes in BEFORE the page's own <script>: it replaces window.fetch, and a page that
    has already fired its first request would answer from the network instead.
    """
    prelude = (
        f"<script>window.__demoBattery={battery};</script>\n<script>{stub}</script>\n"
    )
    at = stripped.find("<script>")
    if at < 0:
        raise SystemExit("no <script> in the page; this check needs updating")
    page = stripped[:at] + prelude + stripped[at:]
    return page.replace("</body>", f"<script>{extra_js}</script></body>", 1)


def render(
    chrome: str, page: str, width: int, scratch: str, tag: str
) -> tuple[str, str]:
    path = pathlib.Path(scratch) / f"page-{tag}.html"
    path.write_text(page, encoding="utf-8")
    result = subprocess.run(
        [
            chrome,
            "--headless",
            "--disable-gpu",
            "--no-sandbox",
            f"--window-size={width},900",
            "--virtual-time-budget=6000",
            "--dump-dom",
            path.as_uri(),
        ],
        capture_output=True,
        text=True,
        timeout=120,
    )
    verdict = re.search(r"<title>(LAYOUT-[^<]*)</title>", result.stdout)
    return (verdict.group(1) if verdict else "", result.stdout)


def report(label: str, verdict: str) -> int:
    if verdict.startswith("LAYOUT-OK"):
        print(f"{label}: OK")
        return 0
    print(f"{label}: FAIL")
    if not verdict:
        print("  no verdict; the page did not run")
        return 1
    for problem in verdict.removeprefix("LAYOUT-FAIL ").split(" || "):
        print("  " + problem)
    return 1


def main() -> int:
    stripped = build_web.strip_comments(
        build_web.extract(build_web.assets() / "index_html.h")
    )
    stub = (ROOT / "tools" / "demo_fleet.js").read_text(encoding="utf-8")

    chrome = find_chrome()
    if chrome is None:
        print("dashboard layout: FAIL (no Chrome/Chromium on PATH to render with)")
        return 1

    status = 0
    with tempfile.TemporaryDirectory(prefix="heliograph-layout-") as scratch:
        page = build_page(stripped, stub, "{soc:68,power:-1240}", ASSERT_JS)
        for width in WIDTHS:
            verdict, _ = render(chrome, page, width, scratch, str(width))
            status |= report(f"dashboard layout @{width}px", verdict)

        for label, soc, power, wanted, unwanted in BATTERY_CASES:
            # The DOM itself is the subject here, so the assertions read it as text rather than
            # measuring it. Rendered at one comfortable width: these are semantics, and running
            # them at five widths would only make a failure five times as loud.
            js = (
                "(function(){\n"
                "const fail=[];const say=m=>fail.push(m);\n"
                "let tries=0;const tick=setInterval(()=>{\n"
                "  if(document.querySelector('.soc')||++tries>60){clearInterval(tick);\n"
                "    const c=[...document.querySelectorAll('.card')].find(x=>x.querySelector('.soc'));\n"
                "    const h=c?c.innerHTML:'';\n"
                "    if(!c)say('no battery card rendered');\n"
                f"    for(const w of {wanted!r}) if(!h.includes(w)) say('missing: '+w);\n"
                f"    for(const w of {unwanted!r}) if(h.includes(w)) say('present but must not be: '+w);\n"
                "    document.title=fail.length?'LAYOUT-FAIL '+fail.join(' || '):'LAYOUT-OK';}\n"
                "},25);})();"
            )
            page = build_page(stripped, stub, f"{{soc:{soc},power:{power}}}", js)
            verdict, _ = render(chrome, page, 1000, scratch, label)
            status |= report(f"battery {label}", verdict)

    return status


if __name__ == "__main__":
    sys.exit(main())
