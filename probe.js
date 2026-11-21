const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36 Edg/131.0.0.0';
(async () => {
  for (const extra of [{}, {is_free: 1}, {is_free: '1'}]) {
    const body = Object.assign({page_size: 20, page: 1, keyword: 'whistle', search: 1}, extra);
    try {
      const r = await fetch('https://makeronline.com/api/search/model/v2.8', {method: 'POST', headers: {'Content-Type': 'application/json', 'Accept': 'application/json'}, body: JSON.stringify(body)});
      const t = await r.text();
      console.log('MO', JSON.stringify(extra), r.status, t.length);
      try { const d = JSON.parse(t); const items = d.data.data; console.log(' total', d.data.page.total, 'first item keys:', Object.keys(items[0]).join(',')); console.log(' first item:', JSON.stringify(items[0]).slice(0, 1500));
        const priced = items.filter(i => Object.keys(i).some(k => /price|pay|free|cost|point/i.test(k) && i[k] && i[k] !== '0' && i[k] !== 0)); console.log(' priced-looking:', priced.length, priced.slice(0,2).map(i => JSON.stringify(Object.fromEntries(Object.entries(i).filter(([k]) => /price|pay|free|cost|point/i.test(k))))).join(' ; '));
      } catch (e) { console.log(' body', t.slice(0, 300)); }
    } catch (e) { console.log('MO error', e.message); }
  }
  for (const url of ['https://makerworld.com/api/v1/search-service/select/design2?keyword=whistle&limit=5&offset=0',
                     'https://makerworld.com/api/v1/search-service/select/design?keyword=whistle&limit=5']) {
    try { const r = await fetch(url, {headers: {'User-Agent': UA, 'Accept': 'application/json'}}); const t = await r.text(); console.log('MW', url, r.status, t.slice(0, 400).replace(/\s+/g, ' ')); } catch (e) { console.log('MW error', e.message); }
  }
})();
