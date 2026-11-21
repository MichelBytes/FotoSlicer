const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36 Edg/131.0.0.0';
const pick = (o, re) => JSON.stringify(Object.fromEntries(Object.entries(o).filter(([k]) => re.test(k))));
(async () => {
  const body = {page_size: 20, page: 1, keyword: 'whistle', category_id: '', order: '', award: '', exclusive: '', color: '', is_free: '', print_type: '0', weight: 1, license: '', printType: '', priceType: '', colorType: '', search: 1};
  const r = await fetch('https://makeronline.com/api/search/model/v2.8', {method: 'POST', headers: {'Content-Type': 'application/json', 'Accept': 'application/json'}, body: JSON.stringify(body)});
  const d = await r.json();
  console.log('MO', r.status, d.code, d.message, 'total', d.data && d.data.page && d.data.page.total);
  const items = (d.data && d.data.data) || [];
  if (items[0]) console.log('MO keys', Object.keys(items[0]).join(','));
  for (const i of items.slice(0, 20)) console.log('MO item', i.mold_id, '|', i.title, '|', pick(i, /price|pay|free|cost|point|exclusive|nsfw|target|permission|status|type/i));
  const m = await (await fetch('https://makerworld.com/api/v1/search-service/select/design2?keyword=whistle&limit=20&offset=0', {headers: {'User-Agent': UA, 'Accept': 'application/json'}})).json();
  if (m.hits[0]) console.log('MW keys', Object.keys(m.hits[0]).join(','));
  for (const h of m.hits.slice(0, 20)) console.log('MW hit', h.id, '|', h.title, '|', pick(h, /price|pay|free|cost|point|exclusive|nsfw|url|slug|mature|status|type|ai/i));
})();
