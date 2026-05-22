// Real-browser side of the AffineUI conformance harness.
//
// Loads a named test (--test <name> => <cases-dir>/<name>/{index.html,case.json}),
// replays the SAME case.json interaction script the AffineUI side runs, and
// writes a PNG at each `snapshot` marker. Pairs with tools/conformance
// (conformance_test) + conformance/diff.py. Coordinates are CSS pixels.
//
//   node shot.js --test <name> [--cases-dir DIR] [--out-dir DIR]
//                [--channel chrome|chromium] [--width W] [--height H] [--dpi S]
//
// Step vocabulary is small + extensible (agents add new types to this dispatch
// and the C++ side as they go); unknown step types are skipped. Starter set:
//   {"click":[x,y]} {"hover":[x,y]} {"wait_ms":N} {"snapshot":"name"}

import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL, fileURLToPath } from 'node:url';

// Force the browser to render with AffineUI's embedded Roboto so the A/B
// compares the SAME typeface on both sides. Chrome resolves `sans-serif`
// to the system default (Arial on Windows), but AffineUI renders Roboto
// — different glyph shapes AND metrics, which inflates every text test's
// diff and bounds how low it can go. We control Roboto in-repo, so we
// inject it into the page (cross-platform, unlike depending on system
// Arial). This isolates CSS/layout conformance from font choice; the
// residual is just rasterizer AA (Skia vs NanoVG), which is irreducible.
const FONTS_DIR = path.resolve(fileURLToPath(import.meta.url), '../../../assets/fonts');
function fontFaceCss() {
  const url = (f) => pathToFileURL(path.join(FONTS_DIR, f)).href;
  return `
    @font-face { font-family:'AUI-Sans'; font-style:normal; font-weight:400;
                 src:url('${url('Roboto-Regular.ttf')}') format('truetype'); }
    @font-face { font-family:'AUI-Sans'; font-style:normal; font-weight:700;
                 src:url('${url('Roboto-Bold.ttf')}') format('truetype'); }
    /* Override every element's family (incl. generic sans-serif/monospace,
       which AffineUI also renders as Roboto) so both sides match. */
    *, *::before, *::after { font-family:'AUI-Sans' !important; }
  `;
}

function parseArgs(argv) {
  const a = { casesDir: 'conformance/cases', outDir: '.', channel: '' };
  for (let i = 2; i < argv.length; i++) {
    const k = argv[i], v = () => argv[++i];
    if (k === '--test') a.test = v();
    else if (k === '--cases-dir') a.casesDir = v();
    else if (k === '--out-dir') a.outDir = v();
    else if (k === '--html') a.html = v();
    else if (k === '--script') a.script = v();
    else if (k === '--channel') a.channel = v();
    else if (k === '--width') a.width = parseInt(v(), 10);
    else if (k === '--height') a.height = parseInt(v(), 10);
    else if (k === '--dpi') a.dpi = parseFloat(v());
    else { console.error(`unknown option ${k}`); process.exit(2); }
  }
  if (!a.test && !a.html) { console.error('usage: node shot.js --test <name> [--cases-dir DIR] [--out-dir DIR] [...]'); process.exit(2); }
  if (!a.html) a.html = path.join(a.casesDir, a.test, 'index.html');
  if (!a.script) a.script = a.test ? path.join(a.casesDir, a.test, 'case.json') : null;
  return a;
}

function loadCase(a) {
  let cfg = { width: 1024, height: 768, dpi: 1, steps: [] };
  if (a.script && fs.existsSync(a.script)) {
    try { Object.assign(cfg, JSON.parse(fs.readFileSync(a.script, 'utf8'))); }
    catch (e) { console.error(`warning: malformed ${a.script}: ${e.message}`); }
  }
  if (a.width) cfg.width = a.width;
  if (a.height) cfg.height = a.height;
  if (a.dpi) cfg.dpi = a.dpi;
  return cfg;
}

async function launch(channel) {
  if (channel) return chromium.launch({ channel });
  try { return await chromium.launch(); }
  catch { return chromium.launch({ channel: 'chrome' }); }
}

const args = parseArgs(process.argv);
const cfg = loadCase(args);
const name = args.test || 'test';
const browser = await launch(args.channel);
try {
  const ctx = await browser.newContext({
    viewport: { width: cfg.width, height: cfg.height },
    deviceScaleFactor: cfg.dpi,
    reducedMotion: 'reduce',
  });
  const page = await ctx.newPage();
  await page.goto(pathToFileURL(path.resolve(args.html)).href, { waitUntil: 'load' });
  // Swap in the shared Roboto so text matches AffineUI, then wait for the
  // faces to finish loading before any screenshot.
  await page.addStyleTag({ content: fontFaceCss() });
  await page.evaluate(() => document.fonts.ready);

  const shot = async (snap) => {
    const out = path.join(args.outDir, `${name}.browser.${snap}.png`);
    await page.screenshot({ path: out, clip: { x: 0, y: 0, width: cfg.width, height: cfg.height } });
    console.error(`wrote ${out}`);
  };

  let took = false;
  for (const step of cfg.steps) {
    if (step.click) await page.mouse.click(step.click[0], step.click[1]);
    else if (step.hover) await page.mouse.move(step.hover[0], step.hover[1]);
    else if (step.wait_ms != null) await page.waitForTimeout(step.wait_ms);
    else if (step.snapshot != null) { await shot(step.snapshot); took = true; }
    // else: unknown step type — skip (agents add new types to both drivers).
  }
  if (!took) await shot('default');
} finally {
  await browser.close();
}
