// A bridge that has just been provisioned and rebooted: on the WiFi, admin password set, and
// no inverter answering yet. Nothing has been configured on the RS485 side at all.
(function(){
  const status = {
    device:{online:false,data_valid:false,data_stale:true,last_successful_poll_seconds_ago:null},
    bridge:{firmware_version:'0.23.0',board_id:'waveshare-rs485-can',
      board_name:'Waveshare ESP32-S3-RS485-CAN',hostname:'heliograph-a1b2c3',
      uptime_seconds:41,wifi_connected:true,wifi_rssi_dbm:-61,mqtt_connected:false,
      modbus_listening:true,modbus_clients:0,time_synced:true,time:'2026-07-29 09:12:03',
      devices_configured:1,devices_started:1,devices_polled:1,max_devices:8,
      device_problems:[],relays:[]},
    status_text:'',error_code:null,
    measurements:{},
    devices:[{id:'eversolar_legacy-unknown',label:'',online:false,data_valid:false,
      data_stale:true,last_successful_poll_seconds_ago:null,ac_power_w:null,
      energy_today_kwh:null,ac_voltage_v:null,temperature_c:null,
      battery_soc_pct:null,battery_power_w:null}],
    totals:{ac_power_w:0,ac_power_devices:0,energy_today_kwh:0,energy_today_devices:0,
      energy_total_kwh:0,energy_total_devices:0,devices_polled:1,devices_answering:0},
    poll_success_total:0,poll_failure_total:37
  };
  const config = {
    bridge_name:'Heliograph',
    wifi:{ssid:'Zonnehuis',hostname:'heliograph-a1b2c3',password_set:true,ip:'',gateway:'',
      subnet:'',dns1:'',dns2:''},
    mqtt:{enabled:false,host:'',port:1883,username_set:false,password_set:false,
      base_topic:'heliograph',discovery_enabled:true},
    modbus:{enabled:true,port:502,unit_id:1,max_clients:8,idle_timeout_seconds:120},
    polling:{interval_seconds:10},
    serial:{override:false,baud_rate:9600,parity:'none',data_bits:8,stop_bits:1},
    // What provisioning leaves behind: no driver named at all.
    driver:{id:'',label:'',options:{}},
    additional_devices:[],
    ntp:{enabled:true,use_dhcp:true,server:'pool.ntp.org',
      timezone:'CET-1CEST,M3.5.0,M10.5.0/3',timezone_name:'Europe/Amsterdam'},
    security:{password_set:true,read_only_mode:true},
    logging:{level:'info'}, updates:{check_enabled:true}, relays:{enabled:false,roles:[]}
  };
  const driversDoc = {drivers:[
    {id:'eversolar_legacy',display_name:'EverSolar / Zeversolar legacy',support_level:'beta',
      description:'AA55 framing over RS485, for TL-series inverters abandoned by their portal.',
      serial_profiles:[{baud_rate:9600,parity:'none',data_bits:8,stop_bits:1}],
      options:[{key:'layout',display_name:'Payload layout',allowed_values:['auto','single','dual'],
                default_value:'auto',description:'auto derives it from the frame length.'},
               {key:'address',display_name:'Assigned bus address',default_value:'16',
                min_value:16,max_value:254,description:'Handed to the inverter at registration.'}]},
    {id:'modbus_profile',display_name:'Modbus RTU (profile-driven)',support_level:'experimental',
      description:'Modbus RTU. The register map is a data file.',
      serial_profiles:[{baud_rate:9600,parity:'none',data_bits:8,stop_bits:1}],
      options:[{key:'profile',display_name:'Register map',allowed_values:['','sph_3_6kw','mic_tl_x'],
                default_value:'',description:'Which model this unit is.'},
               {key:'unit_id',display_name:'Bus address',default_value:'1',min_value:1,max_value:247,
                description:'Must match the address in the inverter’s menu.'}]},
  ]};
  const diagnostics = {uptime_seconds:41,firmware_version:'0.23.0',
    board:'Waveshare ESP32-S3-RS485-CAN',free_heap_bytes:201000,minimum_free_heap_bytes:198000,
    max_alloc_heap_bytes:120000,reset_reason:'power-on',ota_image_state:'valid',
    coredump_present:false,wifi_connected:true,wifi_rssi_dbm:-61,mqtt_connected:false,
    time_synced:true,time:'2026-07-29 09:12:03',poll_success_total:0,poll_failure_total:37,
    consecutive_poll_failures:37,checksum_error_total:0,rs485_timeout_total:37,
    invalid_frame_total:0,last_error:'no reply from the inverter'};

  const json=(b,s)=>Promise.resolve(new Response(JSON.stringify(b),
    {status:s||200,headers:{'Content-Type':'application/json'}}));
  window.__discoveryReport = {status:'done',busy:false,candidates:[]};  // the empty-handed case
  window.fetch=(url,opts)=>{
    const u=String(url);
    const m=u.match(/^\/api\/v1\/devices\/([^/?]+)(\/[a-z]+)?/);
    if(m){
      if(m[2]==='/measurements')return json({measurements:{}});
      if(m[2]==='/capabilities')return json(null,404);
      return json({id:decodeURIComponent(m[1]),label:'',
        identity:{driver_id:'eversolar_legacy'},
        driver:{id:'eversolar_legacy',display_name:'EverSolar / Zeversolar legacy',
                support_level:'beta',supports_write:false},
        online:false,data_valid:false,data_stale:true,
        last_successful_poll_seconds_ago:null,consecutive_poll_failures:37});
    }
    if(u.startsWith('/api/v1/status'))return json(status);
    if(u.startsWith('/api/v1/devices'))return json({devices:status.devices.map(d=>d.id)});
    if(u.startsWith('/api/v1/config/backup'))return json(config);
    if(u.startsWith('/api/v1/config'))return json(config);
    if(u.startsWith('/api/v1/drivers'))return json(driversDoc);
    if(u.startsWith('/api/v1/diagnostics'))return json(diagnostics);
    if(u.startsWith('/api/v1/logs'))return json({level:'info',total:0,returned:0,lines:[]});
    if(u.startsWith('/api/v1/discovery'))return json(window.__discoveryReport);
    if(u.startsWith('/api/v1/capture'))return json({status:'idle'});
    if(u.startsWith('/api/v1/'))return json({ok:true},202);
    return Promise.reject(new Error('fresh bridge stub: no network'));
  };
  window.EventSource=function(){this.close=()=>{}};
})();
