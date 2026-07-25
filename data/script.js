const WS_URL = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';
let ws = null;
let reconnectTimer = null;

function connectWS() {
  if (ws && ws.readyState === WebSocket.OPEN) return;
  ws = new WebSocket(WS_URL);
  ws.onopen = () => {
    document.getElementById('wifiStatus').textContent = '🟢 Connected';
    if (reconnectTimer) { clearInterval(reconnectTimer); reconnectTimer = null; }
  };
  ws.onclose = () => {
    document.getElementById('wifiStatus').textContent = '🔴 Disconnected';
    if (!reconnectTimer) reconnectTimer = setInterval(connectWS, 3000);
  };
  ws.onmessage = (e) => {
    try { updateDashboard(JSON.parse(e.data)); } catch(err) {}
  };
}

function updateDashboard(data) {
  document.getElementById('voltage').textContent = (data.v || 0).toFixed(1) + ' V';
  document.getElementById('wifiStatus').textContent = data.wifi ? '🟢 Connected' : (data.ap ? '🟡 AP Mode' : '🔴 Disconnected');
  document.getElementById('uptime').textContent = formatUptime(data.uptime || 0);

  const ledEl = document.getElementById('ledStatus');
  let ledColor = '#555';
  if (data.ota) ledColor = '#0066ff';
  else if (!data.wifi) ledColor = '#ff0000';
  else {
    let hasTrip = false, hasWarn = false;
    for (const ch of data.ch) { if (ch.s === 2) hasTrip = true; if (ch.s === 1) hasWarn = true; }
    if (hasTrip) ledColor = '#ff0000';
    else if (hasWarn) ledColor = '#ffd700';
    else ledColor = '#00ff00';
  }
  ledEl.style.backgroundColor = ledColor;

  const container = document.getElementById('channels');
  container.innerHTML = '';
  for (let i = 0; i < data.ch.length; i++) {
    const ch = data.ch[i];
    const card = document.createElement('div');
    card.className = 'card' + (ch.s === 2 ? ' tripped' : ch.s === 1 ? ' warning' : ch.r ? '' : ' off');

    const statusLabel = ['OK', 'WARNING', 'TRIPPED', 'DISABLED'][ch.s] || 'UNKNOWN';
    const statusClass = ['ok', 'warning', 'tripped', ''][ch.s] || '';

    card.innerHTML = `
      <div class="card-header">
        <span class="ch-name">${escHtml(ch.n)}</span>
        <span class="ch-status ${statusClass}">${statusLabel}</span>
      </div>
      <div class="ch-stats">
        <div><span class="label">Current</span><br><span class="value">${ch.a.toFixed(2)}</span> <span class="unit">A</span></div>
        <div><span class="label">Power</span><br><span class="value">${ch.w.toFixed(1)}</span> <span class="unit">W</span></div>
        <div><span class="label">Apparent</span><br><span class="value">${ch.va.toFixed(1)}</span> <span class="unit">VA</span></div>
        <div><span class="label">Power Factor</span><br><span class="value">${ch.pf.toFixed(3)}</span></div>
        <div><span class="label">Energy</span><br><span class="value">${ch.kwh.toFixed(3)}</span> <span class="unit">kWh</span></div>
        <div><span class="label">Relay</span><br><span class="relay-indicator ${ch.r ? 'relay-on' : 'relay-off'}">${ch.r ? 'ON' : 'OFF'}</span></div>
      </div>
      <div class="ch-controls">
        <input type="number" id="clim_${i}" value="${ch.cl}" step="0.5" placeholder="Cur limit (A)" title="Current limit">
        <input type="number" id="plim_${i}" value="${ch.pl}" step="100" placeholder="Pwr limit (W)" title="Power limit">
        <input type="text" id="name_${i}" value="${escHtml(ch.n)}" placeholder="Name" title="Channel name">
        <button onclick="setLimit(${i})">Limit</button>
        ${ch.s === 2 ? `<button class="reset" onclick="resetRelay(${i})">Reset</button>` : ''}
      </div>`;
    container.appendChild(card);
  }

  const eventList = document.getElementById('eventList');
  eventList.innerHTML = '';
  if (data.events) {
    for (const ev of data.events.slice(-10).reverse()) {
      const div = document.createElement('div');
      const statusClass = ev.s === 2 ? 'tripped' : ev.s === 1 ? 'warning' : '';
      div.className = 'event ' + statusClass;
      div.innerHTML = `<span class="time">${formatTime(ev.t)}</span><span class="ch-tag">Ch${ev.c+1}</span><span class="msg">${escHtml(ev.m)}</span>`;
      eventList.appendChild(div);
    }
  }

  if (document.getElementById('voltCal').value === '') document.getElementById('voltCal').value = data.voltageCalibration || 260;
  if (document.getElementById('currCal').value === '') document.getElementById('currCal').value = data.currentCalibration || 100;
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

function sendCal(cmd, inputId) {
  const val = parseFloat(document.getElementById(inputId).value);
  if (ws && ws.readyState === WebSocket.OPEN && !isNaN(val)) {
    ws.send(JSON.stringify({cmd:cmd, val:val}));
  }
}

function formatUptime(s) {
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  return String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0') + ':' + String(sec).padStart(2,'0');
}

function formatTime(ms) {
  const d = new Date(ms);
  return d.toLocaleTimeString();
}

function escHtml(s) {
  if (!s) return '';
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

connectWS();
