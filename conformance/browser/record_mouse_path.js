// Record a real mouse path over a conformance case and print a case.json step.
//
// Example:
//   node conformance/browser/record_mouse_path.js --test decius_mode_3d_faders --duration 8
//
// Move the mouse over the opened browser window. The script writes a JSON
// `mouse_recording` step that can be pasted into case.json.

import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

function parseArgs(argv) {
  const a = {
    casesDir: 'conformance/cases',
    channel: 'chrome',
    duration: 8,
    minDistance: 2,
    out: '',
  };
  for (let i = 2; i < argv.length; i++) {
    const k = argv[i], v = () => argv[++i];
    if (k === '--test') a.test = v();
    else if (k === '--cases-dir') a.casesDir = v();
    else if (k === '--html') a.html = v();
    else if (k === '--channel') a.channel = v();
    else if (k === '--duration') a.duration = Number(v());
    else if (k === '--min-distance') a.minDistance = Number(v());
    else if (k === '--out') a.out = v();
    else {
      console.error(`unknown option ${k}`);
      process.exit(2);
    }
  }
  if (!a.test && !a.html) {
    console.error('usage: node record_mouse_path.js --test <name> [--duration seconds] [--out path]');
    process.exit(2);
  }
  if (!a.html) a.html = path.join(a.casesDir, a.test, 'index.html');
  if (!a.script && a.test) a.script = path.join(a.casesDir, a.test, 'case.json');
  return a;
}

function loadCase(a) {
  let cfg = { width: 1024, height: 768, dpi: 1 };
  if (a.script && fs.existsSync(a.script)) {
    try { Object.assign(cfg, JSON.parse(fs.readFileSync(a.script, 'utf8'))); }
    catch (e) { console.error(`warning: malformed ${a.script}: ${e.message}`); }
  }
  return cfg;
}

async function launch(channel) {
  try { return await chromium.launch({ channel, headless: false }); }
  catch { return chromium.launch({ headless: false }); }
}

const args = parseArgs(process.argv);
const cfg = loadCase(args);
const browser = await launch(args.channel);

try {
  const ctx = await browser.newContext({
    viewport: { width: cfg.width, height: cfg.height },
    deviceScaleFactor: cfg.dpi || 1,
    reducedMotion: 'reduce',
  });
  const page = await ctx.newPage();
  await page.goto(pathToFileURL(path.resolve(args.html)).href, { waitUntil: 'load' });
  await page.evaluate((minDistance) => {
    window.__auiMousePath = [];
    let last = null;
    const push = (type, ev) => {
      window.__auiMousePath.push({
        type,
        x: Math.round(ev.clientX),
        y: Math.round(ev.clientY),
      });
    };
    window.addEventListener('mousemove', (ev) => {
      const pt = [Math.round(ev.clientX), Math.round(ev.clientY)];
      if (last) {
        const dx = pt[0] - last[0];
        const dy = pt[1] - last[1];
        if (Math.sqrt(dx * dx + dy * dy) < minDistance) return;
      }
      last = pt;
      push('move', ev);
    }, { passive: true });
    window.addEventListener('mousedown', (ev) => push('down', ev), { passive: true });
    window.addEventListener('mouseup', (ev) => push('up', ev), { passive: true });
  }, Math.max(0, args.minDistance || 0));

  console.error(`Recording ${args.duration}s. Move the mouse over the browser window...`);
  await page.waitForTimeout(Math.max(1, args.duration) * 1000);
  const points = await page.evaluate(() => window.__auiMousePath || []);
  const step = {
    mouse_recording: points,
    step_ms: 0,
    snapshot_prefix: 'path',
  };
  const text = JSON.stringify(step, null, 2);
  if (args.out) {
    fs.writeFileSync(args.out, text + '\n', 'utf8');
    console.error(`wrote ${args.out} (${points.length} points)`);
  } else {
    console.log(text);
    console.error(`recorded ${points.length} points`);
  }
} finally {
  await browser.close();
}
