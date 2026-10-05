/* Cloud transport for the shared dashboard.
 *
 * script.js (exact copy of frontend/script.js) owns ALL rendering: channel
 * cards, charts, events, settings forms, console. This file owns ONLY the
 * transport, loaded after it, overriding by redeclaration:
 *   handleConnect/handleDisconnect -> Firebase device subscription
 *   sendCommand                     -> /devices/<MAC>/cmd write + /ack wait
 *   verifyPin/unlockWithPin/changePin/requirePin -> Google admin, no board PIN
 *   sendTime                        -> set_time via cmd (NTP already has it)
 *   saveApSettings/resetApSettings/saveStaSettings/saveCloudSettings
 *     -> same frames as local, but no handleDisconnect (the board reboots,
 *        the subscription survives; kicking to the connect panel would lose it)
 *
 * Public viewers stay read-only: pinOk mirrors the Google admin check, so the
 * existing body.role-guest gating (.admin-only hidden) applies unchanged.
 */
var CL_ADMIN_EMAIL = 'heng.xiao.hour@gmail.com';
var CL_DEFAULT_MAC = 'EC64C998B0EC';
var CL_STALE_MS = 30000;
var CL_ACK_TIMEOUT_MS = 12000;
// Observed cadence: the threshold follows the board instead of assuming it.
// Starts at 30 s (10 s board safe); once pushes arrive ~1 s apart it
// tightens to 5 s. A fixed 5 s from boot would cry wolf on every 10 s board.
var cl_lastGapMs = 0;
var cl_prevRxMs = 0;

function cl_staleMs() {
  if (cl_lastGapMs > 0 && cl_lastGapMs < 3000) return 5000;
  return CL_STALE_MS;
}

function cl_cadenceLabel() {
  if (cl_lastGapMs > 0 && cl_lastGapMs < 3000) return '~1s';
  if (cl_lastGapMs > 0) return '~' + Math.max(1, Math.round(cl_lastGapMs / 1000)) + 's';
  return '~10s';
}

var cl_db = null;
var cl_auth = null;
var cl_mac = null;
var cl_latestRef = null;
var cl_ackRef = null;
var cl_lastRxMs = 0;
var cl_lastSentId = null;
var cl_lastSentAt = 0;
var cl_userEmail = null;
var cl_isAdmin = false;
var cl_staleTimer = null;
var cl_pending = {}; // id -> {resolve, reject, cmd, timer}

// Stash page errors where the admin can read them (Settings -> console).
window.__cl_errs = [];
window.addEventListener('error', function (ev) {
  try { window.__cl_errs.push(String((ev && ev.message) || ev)); } catch (e) {}
});

var CL_FIREBASE_CONFIG = {
  apiKey: 'AIzaSyA1BCYnxBc9q_ONa58TTkGimlGPn0wyvj0',
  authDomain: 'esp32-electricity-counter.firebaseapp.com',
  databaseURL: 'https://esp32-electricity-counter-default-rtdb.firebaseio.com',
  projectId: 'esp32-electricity-counter'
};

function cl_status(msg) {
  var el = document.getElementById('cloudCmdStatus');
  if (el) el.textContent = msg;
}

function cl_setAdmin(isAdmin, email) {
  cl_isAdmin = !!isAdmin;
  cl_userEmail = email || null;
  // The dashboard's only privilege flag. Public -> guest (read-only),
  // admin Gmail -> admin (full control). No board PIN exists out here.
  pinOk = cl_isAdmin;
  adminPin = '';
  applyPinState();
  var badge = document.getElementById('adminBadge');
  if (badge) badge.classList.toggle('hidden', !cl_isAdmin);
  var ue = document.getElementById('userEmail');
  if (ue) ue.textContent = cl_userEmail || 'not signed in';
  var who = document.getElementById('googleWho');
  if (who) who.textContent = cl_userEmail || 'not signed in';
  var b1 = document.getElementById('authBtn');
  if (b1) b1.textContent = cl_userEmail ? 'Sign out' : 'Sign in with Google';
  var b2 = document.getElementById('authBtn2');
  if (b2) b2.textContent = cl_userEmail ? 'Sign out' : 'Sign in';
}

function cl_googleToggle() {
  if (!cl_auth) return;
  if (cl_auth.currentUser) {
    cl_auth.signOut();
  } else {
    var provider = new firebase.auth.GoogleAuthProvider();
    cl_auth.signInWithPopup(provider).catch(function (err) {
      showToast('Sign-in failed: ' + ((err && err.message) || err));
    });
  }
}

// Cloud payload -> local snapshot shape (see updateDashboard in script.js).
// Push sends fw (not firmwareVersion) and mcu (not mcuTemp); map both.
// Anything the push does not carry (calibration values) is simply absent
// and the dashboard's typeof guards skip it.
// RTDB may hand arrays back as keyed objects after a delete; normalise.
function cl_toArray(x) {
  if (Array.isArray(x)) return x;
  if (x && typeof x === 'object') {
    var keys = Object.keys(x).filter(function (k) { return String(parseInt(k, 10)) === k; });
    keys.sort(function (a, b) { return parseInt(a, 10) - parseInt(b, 10); });
    return keys.map(function (k) { return x[k]; });
  }
  return x;
}

function cl_adapt(latest, mac) {
  if (!latest) return null;
  var d = {};
  d.v = latest.v;
  d.epoch = latest.epoch;
  d.uptime = latest.uptime;
  d.rssi = latest.rssi;
  d.wifi = (latest.wifi === true) || (typeof latest.rssi === 'number');
  d.ap = !!latest.ap;
  d.apSsid = (typeof latest.apSsid === 'string') ? latest.apSsid : undefined;
  d.apIsDefault = latest.apIsDefault;
  d.staSsid = (typeof latest.staSsid === 'string') ? latest.staSsid : undefined;
  d.staIsDefault = latest.staIsDefault;
  d.lastMonth = (typeof latest.lastMonth === 'number') ? latest.lastMonth
    : (latest.cloud && typeof latest.cloud.lastMonth === 'number') ? latest.cloud.lastMonth : undefined;
  d.mcuTemp = (latest.mcu === undefined || latest.mcu === null) ? null : latest.mcu;
  d.eco = !!latest.eco;
  d.firmwareVersion = latest.fw;
  d.time = latest.time;
  d.ch = cl_toArray(latest.ch);
  d.events = cl_toArray(latest.events);
  d.cloud = {
    en: !!(latest.cloud && latest.cloud.en),
    ok: !!(latest.cloud && latest.cloud.ok),
    age: (latest.cloud && typeof latest.cloud.age === 'number') ? latest.cloud.age : -1,
    dev: mac,
    host: (latest.cloud && typeof latest.cloud.host === 'string' && latest.cloud.host)
      ? latest.cloud.host : 'esp32-electricity-counter-default-rtdb.firebaseio.com',
    acct: (latest.cloud && typeof latest.cloud.acct === 'string') ? latest.cloud.acct : ''
  };
  return d;
}

function cl_refreshStale() {
  // The header stays identical to the board-served dashboard: "Connected"
  // when fresh, like the local UI. Detail (cadence, quiet time, debug)
  // lives in Settings under Cloud device.
  var ms = cl_staleMs();
  var stale = !cl_lastRxMs || (Date.now() - cl_lastRxMs > ms);
  var tag = document.getElementById('staleTag');
  if (tag) tag.classList.toggle('hidden', !stale);
  var dot = document.getElementById('cloudDot');
  if (dot) dot.className = 'led ' + (!cl_lastRxMs ? '' : (stale ? 'led-stale' : 'led-ok'));
  var fr = document.getElementById('cloudFresh');
  if (fr) {
    fr.textContent = !cl_lastRxMs ? 'connecting…'
      : (stale ? 'STALE — board quiet >' + Math.round(ms / 1000) + 's'
               : 'live (' + cl_cadenceLabel() + ' pushes)');
  }
  var cs = document.getElementById('connStatus');
  if (cs && cl_mac) {
    cs.textContent = !cl_lastRxMs ? 'Connected'
      : (stale ? 'Stale (board quiet >' + Math.round(ms / 1000) + 's)' : 'Connected');
  }
}

function cl_onLatest(val) {
  var now = Date.now();
  if (cl_lastRxMs) cl_lastGapMs = now - cl_lastRxMs;
  cl_lastRxMs = now;
  if (!val) {
    cl_refreshStale();
    return;
  }
  var d = cl_adapt(val, cl_mac);
  try {
    updateDashboard(d);
  } catch (err) {
    cl_status('RENDER ERROR: ' + ((err && err.message) || err) +
      ' | keys=' + Object.keys(val).join(',') +
      ' | ch=' + (Array.isArray(val.ch) ? val.ch.length : typeof val.ch));
    return;
  }
  var n = 0;
  try { n = document.querySelectorAll('.channel-card').length; } catch (e) {}
  var dbg = document.getElementById('cloudDbg');
  if (dbg) {
    dbg.textContent = 'rendered ' + new Date().toLocaleTimeString() +
      ' · ch=' + (d && d.ch ? d.ch.length : '?') + ' cards=' + n +
      ' · errs=' + (window.__cl_errs || []).length;
  }
  cl_refreshStale();
}

function cl_onAck(ack) {
  if (!ack || !ack.id) return;
  var p = cl_pending[ack.id];
  if (!p) {
    // Not ours (older reply, or sent from another tab) - still show console
    // output so a second admin tab sees what happened.
    if (ack.out) appendConsoleOutput(String(ack.out));
    return;
  }
  delete cl_pending[ack.id];
  if (p.timer) clearTimeout(p.timer);
  var age = Math.round((Date.now() - cl_lastSentAt) / 1000);
  var line = 'board replied in ~' + age + 's: ok=' + ack.ok;
  if (ack.out !== undefined && ack.out !== '') line += ' out=' + ack.out;
  cl_setCmdDone(line);
  if (p.cmd === 'console' && ack.out) appendConsoleOutput(String(ack.out));
  p.resolve(ack);
}

function cl_setCmdDone(line) {
  cl_status(line);
  var cout = document.getElementById('consoleOutput');
  if (cout && line) {
    appendConsoleLine(line);
  }
}

// Display formatting only: EC64C998B0EC -> ec:64:c9:98:b0:ec. RTDB paths
// and option values keep the raw id; only what the eye reads is formatted.
function cl_macFmt(id) {
  if (typeof id !== 'string' || !/^[0-9a-fA-F]{12}$/.test(id)) return id;
  return id.toLowerCase().replace(/(..)(..)(..)(..)(..)(..)/, '$1:$2:$3:$4:$5:$6');
}

function cl_loadDevices() {
  cl_status('Loading devices…');
  return fetch(CL_FIREBASE_CONFIG.databaseURL + '/devices.json?shallow=true')
    .then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    })
    .then(function (obj) { return obj ? Object.keys(obj) : []; })
    .catch(function () {
      return cl_db.ref('devices').get().then(function (snap) {
        var v = snap.val();
        return v ? Object.keys(v) : [];
      });
    })
    .then(function (keys) {
      keys.sort();
      var sel = document.getElementById('devicePicker');
      sel.innerHTML = '';
      if (!keys.length) {
        cl_status('No devices found under /devices yet.');
        setConnectStatus('Cloud — no devices yet', 'disconnected');
        return;
      }
      keys.forEach(function (k) {
        var o = document.createElement('option');
        o.value = k;
        o.textContent = cl_macFmt(k);
        sel.appendChild(o);
      });
      var pick = keys.indexOf(CL_DEFAULT_MAC) >= 0 ? CL_DEFAULT_MAC : keys[0];
      sel.value = pick;
      cl_selectDevice(pick);
    })
    .catch(function (err) {
      cl_status('Could not list devices: ' + ((err && err.message) || err));
      setConnectStatus('Cloud — device list failed', 'disconnected');
    });
}

function cl_selectDevice(mac) {
  cl_mac = mac;
  cl_lastSentId = null;
  cl_status('No command sent yet.');
  if (cl_latestRef) { cl_latestRef.off(); cl_latestRef = null; }
  if (cl_ackRef) { cl_ackRef.off(); cl_ackRef = null; }
  var ipEl = document.getElementById('connectedIp');
  if (ipEl) ipEl.textContent = 'cloud / ' + cl_macFmt(mac);
  cl_latestRef = cl_db.ref('devices/' + mac + '/latest');
  cl_latestRef.on('value',
    function (snap) { cl_onLatest(snap.val()); },
    function (err) { cl_status('Read failed: ' + ((err && err.message) || err)); });
  cl_ackRef = cl_db.ref('devices/' + mac + '/ack');
  cl_ackRef.on('value', function (snap) { cl_onAck(snap.val()); });
  // REST first: renders in ~1s without waiting for the RTDB socket; the
  // subscription above then takes over for live updates.
  fetch(CL_FIREBASE_CONFIG.databaseURL + '/devices/' + mac + '/latest.json')
    .then(function (r) { return r.ok ? r.json() : null; })
    .then(function (v) { if (v) cl_onLatest(v); })
    .catch(function () {});
  showDashboard();
  setConnectStatus('Connecting…', 'connected');
  var cs2 = document.getElementById('connStatus2');
  if (cs2) cs2.textContent = 'Cloud — ' + mac;
}

// ---- Transport overrides (redeclarations win over script.js) ----

function handleConnect() {
  isDemo = false;
  document.body.classList.remove('conn-mode-demo');
  cl_loadDevices();
}

function handleDisconnect() {
  // Cloud never "hangs up": dropping the subscription is only for an
  // explicit sign-out/device change. A board reboot must NOT kick the
  // viewer back to the connect panel - the data resumes on its own.
  if (cl_latestRef) { cl_latestRef.off(); cl_latestRef = null; }
  if (cl_ackRef) { cl_ackRef.off(); cl_ackRef = null; }
  cl_mac = null;
  cl_lastRxMs = 0;
  document.getElementById('app').classList.add('hidden');
  document.getElementById('connectPanel').classList.remove('hidden');
  setConnectStatus('Cloud — pick a device to begin', 'disconnected');
}

function requirePin() {
  if (cl_isAdmin) return true;
  showToast('Sign in with the admin Google account first');
  showPage('settings');
  return false;
}

function verifyPin(pin) {
  return Promise.resolve(cl_isAdmin);
}

function unlockWithPin(pin) {
  if (cl_isAdmin) { pinOk = true; applyPinState(); return Promise.resolve(true); }
  cl_googleToggle();
  return Promise.resolve(false);
}

function changePin() {
  showToast('The LAN PIN lives on the board — change it from the local dashboard');
  return Promise.resolve(false);
}

function sendTime() {
  if (!cl_isAdmin || !cl_mac) return;
  sendCommand({ cmd: 'set_time', t: Math.floor(Date.now() / 1000) }).catch(function () {});
}

function sendCommand(obj) {
  if (!cl_mac) {
    showToast('Pick a device first');
    return Promise.reject(new Error('no device'));
  }
  if (!cl_isAdmin) {
    requirePin();
    return Promise.reject(new Error('admin required'));
  }
  // Wipe password-style fields the moment they leave the page.
  if (obj.cmd === 'set_ap') { var a = document.getElementById('apPass'); if (a) a.value = ''; }
  if (obj.cmd === 'setwifi') { var s = document.getElementById('staPass'); if (s) s.value = ''; }
  if (obj.cmd === 'setcloud') { var c = document.getElementById('cloudAuth'); if (c) c.value = ''; }
  if (obj.cmd === 'console') { var ci = document.getElementById('consoleInput'); }
  var id = String(Date.now()) + '-' + Math.floor(Math.random() * 1000000);
  cl_lastSentId = id;
  cl_lastSentAt = Date.now();
  cl_status('sent ' + obj.cmd + ' (id ' + id + '), waiting for board (polls every ~2s)…');
  var p = {};
  var promise = new Promise(function (resolve, reject) { p.resolve = resolve; p.reject = reject; });
  p.cmd = obj.cmd;
  p.timer = setTimeout(function () {
    if (cl_pending[id]) {
      delete cl_pending[id];
      cl_status('no reply yet for ' + obj.cmd + ' — the board polls every ~2s; keep waiting or retry');
      p.resolve({ timeout: true });
    }
  }, CL_ACK_TIMEOUT_MS);
  cl_pending[id] = p;
  cl_db.ref('devices/' + cl_mac + '/cmd').set({
    id: id,
    frame: JSON.stringify(obj),
    ts: Math.floor(Date.now() / 1000),
    by: cl_userEmail || 'admin'
  }).catch(function (err) {
    if (cl_pending[id]) { delete cl_pending[id]; if (p.timer) clearTimeout(p.timer); }
    cl_status('write failed: ' + ((err && err.message) || err));
    p.reject(err);
  });
  return promise;
}

// Rebooting saves without leaving the subscription (see handleDisconnect).
function saveApSettings() {
  var ssidInput = document.getElementById('apSsid');
  var passInput = document.getElementById('apPass');
  var ssidRaw = (ssidInput && ssidInput.value) || '';
  var ssid = ssidRaw.trim();
  var pass = (passInput && passInput.value) || '';
  var problem = apValidationMessage(ssidRaw, pass);
  if (problem) return showToast(problem);
  return sendCommand({ cmd: 'set_ap', ssid: ssid, pass: pass }).then(function () {
    if (passInput) passInput.value = '';
    showToast('Saved — fallback AP will reboot to apply it', 6000);
    var hint = document.getElementById('apHint');
    if (hint) hint.innerHTML = '<strong>The board is rebooting.</strong> This view resumes on its own in ~30s.';
    return true;
  }).catch(function () { return false; });
}

function resetApSettings() {
  return sendCommand({ cmd: 'reset_ap' }).then(function () {
    showToast('Resetting to the default network name — the board is restarting', 6000);
    return true;
  }).catch(function () { return false; });
}

function saveStaSettings() {
  var ssidInput = document.getElementById('staSsid');
  var passInput = document.getElementById('staPass');
  var ssid = ((ssidInput && ssidInput.value) || '').trim();
  var pass = (passInput && passInput.value) || '';
  if (!ssid) return showToast('Enter your home WiFi name');
  return sendCommand({ cmd: 'setwifi', ssid: ssid, pass: pass }).then(function () {
    if (passInput) passInput.value = '';
    showToast('Saved — the board restarts to join it; this view resumes on its own', 6000);
    return true;
  }).catch(function () { return false; });
}

function saveCloudSettings() {
  var hostInput = document.getElementById('cloudHost');
  var emailInput = document.getElementById('cloudEmail');
  var authInput = document.getElementById('cloudAuth');
  var host = (((hostInput && hostInput.value) || '').trim()).replace(/^https?:\/\//i, '').split('/')[0];
  var email = ((emailInput && emailInput.value) || '').trim();
  var auth = (authInput && authInput.value) || '';
  if (!host || host.indexOf('.') < 0) return showToast('Enter the database host (no https://, no path)');
  if (!email || email.indexOf('@') < 0) return showToast('Enter the board account email');
  if (!auth) return showToast('Enter the account password');
  return sendCommand({ cmd: 'setcloud', host: host, email: email, pass: auth }).then(function () {
    if (authInput) authInput.value = '';
    showToast('Saved — the board restarts; pushes resume in ~30s', 6000);
    return true;
  }).catch(function () { return false; });
}

// ---- Cloud boot (runs after script.js init tried the board WS) ----
(function cl_boot() {
  try { if (ws && (ws.readyState === 0 || ws.readyState === 1)) ws.close(); } catch (e) {}
  ws = null;
  userDisconnect = true; // stop the board-WS path from complaining
  isDemo = false;
  pinOk = false;
  adminPin = '';

  firebase.initializeApp(CL_FIREBASE_CONFIG);
  cl_db = firebase.database();
  cl_auth = firebase.auth();

  document.getElementById('authBtn').addEventListener('click', cl_googleToggle);
  // authBtn2 lived in the old Admin panel (dropped; its badge+button moved
  // into the Cloud device panel). Guarded: the cloud copy no longer has it.
  var ab2 = document.getElementById('authBtn2');
  if (ab2) ab2.addEventListener('click', cl_googleToggle);
  document.getElementById('devicePicker').addEventListener('change', function (e) {
    cl_selectDevice(e.target.value);
  });
  document.getElementById('cloudReloadBtn').addEventListener('click', cl_loadDevices);

  cl_auth.onAuthStateChanged(function (user) {
    var email = user && user.email ? user.email : null;
    cl_setAdmin(email === CL_ADMIN_EMAIL, email);
  });

  cl_staleTimer = setInterval(cl_refreshStale, 5000);
  handleConnect();
})();
