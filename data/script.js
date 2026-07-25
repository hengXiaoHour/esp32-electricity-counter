/*
JSON Payload Changes (compared to original dashboard):
  ADDED   per-channel: ch[i].hasRelay (bool)  — true for relay-controlled channels (1-4)
  ADDED   root: currentCalibration (number[])  — array of 6 per-channel current cal values
  ADDED   root: otaProgress (number, optional) — OTA progress 0-100
  ADDED   root: firmwareVersion (string, optional) — firmware version string
  CHANGED root: currentCalibration is now an array, was a single number

New WebSocket commands:
  {cmd:'set_current_cal', ch:<int>, val:<float>} — per-channel current calibration
  {cmd:'set_voltage_cal', val:<float>} — unchanged

Architecture: UI is hosted externally (PC/phone). ESP32 only runs WebSocket server.
WebSocket connects to user-specified IP: ws://<ip>/ws
*/

let ws = null;
let reconnectTimer = null;
let calInitialized = false;
let currentIP = '';
let manualDisconnect = false;
let bootTimeMs = 0;

function handleConnect() {
  const ip = document.getElementById('esp32Ip').value.trim();
  if (!ip) return;
  currentIP = ip;
  try { localStorage.setItem('esp32monitor_ip', ip); } catch(e) {}
  setConnectStatus('Connecting to ' + ip + '...', 'connecting');
  connectWS(ip);
}

function handleDisconnect() {
  manualDisconnect = true;
  if (reconnectTimer) { clearInterval(reconnectTimer); reconnectTimer = null; }
  if (ws) {
    ws.onclose = () => { ws = null; manualDisconnect = false; showConnectPanel(); };
    ws.close(1000);
  } else {
    manualDisconnect = false;
    showConnectPanel();
  }
}

function connectWS(ip) {
  if (ws) { ws.onclose = null; ws.close(); ws = null; }
  manualDisconnect = false;
  bootTimeMs = 0;
  const url = 'ws://' + ip + '/ws';
  document.getElementById('wsHint').textContent = url;
  ws = new WebSocket(url);
  ws.onopen = () => {
    calInitialized = false;
    if (reconnectTimer) { clearInterval(reconnectTimer); reconnectTimer = null; }
    document.getElementById('connectedIp').textContent = ip;
    document.getElementById('connStatus').textContent = 'Connected';
    document.getElementById('connStatus').style.color = '#2ecc71';
    showDashboard();
  };
  ws.onclose = () => {
    if (!manualDisconnect) {
      setConnectStatus('Disconnected — retrying every 3s...', 'disconnected');
      showConnectPanel();
      if (!reconnectTimer) {
        reconnectTimer = setInterval(() => { if (currentIP) connectWS(currentIP); }, 3000);
      }
    }
  };
  ws.onerror = () => {
    if (!manualDisconnect) {
      setConnectStatus('Connection failed — check IP and try again', 'disconnected');
    }
  };
  ws.onmessage = (e) => {
    try { updateDashboard(JSON.parse(e.data)); } catch(err) {}
  };
}

function showDashboard() {
  document.getElementById('connectPanel').classList.add('hidden');
  document.getElementById('dashboard').classList.remove('hidden');
}

function showConnectPanel() {
  document.getElementById('dashboard').classList.add('hidden');
  document.getElementById('connectPanel').classList.remove('hidden');
}

function setConnectStatus(msg, cls) {
  const el = document.getElementById('connectStatus');
  el.textContent = msg;
  el.className = 'connect-status ' + (cls || '');
}

function updateDashboard(data) {
  if (bootTimeMs === 0 && data.uptime) {
    bootTimeMs = Date.now() - (data.uptime * 1000);
  }
  document.getElementById('voltage').textContent = (data.v || 0).toFixed(1) + ' V';

  const wifiEl = document.getElementById('wifiStatus');
  wifiEl.textContent = data.wifi ? 'Connected' : (data.ap ? 'AP Mode' : 'Disconnected');

  const ledEl = document.getElementById('ledStatus');
  ledEl.className = '';
  let ledColor = '#555';
  if (data.ota) {
    ledColor = '#06f';
    ledEl.className = 'blink-blue';
  } else if (!data.wifi) {
    ledColor = '#ff0000';
  } else {
    let hasTrip = false, hasWarn = false;
    for (const ch of data.ch) { if (ch.s === 2) hasTrip = true; if (ch.s === 1) hasWarn = true; }
    if (hasTrip) { ledColor = '#ff1744'; ledEl.className = 'blink-red'; }
    else if (hasWarn) { ledColor = '#f39c12'; ledEl.className = 'blink-yellow'; }
    else { ledColor = '#2ecc71'; }
  }
  ledEl.style.backgroundColor = ledColor;

  const otaPanel = document.getElementById('otaPanel');
  if (data.ota) {
    otaPanel.classList.remove('hidden');
    if (data.otaProgress !== undefined) {
      document.getElementById('otaPercent').textContent = data.otaProgress + '%';
      document.getElementById('otaProgress').style.width = data.otaProgress + '%';
    }
    document.getElementById('otaVersion').textContent = data.firmwareVersion || '-';
  } else {
    otaPanel.classList.add('hidden');
  }

  const container = document.getElementById('channels');
  container.innerHTML = '';
  for (let i = 0; i < data.ch.length; i++) {
    const ch = data.ch[i];
    const hasRelay = ch.hasRelay !== undefined ? ch.hasRelay : i < 4;

    const card = document.createElement('div');
    let cardClass = 'card';
    if (ch.s === 2) cardClass += ' tripped';
    else if (ch.s === 1) cardClass += ' warning';
    else if (ch.s === 3 || (hasRelay && !ch.r)) cardClass += ' off';
    else cardClass += ' ok';
    card.className = cardClass;

    const statusLabel = ['OK', 'WARNING', 'TRIPPED', 'DISABLED'][ch.s] || 'UNKNOWN';
    let statusClass = ['ok', 'warning', 'tripped', ''][ch.s] || '';
    if (ch.s === 2) statusClass += ' blink-red';
    else if (ch.s === 1) statusClass += ' blink-yellow';

    let relayHtml;
    if (hasRelay) {
      relayHtml = `<span class="relay-indicator ${ch.r ? 'relay-on' : 'relay-off'}">${ch.r ? 'ON' : 'OFF'}</span>`;
    } else {
      relayHtml = `<span class="monitor-only">Monitoring Only</span>`;
    }

    let resetBtn = '';
    if (hasRelay) {
      resetBtn = `<button class="reset" onclick="resetRelay(${i})">Reset</button>`;
    }

    card.innerHTML = `
      <div class="card-header">
        <span class="ch-name">${escHtml(ch.n)}</span>
        <span class="ch-status ${statusClass}">${statusLabel}</span>
      </div>
      <div class="ch-stats">
        <div><span class="label">Current</span><br><span class="value mono">${ch.a.toFixed(2)}</span> <span class="unit">A</span></div>
        <div><span class="label">Power</span><br><span class="value mono">${ch.w.toFixed(1)}</span> <span class="unit">W</span></div>
        <div><span class="label">Apparent</span><br><span class="value mono">${ch.va.toFixed(1)}</span> <span class="unit">VA</span></div>
        <div><span class="label">PF</span><br><span class="value mono">${ch.pf.toFixed(3)}</span></div>
        <div><span class="label">Energy</span><br><span class="value mono">${ch.kwh.toFixed(3)}</span> <span class="unit">kWh</span></div>
        <div><span class="label">Relay</span><br>${relayHtml}</div>
      </div>
      <div class="ch-controls">
        <input type="number" id="clim_${i}" value="${ch.cl}" step="0.5" placeholder="A limit" title="Current limit (A)">
        <input type="number" id="plim_${i}" value="${ch.pl}" step="100" placeholder="W limit" title="Power limit (W)">
        <input type="text" id="name_${i}" value="${escHtml(ch.n)}" placeholder="Name" title="Channel name">
        <button class="limit" onclick="setLimit(${i})">Save</button>
        ${resetBtn}
      </div>`;
    container.appendChild(card);
  }

  const eventList = document.getElementById('eventList');
  eventList.innerHTML = '';
  if (data.events) {
    for (const ev of data.events.slice(-10).reverse()) {
      const div = document.createElement('div');
      const sc = ev.s === 2 ? 'tripped' : ev.s === 1 ? 'warning' : '';
      div.className = 'event ' + sc;
      div.innerHTML = `<span class="time">${formatTime(ev.t)}</span><span class="ch-tag">Ch${ev.c+1}</span><span class="msg">${escHtml(ev.m)}</span>`;
      eventList.appendChild(div);
    }
  }

  if (!calInitialized) {
    const voltInput = document.getElementById('voltCal');
    if (voltInput.value === '') voltInput.value = data.voltageCalibration || 260;

    const calContainer = document.getElementById('currentCalRows');
    if (data.ch && data.ch.length > 0 && calContainer.children.length === 0) {
      const calArray = Array.isArray(data.currentCalibration) ? data.currentCalibration : [];
      for (let i = 0; i < data.ch.length; i++) {
        const row = document.createElement('div');
        row.className = 'cal-row';
        const val = calArray[i] !== undefined ? calArray[i] : 100;
        row.innerHTML = `
          <label>Ch ${i+1}:</label>
          <input type="number" id="currCal_${i}" step="0.1" value="${val}">
          <button onclick="sendCurrentCal(${i})">Set</button>
        `;
        calContainer.appendChild(row);
      }
    }
    calInitialized = true;
  }
}

function setLimit(ch) {
  const clim = document.getElementById('clim_' + ch).value;
  const plim = document.getElementById('plim_' + ch).value;
  const name = document.getElementById('name_' + ch).value;
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({cmd:'set_limit', ch:ch, val:parseFloat(clim)}));
    ws.send(JSON.stringify({cmd:'set_power_limit', ch:ch, val:parseFloat(plim)}));
    if (name) ws.send(JSON.stringify({cmd:'set_name', ch:ch, name:name}));
  }
}

function resetRelay(ch) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({cmd:'reset_relay', ch:ch}));
  }
}

function sendVoltageCal() {
  const val = parseFloat(document.getElementById('voltCal').value);
  if (ws && ws.readyState === WebSocket.OPEN && !isNaN(val)) {
    ws.send(JSON.stringify({cmd:'set_voltage_cal', val}));
  }
}

function sendCurrentCal(ch) {
  const val = parseFloat(document.getElementById('currCal_' + ch).value);
  if (ws && ws.readyState === WebSocket.OPEN && !isNaN(val)) {
    ws.send(JSON.stringify({cmd:'set_current_cal', ch:ch, val}));
  }
}

function startClock() {
  function tick() {
    const el = document.getElementById('currentTime');
    if (el) el.textContent = new Date().toLocaleTimeString();
  }
  tick();
  setInterval(tick, 1000);
}

function formatTime(ts) {
  let ms = ts;
  if (bootTimeMs > 0 && ts > 0 && ts < 1e11) {
    ms = bootTimeMs + (ts * 1000);
  }
  return new Date(ms).toLocaleString();
}

function escHtml(s) {
  if (!s) return '';
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

startClock();
(function init() {
  const savedIP = (typeof localStorage !== 'undefined') ? localStorage.getItem('esp32monitor_ip') : null;
  if (savedIP) {
    document.getElementById('esp32Ip').value = savedIP;
    currentIP = savedIP;
    setConnectStatus('Auto-connecting to ' + savedIP + '...', 'connecting');
    connectWS(savedIP);
  }
})();
