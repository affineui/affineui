// Browser-side DOM hydrator for conformance cases.
//
// Some first-class libraries, Decius included, ship declarative HTML that is
// expanded by their own tiny JS runtime. AffineUI does not execute arbitrary
// page JavaScript, so this tool lets the conformance harness run that runtime
// once in Chrome, serialize the resulting DOM, and feed the same hydrated HTML
// to AffineUI. This keeps renderer conformance focused on layout/paint/SVG
// instead of accidentally testing whether AffineUI embeds a JS VM.

import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

function parseArgs(argv) {
  const a = { casesDir: 'conformance/cases', outHtml: '', channel: '' };
  for (let i = 2; i < argv.length; i++) {
    const k = argv[i], v = () => argv[++i];
    if (k === '--test') a.test = v();
    else if (k === '--cases-dir') a.casesDir = v();
    else if (k === '--html') a.html = v();
    else if (k === '--script') a.script = v();
    else if (k === '--out-html') a.outHtml = v();
    else if (k === '--channel') a.channel = v();
    else { console.error(`unknown option ${k}`); process.exit(2); }
  }
  if (!a.test && !a.html) {
    console.error('usage: node hydrate.js --test <name> --out-html <file> [--cases-dir DIR]');
    process.exit(2);
  }
  if (!a.html) a.html = path.join(a.casesDir, a.test, 'index.html');
  if (!a.script) a.script = a.test ? path.join(a.casesDir, a.test, 'case.json') : null;
  if (!a.outHtml) {
    console.error('hydrate.js requires --out-html');
    process.exit(2);
  }
  return a;
}

function loadCase(a) {
  let cfg = { width: 1024, height: 768, dpi: 1, steps: [] };
  if (a.script && fs.existsSync(a.script)) {
    try { Object.assign(cfg, JSON.parse(fs.readFileSync(a.script, 'utf8'))); }
    catch (e) { console.error(`warning: malformed ${a.script}: ${e.message}`); }
  }
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

function shouldRewrite(ref) {
  if (!ref || ref.startsWith('#') || ref.startsWith('data:')) return false;
  if (/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(ref)) return false;
  return true;
}

function rewriteLocalAssetRefs(html, htmlPath) {
  const base = path.dirname(path.resolve(htmlPath));
  const makeAbs = (ref) => path.resolve(base, ref).replace(/\\/g, '/');
  return html
    .replace(/(<link\b[^>]*\bhref=["'])([^"']+)(["'][^>]*>)/gi,
      (_m, pre, ref, post) => pre + (shouldRewrite(ref) ? makeAbs(ref) : ref) + post)
    .replace(/(<script\b[^>]*\bsrc=["'])([^"']+)(["'][^>]*>)/gi,
      (_m, pre, ref, post) => pre + (shouldRewrite(ref) ? makeAbs(ref) : ref) + post);
}

async function launch(channel) {
  if (channel) return chromium.launch({ channel });
  try { return await chromium.launch(); }
  catch { return chromium.launch({ channel: 'chrome' }); }
}

const args = parseArgs(process.argv);
const cfg = loadCase(args);
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
  await page.evaluate(() => document.fonts && document.fonts.ready);
  await page.evaluate(() => new Promise(requestAnimationFrame));
  await page.evaluate(() => document.querySelectorAll('script').forEach((script) => script.remove()));
  const raw = await page.evaluate(() => '<!doctype html>\n' + document.documentElement.outerHTML);
  const hydrated = rewriteLocalAssetRefs(raw, args.html);
  fs.mkdirSync(path.dirname(args.outHtml), { recursive: true });
  fs.writeFileSync(args.outHtml, hydrated, 'utf8');
  console.error(`wrote ${args.outHtml}`);
} finally {
  await browser.close();
}
