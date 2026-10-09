// Browser-level proof that the CLOUD viewer fills the Settings > Calibration
// panel from a board payload.
//
// The unit test (scripts/e2e_cloud_adapt.js) proves cl_adapt maps the fields.
// This one proves the whole chain in a real page: cloud.js adapter ->
// script.js renderer -> the actual input elements, with a stubbed Firebase so
// the real cloud-viewer/index.html + script.js + cloud.js run unmodified.
//
// The bug it guards: the cloud payload carried no calibration at all, so the
// cloud Settings panel rendered blank while the board held every value. A
// mapping test cannot see that the renderer then fails to fill an input; this
// reads the input values.
//
// Usage:  node scripts/e2e_cloud_settings.js [baseUrl]   (default: the mock-served cloud viewer)
// Exit 0 = every calibration field shows the value the payload carried.
'use strict';

const { chromium } = require(require('child_process')
  .execSync('npm root -g', { encoding: 'utf8' }).trim() + '/playwright');

const base = process.argv[2] || 'http://127.0.0.1:8098';
const ADMIN = 'heng.xiao.hour@gmail.com';
const MAC = 'EC64C998B0EC';

// Distinctive values: a field-to-field mix-up cannot pass, and a field that
// falls back to its HTML default (100 / 1000 / 48) cannot pass either.
const PAYLOAD = {
  dev: MAC, epoch: 1760000000, uptime: 4242, rssi: -58, v: 231.4, mcu: 51.2,
  eco: false, wifi: true, ap: false, staIp: '192.168.1.50', apIp: '',
  voltageCalibration: 231.4,
  currentCalibration: [100.0, 88.5, 112.0, 97.3, 105.6],
  rmsSamples: 1000,
  azBatches: 20,
  noiseFloor: [0.012, 0.034, 0.056, 0.078, 0.091],
  azActive: false, azChannel: -1, azProgress: 0, azQueue: [],
  lpfAlpha: [1.0, 0.5, 0.25, 0.12, 0.06],
  fw: '3.3.5', chip: 'esp32s3', otaRun: false, otaPct: 0,
  time: { ok: true, age: 4 },
  lastMonth: 202610, resetDay: 25,
  apSsid: 'ESP32-Elec-Counter', apIsDefault: true,
  staSsid: 'HomeNet', staIsDefault: false,
  cloud: {
    en: true, ok: true, age: 1,
    host: 'esp32-electricity-counter-default-rtdb.firebaseio.com',
    acct: 'meter@example.com',
  },
  ch: [
    { n: 'Counter 1', a: 1.2, w: 45.0, pf: 0.87, kwh: 1.5, s: 0, mkwh: 48 },
    { n: 'Counter 2', a: 1.6, w: 75.0, pf: 0.9, kwh: 2.5, s: 0, mkwh: 60 },
    { n: 'Counter 3', a: 2.0, w: 105.0, pf: 0.92, kwh: 3.5, s: 0, mkwh: 72 },
    { n: 'Counter 4', a: 2.3, w: 135.0, pf: 0.95, kwh: 4.5, s: 0, mkwh: 84 },
    { n: 'Counter 5', a: 2.7, w: 165.0, pf: 0.97, kwh: 5.5, s: 0, mkwh: 96 },
  ],
  events: [],
};

// Minimal Firebase stub: enough for cloud.js's boot, device picker and
// subscription. Values come back on the next tick, like a real listener.
const FIREBASE_STUB = `
(function () {
  const listeners = {};
  function makeRef(path) {
    return {
      path,
      on(evt, cb) {
        (listeners[path] = listeners[path] || []).push(cb);
        // The payload needs its OWN parentheses: () => {...} is parsed as an
        // arrow with a BLOCK body, so a bare object literal is a syntax error
        // ("Unexpected token ':'"). That error killed cloud.js's boot, which is
        // why the first run showed every field at its HTML default.
        setTimeout(() => cb({ val: () => (${JSON.stringify(PAYLOAD)}) }), 60);
        return this;
      },
      off() { return this; },
      set() { return Promise.resolve(); },
      get() { return Promise.resolve({ val: () => (${JSON.stringify(PAYLOAD)}) }); },
    };
  }
  window.firebase = {
    initializeApp() {},
    database: () => ({ ref: makeRef }),
    auth: () => ({
      currentUser: { email: ${JSON.stringify(ADMIN)} },
      onAuthStateChanged(cb) { setTimeout(() => cb({ email: ${JSON.stringify(ADMIN)} }), 20); },
      signOut() {},
      signInWithPopup() { return Promise.resolve(); },
    }),
  };
  window.firebase.auth.GoogleAuthProvider = function () {};
  // The REST "render in ~1s" shortcut must not hit the network in a test.
  window.fetch = () => Promise.reject(new Error('offline in the test'));
  // /devices.json?shallow=true must return the MAC list, or cl_loadDevices
  // falls back to Object.keys(payload) and picks a FIELD name as a device id.
  window.__deviceList = ${JSON.stringify(PAYLOAD.dev)};
  const realFetch = window.fetch;
  window.fetch = function (url) {
    if (typeof url === 'string' && url.indexOf('devices.json') >= 0) {
      return Promise.resolve({ ok: true, json: () => Promise.resolve({ [${JSON.stringify(PAYLOAD.dev)}]: true }) });
    }
    return realFetch.apply(this, arguments);
  };
})();
`;

let checks = 0;
let failures = 0;

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e && e.message)));
  // The real SDK must NOT load. It warns "Firebase is already defined" and then
  // silently REPLACES window.firebase, so firebase.database() became a live
  // client that never delivers a value in this sandbox: every field stayed at
  // its HTML default and the admin check never fired. The first run of this
  // test "passed" nothing for exactly that reason - it was measuring a page
  // with no transport at all, not a working cloud viewer.
  await page.route('**/firebasejs/**', (route) => route.abort());
  await page.addInitScript(FIREBASE_STUB);
  await page.goto(base + '/', { waitUntil: 'domcontentloaded' });
  await page.waitForTimeout(1500);

  // Open Settings the way the app does, then let the payload render.
  for (const sel of ['#sidebar [data-page="settings"]', '#mobileNav [data-page="settings"]']) {
    const el = await page.$(sel);
    if (el && await el.isVisible()) { await el.click(); break; }
  }
  await page.waitForTimeout(800);

  const state = await page.evaluate(() => {
    const val = (id) => {
      const el = document.getElementById(id);
      return el ? String(el.value) : '(missing #' + id + ')';
    };
    const all = (id) => [...document.querySelectorAll('#calibrationRows [id^="' + id + '"]')]
      .map((el) => String(el.value));
    return {
      isAdmin: document.body.classList.contains('role-admin'),
      voltCal: val('voltCal'),
      rmsSamples: val('rmsSamples'),
      azBatches: val('azBatches'),
      resetDay: val('resetDay'),
      currCal: all('currCal_'),
      // The id is nf_<i> (noise floor), not noiseFloor_<i>: asserting against
      // the wrong prefix returns an empty array, which reads as "the panel
      // rendered no rows" rather than "you spelled it wrong".
      noiseFloor: all('nf_'),
      lpf: all('lpf_'),
      cards: document.querySelectorAll('.channel-card').length,
      // The panel must actually be on screen, not just present in the DOM:
      // an admin-only panel that stayed hidden would "pass" every value check.
      calPanelVisible: (() => {
        const h = [...document.querySelectorAll('.panel-header h3')]
          .find((e) => e.textContent.trim() === 'System Calibration');
        const panel = h && h.closest('.panel-box');
        return !!(panel && panel.getClientRects().length > 0);
      })(),
    };
  });

  const eq = (name, got, want) => {
    checks++;
    const a = JSON.stringify(got), b = JSON.stringify(want);
    if (a === b) console.log(`  ok   ${name}`);
    else { failures++; console.log(`  FAIL ${name}  [got ${a} want ${b}]`); }
  };

  eq('the cloud viewer is admin (Google signed in)', state.isAdmin, true);
  eq('the System Calibration panel is visible', state.calPanelVisible, true);
  eq('voltage calibration', state.voltCal, '231.4');
  eq('RMS samples', state.rmsSamples, '1000');
  eq('auto-zero batches', state.azBatches, '20');
  eq('current calibration rows', state.currCal,
    ['100.0', '88.5', '112.0', '97.3', '105.6']);
  eq('noise floor rows', state.noiseFloor,
    ['0.012', '0.034', '0.056', '0.078', '0.091']);
  eq('LPF alpha rows', state.lpf, ['1.00', '0.50', '0.25', '0.12', '0.06']);
  eq('billing reset day (same payload, same path)', state.resetDay, '25');
  eq('channel cards rendered', state.cards, 5);

  // The install UI is cloud-only, so this is the only place it can be checked.
  // A name-level grep proves the markup shipped; this proves the module LOADED
  // and its handlers are live, which is what actually makes the button work.
  const inst = await page.evaluate(() => {
    const q = (id) => document.getElementById(id);
    const vis = (el) => !!el && el.getClientRects().length > 0;
    return {
      loaded: ['promptInstall', 'showInstallRow', 'hideInstallRow'].every(
        (fn) => typeof window[fn] === 'function'),
      // Hidden until the browser actually offers an install prompt.
      hiddenNow: ['installRow', 'installBtnTop'].map((id) =>
        !vis(q(id))),
      // And the manual-steps hint is what answers when no prompt exists.
      hint: q('installHint') ? q('installHint').classList.contains('hidden') : null,
    };
  });
  eq('the install module loaded on the hosted page', inst.loaded, true);
  eq('no install control is offered before the browser asks', inst.hiddenNow, [true, true]);
  eq('the manual-steps hint starts hidden', inst.hint, true);
  // Drive the handler the way a click would, on a page where no prompt exists.
  const afterClick = await page.evaluate(() => {
    promptInstall();
    return document.getElementById('installHint').classList.contains('hidden');
  });
  check('the install button reveals the manual steps when no prompt exists',
    afterClick === false, 'hint still hidden after promptInstall()');

  eq('no uncaught page errors', pageErrors, []);

  await browser.close();
  console.log(`\n${checks - failures}/${checks} cloud settings checks passed`);
  process.exit(failures ? 1 : 0);
})().catch((e) => {
  console.error('e2e_cloud_settings failed:', e && e.stack ? e.stack : e);
  process.exit(1);
});