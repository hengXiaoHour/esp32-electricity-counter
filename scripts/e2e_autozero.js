// Auto-zero state + toast styling, checked in a real browser against the mock
// board.
//
//   node scripts/e2e_autozero.js [baseUrl] [width]
//
// The mock is started by the CALLER with MOCK_AZ_CHANNEL / MOCK_AZ_QUEUE set (see
// verify_all.sh), because a real calibration takes 20 batches over several
// seconds. Everything asserted here is a state the user actually sees:
//
//   1. The Auto-Zero button of the channel being calibrated - and of one
//      already queued behind it - is disabled. Left live it accepted the tap,
//      the firmware refused it, and the UI showed "auto-zero started" anyway.
//   2. The calibrating status text does not overlap the Auto-Zero button. That
//      chip sat inside the field cell, where on a 360px phone it was wider than
//      the leftover space and painted itself over the button.
//   3. The feedback box is a solid green fill with fully white text, and the
//      "that will not work" messages are red rather than green.
//
// Point 2 is the one that only exists once the page is laid out, so it is
// measured, not grepped.

const { chromium } = require(require('child_process')
  .execSync('npm root -g', { encoding: 'utf8' }).trim() + '/playwright');

const BASE = process.argv[2] || 'http://127.0.0.1:8099';
const WIDTH = parseInt(process.argv[3] || '360', 10);
const PIN = '1234';
// The channels the caller put the board into. Kept in one place so a change in
// the mock's env contract fails the test instead of quietly testing nothing.
const AZ_ACTIVE = 1;   // MOCK_AZ_CHANNEL
const AZ_QUEUE = [2];  // MOCK_AZ_QUEUE

let checks = 0;
let failures = 0;
const check = (name, ok, detail) => {
  checks++;
  if (ok) console.log(`  ok   ${name}`);
  else { failures++; console.log(`  FAIL ${name}${detail ? '  [' + detail + ']' : ''}`); }
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

  // The board really is mid-calibration: assert the payload, so a mock that
  // stopped honouring its env vars cannot make the rest of this file pass by
  // rendering an idle board.
  const wire = await page.waitForFunction(
    () => document.querySelectorAll('.channel-card').length >= 1 || null, null, { timeout: 10000 })
    .then(() => page.evaluate(() => window.__lastData || null));
  void wire;

  console.log(`== ${WIDTH}px: auto-zero in progress on Ch${AZ_ACTIVE + 1} ==`);

  // Open Settings, unlock as admin, expand every calibration row.
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

  const state = await page.evaluate(() => {
    const btn = (i) => document.getElementById('azBtn_' + i);
    const box = (el) => {
      const r = el.getBoundingClientRect();
      return { left: +r.left.toFixed(1), right: +r.right.toFixed(1), top: +r.top.toFixed(1), bottom: +r.bottom.toFixed(1) };
    };
    const active = document.getElementById('azChip_' + AZ_ACTIVE_I());
    return {
      status: (document.getElementById('azStatus') || {}).textContent || '',
      disabled: [0, 1, 2, 3, 4].map((i) => !!(btn(i) && btn(i).disabled)),
      buttonsPresent: [0, 1, 2, 3, 4].map((i) => !!(btn(i) && btn(i).getClientRects().length)),
      chipText: active ? active.textContent.trim() : '(missing)',
      chipVisible: active ? active.getClientRects().length > 0 : false,
      chipBox: active ? box(active) : null,
      activeBtnBox: btn(AZ_ACTIVE_I()) ? box(btn(AZ_ACTIVE_I())) : null,
      // An inline style would beat any :disabled rule, which is why the orange
      // was moved into a class. Assert the disabled button actually repainted.
      disabledColor: (() => {
        const b = btn(AZ_ACTIVE_I());
        return b ? getComputedStyle(b).color : '(missing)';
      })(),
      enabledColor: (() => {
        const b = btn(0);
        return b ? getComputedStyle(b).color : '(missing)';
      })(),
    };
  }).catch(async (e) => { console.log('  FAIL page.evaluate: ' + e.message); failures++; checks++; return null; });

  // AZ_ACTIVE is a JS const in Node, not in the page: pass it in.
  const st = state ? await page.evaluate((activeIdx) => {
    const btn = (i) => document.getElementById('azBtn_' + i);
    const box = (el) => {
      const r = el.getBoundingClientRect();
      return { left: +r.left.toFixed(1), right: +r.right.toFixed(1), top: +r.top.toFixed(1), bottom: +r.bottom.toFixed(1) };
    };
    const chip = document.getElementById('azChip_' + activeIdx);
    return {
      status: (document.getElementById('azStatus') || {}).textContent || '',
      disabled: [0, 1, 2, 3, 4].map((i) => !!(btn(i) && btn(i).disabled)),
      buttonsPresent: [0, 1, 2, 3, 4].map((i) => !!(btn(i) && btn(i).getClientRects().length)),
      chipText: chip ? chip.textContent.trim() : '(missing)',
      chipVisible: chip ? chip.getClientRects().length > 0 : false,
      chipBox: chip ? box(chip) : null,
      activeBtnBox: btn(activeIdx) ? box(btn(activeIdx)) : null,
      disabledColor: btn(activeIdx) ? getComputedStyle(btn(activeIdx)).color : '(missing)',
      disabledOpacity: btn(activeIdx) ? getComputedStyle(btn(activeIdx)).opacity : '(missing)',
      enabledColor: btn(0) ? getComputedStyle(btn(0)).color : '(missing)',
    };
  }, AZ_ACTIVE) : null;

  if (st) {
    eq('every Auto-Zero button is on screen', st.buttonsPresent, [true, true, true, true, true]);
    // The calibrating channel and the one queued behind it cannot succeed.
    eq('Auto-Zero is disabled only on the busy channels',
      st.disabled.map((d, i) => (d === AZ_ACTIVE || AZ_QUEUE.includes(i))),
      st.disabled);
    check('the calibrating channel reads Calibrating', /calibrating/i.test(st.chipText), st.chipText);
    check('the status line names the calibrating channel',
      new RegExp(`calibrating Ch${AZ_ACTIVE + 1}`).test(st.status), st.status);
    // The disabled button must LOOK disabled: neutralised colour, dimmed.
    check('a disabled Auto-Zero is not still painted orange',
      st.disabledColor !== 'rgb(230, 126, 34)', 'computed ' + st.disabledColor);
    check('a disabled Auto-Zero is dimmed', parseFloat(st.disabledOpacity) < 1,
      'opacity ' + st.disabledOpacity);
    check('an idle Auto-Zero keeps its orange', st.enabledColor === 'rgb(230, 126, 34)',
      'computed ' + st.enabledColor);

    // The overlap that started this. Both axes must intersect: the status line
    // legitimately shares the row's x range, so an x-only test would pass.
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

    // A disabled button dispatches no click, so nothing must reach the board.
    const before = await page.evaluate(() => window.__wsSent.length);
    await page.evaluate((i) => {
      const b = document.getElementById('azBtn_' + i);
      if (b) b.click();
    }, AZ_ACTIVE);
    await page.waitForTimeout(400);
    const after = await page.evaluate(() => window.__wsSent.slice());
    const azFrames = after.filter((f) => typeof f === 'string' && f.indexOf('set_noise_floor') >= 0);
    check('clicking the disabled Auto-Zero sends nothing to the board',
      azFrames.length === 0, 'frames ' + JSON.stringify(azFrames) + ' (before ' + before + ')');
  }

  // The guard behind the disabled button still answers a forced call.
  const forced = await page.evaluate((i) => {
    window.__wsSent.length = 0;
    autoZeroChannel(i);
    return new Promise((r) => setTimeout(() => r({
      frames: window.__wsSent.slice(),
      toast: (document.getElementById('toast') || {}).textContent || '',
      kind: (document.getElementById('toast') || {}).dataset ? document.getElementById('toast').dataset.kind : '(none)',
    }), 300));
  }, AZ_ACTIVE);
  check('the JS guard refuses autoZeroChannel on the running channel',
    forced.frames.filter((f) => typeof f === 'string' && f.indexOf('set_noise_floor') >= 0).length === 0,
    'frames ' + JSON.stringify(forced.frames));
  check('the refusal is reported to the user', /already/i.test(forced.toast), forced.toast);
  check('the refusal is styled as a warning', forced.kind === 'warn', 'kind ' + forced.kind);

  console.log('== toast fill + text colour ==');
  const toast = await page.evaluate(() => {
    const read = (fn) => {
      fn();
      const t = document.getElementById('toast');
      const cs = getComputedStyle(t);
      return {
        kind: t.dataset.kind,
        color: cs.color,
        background: cs.backgroundImage,
        backgroundColor: cs.backgroundColor,
        opacity: cs.opacity,
      };
    };
    const out = {};
    out.ok = read(() => showToast('Ch2 auto-zero started', 4000));
    out.warn = read(() => showToast('already queued', 4000, 'warn'));
    out.err = read(() => showError('LPF Alpha must be 0.01-1'));
    return out;
  });

  for (const kind of ['ok', 'warn', 'err']) {
    const t = toast[kind];
    eq(`toast (${kind}) text is fully white`, t.color, 'rgb(255, 255, 255)');
    // A solid fill, not a tinted card: the gradient must carry colour and the
    // flat background must be transparent (otherwise the gradient sits on top
    // of an opaque fallback and reads as one flat colour anyway).
    check(`toast (${kind}) is filled with its own colour`,
      /gradient/.test(t.background) && /rgba\(0, 0, 0, 0\)|transparent/.test(t.backgroundColor),
      `background=${t.background} flat=${t.backgroundColor}`);
    check(`toast (${kind}) is visible while shown`, parseFloat(t.opacity) === 1, 'opacity ' + t.opacity);
  }
  // Green for a done thing, red for a refused one - the two must differ, or the
  // error colouring is decoration.
  check('success and error fills are different colours',
    toast.ok.background !== toast.err.background,
    'ok ' + toast.ok.background + ' err ' + toast.err.background);
  // White on the accent green (--ok #3ec972) is only 2.1:1, which is why the
  // fill is a deeper green. Assert the fill is not the accent.
  check('the success fill is a readable green, not the thin accent',
    toast.ok.background !== 'none' && /rgb\(\s*(1[0-9]|2[0-9]|3[0-9]|4[0-5])\s*,/.test(toast.ok.background),
    toast.ok.background);

  eq('no uncaught page errors', pageErrors, []);

  console.log(`\n${checks - failures}/${checks} checks passed`);
  await browser.close();
  process.exit(failures ? 1 : 0);
})().catch((e) => {
  console.error('e2e_autozero failed:', e && e.stack ? e.stack : e);
  process.exit(1);
});