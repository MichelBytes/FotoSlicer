const { chromium } = require('playwright');
const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36 Edg/131.0.0.0';
const watch = (page, tag) => {
  const seen = [];
  page.on('request', r => { const u = r.url(); if (/search|keyword|query|graphql|api/i.test(u) && !/\.(js|css|png|jpg|webp|svg|woff2?)(\?|$)/i.test(u)) seen.push(r.method() + ' ' + u.slice(0, 300) + (r.postData() ? ' BODY ' + r.postData().slice(0, 400) : '')); });
  page.on('response', async r => { const u = r.url(); if (/search/i.test(u) && /json/.test(r.headers()['content-type'] || '')) { try { seen.push('RESP ' + u.slice(0, 200) + ' => ' + (await r.text()).slice(0, 700)); } catch (e) {} } });
  return seen;
};
(async () => {
  const b = await chromium.launch();
  const ctx = await b.newContext({ userAgent: UA, viewport: { width: 1280, height: 900 }, locale: 'en-US' });
  // Makeronline: type into its search box like the app does.
  const p = await ctx.newPage(); const seen = watch(p, 'MO');
  try {
    await p.goto('https://makeronline.com/en/', { waitUntil: 'networkidle', timeout: 60000 });
    console.log('MO inputs:\n' + (await p.$$eval('input', els => els.map(e => e.outerHTML.slice(0, 250)))).join('\n'));
    const input = await p.$('input[type=search], input[placeholder*="earch" i], input[name*="search" i], input[class*="search" i], input[type=text]');
    if (input) { await input.click({ force: true }).catch(() => {}); await input.fill('whistle'); await input.press('Enter'); }
    await p.waitForTimeout(10000);
    console.log('MO after:', p.url(), '|', await p.title());
    console.log('MO model links:\n' + (await p.$$eval('a[href]', as => as.map(a => a.href + ' | ' + (a.textContent || '').trim().slice(0, 50)).filter(h => /\/model\//.test(h)).slice(0, 12))).join('\n'));
    await p.screenshot({ path: 'makeronline.png' });
  } catch (e) { console.log('MO error', e.message); }
  console.log('MO requests:\n' + seen.slice(0, 40).join('\n'));
  // MakerWorld: its search address.
  const q = await ctx.newPage(); const seen2 = watch(q, 'MW');
  try {
    await q.goto('https://makerworld.com/en/search/models?keyword=whistle', { waitUntil: 'domcontentloaded', timeout: 60000 });
    await q.waitForTimeout(12000);
    console.log('MW after:', q.url(), '|', await q.title());
    const links = await q.$$eval('a[href]', as => as.map(a => a.href + ' | ' + (a.textContent || '').trim().slice(0, 50)).filter(h => /\/models\/\d+/.test(h)));
    console.log('MW model links', links.length + ':\n' + links.slice(0, 8).join('\n'));
    console.log('MW text:', (await q.evaluate(() => document.body.innerText)).slice(0, 400).replace(/\n+/g, ' / '));
    await q.screenshot({ path: 'makerworld.png' });
  } catch (e) { console.log('MW error', e.message); }
  console.log('MW requests:\n' + seen2.slice(0, 25).join('\n'));
  await b.close();
})();
