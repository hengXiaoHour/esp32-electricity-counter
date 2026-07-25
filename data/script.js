let ws = null;
let currentIP = '';
let manualDisconnect = false;
let activeEditChIdx = null;
let latestChannelData = [];
let pendingActions = {};
let isDemoMode = false;
let demoInterval = null;
let lastEventKey = '';
let optimisticRelays = {};

// Initial Setup
(function init() {
  const savedIP = localStorage.getItem('esp32monitor_ip');
  if (savedIP) {
    document.getElementById('esp32Ip').value = savedIP;
  }
  
  // Close modal on Escape key
  window.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') closeEditModal();
  });
})();

function handleConnect() {
  if (isDemoMode) {
    startDemoMode();
    return;
  }
  const ip = document.getElementById('esp32Ip').value.trim();
  if (!ip) return;
  currentIP = ip;
  localStorage.setItem('esp32monitor_ip', ip);
  setConnectStatus('Connecting to ' + ip + '...', 'connecting');
  connectWS(ip);
}

function handleDisconnect() {
  manualDisconnect = true;
  if (demoInterval) clearInterval(demoInterval);
  if (ws) {
    ws.close();
    ws = null;
  }
  showConnectPanel();
}

function connectWS(ip) {
  if (ws) { ws.close(); ws = null; }
  const url = 'ws://' + ip + '/ws';
  
  ws = new WebSocket(url);
  ws.onopen = () => {
    const ipEl = document.getElementById('connectedIp');
    if (ipEl) ipEl.textContent = ip;
    document.getElementById('connStatus').textContent = '\u25cf Connected';
    showDashboard();
  };
  ws.onclose = () => {
    if (!manualDisconnect) {
      setConnectStatus('Disconnected \u2014 check IP address', 'disconnected');
      showConnectPanel();
    }
  };
  ws.onerror = () => {
    setConnectStatus('Connection error', 'disconnected');
  };
  ws.onmessage = (e) => {
    try {
      const data = JSON.parse(e.data);
      updateDashboard(data);
    } catch(err) { console.error('JSON Parse error', err); }
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
  if (el) {
    el.textContent = msg;
    el.className = 'connect-status ' + (cls || '');
  }
}

// Smart DOM-preserving Dashboard Update
function updateDashboard(data) {
  if (!data) return;
  if (Array.isArray(data.ch)) {
    latestChannelData = data.ch;
    // Restore optimistic relay states (overrides broadcast)
    Object.keys(optimisticRelays).forEach(idx => {
      if (latestChannelData[idx]) {
        latestChannelData[idx].r = optimisticRelays[idx];
      }
    });
  }

  // Update Header Totals
  const voltEl = document.getElementById('headerVoltage');
  if (voltEl) voltEl.textContent = (typeof data.v === 'number' ? data.v : 0).toFixed(1) + ' V';
  
  let totalP = 0;
  let totalI = 0;
  let hasTrip = false, hasWarn = false;

  if (Array.isArray(data.ch)) {
    data.ch.forEach(ch => {
      if (!ch) return;
      totalP += ch.w || 0;
      totalI += ch.a || 0;
      if (ch.s === 2) hasTrip = true;
      if (ch.s === 1) hasWarn = true;
    });
  }

  const totPEl = document.getElementById('totalPower');
  if (totPEl) totPEl.textContent = totalP.toFixed(0) + ' W';

  const totIEl = document.getElementById('totalCurrent');
  if (totIEl) totIEl.textContent = totalI.toFixed(2) + ' A';

  // LED Pilot Status
  const led = document.getElementById('ledStatus');
  if (led) {
    if (hasTrip) { led.className = 'led led-trip'; }
    else if (hasWarn) { led.className = 'led led-warn'; }
    else { led.className = 'led led-ok'; }
  }

  // Render / Update Channel Cards without destroying inputs
  const container = document.getElementById('channels');
  if (container && Array.isArray(data.ch)) {
    data.ch.forEach((ch, idx) => {
      let card = container.querySelector(`.card[data-ch="${idx}"]`);
      
      // Create DOM structure if not exists
      if (!card) {
        card = createChannelCardElement(idx);
        container.appendChild(card);
      }

      // Update inner content while preserving user text focus
      updateChannelCardElement(card, ch, idx);
    });
  }

  // Render Events
  if (data.events) renderEvents(data.events);

  // Initialize Calibration Rows once
  const calContainer = document.getElementById('currentCalRows');
  if (calContainer && Array.isArray(data.ch) && calContainer.children.length === 0) {
    data.ch.forEach((ch, idx) => {
      const div = document.createElement('div');
      div.className = 'cal-row';
      div.innerHTML = `
        <label>Ch ${idx+1}:</label>
        <input type="number" id="currCal_${idx}" step="0.1" value="${(data.currentCalibration && data.currentCalibration[idx]) || 100}">
        <button class="btn-sm" onclick="sendCurrentCal(${idx})">Set</button>
      `;
      calContainer.appendChild(div);
    });
  }
}

// Generate Static Structure for Card
function createChannelCardElement(idx) {
  const card = document.createElement('div');
  card.className = 'card';
  card.dataset.ch = idx;
  
  card.innerHTML = `
    <div class="card-header">
      <div style="display: flex; align-items: center; gap: 8px;">
        <span class="ch-title name-field">Channel ${idx+1}</span>
        <span class="ch-status-badge warning-badge hidden">WARN</span>
      </div>
      <button class="relay-toggle-btn off" onclick="toggleRelay(${idx})" title="Click to toggle relay state">
        RELAY OFF
      </button>
    </div>
    <div class="ch-readings">
      <div class="read-item">
        <span class="label">Current</span>
        <span class="val mono val-a">0.00</span><span class="unit"> A</span>
      </div>
      <div class="read-item">
        <span class="label">Power</span>
        <span class="val mono val-w">0</span><span class="unit"> W</span>
      </div>
      <div class="read-item">
        <span class="label">Energy</span>
        <span class="val mono val-kwh">0.00</span><span class="unit"> kWh</span>
      </div>
    </div>
    <div class="load-bar-container">
      <div class="bar-meta">
        <span>Load Capacity</span>
        <span class="bar-pct">0%</span>
      </div>
      <div class="progress-track">
        <div class="progress-fill bar-fill"></div>
      </div>
    </div>
    <div class="ch-submetrics">
      <span>PF: <strong class="val-pf mono">1.00</strong></span>
      <span class="limit-text mono">Max: -- A / -- W</span>
    </div>
    <div class="card-actions">
      <button class="btn-sm btn-edit" onclick="openEditModal(${idx})">Edit Limits</button>
      <button class="btn-sm btn-reset" onclick="handleResetRelay(${idx})">Reset Relay</button>
    </div>
  `;
  return card;
}

// Smart State Update
function updateChannelCardElement(card, ch, idx) {
  if (!card || !ch) return;
  
  // Card Status Class
  const statusClasses = ['ok', 'warning', 'tripped', 'off'];
  const statusIdx = typeof ch.s === 'number' ? ch.s : 0;
  card.className = 'card ' + (statusClasses[statusIdx] || 'ok');

  // Fields
  const nameEl = card.querySelector('.name-field');
  if (nameEl) nameEl.textContent = ch.n || ('Channel ' + (idx + 1));

  // Interactive Relay status button update
  const isRelayOn = ch.r !== undefined ? Boolean(ch.r) : false;
  const toggleBtn = card.querySelector('.relay-toggle-btn');
  if (toggleBtn) {
    toggleBtn.textContent = isRelayOn ? 'RELAY ON' : 'RELAY OFF';
    toggleBtn.className = 'relay-toggle-btn ' + (isRelayOn ? 'on' : 'off');
    toggleBtn.title = isRelayOn ? 'Click to turn Relay OFF' : 'Click to turn Relay ON';
  }

  // Secondary warning / tripped badge (shown next to title if tripped or warning)
  const warnBadge = card.querySelector('.warning-badge');
  if (warnBadge) {
    if (ch.s === 1) {
      warnBadge.textContent = 'WARN';
      warnBadge.className = 'ch-status-badge warning';
      warnBadge.classList.remove('hidden');
    } else if (ch.s === 2) {
      warnBadge.textContent = 'TRIPPED';
      warnBadge.className = 'ch-status-badge tripped';
      warnBadge.classList.remove('hidden');
    } else {
      warnBadge.classList.add('hidden');
    }
  }

  // Readings with safe fallbacks
  const currentVal = typeof ch.a === 'number' ? ch.a : 0;
  const powerVal = typeof ch.w === 'number' ? ch.w : 0;
  const kwhVal = typeof ch.kwh === 'number' ? ch.kwh : 0;
  const pfVal = typeof ch.pf === 'number' ? ch.pf : 1.0;
  const currentLimit = typeof ch.cl === 'number' ? ch.cl : 10;
  const powerLimit = typeof ch.pl === 'number' ? ch.pl : 2200;

  const valA = card.querySelector('.val-a');
  if (valA) valA.textContent = currentVal.toFixed(2);

  const valW = card.querySelector('.val-w');
  if (valW) valW.textContent = powerVal.toFixed(0);

  const valKwh = card.querySelector('.val-kwh');
  if (valKwh) valKwh.textContent = kwhVal.toFixed(2);

  const valPf = card.querySelector('.val-pf');
  if (valPf) valPf.textContent = pfVal.toFixed(2);

  // Capacity Bar Logic (based on Amps limit)
  const pct = Math.min(100, Math.max(0, (currentVal / (currentLimit || 10)) * 100));
  const barPct = card.querySelector('.bar-pct');
  if (barPct) barPct.textContent = pct.toFixed(0) + '%';

  const fill = card.querySelector('.bar-fill');
  if (fill) {
    fill.style.width = pct + '%';
    fill.className = 'progress-fill ' + (pct > 90 ? 'over' : pct > 75 ? 'warn' : '');
  }

  // Limits Text
  const limitText = card.querySelector('.limit-text');
  if (limitText) limitText.textContent = `Max: ${currentLimit.toFixed(1)}A / ${powerLimit.toFixed(0)}W`;
}

function toggleRelay(idx) {
  const ch = (latestChannelData && latestChannelData[idx]) || {};
  const currentRelay = ch.r !== undefined ? ch.r : false;
  const newState = !currentRelay;

  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ cmd: 'set_relay', ch: idx, state: newState }));
  }

  optimisticRelays[idx] = newState;

  if (latestChannelData[idx]) {
    latestChannelData[idx].r = newState;
    if (newState && latestChannelData[idx].s === 2) {
      latestChannelData[idx].s = 0;
    } else if (!newState && latestChannelData[idx].s === 0) {
      latestChannelData[idx].s = 3;
    }
    const card = document.querySelector(`.card[data-ch="${idx}"]`);
    if (card) updateChannelCardElement(card, latestChannelData[idx], idx);
  }

  setTimeout(() => delete optimisticRelays[idx], 1500);
}

function openEditModal(idx) {
  activeEditChIdx = idx;
  const ch = (latestChannelData && latestChannelData[idx]) || {};
  
  document.getElementById('modalTitle').textContent = `Configure Channel ${idx + 1}`;
  document.getElementById('modalChName').value = ch.n || `Channel ${idx + 1}`;
  document.getElementById('modalChClim').value = typeof ch.cl === 'number' ? ch.cl : 10;
  document.getElementById('modalChPlim').value = typeof ch.pl === 'number' ? ch.pl : 2200;
  
  document.getElementById('editModal').classList.remove('hidden');
}

function closeEditModal() {
  activeEditChIdx = null;
  document.getElementById('editModal').classList.add('hidden');
}

function saveModalSettings() {
  if (activeEditChIdx === null) return;
  const idx = activeEditChIdx;
  
  const clim = parseFloat(document.getElementById('modalChClim').value);
  const plim = parseFloat(document.getElementById('modalChPlim').value);
  const name = document.getElementById('modalChName').value.trim();

  if (ws && ws.readyState === WebSocket.OPEN) {
    if (!isNaN(clim)) ws.send(JSON.stringify({ cmd: 'set_limit', ch: idx, val: clim }));
    if (!isNaN(plim)) ws.send(JSON.stringify({ cmd: 'set_power_limit', ch: idx, val: plim }));
    if (name) ws.send(JSON.stringify({ cmd: 'set_name', ch: idx, name: name }));
  } else if (isDemoMode && latestChannelData[idx]) {
    // Immediate UI feedback in demo mode
    if (!isNaN(clim)) latestChannelData[idx].cl = clim;
    if (!isNaN(plim)) latestChannelData[idx].pl = plim;
    if (name) latestChannelData[idx].n = name;
  }

  closeEditModal();
}

function handleResetRelay(idx) {
  const card = document.querySelector(`.card[data-ch="${idx}"]`);
  const btn = card.querySelector('.btn-reset');

  if (pendingActions[idx] === 'confirm') {
    delete pendingActions[idx];
    btn.textContent = 'Resetting...';
    btn.classList.remove('confirming');
    
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ cmd: 'reset_relay', ch: idx }));
    }
  } else {
    pendingActions[idx] = 'confirm';
    btn.textContent = 'Confirm?';
    btn.classList.add('confirming');
    setTimeout(() => {
      delete pendingActions[idx];
      btn.textContent = 'Reset Relay';
      btn.classList.remove('confirming');
    }, 3000);
  }
}

function renderEvents(events) {
  if (!events) return;
  const key = events.map(e => `${e.t}_${e.c}_${e.s}_${e.m}`).join('|');
  if (key === lastEventKey) return;
  lastEventKey = key;

  const list = document.getElementById('eventList');
  list.innerHTML = '';
  document.getElementById('eventCount').textContent = events.length + ' events';

  events.slice(-8).reverse().forEach(ev => {
    const div = document.createElement('div');
    const type = ev.s === 2 ? 'tripped' : ev.s === 1 ? 'warning' : '';
    div.className = 'event-item ' + type;
    div.innerHTML = `
      <span class="time">${new Date(ev.t * 1000).toLocaleTimeString()}</span>
      <span class="tag">CH${ev.c + 1}</span>
      <span class="msg">${ev.m}</span>
    `;
    list.appendChild(div);
  });
}

function sendVoltageCal() {
  const val = parseFloat(document.getElementById('voltCal').value);
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ cmd: 'set_voltage_cal', val }));
  }
}

function sendCurrentCal(idx) {
  const val = parseFloat(document.getElementById(`currCal_${idx}`).value);
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ cmd: 'set_current_cal', ch: idx, val }));
  }
}

// Mock Simulation Mode for Local Browser Testing
function toggleDemoMode(cb) {
  isDemoMode = cb.checked;
}

function startDemoMode() {
  showDashboard();
  const ipEl = document.getElementById('connectedIp');
  if (ipEl) ipEl.textContent = 'Demo';
  document.getElementById('connStatus').textContent = '\u25cf Demo';

  const demoNow = Date.now() / 1000;
  const demoEvents = [
    { t: demoNow - 300, c: 2, s: 2, m: "Over-current trip triggered (>15A)" },
    { t: demoNow - 120, c: 1, s: 1, m: "High load warning (>80%)" }
  ];
  demoInterval = setInterval(() => {
    const mockData = {
      v: 228.4 + (Math.random() * 3 - 1.5),
      ch: [
        { n: "Living Room AC", a: 4.8 + Math.random(), w: 1080 + Math.random()*20, kwh: 12.4, pf: 0.95, cl: 10, pl: 2200, s: 0, r: true },
        { n: "Kitchen Oven", a: 8.2 + Math.random(), w: 1870 + Math.random()*30, kwh: 3.8, pf: 0.99, cl: 10, pl: 2000, s: 1, r: true },
        { n: "Water Heater", a: 0.0, w: 0.0, kwh: 5.1, pf: 0.0, cl: 15, pl: 3500, s: 2, r: false },
        { n: "Server Rack", a: 1.2 + Math.random()*0.1, w: 270 + Math.random()*5, kwh: 48.2, pf: 0.92, cl: 5, pl: 1000, s: 0, r: true }
      ],
      events: demoEvents
    };
    updateDashboard(mockData);
  }, 1000);
}
