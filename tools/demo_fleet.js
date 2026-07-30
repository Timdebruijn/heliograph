// SPDX-License-Identifier: MIT
//
// A fixed three-inverter fleet, answered in the browser. NOT part of what ships: the page in
// src/web/assets/index_html.h contains none of this, and nothing includes this file at build
// time.
//
// It exists twice over. tools/check_dashboard_layout.py renders the real page against it in
// headless Chrome, so the layout assertions run on a fleet that no bridge on this desk has --
// one legacy single-phase PV unit, one three-phase hybrid with a battery, and one configured
// device that never replied. The third is the one worth having: an inverter that answers
// nothing is the case the page has the most to say about and the least opportunity to prove.
//
// It is also how the page can be clicked through with no hardware at all:
//
//     python3 tools/preview_web.py        # serves the page with this stub attached
//
// Set window.__demoBattery = {soc, power} before this file runs to move the hybrid's battery.
// The layout check uses it to render charging, discharging and idle from one page.
(function(){
  const now = () => new Date().toTimeString().slice(0,8);
  const batt = window.__demoBattery || {soc:68, power:-1240};
  const status = {
    device:{manufacturer:'EverSolar',model:'TL3000',serial_number:'EU00T112345678',
      driver_id:'eversolar_legacy',support_level:'beta',online:true,data_valid:true,
      data_stale:false,last_successful_poll_seconds_ago:3},
    bridge:{firmware_version:'0.14.0',board_id:'waveshare-rs485-can',
      board_name:'Waveshare ESP32-S3-RS485-CAN',hostname:'heliograph-a1b2c3',
      uptime_seconds:5061,wifi_connected:true,wifi_rssi_dbm:-54,mqtt_connected:true,
      modbus_listening:true,modbus_clients:0,time_synced:true,time:'2026-07-26 '+now(),
      devices_configured:3,devices_started:3,devices_polled:3,max_devices:8,
      device_problems:[],relays:[]},
    status_text:'Grid-connected (normal)',error_code:null,
    measurements:{
      'ac.power.total':{value:197,unit:'W'},'energy.today':{value:8.16,unit:'kWh'},
      'energy.total':{value:35546.4,unit:'kWh'},'ac.phase_l1.voltage':{value:229.8,unit:'V'},
      'ac.phase_l1.current':{value:0.9,unit:'A'},'ac.phase_l1.power':{value:197,unit:'W'},
      'ac.frequency':{value:49.98,unit:'Hz'},'dc.mppt_1.voltage':{value:331.4,unit:'V'},
      'dc.mppt_1.current':{value:0.6,unit:'A'},'dc.mppt_1.power':{value:199,unit:'W'},
      'inverter.temperature':{value:34.4,unit:'°C'},'inverter.operating_hours':{value:21877,unit:'h'}},
    devices:[
      {id:'eversolar_legacy-EU00T112345678',label:'Schuur',online:true,data_valid:true,
       data_stale:false,last_successful_poll_seconds_ago:3,ac_power_w:197,energy_today_kwh:8.16,
       ac_voltage_v:229.8,temperature_c:34.4,battery_soc_pct:null,battery_power_w:null},
      {id:'modbus_profile-GW2400ABC',label:'Garage',online:true,data_valid:true,data_stale:false,
       last_successful_poll_seconds_ago:11,ac_power_w:1840,energy_today_kwh:11.4,
       ac_voltage_v:231.2,temperature_c:41.2,battery_soc_pct:batt.soc,battery_power_w:batt.power},
      {id:'modbus_profile-GW3300XYZ',label:'Dak achter',online:false,data_valid:false,
       data_stale:true,last_successful_poll_seconds_ago:null,ac_power_w:null,
       energy_today_kwh:null,ac_voltage_v:null,temperature_c:null,battery_soc_pct:null,
       battery_power_w:null}],
    totals:{ac_power_w:2037,ac_power_devices:2,energy_today_kwh:19.56,energy_today_devices:2,
      energy_total_kwh:53967,energy_total_devices:2,devices_polled:3,devices_answering:2},
    poll_success_total:504,poll_failure_total:2
  };
  const measFor = {
    'eversolar_legacy-EU00T112345678':status.measurements,
    'modbus_profile-GW2400ABC':{
      'ac.power.total':{value:1840,unit:'W'},'energy.today':{value:11.4,unit:'kWh'},
      'energy.total':{value:18420.6,unit:'kWh'},'battery.soc':{value:batt.soc,unit:'%'},
      'ac.phase_l1.voltage':{value:231.2,unit:'V'},'ac.phase_l2.voltage':{value:229.6,unit:'V'},
      'ac.phase_l3.voltage':{value:230.4,unit:'V'},'ac.phase_l1.current':{value:2.7,unit:'A'},
      'ac.phase_l2.current':{value:2.6,unit:'A'},'ac.phase_l3.current':{value:2.7,unit:'A'},
      'ac.phase_l1.power':{value:620,unit:'W'},'ac.phase_l2.power':{value:600,unit:'W'},
      'ac.phase_l3.power':{value:620,unit:'W'},'ac.frequency':{value:50.01,unit:'Hz'},
      'dc.mppt_1.voltage':{value:412.8,unit:'V'},'dc.mppt_1.current':{value:1.4,unit:'A'},
      'dc.mppt_1.power':{value:578,unit:'W'},'dc.mppt_2.voltage':{value:408.2,unit:'V'},
      'dc.mppt_2.current':{value:0.3,unit:'A'},'dc.mppt_2.power':{value:122,unit:'W'},
      'dc.power.total':{value:700,unit:'W'},'battery.power':{value:batt.power,unit:'W'},
      'battery.charge_power':{value:0,unit:'W'},'battery.discharge_power':{value:1240,unit:'W'},
      'battery.voltage':{value:51.4,unit:'V'},'battery.current':{value:-24.1,unit:'A'},
      'battery.temperature':{value:22.8,unit:'°C'},'battery.energy_charged':{value:6.82,unit:'kWh'},
      'battery.energy_discharged':{value:4.19,unit:'kWh'},'grid.import_power':{value:0,unit:'W'},
      'grid.export_power':{value:410,unit:'W'},'inverter.temperature':{value:41.2,unit:'°C'},
      'inverter.operating_hours':{value:9214,unit:'h'}},
    'modbus_profile-GW3300XYZ':{}
  };
  const devFor = {
    'eversolar_legacy-EU00T112345678':{id:'eversolar_legacy-EU00T112345678',label:'Schuur',config_slot:0,
      identity:{manufacturer:'EverSolar',model:'TL3000',serial_number:'EU00T112345678',
        driver_id:'eversolar_legacy',protocol_name:'AA55'},
      driver:{id:'eversolar_legacy',display_name:'EverSolar / Zeversolar legacy',
        support_level:'beta',supports_write:false},
      online:true,data_valid:true,data_stale:false,last_successful_poll_seconds_ago:3,
      consecutive_poll_failures:0},
    'modbus_profile-GW2400ABC':{id:'modbus_profile-GW2400ABC',label:'Garage',config_slot:1,
      identity:{manufacturer:'Growatt',model:'SPH 6000TL BL-UP',serial_number:'GW2400ABC',
        driver_id:'modbus_profile',protocol_name:'Modbus RTU'},
      driver:{id:'modbus_profile',display_name:'Growatt SPH hybrid',
        support_level:'experimental',supports_write:false},
      online:true,data_valid:true,data_stale:false,last_successful_poll_seconds_ago:11,
      consecutive_poll_failures:0},
    'modbus_profile-GW3300XYZ':{id:'modbus_profile-GW3300XYZ',label:'Dak achter',config_slot:2,
      identity:{driver_id:'modbus_profile'},
      driver:{id:'modbus_profile',display_name:'Growatt MIC TL-X',
        support_level:'experimental',supports_write:false},
      online:false,data_valid:false,data_stale:true,last_successful_poll_seconds_ago:null,
      consecutive_poll_failures:37}
  };
  const capsFor = {
    'eversolar_legacy-EU00T112345678':{read_only:true,phase_count:1,mppt_count:1,has_battery:false,
      read:['ac.power.total','energy.today','energy.total'],write:[]},
    'modbus_profile-GW2400ABC':{read_only:true,phase_count:3,mppt_count:2,has_battery:true,
      read:['ac.power.total','battery.soc'],write:[]},
    'modbus_profile-GW3300XYZ':{read_only:true,phase_count:1,mppt_count:1,has_battery:false,
      read:['ac.power.total'],write:[]}
  };
  const config = {
    bridge_name:'Heliograph',
    wifi:{ssid:'Zonnehuis',hostname:'heliograph-a1b2c3',password_set:true,ip:'',gateway:'',
      subnet:'',dns1:'',dns2:''},
    mqtt:{enabled:true,host:'192.168.1.10',port:1883,username_set:true,password_set:true,
      base_topic:'heliograph',discovery_enabled:true},
    modbus:{enabled:true,port:502,unit_id:1,max_clients:8,idle_timeout_seconds:120},
    polling:{interval_seconds:10},
    serial:{override:false,baud_rate:9600,parity:'none',data_bits:8,stop_bits:1},
    // options:{} ON PURPOSE, and it is what a real bridge carries. A primary configured through
    // the wizard stores nothing here and answers at its driver's declared default -- exactly how
    // 192.168.20.254 is set up. Giving it an explicit address made this stub "realistic" in the
    // one way that hid a collision: the free-address search read stored options only, saw
    // nothing taken, and handed the new row the address the primary was already using.
    driver:{id:'eversolar_legacy',label:'Schuur',options:{}},
    additional_devices:[
      {driver_id:'modbus_profile',label:'Garage',options:{profile:'sph_3_6kw',unit_id:'2'}},
      {driver_id:'modbus_profile',label:'Dak achter',options:{profile:'mic_tl_x',unit_id:'3'}}],
    ntp:{enabled:true,use_dhcp:true,server:'pool.ntp.org',
      timezone:'CET-1CEST,M3.5.0,M10.5.0/3',timezone_name:'Europe/Amsterdam'},
    security:{password_set:true,read_only_mode:true},
    logging:{level:'info'},
    updates:{check_enabled:true},
    relays:{enabled:false,roles:[]}
  };
  const driversDoc = {drivers:[
    {id:'eversolar_legacy',supports_multiple_devices:true,display_name:'EverSolar / Zeversolar legacy',support_level:'beta',
      description:'AA55 framing over RS485, for TL-series inverters abandoned by their portal.',
      serial_profiles:[{baud_rate:9600,parity:'none',data_bits:8,stop_bits:1}],
      options:[
        {key:'layout',display_name:'Payload layout',allowed_values:['auto','single','dual'],
         default_value:'auto',description:'How to read the measurement payload; auto derives it from the frame length.'},
        {key:'address',display_name:'Assigned bus address',default_value:'16',
         min_value:16,max_value:254,
         description:'Address this bridge hands its inverter at registration. Leave at 16 unless more than one shares the loop.'}]},
    {id:'modbus_profile',supports_multiple_devices:true,display_name:'Modbus RTU (profile-driven)',support_level:'experimental',
      description:'Modbus RTU. The register map is a data file, so one driver serves several models.',
      serial_profiles:[{baud_rate:9600,parity:'none',data_bits:8,stop_bits:1}],
      options:[
        {key:'profile',display_name:'Register map',allowed_values:['','sph_3_6kw','mic_tl_x'],
         default_value:'',description:'Which model this unit is. It cannot be detected — probing identifies the protocol, never the model.'},
        {key:'unit_id',display_name:'Bus address',default_value:'1',min_value:1,max_value:247,
         description:'Must match the address set in the inverter’s own menu, and be unique on this bus.'}]},
    {id:'mock',supports_multiple_devices:true,display_name:'Mock Inverter',support_level:'stable',
      description:'A simulated three-phase hybrid, for UI and integration work with no bus.',
      serial_profiles:[],options:[{key:'unit_id',display_name:'Bus address',default_value:'1'}]},
    // The one driver that answers NO. It is here because the page must be able to say so BEFORE
    // a restart -- the firmware refuses the second row at boot, which is the wrong moment to
    // find out, and until this field existed the page had no way to know.
    {id:'solax_x1',supports_multiple_devices:false,display_name:'SolaX X1 (AA55)',support_level:'experimental',
      description:'One inverter per bridge: the address is handed out at registration, not read.',
      serial_profiles:[{baud_rate:9600,parity:'none',data_bits:8,stop_bits:1}],
      options:[
        {key:'address',display_name:'Assigned bus address',default_value:'10',
         min_value:10,max_value:254,
         description:'Address this bridge hands its inverter at registration.'}]}
  ]};
  const diagnostics = {
    uptime_seconds:5061,firmware_version:'0.14.0',board:'Waveshare ESP32-S3-RS485-CAN',
    free_heap_bytes:182364,minimum_free_heap_bytes:168920,max_alloc_heap_bytes:114676,
    psram_size_bytes:null,psram_free_bytes:null,reset_reason:'power-on',ota_image_state:'valid',
    coredump_present:false,coredump_task:null,coredump_pc:null,coredump_cause:null,
    coredump_cause_name:null,coredump_fault_address:null,wifi_connected:true,wifi_rssi_dbm:-54,
    mqtt_connected:true,time_synced:true,time:'2026-07-26 19:52:08',
    poll_success_total:504,poll_failure_total:2,consecutive_poll_failures:0,
    checksum_error_total:1,rs485_timeout_total:2,invalid_frame_total:0,wifi_reconnect_total:0,
    mqtt_reconnect_total:0,modbus_client_connections_total:3,rest_requests_total:1187,
    mqtt_publish_failure_total:0,last_successful_poll_ms:5058221,
    rs485_stack_free_bytes:3120,loop_stack_free_bytes:4432,last_error:''
  };
  const logs = {level:'info',total:4812,returned:8,lines:[
    '19:51:38 [I] schuur      poll ok   12 measurements  214 ms',
    '19:51:43 [I] mqtt        published 45 topics in 62 ms',
    '19:51:48 [I] garage      poll ok   33 measurements  241 ms',
    '19:51:52 [W] dak-achter  rs485 timeout on 0x0004, retry 1/3',
    '19:51:54 [W] dak-achter  rs485 timeout on 0x0004, retry 2/3',
    '19:51:56 [E] dak-achter  no reply at address 3 after 3 attempts',
    '19:52:03 [I] schuur      poll ok   12 measurements  188 ms',
    '19:52:08 [I] garage      poll ok   33 measurements  207 ms']};

  const json = (body, status) => Promise.resolve(new Response(JSON.stringify(body),
    {status: status || 200, headers: {'Content-Type': 'application/json'}}));
  // The demo's numbers drift, so the sparkline has something to draw. Real readings do this by
  // themselves; a fixed payload would put a flat line on screen and make the chart look broken.
  let tick = 0;
  const drift = () => {
    tick++;
    const w = (base, amp) => Math.max(0, Math.round(base + amp * Math.sin(tick / 3) + amp * 0.4 * Math.sin(tick / 1.7)));
    status.devices[0].ac_power_w = w(197, 60);
    status.devices[1].ac_power_w = w(1840, 420);
    status.totals.ac_power_w = status.devices[0].ac_power_w + status.devices[1].ac_power_w;
    status.measurements['ac.power.total'].value = status.devices[0].ac_power_w;
    status.bridge.time = '2026-07-26 ' + now();
  };
  window.fetch = (url, opts) => {
    const u = String(url);
    const m = u.match(/^\/api\/v1\/devices\/([^/?]+)(\/[a-z]+)?/);
    if (m) {
      const id = decodeURIComponent(m[1]);
      if (m[2] === '/measurements') return json({measurements: measFor[id] || {}});
      if (m[2] === '/capabilities') return json(capsFor[id] || null);
      return json(devFor[id] || {}, devFor[id] ? 200 : 404);
    }
    if (u.startsWith('/api/v1/status')) { drift(); return json(status); }
    if (u.startsWith('/api/v1/devices')) return json({devices: Object.keys(devFor)});
    if (u.startsWith('/api/v1/config/backup')) return json(config);
    if (u.startsWith('/api/v1/config')) {
      if (opts && opts.method === 'PATCH') return json({...config, reboot_required: true});
      return json(config);
    }
    if (u.startsWith('/api/v1/drivers')) return json(driversDoc);
    if (u.startsWith('/api/v1/diagnostics')) return json(diagnostics);
    if (u.startsWith('/api/v1/logs')) return json(logs);
    if (u.startsWith('/api/v1/discovery')) return json({status:'idle',busy:false,candidates:[]});
    if (u.startsWith('/api/v1/capture')) return json({status:'idle'});
    if (u.startsWith('/api/v1/')) return json({ok: true}, 202);
    // Everything else is refused rather than passed through. The page's update check goes out
    // to GitHub Pages, and a layout check that reaches the network is one that fails on a
    // runner without one -- for a reason that has nothing to do with the layout.
    return Promise.reject(new Error('demo stub: no network'));
  };
  // No event stream in the demo: the page's own heartbeat keeps it live, which is exactly the
  // fallback path the real page relies on when SSE drops.
  window.EventSource = function(){ this.close = () => {}; };
})();
