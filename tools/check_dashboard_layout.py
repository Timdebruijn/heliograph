#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Renders the dashboard in a real browser and asserts its LAYOUT, not its pixels.
#
# 0.18.0 shipped a table that scrolled sideways and broke every cell onto a second line, and
# nothing caught it: the tests assert what the page SAYS, never how it comes out. It was found
# by eye, on hardware, minutes after the tag.
#
# Deliberately not a screenshot comparison. A committed baseline image would have to survive a
# different font stack on a CI runner than on the machine that generated it, and a check that
# cries wolf gets switched off -- which is worse than not having it. What broke in 0.18.0 was a
# layout INVARIANT ("a row is one line tall"), and an invariant can be asserted directly:
# relative geometry, no reference image, no font dependency.
#
# The page asserts itself. A script appended to the document runs the checks against real
# computed geometry and writes the verdict into the DOM; Chrome is asked for the rendered DOM
# and this reads the verdict out. That avoids a DevTools-protocol client or a headless-browser
# library -- a dependency this project does not otherwise need, for a job that one <script>
# already does.

import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

SOURCE = "src/web/assets/index_html.h"

# Phone, small tablet, laptop. The first two are where a six-column strip cannot fit and the
# layout has to hold up by scrolling rather than by folding.
WIDTHS = (390, 700, 1200)

# The payload that broke it: a hybrid reporting a battery, which is what makes the widest
# column appear at all. Two devices so a mismatch in row height is visible as a difference and
# not only as an absolute.
#
# WIDTHS matter more than anything else here, and this is the thing the first version of this
# file got wrong. Rendered at 900px the broken 0.18.0 table PASSED -- its min-width was 860, so
# it fitted and never had to wrap. The bug only bites where the table is asked to fit into less
# room than its content needs, which is the phone and tablet case it was reported on. Checking
# one comfortable width proves nothing; the narrow ones are the whole point.
FLEET = """[
  {"id":"inverter-A","label":"Dak huis","online":true,"data_valid":true,"data_stale":false,
   "last_successful_poll_seconds_ago":2,"ac_power_w":2310,"energy_today_kwh":12.5,
   "ac_voltage_v":230.4,"temperature_c":41.2,"battery_soc_pct":64,"battery_power_w":2450},
  {"id":"inverter-B","label":"Schuur achter met een lange naam","online":true,
   "data_valid":true,"data_stale":false,"last_successful_poll_seconds_ago":12,
   "ac_power_w":880,"energy_today_kwh":4.25,"ac_voltage_v":229.1,"temperature_c":38.0,
   "battery_soc_pct":40,"battery_power_w":-1180}
]"""

PAGE = """<!doctype html><html><head><meta charset="utf-8"><style>%(style)s</style></head>
<body><div id="host" style="width:%(width)spx"></div>
<script>
const fmt=(v,d)=>Number(v).toFixed(d);
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
%(script)s
document.getElementById('host').innerHTML=fleetStrip(%(fleet)s);

const fail=[];
const strip=document.querySelector('#host table');
if(!strip){ fail.push('no table rendered'); }
else {
  // How many LINES does each cell's text occupy? A Range over the cell's contents returns one
  // client rect per line box, so this counts wrapping directly -- no height ratio, no pixel
  // tolerance, nothing that depends on the runner's font metrics.
  //
  // The first version of this file compared cell HEIGHTS against the shortest cell and allowed
  // 1.6x. That silently passed the real 0.18.0 regression: padding does not double when the
  // text does, so a wrapped cell measured 74px against 57px -- 1.3x, comfortably inside the
  // tolerance. A check that lets the bug it was written for through is worse than no check,
  // which is why this one is mutation-tested against the broken revision.
  //
  // Rects are MERGED BY VERTICAL OVERLAP, not bucketed by position. A cell holding the
  // "Last reply" pill yields two rects on one line -- the span's padded box at top 79 height
  // 19, and its text at top 82 height 13 -- which overlap and are plainly the same line. An
  // earlier version rounded tops into buckets and split those two into "2 lines", failing a
  // page that was correct. Overlap is what "same line" means; rounding is a guess at it.
  const lineCount=el=>{
    const r=document.createRange();
    r.selectNodeContents(el);
    const rects=[...r.getClientRects()]
      .filter(x=>x.width>0 && x.height>0)
      .sort((a,b)=>a.top-b.top);
    let lines=0, bottom=-Infinity;
    for(const x of rects){
      if(x.top>=bottom){ lines++; bottom=x.bottom; }
      else { bottom=Math.max(bottom,x.bottom); }
    }
    return Math.max(lines,1);
  };
  // Numbers and headers. Not the device-name cell: it is DELIBERATELY two lines when a device
  // has a label, with the id underneath in small type.
  const cells=[...strip.querySelectorAll('td.n'), ...strip.querySelectorAll('th')];
  if(cells.length===0){ fail.push('no cells found to measure'); }
  for(const c of cells){
    const lines=lineCount(c);
    if(lines>1){
      fail.push('wraps onto '+lines+' lines: "'+c.textContent.trim().slice(0,40)+'"');
    }
  }
  // The strip is allowed to scroll sideways -- that is a deliberate decision. The PAGE is not:
  // a document that scrolls horizontally means something escaped its container.
  if(document.documentElement.scrollWidth > document.documentElement.clientWidth){
    fail.push('the document scrolls horizontally: '
      +document.documentElement.scrollWidth+' > '+document.documentElement.clientWidth);
  }
  // The scroll must live on the wrapper, not on the card, or the legend below it slides out of
  // view with the table.
  const scroller=strip.parentElement;
  if(getComputedStyle(scroller).overflowX!=='auto'){
    fail.push('the table is not inside a horizontally scrollable wrapper');
  }
}
document.title = fail.length ? 'LAYOUT-FAIL ' + fail.join(' || ') : 'LAYOUT-OK';
</script></body></html>"""


def find_chrome() -> str | None:
    for name in ("google-chrome", "google-chrome-stable", "chromium", "chromium-browser"):
        found = shutil.which(name)
        if found:
            return found
    mac = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    return mac if pathlib.Path(mac).exists() else None


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    source = (root / SOURCE).read_text()

    style = re.search(r"<style>(.*?)</style>", source, re.S)
    scripts = re.findall(r"<script>(.*?)</script>", source, re.S)
    if not style or not scripts:
        print("dashboard layout: FAIL (no <style> or <script> found)")
        return 1
    joined = "\n".join(scripts)
    start = joined.find("function fleetStrip(fleet){")
    if start < 0:
        print("dashboard layout: FAIL (fleetStrip not found)")
        return 1
    end = joined.find("\nfunction ", start + 10)
    fleet_strip = joined[start:end if end > 0 else len(joined)]

    chrome = find_chrome()
    if chrome is None:
        # Loud, not skipped. A layout check that quietly does nothing when the browser is
        # missing reports a clean tree it never rendered -- which is the failure this whole
        # file exists to stop happening again.
        print("dashboard layout: FAIL (no Chrome/Chromium on PATH to render with)")
        return 1

    status = 0
    with tempfile.TemporaryDirectory(prefix="heliograph-layout-") as scratch:
        for width in WIDTHS:
            page = pathlib.Path(scratch) / f"dashboard-{width}.html"
            page.write_text(PAGE % {
                "style": style.group(1), "script": fleet_strip,
                "fleet": FLEET, "width": width,
            })
            result = subprocess.run(
                [
                    chrome, "--headless", "--disable-gpu", "--no-sandbox",
                    f"--window-size={width},700", "--virtual-time-budget=2000",
                    "--dump-dom", page.as_uri(),
                ],
                capture_output=True, text=True, timeout=120,
            )
            verdict = re.search(r"<title>(LAYOUT-[^<]*)</title>", result.stdout)
            if verdict is None:
                print(f"dashboard layout @{width}px: FAIL (no verdict; the page did not run)")
                print(result.stderr[-600:])
                status = 1
                continue
            text = verdict.group(1)
            if text.startswith("LAYOUT-OK"):
                print(f"dashboard layout @{width}px: OK")
                continue
            status = 1
            print(f"dashboard layout @{width}px: FAIL")
            for problem in text.removeprefix("LAYOUT-FAIL ").split(" || "):
                print("  " + problem)
    return status


if __name__ == "__main__":
    sys.exit(main())
