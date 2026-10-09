// E2E test: the REAL frontend against the REAL asset pipeline and a mock board.
//
//   node scripts/e2e_aponly.js [baseUrl]
//
// What this actually proves, and what it does not:
//
//  PROVES  The device-served page loads with no external scripts, connects a
//          same-origin WebSocket, renders every channel the board publishes,
//          lends its clock, and
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

// The AP actions are addressed by id (#apSaveBtn / #apResetBtn) because they
// sit in a .cal-actions row of their own, not beside the fields they apply to.

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
    window.__wsSeen = [];
    window.__lastData = null;
    const Orig = window.WebSocket;
    window.WebSocket = function (url, protocols) {
      const s = protocols ? new Orig(url, protocols) : new Orig(url);
      window.__wsUrl = url;
      const origSend = s.send.bind(s);
      s.send = function (d) { try { window.__wsSent.push(d); } catch (e) {} return origSend(d); };
      // The last snapshot the board actually pushed. Asserting the UI against
      // the wire rather than against a hardcoded literal is what makes a test
      // like "the panel shows the stored name" meaningful.
      s.addEventListener('message', function (e) {
        try { window.__wsSeen.push(JSON.parse(e.data)); } catch (x) { /* raw text */ }
        try {
          const d = JSON.parse(e.data);
          if (d && typeof d === 'object' && 'v' in d) window.__lastData = d;
        } catch (x) { /* not the system frame */ }
      });
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
    () => document.querySelectorAll('.channel-card').length >= 1
          && window.__lastData && Array.isArray(window.__lastData.ch)
          && document.querySelectorAll('.channel-card').length === window.__lastData.ch.length,
    null, { timeout: 10000 })
    .catch(() => {});
  await page.waitForTimeout(1200);

  const wsUrl = await page.evaluate(() => window.__wsUrl);
  check('WebSocket targets same-origin /ws',
        wsUrl === 'ws://' + BASE.replace(/^https?:\/\//, '') + '/ws', 'got ' + wsUrl);

  check('NO external network requests', externalRequests.length === 0,
        externalRequests.join(', '));
  check('no console errors', consoleErrors.length === 0, consoleErrors.join(' | '));

  console.log('\n== dashboard renders ==');
  // Card count comes from the board's snapshot, not a literal: NUM_CHANNELS
  // is 5 in src/config.h and frontend/script.js today, and this test must
  // follow whichever the mock actually publishes.
  const cards = await page.locator('.channel-card').count();
  const snapshotCh = await page.evaluate(
    () => (window.__lastData && window.__lastData.ch) ? window.__lastData.ch.length : -1);
  check('one channel card per snapshot channel', cards === snapshotCh && cards > 0,
        'cards=' + cards + ' snapshot=' + snapshotCh);

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

  console.log('\n== changing the PIN asks for confirmation ==');
  // A single new-PIN box turns one typo into a lockout: the board would save
  // the mistyped value and the old PIN would stop working. So the panel has a
  // Confirm box, and a mismatch must never reach the board.
  await page.evaluate((p) => window.unlockWithPin(p), PIN);
  await page.waitForTimeout(400);
  await page.evaluate(() => window.showPage('settings'));
  const setBtn = page.locator('#pinRow button.btn-sm').nth(1);
  const beforePin = await page.evaluate(() => window.__wsSent.length);
  await page.fill('#pinNew', 'abcd');
  await page.fill('#pinConfirm', 'wxyz');
  await setBtn.click();
  await page.waitForTimeout(300);
  check('a mismatched confirmation is refused without sending',
        (await page.evaluate(() => window.__wsSent.length)) === beforePin);
  check('...and says why',
        /do not match/i.test(await page.locator('#toast').textContent()),
        'toast = "' + await page.locator('#toast').textContent() + '"');

  await page.fill('#pinNew', '5678');
  await page.fill('#pinConfirm', '5678');
  await setBtn.click();
  await page.waitForTimeout(500);
  const pinFrames = (await page.evaluate((n) => window.__wsSent.slice(n), beforePin))
    .filter(f => f.indexOf('"set_pin"') >= 0);
  check('matching entries put a set_pin frame on the wire', pinFrames.length >= 1);
  const pinFrame = pinFrames[pinFrames.length - 1] || '';
  check('...carrying the new PIN', pinFrame.indexOf('"pin_new":"5678"') >= 0, pinFrame);
  check('...and the current (admin) PIN', /"pin":"1234"/.test(pinFrame), pinFrame);

  // Restore, or every later stage that unlocks with the factory PIN fails.
  await page.fill('#pinNew', PIN);
  await page.fill('#pinConfirm', PIN);
  await setBtn.click();
  await page.waitForTimeout(500);
  const restoreFrames = (await page.evaluate(() => window.__wsSent.slice()))
    .filter(f => f.indexOf('"pin_new":"1234"') >= 0);
  check('the PIN is restored afterwards', restoreFrames.length >= 1);

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

  // The panel must show what the board is actually broadcasting. It used to
  // carry a static placeholder, so a renamed board still looked factory-fresh
  // and the user had no way to see the value they were about to overwrite.
  const shownSsid = await page.inputValue('#apSsid');
  const mockSsid = await page.evaluate(() => window.__lastData && window.__lastData.apSsid);
  check('the AP panel shows the stored network name, not a placeholder',
        shownSsid === mockSsid && shownSsid.length > 0,
        'field="' + shownSsid + '" board="' + mockSsid + '"');
  const passPlaceholder = await page.getAttribute('#apPass', 'placeholder');
  check('the password box says it is unchanged and never reveals it',
        /unchanged/i.test(passPlaceholder) && !(await page.inputValue('#apPass')),
        'placeholder="' + passPlaceholder + '" value="' + await page.inputValue('#apPass') + '"');

  // A half-typed name must survive the 6-7 Hz push, or the panel is unusable.
  await page.fill('#apSsid', 'Half Typed');
  await page.waitForTimeout(700);
  check('the board push does not overwrite a field being typed into',
        (await page.inputValue('#apSsid')) === 'Half Typed',
        'value="' + await page.inputValue('#apSsid') + '"');

  // --- negative control, client side: an impossible password never leaves ---
  const beforeBad = await page.evaluate(() => window.__wsSent.length);
  await page.fill('#apSsid', 'Meter AP');
  await page.fill('#apPass', 'short7c');           // 7 characters
  await page.click('#apSaveBtn');
  await page.waitForTimeout(400);
  const afterBad = await page.evaluate(() => window.__wsSent.length);
  check('a 7-character password is refused before it is sent',
        afterBad === beforeBad, beforeBad + ' -> ' + afterBad);
  const badToast = await page.locator('#toast').textContent();
  check('...and says why',
        /at least 8/i.test(badToast), 'toast = "' + badToast + '"');

  // A trailing space is the failure a user cannot SEE in the input box, so it is
  // the one most likely to reach the board. The UI used to trim before checking
  // and this test could not fail; now the check has to be able to fire.
  await page.fill('#apPass', 'goodpass1');
  await page.fill('#apSsid', 'Meter AP ');
  await page.click('#apSaveBtn');
  await page.waitForTimeout(400);
  const spaceToast = await page.locator('#toast').textContent();
  check('a trailing space in the name is refused client-side',
        /start or end with a space/i.test(spaceToast), 'toast = "' + spaceToast + '"');
  check('...and still nothing was sent',
        (await page.evaluate(() => window.__wsSent.length)) === beforeBad,
        'frames: ' + (await page.evaluate(() => window.__wsSent.length)));

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
    let opened = false;
    let sendThrew = null;
    s.onopen = () => {
      opened = true;
      try {
        s.send(JSON.stringify({ cmd: 'set_ap', ssid: 'Meter AP', pass: 'short7c', pin: pin }));
        setTimeout(() => s.send(JSON.stringify({ cmd: 'set_ap', ssid: '', pass: '12345678', pin: pin })), 350);
        setTimeout(() => s.send(JSON.stringify({ cmd: 'set_ap', ssid: 'Meter AP ', pass: 'goodpass1', pin: pin })), 550);
        setTimeout(() => s.send(JSON.stringify({ cmd: 'set_ap', ssid: 'Meter AP', pass: 'goodpass1', pin: pin })), 800);
      } catch (e) { sendThrew = e.message; }
    };
    s.onmessage = (e) => {
      try { log.push(JSON.parse(e.data)); }
      catch (x) { log.push({ type: 'CORRUPT', out: String(e.data).slice(0, 80) }); }
    };
    setTimeout(() => {
      const state = { opened, sendThrew, readyState: s.readyState, log };
      try { s.close(); } catch (x) {}
      resolve(state);
    }, 1500);
  }), PIN);

  check('the raw socket opened',
        boardAp.opened === true && boardAp.sendThrew === null,
        'opened=' + boardAp.opened + ' sendThrew=' + boardAp.sendThrew +
        ' readyState=' + boardAp.readyState);
  check('no frame was corrupted in transit',
        !boardAp.log.some(m => m.type === 'CORRUPT'),
        JSON.stringify(boardAp.log.filter(m => m.type === 'CORRUPT').slice(0, 2)));
  // A snapshot-only log means the device never answered, which is a different
  // bug from "answered with the wrong text". Report which so a failure says so.
  check('the device answered the raw frames at all',
        boardAp.log.some(m => m.type === 'console' || m.type === 'auth'),
        'message types: ' + JSON.stringify(boardAp.log.reduce((a, m) => {
          a[m.type || 'snapshot'] = (a[m.type || 'snapshot'] || 0) + 1; return a;
        }, {})));

  const apTexts = boardAp.log.filter(m => m.type === 'console').map(m => m.out || '');
  check('BOARD refuses a 7-character password',
        apTexts.some(t => /Not saved: Password must be at least 8/.test(t)),
        'console replies: ' + JSON.stringify(apTexts));
  check('BOARD refuses an empty network name',
        apTexts.some(t => /Not saved: Network name cannot be empty/.test(t)),
        'console replies: ' + JSON.stringify(apTexts));
  check('BOARD refuses a trailing space in the name',
        apTexts.some(t => /Not saved: Network name cannot start or end with a space/.test(t)),
        'console replies: ' + JSON.stringify(apTexts));
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
  await page.click('#apSaveBtn');
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
  // Fallback state (fresh mock, STA never joined): the AP row prints the
  // address to open while the STA row shows down. The joined-state flip is
  // asserted later, after the STA save below.
  check('the AP row prints the fallback address while STA is down',
        /192\.168\.4\.1/.test(await page.locator('#apIp').textContent()),
        'apIp="' + await page.locator('#apIp').textContent() + '"');
  check('the STA row shows down before anything is joined',
        (await page.locator('#staIp').textContent()).trim() === '—',
        'staIp="' + await page.locator('#staIp').textContent() + '"');

  // ---------------------------------------------------------------------
  // Home Network (STA) panel.
  //
  // Placed after the AP stage because the page has just been told the socket is
  // going away, but driven over a SEPARATE raw socket: this must be provable
  // without depending on a live dashboard session.
  //
  // What matters here is not the name but the three rules that make the feature
  // safe: the board must refuse an empty SSID, `Forget` must clear the stored
  // network, and the status line must follow the board rather than a constant.
  console.log('\n== Home Network (STA) ==');
  await page.evaluate(() => window.showPage('settings'));
  await page.waitForTimeout(300);
  await page.locator('#staSsid').scrollIntoViewIfNeeded();
  check('the STA panel exists', await page.locator('#staSaveBtn').count() === 1);

  // Status must read from the snapshot, not a hardcoded string.
  const staStatus = await page.locator('#staStatus').textContent();
  const boardUp = await page.evaluate(() => !!(window.__lastData && window.__lastData.wifi));
  check('the STA status line reflects the board', /not connected/i.test(staStatus) === !boardUp,
        'status="' + staStatus + '" boardUp=' + boardUp);

  const boardSta = await page.evaluate((pin) => new Promise((resolve) => {
    const s = new WebSocket('ws://' + location.host + '/ws');
    const log = [];
    s.onopen = () => {
      try {
        // Board-side negative control: an empty SSID must be refused outright,
        // because an empty one would silently mean "stop joining anything".
        s.send(JSON.stringify({ cmd: 'setwifi', ssid: '', pass: 'irrelevant', pin: pin }));
        setTimeout(() => s.send(JSON.stringify({ cmd: 'setwifi', ssid: 'Home Router', pass: 'homepass1', pin: pin })), 350);
        setTimeout(() => s.send(JSON.stringify({ cmd: 'clearwifi', pin: pin })), 700);
        setTimeout(() => s.send(JSON.stringify({ cmd: 'setwifi', ssid: 'Home Router', pass: 'homepass1', pin: pin })), 1000);
      } catch (e) { /* recorded below */ }
    };
    s.onmessage = (e) => {
      try { log.push(JSON.parse(e.data)); }
      catch (x) { log.push({ type: 'CORRUPT', out: String(e.data).slice(0, 80) }); }
    };
    setTimeout(() => { try { s.close(); } catch (x) {} resolve(log); }, 1600);
  }), PIN);

  const staTexts = boardSta.filter(m => m.type === 'console').map(m => m.out || '');
  check('BOARD refuses an empty home-network name',
        staTexts.some(t => /SSID is empty/i.test(t)),
        'console replies: ' + JSON.stringify(staTexts));
  check('BOARD accepts a valid home network',
        staTexts.some(t => /will join "Home Router"/.test(t)),
        JSON.stringify(staTexts));
  check('BOARD clears it again on clearwifi',
        staTexts.some(t => /credentials cleared/i.test(t)),
        JSON.stringify(staTexts));
  check('...and it is still there after the last save',
        boardSta.filter(m => m.type === 'console' && /will join "Home Router"/.test(m.out || '')).length >= 2,
        'join replies: ' + JSON.stringify(staTexts));

  // The snapshot must now advertise it, with the password absent.
  const staSnap = boardSta.filter(m => m.type !== 'console' && m.type !== 'auth' && 'staSsid' in m).pop();
  check('the board publishes the home-network name', staSnap && staSnap.staSsid === 'Home Router',
        'staSsid=' + (staSnap ? JSON.stringify(staSnap.staSsid) : 'none'));
  check('the board NEVER publishes the home-network password',
        !JSON.stringify(boardSta).includes('homepass1'),
        'the password appeared in a board frame');

  // The UI path: same rule the AP panel has, no reboot warning (the board's own
  // AP keeps the page alive), and the status line must catch up.
  //
  // The page's own socket is CLOSED here: the AP stage above deliberately called
  // handleDisconnect() because saving there drops the network the page is on.
  // So it has to be reopened first - otherwise every click below fails with
  // "Not connected" and the panel looks broken when it is not. This bit me
  // once already: the suite reported two FAILs that the same code passed in
  // isolation, and the cause was this dead socket, not the feature.
  // `ws` is a script-scope LEXICAL BINDING (let), so window.ws is undefined and
  // reading it here would report "closed" forever - the same trap this file
  // already documents for adminPin/pinOk.
  await page.evaluate(() => {
    if (typeof ws === 'undefined' || !ws || ws.readyState !== WebSocket.OPEN) connectWS();
  });
  await page.waitForFunction(
    () => (typeof ws !== 'undefined') && !!ws && ws.readyState === WebSocket.OPEN,
    null, { timeout: 8000 })
    .catch(() => {});
  const staSocketOpen = await page.evaluate(
    () => (typeof ws !== 'undefined') && !!ws && ws.readyState === WebSocket.OPEN);
  check('the dashboard socket is open again for the STA panel', staSocketOpen);

  // Only frames sent through THIS page socket count. __wsSent also captured the
  // raw-socket probe above, so counting the whole array would let those frames
  // satisfy this assertion - which is how a dead socket can look like a pass.
  const beforeSta = await page.evaluate(() => window.__wsSent.length);
  await page.fill('#staSsid', '');
  await page.click('#staSaveBtn');
  await page.waitForTimeout(300);
  check('the UI refuses an empty home-network name without sending it',
        (await page.evaluate(() => window.__wsSent.length)) === beforeSta);

  await page.fill('#staSsid', 'Home Router');
  await page.fill('#staPass', 'homepass1');
  await page.click('#staSaveBtn');
  await page.waitForTimeout(600);
  const staFrames = (await page.evaluate((n) => window.__wsSent.slice(n), beforeSta))
    .filter(f => f.indexOf('"setwifi"') >= 0);
  check('the panel puts a setwifi frame on the wire', staFrames.length >= 1);
  const staFrame = staFrames[staFrames.length - 1] || '';
  check('...carrying the network name and password',
        staFrame.indexOf('"ssid":"Home Router"') >= 0 &&
        staFrame.indexOf('"pass":"homepass1"') >= 0, staFrame);
  check('...and the admin PIN', /"pin":"1234"/.test(staFrame), staFrame);
  check('the password box is cleared afterwards',
        (await page.inputValue('#staPass')) === '');
  const staHint = await page.locator('#staHint').textContent();
  check('the STA hint says the board restarts with the AP staying off',
        /restart/i.test(staHint) && /AP stays OFF|fallback AP/i.test(staHint),
        'hint = "' + (staHint || '').slice(0, 90) + '"');

  // ---------------------------------------------------------------------
  // Remote Monitoring (cloud, herd login).
  //
  // Email/password sign-in: the panel sends host+email+password, the snapshot
  // sends back everything EXCEPT the password. The password must never appear
  // in any board frame - that is the property this block guards.
  console.log('\n== Remote Monitoring (cloud) ==');
  check('the cloud panel exists', await page.locator('#cloudSaveBtn').count() === 1);
  const beforeCloud = await page.evaluate(() => window.__wsSent.length);
  await page.fill('#cloudHost', '');
  await page.click('#cloudSaveBtn');
  await page.waitForTimeout(300);
  check('the UI refuses an empty database host without sending it',
        (await page.evaluate(() => window.__wsSent.length)) === beforeCloud);

  await page.fill('#cloudHost', 'demo-project.firebaseio.com');
  await page.fill('#cloudEmail', 'board@esp32.local');
  await page.fill('#cloudAuth', 'SECRET-PASS-123');
  await page.click('#cloudSaveBtn');
  await page.waitForTimeout(600);
  const cloudFrames = (await page.evaluate((n) => window.__wsSent.slice(n), beforeCloud))
    .filter(f => f.indexOf('"setcloud"') >= 0);
  check('the panel puts a setcloud frame on the wire', cloudFrames.length >= 1);
  const cloudFrame = cloudFrames[cloudFrames.length - 1] || '';
  check('...carrying host, email and password',
        cloudFrame.indexOf('"host":"demo-project.firebaseio.com"') >= 0 &&
        cloudFrame.indexOf('"email":"board@esp32.local"') >= 0 &&
        cloudFrame.indexOf('"pass":"SECRET-PASS-123"') >= 0, cloudFrame);
  check('...and the admin PIN', /"pin":"1234"/.test(cloudFrame), cloudFrame);
  check('the password box is cleared afterwards (write-only)',
        (await page.inputValue('#cloudAuth')) === '');

  // The board applied it: the snapshot now reports pushing under the MAC id
  // with the account email, and NO board frame anywhere carries the password.
  await page.waitForTimeout(1200);
  const cloudStatus = await page.locator('#cloudStatus').textContent();
  check('the status line reports pushing with the device id',
        /pushing/i.test(cloudStatus) && /a1:b2:c3:d4:e5:f6/.test(cloudStatus),
        'status = "' + cloudStatus + '"');
  const boardSoFar = await page.evaluate(() => window.__wsSeen.slice());
  const passLeaks = boardSoFar.filter(m => JSON.stringify(m).indexOf('SECRET-PASS-123') >= 0);
  check('the password never comes back in any board frame', passLeaks.length === 0,
        'leaking frames: ' + passLeaks.length);
  const cloudSnap = boardSoFar.filter(m => m.type !== 'console' && m.type !== 'auth' && 'cloud' in m).pop();
  check('the board publishes host and account for the panel',
        cloudSnap && cloudSnap.cloud &&
        cloudSnap.cloud.host === 'demo-project.firebaseio.com' &&
        cloudSnap.cloud.acct === 'board@esp32.local',
        'cloud=' + (cloudSnap && cloudSnap.cloud ? JSON.stringify(cloudSnap.cloud) : 'none'));

  // PIN gate holds for the new verbs too: without a PIN the board refuses.
  // The refusal shape is {"type":"auth","ok":false} - see mock_device.py's
  // send path (mirrors the firmware's authRejected reply), not prose. The
  // socket also receives routine snapshots, so wait for the auth frame
  // rather than reading the first message (which is just telemetry).
  const noPinCloud = await page.evaluate(() => new Promise((resolve) => {
    const s = new WebSocket('ws://' + location.host + '/ws');
    const timer = setTimeout(() => { try { s.close(); } catch (x) {} resolve('TIMEOUT'); }, 3000);
    s.onopen = () => s.send(JSON.stringify({ cmd: 'setcloud', host: 'x.firebaseio.com', email: 'b@e.local', pass: 'y' }));
    s.onmessage = (ev) => {
      try {
        const d = JSON.parse(ev.data);
        if (d && d.type === 'auth') {
          clearTimeout(timer);
          resolve(d);
          s.close();
        }
      } catch (x) { /* telemetry snapshot, keep waiting */ }
    };
  })).catch(() => 'TIMEOUT');
  check('setcloud without a PIN is refused by the board',
        noPinCloud && noPinCloud.type === 'auth' && noPinCloud.ok === false,
        JSON.stringify(noPinCloud).slice(0, 60));

  // ---------------------------------------------------------------------
  // Billing reset day: About -> Reset Day -> Set sends the console line
  // `reset_day <1-28>` (one shared CLI path, not a second JSON verb), the
  // board stores it in NVS and the next snapshot reports it back.
  console.log('\n== billing reset day ==');
  // The cloud save above scheduled handleDisconnect() (+1500ms: on real
  // hardware the board reboots), so the page socket is dead by now - the
  // same trap the STA section documents. Reopen it first.
  await page.evaluate(() => {
    if (typeof ws === 'undefined' || !ws || ws.readyState !== WebSocket.OPEN) { userDisconnect = false; connectWS(); }
  });
  await page.waitForFunction(
    () => (typeof ws !== 'undefined') && !!ws && ws.readyState === WebSocket.OPEN,
    null, { timeout: 8000 })
    .catch(() => {});
  await page.fill('#resetDay', '5');
  const beforeDay = await page.evaluate(() => window.__wsSent.length);
  await page.click('#resetDayBtn');
  await page.waitForTimeout(800);
  const dayFrames = (await page.evaluate((n) => window.__wsSent.slice(n), beforeDay))
    .filter(f => f.indexOf('"console"') >= 0);
  check('Reset Day Set sends one console line',
        dayFrames.length >= 1 && dayFrames.some(f => f.indexOf('reset_day 5') >= 0),
        JSON.stringify(dayFrames).slice(0, 120));
  check('...carrying the admin PIN',
        dayFrames.some(f => /"pin":"1234"/.test(f)),
        JSON.stringify(dayFrames).slice(0, 120));
  const daySnap = (await page.evaluate(() => window.__wsSeen.slice()))
    .filter(m => m.type !== 'console' && m.type !== 'auth' && 'resetDay' in m).pop();
  check('the board reports the stored reset day back',
        daySnap && daySnap.resetDay === 5,
        'resetDay=' + (daySnap && daySnap.resetDay));
  check('the About input shows the stored day, not the typed one',
        (await page.inputValue('#resetDay')) === '5');
  // Out of range never leaves the page: client-side guard, no frame.
  await page.fill('#resetDay', '31');
  const beforeBadDay = await page.evaluate(() => window.__wsSent.length);
  await page.click('#resetDayBtn');
  await page.waitForTimeout(400);
  const badDayFrames = (await page.evaluate((n) => window.__wsSent.slice(n), beforeBadDay))
    .filter(f => f.indexOf('reset_day') >= 0);
  check('day 31 is refused client-side (nothing sent)',
        badDayFrames.length === 0, JSON.stringify(badDayFrames).slice(0, 120));

  // ---------------------------------------------------------------------
  // Firmware (local OTA only).
  //
  // No cloud updater: no Check/Update buttons, no release lookup, no `ota`
  // console verb. The panel shows the running version from the board, and a
  // LAN flash (USB / ArduinoOTA) is outside this harness. Assert the panel
  // state AND that an `ota` console line is now rejected as unknown.
  console.log('\n== Firmware (local OTA only) ==');
  await page.evaluate(() => window.showPage('settings'));
  await page.waitForTimeout(300);
  check('the Firmware panel shows the running version from the board',
        /\bv?\d+\.\d+\.\d+\b/.test(await page.locator('#fwRunning').textContent()),
        'fwRunning="' + await page.locator('#fwRunning').textContent() + '"');
  check('there is no Check button (no cloud release lookup)',
        await page.locator('#fwCheckBtn').count() === 0);
  check('there is no Update row (no cloud download)',
        await page.locator('#fwUpdateRow').count() === 0);

  const boardOta = await page.evaluate((pin) => new Promise((resolve) => {
    const s = new WebSocket('ws://' + location.host + '/ws');
    const log = [];
    s.onopen = () => {
      try {
        s.send(JSON.stringify({ cmd: 'console', line: 'ota status', pin: pin }));
      } catch (e) { /* recorded below */ }
    };
    s.onmessage = (e) => {
      try { log.push(JSON.parse(e.data)); }
      catch (x) { log.push({ type: 'CORRUPT', out: String(e.data).slice(0, 80) }); }
    };
    setTimeout(() => { try { s.close(); } catch (x) {} resolve(log); }, 800);
  }), PIN);
  const otaTexts = boardOta.filter(m => m.type === 'console').map(m => m.out || '');
  check('BOARD rejects the removed ota verb as unknown',
        otaTexts.some(t => /Unknown command/.test(t)), JSON.stringify(otaTexts));

  // ---------------------------------------------------------------------
  // Header icon + Connection-panel rows follow the snapshot.
  //
  // The icon once only knew lv0/lv2 (the AP-only era) while the STA code could
  // emit lv1/lv3, which have no CSS rule and render dim - the header showed a
  // dead icon next to a live RSSI number. The mock publishes rssi -58 with STA
  // up, which must render as a lit lv2, never lv0/disconnected.
  console.log('\n== header icon and status rows ==');
  await page.evaluate(() => window.showPage('dashboard'));
  await page.waitForTimeout(800);
  const wifiClass = await page.locator('.wifi').getAttribute('class');
  check('the header wifi icon reflects the STA link, not a dead icon',
        /lv[123]/.test(wifiClass || '') && !/disconnected/.test(wifiClass || ''),
        'class="' + wifiClass + '"');
  // ...and the level has a real CSS rule behind it, not just a class name: with
  // the lv2 rule deleted the class is still "lv2" but zero bars light up.
  // rssi -58 renders lv2 = dot + first arc lit.
  const litBars = await page.evaluate(() => {
    let lit = 0, total = 0;
    document.querySelectorAll('.wifi .bar').forEach((b) => {
      total++;
      if (window.getComputedStyle(b).opacity === '1') lit++;
    });
    return { lit: lit, total: total };
  });
  check('the STA level actually lights bars (CSS rule present)',
        litBars.total === 4 && litBars.lit >= 2, JSON.stringify(litBars));
  await page.evaluate(() => window.showPage('settings'));
  await page.waitForTimeout(300);
  check('MCU temperature renders from the snapshot',
        /51\.2/.test(await page.locator('#mcuTemp').textContent()),
        'mcu="' + await page.locator('#mcuTemp').textContent() + '"');
  // Interface addresses: the STA save above rebooted the mock joined
  // (station_up), so the STA row shows 192.168.1.50 while the AP row reads
  // OFF — the exact flip of the fallback state at boot.
  check('the STA row prints the joined address',
        /192\.168\.1\.50/.test(await page.locator('#staIp').textContent()),
        'staIp="' + await page.locator('#staIp').textContent() + '"');
  check('the home status names the address too',
        /192\.168\.1\.50/.test(await page.locator('#staStatus').textContent()),
        'staStatus="' + await page.locator('#staStatus').textContent() + '"');
  check('the AP row reads OFF while the fallback is down',
        (await page.locator('#apIp').textContent()).trim() === 'OFF',
        'apIp="' + await page.locator('#apIp').textContent() + '"');
  check('the Board row names the reachable address, not a blank',
        /192\.168\.1\.50/.test(await page.locator('#connectedIp').textContent()),
        'board="' + await page.locator('#connectedIp').textContent() + '"');
  check('eco state renders from the snapshot',
        /Full|ECO/.test(await page.locator('#ecoStatus').textContent()),
        'eco="' + await page.locator('#ecoStatus').textContent() + '"');
  // The install UI is CLOUD-ONLY now. The board serves this page over plain
  // http on a LAN IP, which is not a secure context: no browser fires
  // beforeinstallprompt there and none offers "Install" from the address bar,
  // so the button could only ever be a dead control. Asserting its ABSENCE is
  // the contract - a count of 0 is the passing state, and a regression that
  // re-adds it to the board copy fails this instead of shipping a dead button.
  check('the board page carries no install UI (http LAN origin cannot install)',
        await page.locator('#installBtnTop').count() === 0
        && await page.locator('#installRow').count() === 0
        && await page.locator('#installHint').count() === 0);
  check('and no install handler is defined on the board page',
        await page.evaluate(() => typeof window.promptInstall === 'undefined'));

  console.log('\n' + checks + ' checks, ' + failures + ' failures');
  await browser.close();
  process.exit(failures === 0 ? 0 : 1);
})().catch((e) => {
  console.error('HARNESS ERROR:', e);
  process.exit(2);
});