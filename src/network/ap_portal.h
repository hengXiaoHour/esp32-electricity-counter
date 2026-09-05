#pragma once

// Lightweight AP-mode page served from PROGMEM at http://192.168.4.1/.
// Same-origin http + ws:// — no mixed-content block like the hosted https UI.
// Read-only live readings (same /ws JSON as STA mode) + WiFi credentials form.
// Kept small on purpose: the partition scheme is No-FS, so no LittleFS.

const char AP_PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Counter Setup</title>
<style>
body{font-family:sans-serif;margin:16px;max-width:480px;background:#101316;color:#e8e8e8}
h2{margin:4px 0;font-size:20px}h3{margin:20px 0 8px;font-size:16px}
.mono{font-variant-numeric:tabular-nums}
#wsState{font-size:13px;margin-bottom:8px;color:#9a9a9a}
#wsState.on{color:#4ade80}
table{width:100%;border-collapse:collapse;font-size:14px}
td,th{padding:6px 4px;border-bottom:1px solid #2a2f35;text-align:right}
td:first-child,th:first-child{text-align:left}
input{width:100%;padding:9px;margin:6px 0;box-sizing:border-box;border:1px solid #333;border-radius:4px;background:#1a1e23;color:#fff}
button{width:100%;padding:11px;background:#2196F3;color:#fff;border:none;border-radius:4px;font-size:16px}
.hdr{display:flex;gap:16px;font-size:15px;margin:8px 0}
.hint{font-size:12px;color:#9a9a9a;margin-top:10px}
</style></head><body>
<h2>ESP32 Counter <span style="font-size:12px;color:#9a9a9a">AP: %APSSID%</span></h2>
<div id="wsState">Connecting to live data...</div>
<div class="hdr mono"><span>V: <b id="v">--</b></span><span>Up: <b id="up">--</b></span></div>
<table><thead><tr><th>Ch</th><th>A</th><th>W</th><th>kWh</th><th>St</th></tr></thead>
<tbody id="ch"></tbody></table>
<h3>WiFi Setup</h3>
<form action="/save" method="POST">
<input type="text" name="ssid" placeholder="WiFi SSID" required>
<input type="password" name="pass" placeholder="WiFi password">
<button type="submit">Save &amp; Connect</button>
</form>
<div class="hint">After saving, the board joins your WiFi — reconnect your phone to it and use the Cloud/Local dashboard.</div>
<script>
var ST=['OK','WARN','TRIP','OFF'];
var el=function(id){return document.getElementById(id);};
var ws=new WebSocket('ws://'+location.host+'/ws');
ws.onopen=function(){var s=el('wsState');s.textContent='Live';s.className='on';};
ws.onclose=function(){var s=el('wsState');s.textContent='Disconnected — reload to retry';s.className='';};
ws.onmessage=function(e){
  try{
    var d=JSON.parse(e.data);
    if(d.type==='console')return;
    el('v').textContent=(+d.v).toFixed(1)+' V';
    var u=+d.uptime;
    el('up').textContent=Math.floor(u/3600)+'h'+Math.floor(u%3600/60)+'m';
    var h='';
    for(var i=0;i<d.ch.length;i++){
      var c=d.ch[i];
      h+='<tr><td>'+c.n+'</td><td class="mono">'+(+c.a).toFixed(2)+'</td><td class="mono">'+(+c.w).toFixed(0)+'</td><td class="mono">'+(+c.kwh).toFixed(2)+'</td><td>'+(ST[c.s]||c.s)+'</td></tr>';
    }
    el('ch').innerHTML=h;
  }catch(x){}
};
</script></body></html>
)rawliteral";

const char AP_SAVED_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Saved</title>
<style>body{font-family:sans-serif;margin:20px;text-align:center;padding-top:40px;background:#101316;color:#e8e8e8}</style>
</head><body>
<h2>Saved!</h2>
<p>Connecting. Rejoin your WiFi and open the dashboard.</p>
</body></html>
)rawliteral";
