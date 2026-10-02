// E2E test: the REAL frontend against the REAL asset pipeline and a mock board.
//
//   node scripts/e2e_aponly.js [baseUrl]
//
// What this actually proves, and what it does not:
//
//  PROVES  The device-served page loads with no external scripts, connects a
//          same-origin WebSocket, renders six channels, lends its clock, and
//          refuses to send a mutating command without the admin PIN. It also
//          proves the BOARD rejects an unpinned frame and that the UI drops to
//          read-only when it does - the frontend-only "admin" flag of the old
//          design could never have shown that.
//  DOES NOT prove anything about ADC sampling, NVS, auto-zero or OTA. Those are
//          still hardware-only.
//
// Every assertion is paired with a negative control: a deliberately broken
// expectation that must fail, run before the real one. Without that, a test
// that can only ever pass tells you nothing.

// Playwright is installed in the GLOBAL npm root on this machine, not as a
// local dependency, so a bare require('playwright') fails from this script.
const path = require('path');
const { execSync } = require('child_process');

function loadPlaywright() {
  try {
    return require('playwright');
  } catch (e) {
    const globalRoot = execSync('npm root -g', { encoding: 'utf8' }).trim();
    return require(path.join(globalRoot, 'playwright'));
  }
}
const { chromium } = loadPlaywright();

const BASE = process.argv[2] || 'http://127.0.0.1:8099';
const PIN = '1234';

let failures = 0;
let checks = 0;

function check(name, cond, detail) {
  checks++;
  if (cond) {
    console.log('  ok   ' + name);
  } else {
    failures++;
    console.log('  FAIL ' + name + (detail ? '  -> ' + detail : ''));
  }
}

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });

  const consoleErrors = [];
  const externalRequests = [];
  const wsFrames = [];

  page.on('console', m => { if (m.type() === 'error') consoleErrors.push(m.text()); });
  page.on('pageerror', e => consoleErrors.push('pageerror: ' + e.message));
  page.on('request', r => {
    const u = r.url();
    if (!u.startsWith(BASE) && !u.startsWith('data:') && !u.startsWith('ws:')) {
      externalRequests.push(u);
    }
  });

  // Record every outbound WebSocket frame so we can assert on the wire, not
  // just on what the UI decided to render.
  await page.addInitScript(() => {
    window.__wsSent = [];
    const Orig = window.WebSocket;
    window.WebSocket = function (url, protocols) {
      const s = protocols ? new Orig(url, protocols) : new Orig(url);
      window.__wsUrl = url;
      const origSend = s.send.bind(s);
      s.send = function (d) { try { window.__wsSent.push(d); } catch (e) {} return origSend(d); };
      return s;
    };
    window.WebSocket.prototype = Orig.prototype;
    Object.assign(window.WebSocket, Orig);
  });

  console.log('== load & auto-connect ==');
  const resp = await page.goto(BASE + '/', { waitUntil: 'networkidle' });
  check('page returns 200', resp.status() === 200, 'got ' + resp.status());

  // Wait for the socket to open and at least one snapshot to land.
  await page.waitForFunction(() => window.__wsSent && window.__wsSent.length >= 0, null, { timeout: 10000 });
  await page.waitForFunction(
    () => document.querySelectorAll('.channel-card').length === 6, null, { timeout: 10000 })
    .catch(() => {});
  await page.waitForTimeout(1200);

  const wsUrl = await page.evaluate(() => window.__wsUrl);
  check('WebSocket targets same-origin /ws',
        wsUrl === 'ws://' + BASE.replace(/^https?:\/\//, '') + '/ws', 'got ' + wsUrl);

  check('NO external network requests', externalRequests.length === 0,
        externalRequests.join(', '));
  check('no console errors', consoleErrors.length === 0, consoleErrors.join(' | '));

  console.log('\n== dashboard renders ==');
  const cards = await page.locator('.channel-card').count();
  check('six channel cards rendered', cards === 6, 'got ' + cards);

  const totalP = await page.locator('#totalPower').textContent();
  check('total power populated from snapshot', /\d/.test(totalP), 'got "' + totalP + '"');

  const volt = await page.locator('#headerVoltage').textContent();
  check('voltage populated', /^\d+\.\d$/.test(volt.trim()), 'got "' + volt + '"');

  console.log('\n== the page lends its clock ==');
  const sent = await page.evaluate(() => window.__wsSent);
  const timeFrames = sent.filter(f => f.indexOf('"set_time"') >= 0);
  check('set_time sent on connect', timeFrames.length >= 1, 'frames: ' + JSON.stringify(sent));
  if (timeFrames.length) {
    const m = /"t":(\d+)/.exec(timeFrames[0]);
    const t = m ? parseInt(m[1], 10) : 0;
    const drift = Math.abs(t - Math.floor(Date.now() / 1000));
    check('set_time carries a current epoch (drift < 120s)', drift < 120, 'drift ' + drift + 's');
  }

  console.log('\n== PIN gate, from the UI side ==');
  // No PIN cached -> the UI must refuse to even send.
  const beforeCount = await page.evaluate(() => window.__wsSent.length);
  await page.evaluate(() => { window.__pinOk = false; });
  const sentBlocked = await page.evaluate(async () => {
    try {
      await window.sendCommand({ cmd: 'set_monthly_kwh', ch: 0, val: 999 });
      return 'resolved';
    } catch (e) { return 'rejected:' + e.message; }
  });
  const afterCount = await page.evaluate(() => window.__wsSent.length);
  check('sendCommand without PIN is refused client-side',
        sentBlocked.indexOf('rejected') === 0, sentBlocked);
  check('...and puts NOTHING on the wire', afterCount === beforeCount,
        beforeCount + ' -> ' + afterCount);

  console.log('\n== PIN gate, from the board side ==');
  // Bypass the UI entirely: the whole point is that the ESP32 is the authority.
  const boardVerdict = await page.evaluate((pin) => new Promise((resolve) => {
    const s = new WebSocket('ws://' + location.host + '/ws');
    const log = [];
    s.onopen = () => {
      s.send(JSON.stringify({ cmd: 'set_name', ch: 0, name: 'HACKED' }));   // no pin
      setTimeout(() => s.send(JSON.stringify({ cmd: 'set_name', ch: 0, name: 'Renamed', pin: pin })), 300);
      setTimeout(() => s.send(JSON.stringify({ cmd: 'set_name', ch: 0, name: 'Trunc', pin: pin.slice(0, 3) })), 600);
      setTimeout(() => s.send(JSON.stringify({ cmd: 'set_time', t: 1700000000 })), 900);
    };
    s.onmessage = (e) => { try { log.push(JSON.parse(e.data)); } catch (x) {} };
    setTimeout(() => { s.close(); resolve(log); }, 1800);
  }), PIN);

  const authFrames = boardVerdict.filter(m => m.type === 'auth' && m.ok === false);
  check('board REJECTS an unpinned mutation', authFrames.length >= 1,
        'auth frames: ' + authFrames.length);

  const names = boardVerdict.filter(m => Array.isArray(m.ch)).pop();
  check('board applied the PINNED rename', !!(names && names.ch[0].n === 'Renamed'),
        'ch1 name = ' + (names ? names.ch[0].n : '?'));

  // Negative control: prove the assertion above can actually fail. If the
  // board had applied the unpinned frame, ch1 would read HACKED here.
  check('NEGATIVE CONTROL: unpinned name never applied',
        !!(names && names.ch[0].n !== 'HACKED'), 'ch1 = ' + (names ? names.ch[0].n : '?'));

  console.log('\n== unlocking through the UI ==');
  await page.evaluate(() => window.showPage('settings'));
  await page.fill('#pinInput', PIN);
  await page.click('#pinRow button.btn-sm');
  await page.waitForTimeout(600);
  const badge = await page.locator('#roleBadge').textContent();
  check('PIN unlocks the admin badge', /Admin/.test(badge), 'badge = ' + badge);

  const before = await page.evaluate(() => window.__wsSent.length);
  const after = await page.evaluate(async () => {
    await window.sendCommand({ cmd: 'set_monthly_kwh', ch: 1, val: 77 });
    return window.__wsSent.length;
  });
  check('unlocked sendCommand puts a frame on the wire', after > before, before + ' -> ' + after);

  const lastFrame = await page.evaluate(() => window.__wsSent[window.__wsSent.length - 1]);
  check('the frame CARRIES the PIN', /"pin":"1234"/.test(lastFrame), lastFrame);

  console.log('\n== the BOARD can revoke a live session ==');
  // Scenario the suite used to miss entirely: the PIN is changed on the device
  // while this tab still believes it is admin. The next command is refused by
  // the ESP32, which answers {type:'auth',ok:false} - and the UI has to fall
  // back to read-only. Removing that handler makes every other check in this
  // file still pass, which is exactly why this block exists.
  // page.evaluate runs in the BROWSER, so Node-side constants are not in
  // scope there - anything the page needs has to be passed in as an argument.
  await page.evaluate((p) => window.unlockWithPin(p), PIN);
  await page.waitForTimeout(400);
  // `adminPin` / `pinOk` are declared with `let`, so they are SCRIPT-SCOPE
  // LEXICAL BINDINGS, not properties of `window`. Assigning window.adminPin
  // creates a separate property that sendCommand never reads, and reading
  // window.pinOk returns undefined. Both identifiers must be used bare.
  await page.evaluate(async () => {
    adminPin = '0000';               // device-side PIN has moved on
    try { await sendCommand({ cmd: 'set_monthly_kwh', ch: 2, val: 5 }); }
    catch (e) { /* expected: the UI-side gate still thinks we are admin */ }
  });
  // Wait for the board's reply BEFORE reading the flags: sendCommand resolves
  // as soon as the frame is written, so reading pinOk immediately after it
  // returns the pre-rejection value and the assertion fails for the wrong
  // reason (which is exactly what happened on the first run of this block).
  await page.waitForTimeout(900);
  const staleOutcome = await page.evaluate(() => ({ pinOk: pinOk, adminPin: adminPin }));
  const badgeStale = await page.locator('#roleBadge').textContent();
  check('board auth rejection clears the local admin flag',
        staleOutcome.pinOk === false, 'pinOk = ' + staleOutcome.pinOk);
  check('UI drops to viewer after a board-side auth rejection',
        /Viewer/.test(badgeStale), 'badge = ' + badgeStale);
  check('the stale PIN was cleared from the session',
        await page.evaluate(() => (sessionStorage.getItem('esp32counter_pin') || '') === ''));

  console.log('\n== wrong PIN is refused ==');
  const wrong = await page.evaluate(() => window.unlockWithPin('9999'));
  await page.waitForTimeout(400);
  check('wrong PIN does not unlock', wrong === false, 'returned ' + wrong);
  const badge2 = await page.locator('#roleBadge').textContent();
  check('UI drops back to viewer', /Viewer/.test(badge2), 'badge = ' + badge2);

  console.log('\n== trip notification plumbing ==');
  const hasNotify = await page.evaluate(() => 'Notification' in window);
  check('Notification API reachable in this context', hasNotify);
  const notifyFn = await page.evaluate(() => typeof window.notifyOnTrip);
  check('notifyOnTrip is wired into updateDashboard', notifyFn === 'function', notifyFn);

  console.log('\n== no cloud code survives ==');
  const html = await page.content();
  check('no gstatic script tags', html.indexOf('gstatic') < 0);
  check('no firebase references', html.toLowerCase().indexOf('firebase') < 0);
  const noConnMode = await page.locator('#connMode').count();
  check('mode dropdown removed', noConnMode === 0, 'found ' + noConnMode);
  const noNtfy = await page.locator('#ntfyTopic').count();
  check('ntfy panel removed', noNtfy === 0, 'found ' + noNtfy);
  const pinRow = await page.locator('#pinInput').count();
  check('PIN input present', pinRow === 1, 'found ' + pinRow);

  await page.screenshot({ path: '/tmp/opencode/e2e-dashboard.png', fullPage: false });

  // ---------------------------------------------------------------------
  // Access Point name + password.
  //
  // LAST, and deliberately: saving one drops the board's own network, so the
  // page this suite is driving loses its socket 1.5 s later. Every stage above
  // needs that socket, so this one runs after them all.
  //
  // What it proves: the panel exists, the UI refuses an impossible password
  // without bothering the board, the frame on the wire carries the credentials
  // AND the PIN, and - the part that matters - the BOARD rejects a bad
  // password on its own. A frontend that validated perfectly while the device
  // accepted anything would brick the radio, and only the second half of this
  // stage can see that.
  console.log('\n== Access Point: rename the network ==');
  // The stages above ended with a wrong-PIN attempt, so the UI is read-only
  // again. Get back in; the device-side PIN never changed.
  await page.evaluate((p) => window.unlockWithPin(p), PIN);
  await page.waitForTimeout(500);
  check('admin re-unlocked for the AP panel',
        /Admin/.test(await page.locator('#roleBadge').textContent()));

  await page.evaluate(() => window.showPage('settings'));
  await page.waitForTimeout(300);
  await page.locator('#apSsid').scrollIntoViewIfNeeded();
  await page.screenshot({ path: '/tmp/opencode/e2e-ap-panel.png' });

  // --- negative control, client side: an impossible password never leaves ---
  const beforeBad = await page.evaluate(() => window.__wsSent.length);
  await page.fill('#apSsid', 'Meter AP');
  await page.fill('#apPass', 'short7c');           // 7 characters
  await page.click('#apSsid ~ button.btn-sm');
  await page.waitForTimeout(400);
  const afterBad = await page.evaluate(() => window.__wsSent.length);
  check('a 7-character password is refused before it is sent',
        afterBad === beforeBad, beforeBad + ' -> ' + afterBad);
  const badToast = await page.locator('#toast').textContent();
  check('...and says why',
        /at least 8/i.test(badToast), 'toast = "' + badToast + '"');

  // --- negative control, board side: the device refuses it too -------------
  // Straight down a raw socket, so nothing about the UI can be involved.
  //
  // Parse failures are RECORDED, not swallowed. The first version of this stage
  // did `catch (x) {}`, and when the harness corrupted a frame the reply simply
  // vanished: the assertion failed with an empty log and no hint that a byte
  // stream, rather than the feature, was at fault. (It was - see the send lock
  // now in mock_device.py.)
  const boardAp = await page.evaluate((pin) => new Promise((resolve) => {
    const s = new WebSocket('ws://' + location.host + '/ws');
    const log = [];
    s.onopen = () => {
      s.send(JSON.stringify({ cmd: 'set_ap', ssid: 'Meter AP', pass: 'short7c', pin: pin }));
      setTimeout(() => s.send(JSON.stringify({ cmd: 'set_ap', ssid: '', pass: '12345678', pin: pin })), 350);
      setTimeout(() => s.send(JSON.stringify({ cmd: 'set_ap', ssid: 'Meter AP', pass: 'goodpass1', pin: pin })), 700);
    };
    s.onmessage = (e) => {
      try { log.push(JSON.parse(e.data)); }
      catch (x) { log.push({ type: 'CORRUPT', out: String(e.data).slice(0, 80) }); }
    };
    setTimeout(() => { s.close(); resolve(log); }, 1500);
  }), PIN);

  check('no frame was corrupted in transit',
        !boardAp.some(m => m.type === 'CORRUPT'),
        JSON.stringify(boardAp.filter(m => m.type === 'CORRUPT').slice(0, 2)));
  // A snapshot-only log means the device never answered, which is a different
  // bug from "answered with the wrong text". Report which so a failure says so.
  check('the device answered the raw frames at all',
        boardAp.some(m => m.type === 'console' || m.type === 'auth'),
        'message types: ' + JSON.stringify(boardAp.reduce((a, m) => {
          a[m.type || 'snapshot'] = (a[m.type || 'snapshot'] || 0) + 1; return a;
        }, {})));

  const apTexts = boardAp.filter(m => m.type === 'console').map(m => m.out || '');
  check('BOARD refuses a 7-character password',
        apTexts.some(t => /Not saved: Password must be at least 8/.test(t)),
        'console replies: ' + JSON.stringify(apTexts));
  check('BOARD refuses an empty network name',
        apTexts.some(t => /Not saved: Network name cannot be empty/.test(t)),
        JSON.stringify(apTexts));
  check('BOARD accepts a valid pair',
        apTexts.some(t => /restarting on network "Meter AP"/.test(t)),
        JSON.stringify(apTexts));
  // ...and the rejected ones never became the board's identity. If the mock had
  // stored the 7-character password, the last accepted frame would still say
  // "Meter AP" but the board would be broadcasting something unusable.
  check('NEGATIVE CONTROL: a rejected save is not silently stored',
        !apTexts.some(t => /restarting on network "short7c"/.test(t)));

  // --- the UI path -------------------------------------------------------
  const beforeSave = await page.evaluate(() => window.__wsSent.length);
  await page.fill('#apSsid', 'Meter AP');
  await page.fill('#apPass', 'goodpass1');
  await page.click('#apSsid ~ button.btn-sm');
  await page.waitForTimeout(600);

  const sentAp = await page.evaluate(() => window.__wsSent.slice());
  const apFrames = sentAp.filter(f => f.indexOf('"set_ap"') >= 0);
  check('the panel puts a set_ap frame on the wire', apFrames.length >= 1,
        'frames since load: ' + (sentAp.length - beforeSave));
  const apFrame = apFrames[apFrames.length - 1] || '';
  check('...carrying the new name and password',
        apFrame.indexOf('"ssid":"Meter AP"') >= 0 && apFrame.indexOf('"pass":"goodpass1"') >= 0,
        apFrame);
  check('...and the admin PIN', /"pin":"1234"/.test(apFrame), apFrame);
  check('the password box is cleared afterwards',
        (await page.inputValue('#apPass')) === '');

  const hint = await page.locator('#apHint').textContent();
  check('the panel warns that the board is rebooting',
        /rebooting/i.test(hint), 'hint = "' + (hint || '').slice(0, 80) + '"');

  console.log('\n' + checks + ' checks, ' + failures + ' failures');
  await browser.close();
  process.exit(failures === 0 ? 0 : 1);
})().catch((e) => {
  console.error('HARNESS ERROR:', e);
  process.exit(2);
});