let ws = null;
let currentIP = '';
let userDisconnect = false;
let activeEditChIdx = null;
let latestChannelData = [];
let pendingActions = {};
let lastAzState = { active: false, channel: -1, queue: [] };
const AZ_BATCHES = 32;
let demoInterval = null;
let lastEventKey = '';
let lastToastEventKey = '';
let chartBuf = {};
let connMode = 'local';            // 'local' | 'cloud' | 'demo'
let cloudHeartbeat = null;        // eco-mode presence heartbeat timer
let cloudClientId = null;         // this tab's /viewers key
let cloudHeartbeatPath = null;    // full RTDB path of our heartbeat entry
let cloudDb = null;
let selectedDeviceId = null;       // board picked in the Cloud dropdown
let cloudLatestRef = null;
let cloudConsoleRef = null;
let consoleHistory = [];
let consoleHistIdx = -1;
const CONSOLE_MAX_LINES = 400;
let lastDataTs = 0;
let cloudWatchdog = null;
const CLOUD_STALE_MS = 10000;
const CHART_MAX = 240;
const CHART_DEFS = {
  'V': { label: 'Voltage (V)', color: '#3b82f6' },
  'P': { label: 'Total Power (W)', color: '#5a8aa0' },
  'I': { label: 'Total Current (A)', color: '#9a9a9a' }
};

const NUM_CHANNELS = 6;

// ============ Auth (Google) ============
let authUser = null;
let authAdmin = false;          // signed-in user email ∈ FB_CONFIG.adminEmails
let authInitialized = false;

function initAuth() {
  // Apply the guest baseline synchronously so admin controls never flash for
  // non-admins on reload; onAuthStateChanged upgrades to admin when it resolves.
  applyAuthState();
  if (!window.firebase || !window.FB_CONFIG || !window.FB_CONFIG.apiKey || !window.firebase.auth) {
    authInitialized = false;
    return;
  }
  try {
    if (!firebase.apps.length) {
      firebase.initializeApp({
        apiKey: window.FB_CONFIG.apiKey,
        authDomain: window.FB_CONFIG.authDomain,
        databaseURL: window.FB_CONFIG.databaseURL
      });
    }
    firebase.auth().onAuthStateChanged((user) => {
      authUser = user;
      authAdmin = !!(user && window.FB_CONFIG.adminEmails &&
                     window.FB_CONFIG.adminEmails.indexOf(user.email) !== -1);
      authInitialized = true;
      applyAuthState();
    });
  } catch (e) {
    console.error('Auth init failed', e);
    authInitialized = false;
    applyAuthState();
  }
}

function toggleGoogleAuth() {
  if (!window.firebase || !firebase.auth) return;
  if (authUser) {
    firebase.auth().signOut().catch((e) => showToast('Sign out failed: ' + e.message));
    return;
  }
  if (!window.FB_CONFIG || !window.FB_CONFIG.apiKey) {
    showToast('Google sign-in not configured (config.js)');
    return;
  }
  const provider = new firebase.auth.GoogleAuthProvider();
  firebase.auth().signInWithPopup(provider).catch((err) => {
    // popup-closed is not an error worth showing
    if (err.code !== 'auth/popup-closed-by-user' && err.code !== 'auth/cancelled-popup-request') {
      showToast('Sign in failed: ' + err.message);
    }
  });
}

// Apply the current role to the whole UI: badge, buttons, and admin-only gating.
function applyAuthState() {
  const guest = !authAdmin;

  // Connection is allowed for everyone; only command-sending is gated later.
  const badge = document.getElementById('roleBadge');
  if (badge) {
    badge.textContent = authAdmin ? 'Admin — full control' : 'Guest — read-only';
    badge.className = 'role-badge ' + (authAdmin ? 'role-admin' : 'role-guest');
  }
  const acctEmail = document.getElementById('acctEmail');
  if (acctEmail) acctEmail.textContent = authUser ? authUser.email : 'Not signed in';

  const sideEmail = document.getElementById('authEmail');
  if (sideEmail) {
    sideEmail.textContent = authUser ? authUser.email : '';
    const row = document.getElementById('authUserRow');
    if (row) row.classList.toggle('hidden', !authUser);
  }
  const authBtnSide = document.getElementById('authBtnSide');
  if (authBtnSide) {
    authBtnSide.querySelector('.nav-label').textContent = authUser ? 'Sign out of Google' : 'Sign in with Google';
  }
  const authBtnSettings = document.getElementById('authBtnSettings');
  if (authBtnSettings) {
    authBtnSettings.textContent = authUser ? 'Sign out of Google' : 'Sign in with Google';
  }

  document.body.classList.toggle('role-admin', authAdmin);
  document.body.classList.toggle('role-guest', guest);

  // If a channel edit modal is open and the user just lost admin, close it.
  if (guest && activeEditChIdx !== null) closeEditModal();

  // Role may govern cloud console access (guest = no /console read).
  syncCloudConsoleListener();

  const guestHint = document.getElementById('guestHint');
  if (guestHint) guestHint.style.display = guest ? 'inline' : 'none';
}

function requireAdmin() {
  if (authAdmin) return true;
  showToast(authUser
    ? 'This Google account is not the admin. Admin controls are locked.'
    : 'Sign in with the admin Google account to change settings.');
  return false;
}

// ============ PWA install prompt ============
let deferredInstallPrompt = null;

function isStandalone() {
  return window.matchMedia && window.matchMedia('(display-mode: standalone)').matches;
}

function showInstallRow() {
  const row = document.getElementById('installRow');
  if (row) row.classList.remove('hidden');
}

function hideInstallRow() {
  const row = document.getElementById('installRow');
  if (row) row.classList.add('hidden');
  const h = document.getElementById('installHint');
  if (h) h.classList.add('hidden');
}

window.addEventListener('beforeinstallprompt', (e) => {
  e.preventDefault();
  deferredInstallPrompt = e;
  showInstallRow();
});

window.addEventListener('appinstalled', () => {
  deferredInstallPrompt = null;
  hideInstallRow();
  showToast('App installed — launch it from your home screen');
});

function promptInstall() {
  // Android / installable: use the browser's install prompt.
  if (deferredInstallPrompt) {
    deferredInstallPrompt.prompt();
    deferredInstallPrompt.userChoice.then((choice) => {
      if (choice.outcome === 'accepted') hideInstallRow();
      deferredInstallPrompt = null;
    });
    return;
  }
  // Desktop: beforeinstallprompt doesn't fire on desktop Chrome — show guidance.
  const h = document.getElementById('installHint');
  if (h) h.classList.toggle('hidden');
}

// Initial Setup
(function init() {
  initAuth();

  try {
    const savedIP = localStorage.getItem('esp32monitor_ip');
    if (savedIP) {
      document.getElementById('esp32Ip').value = savedIP;
    }
  } catch (e) { /* localStorage unavailable (e.g. file://) */ }

  // Cloud is always the default connection mode. No persisted-mode restore.
  const modeSel = document.getElementById('connMode');
  if (modeSel) {
    modeSel.value = 'cloud';
    modeSel.addEventListener('change', updateConnectFields);
    updateConnectFields();
  }

  // If a live session was saved, jump straight into the dashboard.
  autoReconnect();

  // Show the install control on load (hidden only when already installed).
  if (isStandalone()) hideInstallRow();
  else showInstallRow();

  // Prefer a board the user picked before; fall back to config, then the
  // 192.168.100.3 board (esp-858428).
  try {
    const savedDev = localStorage.getItem('esp32monitor_device');
    selectedDeviceId = savedDev || (window.FB_CONFIG && window.FB_CONFIG.deviceId) || 'esp-858428';
  } catch (e) { /* localStorage unavailable */ }
  loadDevicePicker();

  const picker = document.getElementById('devicePicker');
  if (picker) {
    picker.addEventListener('change', () => {
      selectedDeviceId = picker.value || null;
      try { localStorage.setItem('esp32monitor_device', selectedDeviceId || ''); } catch (e) {}
      // Rebind live cloud refs to the newly selected board, if connected.
      if (connMode === 'cloud' && cloudDb) {
        if (cloudLatestRef) cloudLatestRef.off();
        if (cloudConsoleRef) cloudConsoleRef.off();
        connectCloud();
      }
    });
  }

  // Close modal on Escape key
  window.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') closeEditModal();
  });

  // Eco presence: explicit Disconnect already calls stopHeartbeat().
  // These cover the paths it misses — tab close/navigate (pagehide,
  // best-effort; onDisconnect is the backup) and background tabs
  // (pause heartbeat while hidden so the board drops to eco).
  window.addEventListener('pagehide', () => { try { stopHeartbeat(); } catch (e) {} });
  window.addEventListener('beforeunload', () => { try { stopHeartbeat(); } catch (e) {} });
  document.addEventListener('visibilitychange', () => {
    try {
      if (document.hidden) { stopHeartbeat(); }
      else if (connMode === 'cloud' && cloudDb) { startHeartbeat(); }
    } catch (e) {}
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
function connectLocal() {
  const ip = document.getElementById('esp32Ip').value.trim();
  if (!ip) return;
  currentIP = ip;
  connMode = 'local';
  try { localStorage.setItem('esp32monitor_ip', ip); } catch (e) {}
  setConnectStatus('Connecting to ' + ip + '...', 'connecting');
  connectWS(ip);
}

function handleConnect() {
  const mode = document.getElementById('connMode').value;
  document.body.classList.remove('conn-mode-demo');
  try { localStorage.setItem('esp32monitor_connmode', mode); } catch (e) {}
  if (mode === 'cloud') return connectCloud();
  if (mode === 'demo') return startDemoMode();
  return connectLocal();
}

function updateConnectFields() {
  const mode = document.getElementById('connMode').value;
  const ipRow = document.getElementById('ipRow');
  const cloudRow = document.getElementById('cloudRow');
  if (ipRow) ipRow.classList.toggle('hidden', mode !== 'local');
  if (cloudRow) cloudRow.classList.toggle('hidden', mode !== 'cloud');
}

function handleDisconnect() {
  userDisconnect = true;
  if (demoInterval) clearInterval(demoInterval);
  if (cloudWatchdog) clearInterval(cloudWatchdog);
  stopHeartbeat();
  if (cloudLatestRef && cloudDb) { cloudLatestRef.off(); cloudLatestRef = null; }
  if (cloudConsoleRef && cloudDb) { cloudConsoleRef.off(); cloudConsoleRef = null; }
  if (ws) {
    ws.close();
    ws = null;
  }
  connMode = 'local';
  document.body.classList.remove('conn-mode-demo');
  try { localStorage.removeItem('esp32monitor_session'); } catch (e) {}
  showConnectPanel();
}

// Persist a live session so a page reload reconnects straight into the
// dashboard (mode + ip + device), instead of landing back on the connect panel.
function saveSession(mode) {
  const s = { mode: mode };
  if (mode === 'local') {
    s.ip = document.getElementById('esp32Ip') ? document.getElementById('esp32Ip').value.trim() : currentIP;
  }
  if (mode === 'cloud') s.device = currentDeviceId();
  try { localStorage.setItem('esp32monitor_session', JSON.stringify(s)); } catch (e) {}
}

function autoReconnect() {
  let s = null;
  try { s = JSON.parse(localStorage.getItem('esp32monitor_session') || 'null'); } catch (e) { s = null; }
  if (!s || !s.mode) return;
  const modeSel = document.getElementById('connMode');
  if (modeSel) modeSel.value = s.mode;
  updateConnectFields();
  if (s.mode === 'cloud') {
    if (s.device) selectedDeviceId = s.device;
    connectCloud();
  } else if (s.mode === 'demo') {
    startDemoMode();
  } else if (s.mode === 'local') {
    if (s.ip) {
      document.getElementById('esp32Ip').value = s.ip;
      currentIP = s.ip;
    }
    connectLocal();
  }
}

// ============ Cloud (Firebase RTDB) mode ============

// Board-scoped path: every node lives under /devices/<chip-unique-id>/.
// The device picks its own id from its efuse MAC; the dashboard just needs
// to know which board it is watching (see loadDevicePicker above).
function cloudDevPath() {
  return 'devices/' + currentDeviceId();
}

// Keep the /console child_added listener in sync with the live role. Guests
// have no read on /console (security rules); attaching anyway would spam
// PERMISSION_DENIED errors, so only admins hold the listener.
function syncCloudConsoleListener() {
  if (!cloudDb || connMode !== 'cloud') return;
  if (cloudConsoleRef) {
    cloudConsoleRef.off('child_added');
    cloudConsoleRef = null;
  }
  if (authAdmin) {
    cloudConsoleRef = cloudDb.ref(cloudDevPath() + '/console');
    cloudConsoleRef.on('child_added', (snap) => {
      const raw = snap.val();
      const text = (typeof raw === 'string') ? raw
                 : (raw && typeof raw.out === 'string') ? raw.out : '';
      if (text.length) appendConsoleOutput(text);
      snap.ref.remove().catch(() => {});
    });
  }
}

function currentDeviceId() {
  if (selectedDeviceId) return selectedDeviceId;
  if (window.FB_CONFIG && window.FB_CONFIG.deviceId) return window.FB_CONFIG.deviceId;
  return 'esp-000000';
}

// Populate the board dropdown from RTDB /devices. Filters to boards that
// actually pushed a /latest snapshot (online). Keeps the user-selected or
// config-default board selected when present.
function loadDevicePicker() {
  const sel = document.getElementById('devicePicker');
  if (!sel) return;
  try {
    if (!window.firebase || !window.FB_CONFIG || !window.FB_CONFIG.databaseURL) return;
    if (!cloudDb) {
      if (!firebase.apps.length) firebase.initializeApp({ databaseURL: window.FB_CONFIG.databaseURL });
      cloudDb = firebase.database();
    }
    cloudDb.ref('devices').once('value').then((snap) => {
      const val = snap.val() || {};
      const cur = currentDeviceId();
      sel.innerHTML = '';
      let first = null;
      Object.keys(val).forEach((id) => {
        const dev = val[id];
        if (!dev || !dev.latest) return; // not live on cloud yet
        if (!first) first = id;
        const opt = document.createElement('option');
        opt.value = id;
        opt.textContent = id;
        sel.appendChild(opt);
      });
      const target = selectionMatches(cur, val) ? cur : (first || '');
      if (target) sel.value = target;
      selectedDeviceId = sel.value || null;
      try { if (selectedDeviceId) localStorage.setItem('esp32monitor_device', selectedDeviceId); } catch (e) {}
      if (sel.options.length === 0) {
        const opt = document.createElement('option');
        opt.value = '';
        opt.textContent = 'no boards online';
        sel.appendChild(opt);
      }
    }).catch(() => {});
  } catch (e) {}
}

function selectionMatches(id, val) {
  if (!id) return false;
  const dev = val[id];
  return !!(dev && dev.latest);
}

function connectCloud() {
  setConnectStatus('Connecting to Firebase...', 'connecting');
  try {
    if (!window.firebase || !window.FB_CONFIG || !window.FB_CONFIG.databaseURL) {
      throw new Error('config.js missing \u2014 create it from config.example.js');
    }
    if (!cloudDb) {
      if (!firebase.apps.length) firebase.initializeApp({ databaseURL: window.FB_CONFIG.databaseURL });
      cloudDb = firebase.database();
    }
    connMode = 'cloud';
    lastDataTs = Date.now();
    saveSession('cloud');
    const devPath = cloudDevPath();
    cloudLatestRef = cloudDb.ref(devPath + '/latest');
    cloudLatestRef.on('value', (snap) => {
      const val = snap.val();
      if (!val) return;
      lastDataTs = Date.now();
      updateDashboard(normalizeSnapshot(val));
    });

    // Console responses land under /devices/<id>/console/<commandKey>.
    // Print, then delete. Guests always lose the listener so no
    // PERMISSION_DENIED noise makes it up to the console.
    syncCloudConsoleListener();

    const ipEl = document.getElementById('connectedIp');
    if (ipEl) ipEl.textContent = 'Firebase RTDB / ' + currentDeviceId();
    document.getElementById('connStatus').textContent = 'Connected';
    const cs2 = document.getElementById('connStatus2');
    if (cs2) cs2.textContent = 'Connected';
    const wifi = document.querySelector('.wifi');
    if (wifi) wifi.setAttribute('class', 'wifi lv0');
    showDashboard();
    startCloudWatchdog();
    startHeartbeat();
  } catch (err) {
    setConnectStatus('Cloud connect failed: ' + err.message, 'disconnected');
  }
}

// RTDB stores arrays as objects with numeric keys \u2014 convert back.
function toArray(obj) {
  if (Array.isArray(obj)) return obj;
  if (!obj || typeof obj !== 'object') return [];
  const out = [];
  for (const k in obj) {
    if (Object.prototype.hasOwnProperty.call(obj, k) && /^\d+$/.test(k)) {
      out[parseInt(k, 10)] = obj[k];
    }
  }
  return out;
}

function normalizeSnapshot(val) {
  const norm = Object.assign({}, val);
  if (val.ch) norm.ch = toArray(val.ch);
  if (val.currentCalibration) norm.currentCalibration = toArray(val.currentCalibration);
  if (val.noiseFloor) norm.noiseFloor = toArray(val.noiseFloor);
  if (val.azActive !== undefined) norm.azActive = val.azActive;
  if (val.azChannel !== undefined) norm.azChannel = val.azChannel;
  if (val.azProgress !== undefined) norm.azProgress = val.azProgress;
  if (val.azQueue) norm.azQueue = toArray(val.azQueue);
  if (val.lpfAlpha) norm.lpfAlpha = toArray(val.lpfAlpha);
  if (val.events) norm.events = toArray(val.events);
  return norm;
}

function startCloudWatchdog() {
  clearInterval(cloudWatchdog);
  cloudWatchdog = setInterval(() => {
    if (connMode !== 'cloud') { clearInterval(cloudWatchdog); return; }
    const stale = (Date.now() - lastDataTs) > CLOUD_STALE_MS;
    const cs1 = document.getElementById('connStatus');
    const cs2 = document.getElementById('connStatus2');
    if (stale) {
      if (cs1) cs1.textContent = 'Offline \u2014 no data';
      if (cs2) cs2.textContent = 'Offline \u2014 no data';
    } else {
      if (cs1) cs1.textContent = 'Connected';
      if (cs2) cs2.textContent = 'Connected';
    }
  }, 2000);
}

// Eco-mode presence heartbeat: while a Cloud dashboard is open, keep a
// timestamp entry under /devices/<id>/viewers/<tabId> so the board knows
// someone is watching (live 1s push, no modem sleep). Refreshed every 15 s,
// onDisconnect-removed. Guests included — the RTDB rule lets anyone write
// their own viewer key (worst case: a stranger keeps the board awake).
function startHeartbeat() {
  stopHeartbeat();
  try {
    cloudClientId = sessionStorage.getItem('esp32monitor_cid');
    if (!cloudClientId) {
      cloudClientId = 'c' + Math.random().toString(36).slice(2) + Date.now().toString(36);
      sessionStorage.setItem('esp32monitor_cid', cloudClientId);
    }
  } catch (e) {
    cloudClientId = 'c' + Math.random().toString(36).slice(2);
  }
  cloudHeartbeatPath = cloudDevPath() + '/viewers/' + cloudClientId;
  const beat = () => {
    try {
      cloudDb.ref(cloudHeartbeatPath).set({ ts: firebase.database.ServerValue.TIMESTAMP });
    } catch (e) {}
  };
  beat();
  try {
    cloudDb.ref(cloudHeartbeatPath).onDisconnect().remove();
  } catch (e) {}
  cloudHeartbeat = setInterval(beat, 15000);
}

function stopHeartbeat() {
  if (cloudHeartbeat) { clearInterval(cloudHeartbeat); cloudHeartbeat = null; }
  if (cloudDb && cloudHeartbeatPath) {
    try { cloudDb.ref(cloudHeartbeatPath).remove().catch(() => {}); } catch (e) {}
  }
  cloudClientId = null;
  cloudHeartbeatPath = null;
}

// Route a command to the active transport (WS / Firebase / demo).
function sendCommand(obj) {
  if (connMode === 'demo') {
    if (obj.cmd === 'set_name' && latestChannelData[obj.ch]) latestChannelData[obj.ch].n = obj.name;
    if (obj.cmd === 'set_monthly_kwh' && latestChannelData[obj.ch]) latestChannelData[obj.ch].mkwh = obj.val;
    if (obj.cmd === 'reset_counter' && latestChannelData[obj.ch]) {
      latestChannelData[obj.ch].kwh = 0;
      zeroSubCardDisplay(obj.ch);
    }
    return Promise.resolve();
  }
  // Guests may observe live data but never mutate the board.
  if (!authAdmin) {
    requireAdmin();
    return Promise.reject(new Error('Auth required'));
  }
  if (connMode === 'cloud' && cloudDb) {
    const devPath = cloudDevPath();
    return cloudDb.ref(devPath + '/commands').push(obj).then(() => undefined);
  }
  if (connMode === 'local' && ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(obj));
    return Promise.resolve();
  }
  showToast('Not connected');
  return Promise.reject(new Error('Not connected'));
}

function connectWS(ip) {
  if (ws) { ws.close(); ws = null; }
  const url = 'ws://' + ip + '/ws';

  ws = new WebSocket(url);
  ws.onopen = () => {
    saveSession('local');
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
    if (!userDisconnect && connMode === 'local') {
      setConnectStatus('Disconnected \u2014 check IP address', 'disconnected');
      showConnectPanel();
    }
  };
  ws.onerror = () => {
    if (connMode === 'local') setConnectStatus('Connection error', 'disconnected');
  };
  ws.onmessage = (e) => {
    try {
      const data = JSON.parse(e.data);
      if (data && data.type === 'console') {
        appendConsoleOutput(data.out || '');
        return;
      }
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
  // Drop the no-flash marker so the panel's hidden CSS no longer applies.
  document.documentElement.classList.remove('has-session');
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
  if (lmEl && typeof data.lastMonth === 'number') {
    const ym = String(data.lastMonth);
    if (ym.length === 6) {
      lmEl.textContent = ym.slice(0, 4) + '/' + ym.slice(4);
    } else {
      lmEl.textContent = data.lastMonth;
    }
  }

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
  const azActiveCh = (data.azActive === true && typeof data.azChannel === 'number') ? data.azChannel : -1;
  const azQueueArr = Array.isArray(data.azQueue) ? data.azQueue : [];
  lastAzState = { active: azActiveCh >= 0, channel: azActiveCh, queue: azQueueArr };

  const setAzChip = (elId, active, queued) => {
    const chip = document.getElementById(elId);
    if (!chip) return;
    if (active) {
      chip.textContent = '\u25d0 Calibrating\u2026';
      chip.className = 'az-chip active';
    } else if (queued) {
      chip.textContent = '\u25f7 Queued';
      chip.className = 'az-chip queued';
    } else {
      chip.textContent = '';
      chip.className = 'az-chip';
    }
  };

  if (Array.isArray(data.noiseFloor)) {
    data.noiseFloor.forEach((v, i) => {
      const active = i === azActiveCh;
      const queued = !active && azQueueArr.includes(i);
      const inp = document.getElementById(`nf_${i}`);
      if (inp) {
        if (active) {
          inp.classList.add('calibrating');
          inp.title = 'Auto-zero in progress\u2026';
        } else {
          inp.classList.remove('calibrating');
          inp.title = '';
        }
      }
      setAzChip(`azChip_${i}`, active, queued);
      setAzChip(`azHeadChip_${i}`, active, queued);
      if (!active) syncField(`nf_${i}`, v, 3);
    });
  }

  const azStatus = document.getElementById('azStatus');
  if (azStatus) {
    const busy = azActiveCh >= 0 || azQueueArr.length > 0;
    if (busy) {
      let txt = azActiveCh >= 0
        ? `\u25d0 calibrating Ch${azActiveCh + 1} (${data.azProgress || 0}/${AZ_BATCHES})`
        : 'starting auto-zero\u2026';
      if (azQueueArr.length) {
        txt += '\u2003\u00b7\u2003waiting: ' + azQueueArr.map(c => 'Ch' + (c + 1)).join(', ');
      }
      azStatus.textContent = 'Please wait \u2014 ' + txt;
      azStatus.classList.remove('hidden');
    } else {
      azStatus.textContent = '';
      azStatus.classList.add('hidden');
    }
  }
  if (Array.isArray(data.lpfAlpha)) {
    data.lpfAlpha.forEach((v, i) => {
      if (data.azActive !== true) syncField(`lpf_${i}`, v, 2);
    });
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
      const lpf = (data.lpfAlpha && data.lpfAlpha[idx]) != null ? data.lpfAlpha[idx] : 0.2;

      const header = document.createElement('div');
      header.className = 'cal-collapse-header';
      header.innerHTML = `<span class="arrow">&#9654;</span> Ch${idx + 1}<span class="az-chip head" id="azHeadChip_${idx}"></span>`;
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
          <span class="az-chip" id="azChip_${idx}"></span>
        </div>
        <div class="cal-param-row">
          <label>LPF Alpha:</label>
          <input type="number" id="lpf_${idx}" step="0.01" min="0.01" max="1" value="${lpf}" oninput="this.dataset.userSet='true'">
          <button class="btn-sm" onclick="sendLpfAlpha(${idx})">Set</button>
          <span class="hint-inline">(0.01-1, 1=none)</span>
        </div>
        <div class="cal-param-row">
          <button class="btn-sm btn-danger btn-reset-cal" data-ch="${idx}" onclick="handleResetChannelCal(${idx})">&#8634; Reset Cal</button>
          <span class="hint-inline">(cal, noise floor, LPF)</span>
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
    <div class="card-actions admin-only">
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
  if (!requireAdmin()) return;
  populateChannelSelect();
  const sel = document.getElementById('modalChSelect');
  sel.value = idx;
  onModalChannelChange();
  setModalMode('edit');
  document.getElementById('editModal').classList.remove('hidden');
}

function openResetModal(idx) {
  if (!requireAdmin()) return;
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
  sendCommand({ cmd: 'reset_ch_to_default', ch: idx });
  closeEditModal();
  showToast(`Channel ${idx + 1} reset to defaults`);
}

function saveModalSettings() {
  if (activeEditChIdx === null) return;
  const idx = activeEditChIdx;

  const name = document.getElementById('modalChName').value.trim();
  const mkwh = parseFloat(document.getElementById('modalChMkwh').value);

  if (!isNaN(mkwh)) sendCommand({ cmd: 'set_monthly_kwh', ch: idx, val: mkwh });
  if (name) sendCommand({ cmd: 'set_name', ch: idx, name: name });

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
  sendCommand({ cmd: 'reset_counter', ch: idx });
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

function handleResetChannelCal(idx) {
  const btn = document.querySelector(`.btn-reset-cal[data-ch="${idx}"]`);
  const key = 'cal_' + idx;

  if (pendingActions[key] === 'confirm') {
    delete pendingActions[key];
    if (btn) {
      btn.innerHTML = '&#8634; Reset Cal';
      btn.classList.remove('confirming');
    }
    sendResetChannelCal(idx);
  } else {
    pendingActions[key] = 'confirm';
    if (btn) {
      btn.textContent = 'Confirm?';
      btn.classList.add('confirming');
    }
    setTimeout(() => {
      delete pendingActions[key];
      if (btn) {
        btn.innerHTML = '&#8634; Reset Cal';
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
  sendCommand({ cmd: 'set_voltage_cal', val }).then(() => {
    showToast(`Voltage cal set to ${val.toFixed(1)}`);
  });
}

function sendCurrentCal(idx) {
  const val = parseFloat(document.getElementById(`currCal_${idx}`).value);
  if (isNaN(val)) return showToast('Invalid current calibration');
  delete document.getElementById(`currCal_${idx}`).dataset.userSet;
  sendCommand({ cmd: 'set_current_cal', ch: idx, val }).then(() => {
    showToast(`Ch${idx + 1} current cal set to ${val.toFixed(1)}`);
  });
}

function autoZeroChannel(idx) {
  const nf = document.getElementById(`nf_${idx}`);
  if (nf) delete nf.dataset.userSet;

  const az = lastAzState;
  const already = (az.active && az.channel === idx) || (az.queue && az.queue.includes(idx));
  if (already) {
    showToast(`Ch${idx + 1} is already in the auto-zero queue \u2014 please wait`);
    return;
  }

  sendCommand({ cmd: 'set_noise_floor', ch: idx }).then(() => {
    const busy = az.active || (az.queue && az.queue.length > 0);
    showToast(busy
      ? `Ch${idx + 1} queued \u2014 please wait (calibrating Ch${az.channel + 1})`
      : `Ch${idx + 1} auto-zero started \u2014 the noise floor updates shortly`);
  });
}

function sendLpfAlpha(idx) {
  const inp = document.getElementById(`lpf_${idx}`);
  const val = parseFloat(inp.value);
  if (isNaN(val) || val < 0.01 || val > 1) return showToast('LPF Alpha must be 0.01-1 (1 = no filtering)');
  delete inp.dataset.userSet;
  sendCommand({ cmd: 'set_lpf', ch: idx, val }).then(() => {
    showToast(`Ch${idx + 1} LPF alpha set to ${val.toFixed(2)}`);
  });
}

function sendResetChannelCal(idx) {
  ['currCal', 'nf', 'lpf'].forEach(id => {
    const el = document.getElementById(`${id}_${idx}`);
    if (el) delete el.dataset.userSet;
  });
  sendCommand({ cmd: 'reset_ch_cal', ch: idx }).then(() => {
    showToast(`Ch${idx + 1} calibration reset to defaults`);
  });
}

function setRmsSamples() {
  const inp = document.getElementById('rmsSamples');
  const val = parseInt(inp.value);
  if (isNaN(val) || val < 100 || val > 2000) return showToast('RMS Samples must be 100-2000');
  delete inp.dataset.userSet;
  sendCommand({ cmd: 'set_rms_samples', val }).then(() => {
    showToast(`RMS Samples set to ${val}`);
  });
}

function sendNtfyTopic() {
  const inp = document.getElementById('ntfyTopic');
  const topic = inp.value.trim();
  delete inp.dataset.userSet;
  sendCommand({ cmd: 'set_ntfy_topic', val: topic }).then(() => {
    showToast(topic ? `ntfy topic set to "${topic}"` : 'ntfy topic cleared');
  });
}

function sendNtfyEnabled(cb) {
  const on = cb.checked;
  sendCommand({ cmd: 'set_ntfy_enabled', val: on }).then(() => {
    showToast(on ? 'Push notifications enabled' : 'Push notifications disabled');
  }).catch(() => {
    cb.checked = !on;
  });
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
      const lp = document.getElementById(`lpf_${i}`);
      if (lp) delete lp.dataset.userSet;
    }
    sendCommand({ cmd: 'reset_nvs_defaults' }).then(() => {
      showToast('NVS reset to defaults \u2014 values will reload from ESP32');
    });
  } else {
    btn.dataset.confirm = 'true';
    btn.textContent = 'Confirm?';
    setTimeout(() => {
      delete btn.dataset.confirm;
      btn.textContent = '\u21ba Reset NVS to Defaults';
    }, 3000);
  }
}

// ============ Device Console ============
function appendConsoleLine(text, cls) {
  const out = document.getElementById('consoleOutput');
  if (!out) return;
  const line = document.createElement('span');
  line.className = 'console-line' + (cls ? ' ' + cls : '');
  line.textContent = text;
  out.appendChild(line);
  while (out.childElementCount > CONSOLE_MAX_LINES) out.removeChild(out.firstChild);
  out.scrollTop = out.scrollHeight;
}

function appendConsoleOutput(text) {
  if (typeof text !== 'string') return;
  const lines = text.replace(/\n+$/, '').split('\n');
  lines.forEach(l => appendConsoleLine(l));
}

function clearConsole() {
  const out = document.getElementById('consoleOutput');
  if (out) out.innerHTML = '';
}

function sendConsoleCommand() {
  const inp = document.getElementById('consoleInput');
  if (!inp) return;
  const line = inp.value.trim();
  if (!line) return;
  inp.value = '';
  consoleHistory.push(line);
  if (consoleHistory.length > 50) consoleHistory.shift();
  consoleHistIdx = consoleHistory.length;

  appendConsoleLine('> ' + line, 'echo');

  if (connMode === 'demo') {
    appendConsoleLine('  Console unavailable in demo mode.', 'err');
    return;
  }
  sendCommand({ cmd: 'console', line: line }).catch(() => {
    appendConsoleLine('  Not connected — command not sent.', 'err');
  });
}

function onConsoleKey(ev) {
  if (ev.key === 'Enter') {
    ev.preventDefault();
    sendConsoleCommand();
    return;
  }
  if (ev.key === 'ArrowUp' || ev.key === 'ArrowDown') {
    if (!consoleHistory.length) return;
    ev.preventDefault();
    if (ev.key === 'ArrowUp') consoleHistIdx = Math.max(0, consoleHistIdx - 1);
    else consoleHistIdx = Math.min(consoleHistory.length, consoleHistIdx + 1);
    const inp = ev.target;
    inp.value = consoleHistIdx < consoleHistory.length ? consoleHistory[consoleHistIdx] : '';
    setTimeout(() => inp.setSelectionRange(inp.value.length, inp.value.length), 0);
  }
}

// ============ Mock Demo Mode ============
function startDemoMode() {
  connMode = 'demo';
  document.body.classList.add('conn-mode-demo');
  saveSession('demo');
  if (ws) { ws.close(); ws = null; }
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
      lpfAlpha: [0.2, 0.2, 0.2, 0.2, 0.2, 0.2],
      azActive: false, azChannel: -1, azProgress: 0, azQueue: [],
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
