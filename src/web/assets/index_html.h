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
<section id="dash"><div class="grid" id="tiles"></div></section>
<section id="dev" class="hide"><table id="devtbl"></table></section>
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
<b>Admin password</b>
<div class="dim" style="font-size:13px;margin-top:4px">Signing in as <span id="authu">admin</span></div>
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
// plain text on screen. And the device has exactly one admin account, whose name is in the
// config (readable without auth precisely so the UI can render) -- so only the password is
// asked; typing a username nobody chose was noise. Fetched fresh on every prompt, so a
// just-renamed admin user is picked up without a reload.
async function askAuth(){
  let u='admin';
  try{
    const r=await fetch('/api/v1/config');
    if(r.ok){const j=await r.json();u=(j.security&&j.security.admin_username)||'admin'}
  }catch(e){}
  $('#authu').textContent=u;
  return new Promise(resolve=>{
    const d=$('#authdlg'),p=$('#authpw');
    p.value='';d.returnValue='';
    d.onclose=()=>{
      const ok=d.returnValue==='ok'&&p.value!=='';
      if(ok)sessionStorage.setItem('sb_auth',btoa(u+':'+p.value));
      p.value='';
      resolve(ok);
    };
    d.showModal();
  });
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
  if(!sessionStorage.getItem('sb_auth')&&!await askAuth())return authCancelled();
  let r=await fetch(url,{...opts,headers:{...(opts.headers||{}),...authHeader()}});
  if(r.status===401){
    clearAuth();
    if(!await askAuth())return authCancelled();
    r=await fetch(url,{...opts,headers:{...(opts.headers||{}),...authHeader()}});
  }
  return r;
}

function tile(k,v,u,extra=''){return `<div class="card"><div class="k">${esc(k)}</div>
<div class="v">${esc(v)}<span class="u"> ${esc(u)}</span></div>${extra}</div>`}

let caps=null;
function render(s){
  const d=s.device,b=s.bridge,m=s.measurements||{};
  const dot=x=>x?'<span class="dot ok"></span>':'<span class="dot bad"></span>';
  $('#hdr').innerHTML=dot(b.wifi_connected)+'WiFi '+
    dot(b.mqtt_connected)+'MQTT '+
    dot(b.modbus_listening)+'Modbus '+
    dot(d.online)+'Inverter'+
    (d.data_stale?' <span class="tag">stale</span>':'')+
    (d.data_valid?'':' <span class="tag">no data</span>');
  $('#ver').textContent='v'+b.firmware_version;
  // Boards without relays never send the field; the settings card keys off this.
  window.g_relayCount=(b.relays||[]).length;
  const g=id=>m[id]?m[id].value:null;
  if(tab==='dash'){
    $('#tiles').innerHTML=
      tile('AC Power',fmt(g('ac.power.total'),0),'W')+
      tile('Today',fmt(g('energy.today'),2),'kWh')+
      tile('Total',fmt(g('energy.total'),1),'kWh')+
      tile('Temperature',fmt(g('inverter.temperature')),'°C')+
      tile('AC Voltage',fmt(g('ac.phase_l1.voltage')),'V')+
      tile('Frequency',fmt(g('ac.frequency'),2),'Hz')+
      tile('Status',esc(s.status_text??'—'),'')+
      // null means this protocol has no error code field at all -- not "no fault".
      tile('Error code',s.error_code===null?'not reported':esc(s.error_code),'')+
      tile('Last poll',d.last_successful_poll_seconds_ago??'—','s ago')+
      tile('Uptime',up(b.uptime_seconds),'')+
      tile('WiFi',b.wifi_rssi_dbm??'—','dBm')+
      tile('Modbus clients',b.modbus_clients??0,'')+
      // Honest clock: before the first NTP sync there is no time to show. The extra
      // null-guard matters: the API can answer time_synced:true with time:null when
      // formatting failed server-side, and a throw here would kill the whole render loop
      // and put up the "Cannot reach the bridge" banner for a healthy bridge.
      tile('Clock',b.time_synced?(b.time?esc(b.time.split(' ')[1]??b.time):'—'):'not synced','');
      // No firmware tile: the version already sits in the header, permanently.
  }
  if(tab==='dev'){
    let r=`<tr><th>Field</th><th>Value</th></tr>`;
    const add=(k,v)=>r+=`<tr><td class="dim">${esc(k)}</td><td>${esc(v??'—')}</td></tr>`;
    add('Manufacturer',d.manufacturer);add('Model',d.model);add('Serial',d.serial_number);
    add('Driver',d.driver_id);add('Support level',d.support_level);
    add('Online',d.online);add('Data valid',d.data_valid);add('Data stale',d.data_stale);
    if(caps){
      r+=`<tr><th>Capability</th><th></th></tr>`;
      r+=`<tr><td class="dim">Read-only</td><td>${caps.read_only}</td></tr>`;
      r+=`<tr><td class="dim">Phases / MPPTs</td><td>${caps.phase_count} / ${caps.mppt_count}</td></tr>`;
      r+=`<tr><td class="dim">Battery</td><td>${caps.has_battery}</td></tr>`;
      r+=`<tr><td class="dim">Read</td><td>${(caps.read||[]).map(esc).join(', ')||'—'}</td></tr>`;
      // Empty for every driver in this build, and that is the point: the UI shows what the
      // device can do, it does not assume.
      r+=`<tr><td class="dim">Write</td><td>${(caps.write||[]).map(esc).join(', ')||'<span class="dim">none</span>'}</td></tr>`;
    }
    r+=`<tr><th>Measurement</th><th>Value</th></tr>`;
    for(const [k,v] of Object.entries(m)){
      r+=`<tr><td class="dim">${esc(k)}${v.derived?' <span class="tag">derived</span>':''}</td>
      <td class="n">${v.value===null?'<span class="dim">unknown</span>':esc(v.value)+' '+esc(v.unit)}
      ${v.stale?'<span class="tag">stale</span>':''}</td></tr>`;
    }
    $('#devtbl').innerHTML=r;
  }
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
  if(r.cancelled){
    logsAuthStop=true;
    $('#logbox').innerHTML='<span class="dim">Admin password required. '+
      '<a href="#" onclick="logsAuthStop=false;loadLogs();return false">Try again</a></span>';
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
let wizStep=1, wizPoll=null, wizChosen=null, wizReport=null;
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
    profile. Extended tries every profile and repeats — slower and noisier on the bus, so it is
    opt-in.</p>
    <p class="dim">Probing is read-only. It never writes a register, never starts or stops the
    inverter, and never changes an address permanently.</p>
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
    if(!c.length){h+='<p class="dim">Nothing answered. The quick scan only tries each driver’s default line speed — run the <b>extended scan</b> to try all of them. Also check the wiring: A/B swapped is the most common cause, then termination.</p>'}
    c.forEach(x=>{
      h+=`<div style="border:1px solid var(--line);border-radius:8px;padding:12px;margin-top:10px">
      <div style="display:flex;justify-content:space-between;align-items:baseline">
        <b>${esc(x.display_name)}</b><span class="tag">${x.confidence}/100</span></div>
      <table style="margin-top:8px">
      <tr><td class="dim">Driver</td><td>${esc(x.driver_id)} <span class="tag">${esc(x.support_level)}</span></td></tr>
      <tr><td class="dim">Serial profile tried</td><td>${x.serial_profile?`${x.serial_profile.baud_rate} ${x.serial_profile.data_bits}${esc(x.serial_profile.parity[0].toUpperCase())}${x.serial_profile.stop_bits}, timeout ${x.serial_profile.response_timeout_ms} ms`:'—'}</td></tr>
      <tr><td class="dim">Response found</td><td>${x.responded?'yes':'no'}</td></tr>
      <tr><td class="dim">Checksum valid</td><td>${x.checksum_valid?'yes':'no'}</td></tr>
      <tr><td class="dim">Repeat probe agreed</td><td>${x.consistent?'yes':'<b>no — score halved</b>'}</td></tr>
      <tr><td class="dim">Detected</td><td>${esc(x.detected_manufacturer||'—')} ${esc(x.detected_model||'')}</td></tr>
      <tr><td class="dim">Serial number</td><td>${esc(x.serial_number||'—')}</td></tr>
      <tr><td class="dim">Evidence</td><td>${(x.evidence||[]).map(e=>'· '+esc(e)).join('<br>')||'—'}</td></tr>
      </table>
      <button onclick="wizChosen='${esc(x.driver_id)}';wizStep=5;renderWizard()">Choose this driver</button>
      </div>`;
    });
    h+=`<button onclick="wizStep=2;renderWizard()" style="background:none;border:1px solid var(--line);color:var(--fg)">Back</button></div>`;
  } else if(wizStep===5){
    h+=`<div class="card"><b>Step 5 — Confirm</b>
    <p class="dim">Nothing is saved until step 7. An uncertain match is never selected for you
    — that is deliberate: reading the wrong register map produces believable numbers.</p>
    <label for="wd">Driver</label><select id="wd"></select>
    <button onclick="wizChosen=$('#wd').value;wizStep=6;renderWizard();testPoll()">Confirm and test</button>
    <button onclick="wizStep=4;renderWizard()" style="background:none;border:1px solid var(--line);color:var(--fg)">Back</button></div>`;
  } else if(wizStep===6){
    h+=`<div class="card"><b>Step 6 — Test poll</b><div id="tp" class="dim">Polling…</div></div>`;
  } else if(wizStep===7){
    h+=`<div class="card"><b>Step 7 — Saved</b>
    <p class="dim">The driver is stored. It takes effect after a restart.</p>
    <button onclick="wizReboot()">Restart now</button>
    <div id="wizmsg" class="msg" style="display:none"></div></div>`;
  }
  $('#wiz').innerHTML=h;

  if(wizStep===5){
    fetch('/api/v1/drivers').then(r=>r.json()).then(d=>{
      const sel=$('#wd');sel.innerHTML='';
      (d.drivers||[]).forEach(x=>{
        const o=document.createElement('option');
        o.value=x.id;o.textContent=`${x.display_name} (${x.support_level})`;
        if(x.id===wizChosen)o.selected=true;
        sel.appendChild(o);
      });
    });
  }
}

async function startDiscovery(extended){
  wizStep=3;wizReport=null;renderWizard();
  const r=await authFetch('/api/v1/actions/discover'+(extended?'?extended=true':''),{method:'POST'});
  if(r.cancelled){wizStep=2;renderWizard();return}
  if(r.status===401){wizStep=2;renderWizard();alert('Admin password required.');return}
  if(!r.ok&&r.status!==202){wizStep=2;renderWizard();alert('Could not start discovery.');return}
  clearInterval(wizPoll);
  wizPoll=setInterval(async()=>{
    wizReport=await (await fetch('/api/v1/discovery')).json();
    if(wizReport.busy){renderWizard();return}
    clearInterval(wizPoll);
    if(wizReport.auto_selected)wizChosen=wizReport.selected_driver_id;
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

async function saveDriver(){
  // wizChosen is captured when leaving step 5 -- the #wd select no longer exists here (step 6
  // re-rendered the wizard). Before that capture existed, the manual path saved
  // {driver:{id:null}}, which the backend treats as "keep": step 7 then claimed "Saved" while
  // nothing had been saved at all.
  const id=wizChosen;
  if(!id){alert('No driver selected.');wizStep=5;renderWizard();return}
  const r=await authFetch('/api/v1/config',{method:'PATCH',
    headers:{'Content-Type':'application/json'},body:JSON.stringify({driver:{id}})});
  if(!r.ok){alert('Save failed: '+httpWhy(r));return}
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
      window.g_relayCount=(s.bridge.relays||[]).length}
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
    // The line settings are a property of the protocol, not a user choice: the driver
    // configures the UART itself when it starts. An editable field here used to exist and
    // did nothing at all -- shown read-only instead, which is always true.
    const serial=(drv.serial_profiles||[]).map(p=>
      `${p.baud_rate} ${p.data_bits}${p.parity[0].toUpperCase()}${p.stop_bits}`).join(', ');
    return `<div class="dim" style="font-size:12px;margin-top:4px">${esc(drv.description||'')}</div>`+
      (serial?`<div class="dim" style="font-size:12px;margin-top:4px">Serial: ${serial} — set by the driver${drv.serial_profiles.length>1?' (tried in order during discovery)':''}.</div>`:'')+
      (drv.options||[]).map(o=>{
      const stored=id===c.driver.id?(c.driver.options||{})[o.key]:undefined;
      const cur=stored??o.default_value;
      if(o.allowed_values&&o.allowed_values.length){
        return `<label for="opt_${o.key}">${esc(o.display_name)}</label>
          <select id="opt_${o.key}" data-opt="${esc(o.key)}">${o.allowed_values.map(v=>
            `<option ${v===cur?'selected':''}>${esc(v)}</option>`).join('')}</select>
          <div class="dim" style="font-size:12px">${esc(o.description||'')}</div>`;
      }
      return `<label for="opt_${o.key}">${esc(o.display_name)}</label>
        <input id="opt_${o.key}" data-opt="${esc(o.key)}" value="${esc(cur)}">`;
    }).join('');
  };
  window.reloadDriverOpts=()=>{$('#drvopts').innerHTML=optsFor($('#c_drv').value)};
  const driverOpts=`<span id="drvopts">${optsFor(c.driver.id)}</span>`;

  $('#cfgform').innerHTML=`
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
  <div class="card"><b>MQTT</b> <span class="tag" style="font-weight:400">needs restart</span>${chk('c_mqe','Enabled',c.mqtt.enabled)}
    ${txt('c_mqh','Broker host',c.mqtt.host)}${num('c_mqp','Port',c.mqtt.port)}
    ${txt('c_mqu','Username',c.mqtt.username)}${pw('c_mqpw','Password',c.mqtt.password_set)}
    ${txt('c_mqt','Base topic',c.mqtt.base_topic)}
    ${chk('c_mqd','Home Assistant discovery',c.mqtt.discovery_enabled)}</div>
  <div class="card"><b>Modbus TCP</b> <span class="tag" style="font-weight:400">needs restart</span>${chk('c_mbe','Enabled',c.modbus.enabled)}
    ${num('c_mbp','Port',c.modbus.port)}${num('c_mbu','Unit ID',c.modbus.unit_id)}
    <div class="dim" style="font-size:12px;margin-top:8px">Writing is permanently disabled:
    no driver in this build can write to an inverter.</div></div>
  <div class="card"><b>Polling</b> <span class="tag" style="font-weight:400">needs restart</span>${num('c_pi','Interval (seconds)',c.polling.interval_seconds)}</div>
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
  <div class="card"><b>Driver</b> <span class="tag" style="font-weight:400">needs restart</span>
    <label for="c_drv">Active driver</label>
    <select id="c_drv" onchange="reloadDriverOpts()">${(cfgDrivers.drivers||[]).map(d=>
      `<option value="${esc(d.id)}" ${d.id===c.driver.id?'selected':''}>${esc(d.display_name)} (${esc(d.support_level)})</option>`).join('')}</select>
    ${driverOpts}
  </div>
  ${window.g_relayCount>0?`<div class="card"><b>Relays</b> <span class="tag" style="font-weight:400">applied immediately</span>
    ${chk('c_rle','Enabled',(c.relays||{}).enabled)}
    ${Array.from({length:window.g_relayCount},(_, i)=>`
      <label for="c_rlr${i}">Relay ${i+1} role</label>
      <select id="c_rlr${i}" data-role="${i}">${['none','drm0','drm1','drm2','drm3','drm4','drm5','drm6','drm7','drm8'].map(r=>
        `<option ${r===(((c.relays||{}).roles||[])[i]||'none')?'selected':''}>${r}</option>`).join('')}</select>`).join('')}
    <div class="dim" style="font-size:12px;margin-top:8px">DRM curtailment contacts. Two
    locks must open before a relay can move: this switch AND read-only mode being off.
    Disabling releases every relay. Roles name the switches in Home Assistant and build
    the DRM Mode select; see docs/drm.md for the wiring rules (failsafe: a dead bridge
    must leave the inverter running).</div></div>`:''}
  <div class="card"><b>Security</b> <span class="tag" style="font-weight:400">applied immediately</span>${txt('c_au','Admin username',c.security.admin_username)}
    ${pw('c_ap','Admin password',c.security.password_set)}</div>
  <div class="card"><b>Logging</b> <span class="tag" style="font-weight:400">applied immediately</span>
    <label for="c_lg">Level</label><select id="c_lg">${['error','warn','info','debug','trace'].map(l=>
      `<option ${l===c.logging.level?'selected':''}>${l}</option>`).join('')}</select></div>
  <button onclick="saveConfig()">Save settings</button>
  <div id="cm" class="msg" style="display:none"></div>
  <div class="card" style="margin-top:20px"><b>Firmware update (OTA)</b>
    <label for="c_fw">Firmware image (.bin)</label>
    <input id="c_fw" type="file" accept=".bin">
    <button id="ob" onclick="otaUpload()">Upload and install</button>
    <div id="opb" style="display:none;height:8px;max-width:420px;margin-top:12px;border:1px solid var(--line);border-radius:99px;overflow:hidden">
      <div id="opf" style="height:100%;width:0%;background:#2f81f7;transition:width .2s"></div></div>
    <div id="om" class="msg" style="display:none"></div>
    <div class="dim" style="font-size:12px;margin-top:8px">The image is verified before the
    boot partition switches; a rejected upload leaves the running firmware untouched.</div></div>
  <div class="card" style="margin-top:20px">
    <b>Restart</b>
    <p class="dim">Reboots the bridge; all settings are kept. Polling resumes by itself and
    the dashboard reconnects in ~30 seconds.</p>
    <button onclick="rebootFromSettings()">Restart bridge</button>
  </div>
  <div class="card" style="margin-top:20px;border-color:var(--bad)">
    <b>Factory reset</b>
    <p class="dim">Erases everything, including WiFi and passwords, and restarts into the setup
    portal. The board's physical RESET button only reboots — this page is the way back from a
    bad configuration, as long as you can still reach it.</p>
    <button style="background:var(--bad)" onclick="factoryReset()">Erase and restart</button>
  </div>`;
}

async function saveConfig(){
  const v=id=>$(id).value, n=id=>Number($(id).value), b=id=>$(id).checked;
  const body={bridge_name:v('c_name'),
    ...(window.g_relayCount>0?{relays:{enabled:b('c_rle'),
      roles:Array.from({length:window.g_relayCount},(_,i)=>v('c_rlr'+i))}}:{}),
    wifi:{ssid:v('c_ssid'),hostname:v('c_host')},
    mqtt:{enabled:b('c_mqe'),host:v('c_mqh'),port:n('c_mqp'),username:v('c_mqu'),
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
    security:{admin_username:v('c_au')},
    logging:{level:v('c_lg')}};
  // A blank password field means "keep": sending "" would clear it, which is never what an
  // untouched field means.
  if(v('c_wpw'))body.wifi.password=v('c_wpw');
  if(v('c_mqpw'))body.mqtt.password=v('c_mqpw');
  if(v('c_ap'))body.security.admin_password=v('c_ap');
  document.querySelectorAll('[data-opt]').forEach(e=>body.driver.options[e.dataset.opt]=e.value);

  // Send only what actually changed. This form may have been open for hours; PATCHing every
  // field would write its stale copy over anything changed elsewhere in the meantime (another
  // tab, the discovery wizard) -- which is exactly how a driver choice once quietly reverted.
  // Passwords are exempt: they are only in the body when typed, and GET never returns them.
  const same=(a,b)=>JSON.stringify(a)===JSON.stringify(b);
  if(same(body.bridge_name,cfgBefore.bridge_name))delete body.bridge_name;
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
    for(const k of Object.keys(body[sect])){
      if(k.includes('password')||k==='options')continue;
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

  m.className='msg ok';
  if(!pending.length){
    m.innerHTML='Saved and applied.';
  }else{
    m.innerHTML='Saved. These take effect after a restart: <b>'+
      pending.map(esc).join(', ')+'</b>.<br>'+
      '<button style="margin-top:10px" onclick="rebootFromSettings()">Restart now</button>'+
      '<span class="dim" style="font-size:12px"> — or later; the bridge keeps running on the '+
      'old values until you do.</span>';
  }
  m.style.display='block';
}

// XMLHttpRequest, not fetch: fetch cannot report UPLOAD progress, and a minute of silent
// "uploading…" reads as a hang. Auth mirrors authFetch (prompt once, retry once on 401).
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
    // Capabilities change only when the driver does, so fetch them once rather than every
    // refresh -- the device page needs them, the dashboard does not.
    if(tab==='dev'&&!caps&&s.device.id){
      // Only keep an OK response: storing an error body here handed render() an object
      // without read/write arrays and took the whole page down with a misleading banner.
      try{
        const cr=await fetch('/api/v1/devices/'+encodeURIComponent(s.device.id)+'/capabilities');
        if(cr.ok)caps=await cr.json();
      }catch(e){}
    }
    render(s);
    if(tab==='diag')await loadDiag();
    if(tab==='logs')await loadLogs();
    if(tab==='disc'&&!$('#wiz').innerHTML)renderWizard();
    if(tab==='cfg'&&!$('#cfgform').innerHTML)await renderConfig();
    $('#banner').classList.add('hide');
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
