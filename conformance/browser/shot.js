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
//   {"click":[x,y]} {"hover":[x,y]} {"mouse_path":[[x,y],...]}
//   {"mouse_recording":[{"type":"move|down|up","x":N,"y":N},...]}
//   {"wait_ms":N}
//   {"animation_time_ms":N} {"snapshot":"name"}

import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL, fileURLToPath } from 'node:url';

// Force generic-font tests to render with AffineUI's embedded Roboto so the A/B
// compares the same fallback typeface on both sides. Chrome resolves `sans-serif`
// to the system default (Arial on Windows), but AffineUI renders Roboto
// — different glyph shapes AND metrics, which inflates every text test's
// diff and bounds how low it can go. We control Roboto in-repo, so we
// inject it into the page (cross-platform, unlike depending on system
// Arial). This isolates CSS/layout conformance from font choice; the
// residual is just rasterizer AA (Skia vs NanoVG), which is irreducible. Pages
// with their own bundled fonts, like Decius, opt out via body.dcs.
const FONTS_DIR = path.resolve(fileURLToPath(import.meta.url), '../../../assets/fonts');
function fontFaceCss() {
  const url = (f) => pathToFileURL(path.join(FONTS_DIR, f)).href;
  return `
    @font-face { font-family:'AUI-Sans'; font-style:normal; font-weight:400;
                 src:url('${url('Roboto-Regular.ttf')}') format('truetype'); }
    @font-face { font-family:'AUI-Sans'; font-style:normal; font-weight:700;
                 src:url('${url('Roboto-Bold.ttf')}') format('truetype'); }
    /* Only normalize pages that are not carrying their own first-class font
       fixture. A global override here erases icon fonts and invalidates the
       browser reference for Decius. */
    body:not(.dcs), body:not(.dcs) *, body:not(.dcs) *::before, body:not(.dcs) *::after {
      font-family:'AUI-Sans' !important;
    }
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

function list(v) {
  if (Array.isArray(v)) return v;
  if (typeof v === 'string' && v.length) return [v];
  return [];
}

function localScriptPath(args, script) {
  if (path.isAbsolute(script)) return script;
  if (/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(script)) return script;
  return path.resolve(path.dirname(args.html), script);
}

async function injectBrowserScripts(page, args, cfg) {
  for (const script of list(cfg.browser_scripts)) {
    const resolved = localScriptPath(args, script);
    if (/^https?:/i.test(resolved)) {
      await page.addScriptTag({ url: resolved });
    } else {
      await page.addScriptTag({ path: resolved });
    }
  }
  if (list(cfg.browser_scripts).length) {
    await page.evaluate(() => new Promise(requestAnimationFrame));
  }
}

async function launch(channel) {
  if (channel) return chromium.launch({ channel });
  try { return await chromium.launch(); }
  catch { return chromium.launch({ channel: 'chrome' }); }
}

const args = parseArgs(process.argv);
const cfg = loadCase(args);
const name = args.test || 'test';
function shouldUseFontOverride(cfg, name) {
  if (typeof cfg.browser_font_override === 'boolean') {
    return cfg.browser_font_override;
  }
  return !name.startsWith('decius_');
}
const browser = await launch(args.channel);
try {
  const ctx = await browser.newContext({
    viewport: { width: cfg.width, height: cfg.height },
    deviceScaleFactor: cfg.dpi,
    reducedMotion: 'reduce',
  });
  const page = await ctx.newPage();
  await page.goto(pathToFileURL(path.resolve(args.html)).href, { waitUntil: 'load' });
  await injectBrowserScripts(page, args, cfg);
  // Swap in the shared Roboto so generic text matches AffineUI, unless the
  // case is explicitly testing page-provided web fonts or icon fonts.
  if (shouldUseFontOverride(cfg, name)) {
    await page.addStyleTag({ content: fontFaceCss() });
  }
  await page.evaluate(() => document.fonts.ready);

  const setPulse = async (pt) => {
    await page.evaluate((p) => {
      const existing = document.getElementById('__aui_click_pulse');
      if (existing) existing.remove();
      if (!p) return;
      const el = document.createElement('div');
      el.id = '__aui_click_pulse';
      el.style.cssText = [
        'position:fixed',
        `left:${p.x - 20}px`,
        `top:${p.y - 20}px`,
        'width:40px',
        'height:40px',
        'pointer-events:none',
        'z-index:2147483647',
      ].join(';');
      const lineCss = [
        'position:absolute',
        'background:rgba(90,180,255,.75)',
        'box-shadow:0 0 6px rgba(90,180,255,.42)',
      ].join(';');
      const h1 = document.createElement('div');
      h1.style.cssText = `${lineCss};left:0;top:19px;width:16px;height:2px`;
      const h2 = document.createElement('div');
      h2.style.cssText = `${lineCss};right:0;top:19px;width:16px;height:2px`;
      const v1 = document.createElement('div');
      v1.style.cssText = `${lineCss};left:19px;top:0;width:2px;height:16px`;
      const v2 = document.createElement('div');
      v2.style.cssText = `${lineCss};left:19px;bottom:0;width:2px;height:16px`;
      const dot = document.createElement('div');
      dot.style.cssText = [
        'position:absolute',
        'left:17px',
        'top:17px',
        'width:6px',
        'height:6px',
        'background:rgba(90,180,255,.32)',
      ].join(';');
      el.append(h1, h2, v1, v2, dot);
      document.body.appendChild(el);
    }, pt);
  };

  const shot = async (snap, pulse = null) => {
    const out = path.join(args.outDir, `${name}.browser.${snap}.png`);
    await setPulse(pulse);
    await page.screenshot({ path: out, clip: { x: 0, y: 0, width: cfg.width, height: cfg.height } });
    await setPulse(null);
    console.error(`wrote ${out}`);
  };
  const indexedSnap = (prefix, index) => `${prefix}_${String(index).padStart(3, '0')}`;

  let took = false;
  for (const step of cfg.steps) {
    if (step.click) await page.mouse.click(step.click[0], step.click[1]);
    else if (step.hover) await page.mouse.move(step.hover[0], step.hover[1]);
    else if (step.mouse_path) {
      const points = Array.isArray(step.mouse_path) ? step.mouse_path : [];
      for (let i = 0; i < points.length; i++) {
        const pt = points[i];
        if (!Array.isArray(pt) || pt.length < 2) continue;
        await page.mouse.move(pt[0], pt[1]);
        if (step.step_ms != null && step.step_ms > 0) {
          await page.waitForTimeout(step.step_ms);
        }
        if (step.snapshot_prefix) {
          await shot(indexedSnap(step.snapshot_prefix, i));
          took = true;
        }
      }
    }
    else if (step.mouse_recording) {
      const events = Array.isArray(step.mouse_recording) ? step.mouse_recording : [];
      for (let i = 0; i < events.length; i++) {
        const ev = events[i] || {};
        const x = Number(ev.x);
        const y = Number(ev.y);
        if (!Number.isFinite(x) || !Number.isFinite(y)) continue;
        await page.mouse.move(x, y);
        if (ev.type === 'down') await page.mouse.down();
        else if (ev.type === 'up') await page.mouse.up();
        if (step.step_ms != null && step.step_ms > 0) {
          await page.waitForTimeout(step.step_ms);
        }
        if (step.snapshot_prefix) {
          await shot(indexedSnap(step.snapshot_prefix, i),
                     ev.type === 'down' ? { x, y } : null);
          took = true;
        }
      }
    }
    else if (step.wait_ms != null) await page.waitForTimeout(step.wait_ms);
    else if (step.animation_time_ms != null) {
      await page.evaluate((ms) => {
        for (const anim of document.getAnimations({ subtree: true })) {
          anim.pause();
          anim.currentTime = ms;
        }
      }, step.animation_time_ms);
      await page.evaluate(() => new Promise(requestAnimationFrame));
    }
    else if (step.snapshot != null) { await shot(step.snapshot); took = true; }
    // else: unknown step type — skip (agents add new types to both drivers).
  }
  if (!took) await shot('default');
} finally {
  await browser.close();
}
