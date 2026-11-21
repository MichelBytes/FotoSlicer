const { chromium } = require('playwright');
const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36 Edg/131.0.0.0';
(async () => {
  // Plain requests first.
  for (const [name, url] of [['TV plain page', 'https://www.thingiverse.com/search?q=whistle&type=things&sort=relevant'],
                             ['TH api', 'https://thangs.com/api/search/v5/search-by-text?searchTerm=whistle&page=0&pageSize=5&pageScope=root']]) {
    try { const r = await fetch(url, {headers: {'User-Agent': UA, 'Accept': name.includes('api') ? 'application/json' : 'text/html'}}); const t = await r.text(); console.log(name, r.status, t.length, t.slice(0, 300).replace(/\s+/g, ' ')); } catch (e) { console.log(name, 'error', e.message); }
  }
  const b = await chromium.launch();
  const ctx = await b.newContext({ userAgent: UA, viewport: { width: 1280, height: 900 }, locale: 'en-US' });
  const p = await ctx.newPage(); const seen = [];
  p.on('request', r => { const u = r.url(); if (r.resourceType() === 'xhr' || r.resourceType() === 'fetch') seen.push(r.method() + ' ' + u.slice(0, 250) + ' ' + JSON.stringify(Object.fromEntries(Object.entries(r.headers()).filter(([k]) => /auth|token|key/i.test(k)))) + (r.postData() ? ' BODY ' + r.postData().slice(0, 300) : '')); });
  p.on('response', async r => { const u = r.url(); if (/search/i.test(u) && /json/.test(r.headers()['content-type'] || '')) { try { seen.push('RESP ' + r.status() + ' ' + u.slice(0, 200) + ' => ' + (await r.text()).slice(0, 900)); } catch (e) {} } });
  const t0 = Date.now();
  try {
    await p.goto('https://www.thingiverse.com/search?q=whistle&type=things&sort=relevant', { waitUntil: 'domcontentloaded', timeout: 60000 });
    console.log('TV domcontentloaded after', Date.now() - t0, 'ms', p.url(), '|', await p.title());
    await p.waitForTimeout(12000);
    const links = await p.$$eval('a[href]', as => as.map(a => a.href).filter(h => /thing:\d+/.test(h)));
    console.log('TV thing links', links.length, links.slice(0, 4).join(' '));
  } catch (e) { console.log('TV error', e.message); }
  console.log('TV requests:\n' + seen.slice(0, 40).join('\n'));
  await b.close();
})();
