// README screenshots for the dashboard UI.
//
//   node scripts/ui_capture.js <baseUrl> <outDir>
//
// Boots headless Chromium against the mock board (scripts/mock_device.py),
// screenshots the Dashboard and the unlocked Settings page. One-off capture
// tool, not a gate: ui_shot.js owns the pass/fail geometry checks. Re-run
// after any visual redesign and copy the results into doc/img/.

const { chromium } = require(require('child_process')
  .execSync('npm root -g', { encoding: 'utf8' }).trim() + '/playwright');
const path = require('path');

const base = process.argv[2] || 'http://127.0.0.1:8099';
const outDir = process.argv[3] || '/tmp/opencode/ui-capture';

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
  await page.goto(base + '/', { waitUntil: 'networkidle' });
  await page.waitForSelector('#statusBar', { timeout: 10000 });
  // Let a few 7 Hz frames land so cards show values, not placeholders.
  await page.waitForTimeout(1500);

  const go = async (name) => {
    for (const sel of [`#sidebar [data-page="${name}"]`, `#mobileNav [data-page="${name}"]`]) {
      const el = await page.$(sel);
      if (el && await el.isVisible()) { await el.click(); return; }
    }
    throw new Error('no visible nav button for ' + name);
  };

  await go('dashboard');
  await page.waitForTimeout(500);
  await page.screenshot({ path: path.join(outDir, 'ui-dashboard.png') });

  await go('settings');
  await page.waitForTimeout(400);
  await page.evaluate(() => { document.getElementById('pinInput').value = '1234'; });
  await page.click('#pinRow button');
  await page.waitForTimeout(700);
  await page.screenshot({ path: path.join(outDir, 'ui-settings.png'), fullPage: true });

  await browser.close();
  console.log('captured to ' + outDir);
})().catch((e) => {
  console.error('ui_capture failed:', e && e.stack ? e.stack : e);
  process.exit(1);
});
