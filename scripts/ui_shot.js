// Visual regression aid for the dashboard UI.
//
//   node scripts/ui_shot.js <baseUrl> <outDir> [width] [height]
//
// Boots a headless Chromium against the mock board, walks to the Settings page
// and screenshots the panels that are easy to get wrong by hand: the Admin PIN
// rows, the Access Point rows, the calibration rows and the console. It also
// dumps the measured geometry of every field row, because "looks aligned" is
// exactly the kind of claim that has to be measured, not eyeballed.
//
// The geometry dump is the point. A screenshot of a dark UI on a dark theme can
// look fine while a label column is 3px out of line; comparing the numbers
// catches that and screenshots do not.

const { chromium } = require(require('child_process')
  .execSync('npm root -g', { encoding: 'utf8' }).trim() + '/playwright');
const path = require('path');

const base = process.argv[2] || 'http://127.0.0.1:8099';
const outDir = process.argv[3] || '/tmp/opencode/ui';
const width = parseInt(process.argv[4] || '760', 10);
const height = parseInt(process.argv[5] || '900', 10);

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width, height } });
  await page.goto(base + '/', { waitUntil: 'networkidle' });
  await page.waitForSelector('#statusBar', { timeout: 10000 });

  // Collect geometry across every label/input row in the app.
  const measure = async () => page.evaluate(() => {
    const rows = [];
    document.querySelectorAll('.cal-row-single, .cal-param-row, .pin-row').forEach((row) => {
      const label = row.querySelector('label');
      const control = row.querySelector('input, select, span:not(.hint-inline):not(.az-chip)');
      const button = row.querySelector('button');
      const r = (el) => (el ? el.getBoundingClientRect() : null);
      const rl = r(label), rc = r(control), rb = r(button);
      rows.push({
        label: label ? label.textContent.trim().slice(0, 22) : '(none)',
        labelLeft: rl ? +rl.left.toFixed(1) : null,
        labelWidth: rl ? +rl.width.toFixed(1) : null,
        controlLeft: rc ? +rc.left.toFixed(1) : null,
        controlWidth: rc ? +rc.width.toFixed(1) : null,
        controlH: rc ? +rc.height.toFixed(1) : null,
        buttonLeft: rb ? +rb.left.toFixed(1) : null,
        buttonH: rb ? +rb.height.toFixed(1) : null,
      });
    });
    return rows;
  });

  // Unstyled inputs are the recurring bug in this panel: a bare <input> with no
  // class inherits the browser's white background and default font. Detect it
  // from the computed style instead of trusting the markup.
  const unthemed = async () => page.evaluate(() => {
    const bad = [];
    document.querySelectorAll('input, select').forEach((el) => {
      const cs = getComputedStyle(el);
      const parentCls = (el.parentElement && el.parentElement.className) || '';
      const themed = el.classList.contains('console-input') ||
        /form-group|cal-row-single|cal-param-row/.test(parentCls);
      const white = cs.backgroundColor === 'rgb(255, 255, 255)' ||
        cs.backgroundColor === 'rgb(239, 239, 239)' ||
        parseInt(cs.backgroundColor.match(/\d+/g || [0])[0], 10) > 200;
      if (white && !themed) {
        bad.push({
          id: el.id || '(no id)',
          parentCls: parentCls,
          bg: cs.backgroundColor,
          color: cs.color,
        });
      }
    });
    return bad;
  });

  // Below 900px the sidebar is display:none and the bottom bar takes over, so
  // "the settings button" is whichever of the two is actually visible.
  const goSettings = async () => {
    for (const sel of ['#sidebar [data-page="settings"]', '#mobileNav [data-page="settings"]']) {
      const el = await page.$(sel);
      if (el && await el.isVisible()) { await el.click(); return sel; }
    }
    throw new Error('no visible settings nav button');
  };
  const results = {};
  results.viewport = { width, height };
  results.navUsed = await goSettings();
  await page.waitForTimeout(400);

  results.badInputs = await unthemed();
  results.rows = await measure();

  // An element screenshot fails the whole run if the element is display:none
  // (every .admin-only panel is, while locked), so visibility is checked first.
  const shotEl = async (sel, name) => {
    const el = await page.$(sel);
    if (!el || !(await el.isVisible())) return false;
    await el.screenshot({ path: path.join(outDir, name) });
    return true;
  };

  await page.screenshot({ path: path.join(outDir, 'settings-full.png'), fullPage: true });
  results.shotPin = await shotEl('#pinRow', 'admin-pin.png');
  results.shotAp = await shotEl('.panel-box.admin-only', 'access-point.png');

  // Also check the admin (unlocked) state, which hides the guest hint.
  await page.evaluate(() => {
    const i = document.getElementById('pinInput');
    if (i) i.value = '1234';
  });
  await page.click('#pinRow button');
  await page.waitForTimeout(700);
  // The calibration rows only exist once the board has sent a channel list,
  // and they are collapsed by default, so open them: they are the densest
  // form in the app and the easiest place for a column to drift.
  await page.evaluate(() => document.querySelectorAll('.cal-collapse-header').forEach((h) => h.click()));
  await page.waitForTimeout(300);
  results.badInputsAdmin = await unthemed();
  results.rowsAdmin = await measure();
  results.shotApAdmin = await shotEl('.panel-box.admin-only', 'access-point.png');
  results.shotPinAdmin = await shotEl('#pinRow', 'admin-pin-unlocked.png');
  await page.screenshot({ path: path.join(outDir, 'settings-admin-full.png'), fullPage: true });

  // Column alignment is the actual complaint, so make it a pass/fail rather
  // than something to eyeball in the PNGs. The label, field and button of every
  // row in a panel must start at the same x within that panel, and every field
  // must share one height. A CSS rule written for the wrong input type breaks
  // the height; a label that grows past its column breaks the x.
  results.alignment = await page.evaluate(() => {
    const bad = [];
    const vis = (e) => e && e.getClientRects().length > 0;
    // Each group is a set of rows that should line up with each other: the rows
    // directly in a panel, and separately the rows of one collapsed calibration
    // body (they sit inside an indented container of their own).
    const groups = [];
    document.querySelectorAll('.panel-box').forEach((panel) => {
      const title = ((panel.querySelector('.panel-header h3') || {}).textContent || '?').trim();
      if (vis(panel)) groups.push({ title, rows: [...panel.children].filter((c) => c.classList.contains('cal-row-single') && vis(c)) });
      panel.querySelectorAll('.cal-collapse-body').forEach((body, i) => {
        if (vis(body)) groups.push({ title: `${title} > cal row ${i + 1}`, rows: [...body.children].filter((c) => c.classList.contains('cal-param-row') && vis(c)) });
      });
    });

    groups.filter((g) => g.rows.length > 1).forEach((g) => {
      const cols = { label: [], field: [], button: [] };
      const heights = [];
      // Only label+field rows form a column. A standalone action row (the
      // per-channel "Reset Cal" button) is a full-width row by design, and
      // comparing it against the rows above it would report a fault that is
      // not there.
      g.rows.forEach((r) => {
        const l = r.querySelector('label');
        const f = r.querySelector('input, select');
        const b = r.querySelector('button');
        if (!vis(l) || !vis(f)) return;
        cols.label.push(+l.getBoundingClientRect().left.toFixed(1));
        cols.field.push(+f.getBoundingClientRect().left.toFixed(1));
        heights.push(+f.getBoundingClientRect().height.toFixed(1));
        if (vis(b)) cols.button.push(+b.getBoundingClientRect().left.toFixed(1));
      });
      if (cols.field.length < 2) return;
      Object.entries(cols).forEach(([name, xs]) => {
        if (xs.length > 1 && new Set(xs).size > 1) {
          bad.push(`${g.title}: ${name} column at x = ${xs.join(', ')}`);
        }
      });
      if (heights.length > 1 && new Set(heights).size > 1) {
        bad.push(`${g.title}: field heights = ${heights.join(', ')}`);
      }
    });
    return bad;
  });

  console.log(JSON.stringify(results, null, 2));
  await browser.close();

  // A --check mode that always exits 0 is worse than no check at all: it looks
  // like verification. Unthemed inputs and drifted columns both fail here.
  const problems = [];
  if (results.badInputs.length) problems.push('browser-default inputs: ' + JSON.stringify(results.badInputs));
  if (results.badInputsAdmin && results.badInputsAdmin.length) {
    problems.push('browser-default inputs while admin: ' + JSON.stringify(results.badInputsAdmin));
  }
  if (results.alignment.length) problems.push('misaligned rows: ' + JSON.stringify(results.alignment));
  if (problems.length) {
    console.error('\nUI CHECK FAILED\n  ' + problems.join('\n  '));
    process.exit(1);
  }
  console.error('\nUI CHECK PASSED: every field is themed and every row lines up.');
})().catch((e) => {
  console.error('ui_shot failed:', e && e.stack ? e.stack : e);
  process.exit(1);
});