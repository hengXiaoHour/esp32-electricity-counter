// Auto-zero state + toast styling, checked in a real browser against the mock
// board.
//
//   node scripts/e2e_autozero.js [baseUrl] [width]
//
// The caller starts the mock with MOCK_AZ_CHANNEL / MOCK_AZ_QUEUE /
// MOCK_AZ_PROGRESS set (see verify_all.sh): a real calibration takes 20 batches
// over several seconds, and a UI check that never renders the calibrating state
// checks a state the user never sees. Everything asserted here is a state the
// user actually sees:
//
//   1. The Auto-Zero button of the channel being calibrated - and of one already
//      queued behind it - is disabled. Left live it accepted the tap, the
//      firmware refused it, and the UI said "auto-zero started" anyway.
//   2. The calibrating status text does not overlap the Auto-Zero button. That
//      chip sat inside the field cell, where on a 360px phone it was wider than
//      the leftover space and painted itself over the button.
//   3. The feedback box is a solid fill with fully white text, and the "that
//      will not work" messages are red rather than green.
//
// Point 2 only exists once the page is laid out, so it is measured, not grepped.

const { chromium } = require(require('child_process')
  .execSync('npm root -g', { encoding: 'utf8' }).trim() + '/playwright');

const BASE = process.argv[2] || 'http://127.0.0.1:8099';
const WIDTH = parseInt(process.argv[3] || '360', 10);
const PIN = '1234';
// The channels the caller put the board into. One place, so a mock that stops
// honouring its env contract fails this file instead of quietly testing an
// idle board.
const AZ_ACTIVE = 1;   // MOCK_AZ_CHANNEL
const AZ_QUEUE = [2];  // MOCK_AZ_QUEUE

let checks = 0;
let failures = 0;
const check = (name, ok, detail) => {
  checks++;
  if (ok) console.log(`  ok   ${name}`);
  else { failures++; console.log(`  FAIL ${name}${detail ? '  [' + detail + ']' : ''}`); };
};
const eq = (name, got, want) =>
  check(name, JSON.stringify(got) === JSON.stringify(want),
    `got ${JSON.stringify(got)} want ${JSON.stringify(want)}`);

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: WIDTH, height: 900 } });
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e && e.message)));

  await page.addInitScript(() => {
    window.__wsSent = [];
    const Orig = window.WebSocket;
    window.WebSocket = function (url, protocols) {
      const s = protocols ? new Orig(url, protocols) : new Orig(url);
      const origSend = s.send.bind(s);
      s.send = function (d) { try { window.__wsSent.push(d); } catch (e) {} return origSend(d); };
      return s;
    };
    window.WebSocket.prototype = Orig.prototype;
    Object.assign(window.WebSocket, Orig);
  });

  await page.goto(BASE + '/', { waitUntil: 'networkidle' });
  await page.waitForSelector('#statusBar', { timeout: 10000 });

  // The board really is mid-calibration. Asserted from the snapshot the page
  // rendered, so a mock that stopped honouring its env vars cannot let the
  // rest of this file pass against an idle board.
  const seen = await page.waitForFunction(
    () => window.__wsSeen && window.__wsSeen.some((d) => d && d.azActive === true),
    null, { timeout: 10000 }).catch(() => null);
  if (!seen) {
    console.log('  FAIL the mock never sent azActive=true (is MOCK_AZ_CHANNEL set?)');
    failures++; checks++;
  }
  await page.addInitScript(() => {
    window.__wsSeen = [];
    const Orig = window.WebSocket;
    window.WebSocket = function (url, protocols) {
      const s = protocols ? new Orig(url, protocols) : new Orig(url);
      s.addEventListener('message', (e) => {
        try { window.__wsSeen.push(JSON.parse(e.data)); } catch (x) { /* raw text */ }
      });
      return s;
    };
    window.WebSocket.prototype = Orig.prototype;
    Object.assign(window.WebSocket, Orig);
  });

  console.log(`== ${WIDTH}px: auto-zero running on Ch${AZ_ACTIVE + 1} ==`);

  for (const sel of ['#sidebar [data-page="settings"]', '#mobileNav [data-page="settings"]']) {
    const el = await page.$(sel);
    if (el && await el.isVisible()) { await el.click(); break; }
  }
  await page.waitForTimeout(600);
  await page.evaluate((pin) => {
    const i = document.getElementById('pinInput');
    if (i) i.value = pin;
  }, PIN);
  await page.click('#pinRow button');
  await page.waitForTimeout(700);
  await page.evaluate(() => document.querySelectorAll('.cal-collapse-header').forEach((h) => h.click()));
  await page.waitForTimeout(400);

  const st = await page.evaluate((activeIdx) => {
    const btn = (i) => document.getElementById('azBtn_' + i);
    const box = (el) => {
      const r = el.getBoundingClientRect();
      return {
        left: +r.left.toFixed(1), right: +r.right.toFixed(1),
        top: +r.top.toFixed(1), bottom: +r.bottom.toFixed(1),
      };
    };
    const chip = document.getElementById('azChip_' + activeIdx);
    const N = 5;
    return {
      status: (document.getElementById('azStatus') || {}).textContent || '',
      disabled: Array.from({ length: N }, (_, i) => !!(btn(i) && btn(i).disabled)),
      present: Array.from({ length: N }, (_, i) => !!(btn(i) && btn(i).getClientRects().length)),
      chipText: chip ? chip.textContent.trim() : '(missing)',
      chipVisible: chip ? chip.getClientRects().length > 0 : false,
      chipBox: chip ? box(chip) : null,
      activeBtnBox: btn(activeIdx) ? box(btn(activeIdx)) : null,
      disabledColor: btn(activeIdx) ? getComputedStyle(btn(activeIdx)).color : '(missing)',
      disabledOpacity: btn(activeIdx) ? getComputedStyle(btn(activeIdx)).opacity : '(missing)',
      enabledColor: btn(0) ? getComputedStyle(btn(0)).color : '(missing)',
      enabledDisabled: btn(0) ? !!btn(0).disabled : null,
    };
  }, AZ_ACTIVE);

  eq('every Auto-Zero button is on screen', st.present, [true, true, true, true, true]);
  eq('Auto-Zero is disabled on exactly the busy channels',
    st.disabled, Array.from({ length: 5 }, (_, i) => i === AZ_ACTIVE || AZ_QUEUE.includes(i)));
  check('the calibrating channel reads Calibrating', /calibrating/i.test(st.chipText), st.chipText);
  check('the status line names the calibrating channel',
    new RegExp('calibrating Ch' + (AZ_ACTIVE + 1)).test(st.status), st.status);
  // The disabled button must LOOK disabled. An inline colour would beat any
  // :disabled rule, which is why the orange moved into a class.
  check('a disabled Auto-Zero is not still painted orange',
    st.disabledColor !== 'rgb(230, 126, 34)', 'computed ' + st.disabledColor);
  check('a disabled Auto-Zero is dimmed', parseFloat(st.disabledOpacity) < 1,
    'opacity ' + st.disabledOpacity);
  check('an idle Auto-Zero keeps its orange', st.enabledColor === 'rgb(230, 126, 34)',
    'computed ' + st.enabledColor);

  // The overlap that started this. BOTH axes must intersect: the status line
  // legitimately shares the row's x range, so an x-only test passes.
  if (st.chipVisible && st.activeBtnBox) {
    const c = st.chipBox, b = st.activeBtnBox;
    const xOverlap = c.left < b.right - 0.5 && b.left < c.right - 0.5;
    const yOverlap = c.top < b.bottom - 0.5 && b.top < c.bottom - 0.5;
    check('the Calibrating text does not overlap the Auto-Zero button',
      !(xOverlap && yOverlap), `chip ${JSON.stringify(c)} button ${JSON.stringify(b)}`);
  } else {
    check('the Calibrating text is visible to measure', false,
      'chipVisible=' + st.chipVisible);
  }

  // A disabled button dispatches no click, so nothing may reach the board.
  await page.evaluate((i) => {
    window.__wsSent.length = 0;
    const b = document.getElementById('azBtn_' + i);
    if (b) b.click();
  }, AZ_ACTIVE);
  await page.waitForTimeout(400);
  const clicked = await page.evaluate(() => window.__wsSent.slice());
  check('clicking the disabled Auto-Zero sends nothing to the board',
    clicked.filter((f) => typeof f === 'string' && f.indexOf('set_noise_floor') >= 0).length === 0,
    'frames ' + JSON.stringify(clicked));

  // The guard behind the disabled button still answers a forced call.
  const forced = await page.evaluate((i) => {
    window.__wsSent.length = 0;
    autoZeroChannel(i);
    return new Promise((r) => setTimeout(() => {
      const t = document.getElementById('toast');
      r({
        frames: window.__wsSent.slice(),
        toast: t ? t.textContent : '',
        kind: t ? t.dataset.kind : '(none)',
      });
    }, 400));
  }, AZ_ACTIVE);
  check('the JS guard refuses autoZeroChannel on the running channel',
    forced.frames.filter((f) => typeof f === 'string' && f.indexOf('set_noise_floor') >= 0).length === 0,
    'frames ' + JSON.stringify(forced.frames));
  check('the refusal is reported to the user', /already/i.test(forced.toast), forced.toast);
  check('the refusal is styled as a warning', forced.kind === 'warn', 'kind ' + forced.kind);

  console.log('== feedback box fill + text colour ==');
  const toast = await page.evaluate(async () => {
    const read = async (fn) => {
      fn();
      // The toast fades in over 0.25s; reading computed style immediately
      // catches it mid-transition and reports a colour that is not there yet.
      await new Promise((r) => setTimeout(r, 450));
      const t = document.getElementById('toast');
      const cs = getComputedStyle(t);
      return {
        kind: t.dataset.kind,
        color: cs.color,
        gradient: cs.backgroundImage,
        flat: cs.backgroundColor,
        opacity: cs.opacity,
      };
    };
    return {
      ok: await read(() => showToast('Ch2 auto-zero started', 4000)),
      warn: await read(() => showToast('already queued', 4000, 'warn')),
      err: await read(() => showError('LPF Alpha must be 0.01-1')),
    };
  });

  for (const kind of ['ok', 'warn', 'err']) {
    const t = toast[kind];
    eq(`feedback (${kind}) text is fully white`, t.color, 'rgb(255, 255, 255)');
    // A solid fill, not a tinted card: the gradient carries the colour and the
    // flat background must be transparent, or the gradient is painted over an
    // opaque fallback and the box just reads as one flat colour.
    check(`feedback (${kind}) is filled with its own colour`,
      /gradient/.test(t.gradient) && /rgba\(0, 0, 0, 0\)|transparent/.test(t.flat),
      `gradient=${t.gradient} flat=${t.flat}`);
    check(`feedback (${kind}) is fully visible while shown`,
      parseFloat(t.opacity) === 1, 'opacity ' + t.opacity);
  }
  check('success and error fills are different colours',
    toast.ok.gradient !== toast.err.gradient,
    'ok ' + toast.ok.gradient + ' err ' + toast.err.gradient);
  // White on the accent green (--ok #3ec972) is only ~2.1:1, which is why the
  // fill is a deeper green. Assert the fill is NOT the accent itself.
  const greenChannels = (toast.ok.gradient.match(/rgb\(\s*\d+\s*,\s*(\d+)\s*,\s*\d+\s*\)/g) || [])
    .map((s) => parseInt(s.match(/,\s*(\d+)\s*,/)[1], 10));
  check('the success fill is a readable green, not the thin accent',
    greenChannels.length === 2 && greenChannels.every((g) => g >= 0x40 && g <= 0x90),
    'green channel(s) ' + JSON.stringify(greenChannels) + ' in ' + toast.ok.gradient);

  eq('no uncaught page errors', pageErrors, []);

  console.log(`\n${checks - failures}/${checks} checks passed`);
  await browser.close();
  process.exit(failures ? 1 : 0);
})().catch((e) => {
  console.error('e2e_autozero failed:', e && e.stack ? e.stack : e);
  process.exit(1);
});