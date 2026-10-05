/* Cloud viewer for the electricity counter.
 * Public read-only mirror: anyone can watch the readings.
 * Admin (Google sign-in as the owner address, no board PIN involved here)
 * gets full remote control via /devices/<MAC>/cmd, replies on /ack.
 */
(function () {
  'use strict';

  var ADMIN_EMAIL = 'heng.xiao.hour@gmail.com';
  var DEFAULT_MAC = 'EC64C998B0EC';
  var NUM_CHANNELS = 5;
  var STALE_MS = 30000;

  var firebaseConfig = {
    apiKey: 'AIzaSyA1BCYnxBc9q_ONa58TTkGimlGPn0wyvj0',
    authDomain: 'esp32-electricity-counter.firebaseapp.com',
    databaseURL: 'https://esp32-electricity-counter-default-rtdb.firebaseio.com',
    projectId: 'esp32-electricity-counter'
  };

  firebase.initializeApp(firebaseConfig);
  var db = firebase.database();
  var auth = firebase.auth();

  var currentMac = null;
  var latestUnsub = null;
  var ackUnsub = null;
  var lastSentId = null;
  var lastSentAt = 0;
  var userEmail = null;
  var staleTimer = null;

  function $(id) { return document.getElementById(id); }

  function banner(msg) {
    var b = $('banner');
    if (!msg) { b.hidden = true; b.textContent = ''; return; }
    b.hidden = false;
    b.textContent = msg;
  }

  function esc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;');
  }

  // ---------- device list (public, no login needed) ----------
  function loadDevices() {
    banner(null);
    // Shallow REST keeps this to bare keys; falls back to an SDK read.
    return fetch(firebaseConfig.databaseURL + '/devices.json?shallow=true')
      .then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(function (obj) { return obj ? Object.keys(obj) : []; })
      .catch(function () {
        return db.ref('devices').get().then(function (snap) {
          var v = snap.val();
          return v ? Object.keys(v) : [];
        });
      })
      .then(function (keys) {
        keys.sort();
        var sel = $('devicePicker');
        sel.innerHTML = '';
        if (!keys.length) {
          banner('No devices found under /devices yet.');
          return;
        }
        keys.forEach(function (k) {
          var o = document.createElement('option');
          o.value = k;
          o.textContent = k;
          sel.appendChild(o);
        });
        var pick = keys.indexOf(DEFAULT_MAC) >= 0 ? DEFAULT_MAC : keys[0];
        sel.value = pick;
        selectDevice(pick);
      })
      .catch(function (err) {
        banner('Could not list devices: ' + (err && err.message || err));
      });
  }

  function selectDevice(mac) {
    currentMac = mac;
    lastSentId = null;
    setCmdStatus('No command sent yet.');
    if (latestUnsub) { latestUnsub.off(); latestUnsub = null; }
    if (ackUnsub) { ackUnsub.off(); ackUnsub = null; }
    var latestRef = db.ref('devices/' + mac + '/latest');
    latestUnsub = latestRef;
    latestRef.on('value', function (snap) { renderLatest(snap.val()); },
      function (err) { banner('Read failed: ' + (err && err.message || err)); });
    var ackRef = db.ref('devices/' + mac + '/ack');
    ackUnsub = ackRef;
    ackRef.on('value', function (snap) { renderAck(snap.val()); });
  }

  // ---------- public readings ----------
  var STATUS_LABEL = ['OK', 'HIGH', 'TRIPPED', 'OFF'];
  var STATUS_CLASS = ['st-ok', 'st-warn', 'st-trip', 'st-off'];

  function fmtUptime(s) {
    s = Math.max(0, Math.floor(Number(s) || 0));
    var d = Math.floor(s / 86400); s -= d * 86400;
    var h = Math.floor(s / 3600); s -= h * 3600;
    var m = Math.floor(s / 60); s -= m * 60;
    if (d > 0) return d + 'd ' + h + 'h';
    if (h > 0) return h + 'h ' + m + 'm';
    if (m > 0) return m + 'm ' + s + 's';
    return s + 's';
  }

  var lastEpochMs = 0;

  var lastRxMs = 0;

  function renderLatest(v) {
    lastRxMs = Date.now();
    if (!v) {
      $('noview').hidden = false;
      $('readings').hidden = true;
      $('noview').textContent = 'No readings yet for ' + currentMac + '.';
      return;
    }
    $('noview').hidden = true;
    $('readings').hidden = false;
    $('rDev').textContent = v.dev || currentMac;
    var epochMs = (Number(v.epoch) || 0) * 1000;
    lastEpochMs = epochMs;
    $('rTime').textContent = epochMs > 0 ? new Date(epochMs).toLocaleString() : '--';
    $('rUptime').textContent = fmtUptime(v.uptime);
    $('rRssi').textContent = (v.rssi !== undefined && v.rssi !== null) ? v.rssi + ' dBm' : '--';
    $('rVolt').textContent = (v.v !== undefined && v.v !== null) ? Number(v.v).toFixed(1) + ' V' : '--';
    $('rMcu').textContent = (v.mcu !== undefined && v.mcu !== null) ? Number(v.mcu).toFixed(1) + ' C' : '--';
    $('rEco').textContent = v.eco ? 'ON' : 'OFF';
    var tb = $('chBody');
    tb.innerHTML = '';
    var ch = Array.isArray(v.ch) ? v.ch : [];
    for (var i = 0; i < NUM_CHANNELS; i++) {
      var c = ch[i] || {};
      var st = Math.min(3, Math.max(0, parseInt(c.s, 10) || 0));
      var tr = document.createElement('tr');
      tr.innerHTML =
        '<td>CH' + (i + 1) + '</td>' +
        '<td class="num">' + (c.w !== undefined ? Number(c.w).toFixed(1) : '--') + '</td>' +
        '<td class="num">' + (c.kwh !== undefined ? Number(c.kwh).toFixed(3) : '--') + '</td>' +
        '<td class="' + STATUS_CLASS[st] + '">' + STATUS_LABEL[st] + '</td>';
      tb.appendChild(tr);
    }
    refreshStale();
  }

  function refreshStale() {
    // Receipt time, not board epoch: the board clock can lag the browser
    // clock (NTP vs lend), which would cry STALE on a live stream.
    var stale = !lastRxMs || (Date.now() - lastRxMs > STALE_MS);
    $('staleTag').hidden = !stale;
    var dot = $('liveDot');
    dot.className = 'dot ' + (lastRxMs ? (stale ? 'stale' : 'live') : '');
  }

  setInterval(refreshStale, 5000);

  // ---------- auth: Google only, no board PIN anywhere in cloud ----------
  $('authBtn').addEventListener('click', function () {
    if (auth.currentUser) {
      auth.signOut();
    } else {
      var provider = new firebase.auth.GoogleAuthProvider();
      auth.signInWithPopup(provider).catch(function (err) {
        banner('Sign-in failed: ' + (err && err.message || err));
      });
    }
  });

  auth.onAuthStateChanged(function (user) {
    userEmail = user && user.email ? user.email : null;
    var isAdmin = userEmail === ADMIN_EMAIL;
    $('userEmail').textContent = userEmail || '';
    $('authBtn').textContent = user ? 'Sign out' : 'Sign in with Google';
    $('adminBadge').hidden = !isAdmin;
    $('adminPanel').hidden = !isAdmin;
    $('stateLine').textContent = isAdmin
      ? 'Admin via Google (' + userEmail + ') - no PIN in cloud.'
      : 'Admin via Google - no PIN in cloud. Local board dashboard keeps its PIN.';
  });

  // ---------- admin commands: same shapes as the LAN dashboard sends ----------
  function chIdx() {
    return Math.max(0, Math.min(NUM_CHANNELS - 1, parseInt($('chSel').value, 10) || 0));
  }

  function numVal(id) {
    var raw = $(id).value.trim();
    if (!raw) return null;
    var n = Number(raw);
    return isNaN(n) ? undefined : n;
  }

  function buildCmd(kind) {
    var ch = chIdx();
    switch (kind) {
      case 'set_name': {
        var name = $('chName').value.trim();
        if (!name) return { error: 'Type a channel name first.' };
        return { cmd: 'set_name', ch: ch, name: name };
      }
      case 'set_monthly_kwh': {
        var mk = numVal('mkwh');
        if (mk === null) return { error: 'Type a monthly kWh value first.' };
        if (mk === undefined || mk <= 0) return { error: 'Monthly kWh must be > 0.' };
        return { cmd: 'set_monthly_kwh', ch: ch, val: mk };
      }
      case 'reset_counter':
        return { cmd: 'reset_counter', ch: ch };
      case 'set_voltage_cal': {
        var vc = numVal('voltCal');
        if (vc === null) return { error: 'Type a voltage cal value first.' };
        if (vc === undefined) return { error: 'Voltage cal must be a number.' };
        return { cmd: 'set_voltage_cal', val: vc };
      }
      case 'set_current_cal': {
        var cc = numVal('currCal');
        if (cc === null) return { error: 'Type a current cal value first.' };
        if (cc === undefined || cc <= 0) return { error: 'Current cal must be > 0.' };
        return { cmd: 'set_current_cal', ch: ch, val: cc };
      }
      case 'set_noise_floor': {
        var nf = numVal('nfVal');
        if (nf === null) return { cmd: 'set_noise_floor', ch: ch }; // blank = auto-zero
        if (nf === undefined || nf < 0) return { error: 'Noise floor must be >= 0.' };
        return { cmd: 'set_noise_floor', ch: ch, val: nf };
      }
      case 'autozero':
        return { cmd: 'set_noise_floor', ch: ch };
      case 'set_lpf': {
        var lp = numVal('lpfVal');
        if (lp === null) return { error: 'Type an LPF alpha first (0.01-1).' };
        if (lp === undefined || lp < 0.01 || lp > 1) return { error: 'LPF alpha must be 0.01-1.' };
        return { cmd: 'set_lpf', ch: ch, val: lp };
      }
      case 'set_rms_samples': {
        var rs = numVal('rmsVal');
        if (rs === null) return { error: 'Type RMS samples first (100-2000).' };
        if (rs === undefined || rs < 100 || rs > 2000) return { error: 'RMS samples must be 100-2000.' };
        return { cmd: 'set_rms_samples', val: Math.floor(rs) };
      }
      case 'set_az_batches': {
        var az = numVal('azVal');
        if (az === null) return { error: 'Type AZ batches first (1-64).' };
        if (az === undefined || az < 1 || az > 64) return { error: 'AZ batches must be 1-64.' };
        return { cmd: 'set_az_batches', val: Math.floor(az) };
      }
      case 'reset_ch_cal':
        return { cmd: 'reset_ch_cal', ch: ch };
      case 'reset_ch_to_default':
        return { cmd: 'reset_ch_to_default', ch: ch };
      case 'reset_nvs_defaults':
        return { cmd: 'reset_nvs_defaults' };
      case 'set_pin': {
        var np = $('pinNew').value.trim();
        if (!np) return { error: 'Type the new LAN PIN first.' };
        return { cmd: 'set_pin', pin_new: np };
      }
      case 'set_ap': {
        var ssid = $('apSsid').value.trim();
        var pass = $('apPass').value;
        if (!ssid) return { error: 'Type the AP name first.' };
        if (pass.length < 8 || pass.length > 63) return { error: 'AP password must be 8-63 chars.' };
        return { cmd: 'set_ap', ssid: ssid, pass: pass };
      }
      case 'reset_ap':
        return { cmd: 'reset_ap' };
      case 'setwifi': {
        var ws = $('staSsid').value.trim();
        var wp = $('staPass').value;
        if (!ws) return { error: 'Type the home WiFi name first.' };
        if (!wp) return { error: 'Type the home WiFi password first.' };
        return { cmd: 'setwifi', ssid: ws, pass: wp };
      }
      case 'clearwifi':
        return { cmd: 'clearwifi' };
      case 'setcloud': {
        var host = $('clHost').value.trim();
        var em = $('clEmail').value.trim();
        var pw = $('clPass').value;
        if (!host) return { error: 'Type the database host first.' };
        if (!em) return { error: 'Type the cloud account email first.' };
        if (!pw) return { error: 'Type the cloud account password first.' };
        return { cmd: 'setcloud', host: host, email: em, pass: pw };
      }
      case 'clearcloud':
        return { cmd: 'clearcloud' };
      case 'console': {
        var line = $('consoleLine').value.trim();
        if (!line) return { error: 'Type a console line first.' };
        return { cmd: 'console', line: line };
      }
      case 'test_force_rollover':
        return { cmd: 'test_force_rollover' };
      default:
        return { error: 'Unknown command.' };
    }
  }

  function setCmdStatus(msg) {
    $('cmdStatus').textContent = msg;
  }

  function sendCmd(kind) {
    if (!currentMac) { setCmdStatus('Pick a device first.'); return; }
    if (userEmail !== ADMIN_EMAIL) { setCmdStatus('Admin sign-in required.'); return; }
    var built = buildCmd(kind);
    if (built.error) { setCmdStatus(built.error); return; }
    // Wipe password-style fields the moment they leave the page.
    ['apPass', 'staPass', 'clPass'].forEach(function (id) {
      var el = $(id);
      if (el && (kind === 'set_ap' || kind === 'setwifi' || kind === 'setcloud')) el.value = '';
    });
    if (kind === 'console') $('consoleLine').value = '';
    var id = String(Date.now()) + '-' + Math.floor(Math.random() * 1000000);
    lastSentId = id;
    lastSentAt = Date.now();
    setCmdStatus('sent ' + built.cmd + ' (id ' + id + '), waiting for board (polls every ~10s)...');
    db.ref('devices/' + currentMac + '/cmd').set({
      id: id,
      frame: JSON.stringify(built),
      ts: Math.floor(Date.now() / 1000),
      by: userEmail
    }).catch(function (err) {
      setCmdStatus('write failed: ' + (err && err.message || err));
    });
  }

  function renderAck(ack) {
    if (!ack || !lastSentId) return;
    if (ack.id !== lastSentId) return; // an older reply, not ours
    var age = Math.round((Date.now() - lastSentAt) / 1000);
    var line = 'board replied in ~' + age + 's: ok=' + ack.ok;
    if (ack.out !== undefined) line += ' out=' + ack.out;
    setCmdStatus(line);
    var cout = $('consoleOut');
    cout.textContent += line + '\n';
    cout.scrollTop = cout.scrollHeight;
  }

  // Two-click confirm for destructive verbs (matches LAN dashboard habit).
  Array.prototype.forEach.call(document.querySelectorAll('[data-cmd]'), function (btn) {
    btn.addEventListener('click', function () {
      var need = btn.getAttribute('data-confirm');
      if (need && !btn.classList.contains('armed')) {
        btn.classList.add('armed');
        var orig = btn.textContent;
        btn.textContent = 'Confirm?';
        setTimeout(function () {
          btn.classList.remove('armed');
          btn.textContent = orig;
        }, 3000);
        return;
      }
      btn.classList.remove('armed');
      sendCmd(btn.getAttribute('data-cmd'));
    });
  });

  $('consoleLine').addEventListener('keydown', function (ev) {
    if (ev.key === 'Enter') { ev.preventDefault(); sendCmd('console'); }
  });

  // ---------- wiring ----------
  (function init() {
    var chSel = $('chSel');
    for (var i = 0; i < NUM_CHANNELS; i++) {
      var o = document.createElement('option');
      o.value = String(i);
      o.textContent = 'CH' + (i + 1);
      chSel.appendChild(o);
    }
    $('devicePicker').addEventListener('change', function (e) {
      selectDevice(e.target.value);
    });
    $('refreshBtn').addEventListener('click', loadDevices);
    loadDevices();
  })();
})();
