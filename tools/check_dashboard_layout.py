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
# TWO questions, two answers, and the card must not confuse them. The DIRECTION line says which
# way the charge is heading: up and green gaining, down and red giving back. The BAR says how
# much is left, coloured by level alone. One variable used to drive both, so a full battery that
# happened to be charging drew a red bar.
#
# Direction is carried by an ARROW as well as a colour, so it survives a reader who cannot tell
# the red from the green, and the sign of the number never has to be decoded. Asserting only the
# colour would let a future change drop the arrow and leave the meaning in a hue.
BATTERY_CASES = [
    # (label, soc, power, must contain, must NOT contain)
    # Discharging at a comfortable level: red on the line, green on the bar. Both appear, which
    # is exactly what a single shared colour could not produce -- so this case alone would have
    # failed before the split.
    (
        "discharging",
        68,
        -1240,
        ["↓", "var(--bad)", 'title="discharging"', "var(--ok)"],
        ["-1240"],
    ),
    # Charging while nearly empty: green on the line, red on the bar. The mirror image.
    ("charging", 12, 1500, ["↑", "var(--ok)", 'title="charging"', "var(--bad)"], []),
    # A trickle that rounds away must not claim a direction next to a zero.
    ("idle", 55, 0.4, ["idle"], ["charging at 0 W", "discharging at 0 W"]),
    # The middle band, so the amber threshold is not left to inspection.
    ("half full", 35, -200, ["var(--warn)"], []),
    # A device reporting power but no state of charge. Asserted on the BAR's own markup, not on
    # the card's text: this one is discharging, so var(--bad) is legitimately present on the
    # direction line and a whole-card search would have passed for the wrong reason.
    #
    # The fill is invisible here -- an unknown level draws a rect of zero width, same as a
    # genuine 0% -- and what tells them apart on screen is the em dash where the number goes.
    # Pinned anyway: emitting "critically low" for a value nobody supplied is wrong wherever it
    # ends up, and it stops being invisible the day this bar grows a minimum-width sliver.
    (
        "state of charge unknown",
        "null",
        -1240,
        ['height="8" fill="var(--dim)"'],
        ['height="8" fill="var(--bad)"'],
    ),
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

  // The legend became load-bearing the moment the word came out of the cell, and it now has
  // TWO things to carry: what the arrow means, and that the bar answers a different question.
  // Asserted separately, because dropping either half leaves a card that looks explained.
  const card=[...document.querySelectorAll('.card')].find(c=>c.querySelector('.soc'));
  if(!card) say('no battery card for a fleet that reports one');
  else{
    const legend=card.textContent;
    if(!legend.includes('running on stored sun')){
      say('the battery renders without the legend that explains its arrows');
    }
    if(!legend.includes('level and not the direction')){
      say('the legend does not say the bar colour is the level rather than the direction');
    }
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


# The Inverters tab, which is where the page stops reporting and starts accepting input. Both
# of these were reported from a real bridge on 2026-07-29 and neither is a layout question --
# they are here because this is the only check that runs the actual page in a browser.
INVERTERS_JS = r"""
(function(){
const fail=[];
const say=m=>fail.push(m);
const done=()=>{document.title=fail.length?'LAYOUT-FAIL '+fail.join(' || '):'LAYOUT-OK'};
let tries=0;
const tick=setInterval(async()=>{
  if(!document.querySelector('.fleetrow') && ++tries<=80) return;
  clearInterval(tick);
  try{
    goTab('inv');
    await new Promise(r=>setTimeout(r,300));

    // 1. "Add one by hand" must send a row the firmware will ACCEPT. It used to send
    // {driver_id:''}, which validate() refuses with "must name a driver" -- so the button did
    // nothing but raise an error, every time.
    let sent=null;
    const realPatch=window.patch;
    window.patch=async b=>{sent=b;return{ok:true,body:{}}};
    const realAlert=window.alert;
    window.alert=()=>{};
    await addExtra();
    window.patch=realPatch; window.alert=realAlert;
    if(!sent||!sent.additional_devices) say('adding a row by hand sent no device list');
    else{
      const row=sent.additional_devices[sent.additional_devices.length-1];
      if(!row.driver_id) say('the new row names no driver, which the firmware refuses outright');
      // And it must not land on an address something else already answers at: two units
      // sharing one destroy each other's replies, and the row would be skipped at boot.
      // Resolved the way the FIRMWARE resolves it: stored option, else the declared default
      // (DriverDescriptor::optionOr). Written out here rather than calling the page's own
      // addressOf(), so that a page which resolves it wrongly cannot agree with itself.
      //
      // The earlier version of this assertion read stored options only -- the same blind spot as
      // the code it was testing -- so it watched a real bridge hand a second inverter the
      // address the first was already answering at, and said nothing.
      const addrOf=(driverId,options)=>{
        const drv=((drivers&&drivers.drivers)||[]).find(x=>x.id===driverId)||{};
        const opt=(drv.options||[]).find(o=>o.key==='unit_id'||o.key==='address');
        if(!opt)return '';
        const stored=String((options||{})[opt.key]??'').trim();
        return stored!==''?stored:String(opt.default_value??'').trim();
      };
      const mine=addrOf(row.driver_id,row.options);
      if(mine!==''){
        const others=[[cfg.driver.id,cfg.driver.options],
                      ...sent.additional_devices.slice(0,-1).map(d=>[d.driver_id,d.options])]
          .map(([i,o])=>addrOf(i,o));
        if(others.includes(mine)) say('the new row was given address '+mine+', already in use');
      }
    }

    // 1b. A row that is in the configuration but has not started must be VISIBLE and must
    // offer a way back out. It has no device id, so it gets no ordinary card -- which is how a
    // freshly added row became impossible to remove without a reboot, on a bridge whose owner
    // had just been told to open a panel that did not exist.
    cfg.additional_devices=[...(cfg.additional_devices||[]),
                            {driver_id:'growatt_modbus',label:'',options:{unit_id:'9'}}];
    paintInverters();
    await new Promise(r=>setTimeout(r,100));
    const inv=document.getElementById('inv');
    if(!inv.textContent.includes('starts after a restart')){
      say('a configured row that has not started is nowhere on the page');
    }
    if(![...inv.querySelectorAll('button')].some(b=>/removeExtraAt/.test(b.getAttribute('onclick')||''))){
      say('a configured row that has not started offers no way to remove it');
    }

    // 1c. Which rows are pending is decided by DRIVER, not by position. planDevices refuses a
    // row whose driver cannot share a bus and lets later ones through, so counting started
    // devices names the wrong row -- and its Remove button would take out an inverter that is
    // running perfectly well.
    cfg.additional_devices=[{driver_id:'growatt_modbus',label:'Garage',options:{unit_id:'2'}},
                            {driver_id:'eversolar_legacy',label:'Refused',options:{address:'17'}},
                            {driver_id:'growatt_modbus',label:'Dak achter',options:{unit_id:'3'}}];
    paintInverters();
    await new Promise(r=>setTimeout(r,150));
    const pend=[...document.querySelectorAll('#inv .card')]
      .filter(c=>c.textContent.includes('starts after a restart'));
    if(pend.length!==1) say('expected one pending row, drew '+pend.length);
    else{
      if(!pend[0].textContent.includes('Refused')){
        say('the pending card names a running inverter: "'+pend[0].querySelector('b').textContent+'"');
      }
      const btn=pend[0].querySelector('button[onclick^="removeExtraAt"]');
      if(!btn||btn.getAttribute('onclick')!=='removeExtraAt(1)'){
        say('the remove button points at the wrong configuration row: '+(btn&&btn.getAttribute('onclick')));
      }
    }

    // 1d. An OPEN PANEL is work in progress. A raw-bus recording runs for thirty seconds and a
    // firmware upload longer, and neither holds focus -- so the focus guard alone left both
    // being rebuilt every few seconds while they ran.
    togglePanel('cap');
    await new Promise(r=>setTimeout(r,200));
    const mark=document.createElement('span'); mark.id='panel-probe';
    document.getElementById('inv').appendChild(mark);
    for(let i=0;i<5;i++){ S.totals.ac_power_w=2000+i; paint(); await new Promise(r=>setTimeout(r,20)); }
    if(!document.getElementById('panel-probe')) say('the tab was rebuilt under an open panel');
    // And it must resume the moment that panel is closed, or the tab is frozen for good.
    togglePanel('cap');
    await new Promise(r=>setTimeout(r,150));
    const mark2=document.createElement('span'); mark2.id='panel-probe-2';
    document.getElementById('inv').appendChild(mark2);
    S.totals.ac_power_w=9999; paint();
    await new Promise(r=>setTimeout(r,60));
    if(document.getElementById('panel-probe-2')) say('the tab never repainted again after closing the panel');

    // 2. A control being used must survive the five-second refresh. Every paint replaces the
    // tab's innerHTML, which shuts an open <select> -- so the driver list could not be read to
    // the bottom before it closed itself.
    togglePanel('wiz'); wizStep=4; paintInverters();
    await new Promise(r=>setTimeout(r,100));
    const sel=document.querySelector('#wizcard select');
    if(!sel) say('the wizard offers no driver dropdown to choose from');
    else{
      sel.focus();
      paint();
      await new Promise(r=>setTimeout(r,50));
      if(document.querySelector('#wizcard select')!==sel){
        say('a refresh replaced the driver dropdown while it was being used');
      }
      // The other half: the guard must RELEASE. A tab that stops updating once anything was
      // ever touched is a worse bug than the one being fixed.
      //
      // Both guards have to come off, and this assertion predates the second one: blurring the
      // dropdown leaves the WIZARD open, and an open panel is deliberately left alone now. So
      // close that too, or this asserts a repaint that is correctly refused.
      sel.blur();
      togglePanel('wiz');
      await new Promise(r=>setTimeout(r,50));
      const probe=document.createElement('span'); probe.id='release-probe';
      document.getElementById('inv').appendChild(probe);
      paint();
      await new Promise(r=>setTimeout(r,50));
      if(document.getElementById('release-probe')){
        say('the tab never repainted again after the control was left alone');
      }
    }

    // 3. And it must not release too little. A BUTTON keeps focus after it is clicked, so
    // counting one as "busy" would have frozen the Live tab the moment somebody opened it --
    // a worse and quieter bug than the dropdown it was meant to fix.
    goTab('live');
    await new Promise(r=>setTimeout(r,200));
    const nav=document.querySelector('nav button[data-t="live"]');
    if(!nav) say('no nav button to test focus against');
    else{
      nav.focus();
      const marker=document.createElement('span');
      $('#live').appendChild(marker);
      paint();
      await new Promise(r=>setTimeout(r,50));
      if(marker.isConnected) say('a focused nav button stopped the Live tab from updating');
    }
  }catch(e){ say('the interaction assertions threw: '+e.message); }
  done();
},25);
})();
"""


# A bridge that has just been provisioned: on the network, admin password set, and nothing ever
# said about the RS485 side. The firmware still starts its highest-priority driver, which never
# answers -- so from the poll results alone this is indistinguishable from a miswired bus.
#
# It used to be shown as one: three named wiring causes, A/B swapped at the top, on the very
# first screen after setup. Everything here exists so that the page reads the CONFIG instead and
# invites rather than accuses.
FRESH_BRIDGE_JS = r"""
(function(){
const fail=[];
const say=m=>fail.push(m);
let tries=0;
const tick=setInterval(async()=>{
  if(!cfg && ++tries<=80) return;
  clearInterval(tick);
  try{
    if(!neverConfigured()) say('the fresh-bridge stub was not recognised as unconfigured');
    for(const tab of ['live','inv']){
      goTab(tab);
      await new Promise(r=>setTimeout(r,250));
      const body=document.getElementById(tab).textContent;
      if(!body.includes('No inverter set up yet')){
        say(tab+' does not say an inverter still has to be set up');
      }
      // The wiring diagnosis is for a bus somebody has connected. Naming a fault on one that
      // was never wired is the accusation this whole branch exists to stop.
      for(const accusation of ['A and B swapped','Termination in the wrong place','never replied']){
        if(body.includes(accusation)) say(tab+' blames the wiring: "'+accusation+'"');
      }
    }
    // Live must offer a way in rather than a dead headline.
    goTab('live');
    await new Promise(r=>setTimeout(r,250));
    if(!document.querySelector('#live button')) say('Live offers nothing to do next');
    // And the header must not raise an alarm about a bus nobody has connected.
    const chips=document.getElementById('chips');
    if(chips.querySelector('.dot.bad')) say('the header shows a fault on an unconfigured bridge');
  }catch(e){ say('the fresh-bridge assertions threw: '+e.message); }
  document.title=fail.length?'LAYOUT-FAIL '+fail.join(' || '):'LAYOUT-OK';
},25);
})();
"""


# A working bridge whose driver.id was never filled in. "Empty" means the firmware picks the
# highest-priority driver, so somebody who wires up the inverter that driver serves and never
# opens the wizard has exactly this configuration -- and 1850 W on screen.
AUTOPICK_JS = r"""
(function(){
const fail=[];
let tries=0;
const tick=setInterval(async()=>{
  if(!cfg && ++tries<=80) return;
  clearInterval(tick);
  try{
    if(neverConfigured()) fail.push('a producing bridge was called unconfigured');
    goTab('live');
    await new Promise(r=>setTimeout(r,250));
    const live=document.getElementById('live').textContent;
    if(live.includes('No inverter set up yet')) fail.push('Live hides a producing inverter');
    // sp() separates thousands with a THIN SPACE (U+2009), not an ordinary one. Matching on
    // the plain-space form silently found nothing and reported the production as missing.
    if(!live.includes('1\u2009850')) fail.push('Live does not show the production');
  }catch(e){ fail.push('the auto-pick assertions threw: '+e.message); }
  document.title=fail.length?'LAYOUT-FAIL '+fail.join(' || '):'LAYOUT-OK';
},25);
})();
"""


# The Health tab: counters that tick on every poll, and a log the reader scrolls through. The
# tab was rebuilt to move those counters, which threw the log -- and the reader's place in it --
# back to the top every few seconds.
HEALTH_JS = r"""
(function(){
const fail=[];
const say=m=>fail.push(m);
let tries=0;
const tick=setInterval(async()=>{
  if((!cfg||!diag) && ++tries<=80) return;
  clearInterval(tick);
  try{
    goTab('health');
    await new Promise(r=>setTimeout(r,400));
    const box=document.querySelector('#logbox');
    if(!box) say('no log box on the Health tab');
    else{
      for(let i=0;i<5;i++){
        // Minutes, not seconds: up() renders 100..104 s as the same "1 m", so an assertion on
        // those would have watched a value that cannot move and called it frozen.
        diag.uptime_seconds=60*(i+1); diag.free_heap_bytes=180000+i; diag.poll_success_total=500+i;
        paint();
        await new Promise(r=>setTimeout(r,20));
      }
      if(document.querySelector('#logbox')!==box) say('the log was rebuilt to move a counter');
      // The other half: the counters must still be moving.
      if(!/5 m/.test(document.querySelector('#h_up').textContent)) say('the uptime stopped updating');
      if(!/504/.test(document.querySelector('#h_polls').textContent)) say('the poll counter stopped updating');
    }
  }catch(e){ say('the health assertions threw: '+e.message); }
  document.title=fail.length?'LAYOUT-FAIL '+fail.join(' || '):'LAYOUT-OK';
},25);
})();
"""


# The Bridge tab, which is all configuration and two readouts that move on their own. Nothing
# there needs rebuilding when the clock ticks -- and rebuilding it cost a ticked "include
# passwords" box and a chosen backup file every few seconds, because innerHTML replaces the
# elements underneath it and takes everything not in cfg with them.
BRIDGE_JS = r"""
(function(){
const fail=[];
const say=m=>fail.push(m);
let tries=0;
const tick=setInterval(async()=>{
  if((!cfg||!S) && ++tries<=80) return;
  clearInterval(tick);
  try{
    goTab('bridge');
    await new Promise(r=>setTimeout(r,400));
    const box=document.querySelector('#bk_sec');
    if(!box){ say('no include-passwords checkbox to test'); }
    else{
      box.checked=true;
      box.blur();                       // not focused: the focus guard must not be what saves it
      const clockBefore=document.querySelector('#bridge_clock').textContent;
      for(let i=0;i<6;i++){
        S.bridge.time='2026-07-29 10:0'+i+':0'+i;
        S.bridge.wifi_rssi_dbm=-54-i;
        paint();
        await new Promise(r=>setTimeout(r,20));
      }
      if(document.querySelector('#bk_sec')!==box) say('the tab was rebuilt while only the clock moved');
      if(!box.checked) say('the ticked box was reset by a repaint');
      // ...and the data must still be live. A tab that never updates is the other failure.
      if(document.querySelector('#bridge_clock').textContent===clockBefore){
        say('the clock stopped updating');
      }
      if(!document.querySelector('#bridge_wifi').textContent.includes('-59')){
        say('the signal strength stopped updating');
      }
    }
  }catch(e){ say('the bridge assertions threw: '+e.message); }
  document.title=fail.length?'LAYOUT-FAIL '+fail.join(' || '):'LAYOUT-OK';
},25);
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

    The battery override goes in ahead of the stub so the stub reads it; both go ahead of the
    page's own script, for the reason build_web.inject_before_script explains. The assertions
    go LAST, after the page has defined everything they inspect.
    """
    page = build_web.inject_before_script(
        stripped, f"window.__demoBattery={battery};", stub
    )
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
    stripped = build_web.served_page()
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

        # The tab that takes input rather than only showing it.
        page = build_page(stripped, stub, "{soc:68,power:-1240}", INVERTERS_JS)
        verdict, _ = render(chrome, page, 1000, scratch, "inverters")
        status |= report("inverters tab interaction", verdict)

        # And the state every owner passes through exactly once, on a different stub: a bridge
        # that has just been provisioned and has never been told what it is wired to.
        fresh = (ROOT / "tools" / "fresh_bridge.js").read_text(encoding="utf-8")
        page = build_web.inject_before_script(stripped, fresh)
        page = page.replace("</body>", f"<script>{FRESH_BRIDGE_JS}</script></body>", 1)
        verdict, _ = render(chrome, page, 1000, scratch, "fresh")
        status |= report("first run, nothing configured", verdict)

        # The mirror image, and the reason the config alone is not the question: driver.id is
        # equally empty here, but the inverter it auto-picked is answering.
        auto = (ROOT / "tools" / "autopick_bridge.js").read_text(encoding="utf-8")
        page = build_web.inject_before_script(stripped, auto)
        page = page.replace("</body>", f"<script>{AUTOPICK_JS}</script></body>", 1)
        verdict, _ = render(chrome, page, 1000, scratch, "autopick")
        status |= report("auto-picked driver, answering", verdict)

        # Data keeps moving; the page around it does not get rebuilt.
        page = build_page(stripped, stub, "{soc:68,power:-1240}", BRIDGE_JS)
        verdict, _ = render(chrome, page, 1000, scratch, "bridge")
        status |= report("bridge tab keeps what you typed", verdict)

        page = build_page(stripped, stub, "{soc:68,power:-1240}", HEALTH_JS)
        verdict, _ = render(chrome, page, 1000, scratch, "health")
        status |= report("health keeps your place in the log", verdict)

    return status


if __name__ == "__main__":
    sys.exit(main())
