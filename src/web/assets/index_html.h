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
:root{--bg:#0f1115;--card:#181b22;--fg:#e6e8ec;--dim:#8b93a3;--mid:#c9ced8;--ok:#3fb950;--bad:#f85149;--warn:#d29922;--acc:#2f81f7;--line:#262b36;--hair:#1d222b}
@media(prefers-color-scheme:light){:root{--bg:#f6f7f9;--card:#fff;--fg:#1a1d23;--dim:#5b6472;--mid:#3a4149;--line:#e3e6ea;--hair:#eef0f3}}
*{box-sizing:border-box}
body{margin:0;font:15px/1.5 system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--fg)}
a{color:var(--acc)}a:hover{color:#5aa0f9}
code{font:12px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace}
/* --- chrome ------------------------------------------------------------------------ */
header{padding:14px 20px;border-bottom:1px solid var(--line);display:flex;gap:14px;align-items:center;flex-wrap:wrap}
header b{font-size:16px}
nav{display:flex;gap:4px;padding:8px 20px;border-bottom:1px solid var(--line);flex-wrap:wrap}
nav button{background:none;border:0;color:var(--dim);padding:6px 12px;margin:0;border-radius:6px;cursor:pointer;font:inherit}
nav button.on{background:var(--card);color:var(--fg)}
.chips{display:flex;gap:10px;padding:10px 20px;border-bottom:1px solid var(--line);flex-wrap:wrap;align-items:center}
.chip{display:inline-flex;align-items:center;gap:7px;font-size:13px;color:var(--dim)}
main{padding:20px;max-width:960px}
/* --- primitives -------------------------------------------------------------------- */
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px}
.card+.card{margin-top:12px}
.card.bad{border-color:var(--bad)}.card.warn{border-color:var(--warn)}
.stack{display:flex;flex-direction:column;gap:12px}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;flex:0 0 auto}
.dot.ok{background:var(--ok)}.dot.bad{background:var(--bad)}.dot.warn{background:var(--warn)}.dot.off{background:var(--dim)}
.tag{display:inline-block;font-size:11px;padding:2px 7px;border-radius:99px;border:1px solid var(--line);color:var(--dim);white-space:nowrap}
.tag.warn{border-color:var(--warn);color:var(--warn)}.tag.bad{border-color:var(--bad);color:var(--bad)}
.dim{color:var(--dim)}.hide{display:none}.hair{border-top:1px solid var(--line);margin-top:14px;padding-top:14px}
.k{color:var(--dim);font-size:12px;text-transform:uppercase;letter-spacing:.04em}
.v{font-size:22px;font-weight:600;margin-top:4px;font-variant-numeric:tabular-nums}
.u{font-size:13px;color:var(--dim);font-weight:400}
.hint{font-size:12px;color:var(--dim);margin-top:4px}
.between{display:flex;justify-content:space-between;align-items:baseline;gap:12px;flex-wrap:wrap}
.row{display:flex;justify-content:space-between;gap:14px;padding:3px 0}
.row span:last-child{font-variant-numeric:tabular-nums}
.row.rule{border-bottom:1px solid var(--hair)}
.tiles{display:grid;gap:12px;grid-template-columns:repeat(auto-fill,minmax(200px,1fr))}
.cols{display:grid;gap:14px;grid-template-columns:repeat(auto-fill,minmax(200px,1fr))}
.cols3{display:grid;gap:12px;grid-template-columns:repeat(auto-fill,minmax(240px,1fr))}
.groups{display:grid;gap:18px;grid-template-columns:repeat(auto-fill,minmax(240px,1fr))}
.fact .k{font-size:11px}.fact div+div{font-size:14px;margin-top:2px;overflow-wrap:anywhere}
/* --- buttons and fields ------------------------------------------------------------ */
button{padding:9px 14px;border:0;border-radius:8px;background:var(--acc);color:#fff;font:inherit;font-weight:600;cursor:pointer;margin:0}
button:disabled{opacity:.5;cursor:default}
button.alt{background:none;border:1px solid var(--line);color:var(--fg)}
button.link{background:none;border:1px solid var(--acc);color:var(--acc)}
button.danger{background:var(--bad)}
button.dangerAlt{background:none;border:1px solid var(--bad);color:var(--bad)}
button.sm{padding:7px 12px;font-size:13px;font-weight:400}
button.pill{padding:4px 10px;font-size:12px;border-radius:99px;font-weight:400}
button.pill.off{background:none;border:1px solid var(--line);color:var(--dim)}
.acts{display:flex;gap:10px;flex-wrap:wrap;margin-top:14px}
label{display:block;font-size:13px;color:var(--dim);margin:12px 0 4px}
label.check{display:flex;gap:8px;align-items:center;color:var(--fg);font-size:14px;margin:12px 0 0}
input,select{width:100%;max-width:420px;padding:9px 10px;border-radius:8px;border:1px solid var(--line);background:var(--bg);color:var(--fg);font:inherit}
input[type=checkbox]{width:auto}
input.sh,select.sh{max-width:200px}
input:disabled,select:disabled{color:var(--dim)}
select.tiny{width:auto;padding:4px 8px;border-radius:6px;font-size:12px}
/* --- messages ---------------------------------------------------------------------- */
.msg{padding:10px;border-radius:8px;margin-top:12px}
.msg.err{background:#f8514922;border:1px solid var(--bad)}
.msg.ok{background:#3fb95022;border:1px solid var(--ok)}
.msg.warn{background:#d2992222;border:1px solid var(--warn)}
.banner{margin:16px 20px 0;max-width:960px;background:#f8514922;border:1px solid var(--bad);border-radius:10px;padding:12px 14px}
.note{display:flex;align-items:center;gap:10px;margin-top:12px;padding:10px 12px;border:1px solid var(--line);border-radius:8px;font-size:13px}
.note.ok{border-color:var(--ok)}
/* --- live -------------------------------------------------------------------------- */
.hero{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:24px;align-items:end}
.big{font-size:56px;font-weight:600;line-height:1.05;font-variant-numeric:tabular-nums}
.bar{margin-top:10px;height:6px;background:var(--card);border:1px solid var(--line);border-radius:99px;overflow:hidden}
svg{display:block}
.totals{display:flex;gap:24px;margin-top:8px;flex-wrap:wrap}
.fleetrow{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px;display:flex;flex-wrap:wrap;gap:16px;align-items:center}
.fleetrow.bad{border-color:var(--bad)}
.fleetrow .name{flex:1 1 200px;min-width:0}
.fleetrow .mini{flex:0 1 150px;min-width:90px}
.fleetrow .num{flex:0 1 90px;text-align:right}
.fleetrow .num b{font-size:20px;font-weight:600;font-variant-numeric:tabular-nums;display:block}
.fleetrow .when{flex:0 1 140px;text-align:right;font-size:13px}
.soc{display:flex;align-items:center;gap:16px;margin-top:10px;flex-wrap:wrap}
.soc .pct{font-size:34px;font-weight:600;font-variant-numeric:tabular-nums}
.soc .track{flex:1 1 200px;min-width:160px}
.soc .track>.bar2{height:10px;background:var(--bg);border:1px solid var(--line);border-radius:99px;overflow:hidden}
/* --- log --------------------------------------------------------------------------- */
#logbox{background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:12px;
  font:12px/1.7 ui-monospace,SFMono-Regular,Menlo,monospace;white-space:pre-wrap;word-break:break-all;
  max-height:320px;overflow-y:auto;margin-top:10px}
#logbox .lw{color:var(--warn)}#logbox .le{color:var(--bad)}
table{width:100%;border-collapse:collapse;font-size:14px;margin-top:10px}
td,th{text-align:left;padding:7px 8px;border-bottom:1px solid var(--line);vertical-align:top}
th{color:var(--dim);font-weight:500;font-size:12px;text-transform:uppercase}
td.n{text-align:right;font-variant-numeric:tabular-nums}
tr:last-child td{border-bottom:0}
.prog{display:none;height:8px;max-width:420px;margin-top:12px;border:1px solid var(--line);border-radius:99px;overflow:hidden}
.prog>div{height:100%;width:0%;background:var(--acc);transition:width .2s}
dialog{background:var(--card);color:var(--fg);border:1px solid var(--line);border-radius:12px;padding:20px;width:90%;max-width:340px}
dialog::backdrop{background:#000a}
</style></head><body>

<header><b>Heliograph</b>
<span id="host" class="dim" style="font-size:12px"></span>
<span style="flex:1"></span>
<button id="pending" class="tag warn hide" style="cursor:pointer;background:none" onclick="goTab('bridge')"></button>
<a id="updbadge" class="tag hide" href="#" onclick="gotoUpdate();return false" style="border-color:var(--acc);color:var(--acc);text-decoration:none">update available</a>
<span id="ver" class="dim" style="font-size:12px"></span></header>

<nav>
<button data-t="live" class="on">Live</button>
<button data-t="inv">Inverters</button>
<button data-t="int">Integrations</button>
<button data-t="health">Health</button>
<button data-t="bridge">Bridge</button>
</nav>

<div class="chips" id="chips"></div>
<div id="banner" class="banner hide"></div>

<main>
<section id="live"></section>
<section id="inv" class="hide"></section>
<section id="int" class="hide"></section>
<section id="health" class="hide"></section>
<section id="bridge" class="hide"></section>
</main>

<dialog id="authdlg">
<form method="dialog">
<b>Admin sign-in</b>
<div id="autherr" class="msg err hide">Not accepted. Check the username too — it is only
<b>admin</b> if you never changed it.</div>
<label for="authu">Username</label>
<input id="authu" autocomplete="username" autocapitalize="none" autocorrect="off"
       spellcheck="false" required value="admin">
<label for="authpw">Password</label>
<input id="authpw" type="password" autocomplete="current-password" required autofocus>
<div class="acts">
<!-- type=button: implicit submission picks the FIRST submit button, so a submit-type Cancel
     made Enter in the password field mean "cancel". -->
<button type="button" class="alt" onclick="this.closest('dialog').close('cancel')">Cancel</button>
<button value="ok">Unlock</button>
</div>
</form>
</dialog>

<script>
'use strict';
const $=s=>document.querySelector(s[0]==='#'||s[0]==='.'?s:'#'+s);
const esc=s=>String(s??'').replace(/[<>&"']/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;',"'":'&#39;'}[c]));
// null must never render as 0 -- that is why the firmware sends null in the first place.
const fmt=(v,d=1)=>v===null||v===undefined?'—':Number(v).toFixed(d);
const sp=n=>String(n).replace(/\B(?=(\d{3})+(?!\d))/g,'\u2009');
const up=s=>s<3600?Math.floor(s/60)+' m':s<86400?(s/3600).toFixed(1)+' h':Math.floor(s/86400)+' d '+Math.round(s%86400/3600)+' h';
const kb=b=>b===null||b===undefined?'—':Math.round(b/1024)+' kB';

let tab='live', panel=null, logFilter='all', logPaused=false;
let S=null;              // last /api/v1/status
let cfg=null, drivers=null;
const devCache={}, measCache={};
let xdevs=null;          // extra-device rows being edited, or null when not loaded

const TABS=[...document.querySelectorAll('nav button')].map(b=>b.dataset.t);
document.querySelectorAll('nav button').forEach(b=>b.onclick=()=>goTab(b.dataset.t));
function goTab(name){
  tab=name;panel=null;
  document.querySelectorAll('nav button').forEach(x=>x.classList.toggle('on',x.dataset.t===name));
  TABS.forEach(t=>$('#'+t).classList.toggle('hide',t!==name));
  paint();refresh();
}
/// Opens/closes an in-card panel. One at a time: two open forms on one screen is how a value
/// gets typed into the wrong section.
function togglePanel(id){panel=panel===id?null:id;devDraft=null;paint()}

// ---------------- admin auth ----------------
// fetch() never raises the browser's Basic-auth dialog: a 401 is just a 401. So ask once, keep
// it for the tab only, and send the header ourselves.
function authHeader(){const c=sessionStorage.getItem('hg_auth');return c?{'Authorization':'Basic '+c}:{}}
function clearAuth(){sessionStorage.removeItem('hg_auth')}
let authPrompt=null;
async function askAuth(retry){
  if(authPrompt){if(retry)$('#autherr').classList.remove('hide');return authPrompt}
  const remembered=sessionStorage.getItem('hg_user');
  authPrompt=new Promise(resolve=>{
    const d=$('#authdlg'),p=$('#authpw'),un=$('#authu'),err=$('#autherr');
    un.value=remembered||'admin';
    err.classList.toggle('hide',!retry);
    p.value='';d.returnValue='';
    d.onclose=()=>{
      const u=un.value.trim()||'admin';
      const ok=d.returnValue==='ok'&&p.value!=='';
      // UTF-8, not btoa()'s Latin-1: the firmware compares the UTF-8 bytes it stored, so a
      // password with é/ë/ü could otherwise never authenticate.
      if(ok){
        sessionStorage.setItem('hg_auth',
          btoa(String.fromCharCode(...new TextEncoder().encode(u+':'+p.value))));
        sessionStorage.setItem('hg_pending',u);
      }
      p.value='';resolve(ok);
    };
    d.showModal();
  });
  try{return await authPrompt}finally{authPrompt=null}
}
// Only promote a username a request has actually proven, so the next prompt offers the last
// name the device accepted rather than the last one typed.
function rememberUser(){const u=sessionStorage.getItem('hg_pending');if(u)sessionStorage.setItem('hg_user',u)}
const authCancelled=()=>({ok:false,status:0,cancelled:true,
  json:async()=>({error:{code:'cancelled',message:'Admin password required.'}}),text:async()=>'',
  headers:{get:()=>null}});
const httpWhy=r=>r.cancelled?'cancelled (admin password required)':'HTTP '+r.status;
async function authFetch(url,opts={}){
  if(!sessionStorage.getItem('hg_auth')&&!await askAuth(false))return authCancelled();
  const used=sessionStorage.getItem('hg_auth');
  let r=await fetch(url,{...opts,headers:{...(opts.headers||{}),...authHeader()}});
  if(r.status===401){
    // Only discard what THIS request used: two tabs can be in flight with the same bad
    // credentials, and the second 401 must not wipe the ones just accepted.
    if(sessionStorage.getItem('hg_auth')===used){
      clearAuth();
      if(!await askAuth(true))return authCancelled();
    }
    r=await fetch(url,{...opts,headers:{...(opts.headers||{}),...authHeader()}});
  }
  if(r.status!==401)rememberUser();
  return r;
}
async function getJson(url){
  const r=await fetch(url);
  // fetch does not reject on 5xx, and the error body is valid JSON -- rendering it would
  // fabricate a device with undefined everything, or a measurement count of 0, which is the
  // wrong-register-map tell.
  if(!r.ok)throw new Error('HTTP '+r.status);
  return r.json();
}

// ---------------- session history for the sparkline ----------------
// The bridge stores no time series and gains no endpoint for this. Samples come from the
// /api/v1/status payload the page already fetches, and live in sessionStorage so a reload keeps
// the curve. Capped, so a tab left open for a week cannot grow without bound.
const HMAX=720;
let hist=(()=>{try{return JSON.parse(sessionStorage.getItem('hg_hist')||'{}')}catch(e){return{}}})();
let histSaveDue=0;
function record(key,watts){
  if(watts===null||watts===undefined)return;
  const a=hist[key]||(hist[key]=[]);
  a.push(Math.round(watts));
  if(a.length>HMAX)a.splice(0,a.length-HMAX);
}
function saveHist(){
  const now=Date.now();
  if(now<histSaveDue)return;
  histSaveDue=now+20000;
  try{sessionStorage.setItem('hg_hist',JSON.stringify(hist))}catch(e){}
}
/// An SVG polyline of one history series. Below two points it says so instead of drawing: a
/// single sample is a dot pretending to be a trend.
///
/// A LINE, never a filled area, and there is no option to make it one. An area implies
/// integrating from zero across a whole span, and this series starts whenever the page was
/// opened: filling it drew a vertical wall up at the left edge and a cliff down at the right,
/// neither of which is a reading. Keeping a `fill` option next to that paragraph would leave
/// the footgun loaded beside the warning.
///
/// The horizontal axis is SAMPLES, not time. Refreshes are event-driven while the bridge's
/// event stream is up, so the spacing follows the poll, and a gap in the readings shortens the
/// curve rather than flattening it.
function spark(key,w,h,opts={}){
  const a=hist[key]||[];
  if(a.length<2)return `<div class="hint" style="height:${h}px">collecting… the curve fills in as this page stays open</div>`;
  const max=Math.max(...a,1)*1.08, pad=2;
  const pts=a.map((y,i)=>((i/(a.length-1))*w).toFixed(1)+','+(h-pad-(y/max)*(h-pad)).toFixed(1));
  return `<svg class="spark" width="100%" height="${h}" viewBox="0 0 ${w} ${h}" preserveAspectRatio="none">
    <polyline points="${pts.join(' ')}" fill="none" stroke="${opts.stroke||'var(--acc)'}" stroke-width="${opts.weight||2}" stroke-linejoin="round"></polyline>
    ${opts.base?`<line x1="0" y1="${h-1}" x2="${w}" y2="${h-1}" stroke="var(--line)" stroke-width="1"></line>`:''}
  </svg>`;
}
function bar(pct,colour){
  return `<div class="bar"><svg width="100%" height="6" viewBox="0 0 100 6" preserveAspectRatio="none">
    <rect x="0" y="0" width="${Math.max(0,Math.min(100,pct)).toFixed(1)}" height="6" fill="${colour||'var(--acc)'}"></rect></svg></div>`;
}
/// Where a state of charge stops being comfortable. Named because they are a judgement, not a
/// measurement, and a reader should find them stated once rather than inferred from two
/// literals inside a ternary.
const SOC_LOW=20, SOC_HALF=50;
/// The charge bar's colour, which follows the LEVEL and nothing else.
///
/// It used to follow the DIRECTION: the same variable coloured the bar and the charging/
/// discharging line, so a full battery that happened to be charging drew a red bar. The two
/// answer different questions -- "how much is left" and "which way is it going" -- and one
/// colour could only ever answer one of them.
///
/// Unknown is not empty. A device can report battery power without a state of charge, and the
/// percentage beside this already renders as an em dash; painting the empty bar red would turn
/// "we were not told" into "critically low", which is the one thing the reading must never do.
function socColour(pct){
  if(pct===null||pct===undefined)return 'var(--dim)';
  return pct<SOC_LOW?'var(--bad)':pct<SOC_HALF?'var(--warn)':'var(--ok)';
}

// ---------------- canonical measurements ----------------
// Label, group and decimals per id. Order here IS the order on screen. Units are NOT here --
// they arrive with each reading, so only one place decides what a watt is called.
// A new id in measurement.h without a row here is caught by tools/check_measurement_ids.py.
const MEAS=[
  ['ac.power.total','AC power','H',0],
  ['energy.today','Today','H',2],
  ['energy.total','Total','H',1],
  ['battery.soc','State of charge','H',0],
  ['ac.phase_l1.voltage','Voltage L1','AC',1],
  ['ac.phase_l2.voltage','Voltage L2','AC',1],
  ['ac.phase_l3.voltage','Voltage L3','AC',1],
  ['ac.phase_l1.current','Current L1','AC',1],
  ['ac.phase_l2.current','Current L2','AC',1],
  ['ac.phase_l3.current','Current L3','AC',1],
  ['ac.phase_l1.power','Power L1','AC',0],
  ['ac.phase_l2.power','Power L2','AC',0],
  ['ac.phase_l3.power','Power L3','AC',0],
  ['ac.frequency','Frequency','AC',2],
  ['dc.mppt_1.voltage','MPPT 1 voltage','DC / MPPT',1],
  ['dc.mppt_1.current','MPPT 1 current','DC / MPPT',1],
  ['dc.mppt_1.power','MPPT 1 power','DC / MPPT',0],
  ['dc.mppt_2.voltage','MPPT 2 voltage','DC / MPPT',1],
  ['dc.mppt_2.current','MPPT 2 current','DC / MPPT',1],
  ['dc.mppt_2.power','MPPT 2 power','DC / MPPT',0],
  ['dc.power.total','DC power total','DC / MPPT',0],
  ['battery.power','Power','Battery',0],
  ['battery.charge_power','Charging','Battery',0],
  ['battery.discharge_power','Discharging','Battery',0],
  ['battery.voltage','Voltage','Battery',1],
  ['battery.current','Current','Battery',1],
  ['battery.temperature','Temperature','Battery',1],
  ['battery.energy_charged','Energy charged','Battery',2],
  ['battery.energy_discharged','Energy discharged','Battery',2],
  ['grid.import_power','Importing','Grid',0],
  ['grid.export_power','Exporting','Grid',0],
  ['inverter.temperature','Temperature','Device',1],
  ['inverter.operating_hours','Operating hours','Device',0],
];
const LABEL={};for(const [id,label,,dec] of MEAS)LABEL[id]={label,dec};
/// A channel is shown when the payload carries its KEY, not when it carries a value: a present
/// key means "this inverter has this", a null value means "not right now". Filtering on the
/// value makes rows appear and vanish as readings go stale.
function shown(m,id){return m&&m[id]!==undefined}
function reading(m,id){
  const r=m[id];if(!r)return '—';
  const d=(LABEL[id]||{}).dec??1;
  return r.value===null||r.value===undefined?'—':(fmt(r.value,d)+' '+(r.unit||'')).trim();
}
/// The detail groups for one device's measurement map. Empty groups are not rendered: a heading
/// over nothing is an empty card in a different shape.
function groupsFor(m,skipHeadline){
  const seen=new Set();let html='';
  for(const [,,group] of MEAS){
    if(seen.has(group))continue;
    if(group==='H'&&skipHeadline)continue;
    seen.add(group);
    const rows=MEAS.filter(([id,,g])=>g===group&&shown(m,id))
      .map(([id,label])=>`<div class="row rule"><span class="dim">${esc(label)}</span><span>${esc(reading(m,id))}</span></div>`);
    if(rows.length)html+=`<div><div class="k" style="margin-bottom:6px">${esc(group==='H'?'Headline':group)}</div>${rows.join('')}</div>`;
  }
  return html?`<div class="groups">${html}</div>`:'';
}

// ---------------- header, chips, banner ----------------
function paintChrome(){
  if(!S)return;
  const b=S.bridge, d=S.device, fleet=S.devices||[], tot=S.totals||{};
  const answering=tot.devices_answering??0;
  const expected=b.devices_configured??(tot.devices_polled??fleet.length);
  const multi=isFleet(S);
  // location.hostname when the payload does not carry one: this is a label, and the address the
  // page was reached at is never wrong for it.
  $('#host').textContent=b.hostname?b.hostname+'.local':location.hostname;
  $('#ver').textContent='v'+b.firmware_version;
  const chip=(cls,text)=>`<span class="chip"><span class="dot ${cls}"></span>${text}</span>`;
  // Green means ALL of them are answering; anything less is red. An amber "some" is a state you
  // learn to ignore. With one device this is the header it has always been, stale tag included.
  const inv=multi
    ?chip(answering===expected?'ok':'bad',`Inverters ${esc(answering)}/${esc(expected)} answering`)
    :chip(d.online?'ok':'bad','Inverter'+(d.data_stale?' <span class="tag">stale</span>':'')+
       (d.data_valid?'':' <span class="tag">no data</span>'));
  $('#chips').innerHTML=inv+
    chip(b.wifi_connected?'ok':'bad','WiFi '+(b.wifi_rssi_dbm!=null?b.wifi_rssi_dbm+' dBm':'not connected'))+
    chip(b.mqtt_connected?'ok':'off',b.mqtt_connected?'MQTT to '+esc((cfg&&cfg.mqtt&&cfg.mqtt.host)||'broker'):'MQTT off')+
    chip(b.modbus_listening?'ok':'off','Modbus TCP · '+(b.modbus_clients??0)+' clients');
  // One line that must be visible from any tab: a configured device is not running. Detail
  // stays on the Inverters tab; this only says look there.
  const problems=(b.device_problems||[]).length, started=b.devices_started;
  const el=$('#banner');
  if(b.devices_configured!==undefined&&started!==undefined&&(problems||started<b.devices_configured)){
    el.innerHTML=`<div><b>${esc(started)} of ${esc(b.devices_configured)} configured inverters started.</b></div>
      <div style="color:var(--mid);font-size:14px;margin-top:4px">The other inverters answer on this bus, so the bridge and the line speed are fine — which narrows it down.
      <a href="#" onclick="goTab('inv');return false">Three things to check →</a></div>`;
    el.classList.remove('hide');
  }else el.classList.add('hide');
}

// ---------------- Live ----------------
/// Whether this bridge is a fleet, decided in ONE place.
///
/// It was decided in two, and they disagreed. The sparkline chose its source by
/// `devices.length > 1` while the headline chose its layout by devices_configured -- so a
/// bridge with two inverters configured and one of them not yet answering drew a curve of a
/// single inverter's output under a heading that said "all inverters stacked". Invisible while
/// it lasts, because one answering device makes the two numbers equal. Then the second one
/// starts replying, the series switches source mid-curve, and the chart shows a step that never
/// happened -- in exactly the scenario the diagnosis card exists to get people out of
/// (review, 2026-07-28).
function isFleet(s){
  const tot=s.totals||{}, fleet=s.devices||[];
  const polled=tot.devices_polled??fleet.length;
  const expected=(s.bridge||{}).devices_configured??polled;
  return expected>1||polled>1;
}
function paintLive(){
  if(!S)return;
  const b=S.bridge, d=S.device, m=S.measurements||{}, fleet=S.devices||[], tot=S.totals||{};
  const expected=b.devices_configured??(tot.devices_polled??fleet.length);
  const multi=isFleet(S);
  let h='';
  if(multi){
    const answering=tot.devices_answering??0;
    const short=answering!==expected;
    h+=`<div class="hero">
      <div>
        <div class="k">Producing now · all inverters</div>
        <div style="display:flex;align-items:baseline;gap:8px;margin-top:2px">
          <span class="big">${esc(sp(Math.round(tot.ac_power_w??0)))}</span><span style="font-size:20px" class="dim">W</span></div>
        <div class="hint" style="margin-top:2px${short?';color:var(--bad)':''}">${
          short?`${esc(answering)} of ${esc(expected)} inverters answering — this total is short`
               :`${esc(expected)} of ${esc(expected)} inverters answering`}</div>
      </div>
      <div>
        <div class="between"><span class="k">Today, all inverters stacked</span><span class="hint" style="margin:0">held in this browser</span></div>
        ${spark('all',460,76,{base:true})}
        <div class="totals">
          <div><div class="v">${fmt(tot.energy_today_kwh,2)}<span class="u"> kWh</span></div><div class="hint">today${
            tot.energy_today_devices!==expected?', '+esc(tot.energy_today_devices??0)+' of '+esc(expected)+' reporting':''}</div></div>
          <div><div class="v">${esc(sp(Math.round(tot.energy_total_kwh??0)))}<span class="u"> kWh</span></div><div class="hint">lifetime</div></div>
        </div>
      </div></div>`;
  }else{
    const now=(m['ac.power.total']||{}).value;
    const rated=b.rated_power_w||null;   // absent on most drivers; the bar is then omitted
    h+=`<div class="hero">
      <div>
        <div class="k">${shown(m,'battery.soc')?'Solar production now':'Producing now'}</div>
        <div style="display:flex;align-items:baseline;gap:8px;margin-top:2px">
          <span class="big">${esc(now==null?'—':sp(Math.round(now)))}</span><span style="font-size:20px" class="dim">W</span></div>
        ${rated?bar((now||0)/rated*100)+`<div class="hint">${Math.round((now||0)/rated*100)}% of this inverter's ${esc(sp(rated))} W</div>`:''}
        <div class="hint">${d.online?'Last reply '+esc(d.last_successful_poll_seconds_ago??'—')+' s ago':'<span style="color:var(--bad)">not replying</span>'}${
          S.status_text?' · '+esc(S.status_text):''}</div>
      </div>
      <div>
        <div class="between"><span class="k">Today</span><span class="hint" style="margin:0">in this browser only</span></div>
        ${spark('all',460,72,{base:true})}
        <div class="totals">
          <div><div class="v">${reading(m,'energy.today')}</div><div class="hint">today</div></div>
          <div><div class="v">${reading(m,'energy.total')}</div><div class="hint">lifetime</div></div>
          ${shown(m,'inverter.temperature')?`<div><div class="v">${reading(m,'inverter.temperature')}</div><div class="hint">inverter</div></div>`:''}
        </div>
      </div></div>`;
  }
  // Battery, when any device reports one. Direction is carried by an ARROW as well as a colour,
  // so it survives a reader who cannot tell red from green, and the sign of the number never
  // has to be decoded.
  //
  // EITHER channel brings the card up, not both. No shipped profile maps power without a state
  // of charge -- checked, 2026-07-29 -- but profiles are added a TOML row at a time and a half
  // mapped one is the ordinary intermediate state, not an exotic case. A contributor who maps
  // the obvious power register before hunting down the SoC block then sees "— %" over a grey
  // bar, which says what is missing; requiring both would have shown them nothing at all.
  const batts=multi?fleet.filter(f=>f.battery_soc_pct!=null||f.battery_power_w!=null)
                   :(shown(m,'battery.soc')||shown(m,'battery.power')?[{single:true}]:[]);
  for(const f of batts){
    const soc=f.single?(m['battery.soc']||{}).value:f.battery_soc_pct;
    const p=f.single?(m['battery.power']||{}).value:f.battery_power_w;
    const idle=p==null||Math.abs(Number(fmt(Math.abs(p),0)))===0;
    const state=idle?'idle':(p>0?'charging':'discharging');
    // Direction, not level -- the bar below answers the other question. Up and green is the
    // battery gaining, down and red is it giving back, so every cue on this card points the
    // same way: green and upward mean more energy available.
    //
    // The arrow shows the state of charge moving, NOT the direction of power flow. Those are
    // opposites at the terminals -- current goes INTO a battery that is charging -- and the
    // earlier version chose the flow reading. One of the two has to lose, and a reader looking
    // at a percentage is asking which way that number is heading.
    const dirColour=state==='charging'?'var(--ok)':state==='discharging'?'var(--bad)':'var(--dim)';
    const arrow=state==='charging'?'\u2191':state==='discharging'?'\u2193':'';
    const rows=f.single
      ?MEAS.filter(([id,,g])=>g==='Battery'&&shown(m,id)).map(([id,label])=>
        `<div class="fact"><div class="k">${esc(label)}</div><div>${esc(reading(m,id))}</div></div>`).join('')
      :`<div class="fact"><div class="k">Power</div><div>${esc(fmt(Math.abs(p),0))} W ${state==='charging'?'in':'out'}</div></div>`;
    h+=`<div class="card" style="margin-top:20px">
      <div class="between"><div class="k">Battery${f.single?'':' · '+esc(f.label||f.id)}</div>
        <div style="font-size:13px;color:${dirColour}" title="${esc(state)}">${arrow} ${esc(idle?'idle':state+' at '+fmt(Math.abs(p),0)+' W')}</div></div>
      <div class="soc">
        <div style="display:flex;align-items:baseline;gap:6px"><span class="pct">${esc(fmt(soc,0))}</span><span class="dim" style="font-size:16px">%</span></div>
        <div class="track"><div class="bar2"><svg width="100%" height="8" viewBox="0 0 100 8" preserveAspectRatio="none">
          <rect x="0" y="0" width="${Math.max(0,Math.min(100,Number(soc)||0))}" height="8" fill="${socColour(soc)}"></rect></svg></div>
          <div class="between hint" style="margin-top:4px"><span>empty</span><span>full</span></div></div>
      </div>
      <div class="cols" style="margin-top:14px">${rows}</div>
      <div class="hint" style="margin-top:12px">Up and green means the battery is filling; down and red means the house is running on stored sun. The arrow says it as well as the colour, so the meaning survives losing either one. The bar is the level and not the direction — red below ${SOC_LOW}%, amber below ${SOC_HALF}%, green above — and stays grey when the inverter reports power but no state of charge.</div>
    </div>`;
  }
  if(multi){
    h+='<div class="stack" style="margin-top:20px">';
    for(const f of fleet){
      const answering=f.online&&f.data_valid&&!f.data_stale;
      const ago=f.last_successful_poll_seconds_ago;
      const when=(ago===null||ago===undefined)?'<span style="color:var(--bad)">never answered</span>'
        :`<span class="dim">${f.data_stale?'stale — ':''}replied ${esc(ago)} s ago</span>`;
      h+=`<div class="fleetrow${answering?'':' bad'}">
        <div class="name">
          <div style="display:flex;align-items:center;gap:8px"><span class="dot ${answering?'ok':'bad'}"></span><b>${esc(f.label||f.id)}</b></div>
          ${f.label?`<div class="hint" style="font-size:11px;margin-top:2px">${esc(f.id)}</div>`:''}
        </div>
        <div class="mini">${spark('d:'+f.id,150,34,{weight:1.5,stroke:answering?'var(--acc)':'var(--dim)'})}</div>
        <div class="num"><b>${esc(f.ac_power_w==null?'—':sp(Math.round(f.ac_power_w)))}</b><span class="k" style="font-size:11px">W now</span></div>
        <div class="num"><b>${fmt(f.energy_today_kwh,2)}</b><span class="k" style="font-size:11px">kWh today</span></div>
        <div class="when">${when}</div>
      </div>`;
    }
    h+='</div>';
    const each=(cfg&&cfg.polling?cfg.polling.interval_seconds:10)*fleet.length;
    h+=`<div class="hint" style="margin-top:10px">One bus, one poll interval: with ${esc(fleet.length)} inverters each is read about every ${esc(each)} s. Every number above is that inverter's own last reply, never an average.</div>`;
    // The detail groups belong to ONE inverter -- s.measurements is the first device's -- so
    // presenting them as the bridge's would be the lie the fleet totals exist to avoid.
    const g=groupsFor(m,true);
    if(g)h+=`<div class="hair"><div class="between"><div class="k">Detail · ${esc((fleet[0]&&(fleet[0].label||fleet[0].id))||'first inverter')}</div>
      <a href="#" onclick="goTab('inv');return false">every inverter's own readings →</a></div>${g}</div>`;
  }else{
    const g=groupsFor(m,true);
    if(g)h+=`<div style="margin-top:20px">${g}</div>
      <div class="hint" style="margin-top:10px">Only what this driver declares it can read. A driver that reports no battery shows no battery, rather than a row of dashes.</div>`;
  }
  $('#live').innerHTML=h;
}

// ---------------- Inverters ----------------
let invBusy=false, invNextMs=0, invIds=[];
// Identity and capabilities are fixed by the driver at boot and never change while the firmware
// runs, so they are cached ACROSS refreshes. Keeping them on the per-poll device object meant
// they were re-fetched every five seconds -- three requests per inverter instead of two, on the
// board that is also driving the bus.
const capsCache={};
async function loadInverters(force){
  // Rate-limited independently of what triggered it: refresh() runs on a timer AND on every SSE
  // state event, and this tab costs 1+3N requests on the board that is also driving the bus.
  const now=Date.now();
  if(invBusy||(!force&&now<invNextMs))return;
  invBusy=true;invNextMs=now+5000;
  try{
    invIds=((await getJson('/api/v1/devices')).devices)||[];
    for(const id of invIds){
      // Sequential, not parallel: N devices is N handlers on the web task.
      devCache[id]=await getJson('/api/v1/devices/'+encodeURIComponent(id));
      measCache[id]=(await getJson('/api/v1/devices/'+encodeURIComponent(id)+'/measurements')).measurements||{};
      if(capsCache[id]===undefined){
        try{capsCache[id]=await getJson('/api/v1/devices/'+encodeURIComponent(id)+'/capabilities')}
        catch(e){capsCache[id]=null}
      }
    }
  }catch(e){/* leave what is on screen: one failed request must not wipe a page being read */}
  finally{invBusy=false}
  if(tab==='inv')paintInverters();
}
function shapeOf(caps){
  if(!caps)return '—';
  return `${caps.phase_count} phase, ${caps.mppt_count} MPPT, ${caps.has_battery?'battery':'no battery'}`;
}
function paintInverters(){
  if(!S){return}
  const b=S.bridge, problems=b.device_problems||[];
  const configured=b.devices_configured??invIds.length;
  let h=`<div class="between">
    <div><div style="font-size:17px;font-weight:600">${esc(invIds.length)} inverter${invIds.length===1?'':'s'} polled${
      configured!==invIds.length?` of ${esc(configured)} configured`:''}</div>
      <div class="hint">All on the onboard RS485 bus, polled in turn. Up to ${esc(b.max_devices||8)}.</div></div>
    <div class="acts" style="margin:0">
      <button onclick="togglePanel('wiz')">${panel==='wiz'?'Close':'Find inverters'}</button>
      <button class="alt" onclick="addExtra()">Add one by hand</button>
    </div></div>`;
  if(panel==='wiz')h+=`<div class="card" style="margin-top:14px" id="wizcard">${wizardHtml()}</div>`;

  // The diagnosis. Discovery having named nothing, or a configured device never having replied,
  // is where the wizard's old advice ran out at "check your wiring" -- these are the three
  // causes in the order they actually happen, each with the test that settles it.
  const silent=invIds.filter(id=>(devCache[id]||{}).last_successful_poll_seconds_ago==null);
  if(problems.length||configured!==invIds.length||silent.length){
    const who=silent.map(id=>esc((devCache[id]||{}).label||id)).join(', ');
    h+=`<div class="card bad" style="margin-top:14px">
      <b>${silent.length?who+(silent.length===1?' has':' have')+' never replied':'Not every configured inverter started'}</b>
      ${problems.length?`<ul style="margin:6px 0 0 18px;color:var(--dim);font-size:13px">${problems.map(p=>`<li>${esc(p)}</li>`).join('')}</ul>`
        :`<div class="hint">Started at boot, so the driver was built — it is the bus reply that is missing.${
          invIds.length>silent.length?' The others answer on this bus, so the bridge and the line speed are fine.':''}</div>`}
      <div class="stack" style="margin-top:14px">
        <div style="border:1px solid var(--line);border-radius:8px;padding:12px 14px">
          <div class="between"><b>1 · Two inverters on the same address</b>
            <button class="link sm" onclick="startDiscovery(true)">Sweep addresses 1–8</button></div>
          <div class="hint">Two units left on one address answer together and destroy each other's replies. The sweep reports which address each unit actually answers at — that is the check, and it takes about a minute. Polling stops while it runs.</div>
        </div>
        <div style="border:1px solid var(--line);border-radius:8px;padding:12px 14px">
          <div class="between"><b>2 · A and B swapped</b>
            <button class="alt sm" onclick="togglePanel('cap')">${panel==='cap'?'Close':'Record the raw bus'}</button></div>
          <div class="hint">The single most common cause, and swapping them back cannot damage anything — sometimes labelled D+ / D−. A recording with bytes but no valid checksum means the line speed is wrong instead; one with nothing at all points back at the wiring. Connect the ground too: without a shared reference the bus can stay silent however the data wires are arranged.</div>
          ${panel==='cap'?captureHtml():''}
        </div>
        <div style="border:1px solid var(--line);border-radius:8px;padding:12px 14px">
          <div class="between"><b>3 · Termination in the wrong place</b>
            <span class="hint" style="margin:0">no test — check the jumpers</span></div>
          <div class="hint">120 Ω belongs at the <b>two ends of the chain</b>, not on every device. A middle unit with its jumper still closed loads the bus, and the one that drops out is usually the one furthest from the bridge.</div>
        </div>
      </div></div>`;
  }

  h+='<div class="stack" style="margin-top:14px">';
  for(const id of invIds){
    const dev=devCache[id]||{}, m=measCache[id]||{}, caps=capsCache[id];
    const ident=dev.identity||{}, drv=dev.driver||{};
    const ago=dev.last_successful_poll_seconds_ago;
    const live=ago==null?'<span style="color:var(--bad)">never answered</span>'
      :dev.data_stale?`<span class="dim">stale — last reply ${esc(ago)} s ago</span>`
                     :`<span class="dim">replied ${esc(ago)} s ago</span>`;
    const count=Object.keys(m).length;
    h+=`<div class="card${ago==null?' bad':''}">
      <div class="between">
        <div style="display:flex;align-items:center;gap:8px">
          <span class="dot ${ago!=null&&dev.online&&dev.data_valid&&!dev.data_stale?'ok':'bad'}"></span>
          <b>${esc(dev.label||id)}</b>
          ${drv.support_level?`<span class="tag">${esc(drv.support_level)}</span>`:''}
        </div>
        <span style="font-size:13px">${live}</span>
      </div>
      <div class="cols" style="margin-top:14px">
        <div class="fact"><div class="k">Driver</div><div>${esc(drv.display_name||ident.driver_id||'—')}</div></div>
        <div class="fact"><div class="k">Model</div><div>${esc(ident.manufacturer||'—')} ${esc(ident.model||'')}</div></div>
        <div class="fact"><div class="k">Serial</div><div>${esc(ident.serial_number||'—')}</div></div>
        <div class="fact"><div class="k">Shape</div><div>${esc(shapeOf(caps))}</div></div>
      </div>
      ${!dev.label?`<div class="hint" style="margin-top:10px">Id <code>${esc(id)}</code> — this is what the API, the MQTT topics and the Modbus unit mapping use.</div>`:''}
      <div class="acts">
        <button class="alt sm" onclick="togglePanel('m:${esc(id)}')">${panel==='m:'+id?'Hide readings':`All ${esc(count)} reading${count===1?'':'s'}`}</button>
        <button class="alt sm" onclick="togglePanel('s:${esc(id)}')">${panel==='s:'+id?'Close settings':'Name, driver and address'}</button>
      </div>
      ${panel==='m:'+id?`<div class="hair">
        <div class="hint" style="margin:0 0 10px">Everything this driver declares it can read. A channel is listed because the inverter <i>has</i> it; an em dash means it has it but is not reporting a value right now.
        ${count<6?' <b>A handful of readings where a dozen were expected is the wrong-register-map tell.</b>':''}</div>
        ${groupsFor(m,false)||'<div class="dim">Nothing published yet.</div>'}
        ${caps&&(caps.write||[]).length===0?'<div class="hint">This driver cannot write to the inverter. Nothing in this build can.</div>':''}
      </div>`:''}
      ${panel==='s:'+id?deviceForm(id,dev):''}
    </div>`;
  }
  h+='</div>';

  // Bus and polling. One card, because the interval and the line are the same subject: both
  // decide what happens on the wire, and both need a restart.
  const c=cfg||{};
  h+=`<div class="card" style="margin-top:12px">
    <div class="between"><div><b>Bus &amp; polling</b> <span class="tag warn">needs restart</span></div>
      <button class="alt sm" onclick="togglePanel('bus')">${panel==='bus'?'Close':'Change'}</button></div>
    <div class="hint">${c.serial?`${esc(c.serial.baud_rate)} ${esc(c.serial.data_bits)}${esc(String(c.serial.parity||'n')[0].toUpperCase())}${esc(c.serial.stop_bits)}, ${c.serial.override?'<b>overridden here</b>':'set by the driver'}`:'—'} · polling every ${esc((c.polling||{}).interval_seconds??10)} s. Only override the line when discovery found a device at a profile the driver does not lead with — otherwise the driver decides, and that is right for almost every install.</div>
    ${panel==='bus'?busForm(c):''}
  </div>`;
  $('#inv').innerHTML=h;
  if(panel==='bus')wireBusForm();
  if(panel==='wiz'&&wizStep===4)wizLoadDrivers();
}

// What the open device panel currently SHOWS, as opposed to what is stored. Without this,
// changing the driver repainted the tab from the stored identity: the select snapped back and
// the new driver's options never appeared -- so a driver could not be changed here at all.
let devDraft=null;

function deviceForm(id,dev){
  const primary=isPrimary(id);
  const storedDrvId=(dev.identity||{}).driver_id||(dev.driver||{}).id||'';
  if(!devDraft||devDraft.id!==id)devDraft={id,drv:storedDrvId,label:dev.label||''};
  const drvId=devDraft.drv;
  const list=(drivers&&drivers.drivers)||[];
  // Stored option values apply only while the selection IS the configured driver. Otherwise the
  // declared defaults do -- rendering one driver's options under another driver's id is how an
  // option once got saved into the wrong driver's config.
  const stored=drvId===storedDrvId?(primary?((cfg&&cfg.driver&&cfg.driver.options)||{}):extraOptionsFor(id)):{};
  const drv=list.find(x=>x.id===drvId);
  let h=`<div class="hair" data-form="dev:${esc(id)}">
    <span class="tag warn">changes here need a restart</span>
    <label for="dv_label">Name</label>
    <input id="dv_label" value="${esc(devDraft.label)}" placeholder="Schuur" autocomplete="off"
      oninput="devDraft.label=this.value">
    <div class="hint">Shown here and in Home Assistant instead of the id. Renaming never changes the id, so history is kept.</div>
    <label for="dv_drv">Driver</label>
    <select id="dv_drv" onchange="devDraft.drv=this.value;paintInverters()">${list.map(x=>
      `<option value="${esc(x.id)}" ${x.id===drvId?'selected':''}>${esc(x.display_name)} (${esc(x.support_level)})</option>`).join('')}</select>
    ${drvId!==storedDrvId?'<div class="hint" style="color:var(--warn)">A different driver from the one running. Its options below start at this driver\u2019s own defaults, not the stored ones.</div>':''}`;
  h+=optionFields(drv,stored,'dv_o_');
  h+=`<div class="acts">
      <button onclick="saveDevice('${esc(id)}',${primary?'true':'false'})">Save</button>
      <button class="alt" onclick="togglePanel(null)">Cancel</button>
      <span style="flex:1"></span>
      ${primary?'':`<button class="dangerAlt" onclick="removeDevice('${esc(id)}')">Remove this inverter</button>`}
    </div>
    <div class="hint">${primary?'This is the first inverter, which every build has. Point it at a different driver rather than removing it.'
      :'Removing it does not remove what it already published: the old entities stay in Home Assistant, available, showing their last value.'}</div>
    <div id="dv_msg" class="msg hide"></div></div>`;
  return h;
}
/// A driver's declared options, at their stored-or-declared values. Rendered from the driver's
/// own declaration, so a new driver's options appear with no frontend change.
function optionFields(drv,stored,prefix){
  const opts=(drv&&drv.options)||[];
  return opts.map(o=>{
    const cur=(stored||{})[o.key]??o.default_value??'';
    const hint=o.description?`<div class="hint">${esc(o.description)}</div>`:'';
    if(o.allowed_values&&o.allowed_values.length){
      // An empty entry among the allowed values is the driver saying "unset means my default".
      // For a register map that silently means the DEFAULT map, so it is labelled and left
      // unselected until someone picks: probing identifies the protocol, never the model, and
      // the wrong map produces believable numbers.
      const hasBlank=o.allowed_values.includes('');
      const known=o.allowed_values.includes(cur);
      const needs=hasBlank&&cur==='';
      return `<label for="${prefix}${esc(o.key)}">${esc(o.display_name)}</label>
        <select id="${prefix}${esc(o.key)}" data-opt="${esc(o.key)}" ${hasBlank?'data-mustpick="1"':''} onchange="gateForms()">
        ${(known?'':`<option value="${esc(cur)}" selected>${esc(cur)} — not recognised</option>`)}
        ${o.allowed_values.map(v=>`<option value="${esc(v)}" ${v===cur&&!needs?'selected':''}>${v===''?'— choose —':esc(v)}</option>`).join('')}
        </select>${hint}`;
    }
    const num=o.min_value!==undefined?` type="number" min="${esc(o.min_value)}" max="${esc(o.max_value)}"`:'';
    return `<label for="${prefix}${esc(o.key)}">${esc(o.display_name)}</label>
      <input id="${prefix}${esc(o.key)}"${num} data-opt="${esc(o.key)}" value="${esc(cur)}" autocomplete="off">${hint}`;
  }).join('');
}
/// Blocks a Save while any option the driver marked ambiguous-when-empty is still empty.
function gateForms(){
  document.querySelectorAll('[data-form]').forEach(f=>{
    const missing=[...f.querySelectorAll('[data-mustpick]')].some(e=>e.value==='');
    f.querySelectorAll('button').forEach(b=>{if(b.textContent==='Save')b.disabled=missing});
  });
}
// Which /devices entry is the one configured under `driver`, as opposed to additional_devices.
// Order is the boot order, and the primary is always first.
function isPrimary(id){return invIds[0]===id}
function extraOptionsFor(id){
  const idx=invIds.indexOf(id)-1;
  const arr=(cfg&&cfg.additional_devices)||[];
  return (arr[idx]||{}).options||{};
}
function readOpts(prefix){
  const out={};
  document.querySelectorAll('[id^="'+prefix+'"][data-opt]').forEach(e=>out[e.dataset.opt]=e.value);
  return out;
}

// ---------------- Integrations ----------------
function paintInt(){
  const c=cfg;
  if(!c){$('#int').innerHTML='<div class="dim">Reading the configuration…</div>';return}
  const b=(S&&S.bridge)||{}, fleet=(S&&S.devices)||[];
  const units=fleet.length?fleet:[{id:'—',label:''}];
  let h=`<div class="stack">
  <div class="card">
    <div class="between"><div style="display:flex;align-items:center;gap:8px">
      <span class="dot ${b.mqtt_connected?'ok':(c.mqtt.enabled?'bad':'off')}"></span><b>MQTT &amp; Home Assistant</b></div>
      <span class="hint" style="margin:0">${c.mqtt.enabled?(b.mqtt_connected?'connected':'enabled, not connected'):'off'}${
        c.mqtt.discovery_enabled?' · discovery on':''}</span></div>
    <div class="hint">Each inverter appears in Home Assistant as its own device, named by the name you gave it here.</div>
    <div class="cols" style="margin-top:14px">
      <div class="fact"><div class="k">Broker</div><div>${esc(c.mqtt.host||'—')}:${esc(c.mqtt.port)}</div></div>
      <div class="fact"><div class="k">Base topic</div><div><code>${esc(c.mqtt.base_topic)}</code></div></div>
      <div class="fact"><div class="k">Publishes refused</div><div>${esc(diag&&diag.mqtt_publish_failure_total!=null?diag.mqtt_publish_failure_total:'—')}</div></div>
    </div>
    <div class="acts"><button class="alt sm" onclick="togglePanel('mqtt')">${panel==='mqtt'?'Close':'Change MQTT settings'}</button></div>
    ${panel==='mqtt'?mqttForm(c):''}
  </div>
  <div class="card">
    <div class="between"><div style="display:flex;align-items:center;gap:8px">
      <span class="dot ${b.modbus_listening?'ok':(c.modbus.enabled?'bad':'off')}"></span><b>Modbus TCP</b></div>
      <span class="hint" style="margin:0">${c.modbus.enabled?`listening on :${esc(c.modbus.port)} · ${esc(b.modbus_clients??0)} of ${esc(c.modbus.max_clients)} clients`:'off'}</span></div>
    <div class="hint">One unit id per inverter, consecutively from ${esc(c.modbus.unit_id)} — the register map is the same at each. Writing is permanently disabled: no driver in this build can write to an inverter.</div>
    <table><tr><th>Unit id</th><th>Inverter</th></tr>
      ${units.map((f,i)=>`<tr><td class="n">${esc(Number(c.modbus.unit_id)+i)}</td><td>${esc(f.label||f.id)}</td></tr>`).join('')}
    </table>
    <div class="acts"><button class="alt sm" onclick="togglePanel('modbus')">${panel==='modbus'?'Close':'Change Modbus settings'}</button></div>
    ${panel==='modbus'?modbusForm(c):''}
  </div>
  <div class="card">
    <b>Always on, nothing to configure</b>
    <div class="hint">On this bridge's address, same port as this page. Reads need no password — the threat model is a trusted LAN. Everything that <i>changes</i> something does.</div>
    <table>
      <tr><td class="dim">Prometheus</td><td><code>/metrics</code> <span class="dim">— one <code>device=</code> label per inverter</span></td></tr>
      <tr><td class="dim">REST</td><td><code>/api/v1/status</code>, <code>/devices</code>, <code>/diagnostics</code>, <code>/config</code></td></tr>
      <tr><td class="dim">Live updates</td><td><code>/api/v1/events</code> <span class="dim">(SSE)</span></td></tr>
    </table>
  </div></div>`;
  $('#int').innerHTML=h;
}
function mqttForm(c){
  return `<div class="hair" data-form="mqtt">
    <span class="tag warn">changes here need a restart</span>
    <label class="check"><input id="mq_en" type="checkbox" ${c.mqtt.enabled?'checked':''}> Publish over MQTT</label>
    <label class="check"><input id="mq_disc" type="checkbox" ${c.mqtt.discovery_enabled?'checked':''}> Home Assistant discovery — entities appear on their own</label>
    <label for="mq_host">Broker host</label><input id="mq_host" value="${esc(c.mqtt.host)}" autocomplete="off">
    <label for="mq_port">Port</label><input id="mq_port" class="sh" type="number" value="${esc(c.mqtt.port)}">
    ${credField('mq_user','Username',c.mqtt.username_set)}
    ${pwField('mq_pw','Password',c.mqtt.password_set)}
    <div class="hint">Neither is ever sent back to this page. Blank means keep.</div>
    <label for="mq_topic">Base topic</label><input id="mq_topic" value="${esc(c.mqtt.base_topic)}" autocomplete="off">
    <div class="hint">Topics become <code>${esc(c.mqtt.base_topic)}/&lt;inverter id&gt;/&lt;measurement&gt;</code>. Changing it leaves the old topics behind in the broker.</div>
    <div class="acts"><button onclick="saveMqtt()">Save</button><button class="alt" onclick="togglePanel(null)">Cancel</button></div>
    <div id="mq_msg" class="msg hide"></div></div>`;
}
function modbusForm(c){
  return `<div class="hair" data-form="modbus">
    <span class="tag warn">changes here need a restart</span>
    <label class="check"><input id="mb_en" type="checkbox" ${c.modbus.enabled?'checked':''}> Serve Modbus TCP</label>
    <label for="mb_port">Port</label><input id="mb_port" class="sh" type="number" value="${esc(c.modbus.port)}">
    <label for="mb_unit">First unit id</label><input id="mb_unit" class="sh" type="number" min="1" max="247" value="${esc(c.modbus.unit_id)}">
    <div class="hint">Each further inverter takes the next id up.</div>
    <label for="mb_max">Max clients at once</label><input id="mb_max" class="sh" type="number" min="1" max="8" value="${esc(c.modbus.max_clients)}">
    <label for="mb_idle">Idle timeout (seconds)</label><input id="mb_idle" class="sh" type="number" min="0" value="${esc(c.modbus.idle_timeout_seconds)}">
    <div class="hint">Eight at once is this firmware's ceiling. Past the limit a client still connects but is never answered, so raise it if Home Assistant, a scraper and a poller share this bridge. The idle timeout drops a silent client to free its slot; 0 never drops one.</div>
    <div class="acts"><button onclick="saveModbus()">Save</button><button class="alt" onclick="togglePanel(null)">Cancel</button></div>
    <div id="mb_msg" class="msg hide"></div></div>`;
}
// readonly-until-focus on every credential field: browsers autofill saved passwords at render
// time whatever autocomplete says, and an autofilled field is indistinguishable from a typed
// one at save -- so changing a log level would silently overwrite a stored password.
function pwField(id,label,isSet){
  return `<label for="${id}">${label}</label>
    <input id="${id}" type="password" placeholder="${isSet?'(unchanged)':'(not set)'}" autocomplete="new-password" readonly onfocus="this.removeAttribute('readonly')">`;
}
function credField(id,label,isSet){
  return `<label for="${id}">${label}</label>
    <input id="${id}" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false"
      placeholder="${isSet?'(unchanged)':'(not set)'}" readonly onfocus="this.removeAttribute('readonly')">`;
}

// ---------------- Health ----------------
let diag=null;
async function loadDiag(){try{diag=await getJson('/api/v1/diagnostics')}catch(e){}}
// The names a person would ask for. The raw field stays on /api/v1/diagnostics untouched for
// anything scraping it -- this map is presentation, not a second source of truth.
const DIAGGROUPS=[
  ['The bus',[['poll_success_total','Polls that succeeded'],['poll_failure_total','Polls that failed'],
    ['consecutive_poll_failures','Failed in a row right now'],['checksum_error_total','Bad checksums'],
    ['rs485_timeout_total','Timeouts waiting for a reply'],['invalid_frame_total','Frames that made no sense']]],
  ['The network',[['wifi_rssi_dbm','WiFi signal','dBm'],['wifi_reconnect_total','WiFi reconnects'],
    ['mqtt_reconnect_total','MQTT reconnects'],['mqtt_publish_failure_total','MQTT publishes refused'],
    ['modbus_client_connections_total','Modbus clients since boot'],['rest_requests_total','API requests']]],
  ['Memory',[['free_heap_bytes','Free now','kB'],['minimum_free_heap_bytes','Lowest it has ever been','kB'],
    ['max_alloc_heap_bytes','Largest single block','kB'],['psram_free_bytes','PSRAM free','kB'],
    ['rs485_stack_free_bytes','RS485 task stack spare','kB'],['loop_stack_free_bytes','Main task stack spare','kB']]],
  ['This boot',[['uptime_seconds','Running for','up'],['reset_reason','Why it started'],
    ['ota_image_state','Firmware image'],['coredump_present','Crash dump stored'],
    ['board','Board'],['last_error','Last error']]],
];
function paintHealth(){
  const d=diag||{};
  const busErrors=(d.checksum_error_total||0)+(d.rs485_timeout_total||0)+(d.invalid_frame_total||0);
  let h=`<div class="tiles">
    <div class="card"><div class="k">Polls succeeded</div><div class="v">${esc(d.poll_success_total??'—')}<span class="u"> · ${esc(d.poll_failure_total??0)} failed</span></div></div>
    <div class="card"><div class="k">Bus errors</div><div class="v">${esc(busErrors)}<span class="u"> since boot</span></div></div>
    <div class="card"><div class="k">Memory free</div><div class="v">${esc(kb(d.free_heap_bytes))}<span class="u"> · low ${esc(kb(d.minimum_free_heap_bytes))}</span></div></div>
    <div class="card"><div class="k">Last restart</div><div class="v">${esc(d.uptime_seconds!=null?up(d.uptime_seconds):'—')}<span class="u"> · ${esc(d.reset_reason||'')}</span></div></div>
  </div>`;
  if(d.coredump_present){
    h+=`<div class="card bad" style="margin-top:12px"><b>A crash dump is stored</b>
      <div class="hint">Task <code>${esc(d.coredump_task||'—')}</code> at <code>${esc(d.coredump_pc||'—')}</code>${
        d.coredump_cause_name?' — '+esc(d.coredump_cause_name):''}. The backtrace is on
        <code>/api/v1/diagnostics</code>; attach it to an issue.</div></div>`;
  }
  h+=`<div class="card" style="margin-top:12px">
    <div class="between"><b>Log</b>
      <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
        <span class="hint" style="margin:0">show</span>
        ${[['all','everything'],['warn','warnings & errors'],['bus','RS485 only']].map(([k,n])=>
          `<button class="pill${logFilter===k?'':' off'}" onclick="logFilter='${k}';loadLogs(true)">${esc(n)}</button>`).join('')}
        <span style="width:1px;height:18px;background:var(--line)"></span>
        <span class="hint" style="margin:0">level</span>
        <select class="tiny" onchange="setLogLevel(this.value)">${['error','warn','info','debug','trace'].map(l=>
          `<option ${cfg&&cfg.logging.level===l?'selected':''}>${l}</option>`).join('')}</select>
        <label class="check" style="margin:0;font-size:12px;color:var(--dim)"><input type="checkbox" ${logPaused?'checked':''} onchange="logPaused=this.checked"> pause</label>
        <button class="alt pill" onclick="copyForIssue(this)">Copy for an issue</button>
      </div></div>
    <div class="hint">Level is here, next to the log, instead of on a settings page — it is the one setting you change while reading this. Filtering happens in this browser, so it costs the bridge nothing.</div>
    <div id="loginfo" class="hint"></div>
    <div id="logbox"></div>
  </div>
  <div class="card" style="margin-top:12px">
    <div class="between"><b>Counters</b><button class="alt pill" onclick="copyCounters(this)">Copy as text</button></div>
    <div class="hint">Named the way a person would ask for them; the raw field name is still on <code>/api/v1/diagnostics</code>, unchanged.</div>
    <div class="groups" style="margin-top:14px">${DIAGGROUPS.map(([title,rows])=>
      `<div><div class="k" style="margin-bottom:6px">${esc(title)}</div>${rows.map(([key,label,unit])=>{
        let v=d[key];
        if(v===undefined)return '';
        if(v===null)v='—';
        else if(unit==='kB')v=kb(v);
        else if(unit==='up')v=up(v);
        else if(unit)v=v+' '+unit;
        else if(typeof v==='boolean')v=v?'yes':'no';
        else if(typeof v==='number')v=sp(v);
        else if(v==='')v='none since boot';
        return `<div class="row rule"><span style="color:var(--mid)">${esc(label)}</span><span>${esc(v)}</span></div>`;
      }).join('')}</div>`).join('')}</div>
  </div>`;
  $('#health').innerHTML=h;
  renderLogs();
}
let logLines=[], logMeta='', logStop=false;
async function loadLogs(force){
  // Never prompts by itself. The log is admin-gated, and opening a tab must not raise a password
  // dialog nobody asked for -- especially not on a timer, which is how the old page could make
  // the prompt reappear every five seconds. Signing in is a deliberate click.
  if(!sessionStorage.getItem('hg_auth')&&!force){
    const box=$('#logbox');
    if(box)box.innerHTML='<span class="dim">The log is admin-gated: raw lines carry protocol traffic and internal state. '+
      '<a href="#" onclick="loadLogs(true);return false">Sign in to read it</a></span>';
    return;
  }
  if(logStop||(logPaused&&!force))return;
  const r=await authFetch('/api/v1/logs?limit=64');
  // A 401 latches as well as a cancel: this runs on a timer, and without the latch the dialog
  // reappears every few seconds and cannot be escaped except by leaving the tab.
  if(r.cancelled||r.status===401){
    logStop=true;
    $('#logbox').innerHTML='<span class="dim">'+
      (r.cancelled?'Admin sign-in required.':'Admin sign-in was not accepted — check the username as well as the password.')+
      ' <a href="#" onclick="logStop=false;loadLogs(true);return false">Try again</a></span>';
    return;
  }
  if(!r.ok){$('#loginfo').textContent=httpWhy(r);return}
  const d=await r.json();
  logLines=d.lines||[];
  logMeta=`level ${d.level} — ${d.total} lines since boot, showing last ${d.returned}`;
  renderLogs();
}
function renderLogs(){
  const box=$('#logbox');if(!box)return;
  $('#loginfo').textContent=logMeta;
  const keep=l=>logFilter==='all'?true:logFilter==='warn'?(l.includes('[W]')||l.includes('[E]'))
    :/rs485|modbus|poll|crc|checksum|timeout/i.test(l);
  const atBottom=box.scrollHeight-box.scrollTop-box.clientHeight<40;
  const lines=logLines.filter(keep);
  box.innerHTML=lines.map(l=>{
    const cls=l.includes('[E]')?'le':l.includes('[W]')?'lw':'';
    return cls?`<span class="${cls}">${esc(l)}</span>`:esc(l);
  }).join('\n')||'<span class="dim">Nothing matches this filter.</span>';
  if(atBottom)box.scrollTop=box.scrollHeight;
}
async function setLogLevel(level){
  const r=await patch({logging:{level}});
  if(r.ok){cfg.logging.level=level;loadLogs(true)}
}
// Confirms in the button itself. Silent success is indistinguishable from a broken button, and
// clipboard writes fail outright on a page served over plain HTTP in some browsers.
function copyText(text,btn){
  const done=ok=>{
    if(!btn)return;
    const was=btn.textContent;
    btn.textContent=ok?'Copied':'Could not copy — select it by hand';
    setTimeout(()=>{btn.textContent=was},2000);
  };
  if(!navigator.clipboard){done(false);return}
  navigator.clipboard.writeText(text).then(()=>done(true),()=>done(false));
}
function copyForIssue(btn){
  const b=(S&&S.bridge)||{};
  copyText(`Heliograph ${b.firmware_version} on ${b.board_id||'?'}\n`+
    `${(S&&S.devices||[]).map(f=>`${f.id}: online=${f.online} valid=${f.data_valid} stale=${f.data_stale} last=${f.last_successful_poll_seconds_ago}`).join('\n')}\n\n`+
    logLines.join('\n'),btn);
}
function copyCounters(btn){
  copyText(Object.entries(diag||{}).map(([k,v])=>k+': '+v).join('\n'),btn);
}

// ---------------- Bridge ----------------
function paintBridge(){
  const c=cfg;
  if(!c){$('#bridge').innerHTML='<div class="dim">Reading the configuration…</div>';return}
  const b=(S&&S.bridge)||{};
  let h='';
  if(pending.length){
    h+=`<div class="card warn">
      <b>${esc(pending.length)} saved change${pending.length===1?'':'s'} waiting for a restart</b>
      <div class="hint">${esc(pending.join(', '))}. The bridge keeps running on the old values until you restart — nothing is lost by waiting.</div>
      <div class="acts"><button style="background:var(--warn);color:#0f1115" onclick="reboot('#br_msg')">Restart now</button></div>
      <div id="br_msg" class="msg hide"></div></div>`;
  }
  h+=`<div class="stack" style="margin-top:${pending.length?'12px':'0'}">
  <div class="card">
    <div class="between"><b>Network</b>
      <button class="alt sm" onclick="togglePanel('net')">${panel==='net'?'Close':'Change'}</button></div>
    <div class="cols" style="margin-top:12px">
      <div class="fact"><div class="k">WiFi</div><div>${esc(c.wifi.ssid||'—')}${b.wifi_rssi_dbm!=null?' · '+esc(b.wifi_rssi_dbm)+' dBm':''}</div></div>
      <div class="fact"><div class="k">Address</div><div>${c.wifi.ip?esc(c.wifi.ip)+' (static)':'automatic (DHCP)'}</div></div>
      <div class="fact"><div class="k">Clock</div><div>${b.time_synced?esc(b.time||'—'):'not synced'}</div></div>
    </div>
    ${panel==='net'?netForm(c):''}
  </div>
  <div class="card">
    <div class="between"><b>Access &amp; safety</b>
      <button class="alt sm" onclick="togglePanel('access')">${panel==='access'?'Close':'Change'}</button></div>
    <div class="note ${c.security.read_only_mode!==false?'ok':''}" style="${c.security.read_only_mode===false?'border-color:var(--warn)':''}">
      <span class="dot ${c.security.read_only_mode!==false?'ok':'warn'}"></span>
      <span>${c.security.read_only_mode!==false
        ?'<b>Read-only mode is on.</b> The bridge only observes: every inverter command is refused and no relay moves. This is the right setting unless you are deliberately using DRM curtailment.'
        :'<b>Read-only mode is off.</b> Writes to the relay outputs are permitted. No driver in this build can write to an inverter, so this unlocks the DRM contacts and nothing else.'}</span>
    </div>
    ${(b.relays||[]).length?`<div class="hint">This board has ${esc(b.relays.length)} relay${b.relays.length===1?'':'s'}. ${
      (c.relays||{}).enabled?'Relays are enabled.':'Relays are switched off.'}${
      (c.relays||{}).enabled&&c.security.read_only_mode!==false?' <b style="color:var(--warn)">Both gates must be open before a contact can move — no relay will move while read-only mode is on.</b>':''}</div>`
      :'<div class="hint">This board has no relays.</div>'}
    ${panel==='access'?accessForm(c,b):''}
  </div>
  <div class="card">
    <b>Backup</b>
    <div class="hint">One file with every setting. Keep it somewhere other than this bridge — it turns a dead board into a twenty-minute job. A restore shows exactly what would change before anything happens, and the configuration from just before it is kept, so a wrong file is undoable.</div>
    <label class="check"><input id="bk_sec" type="checkbox"> Include passwords (WiFi, MQTT, admin)</label>
    <div class="hint"><b>Off by default, and think before turning it on.</b> With it on the file holds those passwords in plain text, and it will sit in your downloads folder, sync to whatever cloud drive is watching it, and be the obvious thing to attach to a bug report. Without them a restore onto <i>this</i> bridge still works — an absent password means keep the one it already has. Even so the file names your WiFi network, the broker and both usernames: treat it as private either way.</div>
    <div class="acts">
      <button class="alt sm" onclick="downloadBackup()">Download backup</button>
      <button class="alt sm" onclick="togglePanel('restore')">${panel==='restore'?'Close':'Restore from a file'}</button>
      <button class="alt sm" style="color:var(--dim)" onclick="undoRestore()">Undo the last restore</button>
    </div>
    <div id="bk_msg" class="msg hide"></div>
    ${panel==='restore'?`<div class="hair">
      <label for="rs_file">Backup file (.json)</label>
      <input id="rs_file" type="file" accept=".json,application/json">
      <div class="acts"><button onclick="previewRestore()">Show what would change</button></div>
      <div id="rs_msg" class="msg hide"></div><div id="rs_pv"></div></div>`:''}
  </div>
  <div class="card" id="updcard">
    <b>Firmware</b>
    <div id="updbox"><div class="hint">Checking…</div></div>
    <label class="check"><input id="upd_en" type="checkbox" ${(c.updates?c.updates.check_enabled!==false:true)?'checked':''} onchange="saveUpdateCheck(this.checked)"> Check for new releases automatically</label>
    <div id="upd_toggle_msg" class="hint"></div>
    <div class="hint">The check runs in <b>this browser</b>: it fetches one small file from the project's GitHub Pages site and compares it with the version this bridge reports. The bridge never opens a connection to the internet for it.</div>
    <div class="acts">
      <button class="alt sm" onclick="manualCheck()">Check now</button>
      <button class="alt sm" onclick="togglePanel('ota')">${panel==='ota'?'Close':'Install a .bin by hand'}</button>
      <button class="alt sm" onclick="reboot('#fw_msg')">Restart bridge</button>
    </div>
    <div id="fw_msg" class="msg hide"></div>
    ${panel==='ota'?`<div class="hair">
      <label for="ota_file">Firmware image (.bin)</label>
      <input id="ota_file" type="file" accept=".bin">
      <div class="acts"><button id="ota_btn" onclick="otaUpload()">Upload and install</button></div>
      <div id="ota_prog" class="prog"><div></div></div>
      <div id="ota_msg" class="msg hide"></div>
      <div class="hint">The image is verified before the boot partition switches; a rejected upload leaves the running firmware untouched.</div></div>`:''}
  </div>
  <div class="card bad">
    <b>Factory reset</b>
    <div class="hint">Erases everything, including WiFi and passwords, and restarts into the setup portal. Download a backup first — with passwords, since a reset board has none to keep. The board's physical RESET button only reboots; holding <b>BOOT</b> for five seconds does what this does, and is the way back when you cannot reach this page.</div>
    <div class="acts"><button class="danger sm" onclick="factoryReset()">Erase and restart</button></div>
  </div></div>`;
  $('#bridge').innerHTML=h;
  if(panel==='net')wireNetForm();
  updRender();
}
function netForm(c){
  const isStatic=!!c.wifi.ip;
  return `<div class="hair" data-form="net">
    <span class="tag warn">changes here need a restart</span>
    <label for="nw_ssid">WiFi network</label>
    <div style="display:flex;gap:10px;flex-wrap:wrap;align-items:center;max-width:420px">
      <input id="nw_ssid" value="${esc(c.wifi.ssid||'')}" autocomplete="off" style="flex:1 1 220px">
      <button class="alt sm" id="nw_scanbtn" onclick="scanNetworks()">Scan</button>
    </div>
    <span id="nw_scanmsg" class="hint"></span>
    <select id="nw_pick" class="hide" onchange="if(this.value)document.querySelector('#nw_ssid').value=this.value"></select>
    ${pwField('nw_pw','WiFi password',c.wifi.password_set)}
    <div class="hint">Leave blank to keep. The stored value is never sent to this page.</div>
    <label for="nw_host">Hostname</label>
    <input id="nw_host" value="${esc(c.wifi.hostname||'')}" autocomplete="off">
    <div class="hint">This bridge is <code>http://${esc(c.wifi.hostname||'')}.local</code>. Letters, digits and hyphens only.</div>
    <label for="nw_name">Display name</label>
    <input id="nw_name" value="${esc(c.bridge_name||'')}" autocomplete="off">
    <div class="hint">Shown in Home Assistant and on this page. Spaces are fine, and it applies immediately.</div>
    <label for="nw_mode">How this bridge gets its address</label>
    <select id="nw_mode" onchange="wireNetForm()">
      <option value="dhcp" ${isStatic?'':'selected'}>Automatic (DHCP)</option>
      <option value="static" ${isStatic?'selected':''}>Static</option>
    </select>
    <div id="nw_static" class="${isStatic?'':'hide'}">
      <label for="nw_ip">IP address</label><input id="nw_ip" value="${esc(c.wifi.ip||'')}" autocomplete="off">
      <label for="nw_gw">Gateway</label><input id="nw_gw" value="${esc(c.wifi.gateway||'')}" autocomplete="off">
      <label for="nw_sn">Subnet mask</label><input id="nw_sn" value="${esc(c.wifi.subnet||'')}" autocomplete="off">
      <label for="nw_dns1">DNS server</label><input id="nw_dns1" value="${esc(c.wifi.dns1||'')}" autocomplete="off">
      <div class="hint">Required as soon as anything here is configured by name — an NTP server, an MQTT broker. Without it the clock never syncs and nothing says so.</div>
      <label for="nw_dns2">Second DNS server (optional)</label><input id="nw_dns2" value="${esc(c.wifi.dns2||'')}" autocomplete="off">
      <div class="hint"><b>Get this wrong and the bridge disappears.</b> A wrong address does not stop it joining the WiFi — it joins, and is then simply unreachable. If it goes missing: hold BOOT for five seconds and start again from the setup portal.</div>
    </div>
    <label class="check"><input id="nw_ntp" type="checkbox" ${c.ntp.enabled?'checked':''}> Set the clock over NTP</label>
    <label class="check"><input id="nw_ntpdhcp" type="checkbox" ${c.ntp.use_dhcp?'checked':''}> Use the NTP server offered by DHCP</label>
    <label for="nw_ntps">NTP server (fallback)</label><input id="nw_ntps" value="${esc(c.ntp.server||'')}" autocomplete="off">
    <label for="nw_tz">Timezone</label>
    <select id="nw_tz" onchange="wireNetForm()">${tzOptions(c.ntp)}</select>
    <span id="nw_tzc" class="${tzKnown(c.ntp)?'hide':''}">
      <label for="nw_tzcustom">POSIX TZ string</label>
      <input id="nw_tzcustom" value="${esc(c.ntp.timezone||'')}" autocomplete="off">
      <div class="hint">For zones not in the list, e.g. <code>CET-1CEST,M3.5.0,M10.5.0/3</code>.</div>
    </span>
    <div class="acts"><button onclick="saveNetwork()">Save</button><button class="alt" onclick="togglePanel(null)">Cancel</button></div>
    <div id="nw_msg" class="msg hide"></div></div>`;
}
// Hidden rather than disabled on DHCP: an empty field still on screen reads as "you may fill
// this in", and on DHCP filling it in is exactly what the firmware refuses.
function wireNetForm(){
  const st=$('#nw_mode')&&$('#nw_mode').value==='static';
  if($('#nw_static')){
    $('#nw_static').classList.toggle('hide',!st);
    if(st&&!$('#nw_sn').value)$('#nw_sn').value='255.255.255.0';
  }
  if($('#nw_tzc'))$('#nw_tzc').classList.toggle('hide',$('#nw_tz').value!=='__custom');
}
function accessForm(c,b){
  return `<div class="hair" data-form="access">
    <span class="tag">applied immediately</span>
    ${credField('ac_user','Admin username',true)}
    ${pwField('ac_pw','Admin password',c.security.password_set)}
    <div class="hint">Neither half is ever sent back to this page. <b>Write down anything other than “admin”: a forgotten username needs a factory reset.</b></div>
    <label class="check"><input id="ac_ro" type="checkbox" ${c.security.read_only_mode!==false?'checked':''}> Read-only mode — the global write kill switch</label>
    <div class="hint">On by default, and the outermost gate on everything this bridge can change. Turning it off permits writes to the relay outputs; no driver in this build can write to an inverter. Leave it on unless you are deliberately using DRM curtailment.</div>
    ${(b.relays||[]).length?`<label class="check"><input id="ac_rl" type="checkbox" ${(c.relays||{}).enabled?'checked':''}> Relays enabled</label>
      <div class="hint">Both this and read-only mode being off are required before a contact can move. Disabling releases every relay. See docs/drm.md for the wiring rule: a dead bridge must leave the inverter running.</div>
      ${b.relays.map((_,i)=>`<label for="ac_r${i}">Relay ${i+1} role</label>
        <select id="ac_r${i}" data-role="${i}" class="sh">${['none','drm0','drm1','drm2','drm3','drm4','drm5','drm6','drm7','drm8'].map(r=>
          `<option ${r===(((c.relays||{}).roles||[])[i]||'none')?'selected':''}>${r}</option>`).join('')}</select>`).join('')}`:''}
    <div class="acts"><button onclick="saveAccess()">Save</button><button class="alt" onclick="togglePanel(null)">Cancel</button></div>
    <div id="ac_msg" class="msg hide"></div></div>`;
}
function busForm(c){
  return `<div class="hair" data-form="bus">
    <span class="tag warn">changes here need a restart</span>
    <label for="bs_int">Poll interval (seconds)</label>
    <input id="bs_int" class="sh" type="number" min="2" max="600" value="${esc((c.polling||{}).interval_seconds??10)}">
    <div class="hint">With ${esc(invIds.length||1)} inverter${invIds.length===1?'':'s'} on one bus each is read every ${esc(((c.polling||{}).interval_seconds??10)*(invIds.length||1))} s. Faster than 5 s buys little: most inverters refresh their own registers about once a second.</div>
    <label class="check"><input id="bs_ovr" type="checkbox" ${c.serial.override?'checked':''} onchange="wireBusForm()"> Override the line settings the driver chooses</label>
    <div class="hint">Off, the driver configures the line itself — right for almost every install. The discovery wizard turns this on by itself when it finds a device at a profile the driver does not lead with, because the driver would otherwise go back to its own default on the next boot and the bus would fall silent.</div>
    <div class="cols" style="margin-top:12px;max-width:620px">
      <div><label for="bs_baud">Baud rate</label><input id="bs_baud" type="number" value="${esc(c.serial.baud_rate)}"></div>
      <div><label for="bs_par">Parity</label><select id="bs_par">${['none','even','odd'].map(x=>
        `<option ${c.serial.parity===x?'selected':''}>${x}</option>`).join('')}</select></div>
      <div><label for="bs_db">Data bits</label><input id="bs_db" type="number" value="${esc(c.serial.data_bits)}"></div>
      <div><label for="bs_sb">Stop bits</label><input id="bs_sb" type="number" value="${esc(c.serial.stop_bits)}"></div>
    </div>
    <div class="acts"><button onclick="saveBus()">Save</button><button class="alt" onclick="togglePanel(null)">Cancel</button></div>
    <div id="bs_msg" class="msg hide"></div></div>`;
}
function wireBusForm(){
  const on=$('#bs_ovr')&&$('#bs_ovr').checked;
  ['bs_baud','bs_par','bs_db','bs_sb'].forEach(id=>{if($('#'+id))$('#'+id).disabled=!on});
}
async function scanNetworks(){
  const btn=$('#nw_scanbtn'),msg=$('#nw_scanmsg'),pick=$('#nw_pick');
  btn.disabled=true;msg.textContent='scanning… (takes a few seconds)';
  try{
    const r=await authFetch('/api/v1/wifi/scan');
    const d=await r.json();
    if(!r.ok)throw new Error(httpWhy(r));
    const nets=d.networks||[];
    // A real <select>, not a datalist: a datalist filters on the field's current value, and
    // this field is prefilled with the active SSID, so the list looked empty for every other
    // network. The text field stays authoritative, so hidden SSIDs remain typable.
    pick.innerHTML='<option value="">— pick a network —</option>'+nets.map(n=>
      `<option value="${esc(n.ssid)}">${esc(n.ssid)}  (${esc(n.rssi)} dBm)${n.open?' — open':''}</option>`).join('');
    pick.classList.toggle('hide',!nets.length);
    msg.textContent=nets.length+' networks found.';
  }catch(e){msg.textContent='Scan failed: '+e.message}
  btn.disabled=false;
}

// ---------------- config writes ----------------
// Every panel PATCHes only its own section. That is what removes the whole class of bug the old
// single form had: it could sit open for hours and then write its stale copy of everything over
// changes made in another tab or by the wizard.
let pending=[];   // human labels of saved settings still waiting for a restart
const RESTART_NEEDED={
  'wifi.ssid':'WiFi network','wifi.password':'WiFi password','wifi.hostname':'Hostname',
  'wifi.ip':'IP address','wifi.gateway':'Gateway','wifi.subnet':'Subnet mask',
  'wifi.dns1':'DNS server','wifi.dns2':'Second DNS server',
  'mqtt.enabled':'MQTT on/off','mqtt.host':'MQTT broker','mqtt.port':'MQTT port',
  'mqtt.username':'MQTT username','mqtt.password':'MQTT password',
  'mqtt.base_topic':'MQTT base topic','mqtt.discovery_enabled':'Home Assistant discovery',
  'modbus.enabled':'Modbus on/off','modbus.port':'Modbus port','modbus.unit_id':'Modbus unit id',
  'modbus.max_clients':'Modbus max clients','modbus.idle_timeout_seconds':'Modbus idle timeout',
  'polling.interval_seconds':'Poll interval',
  'driver.id':'Active driver','driver.label':'First inverter name','driver.options':'Driver options',
  'ntp.enabled':'NTP on/off','ntp.use_dhcp':'NTP via DHCP','ntp.server':'NTP server','ntp.timezone':'Timezone',
  'serial.override':'RS485 line override','serial.baud_rate':'RS485 baud rate',
  'serial.parity':'RS485 parity','serial.data_bits':'RS485 data bits','serial.stop_bits':'RS485 stop bits',
  'additional_devices':'Extra inverters',
};
const pick_=(o,path)=>path.split('.').reduce((x,k)=>x&&x[k],o);
async function patch(body){
  const r=await authFetch('/api/v1/config',{method:'PATCH',
    headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  const d=await r.json().catch(()=>({}));
  if(r.ok){
    // Renaming the admin account invalidates the credential we just authenticated with, and
    // nothing else would heal it -- the very next admin action would 401 straight after a
    // successful rename. Re-key on the new name with the password half that just worked.
    if(body.security&&body.security.admin_username){
      const cur=atob(sessionStorage.getItem('hg_auth')||'');
      if(cur){
        // Only the new NAME is encoded here. What atob returns is already the UTF-8 bytes that
        // were stored, one per character, so running the password half back through
        // TextEncoder encodes it a second time: 'ë' (c3 ab) became c3 83 c2 ab and the rename
        // caused exactly the 401 this re-key exists to prevent. Silent on ASCII passwords,
        // which is why it survived (review, 2026-07-28).
        const pw=cur.slice(cur.indexOf(':')+1);
        sessionStorage.setItem('hg_auth',btoa(String.fromCharCode(
          ...new TextEncoder().encode(body.security.admin_username+':'))+pw));
        sessionStorage.setItem('hg_user',body.security.admin_username);
      }
    }
    // The server is authoritative on whether a restart is needed; the map above only supplies
    // the labels.
    if(d.reboot_required){
      for(const [path,label] of Object.entries(RESTART_NEEDED)){
        const now=pick_(body,path);
        if(now===undefined)continue;
        if(path.endsWith('password')){if(now&&!pending.includes(label))pending.push(label);continue}
        if(JSON.stringify(now)!==JSON.stringify(pick_(cfg,path))&&!pending.includes(label))pending.push(label);
      }
    }
    cfg=d.config||d;
    const el=$('#pending');
    el.textContent=pending.length+' change'+(pending.length===1?'':'s')+' waiting for a restart';
    el.classList.toggle('hide',!pending.length);
  }
  return {ok:r.ok,status:r.status,cancelled:r.cancelled,body:d,
    why:(d.error&&d.error.message)||httpWhy(r)};
}
function say(sel,cls,text){const m=$(sel);if(!m)return;m.className='msg '+cls;m.innerHTML=text;m.classList.remove('hide')}
const val=id=>{const e=$('#'+id);return e?e.value:''};
const num=id=>Number(val(id));
const chk=id=>{const e=$('#'+id);return e?e.checked:false};

async function saveMqtt(){
  const body={mqtt:{enabled:chk('mq_en'),discovery_enabled:chk('mq_disc'),host:val('mq_host'),
    port:num('mq_port'),base_topic:val('mq_topic')}};
  // Credentials travel only when typed: a blank field means keep, and GET never returns them.
  if(val('mq_user'))body.mqtt.username=val('mq_user').trim();
  if(val('mq_pw'))body.mqtt.password=val('mq_pw');
  const r=await patch(body);
  if(!r.ok){say('#mq_msg','err',esc(r.why));return}
  say('#mq_msg','ok','Saved.'+(r.body.reboot_required?' Takes effect after a restart.':''));
  paintInt();
}
async function saveModbus(){
  const r=await patch({modbus:{enabled:chk('mb_en'),port:num('mb_port'),unit_id:num('mb_unit'),
    max_clients:num('mb_max'),idle_timeout_seconds:num('mb_idle')}});
  if(!r.ok){say('#mb_msg','err',esc(r.why));return}
  say('#mb_msg','ok','Saved. Takes effect after a restart.');
  paintInt();
}
async function saveBus(){
  const r=await patch({polling:{interval_seconds:num('bs_int')},
    serial:{override:chk('bs_ovr'),baud_rate:num('bs_baud'),parity:val('bs_par'),
            data_bits:num('bs_db'),stop_bits:num('bs_sb')}});
  if(!r.ok){say('#bs_msg','err',esc(r.why));return}
  say('#bs_msg','ok','Saved. Takes effect after a restart.');
}
async function saveNetwork(){
  const st=val('nw_mode')==='static';
  const body={bridge_name:val('nw_name'),
    // On DHCP every address field is sent EMPTY, not omitted: clearing is how a bridge goes
    // back from static, and an omitted key means "leave alone" to the patch handler.
    wifi:{ssid:val('nw_ssid'),hostname:val('nw_host'),
      ...(st?{ip:val('nw_ip').trim(),gateway:val('nw_gw').trim(),subnet:val('nw_sn').trim(),
             dns1:val('nw_dns1').trim(),dns2:val('nw_dns2').trim()}
         :{ip:'',gateway:'',subnet:'',dns1:'',dns2:''})},
    ntp:{enabled:chk('nw_ntp'),use_dhcp:chk('nw_ntpdhcp'),server:val('nw_ntps'),
      // The dropdown value is an IANA name; the firmware only understands the POSIX string.
      ...(val('nw_tz')==='__custom'?{timezone:val('nw_tzcustom'),timezone_name:''}
         :{timezone:TZBYNAME[val('nw_tz')],timezone_name:val('nw_tz')})}};
  if(val('nw_pw'))body.wifi.password=val('nw_pw');
  const r=await patch(body);
  if(!r.ok){say('#nw_msg','err',esc(r.why));return}
  say('#nw_msg','ok','Saved.'+(r.body.reboot_required?' The network settings take effect after a restart.':''));
}
async function saveAccess(){
  const body={security:{read_only_mode:chk('ac_ro')}};
  if(val('ac_user').trim())body.security.admin_username=val('ac_user').trim();
  if(val('ac_pw'))body.security.admin_password=val('ac_pw');
  const relays=document.querySelectorAll('[data-role]');
  if($('#ac_rl'))body.relays={enabled:chk('ac_rl'),
    roles:[...relays].map(e=>e.value)};
  const r=await patch(body);
  if(!r.ok){say('#ac_msg','err',esc(r.why));return}
  say('#ac_msg','ok','Saved and applied.');
  paintBridge();
}
async function saveDevice(id,primary){
  const opts=readOpts('dv_o_');
  const body=primary
    ?{driver:{id:val('dv_drv'),label:val('dv_label'),...(Object.keys(opts).length?{options:opts}:{})}}
    :(()=>{
      // The array is replaced wholesale by the API, so it travels whole -- built from the
      // stored list with this one row rewritten, never from the DOM of the other rows.
      const arr=((cfg.additional_devices)||[]).map(d=>({driver_id:d.driver_id,label:d.label||'',options:{...(d.options||{})}}));
      const idx=invIds.indexOf(id)-1;
      if(idx>=0&&idx<arr.length)arr[idx]={driver_id:val('dv_drv'),label:val('dv_label'),options:opts};
      return {additional_devices:arr};
    })();
  const problem=addressProblem(body);
  if(problem){say('#dv_msg','err',esc(problem));return}
  const r=await patch(body);
  if(!r.ok){say('#dv_msg','err',esc(r.why));return}
  say('#dv_msg','ok','Saved. Takes effect after a restart.');
  invNextMs=0;loadInverters(true);
}
/// What the firmware would refuse, or accept and then quietly skip at boot -- said here, next
/// to the field, rather than as `additional_devices[2].driver_id` under a Save button.
function addressProblem(body){
  const seen={};
  const rowOf=(drvId,options)=>{
    const drv=((drivers&&drivers.drivers)||[]).find(x=>x.id===drvId);
    for(const o of ((drv&&drv.options)||[])){
      const v=String((options||{})[o.key]??'').trim();
      if(o.allowed_values&&o.allowed_values.includes('')&&v==='')
        return 'choose a '+o.display_name.toLowerCase()+' — leaving it unset silently uses the driver default.';
    }
    return null;
  };
  const add=(drvId,options,who)=>{
    const bad=rowOf(drvId,options);
    if(bad)return who+': '+bad;
    // The ADDRESS alone, not the address per driver. There is one RS485 bus, and two units
    // answering to the same number collide on it whichever driver is polling them. More than
    // one Modbus driver is compiled in, and they all draw from the same 1-247 numbering, so
    // keying on driver+address let the likeliest real collision through unreported -- in the
    // card built to report exactly that (review, 2026-07-28).
    //
    // No false positives from the protocols that do not address this way: an AA55 device is
    // found by serial number and declares no unit_id, so addr is undefined and it is skipped.
    const addr=(options||{}).unit_id??(options||{}).address;
    if(addr===undefined||String(addr).trim()==='')return null;
    const key=String(addr).trim();
    if(seen[key])return who+' has the same address as '+seen[key]+'. Each inverter on the bus needs its own.';
    seen[key]=who;
    return null;
  };
  const primary=body.driver||(cfg.driver||{});
  let p=add(primary.id,primary.options||(cfg.driver||{}).options,'The first inverter');
  if(p)return p;
  const arr=body.additional_devices||cfg.additional_devices||[];
  for(let i=0;i<arr.length;i++){
    if(!arr[i].driver_id)return 'Inverter '+(i+2)+': pick a driver, or remove the row.';
    p=add(arr[i].driver_id,arr[i].options,'Inverter '+(i+2));
    if(p)return p;
  }
  return null;
}
async function removeDevice(id){
  if(!confirm('Remove this inverter from the configuration? Its entities stay in Home Assistant showing their last value.'))return;
  const idx=invIds.indexOf(id)-1;
  const arr=((cfg.additional_devices)||[]).filter((_,i)=>i!==idx);
  const r=await patch({additional_devices:arr});
  if(!r.ok){say('#dv_msg','err',esc(r.why));return}
  panel=null;invNextMs=0;loadInverters(true);
}
async function addExtra(){
  const arr=((cfg.additional_devices)||[]).map(d=>({driver_id:d.driver_id,label:d.label||'',options:{...(d.options||{})}}));
  if(arr.length+1>=((S&&S.bridge&&S.bridge.max_devices)||8)){alert('This firmware polls at most '+((S&&S.bridge&&S.bridge.max_devices)||8)+' inverters.');return}
  // Deliberately empty: an extra device that names no driver is refused, and an address
  // defaulting to the primary's would collide and be skipped at boot with only a log line.
  arr.push({driver_id:'',label:'',options:{}});
  const r=await patch({additional_devices:arr});
  if(!r.ok){alert('Could not add a row: '+r.why);return}
  invNextMs=0;await loadInverters(true);
  alert('A row was added. Open “Name, driver and address” on the new inverter to fill it in, then restart.');
}
async function saveUpdateCheck(on){
  const r=await patch({updates:{check_enabled:on}});
  if(!r.ok){$('#upd_en').checked=!on;$('#upd_toggle_msg').textContent='Could not save: '+r.why;return}
  $('#upd_toggle_msg').textContent=on?'Checking for updates is on.':'Checking for updates is off.';
}
async function reboot(sel){
  const r=await authFetch('/api/v1/actions/reboot',{method:'POST'});
  if(!r.ok&&r.status!==202){say(sel,'err','Restart refused: '+esc(httpWhy(r)));return}
  pending=[];$('#pending').classList.add('hide');
  say(sel,'ok','Restarting. This page will go blank for a few seconds — reload it after.');
}
async function factoryReset(){
  if(!confirm('Erase all settings including WiFi and passwords?'))return;
  const r=await authFetch('/api/v1/actions/factory-reset',{method:'POST'});
  alert(r.ok?'Erased. The bridge is restarting into setup mode.':'Failed: '+httpWhy(r));
}

// ---------------- discovery wizard ----------------
let wizStep=1, wizPoll=null, wizChosen=null, wizReport=null, wizOptions={}, wizFound={},
    wizSavedSerial=null, wizStoredDriverId=null, wizStoredOptions={};
const STEPS=['Mode','Probing','Candidates','Confirm','Test poll','Save'];
function stepBar(){
  return '<div style="display:flex;gap:6px;flex-wrap:wrap;margin-bottom:14px">'+STEPS.map((n,i)=>
    `<span class="tag" style="${i+1===wizStep?'border-color:var(--acc);color:var(--fg)':''}">${i+1}. ${n}</span>`).join('')+'</div>';
}
function wizardHtml(){
  let h=stepBar();
  if(wizStep===1){
    h+=`<b>Find what is on the bus</b>
    <div class="hint">Check the wiring and the 120 Ω termination first: an unterminated bus at the end of a long cable is the most common reason nothing answers.</div>
    <div class="hint" style="margin-top:8px"><b>Quick</b> tries each auto-detectable driver once, on its own recommended line settings and its own default address — a few seconds. <b>Extended</b> also tries every profile and <b>sweeps addresses 1–8</b>, which is what finds a chain of inverters rather than only the one at the default address. Budget a minute. Polling stops for the whole run either way.</div>
    <div class="hint" style="margin-top:8px">Probing never writes a register and never starts or stops the inverter. One exception, and it is not new: on protocols where the bridge <i>registers</i> devices, being discovered means being handed a bus address — there is no read-only way to find such a device at all.</div>
    <div class="acts">
      <button onclick="startDiscovery(false)">Run quick discovery</button>
      <button class="alt" onclick="startDiscovery(true)">Run extended discovery</button>
      <button class="alt" onclick="wizStep=4;paintInverters()">Skip — choose a driver myself</button>
    </div>`;
  }else if(wizStep===2){
    h+=`<b>Probing</b><div class="hint">Talking to the bus… normal polling is paused.</div>
      <div class="hint">${esc(wizReport?wizReport.status:'starting')}${wizReport&&wizReport.elapsed_ms?' · '+Math.round(wizReport.elapsed_ms/1000)+' s':''}</div>`;
  }else if(wizStep===3){
    const c=(wizReport&&wizReport.candidates)||[];
    const swept=(wizReport&&wizReport.swept_addresses)||[];
    const sweptText=swept.length?`Addresses ${esc(swept[0])}–${esc(swept[swept.length-1])} and each driver's own default were tried.`:'';
    h+=`<b>What answered</b><div class="hint">${esc(wizReport?wizReport.reason:'')}</div>`;
    if(!c.length)h+=`<div class="hint" style="margin-top:8px">Nothing was identified. ${sweptText||'The quick scan only tries each driver\u2019s default line speed and default address — run the <b>extended</b> scan to try all of them, and addresses 1–8.'} Then check the wiring: A/B swapped is the most common cause, then termination, then a missing ground.</div>`;
    const unk=(wizReport&&wizReport.unidentified_addresses)||[];
    if(unk.length){
      h+=`<div class="msg err" style="display:block"><b>Traffic at ${unk.length===1?'an address':'addresses'} with no device identified.</b>
        <ul style="margin:6px 0 0 18px">${unk.map(u=>`<li>Address <b>${esc(u.address)}</b> (${esc(u.driver_id)}): ${esc(u.note)}</li>`).join('')}</ul>
        <div class="hint">On a chain of identical inverters this usually means two are still on the same address — their replies collide. Put one unit on the bus at a time to confirm, then reassign.</div></div>`;
    }
    // Devices, not cards: two drivers can both claim one physical inverter, and telling someone
    // to add "3 devices" when two of the cards are one unit produces the duplicate-id collision
    // the boot loop refuses.
    const deviceCount=new Set(c.map(x=>x.driver_id+'@'+(x.address??''))).size;
    if(deviceCount>1)h+=`<div class="hint" style="margin-top:8px">${esc(deviceCount)} devices answered. ${sweptText} This configures one — add the rest afterwards, using the addresses below.</div>`;
    if(wizReport&&wizReport.candidates_omitted>0)h+=`<div class="hint">${esc(wizReport.candidates_omitted)} further lower-scoring candidate(s) are not shown.</div>`;
    for(const x of c){
      h+=`<div style="border:1px solid var(--line);border-radius:8px;padding:12px;margin-top:10px">
        <div class="between"><b>${esc(x.display_name)}</b><span class="tag">${esc(x.confidence)}/100</span></div>
        <table>
        <tr><td class="dim">Driver</td><td>${esc(x.driver_id)} <span class="tag">${esc(x.support_level)}</span></td></tr>
        <tr><td class="dim">Bus address</td><td>${x.address!=null?'<b>'+esc(x.address)+'</b>'
          :(swept.length?'<span class="dim">assigned by the protocol</span>'
                        :'<span class="dim">not probed — the quick scan does not sweep addresses</span>')}</td></tr>
        <tr><td class="dim">Line settings tried</td><td>${x.serial_profile?`${esc(x.serial_profile.baud_rate)} ${esc(x.serial_profile.data_bits)}${esc(String(x.serial_profile.parity)[0].toUpperCase())}${esc(x.serial_profile.stop_bits)}, timeout ${esc(x.serial_profile.response_timeout_ms)} ms`:'—'}</td></tr>
        <tr><td class="dim">Replied</td><td>${x.responded?'yes':'no'}</td></tr>
        <tr><td class="dim">Checksum valid</td><td>${x.checksum_valid?'yes':'no'}</td></tr>
        <tr><td class="dim">Repeat probe agreed</td><td>${x.consistent?'yes':'<b>no — score halved</b>'}</td></tr>
        <tr><td class="dim">Detected</td><td>${esc(x.detected_manufacturer||'—')} ${esc(x.detected_model||'')}</td></tr>
        <tr><td class="dim">Serial number</td><td>${esc(x.serial_number||'—')}</td></tr>
        <tr><td class="dim">Evidence</td><td>${(x.evidence||[]).map(e=>'· '+esc(e)).join('<br>')||'—'}</td></tr>
        </table>
        <div class="acts"><button onclick='wizPick(${JSON.stringify(x.driver_id)},${JSON.stringify(x.options||{})})'>Choose this device</button></div>
      </div>`;
    }
    h+=`<div class="acts"><button class="alt" onclick="wizStep=1;paintInverters()">Back</button></div>`;
    // The dead end this exists for: discovery has finished and named nothing, and the only
    // remaining advice used to be "check your wiring". A raw capture is the next real step, and
    // it is the first thing docs/adding-a-device.md asks a contributor for.
    if(!c.length)h+=`<div class="hair">${captureHtml()}</div>`;
  }else if(wizStep===4){
    h+=`<b>Confirm</b>
      <div class="hint">Nothing is saved until the last step. An uncertain match is never selected for you: reading the wrong register map produces believable numbers. Which is why the map is a field here and not an assumption — probing identifies the <i>protocol</i>, never the model.</div>
      <label for="wz_drv">Driver</label><select id="wz_drv" onchange="wizRenderOpts()"></select>
      <div id="wz_opts" data-form="wiz"></div>
      <div id="wz_note" class="msg err hide">Pick a register map first — it cannot be detected, and the wrong one produces believable numbers.</div>
      <div class="acts">
        <button id="wz_go" disabled onclick="wizConfirm()">Confirm and test</button>
        <button class="alt" onclick="wizStep=3;paintInverters()">Back</button></div>`;
  }else if(wizStep===5){
    h+=`<b>Test poll</b>
      <div class="hint">This polls the configuration the bridge is <b>running now</b> — the driver is built once at boot, so nothing chosen above is in force yet. Useful for "is anything alive on this bus", not for confirming the register map. Check that after the restart, by the number of readings published.</div>
      <div id="wz_tp" class="hint">Polling…</div>`;
  }else if(wizStep===6){
    h+=`<b>Saved</b><div class="hint">The driver is stored. It takes effect after a restart.</div>`;
    if(wizSavedSerial&&wizSavedSerial.override)h+=`<div class="hint">This device answered at <b>${esc(wizSavedSerial.baud_rate)} ${esc(wizSavedSerial.data_bits)}${esc(String(wizSavedSerial.parity)[0].toUpperCase())}${esc(wizSavedSerial.stop_bits)}</b>, which is not this driver's default, so those line settings were saved with it. Change or clear that under <b>Bus &amp; polling</b>.</div>`;
    if(wizSavedSerial&&wizSavedSerial.override===false)h+=`<div class="hint">This device answered at the driver's own default, so no line override is stored — the driver decides. Any override left from an earlier run has been switched off.</div>`;
    h+=`<div class="acts"><button onclick="reboot('#wz_msg')">Restart now</button>
      <button class="alt" onclick="panel=null;paintInverters()">Later</button></div>
      <div id="wz_msg" class="msg hide"></div>`;
  }
  return h;
}
function wizPick(id,options){wizChosen=id;wizFound=options||{};wizStep=4;paintInverters()}
async function startDiscovery(extended){
  panel='wiz';
  // wizFound is cleared, not merely reassigned on the next pick: it outranks the stored
  // configuration in the confirm step, so a leftover address would propose a device nobody chose.
  wizStep=2;wizReport=null;wizChosen=null;wizFound={};paintInverters();
  const r=await authFetch('/api/v1/actions/discover'+(extended?'?extended=true':''),{method:'POST'});
  if(r.cancelled||r.status===401){wizStep=1;paintInverters();return}
  if(!r.ok&&r.status!==202){wizStep=1;paintInverters();alert('Could not start discovery: '+httpWhy(r));return}
  clearInterval(wizPoll);
  wizPoll=setInterval(async()=>{
    try{wizReport=await getJson('/api/v1/discovery')}catch(e){return}
    if(wizReport.busy){if(panel==='wiz')paintInverters();return}
    clearInterval(wizPoll);
    if(wizReport.auto_selected){wizChosen=wizReport.selected_driver_id;wizFound=wizReport.selected_options||{}}
    wizStep=3;paintInverters();
  },1000);
}
async function wizLoadDrivers(){
  const sel=$('#wz_drv');
  if(!sel)return;
  if(!drivers)drivers=await getJson('/api/v1/drivers');
  if(!cfg)cfg=await getJson('/api/v1/config');
  wizStoredDriverId=cfg.driver.id;wizStoredOptions=cfg.driver.options||{};
  sel.innerHTML=(drivers.drivers||[]).map(x=>
    `<option value="${esc(x.id)}" ${x.id===wizChosen?'selected':''}>${esc(x.display_name)} (${esc(x.support_level)})</option>`).join('');
  wizRenderOpts();
}
function wizRenderOpts(){
  const box=$('#wz_opts');if(!box)return;
  const id=($('#wz_drv')||{}).value;
  const drv=((drivers&&drivers.drivers)||[]).find(x=>x.id===id);
  // Seeded from what is STORED when this is the driver already configured. Rendering declared
  // defaults unconditionally meant re-running the wizard silently rewrote a working
  // {profile:"mic_tl_x", unit_id:"3"} back to the defaults and reported success. What the
  // device ANSWERED at wins over both -- that is the whole point of sweeping.
  const stored=(id===wizStoredDriverId)?wizStoredOptions:{};
  const merged={...stored,...(id===wizChosen?wizFound:{})};
  box.innerHTML=optionFields(drv,merged,'wz_o_');
  const missing=[...box.querySelectorAll('[data-mustpick]')].some(e=>e.value==='');
  $('#wz_go').disabled=missing;
  $('#wz_note').classList.toggle('hide',!missing);
}
function wizConfirm(){
  // Captured on the way out: the step change replaces this subtree, so reading the fields later
  // would silently save nothing.
  wizChosen=($('#wz_drv')||{}).value||null;
  wizOptions=readOpts('wz_o_');
  wizStep=5;paintInverters();testPoll();
}
async function testPoll(){
  const r=await authFetch('/api/v1/actions/poll',{method:'POST'});
  const el=()=>$('#wz_tp');
  if(!r.ok&&r.status!==202){if(el())el().textContent='Poll refused: '+httpWhy(r);return}
  // The poll runs on the RS485 task and re-registers first -- three bus transactions before the
  // measurement. 1500 ms showed the pre-poll state.
  setTimeout(async()=>{
    let s;try{s=await getJson('/api/v1/status')}catch(e){return}
    const p=s.measurements&&s.measurements['ac.power.total'];
    if(!el())return;
    el().innerHTML=`Inverter online: <b>${esc(s.device.online)}</b> · data valid: <b>${esc(s.device.data_valid)}</b><br>
      AC power: <b>${p&&p.value!==null?esc(p.value)+' W':'unknown'}</b> · serial: <b>${esc(s.device.serial_number||'—')}</b>
      <div class="acts"><button onclick="saveDriver()">Save this driver</button></div>`;
  },3000);
}
/// What to store for the line, given the candidate that answered.
///
/// Extended discovery tries every profile a driver advertises, so a device can reply at one the
/// driver does not lead with; saving the driver alone then means the next boot configures the
/// driver's first profile and the bus goes quiet. Returns {override:false} rather than null when
/// the match IS the default -- a PATCH without `serial` leaves the stored value alone, so
/// omitting the key made the wizard a one-way ratchet.
async function discoveredSerialOverride(id){
  const cand=((wizReport&&wizReport.candidates)||[]).find(c=>c.driver_id===id);
  const found=cand&&cand.serial_profile;
  if(!found)return null;
  if(!drivers){try{drivers=await getJson('/api/v1/drivers')}catch(e){return null}}
  const drv=((drivers.drivers)||[]).find(d=>d.id===id);
  const def=drv&&(drv.serial_profiles||[])[0];
  if(!def)return null;
  if(def.baud_rate===found.baud_rate&&def.parity===found.parity&&
     def.data_bits===found.data_bits&&def.stop_bits===found.stop_bits)return {override:false};
  return {override:true,baud_rate:found.baud_rate,parity:found.parity,
          data_bits:found.data_bits,stop_bits:found.stop_bits};
}
async function saveDriver(){
  if(!wizChosen){alert('No driver selected.');wizStep=4;paintInverters();return}
  const body={driver:{id:wizChosen}};
  if(Object.keys(wizOptions).length)body.driver.options=wizOptions;
  const serial=await discoveredSerialOverride(wizChosen);
  if(serial)body.serial=serial;
  const r=await patch(body);
  if(!r.ok){alert('Save failed: '+r.why);return}
  wizSavedSerial=serial;wizStep=6;paintInverters();
}

// ---------------- raw bus capture ----------------
let capTimer=null, capture=null;
function captureHtml(){
  return `<div style="margin-top:12px">
    <div class="hint"><b>Polling stops for the whole recording</b>, and the bridge stays silent throughout: it only listens. Make the other end talk while it runs — start the vendor app, plug in the monitoring dongle, or wait for its own poll cycle.</div>
    <label for="cap_baud">Listen at</label>
    <select id="cap_baud" class="sh">${[9600,19200,38400,57600,115200].map(b=>`<option ${b===9600?'selected':''}>${b}</option>`).join('')}</select>
    <div class="hint">The baud rate is part of what you do not know yet. Guess, look at the result, try another: a <b>wrong</b> rate gives plenty of bytes and <b>no valid checksums</b>, which is exactly how you tell. 9600 first — it is by far the most common.</div>
    <label for="cap_par">Parity</label>
    <select id="cap_par" class="sh">${['none','even','odd'].map(p=>`<option>${p}</option>`).join('')}</select>
    <label for="cap_secs">Record for (seconds)</label>
    <input id="cap_secs" class="sh" type="number" value="30" min="1" max="300">
    <div class="acts"><button onclick="startCapture()">Start recording</button></div>
    <div id="cap_msg" class="msg hide"></div><div id="cap_out"></div></div>`;
}
async function startCapture(){
  const q='?seconds='+(Number(val('cap_secs'))||30)+'&baud='+val('cap_baud')+'&parity='+val('cap_par');
  say('#cap_msg','','Starting…');$('#cap_out').innerHTML='';
  const r=await authFetch('/api/v1/actions/capture'+q,{method:'POST'});
  if(!r.ok&&r.status!==202){
    const d=await r.json().catch(()=>({}));
    say('#cap_msg','err',esc((d.error&&d.error.message)||httpWhy(r)));return;
  }
  say('#cap_msg','','Recording… make the other device talk now.');
  if(capTimer)clearInterval(capTimer);
  capTimer=setInterval(pollCapture,1000);
}
async function pollCapture(){
  // The panel can be replaced while a capture runs. Stop rather than throw on a null every
  // second for the rest of the session -- the report is still there to fetch.
  if(!$('#cap_msg')){clearInterval(capTimer);capTimer=null;return}
  let d;try{d=await getJson('/api/v1/capture')}catch(e){return}
  if(d.status==='requested'||d.status==='running'){
    say('#cap_msg','','Recording… '+Math.round((d.elapsed_ms||0)/1000)+' of '+(d.requested_seconds||0)+
      ' s — '+(d.summary?d.summary.bytes:0)+' bytes so far.');
    return;
  }
  clearInterval(capTimer);capTimer=null;
  if(d.status==='failed'){say('#cap_msg','err','Could not record: '+esc(d.error||'unknown'));return}
  if(d.status==='idle')return;
  renderCapture(d);
}
function renderCapture(d){
  capture=d;
  const s=d.summary||{}, ok=(s.modbus_crc_ok||0)+(s.aa55_frames_ok||0);
  // The verdict, stated rather than left to be inferred from a table of hex. Bytes with no valid
  // checksum is a wrong baud rate, which is a different problem from a silent bus -- telling
  // them apart is most of the value of doing this at all.
  if(!s.bytes)say('#cap_msg','err','Nothing at all on the line at '+esc(d.line.baud_rate)+
    ' baud. Either the device said nothing while this ran, or the wiring is wrong (A/B swapped is the usual one).');
  else if(!ok)say('#cap_msg','err',esc(s.bytes)+' bytes, but not one valid checksum. That is what a wrong baud rate looks like — try another from the list. It can also mean a protocol that is neither Modbus RTU nor AA55, which the hex below is still worth keeping.');
  else say('#cap_msg','ok',esc(s.frames)+' frames, '+esc(s.bytes)+' bytes, '+esc(ok)+
    ' with a valid checksum at '+esc(d.line.baud_rate)+' baud. That is the right line speed.');
  $('#cap_out').innerHTML=`
    ${s.truncated?'<div class="msg err" style="display:block">Stopped early — the frame limit was reached. The beginning is kept, which is the part a handshake needs.</div>':''}
    <table><thead><tr><th>Time</th><th>Gap</th><th>Len</th><th>Checksum</th><th>Bytes</th></tr></thead><tbody>
      ${(d.frames||[]).map(f=>`<tr><td class="n">${esc(f.offset_ms)} ms</td><td class="n dim">${esc(f.gap_before_ms)} ms</td>
        <td class="n">${esc(f.length)}</td>
        <td>${f.modbus_crc_ok?'<span style="color:var(--ok)">Modbus</span>':f.aa55_ok?'<span style="color:var(--ok)">AA55</span>':'<span class="dim">—</span>'}</td>
        <td><code style="word-break:break-all">${esc(f.hex)}</code></td></tr>`).join('')}
    </tbody></table>
    <div class="acts"><button onclick="downloadCapture()">Download as a text file</button></div>
    <div class="hint">Attach it to an issue. Note what the device is and what was talking to it while this ran — that context is the part nobody can recover from the bytes.</div>`;
}
function downloadCapture(){
  const d=capture;if(!d)return;
  const s=d.summary||{};
  const lines=['# Heliograph raw RS485 capture',
    '# line: '+d.line.baud_rate+' baud, '+d.line.parity+' parity, '+d.line.data_bits+' data bits, '+d.line.stop_bits+' stop bits',
    '# frame boundaries cut on '+d.line.idle_gap_ms+' ms of silence',
    '# '+s.frames+' frames, '+s.bytes+' bytes, '+(s.modbus_crc_ok||0)+' valid Modbus CRC, '+
      (s.aa55_frames_ok||0)+' valid AA55'+(s.truncated?' (stopped at the frame limit)':''),
    '#','# time_ms  gap_ms  len  checksum  bytes',
    ...(d.frames||[]).map(f=>String(f.offset_ms).padStart(8)+'  '+String(f.gap_before_ms).padStart(6)+'  '+
      String(f.length).padStart(3)+'  '+(f.modbus_crc_ok?'modbus  ':f.aa55_ok?'aa55    ':'-       ')+'  '+f.hex)];
  saveBlob(new Blob([lines.join('\n')+'\n'],{type:'text/plain'}),'heliograph-capture-'+d.line.baud_rate+'.txt');
}
function saveBlob(blob,name){
  const url=URL.createObjectURL(blob);
  const a=document.createElement('a');a.href=url;a.download=name;
  // In the DOM before the click: Firefox ignores a synthetic click on a detached anchor, so a
  // download that works in Chrome does nothing at all there, silently.
  document.body.appendChild(a);a.click();a.remove();
  // Revoked on the next tick: Safari has not started the download when click() returns.
  setTimeout(()=>URL.revokeObjectURL(url),1000);
}

// ---------------- backup and restore ----------------
// Nothing is staged on the bridge between preview and apply, which is why applying re-sends the
// file rather than confirming a token: there is no server-side session to expire, to be raced
// by a second tab, or to apply something other than what was on screen.
let rsPending=null;
async function downloadBackup(){
  const withSecrets=chk('bk_sec');
  say('#bk_msg','','Preparing…');
  const r=await authFetch('/api/v1/config/backup'+(withSecrets?'?secrets=true':''));
  if(!r.ok){say('#bk_msg','err','Could not export: '+esc(httpWhy(r)));return}
  const text=await r.text();
  let name='heliograph-backup.json';
  const cd=(r.headers.get&&r.headers.get('Content-Disposition'))||'';
  const m=cd.match(/filename="([^"]+)"/);
  if(m)name=m[1];
  saveBlob(new Blob([text],{type:'application/json'}),name);
  say('#bk_msg','ok','Downloaded '+esc(name)+(withSecrets?' — it contains passwords in plain text.':' — no passwords in it.'));
}
async function previewRestore(){
  $('#rs_pv').innerHTML='';
  const f=$('#rs_file').files[0];
  if(!f){say('#rs_msg','err','Choose a backup file first.');return}
  let text;try{text=await f.text()}catch(e){say('#rs_msg','err','Could not read that file: '+esc(e.message));return}
  say('#rs_msg','','Checking the file…');
  const r=await authFetch('/api/v1/config/restore?dry_run=true',
    {method:'POST',headers:{'Content-Type':'application/json'},body:text});
  const d=await r.json().catch(()=>({}));
  if(!r.ok){say('#rs_msg','err',esc((d.error&&d.error.message)||httpWhy(r)));return}
  rsPending=text;
  $('#rs_msg').classList.add('hide');
  const b=d.backup||{};
  const src=[b.firmware_version?'written by firmware '+esc(b.firmware_version):'',
    b.exported_at?'on '+esc(String(b.exported_at).replace('T',' ').replace('Z',' UTC')):'',
    b.includes_secrets?'<b>contains passwords</b>':'contains no passwords'].filter(Boolean).join(' · ');
  if(!d.change_count){
    $('#rs_pv').innerHTML=`<div class="msg ok" style="display:block">This backup matches the bridge's current configuration exactly — nothing would change.<div class="hint">${src}</div></div>`;
    return;
  }
  $('#rs_pv').innerHTML=`<div class="hint">${src}</div>
    <table><thead><tr><th>Setting</th><th>Now</th><th>After restore</th></tr></thead><tbody>
    ${(d.changes||[]).map(c=>`<tr><td>${esc(c.field)}</td><td class="dim">${esc(c.before)}</td><td><b>${esc(c.after)}</b></td></tr>`).join('')}
    </tbody></table>
    <div class="hint">${esc(d.change_count)} setting(s) would change.${
      d.reboot_required?' The bridge <b>restarts</b> afterwards — some of these only take effect at boot.':''}${
      d.rollback_exists?' You already have an undo point from an earlier restore; this <b>replaces</b> it, so undoing afterwards comes back to the configuration the bridge has right now.':''}</div>
    <div class="acts"><button onclick="applyRestore()">Apply these ${esc(d.change_count)} change(s)</button></div>`;
}
async function applyRestore(){
  if(rsPending===null){say('#rs_msg','err','Preview the file again before applying.');return}
  $('#rs_pv').innerHTML='';
  say('#rs_msg','','Applying…');
  const r=await authFetch('/api/v1/config/restore',
    {method:'POST',headers:{'Content-Type':'application/json'},body:rsPending});
  const d=await r.json().catch(()=>({}));
  if(!r.ok){say('#rs_msg','err',esc((d.error&&d.error.message)||httpWhy(r)));return}
  rsPending=null;
  say('#rs_msg','ok','Restored '+esc(d.changed_fields||0)+' setting(s).'+
    (d.reboot_required?' The bridge is restarting — reload this page in ~30 seconds.':' No restart needed.')+
    (d.rollback_stored?' You can undo this below.':' No undo was stored — the flash was full.'));
}
async function undoRestore(){
  if(!confirm('Go back to the configuration this bridge had before the last restore?'))return;
  say('#bk_msg','','Rolling back…');
  const r=await authFetch('/api/v1/actions/undo-restore',{method:'POST'});
  const d=await r.json().catch(()=>({}));
  if(!r.ok){say('#bk_msg','err',esc((d.error&&d.error.message)||httpWhy(r)));return}
  say('#bk_msg','ok','Rolled back.'+(d.rebooting?' The bridge is restarting — reload in ~30 seconds.':' No restart needed.')+
    ' Pressing this again returns to the restored configuration.');
}

// ---------------- firmware update ----------------
// The check runs HERE, in your browser, never on the bridge -- which is what keeps "works with
// no internet at all" true for the device. The feed comes from GitHub Pages rather than the
// release assets, because release downloads redirect to a host that sends no CORS headers.
const UPDATE_FEED='https://timdebruijn.github.io/heliograph/';
let updLatest=null;
async function updatesEnabled(){
  if(window.__updEnabled!==undefined)return window.__updEnabled;
  try{const c=cfg||await getJson('/api/v1/config');window.__updEnabled=!(c.updates&&c.updates.check_enabled===false)}
  catch(e){window.__updEnabled=false}
  return window.__updEnabled;
}
// Not a string compare, and that is the whole point: "0.9.0" sorts ABOVE "0.14.0" as text, so a
// naive version nags forever about a downgrade. Checked by tools/check_web_js.py in node.
function semver(text){
  const m=/^\s*v?(\d{1,5})\.(\d{1,5})\.(\d{1,5})(?!\d|\.\d)/.exec(String(text||''));
  return m?[+m[1],+m[2],+m[3]]:null;
}
function isNewer(current,candidate){
  const a=semver(current), b=semver(candidate);
  if(!a||!b)return false;
  for(let i=0;i<3;i++){if(b[i]!==a[i])return b[i]>a[i]}
  return false;
}
/// Silent on every failure: no internet, a captive portal, a blocked domain and a rewritten feed
/// all mean "say nothing", never an error on a page whose job is showing solar production.
async function checkForUpdate(manual){
  if(!S||!S.bridge)return null;
  try{
    const r=await fetch(UPDATE_FEED+'latest.json',{cache:'no-store'});
    if(!r.ok)throw new Error('HTTP '+r.status);
    const d=await r.json();
    if(!semver(d.version))throw new Error('unrecognised feed');
    updLatest=d;
  }catch(e){updLatest=null;return manual?{error:e.message}:null}
  const newer=isNewer(S.bridge.firmware_version,updLatest.version);
  const badge=$('#updbadge');
  if(badge)badge.classList.toggle('hide',!newer);
  return {newer,latest:updLatest.version};
}
async function gotoUpdate(){
  goTab('bridge');
  const c=$('#updcard');
  if(c)window.scrollTo({top:Math.max(0,c.getBoundingClientRect().top+window.scrollY-80),behavior:'smooth'});
}
function updRender(){
  const box=$('#updbox');if(!box)return;
  const cur=(S&&S.bridge&&S.bridge.firmware_version)||'unknown';
  if(!updLatest){
    box.innerHTML=`<div class="hint">Running <b>${esc(cur)}</b>. No release information — either the check is off, or this browser could not reach github.io.</div>`;
    return;
  }
  const board=(S&&S.bridge&&S.bridge.board_id)||'';
  const asset=(updLatest.boards||{})[board];
  if(!isNewer(cur,updLatest.version)){
    box.innerHTML=`<div class="hint">Running <b>${esc(cur)}</b>, which is the latest release (<b>${esc(updLatest.version)}</b>).</div>`;
    return;
  }
  if(!asset){
    // A release carrying no image for this board says so, rather than offering a button that
    // would hand the bridge somebody else's firmware.
    box.innerHTML=`<div class="msg err" style="display:block">Release <b>${esc(updLatest.version)}</b> is available but carries no image for this board (<code>${esc(board||'unknown')}</code>). Nothing to install from here.</div>`;
    return;
  }
  box.innerHTML=`<div class="msg ok" style="display:block"><b>${esc(updLatest.version)}</b> is available. You are running ${esc(cur)}.
      ${updLatest.notes_url?`<a href="${esc(updLatest.notes_url)}" target="_blank" rel="noopener noreferrer">Release notes</a>`:''}</div>
    <div class="hint">${Math.round((asset.size||0)/1024)} kB. Your browser downloads it and hands it to the bridge, which checks it against the checksum from the release before writing anything. That proves the image arrived intact — it is not a signature, and does not prove who built it.</div>
    <div class="acts"><button id="upd_btn" onclick="installUpdate()">Install ${esc(updLatest.version)}</button></div>
    <div id="upd_prog" class="prog"><div></div></div>
    <div id="upd_msg" class="msg hide"></div>`;
}
async function manualCheck(){
  const box=$('#updbox');
  if(box)box.innerHTML='<div class="hint">Checking…</div>';
  const r=await checkForUpdate(true);
  if(r&&r.error){
    box.innerHTML=`<div class="msg err" style="display:block">Could not reach the release feed: ${esc(r.error)}. This browser needs internet access for the check; the bridge itself never makes this request.</div>`;
    return;
  }
  updRender();
}
// XMLHttpRequest, not fetch: fetch cannot report UPLOAD progress, and a minute of silent
// "uploading…" reads as a hang.
function upload(url,blob,filename,btnSel,progSel,msgSel,done){
  const btn=$(btnSel), pb=$(progSel), pf=pb&&pb.firstElementChild;
  const send=mayRetry=>{
    const x=new XMLHttpRequest();
    x.open('POST',url);
    x.setRequestHeader('Authorization','Basic '+sessionStorage.getItem('hg_auth'));
    x.upload.onprogress=e=>{
      if(!e.lengthComputable)return;
      const p=Math.round(e.loaded/e.total*100);
      if(pf)pf.style.width=p+'%';
      say(msgSel,'',p<100?'Uploading to the bridge… '+p+'%':'Upload complete — verifying the checksum and writing flash…');
    };
    x.onload=()=>{
      if(x.status===401&&mayRetry){
        clearAuth();
        askAuth(true).then(ok=>{if(ok)send(false);else{if(btn)btn.disabled=false;say(msgSel,'err','Cancelled.')}});
        return;
      }
      let d={};try{d=JSON.parse(x.responseText)}catch(e){}
      if(pb)pb.style.display='none';
      if(x.status<200||x.status>=300){
        if(btn)btn.disabled=false;
        say(msgSel,'err','Refused: '+esc((d.error&&d.error.message)||('HTTP '+x.status))+' — the running firmware is untouched.');
        return;
      }
      say(msgSel,'ok',done);
    };
    x.onerror=()=>{if(pb)pb.style.display='none';if(btn)btn.disabled=false;
      say(msgSel,'err','Upload failed: network error — the running firmware is untouched.')};
    say(msgSel,'','Uploading to the bridge… 0%');
    if(pb){pb.style.display='block';pf.style.width='0%'}
    if(btn)btn.disabled=true;
    // FormData: the browser sets the multipart boundary itself; setting Content-Type by hand
    // here breaks the upload.
    const fd=new FormData();fd.append('firmware',blob,filename);
    x.send(fd);
  };
  send(true);
}
async function otaUpload(){
  const f=$('#ota_file').files[0];
  if(!f){say('#ota_msg','err','Choose a firmware .bin first.');return}
  if(!sessionStorage.getItem('hg_auth')&&!await askAuth(false)){say('#ota_msg','err','Cancelled.');return}
  upload('/api/v1/ota',f,f.name,'#ota_btn','#ota_prog','#ota_msg',
    'Verified and installed. Rebooting into the new firmware — reload this page in ~15 seconds.');
}
async function installUpdate(){
  const board=(S&&S.bridge&&S.bridge.board_id)||'';
  const asset=(updLatest&&(updLatest.boards||{})[board])||null;
  if(!asset)return;
  say('#upd_msg','','Downloading '+esc(asset.file)+'…');
  $('#upd_btn').disabled=true;
  let blob;
  try{
    const r=await fetch(UPDATE_FEED+asset.file,{cache:'no-store'});
    if(!r.ok)throw new Error('HTTP '+r.status);
    blob=await r.blob();
  }catch(e){
    $('#upd_btn').disabled=false;
    say('#upd_msg','err','Could not download the image: '+esc(e.message)+' — nothing was sent to the bridge.');
    return;
  }
  // Checked here even though the firmware hashes it: it costs nothing and turns "the firmware
  // refused it" into "the download was truncated", which is a different problem with a
  // different fix.
  if(asset.size&&blob.size!==asset.size){
    $('#upd_btn').disabled=false;
    say('#upd_msg','err','Download is '+blob.size+' bytes, expected '+asset.size+' — truncated. Nothing was sent to the bridge.');
    return;
  }
  if(!sessionStorage.getItem('hg_auth')&&!await askAuth(false)){
    $('#upd_btn').disabled=false;say('#upd_msg','err','Cancelled.');return;
  }
  // board and sha256 both travel: the firmware refuses an image built for another board, and one
  // that does not hash to what the release promised -- before the boot partition flips.
  upload('/api/v1/ota?board='+encodeURIComponent(board)+'&sha256='+encodeURIComponent(asset.sha256||''),
    blob,asset.file,'#upd_btn','#upd_prog','#upd_msg',
    'Verified and installed. Rebooting into '+esc(updLatest.version)+' — reload this page in ~15 seconds.');
}

// ---------------- timezones ----------------
// [city, POSIX TZ, IANA name] per region. GENERATED from the IANA tzdata -- do not hand-edit.
// The firmware only ever receives the POSIX string; the IANA name is stored alongside purely so
// this dropdown can re-select the exact city that was picked (many cities share one POSIX
// string).
const TZ={"Europe":[["Amsterdam","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Amsterdam"],["Andorra","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Andorra"],["Astrakhan","<+04>-4","Europe/Astrakhan"],["Athens","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Athens"],["Belfast","GMT0BST,M3.5.0/1,M10.5.0","Europe/Belfast"],["Belgrade","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Belgrade"],["Berlin","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Berlin"],["Bratislava","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Bratislava"],["Brussels","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Brussels"],["Bucharest","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Bucharest"],["Budapest","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Budapest"],["Busingen","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Busingen"],["Chisinau","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Chisinau"],["Copenhagen","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Copenhagen"],["Dublin","GMT0IST,M3.5.0/1,M10.5.0","Europe/Dublin"],["Gibraltar","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Gibraltar"],["Guernsey","GMT0BST,M3.5.0/1,M10.5.0","Europe/Guernsey"],["Helsinki","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Helsinki"],["Isle of Man","GMT0BST,M3.5.0/1,M10.5.0","Europe/Isle_of_Man"],["Istanbul","<+03>-3","Europe/Istanbul"],["Jersey","GMT0BST,M3.5.0/1,M10.5.0","Europe/Jersey"],["Kaliningrad","EET-2","Europe/Kaliningrad"],["Kiev","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Kiev"],["Kirov","MSK-3","Europe/Kirov"],["Kyiv","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Kyiv"],["Lisbon","WET0WEST,M3.5.0/1,M10.5.0","Europe/Lisbon"],["Ljubljana","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Ljubljana"],["London","GMT0BST,M3.5.0/1,M10.5.0","Europe/London"],["Luxembourg","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Luxembourg"],["Madrid","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Madrid"],["Malta","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Malta"],["Mariehamn","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Mariehamn"],["Minsk","<+03>-3","Europe/Minsk"],["Monaco","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Monaco"],["Moscow","MSK-3","Europe/Moscow"],["Nicosia","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Nicosia"],["Oslo","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Oslo"],["Paris","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Paris"],["Podgorica","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Podgorica"],["Prague","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Prague"],["Riga","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Riga"],["Rome","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Rome"],["Samara","<+04>-4","Europe/Samara"],["San Marino","CET-1CEST,M3.5.0,M10.5.0/3","Europe/San_Marino"],["Sarajevo","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Sarajevo"],["Saratov","<+04>-4","Europe/Saratov"],["Simferopol","MSK-3","Europe/Simferopol"],["Skopje","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Skopje"],["Sofia","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Sofia"],["Stockholm","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Stockholm"],["Tallinn","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Tallinn"],["Tirane","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Tirane"],["Tiraspol","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Tiraspol"],["Ulyanovsk","<+04>-4","Europe/Ulyanovsk"],["Uzhgorod","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Uzhgorod"],["Vaduz","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Vaduz"],["Vatican","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Vatican"],["Vienna","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Vienna"],["Vilnius","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Vilnius"],["Volgograd","MSK-3","Europe/Volgograd"],["Warsaw","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Warsaw"],["Zagreb","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Zagreb"],["Zaporozhye","EET-2EEST,M3.5.0/3,M10.5.0/4","Europe/Zaporozhye"],["Zurich","CET-1CEST,M3.5.0,M10.5.0/3","Europe/Zurich"]],"America":[["Anchorage","AKST9AKDT,M3.2.0,M11.1.0","America/Anchorage"],["Argentina/Buenos Aires","<-03>3","America/Argentina/Buenos_Aires"],["Bogota","<-05>5","America/Bogota"],["Caracas","<-04>4","America/Caracas"],["Chicago","CST6CDT,M3.2.0,M11.1.0","America/Chicago"],["Denver","MST7MDT,M3.2.0,M11.1.0","America/Denver"],["Halifax","AST4ADT,M3.2.0,M11.1.0","America/Halifax"],["Havana","CST5CDT,M3.2.0/0,M11.1.0/1","America/Havana"],["Lima","<-05>5","America/Lima"],["Los Angeles","PST8PDT,M3.2.0,M11.1.0","America/Los_Angeles"],["Mexico City","CST6","America/Mexico_City"],["New York","EST5EDT,M3.2.0,M11.1.0","America/New_York"],["Phoenix","MST7","America/Phoenix"],["Santiago","<-04>4<-03>,M9.1.6/24,M4.1.6/24","America/Santiago"],["Sao Paulo","<-03>3","America/Sao_Paulo"],["St Johns","NST3:30NDT,M3.2.0,M11.1.0","America/St_Johns"],["Toronto","EST5EDT,M3.2.0,M11.1.0","America/Toronto"],["Vancouver","MST7","America/Vancouver"]],"Asia":[["Almaty","<+05>-5","Asia/Almaty"],["Baghdad","<+03>-3","Asia/Baghdad"],["Baku","<+04>-4","Asia/Baku"],["Bangkok","<+07>-7","Asia/Bangkok"],["Dhaka","<+06>-6","Asia/Dhaka"],["Dubai","<+04>-4","Asia/Dubai"],["Ho Chi Minh","<+07>-7","Asia/Ho_Chi_Minh"],["Hong Kong","HKT-8","Asia/Hong_Kong"],["Jakarta","WIB-7","Asia/Jakarta"],["Jerusalem","IST-2IDT,M3.4.4/26,M10.5.0","Asia/Jerusalem"],["Kabul","<+0430>-4:30","Asia/Kabul"],["Karachi","PKT-5","Asia/Karachi"],["Kathmandu","<+0545>-5:45","Asia/Kathmandu"],["Kolkata","IST-5:30","Asia/Kolkata"],["Kuala Lumpur","<+08>-8","Asia/Kuala_Lumpur"],["Manila","PST-8","Asia/Manila"],["Riyadh","<+03>-3","Asia/Riyadh"],["Seoul","KST-9","Asia/Seoul"],["Shanghai","CST-8","Asia/Shanghai"],["Singapore","<+08>-8","Asia/Singapore"],["Taipei","CST-8","Asia/Taipei"],["Tashkent","<+05>-5","Asia/Tashkent"],["Tbilisi","<+04>-4","Asia/Tbilisi"],["Tehran","<+0330>-3:30","Asia/Tehran"],["Tokyo","JST-9","Asia/Tokyo"],["Yangon","<+0630>-6:30","Asia/Yangon"]],"Africa":[["Abidjan","GMT0","Africa/Abidjan"],["Accra","GMT0","Africa/Accra"],["Addis Ababa","EAT-3","Africa/Addis_Ababa"],["Algiers","CET-1","Africa/Algiers"],["Cairo","EET-2EEST,M4.5.5/0,M10.5.4/24","Africa/Cairo"],["Casablanca","XXX-2<+01>-1,0/0,J365/23","Africa/Casablanca"],["Johannesburg","SAST-2","Africa/Johannesburg"],["Lagos","WAT-1","Africa/Lagos"],["Nairobi","EAT-3","Africa/Nairobi"],["Tunis","CET-1","Africa/Tunis"]],"Australia":[["Adelaide","ACST-9:30ACDT,M10.1.0,M4.1.0/3","Australia/Adelaide"],["Brisbane","AEST-10","Australia/Brisbane"],["Darwin","ACST-9:30","Australia/Darwin"],["Hobart","AEST-10AEDT,M10.1.0,M4.1.0/3","Australia/Hobart"],["Melbourne","AEST-10AEDT,M10.1.0,M4.1.0/3","Australia/Melbourne"],["Perth","AWST-8","Australia/Perth"],["Sydney","AEST-10AEDT,M10.1.0,M4.1.0/3","Australia/Sydney"]],"Pacific":[["Auckland","NZST-12NZDT,M9.5.0,M4.1.0/3","Pacific/Auckland"],["Fiji","<+12>-12","Pacific/Fiji"],["Guam","ChST-10","Pacific/Guam"],["Honolulu","HST10","Pacific/Honolulu"],["Tahiti","<-10>10","Pacific/Tahiti"]],"Atlantic":[["Azores","<-01>1<+00>,M3.5.0/0,M10.5.0/1","Atlantic/Azores"],["Canary","WET0WEST,M3.5.0/1,M10.5.0","Atlantic/Canary"],["Cape Verde","<-01>1","Atlantic/Cape_Verde"],["Reykjavik","GMT0","Atlantic/Reykjavik"]],"Indian":[["Maldives","<+05>-5","Indian/Maldives"],["Mauritius","<+04>-4","Indian/Mauritius"],["Reunion","<+04>-4","Indian/Reunion"]],"UTC":[["UTC","UTC0","UTC"]]};
const TZBYNAME={};for(const g in TZ)for(const [city,posix,name] of TZ[g])TZBYNAME[name]=posix;
function tzKnown(ntp){
  if(ntp.timezone_name&&TZBYNAME[ntp.timezone_name]===ntp.timezone)return ntp.timezone_name;
  for(const g in TZ)for(const e of TZ[g])if(e[1]===ntp.timezone)return e[2];
  return null;
}
function tzOptions(ntp){
  const sel=tzKnown(ntp);
  let h='';
  for(const g in TZ){
    h+=`<optgroup label="${esc(g)}">`+TZ[g].map(([city,posix,name])=>
      `<option value="${esc(name)}" ${name===sel?'selected':''}>${esc(city)}</option>`).join('')+'</optgroup>';
  }
  return h+`<option value="__custom" ${sel===null?'selected':''}>Custom (POSIX string)…</option>`;
}

// ---------------- paint and refresh ----------------
function paint(){
  paintChrome();
  if(tab==='live')paintLive();
  else if(tab==='inv')paintInverters();
  else if(tab==='int')paintInt();
  else if(tab==='health')paintHealth();
  else if(tab==='bridge')paintBridge();
}
async function refresh(){
  try{
    S=await getJson('/api/v1/status');
    // One request feeds the whole Live tab: /status already carries devices[] and totals, so
    // the fleet rows need no per-device fetch. The sparkline series come from here too.
    const tot=S.totals||{}, fleet=S.devices||[];
    // Same question, same answer as the headline above it: see isFleet().
    if(isFleet(S))record('all',tot.ac_power_w);
    else record('all',(S.measurements&&S.measurements['ac.power.total']||{}).value);
    for(const f of fleet)record('d:'+f.id,f.ac_power_w);
    saveHist();
    if(!cfg){try{cfg=await getJson('/api/v1/config')}catch(e){}}
    // Unauthenticated, and every tab shows something from it, so it is fetched once up front
    // rather than only on the Health tab.
    if(!diag)await loadDiag();
    if(!drivers&&(tab==='inv'))
      {try{drivers=await getJson('/api/v1/drivers')}catch(e){}}
    // Once per page load, and only when it is switched on. Fired here because this is the first
    // moment the running version is known.
    if(!window.__updChecked){
      window.__updChecked=true;
      updatesEnabled().then(on=>{if(on)checkForUpdate(false).then(()=>{if(tab==='bridge')updRender()})});
    }
    if(tab==='health'){await loadDiag();paint();if(!logPaused)loadLogs(false);return}
    if(tab==='inv')loadInverters(false);
    paint();
  }catch(e){
    const el=$('#banner');
    el.textContent='Cannot reach the bridge: '+e.message;
    el.classList.remove('hide');
  }
}

// SSE is an optimisation, not the transport: if it drops, the interval below keeps the page
// live. And while it IS alive the interval backs off to a heartbeat -- the old fixed 5 s poll
// ran on top of an event stream emitting up to once a second, on the board that is also driving
// the RS485 bus.
let es=null, lastEvent=0;
function connect(){
  try{
    es=new EventSource('/api/v1/events');
    es.addEventListener('state',()=>{lastEvent=Date.now();refresh()});
    es.onerror=()=>{es.close();setTimeout(connect,10000)};
  }catch(e){}
}
refresh();connect();
setInterval(()=>{if(Date.now()-lastEvent>15000)refresh()},5000);
</script></body></html>
)HTML";

}  // namespace heliograph::web
