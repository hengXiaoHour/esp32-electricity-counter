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
      if (data.cmd === 'log_list') {
        renderLogList(data.files);
      } else if (data.cmd === 'log_data') {
        openLogViewer(data.date, data.csv);
      } else {
        updateDashboard(data);
      }
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

  // Sync voltage calibration from ESP32 (don't overwrite while user is typing/setting)
  const voltCalInput = document.getElementById('voltCal');
  if (voltCalInput && document.activeElement !== voltCalInput && !voltCalInput.dataset.userSet && typeof data.voltageCalibration === 'number') {
    voltCalInput.value = data.voltageCalibration.toFixed(1);
  }

  // Sync calibration values from ESP32 (don't overwrite while user is typing)
  const syncField = (id, val, decimals) => {
    const inp = document.getElementById(id);
    if (inp && document.activeElement !== inp && !inp.dataset.userSet && typeof val === 'number') inp.value = val.toFixed(decimals);
  };
  if (Array.isArray(data.currentCalibration)) {
    data.currentCalibration.forEach((v, i) => syncField(`currCal_${i}`, v, 1));
  }
  if (Array.isArray(data.noiseFloor)) {
    data.noiseFloor.forEach((v, i) => syncField(`nf_${i}`, v, 3));
  }

  const rmsInput = document.getElementById('rmsSamples');
  if (rmsInput && typeof data.rmsSamples === 'number' && !rmsInput.dataset.userSet) {
    rmsInput.value = data.rmsSamples;
  }

  // Initialize collapsible calibration sections once
  const calContainer = document.getElementById('calibrationRows');
  if (calContainer && Array.isArray(data.ch) && calContainer.children.length === 0) {
    data.ch.forEach((_, idx) => {
      const currCal = (data.currentCalibration && data.currentCalibration[idx]) || 100;
      const nf = (data.noiseFloor && data.noiseFloor[idx]) || 0;

      const header = document.createElement('div');
      header.className = 'cal-collapse-header';
      header.innerHTML = `<span class="arrow">&#9654;</span> Ch${idx+1}`;
      header.onclick = () => {
        const body = header.nextElementSibling;
        const isOpen = body.classList.toggle('open');
        header.classList.toggle('expanded', isOpen);
      };

      const body = document.createElement('div');
      body.className = 'cal-collapse-body';
      body.innerHTML = `
        <div class="cal-param-row">
          <label>Current Cal:</label>
          <input type="number" id="currCal_${idx}" step="0.1" value="${currCal}" oninput="this.dataset.userSet='true'">
          <button class="btn-sm" onclick="sendCurrentCal(${idx})">Set</button>
        </div>
        <div class="cal-param-row">
          <label>Noise Floor:</label>
          <input type="number" id="nf_${idx}" step="0.001" value="${nf}" oninput="this.dataset.userSet='true'">
          <button class="btn-sm" onclick="autoZeroChannel(${idx})" style="color:#e67e22;">Auto-Zero</button>
        </div>
      `;

      calContainer.appendChild(header);
      calContainer.appendChild(body);
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
        <span>Monthly Budget</span>
        <span class="bar-pct">0%</span>
      </div>
      <div class="progress-track">
        <div class="progress-fill bar-fill"></div>
      </div>
    </div>
    <div class="ch-submetrics">
      <span>PF: <strong class="val-pf mono">1.00</strong></span>
      <span class="limit-text mono">Limit: -- kWh/mo</span>
    </div>
    <div class="card-actions">
      <button class="btn-sm btn-edit" onclick="openEditModal(${idx})">Edit</button>
      <button class="btn-sm btn-reset" onclick="handleResetRelay(${idx})">&#8634; Reset Counter</button>
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
  const monthlyKwhLimit = typeof ch.mkwh === 'number' ? ch.mkwh : 48;

  const valA = card.querySelector('.val-a');
  if (valA) valA.textContent = currentVal.toFixed(2);

  const valW = card.querySelector('.val-w');
  if (valW) valW.textContent = powerVal.toFixed(0);

  const valKwh = card.querySelector('.val-kwh');
  if (valKwh) valKwh.textContent = kwhVal.toFixed(2);

  const valPf = card.querySelector('.val-pf');
  if (valPf) valPf.textContent = pfVal.toFixed(2);

  // Monthly kWh Budget Bar
  const pct = Math.min(100, Math.max(0, (kwhVal / (monthlyKwhLimit || 48)) * 100));
  const barPct = card.querySelector('.bar-pct');
  if (barPct) barPct.textContent = pct.toFixed(0) + '%';

  const fill = card.querySelector('.bar-fill');
  if (fill) {
    fill.style.width = pct + '%';
    fill.className = 'progress-fill ' + (pct > 90 ? 'over' : pct > 75 ? 'warn' : '');
  }

  // Monthly kWh Budget Text
  const limitText = card.querySelector('.limit-text');
  if (limitText) limitText.textContent = `Limit: ${monthlyKwhLimit.toFixed(1)} kWh/mo`;
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
  document.getElementById('modalChMkwh').value = typeof ch.mkwh === 'number' ? ch.mkwh : 48;
  
  document.getElementById('editModal').classList.remove('hidden');
}

function closeEditModal() {
  activeEditChIdx = null;
  document.getElementById('editModal').classList.add('hidden');
}

function resetModalToDefaults() {
  if (activeEditChIdx === null) return;
  const idx = activeEditChIdx;
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ cmd: 'reset_ch_to_default', ch: idx }));
  }
  closeEditModal();
  showToast(`Channel ${idx+1} reset to defaults`);
}

function saveModalSettings() {
  if (activeEditChIdx === null) return;
  const idx = activeEditChIdx;
  
  const name = document.getElementById('modalChName').value.trim();
  const mkwh = parseFloat(document.getElementById('modalChMkwh').value);

  if (ws && ws.readyState === WebSocket.OPEN) {
    if (!isNaN(mkwh)) ws.send(JSON.stringify({ cmd: 'set_monthly_kwh', ch: idx, val: mkwh }));
    if (name) ws.send(JSON.stringify({ cmd: 'set_name', ch: idx, name: name }));
  } else if (isDemoMode && latestChannelData[idx]) {
    if (!isNaN(mkwh)) latestChannelData[idx].mkwh = mkwh;
    if (name) latestChannelData[idx].n = name;
  }

  closeEditModal();
}

function showToast(msg) {
  let toast = document.getElementById('toast');
  if (!toast) {
    toast = document.createElement('div');
    toast.id = 'toast';
    document.body.appendChild(toast);
  }
  toast.textContent = msg;
  toast.className = 'toast show';
  clearTimeout(toast._hide);
  toast._hide = setTimeout(() => { toast.className = 'toast'; }, 2500);
}

function handleResetRelay(idx) {
  const card = document.querySelector(`.card[data-ch="${idx}"]`);
  const btn = card.querySelector('.btn-reset');

  if (pendingActions[idx] === 'confirm') {
    delete pendingActions[idx];
    btn.innerHTML = '&#8634; Reset Counter';
    btn.classList.remove('confirming');
    
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ cmd: 'reset_relay', ch: idx }));
      showToast(`Channel ${idx+1}: limits reset, energy cleared, relay OFF`);
    }
  } else {
    pendingActions[idx] = 'confirm';
    btn.textContent = 'Confirm?';
    btn.classList.add('confirming');
    setTimeout(() => {
      delete pendingActions[idx];
      btn.innerHTML = '&#8634; Reset Counter';
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

function resetChannelNames() {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ cmd: 'reset_channel_names' }));
    showToast('Resetting all channel names...');
  } else {
    showToast('Not connected');
  }
}

function sendVoltageCal() {
  const val = parseFloat(document.getElementById('voltCal').value);
  if (isNaN(val)) return showToast('Invalid voltage calibration');
  delete document.getElementById('voltCal').dataset.userSet;
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ cmd: 'set_voltage_cal', val }));
    showToast(`Voltage cal set to ${val.toFixed(1)}`);
  } else {
    showToast('Not connected');
  }
}

function sendCurrentCal(idx) {
  const val = parseFloat(document.getElementById(`currCal_${idx}`).value);
  if (isNaN(val)) return showToast('Invalid current calibration');
  delete document.getElementById(`currCal_${idx}`).dataset.userSet;
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ cmd: 'set_current_cal', ch: idx, val }));
    showToast(`Ch${idx+1} current cal set to ${val.toFixed(1)}`);
  } else {
    showToast('Not connected');
  }
}

function autoZeroChannel(idx) {
  const nf = document.getElementById(`nf_${idx}`);
  if (nf) delete nf.dataset.userSet;
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ cmd: 'set_noise_floor', ch: idx }));
    showToast(`Ch${idx+1} auto-zero started (2s)...`);
  } else {
    showToast('Not connected');
  }
}

function setRmsSamples() {
  const inp = document.getElementById('rmsSamples');
  const val = parseInt(inp.value);
  if (isNaN(val) || val < 100 || val > 2000) return showToast('RMS Samples must be 100-2000');
  delete inp.dataset.userSet;
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ cmd: 'set_rms_samples', val }));
    showToast(`RMS Samples set to ${val}`);
  } else {
    showToast('Not connected');
  }
}

function handleResetNvs() {
  const btn = document.getElementById('resetNvsBtn');
  if (btn.dataset.confirm === 'true') {
    delete btn.dataset.confirm;
    btn.textContent = '\u21ba Reset NVS to Defaults';
    delete document.getElementById('rmsSamples').dataset.userSet;
    delete document.getElementById('voltCal').dataset.userSet;
    for (let i = 0; i < 6; i++) {
      const nf = document.getElementById(`nf_${i}`);
      if (nf) delete nf.dataset.userSet;
      const cc = document.getElementById(`currCal_${i}`);
      if (cc) delete cc.dataset.userSet;
    }
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ cmd: 'reset_nvs_defaults' }));
      showToast('NVS reset to defaults — values will reload from ESP32');
    }
  } else {
    btn.dataset.confirm = 'true';
    btn.textContent = 'Confirm?';
    setTimeout(() => {
      delete btn.dataset.confirm;
      btn.textContent = '\u21ba Reset NVS to Defaults';
    }, 3000);
  }
}

// ============ Data Logs ============

function fetchLogList() {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ cmd: 'get_logs' }));
  }
}

function fetchLogDate(date) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ cmd: 'get_logs', date: date }));
  }
}

function renderLogList(files) {
  const list = document.getElementById('logFileList');
  list.innerHTML = '';
  if (!files || files.length === 0) {
    list.innerHTML = '<span style="color: var(--text-dim); font-size: 0.75rem;">No logs available.</span>';
    return;
  }
  files.forEach(date => {
    const btn = document.createElement('span');
    btn.className = 'log-file-item';
    btn.textContent = date;
    btn.onclick = () => fetchLogDate(date);
    list.appendChild(btn);
  });
}

function openLogViewer(date, csv) {
  document.getElementById('logViewerTitle').textContent = date + '.csv';
  document.getElementById('logContent').textContent = csv || '(empty)';
  document.getElementById('logViewer').classList.remove('hidden');
}

function closeLogViewer() {
  document.getElementById('logViewer').classList.add('hidden');
  document.getElementById('logContent').textContent = '';
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
        { n: "Living Room AC", a: 4.8 + Math.random(), w: 1080 + Math.random()*20, kwh: 12.4, pf: 0.95, mkwh: 48, s: 0, r: true },
        { n: "Kitchen Oven", a: 8.2 + Math.random(), w: 1870 + Math.random()*30, kwh: 3.8, pf: 0.99, mkwh: 48, s: 1, r: true },
        { n: "Water Heater", a: 0.0, w: 0.0, kwh: 5.1, pf: 0.0, mkwh: 48, s: 2, r: false },
        { n: "Server Rack", a: 1.2 + Math.random()*0.1, w: 270 + Math.random()*5, kwh: 48.2, pf: 0.92, mkwh: 48, s: 0, r: true }
      ],
      events: demoEvents
    };
    updateDashboard(mockData);
  }, 1000);
}
