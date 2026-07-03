// Screenshot a probe case in Chrome and report the hex input's bounding
// rect so an ink-bbox analysis can compare against the engine render.
import { chromium } from 'playwright';

(async () => {
  const file = process.argv[2];
  let browser;
  try { browser = await chromium.launch(); }
  catch { browser = await chromium.launch({ channel: 'chrome' }); }
  const page = await browser.newPage({ viewport: { width: 1280, height: 200 }, deviceScaleFactor: 1 });
  await page.goto('file:///' + file.split('\\').join('/'));
  await page.waitForTimeout(300);
  const r = await page.evaluate(() => {
    const el = document.querySelector('.dcs-colorfield__hex');
    const b = el.getBoundingClientRect();
    const cs = getComputedStyle(el);
    return { x: b.x, y: b.y, w: b.width, h: b.height,
             lineHeight: cs.lineHeight, textAlign: cs.textAlign,
             fontSize: cs.fontSize, whiteSpace: cs.whiteSpace };
  });
  console.log(JSON.stringify(r));
  await page.screenshot({ path: process.argv[3] });
  await browser.close();
})();
