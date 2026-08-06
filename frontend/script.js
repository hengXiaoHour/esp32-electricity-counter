let ws = null;
let currentIP = '';
let userDisconnect = false;
let activeEditChIdx = null;
let latestChannelData = [];
let pendingActions = {};
let isDemoMode = false;
let demoInterval = null;
let lastEventKey = '';
let lastToastEventKey = '';
let chartBuf = {};
const CHART_MAX = 240;
const CHART_DEFS = {
  'V': { label: 'Voltage (V)', color: '#3b82f6' },
  'P': { label: 'Total Power (W)', color: '#5a8aa0' },
  'I': { label: 'Total Current (A)', color: '#9a9a9a' }
};

const NUM_CHANNELS = 6;

// Initial Setup
(function init() {
  try {
    const savedIP = localStorage.getItem('esp32monitor_ip');
    if (savedIP) {
      document.getElementById('esp32Ip').value = savedIP;
    }
  } catch (e) { /* localStorage unavailable (e.g. file://) */ }

  // Close modal on Escape key
  window.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') closeEditModal();
  });

  initCharts();

  window.addEventListener('resize', () => {
    if (document.getElementById('page-analytics').classList.contains('active')) {
      drawAllCharts();
    }
  });
})();

// ============ Page Navigation ============
function showPage(name) {
  const pages = document.querySelectorAll('.page');
  pages.forEach(p => p.classList.toggle('active', p.id === 'page-' + name));
  const navs = document.querySelectorAll('.nav-item, .mnav-item');
  navs.forEach(n => n.classList.toggle('active', n.dataset.page === name));
  if (name === 'analytics') drawAllCharts();
}

function toggleSidebar() {
  const app = document.getElementById('app');
  app.classList.toggle('side-collapsed');
  const btn = document.getElementById('menuToggle');
  if (btn) btn.title = app.classList.contains('side-collapsed') ? 'Expand sidebar' : 'Collapse sidebar';
}

// ============ Connection ============
function handleConnect() {
  if (isDemoMode) {
    startDemoMode();
    return;
  }
  const ip = document.getElementById('esp32Ip').value.trim();
  if (!ip) return;
  currentIP = ip;
  try { localStorage.setItem('esp32monitor_ip', ip); } catch (e) {}
  setConnectStatus('Connecting to ' + ip + '...', 'connecting');
  connectWS(ip);
}

function handleDisconnect() {
  userDisconnect = true;
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
    document.getElementById('connStatus').textContent = 'Connected';
    const cs2 = document.getElementById('connStatus2');
    if (cs2) cs2.textContent = 'Connected';
    const wifi = document.querySelector('.wifi');
    if (wifi) wifi.setAttribute('class', 'wifi lv0');
    showDashboard();
  };
  ws.onclose = () => {
    if (!userDisconnect) {
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
    } catch (err) { console.error('JSON Parse error', err); }
  };
}

function showDashboard() {
  document.getElementById('connectPanel').classList.add('hidden');
  document.getElementById('app').classList.remove('hidden');
}

function showConnectPanel() {
  document.getElementById('app').classList.add('hidden');
  document.getElementById('connectPanel').classList.remove('hidden');
}

function setConnectStatus(msg, cls) {
  const el = document.getElementById('connectStatus');
  if (el) {
    el.textContent = msg;
    el.className = 'connect-status ' + (cls || '');
  }
}

// ============ WiFi signal strength (real RSSI from ESP32 STA mode) ============
function rssiLevel(rssi) {
  if (rssi >= -60) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

function updateWifiIcon(data) {
  const wifi = document.querySelector('.wifi');
  if (!wifi) return;
  const ap = !!data.ap;
  if (ap) {
    wifi.setAttribute('class', 'wifi lv2');
    wifi.setAttribute('title', 'Access Point mode');
    return;
  }
  if (!data.wifi || typeof data.rssi !== 'number') {
    wifi.setAttribute('class', 'wifi lv0 disconnected');
    wifi.setAttribute('title', 'Not connected');
    return;
  }
  const lv = rssiLevel(data.rssi);
  wifi.setAttribute('class', 'wifi lv' + lv);
  wifi.setAttribute('title', 'RSSI ' + data.rssi + ' dBm');
}

// ============ Smart DOM-preserving Dashboard Update ============
function updateDashboard(data) {
  if (!data) return;
  if (Array.isArray(data.ch)) {
    latestChannelData = data.ch;
  }

  // Header totals
  const voltEl = document.getElementById('headerVoltage');
  if (voltEl) voltEl.textContent = (typeof data.v === 'number' ? data.v : 0).toFixed(1);

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

  // Date + device time
  const deviceTimeEl = document.getElementById('deviceTime');
  if (deviceTimeEl) {
    const epoch = data.epoch;
    deviceTimeEl.textContent = (typeof epoch === 'number' && epoch > 1600000000)
      ? new Date(epoch * 1000).toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' })
      : '--';
  }

  const todayEl = document.getElementById('todayDate');
  if (todayEl) {
    const epoch = data.epoch;
    if (typeof epoch === 'number' && epoch > 1600000000) {
      const d = new Date(epoch * 1000);
      const dd = String(d.getDate()).padStart(2, '0');
      const mon = d.toLocaleString('en-US', { month: 'short' }).toUpperCase();
      todayEl.textContent = `${dd} ${mon} ${d.getFullYear()}`;
    } else {
      todayEl.textContent = '--';
    }
  }

  if (data.firmwareVersion) {
    const fw = 'v' + data.firmwareVersion;
    const fw2El = document.getElementById('fwVersion2');
    if (fw2El) fw2El.textContent = fw;
  }

  const lmEl = document.getElementById('lastMonth');
  if (lmEl && typeof data.lastMonth === 'number') lmEl.textContent = data.lastMonth;

  // System status LED
  const led = document.getElementById('sysLed');
  if (led) {
    if (hasTrip) { led.className = 'led led-trip'; }
    else if (hasWarn) { led.className = 'led led-warn'; }
    else { led.className = 'led led-ok'; }
  }

  updateWifiIcon(data);

  // Channel cards (DOM-preserving)
  const container = document.getElementById('channels');
  if (container && Array.isArray(data.ch)) {
    for (let i = 0; i < NUM_CHANNELS; i++) {
      let card = container.querySelector(`.channel-card[data-ch="${i}"]`);
      if (!card) {
        card = createChannelCardElement(i);
        container.appendChild(card);
      }
      updateChannelCardElement(card, data.ch[i], i);
    }
  }

  // Events
  if (data.events) renderEvents(data.events);

  // ntfy sync
  if (data.ntfy) {
    const topicInput = document.getElementById('ntfyTopic');
    if (topicInput && document.activeElement !== topicInput && !topicInput.dataset.userSet && typeof data.ntfy.topic === 'string') {
      topicInput.value = data.ntfy.topic;
    }
    const enInput = document.getElementById('ntfyEnabled');
    if (enInput && !enInput.dataset.userSet && typeof data.ntfy.enabled === 'boolean') {
      enInput.checked = data.ntfy.enabled;
    }
  }

  // Voltage calibration sync
  const voltCalInput = document.getElementById('voltCal');
  if (voltCalInput && document.activeElement !== voltCalInput && !voltCalInput.dataset.userSet && typeof data.voltageCalibration === 'number') {
    voltCalInput.value = data.voltageCalibration.toFixed(1);
  }

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

  // Build calibration collapsible rows once
  const calContainer = document.getElementById('calibrationRows');
  if (calContainer && Array.isArray(data.ch) && calContainer.children.length === 0) {
    data.ch.forEach((_, idx) => {
      const currCal = (data.currentCalibration && data.currentCalibration[idx]) || 100;
      const nf = (data.noiseFloor && data.noiseFloor[idx]) || 0;

      const header = document.createElement('div');
      header.className = 'cal-collapse-header';
      header.innerHTML = `<span class="arrow">&#9654;</span> Ch${idx + 1}`;
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

  // OTA progress
  const otaPanel = document.getElementById('otaPanel');
  if (otaPanel) {
    if (data.ota === true) {
      otaPanel.classList.remove('hidden');
      const p = Math.max(0, Math.min(100, data.otaProgress || 0));
      const fill = document.getElementById('otaProgress');
      if (fill) fill.style.width = p + '%';
      const pct = document.getElementById('otaPercent');
      if (pct) pct.textContent = p + '%';
      if (data.firmwareVersion) {
        const ov = document.getElementById('otaVersion');
        if (ov) ov.textContent = 'v' + data.firmwareVersion;
      }
    } else {
      otaPanel.classList.add('hidden');
    }
  }

  // Charts
  pushSamples(data, totalP, totalI);
  drawAllCharts();
}

// ============ Channel Cards (independent) ============
function createChannelCardElement(idx) {
  const card = document.createElement('div');
  card.className = 'card channel-card state-ok';
  card.dataset.ch = idx;
  card.innerHTML = `
    <div class="card-header">
      <span class="ch-title name-field">Channel ${idx + 1}</span>
      <span class="ch-status-badge ok">OK</span>
    </div>
    <div class="ch-readings">
      <div class="read-row"><span class="label">PWR</span><span class="val mono"><span class="val-w">0</span><span class="unit">W</span></span></div>
      <div class="read-row"><span class="label">CUR</span><span class="val mono"><span class="val-a">0.00</span><span class="unit">A</span></span></div>
      <div class="read-row"><span class="label">ENG</span><span class="val mono"><span class="val-kwh">0.00</span><span class="unit">kWh</span></span></div>
    </div>
    <div class="load-bar-container">
      <div class="bar-meta"><span class="limit-text mono">LIMIT -- kWh/mo</span><span class="bar-pct">0%</span></div>
      <div class="progress-track"><div class="progress-fill bar-fill"></div></div>
    </div>
    <div class="ch-submetrics">
      <span>PF: <strong class="val-pf mono">1.00</strong></span>
      <span class="mono">Monthly</span>
    </div>
    <div class="card-actions">
      <button class="btn-sm" onclick="openEditModal(${idx})">Edit</button>
      <button class="btn-sm btn-reset" onclick="handleResetChannel(${idx})">&#8634; Reset Counter</button>
    </div>`;
  return card;
}

function updateChannelCardElement(card, ch, idx) {
  if (!card || !ch) return;

  const statusClasses = ['state-ok', 'state-warn', 'state-trip', 'state-off'];
  const statusIdx = typeof ch.s === 'number' ? ch.s : 0;
  card.className = 'card channel-card ' + (statusClasses[statusIdx] || 'state-ok');
  card.dataset.ch = idx;

  const nameEl = card.querySelector('.name-field');
  if (nameEl) nameEl.textContent = ch.n || ('Channel ' + (idx + 1));

  const badge = card.querySelector('.ch-status-badge');
  if (badge) {
    if (ch.s === 1) {
      badge.textContent = 'WARN';
      badge.className = 'ch-status-badge warning';
    } else if (ch.s === 2) {
      badge.textContent = 'TRIPPED';
      badge.className = 'ch-status-badge tripped';
    } else if (ch.s === 3) {
      badge.textContent = 'OFF';
      badge.className = 'ch-status-badge off';
    } else {
      badge.textContent = 'OK';
      badge.className = 'ch-status-badge ok';
    }
  }

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

  const pct = Math.min(100, Math.max(0, (kwhVal / (monthlyKwhLimit || 48)) * 100));
  const barPct = card.querySelector('.bar-pct');
  if (barPct) barPct.textContent = pct.toFixed(0) + '%';

  const fill = card.querySelector('.bar-fill');
  if (fill) {
    fill.style.width = pct + '%';
    fill.className = 'progress-fill ' + (pct > 90 ? 'over' : pct > 75 ? 'warn' : '');
  }

  const limitText = card.querySelector('.limit-text');
  if (limitText) limitText.textContent = `LIMIT ${monthlyKwhLimit.toFixed(1)} kWh/mo`;
}

// ============ Edit / Reset Modal ============
function populateChannelSelect() {
  const sel = document.getElementById('modalChSelect');
  sel.innerHTML = '';
  for (let idx = 0; idx < NUM_CHANNELS; idx++) {
    const ch = (latestChannelData && latestChannelData[idx]) || {};
    const name = ch.n || ('Channel ' + (idx + 1));
    const opt = document.createElement('option');
    opt.value = idx;
    opt.textContent = `${idx + 1}. ${name}`;
    sel.appendChild(opt);
  }
}

function setModalMode(mode) {
  const editMode = mode !== 'reset';
  document.getElementById('modalNameGroup').style.display = editMode ? '' : 'none';
  document.getElementById('modalMkwhGroup').style.display = editMode ? '' : 'none';
  document.getElementById('modalResetDefaultBtn').style.display = editMode ? '' : 'none';
  document.getElementById('modalSaveBtn').style.display = editMode ? '' : 'none';
  const resetBtn = document.getElementById('modalResetCounterBtn');
  resetBtn.style.display = editMode ? 'none' : '';
  document.getElementById('modalTitle').textContent = editMode ? 'Configure Channel' : 'Reset Counter';
}

function openEditModal(idx) {
  populateChannelSelect();
  const sel = document.getElementById('modalChSelect');
  sel.value = idx;
  onModalChannelChange();
  setModalMode('edit');
  document.getElementById('editModal').classList.remove('hidden');
}

function openResetModal(idx) {
  populateChannelSelect();
  const sel = document.getElementById('modalChSelect');
  sel.value = idx;
  onModalChannelChange();
  setModalMode('reset');
  document.getElementById('editModal').classList.remove('hidden');
}

function onModalChannelChange() {
  const idx = parseInt(document.getElementById('modalChSelect').value, 10);
  activeEditChIdx = idx;
  const ch = (latestChannelData && latestChannelData[idx]) || {};
  document.getElementById('modalChName').value = ch.n || ('Channel ' + (idx + 1));
  document.getElementById('modalChMkwh').value = typeof ch.mkwh === 'number' ? ch.mkwh : 48;
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
  showToast(`Channel ${idx + 1} reset to defaults`);
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

function zeroSubCardDisplay(idx) {
  const card = document.querySelector(`.channel-card[data-ch="${idx}"]`);
  if (!card) return;
  const valKwh = card.querySelector('.val-kwh');
  if (valKwh) valKwh.textContent = '0.00';
  const barPct = card.querySelector('.bar-pct');
  if (barPct) barPct.textContent = '0%';
  const fill = card.querySelector('.bar-fill');
  if (fill) {
    fill.style.width = '0%';
    fill.className = 'progress-fill';
  }
}

function sendResetCounter(idx) {
  if (latestChannelData && latestChannelData[idx]) {
    latestChannelData[idx].kwh = 0;
  }
  zeroSubCardDisplay(idx);
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ cmd: 'reset_counter', ch: idx }));
  }
}

function handleResetChannel(idx) {
  const card = document.querySelector(`.channel-card[data-ch="${idx}"]`);
  const btn = card ? card.querySelector('.btn-reset') : null;
  const key = 'ch_' + idx;

  if (pendingActions[key] === 'confirm') {
    delete pendingActions[key];
    if (btn) {
      btn.innerHTML = '&#8634; Reset Counter';
      btn.classList.remove('confirming');
    }
    sendResetCounter(idx);
    showToast(`Channel ${idx + 1}: counter reset to 0 kWh`);
  } else {
    pendingActions[key] = 'confirm';
    if (btn) {
      btn.textContent = 'Confirm?';
      btn.classList.add('confirming');
    }
    setTimeout(() => {
      delete pendingActions[key];
      if (btn) {
        btn.innerHTML = '&#8634; Reset Counter';
        btn.classList.remove('confirming');
      }
    }, 3000);
  }
}

function confirmModalReset() {
  const idx = activeEditChIdx;
  if (idx === null) return;
  sendResetCounter(idx);
  closeEditModal();
  showToast(`Channel ${idx + 1}: counter reset to 0 kWh`);
}

// ============ Event Log ============
function renderEvents(events) {
  if (!events) return;
  const key = events.map(e => `${e.t}_${e.c}_${e.s}_${e.m}`).join('|');
  if (key === lastEventKey) return;
  lastEventKey = key;

  const latest = events[events.length - 1];
  if (latest && latest.m) {
    const m = String(latest.m);
    if (m.indexOf('limit') !== -1 || m.indexOf('Monthly reset') !== -1) {
      const tkey = `${latest.t}_${latest.c}_${latest.m}`;
      if (tkey !== lastToastEventKey) {
        lastToastEventKey = tkey;
        showToast(`Ch${latest.c + 1}: ${latest.m}`);
      }
    }
  }

  const list = document.getElementById('eventList');
  list.innerHTML = '';
  const countEl = document.getElementById('eventCount');
  if (countEl) countEl.textContent = events.length + ' events';

  if (!events.length) {
    const div = document.createElement('div');
    div.className = 'event-item empty';
    div.textContent = 'No events logged yet.';
    list.appendChild(div);
    return;
  }

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

// ============ Calibration + Settings Commands ============
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
    showToast(`Ch${idx + 1} current cal set to ${val.toFixed(1)}`);
  } else {
    showToast('Not connected');
  }
}

function autoZeroChannel(idx) {
  const nf = document.getElementById(`nf_${idx}`);
  if (nf) delete nf.dataset.userSet;
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ cmd: 'set_noise_floor', ch: idx }));
    showToast(`Ch${idx + 1} auto-zero started (2s)...`);
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

function sendNtfyTopic() {
  const inp = document.getElementById('ntfyTopic');
  const topic = inp.value.trim();
  delete inp.dataset.userSet;
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ cmd: 'set_ntfy_topic', val: topic }));
    showToast(topic ? `ntfy topic set to "${topic}"` : 'ntfy topic cleared');
  } else {
    showToast('Not connected');
  }
}

function sendNtfyEnabled(cb) {
  const on = cb.checked;
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ cmd: 'set_ntfy_enabled', val: on }));
    showToast(on ? 'Push notifications enabled' : 'Push notifications disabled');
  } else {
    cb.checked = !on;
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
      showToast('NVS reset to defaults \u2014 values will reload from ESP32');
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

// ============ Mock Demo Mode ============
function toggleDemoMode(cb) {
  isDemoMode = cb.checked;
}

function startDemoMode() {
  showDashboard();
  const ipEl = document.getElementById('connectedIp');
  if (ipEl) ipEl.textContent = 'Demo';
  document.getElementById('connStatus').textContent = 'Demo';
  const cs2 = document.getElementById('connStatus2');
  if (cs2) cs2.textContent = 'Demo';

  const demoNow = Date.now() / 1000;
  const demoEvents = [
    { t: demoNow - 300, c: 2, s: 2, m: "Monthly limit reached \u2014 over budget" },
    { t: demoNow - 120, c: 1, s: 1, m: "Approaching monthly limit" }
  ];
  demoInterval = setInterval(() => {
    const mockData = {
      v: 228.4 + (Math.random() * 3 - 1.5),
      wifi: true,
      ap: false,
      rssi: -55 - Math.floor(Math.random() * 20),
      firmwareVersion: '1.2.0',
      lastMonth: 202608,
      epoch: Math.floor(Date.now() / 1000),
      ota: false,
      ch: [
        { n: "Counter 1", a: 4.8 + Math.random(), w: 1080 + Math.random() * 20, kwh: 42.4, pf: 0.95, mkwh: 48, s: 0 },
        { n: "Counter 2", a: 0.0, w: 0.0, kwh: 1.2, pf: 0.0, mkwh: 48, s: 3 },
        { n: "Counter 3", a: 8.2 + Math.random(), w: 1870 + Math.random() * 30, kwh: 50.1, pf: 0.99, mkwh: 48, s: 2 },
        { n: "Counter 4", a: 5.1, w: 1100, kwh: 33.5, pf: 0.93, mkwh: 48, s: 1 },
        { n: "Kitchen Oven", a: 1.2 + Math.random() * 0.1, w: 270 + Math.random() * 5, kwh: 3.8, pf: 0.92, mkwh: 48, s: 0 },
        { n: "Server Rack", a: 0.0, w: 0.0, kwh: 5.1, pf: 0.0, mkwh: 48, s: 0 }
      ],
      events: demoEvents
    };
    updateDashboard(mockData);
  }, 1000);
}

// ============ Charts (client-side rolling) ============
function initCharts() {
  chartBuf = {};
  const summary = document.getElementById('summaryCharts');
  summary.innerHTML = '';
  ['V', 'P', 'I'].forEach(key => {
    chartBuf[key] = [];
    summary.appendChild(makeChartCell(key, CHART_DEFS[key].label));
  });

  const chGrid = document.getElementById('channelCharts');
  chGrid.innerHTML = '';
  for (let i = 0; i < NUM_CHANNELS; i++) {
    const key = 'ch' + i;
    chartBuf[key] = [];
    chGrid.appendChild(makeChartCell(key, 'Channel ' + (i + 1)));
  }
}

function makeChartCell(key, label) {
  const cell = document.createElement('div');
  cell.className = 'chart-cell';
  cell.dataset.buf = key;
  cell.innerHTML = `
    <div class="chart-head">
      <span class="ch-label">${label}</span>
      <span class="ch-now mono" id="chNow_${key}">--</span>
    </div>
    <canvas data-buf="${key}"></canvas>`;
  return cell;
}

function pushSamples(data, totalP, totalI) {
  if (typeof data.v === 'number') pushBuf('V', data.v);
  pushBuf('P', totalP);
  pushBuf('I', totalI);
  if (Array.isArray(data.ch)) {
    data.ch.forEach((ch, i) => {
      pushBuf('ch' + i, (ch && typeof ch.w === 'number') ? ch.w : 0);
    });
  }
  const rangeEl = document.getElementById('chartRange');
  if (rangeEl && chartBuf.P) rangeEl.textContent = `last ${chartBuf.P.length}s`;
}

function pushBuf(key, val) {
  if (!chartBuf[key]) chartBuf[key] = [];
  chartBuf[key].push(val);
  if (chartBuf[key].length > CHART_MAX) chartBuf[key].shift();
}

function drawAllCharts() {
  document.querySelectorAll('.chart-cell').forEach(cell => {
    const key = cell.dataset.buf;
    const canvas = cell.querySelector('canvas');
    const nowEl = document.getElementById('chNow_' + key);
    if (nowEl) {
      const def = CHART_DEFS[key];
      const last = chartBuf[key] && chartBuf[key].length ? chartBuf[key][chartBuf[key].length - 1] : null;
      if (last !== null) {
        nowEl.textContent = (key === 'V' ? last.toFixed(1) : key === 'I' ? last.toFixed(2) : last.toFixed(0));
      } else {
        nowEl.textContent = '--';
      }
    }
    drawChart(canvas, chartBuf[key] || [], CHART_DEFS[key] ? CHART_DEFS[key].color : '#5a8aa0');
  });
}

function drawChart(canvas, data, color) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth || 250;
  const h = canvas.clientHeight || 70;
  canvas.width = w * dpr;
  canvas.height = h * dpr;
  const ctx = canvas.getContext('2d');
  ctx.scale(dpr, dpr);
  ctx.clearRect(0, 0, w, h);

  if (!data || data.length < 2) {
    ctx.strokeStyle = 'rgba(255,255,255,0.06)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, h / 2);
    ctx.lineTo(w, h / 2);
    ctx.stroke();
    return;
  }

  let min = Math.min.apply(null, data);
  let max = Math.max.apply(null, data);
  if (max - min < 1e-9) { min -= 1; max += 1; }
  const pad = (max - min) * 0.12;
  min -= pad; max += pad;

  const step = w / (data.length - 1);
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.6;
  ctx.lineJoin = 'round';
  ctx.beginPath();
  data.forEach((v, i) => {
    const x = i * step;
    const y = h - ((v - min) / (max - min)) * (h - 4) - 2;
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.stroke();
}
