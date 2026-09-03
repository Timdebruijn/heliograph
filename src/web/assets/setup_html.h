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
<title>Heliograph setup</title>
<!-- The icon ships INSIDE the page rather than as /favicon.ico, which is why there is no
     route for it. A browser asks for /favicon.ico on its own unless the document names an
     icon, and that request was reaching a server with no handler for it: a 404 in every
     visitor's console, on every load, for a file that was never going to exist.
     A data: URI costs one line in a page that is gzipped into flash, and no second request
     to a web task that serves one client at a time. SVG so it stays sharp at any size
     without shipping several rasters.
     Square linecaps, not round: at 16 px the four DIAGONAL rays do not land on the pixel
     grid, and rounded ends anti-alias them into pale smudges while the cardinal four stay
     crisp -- a lopsided blur rather than a sun. Judged by rasterising at 16 px and looking
     at the pixels, which is not what magnifying the SVG shows: vector art scales cleanly
     and hides exactly this. The amber matches the dashboard's --warn; this page's palette has no
     warning colour of its own, so it is written out here as a literal. -->
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'%3E%3Ccircle cx='8' cy='8' r='3.4' fill='%23d29922'/%3E%3Cg stroke='%23d29922' stroke-width='2' stroke-linecap='butt'%3E%3Cpath d='M8 1v2M8 13v2M1 8h2M13 8h2M3.05 3.05l1.4 1.4M11.55 11.55l1.4 1.4M12.95 3.05l-1.4 1.4M4.45 11.55l-1.4 1.4'/%3E%3C/g%3E%3C/svg%3E"><style>
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
/* Only the restore preview uses a table here. Same rules as the dashboard's, so the two pages
   still read as one product. */
table{width:100%;border-collapse:collapse;font-size:13px;margin-top:10px}
td,th{text-align:left;padding:6px 6px;border-bottom:1px solid var(--line);vertical-align:top;
  word-break:break-word}
th{color:var(--dim);font-weight:500;font-size:11px;text-transform:uppercase}
tr:last-child td{border-bottom:0}
button.alt{background:none;border:1px solid var(--line);color:var(--fg)}
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
    <!-- Typed, not fetched. The username used to come from GET /api/v1/config, which is
         unauthenticated -- so this page's convenience was the reason every LAN reader was
         handed half of the bridge's login. Defaults to the factory value. -->
    <label for="curuser">Admin username</label>
    <input id="curuser" autocomplete="username" autocapitalize="none" autocorrect="off"
           spellcheck="false" value="admin">
    <label for="cur">Admin password</label>
    <input id="cur" type="password" autocomplete="current-password">
    <div class="hint">This bridge is already set up, so changing its network needs the admin
    password you chose. Forgotten it? Hold <b>BOOT</b> for ~5 seconds while the board is running
    to erase the configuration and start over.</div>
  </div>

  <button id="b" type="submit">Save and restart</button>
  <div id="m" class="msg"></div>
</form>

<!-- The other way in. A board that has just been factory-reset, or a replacement board for one
     that died, does not need its WiFi typed and then twenty settings re-entered by hand: the
     backup carries all of them. This is the context that makes the feature worth having, which
     is why it is on the setup page and not only in Settings. -->
<div class="card">
  <b>Restore from a backup</b>
  <p class="hint" style="margin-top:8px">Have a configuration backup from this or another
  Heliograph? Apply it here instead of filling in the form. You will see exactly what it
  changes before anything happens.</p>
  <label for="rsf">Backup file (.json)</label>
  <input id="rsf" type="file" accept=".json,application/json">
  <button id="rsb" type="button" onclick="previewRestore()">Show what it would change</button>
  <div id="rm" class="msg"></div>
  <div id="rp"></div>
</div>

<div class="card">
  <p class="sub" style="margin:0">After saving, the bridge restarts and joins your network.
  This setup network disappears. Find it again by its hostname, or via your router.</p>
  <!-- This page asks for a network and a password and nothing about the inverter, so it used to
       end reading like the end of the job. It is not: the bridge arrives on the LAN knowing
       nothing about what is on the RS485 pair, and the owner who thinks they are finished meets
       a dashboard with no production on it. One sentence, here, where the expectation is set. -->
  <p class="sub" style="margin:8px 0 0">One thing is left after that: telling it which inverter
  it is wired to. Open <b>Inverters</b> on the dashboard and let it search — the RS485 pair (A, B
  and ground) needs to be connected first.</p>
</div>
</div>
<script>
const $=s=>document.getElementById(s);
const msg=(t,ok)=>{const m=$('m');m.textContent=t;m.className='msg '+(ok?'ok':'err')};
const esc=s=>String(s??'').replace(/[<>&"']/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;',"'":'&#39;'}[c]));

// Basic auth by hand: fetch() will not surface the browser's credential dialog reliably, least
// of all in the captive-portal mini-browser this page is usually opened in.
//
// Always UTF-8. btoa() takes a string of code units and only throws above U+00FF, so a plain
// btoa() silently emits LATIN-1 for exactly the range that matters here -- é, ë, ü, ö, ç. The
// firmware compares against the bytes it stored from the setup POST, which are UTF-8, so those
// passwords would never match and the owner would be locked out of their own recovery with
// "password not accepted" (review, 2026-07-25).
// Trimmed: Basic auth compares bytes, and this page is most often opened in a phone's
// captive-portal browser, where a stray autocorrect space is easy and invisible.
function basicAuthHeader(){
  const raw=($('curuser').value.trim()||'admin')+':'+$('cur').value;
  return 'Basic '+btoa(String.fromCharCode(...new TextEncoder().encode(raw)));
}

// Two modes. First boot: no admin password exists, provisioning is open, and the form sets one.
// Reconfigure: this setup network came back because the bridge lost WiFi (a router reboot is
// enough), a password already exists, and provisioning demands it -- so the form authenticates
// and changes only the network. Detected up front rather than on a failed submit, because a 401
// from requestAuthentication() has an empty body and the old code parsed it as JSON.
let reauth=false, modeKnown=false;
const setupReady=fetch('/api/v1/config').then(r=>r.json()).then(c=>{
  const sec=(c&&c.security)||{};
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
      headers['Authorization']=basicAuthHeader();
    }else{
      body.security={admin_password:$('admin').value};
    }
    const r=await fetch('/api/v1/provision',{method:'POST',headers,body:JSON.stringify(body)});
    if(r.status===401){
      refuse('Those admin credentials were not accepted — check the username too if you ever '+
             'changed it. Forgotten them? Hold BOOT for ~5 seconds while the board is running '+
             'to factory-reset it.');
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

// ---------------- Restore ----------------
// Same two-step flow as the settings page, and the same firmware endpoint. The file is posted
// straight from a FileReader and nothing is staged on the bridge between the two steps, so
// what gets applied is exactly what was previewed.

const rmsg=(t,cls)=>{const m=$('rm');m.textContent=t;m.className='msg '+cls;m.style.display='block'};
let rsPending=null;

// Authenticated exactly like provisioning, and for the same reason: the setup network also
// comes back on an ALREADY-CONFIGURED bridge after a WiFi outage, and its AP is open. Without
// this, anyone in radio range of a bridge whose router had rebooted could overwrite its whole
// configuration -- admin password, broker, relay gates -- from a file.
function restoreHeaders(){
  const h={'Content-Type':'application/json'};
  if(reauth)h['Authorization']=basicAuthHeader();
  return h;
}

async function restorePost(query){
  const f=$('rsf').files[0];
  if(!f){rmsg('Choose a backup file first.','err');return null}
  if(reauth&&!$('cur').value){
    rmsg('This bridge is already set up, so restoring needs its admin password — fill it in above.','err');
    return null;
  }
  const text=rsPending!==null&&query===''?rsPending:await f.text();
  const r=await fetch('/api/v1/config/restore'+query,
    {method:'POST',headers:restoreHeaders(),body:text});
  if(r.status===401){
    rmsg('Those admin credentials were not accepted — check the username too if you ever changed it.','err');
    return null;
  }
  const d=await r.json().catch(()=>({}));
  if(!r.ok){rmsg((d.error&&d.error.message)||('HTTP '+r.status),'err');return null}
  return {text,data:d};
}

async function previewRestore(){
  $('rp').innerHTML='';rsPending=null;
  rmsg('Checking the file…','');
  const out=await restorePost('?dry_run=true');
  if(!out)return;
  rsPending=out.text;
  $('rm').style.display='none';
  const d=out.data, b=d.backup||{};
  const src=[b.firmware_version?'written by firmware '+esc(b.firmware_version):'',
             b.exported_at?'on '+esc(b.exported_at.replace('T',' ').replace('Z',' UTC')):'',
             b.includes_secrets?'<b>contains passwords</b>':'contains no passwords']
            .filter(Boolean).join(' · ');
  if(!d.change_count){
    $('rp').innerHTML=`<div class="msg ok">This backup matches what the bridge already has —
      nothing would change.<div class="hint">${src}</div></div>`;
    return;
  }
  // Named where it bites. A redacted backup on a fresh board leaves no WiFi password and no
  // admin password, and the firmware refuses the second of those outright -- but the WiFi one
  // it will happily accept, leaving a bridge that cannot join anything.
  const noWifiPw=!b.includes_secrets;
  $('rp').innerHTML=`<div class="hint">${src}</div>
    <table><thead><tr><th>Setting</th><th>Now</th><th>After</th></tr></thead><tbody>
    ${(d.changes||[]).map(c=>`<tr><td>${esc(c.field)}</td><td>${esc(c.before)}</td>
      <td><b>${esc(c.after)}</b></td></tr>`).join('')}</tbody></table>
    <div class="hint">${d.change_count} setting(s) would change.${
      d.reboot_required?' The bridge restarts afterwards.':''}</div>
    ${noWifiPw?`<div class="msg err">This backup carries no passwords, so it cannot supply the
      WiFi password either. Fill the form in above instead, or export a new backup with
      passwords included.</div>`:''}
    <button type="button" onclick="applyRestore()">Apply and restart</button>`;
}

async function applyRestore(){
  $('rp').innerHTML='';
  rmsg('Applying…','');
  const out=await restorePost('');
  if(!out)return;
  rsPending=null;
  $('rm').className='msg ok';
  // Whether it restarts is the firmware's call, not an assumption: a restore that touched only
  // live-applied settings does not reboot, and promising a restart that never comes leaves
  // someone waiting for a bridge that is already back.
  $('rm').innerHTML='Restored '+(out.data.changed_fields||0)+' setting(s). '+
    (out.data.reboot_required
      ? 'The bridge is restarting and joining the network from the backup — this setup network '+
        'disappears. Give it ~30 seconds.'
      : 'No restart was needed, so this setup network is still up.');
}
</script></body></html>)HTML";

}  // namespace heliograph::web
