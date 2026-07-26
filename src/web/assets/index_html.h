// SPDX-License-Identifier: MIT
//
// The local web UI. Vanilla HTML/CSS/JS, no framework, no CDN -- a bridge on an isolated
// network must render fully offline, and a CDN reference would also leak that the device
// exists to a third party on every page load.
//
// Served from PROGMEM rather than LittleFS: it is a few KB, and one fewer moving part during
// OTA (a filesystem image that can be out of step with the firmware is a real failure mode).

#pragma once

#include <pgmspace.h>

namespace heliograph::web {

inline const char kIndexHtml[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Heliograph</title><style>
:root{--bg:#0f1115;--card:#181b22;--fg:#e6e8ec;--dim:#8b93a3;--ok:#3fb950;--bad:#f85149;--warn:#d29922;--line:#262b36}
@media(prefers-color-scheme:light){:root{--bg:#f6f7f9;--card:#fff;--fg:#1a1d23;--dim:#5b6472;--line:#e3e6ea}}
*{box-sizing:border-box}body{margin:0;font:15px/1.5 system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--fg)}
header{padding:16px 20px;border-bottom:1px solid var(--line);display:flex;gap:16px;align-items:baseline;flex-wrap:wrap}
h1{font-size:17px;margin:0;font-weight:600}
nav{display:flex;gap:4px;padding:8px 20px;border-bottom:1px solid var(--line);flex-wrap:wrap}
nav button{background:none;border:0;color:var(--dim);padding:6px 12px;margin:0;border-radius:6px;cursor:pointer;font:inherit}
nav button.on{background:var(--card);color:var(--fg)}
/* Form controls: same rules as the setup page, so both pages read as one product. Without
   these the browser defaults leak through -- white inputs on the dark theme. */
label{display:block;font-size:13px;color:var(--dim);margin:12px 0 4px}
input,select{width:100%;max-width:420px;padding:9px 10px;border-radius:8px;border:1px solid var(--line);background:var(--bg);color:var(--fg);font:inherit}
button{margin-top:14px;padding:10px 16px;border:0;border-radius:8px;background:#2f81f7;color:#fff;font:inherit;font-weight:600;cursor:pointer}
button:disabled{opacity:.5;cursor:default}
.msg{padding:10px;border-radius:8px;margin-top:12px}
.msg.err{background:#f8514922;border:1px solid var(--bad)}
.msg.ok{background:#3fb95022;border:1px solid var(--ok)}
main{padding:20px;max-width:900px}
.grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fill,minmax(180px,1fr))}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px}
.card .k{color:var(--dim);font-size:12px;text-transform:uppercase;letter-spacing:.04em}
.card .v{font-size:22px;font-weight:600;margin-top:4px;font-variant-numeric:tabular-nums}
.card .u{font-size:13px;color:var(--dim);font-weight:400}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
.dot.ok{background:var(--ok)}.dot.bad{background:var(--bad)}.dot.warn{background:var(--warn)}
table{width:100%;border-collapse:collapse;font-size:14px}
td,th{text-align:left;padding:7px 8px;border-bottom:1px solid var(--line)}
th{color:var(--dim);font-weight:500;font-size:12px;text-transform:uppercase}
td.n{text-align:right;font-variant-numeric:tabular-nums}
.dim{color:var(--dim)}.hide{display:none}
.tag{font-size:11px;padding:2px 7px;border-radius:99px;border:1px solid var(--line);color:var(--dim)}
/* Settings layout. The cards carry no margin of their own -- deliberately, because the
   dashboard packs them into a .grid that owns its own gap -- so on the settings tab, where
   they are stacked instead, they used to touch: twelve blocks with 0 px between them read as
   one undifferentiated wall. The spacing lives here, scoped to #cfgform, rather than as a
   margin on .card that would then have to be undone for the grid.
   Two scales on purpose: 12 px between cards that belong to one subject, 26 px between
   subjects, so the grouping is visible without a single border. */
#cfgform{display:flex;flex-direction:column;gap:26px}
.cfgsec{display:flex;flex-direction:column;gap:12px}
.cfgsec>h3{margin:0;font-size:12px;font-weight:600;text-transform:uppercase;letter-spacing:.06em;color:var(--dim)}
/* The first control in a card sits directly under the card's own title, where label's 12 px
   top margin is the right gap; a heading-less card would otherwise start with a double gap. */
.card>b:first-child+label{margin-top:10px}
/* The gap already separates it. Without this the global button margin stacks on top of the
   flex gap and Save floats a long way from the settings it saves. */
.cfgsave>button{margin-top:0}
.card code{font:12px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;color:var(--fg)}
/* Last row of a table inside a card: the card's own padding is the bottom edge, so the row
   rule underneath it draws a line to nowhere. */
.card table tr:last-child td{border-bottom:0}
dialog{background:var(--card);color:var(--fg);border:1px solid var(--line);border-radius:12px;padding:20px;width:90%;max-width:340px}
dialog::backdrop{background:#000a}
.err{background:#f8514922;border:1px solid var(--bad);padding:10px;border-radius:8px;margin-bottom:12px}
#logbox{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px;
  font:12px/1.6 ui-monospace,SFMono-Regular,Menlo,monospace;white-space:pre-wrap;word-break:break-all;
  height:60vh;overflow-y:auto}
#logbox .lw{color:var(--warn,#d29922)}#logbox .le{color:var(--bad)}
</style></head><body>
<header><h1>Heliograph</h1>
<span id="hdr" class="dim">connecting…</span>
<span style="flex:1"></span><span id="ver" class="tag"></span></header>
<nav>
<button data-t="dash" class="on">Dashboard</button>
<button data-t="dev">Device</button>
<button data-t="diag">Diagnostics</button>
<button data-t="logs">Logs</button>
<button data-t="disc">Discovery</button>
<button data-t="cfg">Settings</button>
</nav>
<main>
<div id="banner" class="err hide"></div>
<section id="dash"><div class="grid" id="tiles"></div><div id="fleet"></div></section>
<section id="dev" class="hide"><div id="devbox"></div></section>
<section id="diag" class="hide"><table id="diagtbl"></table></section>
<section id="logs" class="hide">
  <div style="display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-bottom:10px">
    <span id="loginfo" class="dim" style="font-size:13px"></span>
    <span style="flex:1"></span>
    <label style="display:flex;gap:6px;align-items:center;font-size:13px;color:var(--dim)">
      <input id="logpause" type="checkbox" style="width:auto"> Pause</label>
  </div>
  <div id="logbox"></div>
</section>
<section id="disc" class="hide"><div id="wiz"></div></section>
<section id="cfg" class="hide"><div id="cfgform"></div></section>
</main>
<dialog id="authdlg">
<form method="dialog">
<b>Admin sign-in</b>
<!-- Shown only after a 401. Both fields can be the reason now, and they fail identically, so
     a silent re-prompt would read as "wrong password" and send the user round in circles. -->
<div id="autherr" class="msg err" style="display:none">Not accepted. Check the username too — it
is only <b>admin</b> if you never changed it.</div>
<label for="authu">Username</label>
<input id="authu" autocomplete="username" autocapitalize="none" autocorrect="off"
       spellcheck="false" required value="admin">
<label for="authpw">Password</label>
<input id="authpw" type="password" autocomplete="current-password" required autofocus>
<div style="display:flex;gap:10px">
<!-- Cancel is type=button on purpose: implicit form submission (Enter in the password
     field) picks the FIRST submit button in tree order, so a submit-type Cancel here made
     Enter mean "cancel" -- the natural keystroke silently aborted every admin action. -->
<button type="button" onclick="this.closest('dialog').close('cancel')" style="background:none;border:1px solid var(--line);color:var(--fg)">Cancel</button>
<button value="ok">Unlock</button>
</div>
</form>
</dialog>
<script>
// Accepts both '#id' and a full selector. The wizard and settings code was written against
// a getElementById-style helper (as the setup page uses) and pasted into this page, where $
// was querySelector -- so $('wiz') looked for a <wiz> element and returned null. Rather than
// rely on every call site remembering which page it is on, take both.
const $=s=>document.querySelector(s[0]==='#'||s[0]==='.'?s:'#'+s);
let tab='dash';
document.querySelectorAll('nav button').forEach(b=>b.onclick=()=>{
  tab=b.dataset.t;
  document.querySelectorAll('nav button').forEach(x=>x.classList.toggle('on',x===b));
  ['dash','dev','diag','logs','disc','cfg'].forEach(t=>$('#'+t).classList.toggle('hide',t!==tab));
  refresh();
});
// null means unknown, and it must never render as 0 -- that is the whole point of the
// firmware sending null in the first place.
const fmt=(v,d=1)=>v===null||v===undefined?'—':Number(v).toFixed(d);
// "3 m" -> "7.4 h" -> "3 d 5 h": an uptime tile that reads at a glance beats raw hours,
// and 0 h for the first hour looked simply broken.
const up=s=>s<3600?Math.floor(s/60)+' m':s<86400?(s/3600).toFixed(1)+' h':Math.floor(s/86400)+' d '+Math.round(s%86400/3600)+' h';
// Quotes included: esc() output lands inside double-quoted HTML attributes (input values),
// where an unescaped `"` in a stored config string would break out of the attribute and
// inject markup for every later visitor of the settings page.
const esc=s=>String(s??'').replace(/[<>&"']/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;',"'":'&#39;'}[c]));

// ---------------- Admin auth ----------------
// fetch() never triggers the browser's Basic-auth dialog: a 401 just comes back as a 401 and
// the page is expected to deal with it. So ask once, keep it for the tab only (sessionStorage,
// not localStorage -- an admin password should not outlive the session), and send the header
// ourselves on every mutating call.
function authHeader(){
  const c=sessionStorage.getItem('sb_auth');
  return c?{'Authorization':'Basic '+c}:{};
}
// A real modal with a masked input, not window.prompt(): prompt() shows the password in
// plain text on screen.
//
// The username is typed, not fetched. This dialog used to read it from GET /config, which is
// unauthenticated -- so the config endpoint was handing every LAN reader half of a login that
// has no brute-force protection, purely so this field could be filled in for them. The field
// defaults to 'admin', the factory value and what almost every install keeps.
//
// `retry` is true when this prompt follows a 401. That case has to say so: a wrong USERNAME and
// a wrong PASSWORD are now both possible and they fail identically, so an unexplained second
// dialog reads as "bad password" and the user retypes the password forever.
async function askAuth(retry){
  const remembered=sessionStorage.getItem('sb_user');
  return new Promise(resolve=>{
    const d=$('#authdlg'),p=$('#authpw'),un=$('#authu'),err=$('#autherr');
    un.value=remembered||'admin';
    err.style.display=retry?'block':'none';
    p.value='';d.returnValue='';
    d.onclose=()=>{
      // Trimmed, and typed lowercase by the input's autocapitalize=none: HTTP Basic compares
      // bytes, so a phone keyboard capitalising the first letter or autocorrect adding a
      // trailing space is a login that fails with nothing on screen to show why.
      const u=un.value.trim()||'admin';
      const ok=d.returnValue==='ok'&&p.value!=='';
      // UTF-8, not btoa()'s default. btoa() only throws above U+00FF, so it would silently emit
      // Latin-1 for é/ë/ü/ö/ç -- and the firmware compares against the UTF-8 bytes it stored,
      // so such a password could never authenticate here (review, 2026-07-25).
      if(ok){
        sessionStorage.setItem('sb_auth',
          btoa(String.fromCharCode(...new TextEncoder().encode(u+':'+p.value))));
        // Held apart from sb_auth, and NOT written here: rememberUser() is called only once a
        // request with these credentials has actually come back non-401. Storing it on submit
        // meant a typo'd username was cached and then helpfully re-filled on every retry, so
        // the user was handed their own mistake back and could never converge (review).
        sessionStorage.setItem('sb_pending',u);
      }
      p.value='';
      resolve(ok);
    };
    d.showModal();
  });
}
// Promotes the username that was just proven to work. Anything else stays pending, so the next
// prompt offers the last name the device actually accepted rather than the last one typed.
function rememberUser(){
  const u=sessionStorage.getItem('sb_pending');
  if(u)sessionStorage.setItem('sb_user',u);
}
function clearAuth(){sessionStorage.removeItem('sb_auth')}

/// fetch for endpoints that need the admin password. Prompts on 401 and retries once, so a
/// wrong password is correctable without reloading the page.
// Cancelling used to throw, and not one call site caught it: the wizard just sat on
// "Scanning..." forever with no message. A response-shaped object instead, so every caller's
// existing "not ok" path reports it like any other failure.
const authCancelled=()=>({ok:false,status:0,cancelled:true,
  json:async()=>({error:{code:'cancelled',message:'Admin password required.'}}),
  text:async()=>''});
// Failure phrasing for any authFetch result: a dismissed password dialog must never surface
// as the baffling "HTTP 0" (status is 0 on the cancellation object above).
const httpWhy=r=>r.cancelled?'cancelled (admin password required)':'HTTP '+r.status;
async function authFetch(url,opts={}){
  if(!sessionStorage.getItem('sb_auth')&&!await askAuth(false))return authCancelled();
  let r=await fetch(url,{...opts,headers:{...(opts.headers||{}),...authHeader()}});
  if(r.status===401){
    clearAuth();
    // retry=true: the dialog explains that the username is a candidate too.
    if(!await askAuth(true))return authCancelled();
    r=await fetch(url,{...opts,headers:{...(opts.headers||{}),...authHeader()}});
  }
  if(r.status!==401)rememberUser();
  return r;
}

function tile(k,v,u,extra=''){return `<div class="card"><div class="k">${esc(k)}</div>
<div class="v">${esc(v)}<span class="u"> ${esc(u)}</span></div>${extra}</div>`}

/// Bridge-wide tiles: true whatever is on the bus, so they are the same in both layouts.
function bridgeTiles(b){
  return tile('Uptime',up(b.uptime_seconds),'')+
    tile('WiFi',b.wifi_rssi_dbm??'—','dBm')+
    tile('Modbus clients',b.modbus_clients??0,'')+
    // Honest clock: before the first NTP sync there is no time to show. The extra
    // null-guard matters: the API can answer time_synced:true with time:null when
    // formatting failed server-side, and a throw here would kill the whole render loop
    // and put up the "Cannot reach the bridge" banner for a healthy bridge.
    tile('Clock',b.time_synced?(b.time?esc(b.time.split(' ')[1]??b.time):'—'):'not synced','');
    // No firmware tile: the version already sits in the header, permanently.
}

/// The single-inverter dashboard, unchanged. `g` reads the first device's measurements.
function singleTiles(s,d,g){
  return tile('AC Power',fmt(g('ac.power.total'),0),'W')+
    tile('Today',fmt(g('energy.today'),2),'kWh')+
    tile('Total',fmt(g('energy.total'),1),'kWh')+
    tile('Temperature',fmt(g('inverter.temperature')),'°C')+
    tile('AC Voltage',fmt(g('ac.phase_l1.voltage')),'V')+
    tile('Frequency',fmt(g('ac.frequency'),2),'Hz')+
    tile('Status',esc(s.status_text??'—'),'')+
    // null means this protocol has no error code field at all -- not "no fault".
    tile('Error code',s.error_code===null?'not reported':esc(s.error_code),'')+
    tile('Last poll',d.last_successful_poll_seconds_ago??'—','s ago');
}

/// Totals across every polled inverter, each carrying how many it actually covers.
///
/// The count is not decoration. A sum over two of three inverters looks exactly like a sum
/// over three that had a bad day, and the difference is the whole diagnosis. Temperature,
/// voltage, frequency, status and error code have no bridge-wide meaning at all, so they are
/// not averaged into something plausible -- they live per device, on the strip below and on
/// the Device tab.
function fleetTiles(fleet,tot,expected){
  // Against the configured count, so a device that never started is counted as not reporting
  // rather than quietly left out of the question. esc() on both: this is the only place in the
  // new code that interpolates a number straight into innerHTML, and the file's rule is that
  // everything is escaped whatever its type is claimed to be.
  const sub=(n)=>{
    const of=expected??fleet.length;
    return n===of?`<div class="k" style="margin-top:6px;text-transform:none">${esc(of)} inverters</div>`
      :`<div class="k" style="margin-top:6px;text-transform:none;color:var(--bad)">${esc(n)} of ${esc(of)} reporting</div>`;
  };
  const t=(label,value,unit,decimals,count)=>tile(label,fmt(value,decimals),unit,sub(count??0));
  return t('AC Power',tot.ac_power_w,'W',0,tot.ac_power_devices)+
    t('Today',tot.energy_today_kwh,'kWh',2,tot.energy_today_devices)+
    t('Total',tot.energy_total_kwh,'kWh',1,tot.energy_total_devices);
}

/// One line per inverter under the totals, so a dead one cannot hide inside a sum.
///
/// Deliberately not a second copy of the Device tab: id, whether it is answering, what it is
/// producing, and how long ago it last replied. Anything more belongs where there is room.
function fleetStrip(fleet){
  const rows=fleet.map(f=>{
    const answering=f.online&&f.data_valid&&!f.data_stale;
    const ago=f.last_successful_poll_seconds_ago;
    // Never answered is its own state: that is a bus fault, not a sleeping inverter, and the
    // two are indistinguishable in `online:false` alone.
    const when=(ago===null||ago===undefined)?'<span class="tag" style="background:var(--bad)">never answered</span>'
      :`<span class="tag">${f.data_stale?'stale — ':''}replied ${esc(ago)} s ago</span>`;
    const w=(f.ac_power_w===null||f.ac_power_w===undefined)?'—':fmt(f.ac_power_w,0)+' W';
    return `<tr><td><span class="dot ${answering?'ok':'bad'}"></span>${esc(f.id)}</td>
      <td class="n">${esc(w)}</td><td class="n">${when}</td></tr>`;
  }).join('');
  return `<div class="card" style="margin-top:14px"><table>
    <tr><th>Inverter</th><th style="text-align:right">AC power</th>
    <th style="text-align:right">Last reply</th></tr>${rows}</table></div>`;
}

function render(s){
  const d=s.device,b=s.bridge,m=s.measurements||{};
  const dot=x=>x?'<span class="dot ok"></span>':'<span class="dot bad"></span>';
  const fleet=s.devices||[], tot=s.totals||{};
  const polled=tot.devices_polled??fleet.length, answering=tot.devices_answering??0;
  // The CONFIGURED count is the denominator, not the started one. Three identical inverters
  // left on the default address all resolve to the same id, so two of them are skipped at boot
  // and `devices_polled` is 1 -- which, keyed off the started count, put the single-device
  // layout and a green "Inverter" back on screen in the most likely bring-up mistake there is
  // (review, 2026-07-26). A device that did not start is missing, not absent from the question.
  const expected=b.devices_configured??polled;
  // The inverter indicator describes EVERY device, not the first one. On a bus of three it was
  // driven by device 1's `online`, so a dead second inverter left the one indicator anybody
  // glances at green (#38). Green means all of them are answering; anything less is red,
  // deliberately -- an amber "some" is a state you learn to ignore.
  //
  // With ONE device this is byte for byte the header it has always been, `online` and its two
  // tags included. The strict rule would have turned it red during the stale window where it
  // used to stay green, and that is a change to every existing single-inverter bridge that
  // nobody asked for.
  const multi=expected>1||polled>1;
  $('#hdr').innerHTML=dot(b.wifi_connected)+'WiFi '+
    dot(b.mqtt_connected)+'MQTT '+
    dot(b.modbus_listening)+'Modbus '+
    (multi?dot(answering===expected)+`Inverters ${esc(answering)}/${esc(expected)}`
          :dot(d.online)+'Inverter'+
           (d.data_stale?' <span class="tag">stale</span>':'')+
           (d.data_valid?'':' <span class="tag">no data</span>'));
  $('#ver').textContent='v'+b.firmware_version;
  // Boards without relays never send the field; the settings card keys off this.
  window.g_relayCount=(b.relays||[]).length;
  window.g_maxDevices=b.max_devices||window.g_maxDevices;
  const g=id=>m[id]?m[id].value:null;
  if(tab==='dash'){
    // One inverter: exactly the dashboard this has always been. Several: the tiles that can
    // honestly be added become bridge totals, the ones that only mean something per device
    // move to the strip below, and nothing on this page presents one inverter as the bridge.
    $('#tiles').innerHTML=(multi?fleetTiles(fleet,tot,expected):singleTiles(s,d,g))+bridgeTiles(b);
    $('#fleet').innerHTML=multi?fleetStrip(fleet):'';
  }
  if(tab==='dev'){
    // Every configured device, not just the first. The bridge polls up to eight; this tab
    // rendered s.device, which is the first one, so a second inverter that was wired,
    // addressed and configured was invisible on every screen the owner has -- and every way
    // it can fail to start was a single warn line in a ring buffer.
    renderDevices(b);
  }
}

/// One device's table. Extracted unchanged from the single-device version so that the rows,
/// their order and their wording stay exactly what they were.
function deviceTable(d,caps,m){
  let r=`<tr><th>Field</th><th>Value</th></tr>`;
  const add=(k,v)=>r+=`<tr><td class="dim">${esc(k)}</td><td>${esc(v??'—')}</td></tr>`;
  add('Manufacturer',d.manufacturer);add('Model',d.model);add('Serial',d.serial_number);
  add('Driver',d.driver_id);add('Support level',d.support_level);
  add('Online',d.online);add('Data valid',d.data_valid);add('Data stale',d.data_stale);
  if(caps){
    r+=`<tr><th>Capability</th><th></th></tr>`;
    // "Driver read-only", not "Read-only": this is the driver's own write capability, a
    // different thing from the security.read_only_mode switch under Settings. Two rows named
    // "Read-only" meaning different things is how someone looks for the kill switch here.
    r+=`<tr><td class="dim">Driver read-only</td><td>${caps.read_only}</td></tr>`;
    r+=`<tr><td class="dim">Phases / MPPTs</td><td>${caps.phase_count} / ${caps.mppt_count}</td></tr>`;
    r+=`<tr><td class="dim">Battery</td><td>${caps.has_battery}</td></tr>`;
    r+=`<tr><td class="dim">Read</td><td>${(caps.read||[]).map(esc).join(', ')||'—'}</td></tr>`;
    // Empty for every driver in this build, and that is the point: the UI shows what the
    // device can do, it does not assume.
    r+=`<tr><td class="dim">Write</td><td>${(caps.write||[]).map(esc).join(', ')||'<span class="dim">none</span>'}</td></tr>`;
  }
  const entries=Object.entries(m||{});
  // The count is the tell for a wrong register map. A driver that ships several maps will
  // happily decode the wrong one: a couple of measurements instead of a dozen, and the couple
  // it does publish look entirely plausible. See the per-device protocol docs.
  r+=`<tr><th>Measurement</th><th>Value (${entries.length})</th></tr>`;
  for(const [k,v] of entries){
    r+=`<tr><td class="dim">${esc(k)}${v.derived?' <span class="tag">derived</span>':''}</td>
    <td class="n">${v.value===null?'<span class="dim">unknown</span>':esc(v.value)+' '+esc(v.unit)}
    ${v.stale?'<span class="tag">stale</span>':''}</td></tr>`;
  }
  return r;
}

/// Renders one block per polled device, preceded by the reconciliation the firmware alone can
/// do: how many devices the configuration asks for versus how many are actually being polled,
/// and why each missing one is missing. Those strings come from the boot loop, not from
/// re-deriving device ids in the browser -- the page must not have its own opinion about what
/// a device id looks like.
// Only keep an OK response. fetch() does not reject on 4xx/5xx, and the error body is valid
// JSON, so `await r.json()` on a 500 yields a truthy object with none of the expected fields --
// which this page then renders as `undefined` rows, or as a measurement count of 0. That count
// is the wrong-register-map tell, so a single failed request would fabricate the exact symptom
// someone is here to diagnose. The old single-device path carried this guard and a comment
// about the day it bit; the first version of this function dropped both (review, 2026-07-25).
async function getJson(url){
  const r=await fetch(url);
  if(!r.ok)throw new Error('HTTP '+r.status);
  return r.json();
}

// Identity and capabilities do not change while the firmware runs -- they are fixed by the
// driver at boot. Cached per device so a tab left open re-fetches only what actually moves.
const devCache={};

/// The one line that must be visible from any tab: some configured device is not running.
/// Detail stays on the Device tab; this only says look there.
function deviceBanner(b){
  const el=$('#banner');
  if(!el||!b)return;
  const configured=b.devices_configured;
  const problems=(b.device_problems||[]).length;
  // Undefined means this firmware does not report it -- which is not the same as "nothing is
  // wrong", so say nothing rather than reassure.
  if(configured===undefined){return}
  const started=b.devices_started;
  if(started!==undefined&&(problems||started<configured)){
    el.textContent=`${started} of ${configured} configured devices started — see the Device tab.`;
    el.classList.remove('hide');
  }
}

let devsBusy=false, devsNextMs=0;
async function renderDevices(b){
  // Rate-limited independently of what triggered it. refresh() runs on the 5 s timer AND on
  // every SSE state event, which the server emits up to once a second -- so without this the
  // tab issued 1+3N requests per second, back to back, on the one board that is also driving
  // an RS485 bus. The data underneath changes at the poll interval, ten seconds by default.
  const now=Date.now();
  if(devsBusy||now<devsNextMs)return;
  devsBusy=true;devsNextMs=now+5000;
  const box=$('#devbox');
  try{
    const ids=((await getJson('/api/v1/devices')).devices)||[];
    const configured=b.devices_configured??ids.length;
    const problems=b.device_problems||[];
    let h='';
    // "Started", not "polling": this compares what the configuration asks for against what the
    // firmware managed to CREATE at boot. A device with A and B swapped starts perfectly and
    // never returns a byte, so a count alone must not be read as health -- the per-device rows
    // below carry that, and the summary says which ones are actually answering.
    if(problems.length||configured!==ids.length){
      h+=`<div class="msg err" style="display:block">Started ${ids.length} of ${configured}
        configured device${configured===1?'':'s'}.${problems.length?'<ul style="margin:6px 0 0 18px">'+
        problems.map(p=>`<li>${esc(p)}</li>`).join('')+'</ul>':
        '<div style="margin-top:6px">A device added since the last restart only starts after one.</div>'}</div>`;
    }
    const cards=[];
    let answering=0;
    for(const id of ids){
      // Sequential rather than parallel, and cached: eight devices is eight handlers on the
      // web task, and identity and capabilities never change once the driver has started.
      const dev=await getJson('/api/v1/devices/'+encodeURIComponent(id));
      const m=(await getJson('/api/v1/devices/'+encodeURIComponent(id)+'/measurements')).measurements||{};
      if(devCache[id]===undefined){
        try{devCache[id]=await getJson('/api/v1/devices/'+encodeURIComponent(id)+'/capabilities')}
        catch(e){devCache[id]=null}
      }
      const ident={...(dev.identity||{}),online:dev.online,data_valid:dev.data_valid,
                   data_stale:dev.data_stale,support_level:(dev.driver||{}).support_level};
      const ago=dev.last_successful_poll_seconds_ago;
      // The same three conditions the status endpoint counts (totals.devices_answering) and the
      // header indicator reads. Without the staleness term this tab said "3 of 3 answering"
      // while the header said 2 of 3 -- for the seventy seconds between a device going stale
      // and going offline, which is exactly when someone clicks through to this tab to find out
      // what is wrong (review, 2026-07-26).
      if(dev.online&&dev.data_valid&&!dev.data_stale)answering++;
      // The line that answers "is this one alive", above the table rather than thirteen rows
      // into it. Never answered at all is its own state: that is a bus fault, not a sleeping
      // inverter, and the two look identical in `online: false` alone.
      const live=ago===null||ago===undefined
        ? '<span class="tag" style="background:var(--bad)">never answered</span>'
        : (dev.data_stale?`<span class="tag">stale — last reply ${esc(ago)} s ago</span>`
                         :`<span class="tag">replied ${esc(ago)} s ago</span>`);
      cards.push(`<div class="card"><div style="display:flex;justify-content:space-between;
        align-items:baseline;gap:10px"><b>${esc(id)}</b>${live}</div>
        <table>${deviceTable(ident,devCache[id],m)}</table></div>`);
    }
    if(ids.length&&!problems.length&&configured===ids.length){
      h+=`<div class="${answering===ids.length?'dim':'msg err'}"
        style="font-size:13px;margin-bottom:10px${answering===ids.length?'':';display:block'}">
        ${answering} of ${ids.length} started device${ids.length===1?'':'s'} answering.</div>`;
    }
    box.innerHTML=h+cards.join('')||'<div class="dim">No device is being polled.</div>';
  }catch(e){
    // Leave whatever was on screen rather than blanking it: a single failed request must not
    // wipe a page someone is reading.
    if(!box.innerHTML)box.innerHTML='<div class="dim">Could not read the device list.</div>';
  }finally{devsBusy=false}
}

// Settings-page network picker: same scan as the setup wizard, admin-gated (the endpoint
// requires auth outside the portal). Results land in a real <select> that copies the
// choice into the free-text field -- NOT a <datalist>: a datalist filters suggestions on
// the field's current value, and this field is prefilled with the active SSID, so the
// list appeared empty for every other network (live, 2026-07-22). The text field stays
// authoritative, so hidden SSIDs remain typable.
async function scanNetworksList(){
  const btn=$('#c_scanbtn'),msg=$('#c_scanmsg'),pick=$('#c_ssidpick');
  btn.disabled=true;msg.textContent='scanning… (takes a few seconds)';
  try{
    const r=await authFetch('/api/v1/wifi/scan');
    const d=await r.json();
    if(!r.ok)throw new Error(httpWhy(r));
    const nets=d.networks||[];
    pick.innerHTML='<option value="">— pick a network —</option>'+nets.map(n=>
      `<option value="${esc(n.ssid)}">${esc(n.ssid)}  (${n.rssi} dBm)${n.open?' — open':''}</option>`
    ).join('');
    pick.style.display=nets.length?'block':'none';
    msg.textContent=nets.length+' networks found.';
  }catch(e){msg.textContent='Scan failed: '+e.message}
  btn.disabled=false;
}

async function loadDiag(){
  const r=await fetch('/api/v1/diagnostics');const d=await r.json();
  let h=`<tr><th>Metric</th><th>Value</th></tr>`;
  for(const [k,v] of Object.entries(d))
    h+=`<tr><td class="dim">${esc(k)}</td><td class="n">${esc(v??'—')}</td></tr>`;
  $('#diagtbl').innerHTML=h;
}

// ---------------- Logs ----------------
// Admin-gated on the API side: raw lines carry protocol traffic and internal state. The
// auth-cancel guard matters because this loads on a 5s refresh -- without it, dismissing
// the password dialog would re-summon it every 5 seconds forever.
let logsAuthStop=false;
async function loadLogs(){
  if(logsAuthStop||$('#logpause').checked)return;
  const r=await authFetch('/api/v1/logs?limit=64');
  // A 401 latches too, not just a cancel. authFetch has already prompted twice and failed, and
  // this runs on a 5 s timer: without the latch the dialog reappears every five seconds and
  // cannot be escaped except by leaving the tab. That was survivable while a wrong password was
  // the only way to get here; a mistyped username is now a second way, and a far quieter one.
  if(r.cancelled||r.status===401){
    logsAuthStop=true;
    $('#logbox').innerHTML='<span class="dim">'+
      (r.cancelled?'Admin sign-in required.':'Admin sign-in was not accepted — check the '+
       'username as well as the password.')+
      ' <a href="#" onclick="logsAuthStop=false;loadLogs();return false">Try again</a></span>';
    return;
  }
  if(!r.ok){$('#loginfo').textContent=httpWhy(r);return}
  const d=await r.json();
  $('#loginfo').textContent=`level ${d.level} (change it under Settings) — ${d.total} lines since boot, showing last ${d.returned}`;
  const box=$('#logbox');
  // Only pin to the newest line if the reader was already there; never yank the scrollbar
  // out of someone's hands mid-read.
  const atBottom=box.scrollHeight-box.scrollTop-box.clientHeight<40;
  box.innerHTML=(d.lines||[]).map(l=>{
    const cls=l.includes('[E]')?'le':l.includes('[W]')?'lw':'';
    return cls?`<span class="${cls}">${esc(l)}</span>`:esc(l);
  }).join('\n')||'<span class="dim">No lines yet.</span>';
  if(atBottom)box.scrollTop=box.scrollHeight;
}

// ---------------- Discovery wizard (§28: 7 steps) ----------------
let wizStep=1, wizPoll=null, wizChosen=null, wizReport=null, wizSavedSerial=null,
    wizOptions={};
// What the bridge already has configured, so step 5 can offer it back instead of overwriting it.
let wizStoredDriverId=null, wizStoredOptions={};
/// The options the chosen candidate actually answered at, carried from step 4 into step 5.
let wizFound={};
const STEPS=['Interface','Mode','Probing','Candidates','Confirm','Test poll','Save'];

function stepBar(){
  return '<div style="display:flex;gap:6px;flex-wrap:wrap;margin-bottom:14px">'+
    STEPS.map((n,i)=>`<span class="tag" style="${i+1===wizStep?'border-color:#2f81f7;color:var(--fg)':''}">${i+1}. ${n}</span>`).join('')+
    '</div>';
}

function renderWizard(){
  let h=stepBar();
  if(wizStep===1){
    h+=`<div class="card"><b>Step 1 — Physical interface</b>
    <p class="dim">Discovery probes the onboard RS485 bus. Check the wiring and the 120 Ω
    termination jumper before starting: an unterminated bus at the end of a long cable is the
    most common reason nothing answers.</p>
    <button onclick="wizStep=2;renderWizard()">Continue</button></div>`;
  } else if(wizStep===2){
    h+=`<div class="card"><b>Step 2 — Automatic or manual</b>
    <p class="dim">Quick tries each auto-detectable driver once, on its own recommended serial
    profile and its own default address — a few seconds. Extended also tries every profile and
    <b>sweeps bus addresses 1–8</b>, which is what finds a chain of inverters rather than just
    the one at the default address. Budget up to a minute, and note that polling stops for the
    whole run.</p>
    <p class="dim">Probing never writes a register and never starts or stops the inverter.
    Modbus drivers only ever read. One exception, and it is not new: on protocols where the
    bridge <i>registers</i> devices, being discovered means being handed a bus address — there
    is no read-only way to find such a device at all.</p>
    <button onclick="startDiscovery(false)">Run quick discovery</button>
    <button onclick="startDiscovery(true)" style="background:none;border:1px solid var(--line);color:var(--fg)">Run extended discovery</button>
    <button onclick="wizStep=5;renderWizard()" style="background:none;border:1px solid var(--line);color:var(--fg)">Skip — select a driver manually</button></div>`;
  } else if(wizStep===3){
    h+=`<div class="card"><b>Step 3 — Probing</b>
    <p class="dim">Talking to the bus… this pauses normal polling.</p>
    <p class="dim">${wizReport?esc(wizReport.status):'starting'} · ${wizReport&&wizReport.elapsed_ms?Math.round(wizReport.elapsed_ms/1000)+'s':''}</p></div>`;
  } else if(wizStep===4){
    const c=(wizReport&&wizReport.candidates)||[];
    h+=`<div class="card"><b>Step 4 — Candidates</b>
    <p class="dim">${esc(wizReport?wizReport.reason:'')}</p>`;
    const swept=(wizReport&&wizReport.swept_addresses)||[];
    const sweptText=swept.length?`Addresses ${esc(swept[0])}–${esc(swept[swept.length-1])} and each driver’s own default were tried.`:'';
    if(!c.length){h+=`<p class="dim">Nothing was identified. ${sweptText||'The quick scan only tries each driver’s default line speed and default address — run the <b>extended scan</b> to try all of them, and addresses 1–8.'} Check the wiring too: A/B swapped is the most common cause, then termination.</p>`}
    // Traffic without an identification is the one fault a sweep can name that nothing else
    // can: two inverters left on the same unit id answer together and their replies collide.
    // Shown before the candidates, because it explains a device that is missing from them.
    const unk=(wizReport&&wizReport.unidentified_addresses)||[];
    if(unk.length){
      h+=`<div class="msg err" style="display:block"><b>Traffic at ${unk.length===1?'an address':'addresses'} with no device identified.</b>
      <ul style="margin:6px 0 0 18px">${unk.map(u=>`<li>Address <b>${esc(u.address)}</b> (${esc(u.driver_id)}): ${esc(u.note)}</li>`).join('')}</ul>
      <div style="margin-top:6px">On a chain of identical inverters this usually means two of them
      are still on the same address — their replies collide. Put one unit on the bus at a time to
      confirm, then reassign.</div></div>`;
    }
    // Devices, not cards: two drivers can both claim one physical inverter, and telling the
    // owner to add "3 devices" when two of the cards are one unit produces exactly the
    // duplicate-id collision the boot loop refuses.
    const deviceCount=new Set(c.map(x=>x.driver_id+'@'+(x.address??''))).size;
    if(deviceCount>1){
      h+=`<p class="dim">${deviceCount} devices answered. ${sweptText} The wizard configures one — add the rest afterwards under <b>Settings → Extra devices</b>, using the addresses below.</p>`;
    }
    if(wizReport&&wizReport.candidates_omitted>0){
      h+=`<p class="dim">${esc(wizReport.candidates_omitted)} further lower-scoring candidate(s) are not shown.</p>`;
    }
    c.forEach(x=>{
      h+=`<div style="border:1px solid var(--line);border-radius:8px;padding:12px;margin-top:10px">
      <div style="display:flex;justify-content:space-between;align-items:baseline">
        <b>${esc(x.display_name)}</b><span class="tag">${x.confidence}/100</span></div>
      <table style="margin-top:8px">
      <tr><td class="dim">Driver</td><td>${esc(x.driver_id)} <span class="tag">${esc(x.support_level)}</span></td></tr>
      <!-- Absent means two different things and they must not read the same: in a quick scan
           no address was selected at all, so this device is wherever its driver defaults to;
           in a swept scan it means the protocol hands out addresses itself. -->
      <tr><td class="dim">Bus address</td><td>${x.address!=null?'<b>'+esc(x.address)+'</b>'
        :(swept.length?'<span class="dim">assigned by the protocol</span>'
                      :'<span class="dim">not probed — the quick scan does not sweep addresses</span>')}</td></tr>
      <tr><td class="dim">Serial profile tried</td><td>${x.serial_profile?`${x.serial_profile.baud_rate} ${x.serial_profile.data_bits}${esc(x.serial_profile.parity[0].toUpperCase())}${x.serial_profile.stop_bits}, timeout ${x.serial_profile.response_timeout_ms} ms`:'—'}</td></tr>
      <tr><td class="dim">Response found</td><td>${x.responded?'yes':'no'}</td></tr>
      <tr><td class="dim">Checksum valid</td><td>${x.checksum_valid?'yes':'no'}</td></tr>
      <tr><td class="dim">Repeat probe agreed</td><td>${x.consistent?'yes':'<b>no — score halved</b>'}</td></tr>
      <tr><td class="dim">Detected</td><td>${esc(x.detected_manufacturer||'—')} ${esc(x.detected_model||'')}</td></tr>
      <tr><td class="dim">Serial number</td><td>${esc(x.serial_number||'—')}</td></tr>
      <tr><td class="dim">Evidence</td><td>${(x.evidence||[]).map(e=>'· '+esc(e)).join('<br>')||'—'}</td></tr>
      </table>
      <!-- The options travel as JSON through esc(), not as a hand-built string: they come from
           the device, and every other value on this page goes through the same escape. -->
      <button onclick="wizChosen=${esc(JSON.stringify(x.driver_id))};wizFound=${esc(JSON.stringify(x.options||{}))};wizStep=5;renderWizard()">Choose this device</button>
      </div>`;
    });
    h+=`<button onclick="wizStep=2;renderWizard()" style="background:none;border:1px solid var(--line);color:var(--fg)">Back</button></div>`;
  } else if(wizStep===5){
    h+=`<div class="card"><b>Step 5 — Confirm</b>
    <p class="dim">Nothing is saved until step 7. An uncertain match is never selected for you
    — that is deliberate: reading the wrong register map produces believable numbers. Which is
    why the map is a field here and not an assumption: probing identifies the <i>protocol</i>,
    never the model, so a driver that ships several register maps cannot pick one for you.</p>
    <label for="wd">Driver</label><select id="wd" onchange="wizRenderOpts()"></select>
    <div id="wizopts"></div>
    <div id="wizoptnote" class="msg err" style="display:none">Pick a register map first — it
    cannot be detected, and the wrong one produces believable numbers.</div>
    <!-- Starts disabled: wizGateConfirm() cannot run until two fetches resolve, and on a slow
         bridge that is long enough to click through with no map chosen. -->
    <button id="wizconfirm" disabled onclick="wizCapture();wizStep=6;renderWizard();testPoll()">Confirm and test</button>
    <button onclick="wizStep=4;renderWizard()" style="background:none;border:1px solid var(--line);color:var(--fg)">Back</button></div>`;
  } else if(wizStep===6){
    h+=`<div class="card"><b>Step 6 — Test poll</b>
    <p class="dim">This polls the configuration the bridge is <b>running now</b> — the driver is
    built once at boot, so nothing chosen in step 5 is in force yet. Useful for "is anything
    alive on this bus", not for confirming the register map. Check that after the restart, by
    the number of published measurements.</p>
    <div id="tp" class="dim">Polling…</div></div>`;
  } else if(wizStep===7){
    h+=`<div class="card"><b>Step 7 — Saved</b>
    <p class="dim">The driver is stored. It takes effect after a restart.</p>`+
    // Said out loud, because it is a setting the user never asked for and would otherwise
    // find in Settings with no idea where it came from.
    (wizSavedSerial&&wizSavedSerial.override?`<p class="dim">This device answered at
      <b>${wizSavedSerial.baud_rate} ${wizSavedSerial.data_bits}${esc(wizSavedSerial.parity[0].toUpperCase())}${wizSavedSerial.stop_bits}</b>,
      which is not this driver's default, so those line settings were saved with it. You can
      change or clear that under <b>Settings → RS485 line</b>.</p>`:'')+
    // Worth saying in the other direction too, and phrased so it is true whether or not an
    // override existed before: this run cleared one if there was one. cfgBefore is only
    // populated once the Settings tab has been opened, so it cannot be relied on here.
    (wizSavedSerial&&wizSavedSerial.override===false?`<p class="dim">This device answered at
      this driver's own default, so no <b>RS485 line</b> override is stored — the driver
      decides. Any override left over from an earlier run has been switched off.</p>`:'')+
    `
    <button onclick="wizReboot()">Restart now</button>
    <div id="wizmsg" class="msg" style="display:none"></div></div>`;
  }
  $('#wiz').innerHTML=h;

  if(wizStep===5){
    // Both, before rendering: the option fields must offer what is stored, not blow it away.
    Promise.all([fetch('/api/v1/drivers').then(r=>r.json()),
                 fetch('/api/v1/config').then(r=>r.json()).catch(()=>null)])
    .then(([d,cfg])=>{
      cfgDrivers=d;
      if(cfg&&cfg.driver){wizStoredDriverId=cfg.driver.id;wizStoredOptions=cfg.driver.options||{}}
      const sel=$('#wd');
      if(!sel)return;  // the user left step 5 while this was in flight
      sel.innerHTML='';
      (d.drivers||[]).forEach(x=>{
        const o=document.createElement('option');
        o.value=x.id;o.textContent=`${x.display_name} (${x.support_level})`;
        if(x.id===wizChosen)o.selected=true;
        sel.appendChild(o);
      });
      wizRenderOpts();
    });
  }
}

async function startDiscovery(extended){
  // wizFound is cleared here and not only reassigned on the next pick: it outranks the stored
  // configuration in step 5, so an address left over from a previous run -- or from a run the
  // user then skipped past -- would silently propose a device they did not choose this time.
  wizStep=3;wizReport=null;wizChosen=null;wizFound={};renderWizard();
  const r=await authFetch('/api/v1/actions/discover'+(extended?'?extended=true':''),{method:'POST'});
  if(r.cancelled){wizStep=2;renderWizard();return}
  if(r.status===401){wizStep=2;renderWizard();alert('Admin password required.');return}
  if(!r.ok&&r.status!==202){wizStep=2;renderWizard();alert('Could not start discovery.');return}
  clearInterval(wizPoll);
  wizPoll=setInterval(async()=>{
    wizReport=await (await fetch('/api/v1/discovery')).json();
    if(wizReport.busy){renderWizard();return}
    clearInterval(wizPoll);
    // The options come with the selection. Without them the auto-select path landed on step 5
    // with the driver filled in and the address left at its default -- the one thing the sweep
    // exists to get right.
    if(wizReport.auto_selected){
      wizChosen=wizReport.selected_driver_id;
      wizFound=wizReport.selected_options||{};
    }
    wizStep=4;renderWizard();
  },1000);
}

async function testPoll(){
  const r=await authFetch('/api/v1/actions/poll',{method:'POST'});
  const el=$('#tp');
  if(r.status===401){el.textContent='Admin password required.';return}
  if(!r.ok&&r.status!==202){el.textContent='Poll refused: '+httpWhy(r);return}
  // The poll runs on the RS485 task, and right after discovery it re-registers first --
  // three bus transactions before the measurement. 1500 ms showed the pre-poll state.
  setTimeout(async()=>{
    const s=await (await fetch('/api/v1/status')).json();
    const p=s.measurements&&s.measurements['ac.power.total'];
    el.innerHTML=`Inverter online: <b>${s.device.online}</b><br>
      Data valid: <b>${s.device.data_valid}</b><br>
      AC power: <b>${p&&p.value!==null?p.value+' W':'unknown'}</b><br>
      Serial: <b>${esc(s.device.serial_number||'—')}</b>
      <button onclick="saveDriver()">Save this driver</button>`;
  },3000);
}

// What to store for the line, given the candidate that answered.
//
// Extended discovery tries every profile a driver advertises (quick mode only tries the
// first), so a device can reply at one the driver does not lead with. Saving the driver alone
// then means the next boot configures the driver's own first profile and the bus goes quiet.
//
// Returns an override when the match differs from what this driver would configure by itself,
// and {override:false} when it does not -- NOT null. Omitting the key entirely made the wizard
// a one-way ratchet: a PATCH without `serial` leaves the stored value alone, so once anything
// had pinned 115200, re-running the wizard on a device that answers at the driver's default
// could never undo it and step 7 would not even mention the setting that was about to silence
// the bus. Null is reserved for "this run learned nothing", where leaving it alone is right.
async function discoveredSerialOverride(id){
  const cand=((wizReport&&wizReport.candidates)||[]).find(c=>c.driver_id===id);
  const found=cand&&cand.serial_profile;
  if(!found)return null;
  // cfgDrivers is only populated once the Settings tab has been opened, and the wizard is
  // reachable without ever going there. Fetch rather than assume: with no default to compare
  // against, every discovery would pin an override, including the overwhelming majority where
  // the driver's own first choice is already right.
  if(!cfgDrivers){
    try{cfgDrivers=await (await fetch('/api/v1/drivers')).json()}catch(e){return null}
  }
  const drv=((cfgDrivers&&cfgDrivers.drivers)||[]).find(d=>d.id===id);
  const def=drv&&(drv.serial_profiles||[])[0];
  if(!def)return null;  // nothing to compare against; do not guess
  if(def.baud_rate===found.baud_rate&&def.parity===found.parity&&
     def.data_bits===found.data_bits&&def.stop_bits===found.stop_bits){
    return {override:false};
  }
  return {override:true,baud_rate:found.baud_rate,parity:found.parity,
          data_bits:found.data_bits,stop_bits:found.stop_bits};
}

// The selected driver's declared options, at their declared defaults.
//
// Discovery identifies a PROTOCOL, never a model: a probe that gets a valid Modbus reply has
// proved the protocol and nothing about which register map that unit speaks. A driver shipping
// several maps therefore cannot have one chosen for it, and until this existed the wizard
// silently saved the driver alone -- so the firmware fell back to whichever map is marked
// default, and every reading came from the wrong table. Step 5's own text warns that reading
// the wrong map "produces believable numbers"; the wizard was the thing producing them.
//
// Rendered from the driver's own declaration, so a new driver's options appear here with no
// frontend change, exactly as they already do in Settings.
// Captured on the way out of step 5, for the same reason wizChosen is: renderWizard() replaces
// the whole #wiz subtree on every step change, so by the time saveDriver() runs at step 7 both
// the select and these fields are gone. Reading them there would silently save nothing.
function wizCapture(){
  wizChosen=($('#wd')||{}).value||null;
  wizOptions={};
  document.querySelectorAll('#wizopts [data-opt]').forEach(e=>{ wizOptions[e.dataset.opt]=e.value });
}

function wizRenderOpts(){
  const box=$('#wizopts');
  if(!box)return;
  const id=($('#wd')||{}).value;
  const drv=((cfgDrivers&&cfgDrivers.drivers)||[]).find(x=>x.id===id);
  const opts=(drv&&drv.options)||[];
  if(!opts.length){box.innerHTML='';wizGateConfirm();return}
  box.innerHTML=opts.map(o=>{
    // Seeded from what is STORED when this is the driver already configured, exactly as the
    // settings page does. Rendering declared defaults unconditionally meant re-running the
    // wizard -- which the bring-up docs tell you to do when the line speed is wrong -- silently
    // rewrote a working {profile:"mic_tl_x", unit_id:"3"} back to the defaults and reported
    // success. Every rendered key is asserted in the PATCH, so nothing survives by omission.
    const stored=(id===wizStoredDriverId)?(wizStoredOptions||{})[o.key]:undefined;
    // What the device ANSWERED at wins over both. Only ever the bus address -- discovery
    // reports the options it probed with, and it only ever varies that one -- and it is the
    // whole point of sweeping: finding an inverter at address 3 and then offering to configure
    // address 1 would be a worse answer than not looking (#37).
    const found=(id===wizChosen)?(wizFound||{})[o.key]:undefined;
    const cur=found??stored??o.default_value??'';
    const hint=o.description?`<div class="dim" style="font-size:12px">${esc(o.description)}</div>`:'';
    if(o.allowed_values&&o.allowed_values.length){
      // An empty entry among the allowed values is the driver saying "unset means my own
      // default". In Settings that is fine -- you are editing a device you already set up.
      // Here it is not: this is the screen where the register map gets decided, and an unlabeled
      // blank line that silently resolves to whichever map is marked default is precisely the
      // failure this change exists to remove. Labelled, and left unselected so the step cannot
      // be completed until someone has actually chosen.
      const hasBlank=o.allowed_values.includes('');
      const needs=hasBlank&&cur==='';
      return `<label for="wopt_${esc(o.key)}">${esc(o.display_name)}</label>
        <select id="wopt_${esc(o.key)}" data-opt="${esc(o.key)}" ${hasBlank?'data-mustpick="1"':''}
          onchange="wizGateConfirm()">${
          o.allowed_values.map(v=>`<option value="${esc(v)}" ${v===cur&&!needs?'selected':''}>${
            v===''?'— choose —':esc(v)}</option>`).join('')
        }</select>${hint}`;
    }
    const num=o.min_value!==undefined
      ? ` type="number" min="${esc(o.min_value)}" max="${esc(o.max_value)}"` : '';
    return `<label for="wopt_${esc(o.key)}">${esc(o.display_name)}</label>
      <input id="wopt_${esc(o.key)}"${num} data-opt="${esc(o.key)}" value="${esc(cur)}">${hint}`;
  }).join('');
  wizGateConfirm();
}

// Blocks "Confirm and test" while any option the driver marked as ambiguous-when-empty is still
// empty. Cheaper than letting someone click through and discover months later that the numbers
// came from the wrong table.
function wizGateConfirm(){
  const btn=$('#wizconfirm');
  if(!btn)return;
  // Runs on every render, including for drivers with no options at all -- that is what releases
  // the button from the disabled state it is rendered in.
  const missing=[...document.querySelectorAll('#wizopts [data-mustpick]')].some(e=>e.value==='');
  btn.disabled=missing;
  const note=$('#wizoptnote');
  if(note)note.style.display=missing?'block':'none';
}

async function saveDriver(){
  // wizChosen is captured when leaving step 5 -- the #wd select no longer exists here (step 6
  // re-rendered the wizard). Before that capture existed, the manual path saved
  // {driver:{id:null}}, which the backend treats as "keep": step 7 then claimed "Saved" while
  // nothing had been saved at all.
  const id=wizChosen;
  if(!id){alert('No driver selected.');wizStep=5;renderWizard();return}
  const body={driver:{id}};
  // Only when the driver actually declares options -- an empty object would still be a change
  // the backend has to merge, and for a driver with no options it says nothing.
  if(Object.keys(wizOptions).length)body.driver.options=wizOptions;
  const serial=await discoveredSerialOverride(id);
  if(serial)body.serial=serial;
  const r=await authFetch('/api/v1/config',{method:'PATCH',
    headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  if(!r.ok){
    // The body, not just the status. httpWhy() gives "HTTP 400", and the firmware's message is
    // the whole point of a 400 here: it names the field and the range. The wizard is the path
    // the bring-up docs send people down, and it was the one place that threw that away.
    const d=await r.json().catch(()=>({}));
    alert('Save failed: '+((d.error&&d.error.message)||httpWhy(r)));
    return;
  }
  wizSavedSerial=serial;
  wizStep=7;renderWizard();
}

// Same behaviour as rebootFromSettings: the click MUST answer with something. A bare fire-and-
// forget looked like a dead button -- the device rebooted fine, but nobody could tell.
async function wizReboot(){
  const m=$('#wizmsg');
  // authFetch no longer throws on cancel -- it returns the cancellation object, so the old
  // try/catch here had become dead code and the fall-through said "HTTP 0".
  const r=await authFetch('/api/v1/actions/reboot',{method:'POST'});
  if(!r.ok&&r.status!==202){
    m.className='msg err';m.textContent='Restart refused: '+httpWhy(r);m.style.display='block';return;
  }
  m.className='msg ok';
  m.textContent='Restarting. This page will go blank for a few seconds — reload it after.';
  m.style.display='block';
}

// Timezones for the settings dropdown: [city, POSIX TZ, IANA name] grouped by region.
// GENERATED from the IANA tzdata on macOS (each TZif file's POSIX footer) -- do not hand-edit;
// The firmware only ever receives the POSIX
// string; the IANA name is stored alongside purely so this dropdown can re-select the exact
// city the user picked (many cities share one POSIX string).
const TZ={"Europe":[["Amsterdam","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Amsterdam"],["Andorra","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Andorra"],["Astrakhan","<+04>-4","Europe/Astrakhan"],["Athens","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Athens"],["Belfast","GMT0BST,M3.5.0/1,M10.5.0","Europe/Belfast"],["Belgrade","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Belgrade"],["Berlin","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Berlin"],["Bratislava","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Bratislava"],["Brussels","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Brussels"],["Bucharest","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Bucharest"],["Budapest","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Budapest"],["Busingen","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Busingen"],["Chisinau","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Chisinau"],["Copenhagen","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Copenhagen"],["Dublin","GMT0IST,M3.5.0/1,M10.5.0","Europe/Dublin"],["Gibraltar","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Gibraltar"],["Guernsey","GMT0BST,M3.5.0/1,M10.5.0","Europe/Guernsey"],["Helsinki","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Helsinki"],["Isle of Man","GMT0BST,M3.5.0/1,M10.5.0","Europe/Isle_of_Man"],["Istanbul","<+03>-3","Europe/Istanbul"],["Jersey","GMT0BST,M3.5.0/1,M10.5.0","Europe/Jersey"],["Kaliningrad","EET-2","Europe/Kaliningrad"],["Kiev","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Kiev"],["Kirov","MSK-3","Europe/Kirov"],["Kyiv","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Kyiv"],["Lisbon","WET0WEST,M3.5.0/1,M10.5.0","Europe/Lisbon"],["Ljubljana","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Ljubljana"],["London","GMT0BST,M3.5.0/1,M10.5.0","Europe/London"],["Luxembourg","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Luxembourg"],["Madrid","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Madrid"],["Malta","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Malta"],["Mariehamn","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Mariehamn"],["Minsk","<+03>-3","Europe/Minsk"],["Monaco","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Monaco"],["Moscow","MSK-3","Europe/Moscow"],["Nicosia","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Nicosia"],["Oslo","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Oslo"],["Paris","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Paris"],["Podgorica","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Podgorica"],["Prague","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Prague"],["Riga","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Riga"],["Rome","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Rome"],["Samara","<+04>-4","Europe/Samara"],["San Marino","CET-1CEST,M3.5.0,M10.5.0/3","Europe/San_Marino"],["Sarajevo","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Sarajevo"],["Saratov","<+04>-4","Europe/Saratov"],["Simferopol","MSK-3","Europe/Simferopol"],["Skopje","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Skopje"],["Sofia","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Sofia"],["Stockholm","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Stockholm"],["Tallinn","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Tallinn"],["Tirane","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Tirane"],["Tiraspol","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Tiraspol"],["Ulyanovsk","<+04>-4","Europe/Ulyanovsk"],["Uzhgorod","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Uzhgorod"],["Vaduz","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Vaduz"],["Vatican","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Vatican"],["Vienna","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Vienna"],["Vilnius","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Vilnius"],["Volgograd","MSK-3","Europe/Volgograd"],["Warsaw","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Warsaw"],["Zagreb","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Zagreb"],["Zaporozhye","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Zaporozhye"],["Zurich","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Zurich"]],"America":[["Anchorage","AKST9AKDT,M3.2.0,M11.1.0","America/Anchorage"],["Argentina/Buenos Aires","<-03>3","America/Argentina/Buenos_Aires"],["Bogota","<-05>5","America/Bogota"],["Caracas","<-04>4","America/Caracas"],["Chicago","CST6CDT,M3.2.0,M11.1.0","America/Chicago"],["Denver","MST7MDT,M3.2.0,M11.1.0","America/Denver"],["Halifax","AST4ADT,M3.2.0,M11.1.0","America/Halifax"],["Havana","CST5CDT,M3.2.0/0,M11.1.0/1","America/Havana"],["Lima","<-05>5","America/Lima"],["Los Angeles","PST8PDT,M3.2.0,M11.1.0","America/Los_Angeles"],["Mexico City","CST6","America/Mexico_City"],["New York","EST5EDT,M3.2.0,M11.1.0","America/New_York"],["Phoenix","MST7","America/Phoenix"],["Santiago","<-04>4<-03>,M9.1.6/24,M4.1.6/24","America/Santiago"],["Sao Paulo","<-03>3","America/Sao_Paulo"],["St Johns","NST3:30NDT,M3.2.0,M11.1.0","America/St_Johns"],["Toronto","EST5EDT,M3.2.0,M11.1.0","America/Toronto"],["Vancouver","MST7","America/Vancouver"]],"Asia":[["Almaty","<+05>-5","Asia/Almaty"],["Baghdad","<+03>-3","Asia/Baghdad"],["Baku","<+04>-4","Asia/Baku"],["Bangkok","<+07>-7","Asia/Bangkok"],["Dhaka","<+06>-6","Asia/Dhaka"],["Dubai","<+04>-4","Asia/Dubai"],["Ho Chi Minh","<+07>-7","Asia/Ho_Chi_Minh"],["Hong Kong","HKT-8","Asia/Hong_Kong"],["Jakarta","WIB-7","Asia/Jakarta"],["Jerusalem","IST-2IDT,M3.4.4/26,M10.5.0","Asia/Jerusalem"],["Kabul","<+0430>-4:30","Asia/Kabul"],["Karachi","PKT-5","Asia/Karachi"],["Kathmandu","<+0545>-5:45","Asia/Kathmandu"],["Kolkata","IST-5:30","Asia/Kolkata"],["Kuala Lumpur","<+08>-8","Asia/Kuala_Lumpur"],["Manila","PST-8","Asia/Manila"],["Riyadh","<+03>-3","Asia/Riyadh"],["Seoul","KST-9","Asia/Seoul"],["Shanghai","CST-8","Asia/Shanghai"],["Singapore","<+08>-8","Asia/Singapore"],["Taipei","CST-8","Asia/Taipei"],["Tashkent","<+05>-5","Asia/Tashkent"],["Tbilisi","<+04>-4","Asia/Tbilisi"],["Tehran","<+0330>-3:30","Asia/Tehran"],["Tokyo","JST-9","Asia/Tokyo"],["Yangon","<+0630>-6:30","Asia/Yangon"]],"Africa":[["Abidjan","GMT0","Africa/Abidjan"],["Accra","GMT0","Africa/Accra"],["Addis Ababa","EAT-3","Africa/Addis_Ababa"],["Algiers","CET-1","Africa/Algiers"],["Cairo","EET-2EEST,M4.5.5/0,M10.5.4/24","Africa/Cairo"],["Casablanca","XXX-2<+01>-1,0/0,J365/23","Africa/Casablanca"],["Johannesburg","SAST-2","Africa/Johannesburg"],["Lagos","WAT-1","Africa/Lagos"],["Nairobi","EAT-3","Africa/Nairobi"],["Tunis","CET-1","Africa/Tunis"]],"Australia":[["Adelaide","ACST-9:30ACDT,M10.1.0,M4.1.0/3","Australia/Adelaide"],["Brisbane","AEST-10","Australia/Brisbane"],["Darwin","ACST-9:30","Australia/Darwin"],["Hobart","AEST-10AEDT,M10.1.0,M4.1.0/3","Australia/Hobart"],["Melbourne","AEST-10AEDT,M10.1.0,M4.1.0/3","Australia/Melbourne"],["Perth","AWST-8","Australia/Perth"],["Sydney","AEST-10AEDT,M10.1.0,M4.1.0/3","Australia/Sydney"]],"Pacific":[["Auckland","NZST-12NZDT,M9.5.0,M4.1.0/3","Pacific/Auckland"],["Fiji","<+12>-12","Pacific/Fiji"],["Guam","ChST-10","Pacific/Guam"],["Honolulu","HST10","Pacific/Honolulu"],["Tahiti","<-10>10","Pacific/Tahiti"]],"Atlantic":[["Azores","<-01>1<+00>,M3.5.0/0,M10.5.0/1","Atlantic/Azores"],["Canary","WET0WEST,M3.5.0/1,M10.5.0","Atlantic/Canary"],["Cape Verde","<-01>1","Atlantic/Cape_Verde"],["Reykjavik","GMT0","Atlantic/Reykjavik"]],"Indian":[["Maldives","<+05>-5","Indian/Maldives"],["Mauritius","<+04>-4","Indian/Mauritius"],["Reunion","<+04>-4","Indian/Reunion"]],"UTC":[["UTC","UTC0","UTC"]]};
const TZBYNAME={};for(const g in TZ)for(const [city,posix,name] of TZ[g])TZBYNAME[name]=posix;

// Which dropdown entry represents the stored config? Prefer the stored IANA name (the city
// the user picked); fall back to the first city sharing the stored POSIX string (configs
// from before the dropdown existed); null means "not in the list" -> the custom field.
function tzKnown(ntp){
  if(ntp.timezone_name&&TZBYNAME[ntp.timezone_name]===ntp.timezone)return ntp.timezone_name;
  for(const g in TZ)for(const e of TZ[g])if(e[1]===ntp.timezone)return e[2];
  return null;
}
function tzOptions(ntp){
  const sel=tzKnown(ntp);
  let h='';
  for(const g in TZ){
    h+=`<optgroup label="${g}">`+TZ[g].map(([city,posix,name])=>
      `<option value="${name}" ${name===sel?'selected':''}>${esc(city)}</option>`).join('')+'</optgroup>';
  }
  return h+`<option value="__custom" ${sel===null?'selected':''}>Custom (POSIX string)…</option>`;
}
window.tzToggle=()=>{$('#tzcustom').classList.toggle('hide',$('#c_ntptz').value!=='__custom')};

// ---------------- Settings ----------------
let cfgDrivers=null, cfgBefore=null;

// Which settings only take effect after a restart, and why.
//
// Everything the firmware reads once during setup() or startOutputs() is in here. Anything
// read live on each use (bridge name, admin credentials) is not. Vague is useless: "some
// settings need a restart" leaves the user guessing which, and whether it happens by itself.
const RESTART_NEEDED={
  'wifi.ssid':'WiFi network',
  'wifi.password':'WiFi password',
  'wifi.hostname':'Hostname',
  'mqtt.enabled':'MQTT on/off','mqtt.host':'MQTT broker','mqtt.port':'MQTT port',
  'mqtt.username':'MQTT username','mqtt.password':'MQTT password',
  'mqtt.base_topic':'MQTT base topic','mqtt.discovery_enabled':'Home Assistant discovery',
  'modbus.enabled':'Modbus on/off','modbus.port':'Modbus port','modbus.unit_id':'Modbus unit ID',
  'polling.interval_seconds':'Polling interval',
  'driver.id':'Active driver','driver.options':'Driver options',
  'ntp.enabled':'NTP on/off','ntp.use_dhcp':'NTP via DHCP','ntp.server':'NTP server',
  'ntp.timezone':'Timezone',
  'serial.override':'RS485 line override','serial.baud_rate':'RS485 baud rate',
  'serial.parity':'RS485 parity','serial.data_bits':'RS485 data bits',
  'serial.stop_bits':'RS485 stop bits',
  'additional_devices':'Extra devices',
};
// Applied immediately, no restart:
//   bridge_name          - read fresh on every status response
//   security.*           - read fresh on every authenticated request
//   logging.level        - applied on save

function pick(o,path){return path.split('.').reduce((x,k)=>x&&x[k],o)}

async function renderConfig(){
  const c=await (await fetch('/api/v1/config')).json();
  cfgBefore=c;
  if(!cfgDrivers)cfgDrivers=await (await fetch('/api/v1/drivers')).json();
  // The Relays card keys off the board's relay count, which normally arrives with the
  // status refresh -- but this form renders once per session, and a fast click on the
  // Settings tab can beat the first status response. Establish the count here rather
  // than caching a card-less form for the whole session.
  if(window.g_relayCount===undefined){
    try{const s=await(await fetch('/api/v1/status')).json();
      window.g_relayCount=(s.bridge.relays||[]).length;
      window.g_maxDevices=s.bridge.max_devices||window.g_maxDevices}
    catch(e){window.g_relayCount=0}
  }

  // autocomplete=off on every plain settings field: a text input directly above a password
  // input (MQTT username + password) otherwise reads as a login form to password managers,
  // which then autofill the saved heliograph ADMIN credential into the pair. The admin
  // credential belongs only in the auth dialog; none of these config fields are logins.
  const txt=(id,label,val,hint='')=>`<label for="${id}">${label}</label>
    <input id="${id}" autocomplete="off" value="${esc(val??'')}">${hint?`<div class="hint dim" style="font-size:12px">${hint}</div>`:''}`;
  // readonly-until-focus: browsers and password managers autofill saved credentials into
  // password fields at render time, ignoring autocomplete="new-password". An autofilled
  // field is indistinguishable from a typed one at save, so a user changing an unrelated
  // setting would silently overwrite the stored password with whatever the browser guessed
  // (live, 2026-07-21: saving a log-level change reported "MQTT password" as modified).
  // Autofill skips readonly fields; a deliberate click/focus unlocks typing.
  const pw=(id,label,isSet)=>`<label for="${id}">${label}</label>
    <input id="${id}" type="password" placeholder="${isSet?'(unchanged)':'(not set)'}" autocomplete="new-password" readonly onfocus="this.removeAttribute('readonly')">
    <div class="dim" style="font-size:12px">Leave blank to keep. The current value is never sent to this page.</div>`;
  // Credential-like text field (the MQTT username): never returned by GET, so never
  // pre-filled -- blank means keep, typing changes it, exactly like the password fields.
  // Visible text, not dots (a username is not secret to its owner), autocomplete=off for the
  // same login-form reason as the other settings fields.
  // readonly-until-focus like pw() above, and for the same reason: a text field sitting on top
  // of a password field is a login form to a password manager, and autocomplete="off" is widely
  // ignored for that pair. An autofilled username here would PATCH security.admin_username on
  // every unrelated save. autocapitalize/autocorrect off because HTTP Basic compares bytes: a
  // phone that capitalises the first letter stores a name its owner cannot see is different.
  const credtxt=(id,label,isSet,hint)=>`<label for="${id}">${label}</label>
    <input id="${id}" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false"
      placeholder="${isSet?'(unchanged)':'(not set)'}" readonly onfocus="this.removeAttribute('readonly')">
    <div class="dim" style="font-size:12px">${hint}</div>`;
  const num=(id,label,val)=>`<label for="${id}">${label}</label><input id="${id}" type="number" value="${val}">`;
  const chk=(id,label,val)=>`<label style="display:flex;gap:8px;align-items:center;margin-top:12px">
    <input id="${id}" type="checkbox" ${val?'checked':''} style="width:auto"> ${label}</label>`;

  // Options of the driver *selected in the dropdown*, not the active one. These must follow
  // the dropdown: rendering only the active driver's options meant that switching drivers kept
  // the old driver's fields on screen, and save then wrote those into the new driver's config
  // (observed live: one driver's option stored under another driver's id). Stored values
  // apply only when the selection IS the active driver; otherwise the declared defaults do.
  const optsFor=id=>{
    const drv=(cfgDrivers.drivers||[]).find(x=>x.id===id);
    if(!drv)return'';
    // What this driver would configure on its own. Read-only here on purpose: it is a property
    // of the protocol, not a per-install choice. It stopped being the last word once the RS485
    // line card gained an override, so it says which one is actually in force rather than
    // stating the driver's list as fact.
    const serial=(drv.serial_profiles||[]).map(p=>
      `${p.baud_rate} ${p.data_bits}${p.parity[0].toUpperCase()}${p.stop_bits}`).join(', ');
    const overridden=c.serial&&c.serial.override;
    return `<div class="dim" style="font-size:12px;margin-top:4px">${esc(drv.description||'')}</div>`+
      (serial?`<div class="dim" style="font-size:12px;margin-top:4px">Serial: ${serial} — set by the driver${drv.serial_profiles.length>1?' (extended discovery tries them in order)':''}.${overridden?' <b>Overridden</b> by the RS485 line card above.':''}</div>`:'')+
      (drv.options||[]).map(o=>{
      const stored=id===c.driver.id?(c.driver.options||{})[o.key]:undefined;
      const cur=stored??o.default_value;
      if(o.allowed_values&&o.allowed_values.length){
        // A stored value the firmware does not recognise -- a hand-edited config, or an
        // option value that disappeared in a firmware update -- gets its own selected entry
        // so it stays VISIBLE and stays the select's value. Without it the browser silently
        // selects the first option, and the untouched-card diffing in the save path then
        // sees a difference and writes that fallback into the config on the next unrelated
        // save: a setting the user never looked at, quietly changed. Reachable since the
        // driver profile option became an enumerated list (2026-07-24).
        const known=o.allowed_values.includes(cur);
        // Explicit value= on every option: without it the browser takes the option's TEXT as
        // its value, so the annotated entry below would submit its own label.
        return `<label for="opt_${o.key}">${esc(o.display_name)}</label>
          <select id="opt_${o.key}" data-opt="${esc(o.key)}">${
            (known?'':`<option value="${esc(cur)}" selected>${esc(cur)} — not recognised</option>`)+
            o.allowed_values.map(v=>
            `<option value="${esc(v)}" ${v===cur?'selected':''}>${v===''?'(driver default)':esc(v)}</option>`).join('')}</select>
          <div class="dim" style="font-size:12px">${esc(o.description||'')}</div>`;
      }
      const num=o.min_value!==undefined
        ? ` type="number" min="${esc(o.min_value)}" max="${esc(o.max_value)}"` : '';
      return `<label for="opt_${o.key}">${esc(o.display_name)}</label>
        <input id="opt_${o.key}"${num} data-opt="${esc(o.key)}" value="${esc(cur)}">`;
    }).join('');
  };
  window.reloadDriverOpts=()=>{$('#drvopts').innerHTML=optsFor($('#c_drv').value)};

  // --- extra devices -------------------------------------------------------------------
  // Rendered from an array rather than from the DOM, so adding, removing and re-rendering a
  // row cannot get out of step with what will be saved. Every row carries a driver id and that
  // driver's declared options at their stored-or-declared values -- the same generic mechanism
  // the Driver card uses, so a new driver's options appear here with no change either.
  //
  // Deliberately NOT pre-filled with anything when a row is added: an extra device that names
  // no driver is refused by the firmware, and an address silently defaulting to the same one
  // the primary uses would collide and be skipped at boot with only a log line to say so.
  let xdevs = ((c.additional_devices)||[]).map(d=>({id:d.driver_id||'',options:{...(d.options||{})}}));

  // Fills a row's model with the driver's declared defaults. Called when a row is added or its
  // driver changes, so the model always holds what the fields SHOW.
  //
  // The bug this exists to close: the fields rendered `row.options[key] ?? default_value`, but
  // only an edit wrote to the model, so an untouched field displayed the driver's default and
  // sent nothing. The firmware then applied that same default anyway -- unit id 1, the primary's
  // address -- and skipped the device at boot with one log line. The commit that added this card
  // claimed a new row "starts empty on purpose" to prevent exactly that; it did not (review).
  const seedRowOptions=row=>{
    const drv=(cfgDrivers.drivers||[]).find(x=>x.id===row.id);
    for(const o of ((drv&&drv.options)||[])){
      if(row.options[o.key]===undefined)row.options[o.key]=o.default_value??'';
    }
  };

  const xdevOptionFields=(row,index)=>{
    const drv=(cfgDrivers.drivers||[]).find(x=>x.id===row.id);
    return ((drv&&drv.options)||[]).map(o=>{
      const cur=row.options[o.key]??o.default_value??'';
      const hint=o.description?`<div class="dim" style="font-size:12px">${esc(o.description)}</div>`:'';
      if(o.allowed_values&&o.allowed_values.length){
        // A stored value the firmware no longer recognises keeps its own entry, exactly as the
        // Driver card does. Without it the browser silently selects the first option while the
        // model keeps the old value, every save resends it, and the REST gate refuses the whole
        // configuration -- the lockout class this codebase has already had to remove once.
        const known=o.allowed_values.includes(cur);
        // An empty entry among the allowed values is the driver saying "unset means my default".
        // On this card that is a trap: for a register map it silently means the DEFAULT map, so
        // it is labelled and left unselected until someone picks, like the wizard does.
        const hasBlank=o.allowed_values.includes('');
        return `<label for="xd${index}_${esc(o.key)}">${esc(o.display_name)}</label>
          <select id="xd${index}_${esc(o.key)}" ${hasBlank?'data-mustpick="1"':''}
            onchange="setExtraOption(${index},'${esc(o.key)}',this.value)">${
            (known?'':`<option value="${esc(cur)}" selected>${esc(cur)} — not recognised</option>`)+
            o.allowed_values.map(v=>`<option value="${esc(v)}" ${known&&v===cur?'selected':''}>${
              v===''?'— choose —':esc(v)}</option>`).join('')}</select>${hint}`;
      }
      // A bounded option gets a number field with the driver's own limits. The firmware
      // refuses an out-of-range value either way; showing the bounds is what stops someone
      // discovering them from an error message after a save.
      const num=o.min_value!==undefined
        ? ` type="number" min="${esc(o.min_value)}" max="${esc(o.max_value)}"` : '';
      return `<label for="xd${index}_${esc(o.key)}">${esc(o.display_name)}</label>
        <input id="xd${index}_${esc(o.key)}"${num} value="${esc(cur)}"
          oninput="setExtraOption(${index},'${esc(o.key)}',this.value)">${hint}`;
    }).join('');
  };

  window.renderExtraDevices=()=>{
    const box=$('#xdevs');
    if(!box)return;
    box.innerHTML=xdevs.map((row,i)=>`
      <div style="border:1px solid var(--line);border-radius:8px;padding:12px;margin-top:10px">
        <div style="display:flex;justify-content:space-between;align-items:baseline">
          <b>Device ${i+2}</b>
          <a href="#" onclick="removeExtraDevice(${i});return false">Remove</a></div>
        <label for="xd${i}_drv">Driver</label>
        <select id="xd${i}_drv" onchange="setExtraDriver(${i},this.value)">
          <option value="" ${row.id?'':'selected'}>— choose —</option>${
          (cfgDrivers.drivers||[]).map(d=>
            `<option value="${esc(d.id)}" ${d.id===row.id?'selected':''}>${esc(d.display_name)}</option>`).join('')}
          ${row.id&&!(cfgDrivers.drivers||[]).some(d=>d.id===row.id)?
            `<option value="${esc(row.id)}" selected>${esc(row.id)} — not recognised</option>`:''}
        </select>
        ${xdevOptionFields(row,i)}
      </div>`).join('')||
      '<div class="dim" style="font-size:12px;margin-top:8px">None. This bridge polls one inverter.</div>';
    const add=$('#xdevadd');
    if(add)add.disabled=xdevs.length+1>=(window.g_maxDevices||8);
  };
  window.addExtraDevice=()=>{xdevs.push({id:'',options:{}});renderExtraDevices()};
  window.removeExtraDevice=i=>{xdevs.splice(i,1);renderExtraDevices()};
  window.setExtraDriver=(i,id)=>{
    // Options belong to a driver, so switching driver drops the old one's values rather than
    // carrying keys the new driver never declared -- which the firmware would refuse anyway.
    xdevs[i]={id,options:{}};
    seedRowOptions(xdevs[i]);
    renderExtraDevices();
  };
  window.setExtraOption=(i,key,value)=>{xdevs[i].options[key]=value};
  window.extraDevicesBody=()=>xdevs.map(d=>({driver_id:d.id,options:{...d.options}}));

  // What the firmware would refuse, or accept and then quietly skip at boot -- said here, next
  // to the row, instead of as `additional_devices[2].driver_id: ...` under the Save button,
  // naming an index the card never shows. A boot-time skip is one warn line in a log buffer
  // nobody is watching, so a mistake made here has to be caught here.
  window.extraDevicesProblem=()=>{
    const seen={};
    // The primary's address counts too -- it is the one every default collides with.
    const drvNow=$('#c_drv')?$('#c_drv').value:'';
    const primaryAddr=(()=>{
      const e=document.querySelector('#cfgform [data-opt="unit_id"], #cfgform [data-opt="address"]');
      return e?drvNow+'|'+e.value.trim():null;
    })();
    if(primaryAddr)seen[primaryAddr]='the inverter under Driver';
    for(let i=0;i<xdevs.length;i++){
      const label='Device '+(i+2);
      const row=xdevs[i];
      if(!row.id)return label+': pick a driver, or remove the row.';
      const drv=(cfgDrivers.drivers||[]).find(x=>x.id===row.id);
      for(const o of ((drv&&drv.options)||[])){
        const v=(row.options[o.key]??'').trim();
        if(o.allowed_values&&o.allowed_values.includes('')&&v===''){
          return label+': choose a '+o.display_name.toLowerCase()+
                 ' — leaving it unset silently uses the driver default.';
        }
      }
      // Same driver at the same address is the same device. The firmware skips the second with
      // a log line; here it is a sentence.
      const addr=row.options.unit_id??row.options.address;
      if(addr!==undefined){
        const key=row.id+'|'+String(addr).trim();
        if(seen[key])return label+' has the same address as '+seen[key]+
                             '. Each inverter on the bus needs its own.';
        seen[key]=label;
      }
    }
    return null;
  };
  const driverOpts=`<span id="drvopts">${optsFor(c.driver.id)}</span>`;

  $('#cfgform').innerHTML=`
  <section class="cfgsec"><h3>Network</h3>
  <div class="card"><b>Bridge</b> <span class="tag" style="font-weight:400">name applied immediately</span>${txt('c_name','Name',c.bridge_name,
      'Display name: shown in Home Assistant and on this dashboard. Spaces are fine.')}
    ${txt('c_host','Hostname',c.wifi.hostname,
      'Network name: this bridge is http://'+esc(c.wifi.hostname)+'.local. Letters, digits and hyphens only; applied after restart.')}</div>
  <div class="card"><b>WiFi</b> <span class="tag" style="font-weight:400">needs restart</span>
    <label for="c_ssid">SSID</label>
    <input id="c_ssid" autocomplete="off" value="${esc(c.wifi.ssid??'')}">
    <button type="button" id="c_scanbtn" style="margin-top:6px" onclick="scanNetworksList()">Scan networks</button>
    <span id="c_scanmsg" class="dim" style="font-size:12px"></span>
    <select id="c_ssidpick" style="display:none;margin-top:6px"
      onchange="if(this.value){document.querySelector('#c_ssid').value=this.value}"></select>
    ${pw('c_wpw','Password',c.wifi.password_set)}</div>
  <div class="card"><b>Time (NTP)</b> <span class="tag" style="font-weight:400">needs restart</span>${chk('c_ntpe','Enabled',c.ntp.enabled)}
    ${chk('c_ntpd','Use NTP server from DHCP',c.ntp.use_dhcp)}
    ${txt('c_ntps','NTP server (fallback)',c.ntp.server)}
    <label for="c_ntptz">Timezone</label>
    <select id="c_ntptz" onchange="tzToggle()">${tzOptions(c.ntp)}</select>
    <span id="tzcustom" class="${tzKnown(c.ntp)?'hide':''}">
      <label for="c_ntptzc">POSIX TZ string</label>
      <input id="c_ntptzc" value="${esc(c.ntp.timezone)}">
      <div class="dim" style="font-size:12px">For zones not in the list, e.g. CET-1CEST,M3.5.0,M10.5.0/3.</div>
    </span>
    <div class="dim" style="font-size:12px;margin-top:8px">A DHCP-provided server wins; the
    fallback is used when the network offers none.</div></div>
  </section>
  <section class="cfgsec"><h3>MQTT &amp; Home Assistant</h3>
  <div class="card"><b>MQTT</b> <span class="tag" style="font-weight:400">needs restart</span>${chk('c_mqe','Enabled',c.mqtt.enabled)}
    ${txt('c_mqh','Broker host',c.mqtt.host)}${num('c_mqp','Port',c.mqtt.port)}
    ${credtxt('c_mqu','Username',c.mqtt.username_set,'Leave blank to keep. Not shown here; send an empty string via the API to clear it (null is a no-op for usernames).')}${pw('c_mqpw','Password',c.mqtt.password_set)}
    ${txt('c_mqt','Base topic',c.mqtt.base_topic)}
    ${chk('c_mqd','Home Assistant discovery',c.mqtt.discovery_enabled)}</div>
  </section>
  <section class="cfgsec"><h3>Modbus TCP</h3>
  <div class="card"><b>Modbus TCP</b> <span class="tag" style="font-weight:400">needs restart</span>${chk('c_mbe','Enabled',c.modbus.enabled)}
    ${num('c_mbp','Port',c.modbus.port)}${num('c_mbu','Unit ID',c.modbus.unit_id)}
    <div class="dim" style="font-size:12px;margin-top:8px">Writing is permanently disabled:
    no driver in this build can write to an inverter.</div></div>
  </section>
  <section class="cfgsec"><h3>REST &amp; Prometheus</h3>
  <!-- Read-only by necessity: neither output has a setting. Both are always on, both listen on
       the web server's own port, and neither can be turned off short of a rebuild. Saying so in
       one card beats an empty section under a heading the user came looking for -- and it is the
       only place that answers "what do I point my scraper at". -->
  <div class="card"><b>Endpoints</b> <span class="tag" style="font-weight:400">no settings</span>
    <div class="dim" style="font-size:12px;margin-top:10px">Always available on this bridge's
    address, on the same port as this page. Nothing here to configure.</div>
    <table style="margin-top:10px"><tbody>
    <tr><td>Prometheus</td><td><code>/metrics</code></td></tr>
    <tr><td>REST</td><td><code>/api/v1/status</code>, <code>/devices</code>,
      <code>/diagnostics</code>, <code>/config</code></td></tr>
    <tr><td>Live updates</td><td><code>/api/v1/events</code> (SSE)</td></tr>
    </tbody></table>
    <div class="dim" style="font-size:12px;margin-top:8px">Reads need no password — the
    threat model is a trusted LAN. Everything that <i>changes</i> something does. See
    docs/security.md and docs/prometheus.md.</div></div>
  </section>
  <section class="cfgsec"><h3>Device &amp; driver</h3>
  <div class="card"><b>Polling</b> <span class="tag" style="font-weight:400">needs restart</span>${num('c_pi','Interval (seconds)',c.polling.interval_seconds)}</div>
  <div class="card"><b>RS485 line</b> <span class="tag" style="font-weight:400">needs restart</span>
    ${chk('c_ser','Override the line settings the driver chooses',c.serial.override)}
    <div class="dim" style="font-size:12px;margin-top:6px">Off, the driver configures the line
    itself — right for almost every install. The discovery wizard turns this on by itself when
    it finds a device at a profile the driver does not lead with, because the driver would
    otherwise go back to its own default on the next boot and the bus would fall silent.</div>
    <label for="c_serbaud">Baud rate</label>
    <input id="c_serbaud" type="number" value="${c.serial.baud_rate}">
    <label for="c_serpar">Parity</label>
    <select id="c_serpar">${['none','even','odd'].map(x=>
      `<option value="${x}" ${c.serial.parity===x?'selected':''}>${x}</option>`).join('')}</select>
    <label for="c_serdb">Data bits</label>
    <input id="c_serdb" type="number" value="${c.serial.data_bits}">
    <label for="c_sersb">Stop bits</label>
    <input id="c_sersb" type="number" value="${c.serial.stop_bits}"></div>
  <div class="card"><b>Driver</b> <span class="tag" style="font-weight:400">needs restart</span>
    <label for="c_drv">Active driver</label>
    <select id="c_drv" onchange="reloadDriverOpts()">${
      // A stored id that matches no option selects nothing, so the browser silently shows the
      // first driver -- and the save path then diffs that against the stored value, sees a
      // change and PATCHes it. The DEFAULT state hits this: a freshly provisioned bridge stores
      // "" meaning "let the firmware pick the highest-priority driver", so saving any unrelated
      // setting would quietly pin the driver to whichever one happens to sort first. Same class
      // as the driver-option select below; fixed here too (review, 2026-07-25).
      (((cfgDrivers.drivers||[]).some(d=>d.id===c.driver.id))?'':
        `<option value="${esc(c.driver.id)}" selected>${c.driver.id?esc(c.driver.id)+' — not recognised':'(firmware picks automatically)'}</option>`)+
      (cfgDrivers.drivers||[]).map(d=>
      `<option value="${esc(d.id)}" ${d.id===c.driver.id?'selected':''}>${esc(d.display_name)} (${esc(d.support_level)})</option>`).join('')}</select>
    ${driverOpts}
  </div>
  <div class="card"><b>Extra devices</b> <span class="tag" style="font-weight:400">needs restart</span>
    <div class="dim" style="font-size:12px">More inverters on the SAME RS485 bus, polled in
    turn after the one above. Each needs its own address — set that in the device's own menu
    first, and give it the driver's address option here. Up to ${window.g_maxDevices||8} devices
    in total, this card holds the rest.</div>
    <div id="xdevs"></div>
    <button type="button" id="xdevadd" onclick="addExtraDevice()"
      style="background:none;border:1px solid var(--line);color:var(--fg);margin-top:10px">Add a device</button>
    <div class="dim" style="font-size:12px;margin-top:8px">Removing a device here — or changing
    its address — does not remove what it already published: the old entities stay in Home
    Assistant, <b>available</b>, showing their last value. See docs/mqtt.md.</div>
  </div>
  </section>
  ${window.g_relayCount>0?`<section class="cfgsec"><h3>Relays &amp; DRM</h3>
  <div class="card"><b>Relays</b> <span class="tag" style="font-weight:400">applied immediately</span>
    ${chk('c_rle','Enabled',(c.relays||{}).enabled)}
    <!-- The second gate, named where it bites. Enabling relays while read-only mode is still
         on is a silent dead end: the card looks configured, the switches appear in Home
         Assistant, and nothing ever actuates (live, first Relay-6CH bring-up 2026-07-23).
         Shown/hidden from the live checkboxes below, not from the saved config. -->
    <div id="rlgate" class="msg err" role="alert" style="display:none;font-size:13px">Relays are enabled, but
      read-only mode is still on — no relay will move. Turn off
      <a href="#" onclick="return focusReadOnly()">Read-only mode</a> under Security below and save.</div>
    ${Array.from({length:window.g_relayCount},(_, i)=>`
      <label for="c_rlr${i}">Relay ${i+1} role</label>
      <select id="c_rlr${i}" data-role="${i}">${['none','drm0','drm1','drm2','drm3','drm4','drm5','drm6','drm7','drm8'].map(r=>
        `<option ${r===(((c.relays||{}).roles||[])[i]||'none')?'selected':''}>${r}</option>`).join('')}</select>`).join('')}
    <div class="dim" style="font-size:12px;margin-top:8px">DRM curtailment contacts. Two
    locks must open before a relay can move: this switch AND read-only mode (under Security,
    below) being off.
    Disabling releases every relay. Roles name the switches in Home Assistant and build
    the DRM Mode select; see docs/drm.md for the wiring rules (failsafe: a dead bridge
    must leave the inverter running).</div></div>
  </section>`:''}
  <section class="cfgsec"><h3>Security &amp; logging</h3>
  <div class="card"><b>Security</b> <span class="tag" style="font-weight:400">applied immediately</span>${credtxt('c_au','Admin username',true,'Leave blank to keep. Not shown here — it is half of the login and this page is readable without one. <b>Write down anything other than “admin”: it cannot be read back, and a forgotten username needs a factory reset.</b>')}
    ${pw('c_ap','Admin password',c.security.password_set)}
    <!-- !==false, not a plain truthiness test: a missing field must render as ON. If GET ever
         stops carrying read_only_mode (a serialiser regression, an older firmware behind a
         proxy), a truthy test renders the box unchecked, and the diff below then reads that as
         a deliberate change and PATCHes read_only_mode:false -- pressing Save without touching
         anything would silently open the global write gate. Absent means on; the only way past
         this switch stays a deliberate click. -->
    ${chk('c_ro','Read-only mode',c.security.read_only_mode!==false)}
    <div class="dim" style="font-size:12px;margin-top:4px">The global write kill switch, on by
    default, and the outermost gate on everything this bridge can change. While it is on the
    bridge only observes: every inverter command is refused and no relay moves, whatever the
    Relays card says. <b>Turning it off permits writes to the inverter and to the relay
    outputs.</b> No driver in this build can write to an inverter, so today this unlocks the
    relay/DRM contacts — leave it on unless you are deliberately using them.</div></div>
  <div class="card"><b>Logging</b> <span class="tag" style="font-weight:400">applied immediately</span>
    <label for="c_lg">Level</label><select id="c_lg">${['error','warn','info','debug','trace'].map(l=>
      `<option ${l===c.logging.level?'selected':''}>${l}</option>`).join('')}</select></div>
  </section>
  <!-- Save applies everything above it and nothing below: OTA, restart and factory reset each
       act the moment their own button is pressed. Keeping it out of the last card, rather than
       inside one, is what makes that boundary visible. -->
  <div class="cfgsave">
    <button onclick="saveConfig()">Save settings</button>
    <div id="cm" class="msg" style="display:none"></div>
  </div>
  <section class="cfgsec"><h3>Backup &amp; restore</h3>
  <div class="card"><b>Download a backup</b>
    <div class="dim" style="font-size:12px;margin-top:10px">One file with every setting on this
    page. Keep it somewhere other than this bridge — it is what turns a dead board into a
    twenty-minute job instead of an evening of remembering what you configured.</div>
    ${chk('bk_sec','Include passwords (WiFi, MQTT, admin)',false)}
    <div class="dim" style="font-size:12px;margin-top:4px"><b>Off by default, and think before
    turning it on.</b> With it on the file holds those passwords <b>in plain text</b>, and it
    will sit in your downloads folder, sync to whatever cloud drive is watching it, and be the
    obvious thing to attach to a bug report. With it off the file is safe to keep anywhere, and
    restoring it onto <i>this</i> bridge still works — an absent password means “keep the one
    the bridge already has”. Only a factory-reset board needs them typed again.</div>
    <button type="button" onclick="downloadBackup()">Download backup</button>
    <div id="bkm" class="msg" style="display:none"></div></div>
  <div class="card"><b>Restore from a backup</b>
    <div class="dim" style="font-size:12px;margin-top:10px">Nothing is applied until you have
    seen exactly what would change and pressed the second button.</div>
    <label for="rs_file">Backup file (.json)</label>
    <input id="rs_file" type="file" accept=".json,application/json">
    <button type="button" onclick="previewRestore()">Show what would change</button>
    <div id="rsm" class="msg" style="display:none"></div>
    <div id="rspv"></div></div>
  <div class="card"><b>Undo the last restore</b>
    <div class="dim" style="font-size:12px;margin-top:10px">The bridge keeps the configuration
    it had immediately before the most recent restore, so a wrong file does not cost you a
    factory reset. Only that one: an ordinary Save does not create a restore point.</div>
    <button type="button" style="background:none;border:1px solid var(--line);color:var(--fg)"
      onclick="undoRestore()">Go back to the previous configuration</button>
    <div id="rbm" class="msg" style="display:none"></div></div>
  </section>
  <section class="cfgsec"><h3>Firmware &amp; recovery</h3>
  <div class="card"><b>Firmware update (OTA)</b>
    <label for="c_fw">Firmware image (.bin)</label>
    <input id="c_fw" type="file" accept=".bin">
    <button id="ob" onclick="otaUpload()">Upload and install</button>
    <div id="opb" style="display:none;height:8px;max-width:420px;margin-top:12px;border:1px solid var(--line);border-radius:99px;overflow:hidden">
      <div id="opf" style="height:100%;width:0%;background:#2f81f7;transition:width .2s"></div></div>
    <div id="om" class="msg" style="display:none"></div>
    <div class="dim" style="font-size:12px;margin-top:8px">The image is verified before the
    boot partition switches; a rejected upload leaves the running firmware untouched.</div></div>
  <div class="card">
    <b>Restart</b>
    <p class="dim">Reboots the bridge; all settings are kept. Polling resumes by itself and
    the dashboard reconnects in ~30 seconds.</p>
    <button onclick="rebootFromSettings()">Restart bridge</button>
  </div>
  <div class="card" style="border-color:var(--bad)">
    <b>Factory reset</b>
    <p class="dim">Erases everything, including WiFi and passwords, and restarts into the setup
    portal. The board's physical RESET button only reboots — this page is the way back from a
    bad configuration, as long as you can still reach it.</p>
    <button style="background:var(--bad)" onclick="factoryReset()">Erase and restart</button>
  </div>
  </section>`;

  // After the card exists in the DOM, not before: renderExtraDevices() writes into #xdevs.
  renderExtraDevices();

  // Keep the Relays card's gate warning in step with both checkboxes as they are clicked --
  // the user is told the combination is dead before saving it, not after wondering why the
  // relays are silent.
  if(window.g_relayCount>0){
    const upd=()=>{$('#rlgate').style.display=($('#c_rle').checked&&$('#c_ro').checked)?'block':'none'};
    $('#c_rle').onchange=upd;$('#c_ro').onchange=upd;upd();
  }
}

// The Relays card's link to the switch that is actually blocking it. Focus as well as scroll:
// on a long settings page a jump alone leaves the user hunting for which control was meant.
function focusReadOnly(){
  const e=$('#c_ro');
  e.scrollIntoView({block:'center',behavior:'smooth'});
  e.focus();
  return false;
}

async function saveConfig(){
  const v=id=>$(id).value, n=id=>Number($(id).value), b=id=>$(id).checked;
  const body={bridge_name:v('c_name'),
    ...(window.g_relayCount>0?{relays:{enabled:b('c_rle'),
      roles:Array.from({length:window.g_relayCount},(_,i)=>v('c_rlr'+i))}}:{}),
    wifi:{ssid:v('c_ssid'),hostname:v('c_host')},
    mqtt:{enabled:b('c_mqe'),host:v('c_mqh'),port:n('c_mqp'),
          base_topic:v('c_mqt'),discovery_enabled:b('c_mqd')},
    modbus:{enabled:b('c_mbe'),port:n('c_mbp'),unit_id:n('c_mbu')},
    polling:{interval_seconds:n('c_pi')},
    ntp:{enabled:b('c_ntpe'),use_dhcp:b('c_ntpd'),server:v('c_ntps'),
         // Dropdown value is an IANA name; the firmware only understands the POSIX string, so
         // translate here. Custom passes the raw string through with no name attached.
         ...(v('c_ntptz')==='__custom'
             ?{timezone:v('c_ntptzc'),timezone_name:''}
             :{timezone:TZBYNAME[v('c_ntptz')],timezone_name:v('c_ntptz')})},
    driver:{id:v('c_drv'),options:{}},
    // read_only_mode is rendered from the stored value, so the generic per-key diff below is
    // enough: no rendered default to mistake for a change (unlike driver options / relay roles).
    // admin_username is added below only when typed, like the other credential fields.
    serial:{override:b('c_ser'),baud_rate:n('c_serbaud'),parity:v('c_serpar'),
            data_bits:n('c_serdb'),stop_bits:n('c_sersb')},
    security:{read_only_mode:b('c_ro')},
    logging:{level:v('c_lg')}};
  // A blank password field means "keep": sending "" would clear it, which is never what an
  // untouched field means.
  if(v('c_wpw'))body.wifi.password=v('c_wpw');
  // The MQTT username is credential-like (never returned by GET), so like the passwords it
  // travels only when typed -- a blank field means keep.
  if(v('c_mqu'))body.mqtt.username=v('c_mqu').trim();
  // Same rule for the admin username, and the same reason: GET no longer returns it, so there
  // is nothing to pre-fill and a blank field can only mean keep. Trimmed, because validate()
  // only rejects an empty one -- "beheerder " would be stored and then never match again, with
  // no way to see the difference and no way to read the stored value back.
  if(v('c_au').trim())body.security.admin_username=v('c_au').trim();
  if(v('c_mqpw'))body.mqtt.password=v('c_mqpw');
  if(v('c_ap'))body.security.admin_password=v('c_ap');
  // Scoped to the settings form. The wizard renders its own [data-opt] fields at step 5, and
  // both views live in the DOM at the same time -- an unscoped query saved the wizard's
  // half-finished choices into the active driver's config.
  document.querySelectorAll('#cfgform [data-opt]')
    .forEach(e=>body.driver.options[e.dataset.opt]=e.value);
  // The list travels whole -- the API replaces the array rather than merging, and there is no
  // stable key to merge on -- but ONLY when it differs from what this page loaded. Sending it
  // unconditionally gave it the one thing every other section is protected from: this form can
  // sit open for hours, so a save of something unrelated wrote its stale copy over a list
  // changed since, from another tab or a curl. An empty array still travels when the stored one
  // was not empty, which is how a device is removed.
  if(window.extraDevicesBody){
    const problem=window.extraDevicesProblem();
    if(problem){
      const m=$('#cm');
      m.className='msg err';m.textContent=problem;m.style.display='block';
      return;
    }
    // Compared here rather than with same() below: that helper is declared further down and
    // would be in its temporal dead zone at this point.
    const wanted=window.extraDevicesBody();
    if(JSON.stringify(wanted)!==JSON.stringify(cfgBefore.additional_devices||[])){
      body.additional_devices=wanted;
    }
  }

  // Send only what actually changed. This form may have been open for hours; PATCHing every
  // field would write its stale copy over anything changed elsewhere in the meantime (another
  // tab, the discovery wizard) -- which is exactly how a driver choice once quietly reverted.
  // Passwords are exempt: they are only in the body when typed, and GET never returns them.
  const same=(a,b)=>JSON.stringify(a)===JSON.stringify(b);
  if(same(body.bridge_name,cfgBefore.bridge_name))delete body.bridge_name;
  // Roles render as 'none' for entries the stored config never had -- comparing the
  // rendered defaults against the shorter stored array would mark every save as a roles
  // change (same class as the driver-options default bug, 2026-07-22). Compare against
  // the stored array padded with the same defaults.
  if(body.relays){
    const before=Array.from({length:window.g_relayCount},(_,i)=>(((cfgBefore.relays||{}).roles||[])[i]||'none'));
    if(same(body.relays.roles,before))delete body.relays.roles;
  }
  // Driver options travel only when the driver card was touched; an untouched card must not
  // re-assert its rendered options either. Compare per rendered key, not whole objects: the
  // stored map may carry stale keys from a previously active driver (the pre-fix stacking bug),
  // and a key the form does not even show is no evidence the user changed anything.
  // Compare against stored-or-declared-default: an option the user never stored renders as
  // its default, and that rendered default is not a change either -- comparing it against
  // the absent stored key reported "Driver options" changed on every unrelated save (live,
  // 2026-07-22: a log-level change announced a driver-options restart).
  const drvSel=(cfgDrivers.drivers||[]).find(x=>x.id===body.driver.id);
  const defOf=k=>{const o=((drvSel&&drvSel.options)||[]).find(x=>x.key===k);return o?o.default_value:undefined};
  const optsTouched=Object.entries(body.driver.options)
    .some(([k,val])=>!same(val,(cfgBefore.driver.options||{})[k]??defOf(k)));
  if(same(body.driver.id,cfgBefore.driver.id)&&!optsTouched)delete body.driver.options;
  for(const sect of Object.keys(body)){
    if(typeof body[sect]!=='object')continue;
    // An ARRAY section is replaced wholesale by the API, so there is nothing to diff -- and
    // diffing it is actively harmful: Object.keys() on an array yields indices, so an unchanged
    // element got `delete`d, leaving a hole that JSON.stringify writes as `null`. The firmware
    // then refuses the whole patch with "expected an object". Found by driving the real page
    // against a stub, which is the only reason it did not ship.
    if(Array.isArray(body[sect]))continue;
    for(const k of Object.keys(body[sect])){
      // Credential-like keys (passwords, both usernames) and driver options are only in the
      // body when the user acted; GET never returns them, so there is nothing to diff against
      // and they must not be dropped. admin_username used to be the exception here because it
      // WAS returned -- that is exactly what this release stopped doing.
      if(k.includes('password')||k.endsWith('username')||k==='options')continue;
      if(same(body[sect][k],(cfgBefore[sect]||{})[k]))delete body[sect][k];
    }
    if(!Object.keys(body[sect]).length)delete body[sect];
  }

  const m=$('#cm');
  if(!Object.keys(body).length){
    m.className='msg ok';m.textContent='Nothing changed.';m.style.display='block';return;
  }
  let r;
  try{
    r=await authFetch('/api/v1/config',{method:'PATCH',
      headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  }catch(e){m.className='msg err';m.textContent='Cancelled.';m.style.display='block';return}
  const d=await r.json().catch(()=>({}));
  // Renaming the admin account invalidates the credential we just authenticated with. Before
  // this release the sign-in dialog re-fetched the name from GET /config, so a rename healed
  // itself; now nothing would, and the very next admin action would 401 straight after a
  // successful rename -- the most confusing possible moment for it. Re-key on the new name,
  // reusing the password half of the credential that just worked.
  if(r.ok&&body.security&&body.security.admin_username){
    const cur=atob(sessionStorage.getItem('sb_auth')||'');
    const pw=cur.slice(cur.indexOf(':')+1);
    if(cur){
      const raw=body.security.admin_username+':'+pw;
      sessionStorage.setItem('sb_auth',
        btoa(String.fromCharCode(...new TextEncoder().encode(raw))));
      sessionStorage.setItem('sb_user',body.security.admin_username);
    }
  }
  if(!r.ok){
    m.className='msg err';
    m.textContent=(d.error&&d.error.message)||('HTTP '+r.status);
    m.style.display='block';
    return;
  }

  // Say exactly which changed settings are waiting on a restart, and offer one. The firmware
  // reads these once at boot; nothing applies them later by itself.
  const pending=[];
  for(const [path,label] of Object.entries(RESTART_NEEDED)){
    const now=pick(body,path), was=pick(cfgBefore,path);
    if(now===undefined)continue;
    // Passwords are never returned by GET /config, so a *_set flag is all we can compare
    // against: if the field was filled in at all, treat it as changed.
    if(path.endsWith('password')){ if(now)pending.push(label); continue; }
    if(JSON.stringify(now)!==JSON.stringify(was))pending.push(label);
  }
  cfgBefore=d;

  // The server is authoritative on whether a restart is needed (reboot_required); `pending`
  // only supplies the nice per-field labels. They agree for anything editable on this form;
  // the generic wording covers a reboot-only field changed through the API directly.
  m.className='msg ok';
  if(!d.reboot_required){
    m.innerHTML='Saved and applied.';
  }else{
    const which=pending.length
      ? 'These take effect after a restart: <b>'+pending.map(esc).join(', ')+'</b>.'
      : 'Some changes take effect after a restart.';
    m.innerHTML='Saved. '+which+'<br>'+
      '<button style="margin-top:10px" onclick="rebootFromSettings()">Restart now</button>'+
      '<span class="dim" style="font-size:12px"> — or later; the bridge keeps running on the '+
      'old values until you do.</span>';
  }
  m.style.display='block';
}

// XMLHttpRequest, not fetch: fetch cannot report UPLOAD progress, and a minute of silent
// "uploading…" reads as a hang. Auth mirrors authFetch (prompt once, retry once on 401).
// ---------------- Backup and restore ----------------
// The file never touches the server on the download path and never persists on the upload
// path: a Blob built in the page, and a body posted straight from a FileReader. Nothing is
// staged on the bridge between preview and apply, which is why the apply re-sends the file
// rather than confirming a token -- there is no server-side session to expire, to be raced by
// a second tab, or to apply something other than what was on screen.

function backupNote(el,cls,text){const m=$(el);m.style.display='block';m.className='msg '+cls;m.textContent=text}

async function downloadBackup(){
  const withSecrets=$('#bk_sec').checked;
  backupNote('#bkm','','Preparing…');
  const r=await authFetch('/api/v1/config/backup'+(withSecrets?'?secrets=true':''));
  // No null check: authFetch always answers with a response-shaped object, and the cancellation
  // one is ok:false with cancelled:true, which httpWhy phrases properly. A `!r` guard here
  // would silently return with nothing on screen -- the exact failure authCancelled exists to
  // prevent.
  if(!r.ok){backupNote('#bkm','err','Could not export: '+httpWhy(r));return}
  const text=await r.text();
  // Filename from the server's Content-Disposition when it survives the fetch, so the file is
  // named after the bridge that wrote it rather than after the page that asked.
  let name='heliograph-backup.json';
  const cd=r.headers.get('Content-Disposition')||'';
  const m=cd.match(/filename="([^"]+)"/);
  if(m)name=m[1];
  const url=URL.createObjectURL(new Blob([text],{type:'application/json'}));
  const a=document.createElement('a');a.href=url;a.download=name;
  // In the DOM before the click: Firefox ignores a synthetic click on a detached anchor, so a
  // download that works in Chrome does nothing at all there, silently.
  document.body.appendChild(a);a.click();a.remove();
  // Revoked on the next tick, not immediately: Safari has not started the download yet when
  // click() returns, and freeing the object URL here cancels it.
  setTimeout(()=>URL.revokeObjectURL(url),1000);
  backupNote('#bkm','ok','Downloaded '+name+(withSecrets?' — it contains passwords in plain text.'
    :' — no passwords in it.'));
}

// Held between preview and apply so the confirm sends the bytes that were previewed, not a
// re-read of a file input the user may have changed in between.
let rsPending=null;

async function readChosenBackup(){
  const f=$('#rs_file').files[0];
  if(!f){backupNote('#rsm','err','Choose a backup file first.');return null}
  try{return await f.text()}
  catch(e){backupNote('#rsm','err','Could not read that file: '+e.message);return null}
}

async function previewRestore(){
  $('#rspv').innerHTML='';
  const text=await readChosenBackup();
  if(text===null)return;
  backupNote('#rsm','','Checking the file…');
  const r=await authFetch('/api/v1/config/restore?dry_run=true',
    {method:'POST',headers:{'Content-Type':'application/json'},body:text});
  const d=await r.json().catch(()=>({}));
  if(!r.ok){
    backupNote('#rsm','err',(d.error&&d.error.message)||httpWhy(r));
    return;
  }
  rsPending=text;
  $('#rsm').style.display='none';
  const b=d.backup||{};
  const src=[b.firmware_version?'written by firmware '+esc(b.firmware_version):'',
             b.exported_at?'on '+esc(b.exported_at.replace('T',' ').replace('Z',' UTC')):'',
             b.includes_secrets?'<b>contains passwords</b>':'contains no passwords']
            .filter(Boolean).join(' · ');
  if(!d.change_count){
    $('#rspv').innerHTML=`<div class="msg ok" style="display:block">This backup matches the
      bridge's current configuration exactly — nothing would change.<div class="dim"
      style="font-size:12px;margin-top:6px">${src}</div></div>`;
    return;
  }
  $('#rspv').innerHTML=`
    <div class="dim" style="font-size:12px;margin-top:12px">${src}</div>
    <table style="margin-top:10px"><thead><tr><th>Setting</th><th>Now</th><th>After restore</th></tr></thead>
    <tbody>${(d.changes||[]).map(c=>`<tr><td>${esc(c.field)}</td>
      <td class="dim">${esc(c.before)}</td><td><b>${esc(c.after)}</b></td></tr>`).join('')}</tbody></table>
    <div class="dim" style="font-size:12px;margin-top:10px">${d.change_count} setting(s) would
    change.${d.reboot_required?' The bridge <b>restarts</b> afterwards — some of these only take effect at boot.':''}
    ${d.rollback_available?' The configuration it has now is kept, so this can be undone.'
      :' <b>No undo will be stored</b> — the flash had no room for a second copy.'}</div>
    <button type="button" onclick="applyRestore()">Apply these ${d.change_count} change(s)</button>`;
}

async function applyRestore(){
  if(rsPending===null){backupNote('#rsm','err','Preview the file again before applying.');return}
  $('#rspv').innerHTML='';
  backupNote('#rsm','','Applying…');
  const r=await authFetch('/api/v1/config/restore',
    {method:'POST',headers:{'Content-Type':'application/json'},body:rsPending});
  const d=await r.json().catch(()=>({}));
  if(!r.ok){backupNote('#rsm','err',(d.error&&d.error.message)||httpWhy(r));return}
  rsPending=null;
  backupNote('#rsm','ok','Restored '+(d.changed_fields||0)+' setting(s).'+
    (d.reboot_required?' The bridge is restarting — reload this page in ~30 seconds.'
                      :' No restart needed.')+
    (d.rollback_stored?' You can undo this below.':' No undo was stored — the flash was full.'));
}

async function undoRestore(){
  if(!confirm('Go back to the configuration this bridge had before the last restore?'))return;
  backupNote('#rbm','','Rolling back…');
  const r=await authFetch('/api/v1/actions/undo-restore',{method:'POST'});
  const d=await r.json().catch(()=>({}));
  if(!r.ok){backupNote('#rbm','err',(d.error&&d.error.message)||httpWhy(r));return}
  backupNote('#rbm','ok','Rolled back.'+(d.rebooting?' The bridge is restarting — reload in ~30 seconds.'
    :' No restart needed.')+' Pressing this again returns to the restored configuration.');
}

async function otaUpload(){
  const f=$('#c_fw').files[0], m=$('#om'), pb=$('#opb'), pf=$('#opf'), btn=$('#ob');
  m.style.display='block';
  if(!f){m.className='msg err';m.textContent='Choose a firmware .bin first.';return}
  if(!sessionStorage.getItem('sb_auth')&&!await askAuth()){m.className='msg err';m.textContent='Cancelled.';return}
  const done=(cls,text)=>{pb.style.display='none';btn.disabled=false;m.className=cls;m.textContent=text};
  const send=(mayRetry)=>{
    const x=new XMLHttpRequest();
    x.open('POST','/api/v1/ota');
    x.setRequestHeader('Authorization','Basic '+sessionStorage.getItem('sb_auth'));
    x.upload.onprogress=e=>{
      if(!e.lengthComputable)return;
      const p=Math.round(e.loaded/e.total*100);
      pf.style.width=p+'%';
      m.textContent=p<100?'Uploading… '+p+'% ('+Math.round(e.loaded/1024)+' of '+Math.round(e.total/1024)+' kB)'
                         :'Upload complete — verifying and writing flash…';
    };
    x.onload=()=>{
      if(x.status===401&&mayRetry){
        clearAuth();
        askAuth().then(ok=>{if(ok)send(false);else done('msg err','Cancelled.')});
        return;
      }
      let d={};try{d=JSON.parse(x.responseText)}catch(e){}
      if(x.status<200||x.status>=300){
        done('msg err','Update refused: '+((d.error&&d.error.message)||('HTTP '+x.status))+
             ' — the running firmware is untouched.');
        return;
      }
      done('msg ok','Verified and installed. Rebooting into the new firmware — reload this page in ~15 seconds.');
    };
    x.onerror=()=>done('msg err','Upload failed: network error — the running firmware is untouched.');
    m.className='msg';m.textContent='Uploading… 0%';
    pb.style.display='block';pf.style.width='0%';btn.disabled=true;
    // FormData: the browser sets the multipart boundary itself; setting Content-Type manually
    // here would break the upload.
    const fd=new FormData();fd.append('firmware',f,f.name);
    x.send(fd);
  };
  send(true);
}

async function rebootFromSettings(){
  // display:block explicitly: #cm starts hidden, and outside the save flow (the standalone
  // Restart card) nothing else has unhidden it yet.
  const m=$('#cm');m.style.display='block';
  const r=await authFetch('/api/v1/actions/reboot',{method:'POST'});
  if(!r.ok){m.className='msg err';m.textContent='Restart refused: '+httpWhy(r);return}
  m.className='msg ok';
  m.textContent='Restarting. This page will go blank for a few seconds — reload it after.';
}

async function factoryReset(){
  if(!confirm('Erase all settings including WiFi and passwords?'))return;
  const r=await authFetch('/api/v1/actions/factory-reset',{method:'POST'});
  alert(r.ok?'Erased. The bridge is restarting into setup mode.':'Failed: '+httpWhy(r));
}

async function refresh(){
  try{
    const r=await fetch('/api/v1/status');
    if(!r.ok)throw new Error('HTTP '+r.status);
    const s=await r.json();
    // The device page fetches its own capabilities, per device and cached there; this global
    // one served the single-device table that renderDevices replaced. Its OK-response guard
    // moved with it, into getJson().
    render(s);
    if(tab==='diag')await loadDiag();
    if(tab==='logs')await loadLogs();
    if(tab==='disc'&&!$('#wiz').innerHTML)renderWizard();
    if(tab==='cfg'&&!$('#cfgform').innerHTML)await renderConfig();
    // Cleared first, then possibly re-raised: the reachability banner owns this element, and
    // a device warning must not survive a later refresh that finds everything fine.
    $('#banner').classList.add('hide');
    // Every tab, not only the device page. A restart lands on the Dashboard, and "one of your
    // three inverters did not start" is not something to find by clicking around.
    deviceBanner(s.bridge);
  }catch(e){
    $('#banner').textContent='Cannot reach the bridge: '+e.message;
    $('#banner').classList.remove('hide');
  }
}

// SSE is an optimisation. If it drops, the interval below keeps the page live -- the UI must
// not depend on it.
let es;
function connect(){
  try{
    es=new EventSource('/api/v1/events');
    es.addEventListener('state',()=>refresh());
    es.onerror=()=>{es.close();setTimeout(connect,10000)};
  }catch(e){}
}
refresh();connect();setInterval(refresh,5000);
</script></body></html>)HTML";

}  // namespace heliograph::web
