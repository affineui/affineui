// One-off: print menubar button geometry from the real page.
import { chromium } from 'playwright';
import { pathToFileURL } from 'url';

const html = process.argv[2];
let browser;
try { browser = await chromium.launch(); }
catch { browser = await chromium.launch({ channel: 'chrome' }); }
const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
await page.goto(pathToFileURL(html).href, { waitUntil: 'load' });
await page.evaluate(() => document.fonts.ready);
const fb = await page.evaluate(() => {
  const c = document.createElement('canvas').getContext('2d');
  c.font = '11px "IBM Plex Sans"';
  const m = c.measureText('View');
  return { asc: m.fontBoundingBoxAscent, desc: m.fontBoundingBoxDescent,
           actAsc: m.actualBoundingBoxAscent, actDesc: m.actualBoundingBoxDescent,
           adv: m.width };
});
console.error('plex11 metrics', JSON.stringify(fb));
const data = await page.evaluate(() => {
  const out = [];
  for (const btn of document.querySelectorAll('.dcs-menubar__item')) {
    const b = btn.getBoundingClientRect();
    const cs = getComputedStyle(btn);
    const range = document.createRange();
    range.selectNodeContents(btn);
    const t = range.getBoundingClientRect();
    out.push({
      label: btn.textContent.trim(),
      btn: { x: b.x, y: b.y, w: b.width, h: b.height },
      text: { x: t.x, y: t.y, w: t.width, h: t.height },
      pad: [cs.paddingTop, cs.paddingRight, cs.paddingBottom, cs.paddingLeft],
      lh: cs.lineHeight, fs: cs.fontSize, ff: cs.fontFamily,
      align: cs.alignItems, disp: cs.display,
    });
  }
  const bar = document.querySelector('.dcs-menubar');
  const bb = bar.getBoundingClientRect();
  out.push({ label: '__bar__', btn: { x: bb.x, y: bb.y, w: bb.width, h: bb.height },
             align: getComputedStyle(bar).alignItems });
  return out;
});
console.log(JSON.stringify(data, null, 1));
await browser.close();
