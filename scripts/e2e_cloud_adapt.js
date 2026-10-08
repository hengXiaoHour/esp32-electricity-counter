// Functional test for the cloud transport adapter (cloud-viewer/cloud.js).
//
// The cloud viewer renders the SAME dashboard as the board with the SAME
// script.js, so cl_adapt() is the only place where the cloud payload's names
// are translated back into the local snapshot's names. It drifted once: the
// calibration block (voltage/current cal, RMS samples, auto-zero state, noise
// floor, LPF alpha) was neither pushed nor mapped, so Settings > Calibration
// came up blank in the cloud viewer while the board held every value.
//
// check_docs.py greps for the key NAMES, which cannot catch a mapping that
// uses the right key wrongly (e.g. `typeof latest.azBatches === 'string'`).
// This runs the real function on a real-shaped payload and asserts the values,
// so both the missing key and the mistyped one fail.
//
// Usage:  node scripts/e2e_cloud_adapt.js
// Exit 0 = the cloud payload maps onto the local snapshot shape.
'use strict';

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const CLOUD_JS = path.join(ROOT, 'cloud-viewer', 'cloud.js');

let checks = 0;
let failures = 0;

function ok(name, cond, detail) {
  checks++;
  if (cond) {
    console.log(`  ok   ${name}`);
  } else {
    failures++;
    console.log(`  FAIL ${name}${detail !== undefined ? `  [${detail}]` : ''}`);
  }
}

function eq(name, got, want) {
  const a = JSON.stringify(got);
  const b = JSON.stringify(want);
  ok(name, a === b, `got ${a} want ${b}`);
}

// --- load cl_adapt without running the Firebase boot ----------------------
// cloud.js ends with an IIFE (cl_boot) that touches firebase/document. Cut the
// file at that IIFE and return the two pure functions instead.
function loadAdapter() {
  const src = fs.readFileSync(CLOUD_JS, 'utf8');
  const cut = src.indexOf('(function cl_boot()');
  if (cut < 0) throw new Error('cl_boot IIFE not found - the file layout changed');
  const head = src.slice(0, cut);
  if (!/function cl_adapt\s*\(/.test(head)) throw new Error('cl_adapt not found before cl_boot');
  // eslint-disable-next-line no-new-func
  return new Function(`${head}\nreturn { cl_adapt: cl_adapt, cl_toArray: cl_toArray };`)();
}

// --- fixtures -------------------------------------------------------------
// One board, two serializers. LAN (system_json.cpp) names, and the cloud
// payload's renamed keys. The values are deliberately distinctive so a
// field-to-field mix-up cannot pass.
const LAN_CAL = {
  voltageCalibration: 231.4,
  currentCalibration: [100.0, 88.5, 112.0, 97.3, 105.6],
  rmsSamples: 1000,
  azBatches: 20,
  noiseFloor: [0.012, 0.034, 0.056, 0.078, 0.091],
  azActive: true,
  azChannel: 2,
  azProgress: 7,
  azQueue: [3, 4],
  lpfAlpha: [1.0, 0.5, 0.25, 0.12, 0.06],
};

// Cloud payload: same values, plus the renames the firmware actually does
// (mcuTemp -> mcu, firmwareVersion -> fw, ota -> otaRun, chip kept) and the
// keyed-object form RTDB hands back for an array.
const CLOUD_LATEST = {
  dev: 'EC64C998B0EC',
  epoch: 1760000000,
  uptime: 4242,
  rssi: -58,
  v: 231.4,
  mcu: 51.2,
  eco: false,
  wifi: true,
  ap: false,
  staIp: '192.168.1.50',
  apIp: '',
  ...LAN_CAL,
  currentCalibration: { 0: 100.0, 1: 88.5, 2: 112.0, 3: 97.3, 4: 105.6 },
  noiseFloor: { 0: 0.012, 1: 0.034, 2: 0.056, 3: 0.078, 4: 0.091 },
  lpfAlpha: { 0: 1.0, 1: 0.5, 2: 0.25, 3: 0.12, 4: 0.06 },
  azQueue: { 0: 3, 1: 4 },
  ch: { 0: { n: 'Counter 1', a: 1.2, w: 45.0, pf: 0.87, kwh: 1.5, s: 0, mkwh: 48 } },
  events: {},
  fw: '3.3.5',
  chip: 'esp32s3',
  otaRun: false,
  otaPct: 0,
  lastMonth: 202610,
  resetDay: 25,
  apSsid: 'ESP32-Elec-Counter',
  apIsDefault: true,
  staSsid: 'HomeNet',
  staIsDefault: false,
  cloud: { en: true, ok: true, age: 1, host: 'esp32-electricity-counter-default-rtdb.firebaseio.com', acct: 'meter@example.com' },
};

function main() {
  const { cl_adapt } = loadAdapter();
  const d = cl_adapt(CLOUD_LATEST, 'EC64C998B0EC');

  ok('cl_adapt returns an object', d && typeof d === 'object', typeof d);

  // The whole point of the bug report: every calibration value must survive.
  eq('voltageCalibration', d.voltageCalibration, LAN_CAL.voltageCalibration);
  eq('currentCalibration', d.currentCalibration, LAN_CAL.currentCalibration);
  eq('rmsSamples', d.rmsSamples, LAN_CAL.rmsSamples);
  eq('azBatches', d.azBatches, LAN_CAL.azBatches);
  eq('noiseFloor', d.noiseFloor, LAN_CAL.noiseFloor);
  eq('lpfAlpha', d.lpfAlpha, LAN_CAL.lpfAlpha);
  eq('azActive', d.azActive, true);
  eq('azChannel', d.azChannel, LAN_CAL.azChannel);
  eq('azProgress', d.azProgress, LAN_CAL.azProgress);
  eq('azQueue', d.azQueue, LAN_CAL.azQueue);

  // The renames the firmware performs must stay mapped (guards a future edit
  // that 'simplifies' them back to the LAN names the cloud never sends).
  eq('mcuTemp <- mcu', d.mcuTemp, 51.2);
  eq('firmwareVersion <- fw', d.firmwareVersion, '3.3.5');
  eq('ota <- otaRun', d.ota, false);
  eq('otaProgress <- otaPct', d.otaProgress, 0);
  eq('lastMonth', d.lastMonth, 202610);
  eq('resetDay', d.resetDay, 25);
  eq('staSsid', d.staSsid, 'HomeNet');
  eq('cloud.acct', d.cloud.acct, 'meter@example.com');

  // RTDB hands arrays back as keyed objects after a delete; the channels must
  // still arrive in index order or the cards render in the wrong order.
  ok('ch is a real array of 1', Array.isArray(d.ch) && d.ch.length === 1, JSON.stringify(d.ch));
  eq('ch[0].n', d.ch && d.ch[0].n, 'Counter 1');
  ok('events is an array', Array.isArray(d.events), typeof d.events);

  // An older board (payload without the block) must not invent values: the
  // renderer's typeof guards have to see "absent", not NaN/zero.
  const old = cl_adapt({ v: 231.4, ch: [], events: [] }, 'EC64C998B0EC');
  ok('a payload without calibration maps to undefined, not 0',
    old.voltageCalibration === undefined && old.currentCalibration === undefined &&
    old.rmsSamples === undefined && old.noiseFloor === undefined &&
    old.lpfAlpha === undefined && old.azActive === false,
    JSON.stringify({ v: old.voltageCalibration, r: old.rmsSamples, nf: old.noiseFloor }));

  console.log(`\n${checks - failures}/${checks} cloud adapter checks passed`);
  process.exit(failures ? 1 : 0);
}

try {
  main();
} catch (err) {
  console.log(`  FAIL adapter could not run: ${err && err.message}`);
  process.exit(1);
}