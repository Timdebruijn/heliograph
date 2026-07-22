// SPDX-License-Identifier: MIT
//
// The setup portal, served on the Heliograph-Setup-XXXX access point.
//
// Separate from index.html on purpose: at this point there is no network, no device and no
// data -- showing a dashboard full of dashes would only confuse. This page has exactly one
// job, and it says so.

#pragma once

#include <pgmspace.h>

namespace heliograph::web {

inline const char kSetupHtml[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Heliograph setup</title><style>
:root{--bg:#0f1115;--card:#181b22;--fg:#e6e8ec;--dim:#8b93a3;--ok:#3fb950;--bad:#f85149;--line:#262b36}
@media(prefers-color-scheme:light){:root{--bg:#f6f7f9;--card:#fff;--fg:#1a1d23;--dim:#5b6472;--line:#e3e6ea}}
*{box-sizing:border-box}body{margin:0;padding:24px;font:15px/1.55 system-ui,-apple-system,sans-serif;
background:var(--bg);color:var(--fg);display:flex;justify-content:center}
.w{max-width:420px;width:100%}
h1{font-size:19px;margin:0 0 4px}
p.sub{color:var(--dim);margin:0 0 20px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:18px;margin-bottom:14px}
label{display:block;font-size:13px;color:var(--dim);margin:12px 0 4px}
input,select{width:100%;padding:9px 10px;border-radius:8px;border:1px solid var(--line);
background:var(--bg);color:var(--fg);font:inherit}
button{width:100%;margin-top:18px;padding:11px;border:0;border-radius:8px;background:#2f81f7;
color:#fff;font:inherit;font-weight:600;cursor:pointer}
button:disabled{opacity:.5;cursor:default}
.msg{padding:10px;border-radius:8px;margin-top:12px;display:none}
.msg.err{background:#f8514922;border:1px solid var(--bad);display:block}
.msg.ok{background:#3fb95022;border:1px solid var(--ok);display:block}
.hint{font-size:12px;color:var(--dim);margin-top:6px}
</style></head><body><div class="w">
<h1>Heliograph setup</h1>
<p class="sub">Connect this bridge to your network.</p>

<form id="f" class="card">
  <label for="ssid">WiFi network</label>
  <select id="ssid"><option value="">scanning…</option></select>
  <div class="hint">Not listed? Pick “Other…” and type the name.</div>

  <label for="ssid2" id="ssid2l" style="display:none">Network name</label>
  <input id="ssid2" style="display:none" autocomplete="off">

  <label for="pw">WiFi password</label>
  <input id="pw" type="password" autocomplete="new-password">

  <label for="admin">Admin password</label>
  <input id="admin" type="password" autocomplete="new-password">
  <div class="hint">Required. Protects settings, OTA and reboot. There is no default.</div>

  <label for="admin2">Admin password (again)</label>
  <input id="admin2" type="password" autocomplete="new-password">
  <div class="hint">The board has no reset button — a typo here means recovery over USB.
  Type it twice.</div>

  <button id="b" type="submit">Save and restart</button>
  <div id="m" class="msg"></div>
</form>

<div class="card">
  <p class="sub" style="margin:0">After saving, the bridge restarts and joins your network.
  This setup network disappears. Find it again by its hostname, or via your router.</p>
</div>
</div>
<script>
const $=s=>document.getElementById(s);
const msg=(t,ok)=>{const m=$('m');m.textContent=t;m.className='msg '+(ok?'ok':'err')};

fetch('/api/v1/wifi/scan').then(r=>r.json()).then(d=>{
  const s=$('ssid');s.innerHTML='';
  // Strongest first: the network the user wants is almost always the loudest one.
  (d.networks||[]).sort((a,b)=>b.rssi-a.rssi).forEach(n=>{
    const o=document.createElement('option');
    o.value=n.ssid;o.textContent=`${n.ssid}  (${n.rssi} dBm)${n.open?' — open':''}`;
    s.appendChild(o);
  });
  const o=document.createElement('option');o.value='__other__';o.textContent='Other…';
  s.appendChild(o);
}).catch(()=>{
  // A failed scan must not block setup: the field falls back to free text.
  $('ssid').innerHTML='<option value="__other__">Other…</option>';
});

$('ssid').onchange=e=>{
  const other=e.target.value==='__other__';
  $('ssid2').style.display=other?'block':'none';
  $('ssid2l').style.display=other?'block':'none';
};

$('f').onsubmit=async e=>{
  e.preventDefault();
  const ssid=$('ssid').value==='__other__'?$('ssid2').value.trim():$('ssid').value;
  if(!ssid){msg('Pick or type a network name.');return}
  if(!$('admin').value){msg('An admin password is required.');return}
  // A typo'd admin password can only be recovered over USB (no reset button), so it is
  // the one field worth the friction of typing twice.
  if($('admin').value!==$('admin2').value){msg('The admin passwords do not match.');return}
  $('b').disabled=true;msg('Saving…',true);
  try{
    const r=await fetch('/api/v1/provision',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({wifi:{ssid,password:$('pw').value},
                           security:{admin_password:$('admin').value}})});
    const d=await r.json();
    if(!r.ok)throw new Error(d.error?d.error.message:('HTTP '+r.status));
    // The concrete next step, not "find it via your router": this AP is about to vanish
    // and the user needs an address to type on the network they are returning to.
    const host=(d.hostname||'heliograph')+'.local';
    const m=$('m');m.className='msg ok';
    m.innerHTML='Saved — restarting.<br>Reconnect to your own WiFi, then open '+
      '<b>http://'+host+'</b> (give it ~30 seconds).';
  }catch(err){msg(err.message);$('b').disabled=false}
};
</script></body></html>)HTML";

}  // namespace heliograph::web
