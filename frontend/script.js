let ws = null;
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
// Demo is the only alternative to a live connection, so this is a boolean.
// It was `connMode` with a 'local' | 'cloud' | 'demo' string union back when
// there were three transports to choose between.
let isDemo = false;
let timeSyncTimer = null;         // re-lend the clock to the board periodically
let lastDataTs = 0;
// Admin-PIN state. Declared HERE, at the top of the state block, even though
// the functions that use it live further down: init() is an IIFE that runs
// while this file is still being evaluated, and a `let` declared below it is
// still in its temporal dead zone when init() touches it - which threw
// "Cannot access 'adminPin' before initialization" and killed the whole
// script, leaving a page that loaded but rendered nothing.
let adminPin = '';
let pinOk = false;
let consoleHistory = [];
let consoleHistIdx = -1;
const CONSOLE_MAX_LINES = 400;
const CHART_MAX = 240;
const CHART_DEFS = {
  'V': { label: 'Voltage (V)', color: '#3b82f6' },
  'P': { label: 'Total Power (W)', color: '#5a8aa0' },
  'I': { label: 'Total Current (A)', color: '#9a9a9a' }
};

const NUM_CHANNELS = 6;


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
  // ?demo=1 renders the UI with mocked data and opens no socket. The only
  // remaining reason the demo path exists: previewing the dashboard on a
  // machine that cannot join the board's WiFi.
  const demo = /[?&]demo=1\b/.test(location.search);
  isDemo = demo;

  // Restore a remembered PIN so a returning admin is not re-prompted. The
  // board still re-validates it, so a changed PIN simply leaves us read-only.
  adminPin = pinCached();
  applyPinState();

  if (isDemo) {
    startDemoMode();
  } else {
    handleConnect();
  }

  if (isStandalone()) hideInstallRow();
  else showInstallRow();

  // Close modal on Escape key
  window.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') closeEditModal();
  });

  // The board no longer has a cloud presence heartbeat to stop, but a trip
  // notification fired in this tab should not outlive it.
  window.addEventListener('pagehide', () => { try { clearPinCache(); } catch (e) {} });

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
// ============ Connection ============
// There is nothing to choose any more. The page is SERVED BY the board, so
// location.host is the board - there is no IP to type, no mode dropdown, and
// no cloud to fall back to. Connect, or do not.
function deviceWSUrl() {
  return 'ws://' + location.host + '/ws';
}

function handleConnect() {
  document.body.classList.remove('conn-mode-demo');
  isDemo = false;
  connectWS();
}

function handleDisconnect() {
  userDisconnect = true;
  if (demoInterval) clearInterval(demoInterval);
  if (timeSyncTimer) { clearInterval(timeSyncTimer); timeSyncTimer = null; }
  if (ws) { ws.close(); ws = null; }
  isDemo = false;
  document.body.classList.remove('conn-mode-demo');
  showConnectPanel();
}

// ============ Access Point (name + password) ============
// The board's own network identity. Saved to its flash, then the board reboots
// to apply it, so the page it is about to lose the connection to says so first.
//
// The length rules below are a copy of src/network/ap_creds.cpp. The board is
// the authority and re-checks everything - these only save the user a round
// trip and a surprise reboot. Keep the two in step; there is a check in
// scripts/check_docs.py that fails if they drift apart.
const AP_SSID_MAX = 32;
const AP_PASS_MIN = 8;
const AP_PASS_MAX = 63;

function apValidationMessage(ssidRaw, pass) {
  const ssid = ssidRaw.trim();
  if (!ssid) return 'Enter a network name';
  // Compared against the RAW value, not the trimmed one. Trimming first made
  // this check unreachable - the trailing space was gone before it was tested -
  // and the device would then be the only thing telling the user, which is the
  // opposite of what a pre-flight check is for. Rejecting also beats silently
  // saving something other than what they typed.
  if (ssid !== ssidRaw) return 'Network name cannot start or end with a space';
  if (/^[ ]+$/.test(ssid)) return 'Network name cannot be only spaces';
  // Length is counted the way the board counts it: OCTETS, not characters, so a
  // 20-character accented name is measured the same way on both sides.
  if (new TextEncoder().encode(ssid).length > AP_SSID_MAX) {
    return `Network name must be ${AP_SSID_MAX} characters or fewer`;
  }
  // eslint-disable-next-line no-control-regex
  if (/[\u0000-\u001f]/.test(ssid) || /[\u0000-\u001f]/.test(pass)) {
    return 'Name and password cannot contain control characters';
  }
  if (!pass) return 'Enter a password (an open network is not allowed)';
  const passLen = new TextEncoder().encode(pass).length;
  if (passLen < AP_PASS_MIN) return `Password must be at least ${AP_PASS_MIN} characters`;
  if (passLen > AP_PASS_MAX) return `Password must be ${AP_PASS_MAX} characters or fewer`;
  return '';
}

function saveApSettings() {
  const ssidInput = document.getElementById('apSsid');
  const passInput = document.getElementById('apPass');
  const ssidRaw = ssidInput.value || '';
  const ssid = ssidRaw.trim();
  const pass = passInput.value || '';

  // The RAW value goes to the validator, not the trimmed one. Passing `ssid`
  // here made its leading/trailing-space rule unreachable, and a silent save of
  // "Meter AP" when the box said "Meter AP " is exactly the confusion the rule
  // exists to prevent.
  const problem = apValidationMessage(ssidRaw, pass);
  if (problem) return showToast(problem);
  if (isDemo) return showToast('Not available in demo mode');

  // Say the reboot is coming BEFORE the frame goes out. sendCommand resolves
  // as soon as the frame is written, not when the board acknowledges it, and
  // the connection is about to disappear.
  return sendCommand({ cmd: 'set_ap', ssid: ssid, pass: pass }).then(() => {
    passInput.value = '';
    showToast(`Saved — the board is restarting as "${ssid}"`, 6000);
    showToast('Rejoin that WiFi, then reopen http://192.168.4.1/', 6000);
    // The page is about to lose its socket. Say so plainly rather than letting
    // the reconnect logic spin.
    const hint = document.getElementById('apHint');
    if (hint) {
      hint.innerHTML = '<strong>The board is rebooting to join "' + ssid +
        '".</strong> Join that network, then reopen ' +
        '<span class="mono">http://192.168.4.1/</span>.';
    }
    setTimeout(() => handleDisconnect(), 1500);
    return true;
  }).catch(() => false);
}

function resetApSettings() {
  if (isDemo) return showToast('Not available in demo mode');
  return sendCommand({ cmd: 'reset_ap' }).then(() => {
    showToast('Resetting to the default network name — the board is restarting', 6000);
    setTimeout(() => handleDisconnect(), 1500);
    return true;
  }).catch(() => false);
}

// ============ Admin PIN ============
// The ESP32 enforces the PIN (see src/network/auth_gate.cpp); this side only
// decides what the UI is allowed to show. Unlocking is explicit: a viewer gets
// read-only until they enter the PIN in Settings.
// (adminPin / pinOk are declared in the top state block - see the
// temporal-dead-zone note there.)
function pinCached() {
  try { return sessionStorage.getItem('esp32counter_pin') || ''; } catch (e) { return ''; }
}
function pinCache(p) {
  try { sessionStorage.setItem('esp32counter_pin', p || ''); } catch (e) {}
}
function clearPinCache() {
  try { sessionStorage.removeItem('esp32counter_pin'); } catch (e) {}
}

function requirePin() {
  if (pinOk) return true;
  showToast('Enter the admin PIN in Settings first');
  showPage('settings');
  const row = document.getElementById('pinRow');
  if (row) row.classList.remove('hidden');
  return false;
}

function applyPinState() {
  document.body.classList.toggle('role-admin', pinOk);
  document.body.classList.toggle('role-guest', !pinOk);
  const badge = document.getElementById('roleBadge');
  if (badge) {
    badge.textContent = pinOk ? 'Admin — full control' : 'Viewer — read-only';
    badge.className = 'role-badge ' + (pinOk ? 'role-admin' : 'role-guest');
  }
  const hint = document.getElementById('guestHint');
  // '' rather than 'inline': the hint is a block-level <p class="hint"> now, and
  // forcing it inline used to squeeze its margin away.
  if (hint) hint.style.display = pinOk ? 'none' : '';
  // Losing admin while a modal is open must close it, or a viewer would be
  // left staring at controls that no longer do anything.
  if (!pinOk && activeEditChIdx !== null) closeEditModal();
}

// Asks the board whether the PIN we hold is right. Sent on connect (so a
// remembered PIN unlocks the UI automatically) and whenever it changes.
function verifyPin(pin) {
  return new Promise((resolve) => {
    if (!ws || ws.readyState !== WebSocket.OPEN) return resolve(false);
    ws.addEventListener('message', function h(e) {
      try {
        const d = JSON.parse(e.data);
        if (d && d.type === 'console') {
          if ((d.out || '').indexOf('PIN OK') >= 0) {
            ws.removeEventListener('message', h);
            resolve(true);
          } else if ((d.out || '').indexOf('PIN incorrect') >= 0) {
            ws.removeEventListener('message', h);
            resolve(false);
          }
        } else if (d && d.type === 'auth' && d.ok === false) {
          ws.removeEventListener('message', h);
          resolve(false);
        }
      } catch (x) { /* keep waiting */ }
    });
    ws.send(JSON.stringify({ cmd: 'verify_pin', pin: pin }));
    // No answer from the board means the PIN is UNVERIFIED, which is not the
    // same as verified. This used to resolve with the current pinOk, so a
    // previously-unlocked session that then tried a wrong PIN was reported as
    // a successful unlock - the stale value leaked straight through.
    setTimeout(() => resolve(false), 3000);
  });
}

// Changing the PIN requires the CURRENT one - the board enforces that, since
// set_pin goes through the same gate as every other mutation.
function changePin() {
  if (!requirePin()) return Promise.resolve(false);
  const input = document.getElementById('pinNew');
  const val = (input.value || '').trim();
  if (val.length < 4 || val.length > 16) {
    showToast('PIN must be 4-16 characters');
    return Promise.resolve(false);
  }
  return sendCommand({ cmd: 'set_pin', pin_new: val }).then(() => {
    input.value = '';
    // Keep the session usable without re-prompting.
    adminPin = val;
    pinCache(val);
    pinOk = true;
    applyPinState();
    showToast('Admin PIN changed');
    return true;
  }).catch(() => false);
}

async function unlockWithPin(pin) {
  pin = (pin || '').trim();
  if (!pin) { pinOk = false; applyPinState(); return false; }
  const ok = await verifyPin(pin);
  pinOk = ok;
  if (ok) { adminPin = pin; pinCache(pin); } else { adminPin = ''; clearPinCache(); }
  applyPinState();
  return ok;
}
// Route a command to the board. One transport, one place.
//
// The admin PIN is attached to EVERY mutating command rather than negotiated
// once per session. That is deliberate: there is no session token to forge,
// revoke, or keep in sync per WebSocket client, and the ESP32 has no
// per-client state to hang one on. A read-only viewer simply never has one to
// send, and the board rejects the frame.
function sendCommand(obj) {
  if (isDemo) {
    if (obj.cmd === 'set_name' && latestChannelData[obj.ch]) latestChannelData[obj.ch].n = obj.name;
    if (obj.cmd === 'set_monthly_kwh' && latestChannelData[obj.ch]) latestChannelData[obj.ch].mkwh = obj.val;
    if (obj.cmd === 'reset_counter' && latestChannelData[obj.ch]) {
      latestChannelData[obj.ch].kwh = 0;
      zeroSubCardDisplay(obj.ch);
    }
    return Promise.resolve();
  }

  // set_time is exempt on the device and must reach it even for a viewer -
  // it is what gives the board a clock, and without a clock the monthly
  // rollover never fires. verify_pin is exempt for obvious reasons.
  const exempt = (obj.cmd === 'set_time' || obj.cmd === 'verify_pin');
  if (!exempt && !pinOk) {
    requirePin();
    return Promise.reject(new Error('PIN required'));
  }

  if (ws && ws.readyState === WebSocket.OPEN) {
    const frame = Object.assign({}, obj);
    if (!exempt && adminPin) frame.pin = adminPin;
    ws.send(JSON.stringify(frame));
    return Promise.resolve();
  }
  showToast('Not connected');
  return Promise.reject(new Error('Not connected'));
}

// Lend the browser's clock to the board, then keep re-lending it.
//
// This is not cosmetic. The board has no NTP in AP-only mode, and
// LimitManager::rolloverIfNeeded() refuses to act on an unset clock - so
// without this the monthly billing reset would silently stop happening.
function sendTime() {
  sendCommand({ cmd: 'set_time', t: Math.floor(Date.now() / 1000) })
    .catch(() => {});
}

function connectWS() {
  if (ws) { ws.close(); ws = null; }

  ws = new WebSocket(deviceWSUrl());

  ws.onopen = () => {
    setConnectStatus('Connected', 'connected');
    const ipEl = document.getElementById('connectedIp');
    if (ipEl) ipEl.textContent = location.host;
    const cs2 = document.getElementById('connStatus2');
    if (cs2) cs2.textContent = 'Connected';
    showDashboard();

    // Clock first: the rollover check runs on every sensor cycle, so the
    // sooner the board has a valid time the better.
    sendTime();
    if (timeSyncTimer) clearInterval(timeSyncTimer);
    timeSyncTimer = setInterval(sendTime, 10 * 60 * 1000);

    // Re-validate a remembered PIN. The board is the authority here, not
    // sessionStorage.
    const cached = pinCached();
    if (cached) unlockWithPin(cached);
  };

  ws.onclose = () => {
    if (timeSyncTimer) { clearInterval(timeSyncTimer); timeSyncTimer = null; }
    pinOk = false;
    adminPin = '';
    applyPinState();
    if (!userDisconnect && !isDemo) {
      setConnectStatus('Disconnected — is your phone on the board\'s WiFi?', 'disconnected');
      showConnectPanel();
    }
  };

  ws.onerror = () => {
    if (!isDemo) setConnectStatus('Connection error', 'disconnected');
  };

  ws.onmessage = (e) => {
    try {
      const data = JSON.parse(e.data);
      if (data && data.type === 'console') {
        appendConsoleOutput(data.out || '');
        return;
      }
      if (data && data.type === 'auth') {
        // The board rejected a command for a missing/wrong PIN. The UI is not
        // the authority, so drop straight back to read-only.
        pinOk = false;
        adminPin = '';
        clearPinCache();
        applyPinState();
        showToast('Admin PIN rejected — commands are now read-only');
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
  setConnectStatus('Disconnected — is your phone on the board\'s WiFi?', 'disconnected');
}

function setConnectStatus(msg, cls) {
  const el = document.getElementById('connectStatus');
  if (el) {
    el.textContent = msg;
    el.className = 'connect-status ' + (cls || '');
  }
}

// ============ Clock ============
// The board has no internet and therefore no NTP. It borrows this browser's
// clock (sendTime, on connect and every 10 min) and reports back whether it
// has a valid one. Until it does, the monthly rollover is dormant - so this
// indicator is not cosmetic, it is telling you whether billing will reset.
function renderClock(data) {
  const el = document.getElementById('timeStatus');
  if (!el) return;
  const t = data && data.time;
  if (!t || !t.ok) {
    el.textContent = 'not set — monthly reset will not fire';
    el.style.color = '#e74c3c';
    return;
  }
  const age = (typeof t.age === 'number' && t.age < 86400 * 30) ? t.age : null;
  el.textContent = age === null ? 'set' : 'set, ' + fmtAge(age) + ' ago';
  el.style.color = '';
}

function fmtAge(sec) {
  if (sec < 90) return Math.round(sec) + 's';
  if (sec < 5400) return Math.round(sec / 60) + 'm';
  if (sec < 172800) return Math.round(sec / 3600) + 'h';
  return Math.round(sec / 86400) + 'd';
}

// ============ Trip notifications ============
// Replaces the ntfy.sh push that died with the cloud. ntfy needed the internet;
// a Web Notification does not, so a trip still alerts you as long as this
// dashboard is open — which, in AP-only mode, means as long as you are on the
// board's WiFi.
let notifiedTripSignature = '';
let notificationAsked = false;

function notifyOnTrip(data, hasTrip) {
  if (!hasTrip) { notifiedTripSignature = ''; return; }

  const tripped = (data.ch || []).filter(c => c && c.s === 2);
  // Key on WHICH channels are tripped, so a new trip fires a fresh
  // notification but a snapshot arriving 6x a second does not.
  const sig = tripped.map(c => c.n).join('|');
  if (sig === notifiedTripSignature) return;
  const first = !notifiedTripSignature;
  notifiedTripSignature = sig;

  if (!notificationAsked && 'Notification' in window && Notification.permission === 'default') {
    notificationAsked = true;
    Notification.requestPermission();
  }
  if (!('Notification' in window) || Notification.permission !== 'granted') return;

  const body = tripped.map(c => c.n + ': ' + (+c.kwh).toFixed(1) + ' kWh of ' +
                                     (+c.mkwh).toFixed(0) + ' kWh').join('\n');
  const title = tripped.length === 1
    ? tripped[0].n + ' is over budget'
    : tripped.length + ' channels are over budget';
  try {
    const n = new Notification(title, {
      body: body,
      tag: 'esp32counter-trip',   // replaces the previous one instead of stacking
      requireInteraction: first
    });
    n.onclick = function () { window.focus(); showPage('dashboard'); n.close(); };
  } catch (e) { /* notification failed — the on-page LED still shows it */ }
}

// ============ Network indicator (AP-only) ============
// The board is an access point and never joins a network, so there is no RSSI
// to display. The icon shows the shape of the world we are actually in: an AP
// with clients, or an idle AP nobody is watching.
function updateWifiIcon(data) {
  const wifi = document.querySelector('.wifi');
  if (!wifi) return;
  if (!data.ap) {
    wifi.setAttribute('class', 'wifi lv0 disconnected');
    wifi.setAttribute('title', 'Access point starting');
    return;
  }
  // Any WebSocket client is by definition on the AP, so the link is up.
  wifi.setAttribute('class', 'wifi lv2');
  wifi.setAttribute('title', 'Connected over the board\'s own WiFi (AP mode)');
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

  // Access Point panel: show the identity the board is actually broadcasting, so
  // the user edits the stored value instead of a placeholder that looks like a
  // fresh board. The password is deliberately never sent - the board reports
  // only whether both still match the factory ones, which is all the wording
  // needs.
  //
  // dataset.userSet is the same guard the calibration fields use: this message
  // arrives 6-7 times a second, and without it the board would overwrite a
  // half-typed network name on every push.
  const apSsidEl = document.getElementById('apSsid');
  if (apSsidEl && !apSsidEl.dataset.userSet && typeof data.apSsid === 'string') {
    apSsidEl.value = data.apSsid;
  }
  const apPassEl = document.getElementById('apPass');
  if (apPassEl && !apPassEl.dataset.userSet && typeof data.apIsDefault === 'boolean') {
    apPassEl.placeholder = data.apIsDefault
      ? 'unchanged (factory default)'
      : 'unchanged (custom — type to replace)';
  }

  // System status LED
  const led = document.getElementById('sysLed');
  if (led) {
    if (hasTrip) { led.className = 'led led-trip'; }
    else if (hasWarn) { led.className = 'led led-warn'; }
    else { led.className = 'led led-ok'; }
  }

  renderClock(data);
  notifyOnTrip(data, hasTrip);

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
          <div class="cal-field">
            <input type="number" id="currCal_${idx}" step="0.1" value="${currCal}" oninput="this.dataset.userSet='true'">
          </div>
          <button class="btn-sm" onclick="sendCurrentCal(${idx})">Set</button>
        </div>
        <div class="cal-param-row">
          <label>Noise Floor:</label>
          <div class="cal-field">
            <input type="number" id="nf_${idx}" step="0.001" value="${nf}" oninput="this.dataset.userSet='true'">
            <span class="az-chip" id="azChip_${idx}"></span>
          </div>
          <button class="btn-sm" onclick="autoZeroChannel(${idx})" style="color:#e67e22;">Auto-Zero</button>
        </div>
        <div class="cal-param-row">
          <label>LPF Alpha:</label>
          <div class="cal-field">
            <input type="number" id="lpf_${idx}" step="0.01" min="0.01" max="1" value="${lpf}" oninput="this.dataset.userSet='true'">
            <span class="hint-inline">(0.01-1, 1=none)</span>
          </div>
          <button class="btn-sm" onclick="sendLpfAlpha(${idx})">Set</button>
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
  if (!requirePin()) return;
  populateChannelSelect();
  const sel = document.getElementById('modalChSelect');
  sel.value = idx;
  onModalChannelChange();
  setModalMode('edit');
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

function showToast(msg, ms) {
  let toast = document.getElementById('toast');
  if (!toast) {
    toast = document.createElement('div');
    toast.id = 'toast';
    document.body.appendChild(toast);
  }
  toast.textContent = msg;
  toast.className = 'toast show';
  clearTimeout(toast._hide);
  // Callers that are about to lose the connection need longer than the default:
  // 2.5 s is gone before a phone has finished switching WiFi networks.
  toast._hide = setTimeout(() => { toast.className = 'toast'; }, ms || 2500);
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

  if (isDemo) {
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
  isDemo = true;
  document.body.classList.add('conn-mode-demo');
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
