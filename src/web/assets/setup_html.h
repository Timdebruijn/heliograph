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
  <div class="hint" id="pwhint" style="display:none">Leave empty to keep the password already
  stored for this network.</div>

  <label for="admin">Admin password</label>
  <input id="admin" type="password" autocomplete="new-password">
  <div class="hint">Required. Protects settings, OTA and reboot. There is no default.</div>

  <label for="admin2">Admin password (again)</label>
  <input id="admin2" type="password" autocomplete="new-password">
  <div class="hint">Type it twice — a typo here means holding BOOT for 5 seconds to factory-reset,
  or re-flashing over USB.</div>

  <!-- Shown only when this bridge already has an admin password, i.e. the setup network came
       back after a WiFi outage rather than on first boot. Then the new-password fields above are
       hidden and this one authenticates the change instead. -->
  <div id="reauth" style="display:none">
    <label for="cur">Admin password</label>
    <input id="cur" type="password" autocomplete="current-password">
    <div class="hint">This bridge is already set up, so changing its network needs the admin
    password you chose. Forgotten it? Hold <b>BOOT</b> for ~5 seconds while the board is running
    to erase the configuration and start over.</div>
  </div>

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

// Two modes. First boot: no admin password exists, provisioning is open, and the form sets one.
// Reconfigure: this setup network came back because the bridge lost WiFi (a router reboot is
// enough), a password already exists, and provisioning demands it -- so the form authenticates
// and changes only the network. Detected up front rather than on a failed submit, because a 401
// from requestAuthentication() has an empty body and the old code parsed it as JSON.
let reauth=false, adminUser='admin', modeKnown=false;
const setupReady=fetch('/api/v1/config').then(r=>r.json()).then(c=>{
  const sec=(c&&c.security)||{};
  adminUser=sec.admin_username||'admin';
  reauth=!!sec.password_set;
  modeKnown=true;
  if(reauth){
    $('pwhint').style.display='block';
    $('reauth').style.display='block';
    // The existing password stays as it is; this flow only moves the bridge to a new network.
    for(const id of ['admin','admin2']){const el=$(id);el.style.display='none';
      const lab=document.querySelector('label[for="'+id+'"]');if(lab)lab.style.display='none';
      if(el.nextElementSibling&&el.nextElementSibling.className==='hint')
        el.nextElementSibling.style.display='none';}
    $('b').textContent='Save network and restart';
  }
}).catch(()=>{
  // Which mode we are in decides whether the form must carry a new admin password or an
  // existing one. Guessing wrong means submitting a body the firmware refuses and then blaming
  // the user's password for it, so the form stays shut until this is actually known.
  msg('Could not read this bridge’s state. Reload the page to try again.');
  $('b').disabled=true;
});

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
  // A failed scan must not block setup: the field falls back to free text, and the text box is
  // revealed straight away rather than hidden behind discovering the "Other…" entry. This is the
  // normal path in reconfigure mode, where the scan is admin-gated like provisioning itself.
  $('ssid').innerHTML='<option value="__other__">Other…</option>';
  $('ssid2').style.display='block';$('ssid2l').style.display='block';
});

$('ssid').onchange=e=>{
  const other=e.target.value==='__other__';
  $('ssid2').style.display=other?'block':'none';
  $('ssid2l').style.display=other?'block':'none';
};

$('f').onsubmit=async e=>{
  e.preventDefault();
  // Disabled first, before any await: two quick taps while the mode lookup is still in flight
  // would otherwise fire two provisions.
  $('b').disabled=true;
  // Which mode we are in decides what the form must contain. Bounded, because a web task busy
  // with a WiFi scan can leave this pending for seconds and a dead-looking button with no
  // message is worse than an honest one.
  await Promise.race([setupReady, new Promise(r=>setTimeout(r,3000))]);
  if(!modeKnown){
    $('b').disabled=false;
    msg('Could not read this bridge’s state. Reload the page to try again.');
    return;
  }
  // Every rejection has to hand the button back, or the form is dead until a reload.
  const refuse=t=>{$('b').disabled=false;msg(t)};
  const ssid=$('ssid').value==='__other__'?$('ssid2').value.trim():$('ssid').value;
  if(!ssid){refuse('Pick or type a network name.');return}
  if(reauth){
    if(!$('cur').value){refuse('Enter the admin password for this bridge.');return}
  }else{
    if(!$('admin').value){refuse('An admin password is required.');return}
    // A typo'd admin password costs a factory reset, so it is the one field worth the
    // friction of typing twice.
    if($('admin').value!==$('admin2').value){refuse('The admin passwords do not match.');return}
  }
  msg('Saving…',true);
  try{
    const headers={'Content-Type':'application/json'};
    const body={wifi:{ssid}};
    // An absent password leaves the stored one alone; an empty string SETS it to empty. In
    // reconfigure mode the field starts blank while a password is already stored, so sending it
    // unconditionally wiped the credential of anyone reconnecting to the same network -- after
    // which the bridge could never join anything again and sat in the portal forever. On first
    // boot there is nothing to preserve, so an open network still sends the empty value.
    if(!reauth||$('pw').value!=='')body.wifi.password=$('pw').value;
    if(reauth){
      // Basic auth by hand: fetch() will not surface the browser's credential dialog reliably,
      // least of all in the captive-portal mini-browser this page is usually opened in.
      //
      // Always UTF-8. btoa() takes a string of code units and only throws above U+00FF, so a
      // plain btoa() silently emits LATIN-1 for exactly the range that matters here -- é, ë, ü,
      // ö, ç. The firmware compares against the bytes it stored from the setup POST, which are
      // UTF-8, so those passwords would never match and the owner would be locked out of their
      // own recovery with "password not accepted" (review, 2026-07-25).
      const raw=adminUser+':'+$('cur').value;
      headers['Authorization']='Basic '+btoa(String.fromCharCode(...new TextEncoder().encode(raw)));
    }else{
      body.security={admin_password:$('admin').value};
    }
    const r=await fetch('/api/v1/provision',{method:'POST',headers,body:JSON.stringify(body)});
    if(r.status===401){
      refuse('That admin password was not accepted. Forgotten it? Hold BOOT for ~5 seconds '+
             'while the board is running to factory-reset it.');
      return;
    }
    // Tolerate a missing or non-JSON body on ANY status, not just 401. requestAuthentication()
    // answers empty, and a captive-portal interceptor or a truncated response can do the same
    // on other codes -- json() then throws "Unexpected end of JSON input", which is exactly the
    // useless message this rewrite set out to remove rather than relocate.
    const d=await r.json().catch(()=>({}));
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
