// 手机端的“钥匙串”：所在的组、组令牌（组里每台电脑都认），以及组里电脑的缓存（编号、名字、地址）。
// 电脑列表每次从电脑那里拿最新的；不管页面是从哪台电脑打开的，都用这一份去连组里任意一台。
//
// 存两处：
//   localStorage：平时用。
//   网址的 ?k=：iPhone 主屏幕 App 和 Safari 存储不互通，但“添加到主屏幕”会保存完整网址，
//               图标就自带授权；万一 App 的数据被清，也能从网址恢复。

const KEY = 'pd-ring';

// legacy：1.0.0 的钥匙串里每台电脑各有一个令牌，启动时挨个试，找出组里还认的那个
function emptyRing() {
  return { group: '', groupName: '', token: '', phone: '', current: '', pcs: {}, legacy: [] };
}

export function loadRing() {
  const ring = emptyRing();
  try {
    const r = JSON.parse(localStorage.getItem(KEY) || 'null');
    if (r && typeof r === 'object') Object.assign(ring, r);
  } catch { /* 存储不可用或内容损坏 */ }
  if (!ring.pcs || typeof ring.pcs !== 'object') ring.pcs = {};
  if (!Array.isArray(ring.legacy)) ring.legacy = [];
  for (const p of Object.values(ring.pcs)) {
    if (!p.token) continue;
    ring.legacy.push({ id: p.id, addrs: p.addrs || [], https: p.https, token: p.token });
    delete p.token;
  }
  return ring;
}

export function saveRing(ring) {
  try { localStorage.setItem(KEY, JSON.stringify(ring)); } catch { /* 同上 */ }
  writeRingToUrl(ring);
}

// 换到另一个组（在别的组的电脑上授权）：之前的电脑列表作废
export function resetRing(ring, group, token) {
  const phone = ring.phone;
  Object.assign(ring, emptyRing(), { phone, group: group.id, groupName: group.name || '', token });
}

function b64url(s) {
  const bytes = new TextEncoder().encode(s);
  let bin = '';
  for (const b of bytes) bin += String.fromCharCode(b);
  return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

function unb64url(s) {
  let t = s.replace(/-/g, '+').replace(/_/g, '/');
  while (t.length % 4) t += '=';
  const bin = atob(t);
  return new TextDecoder().decode(Uint8Array.from(bin, (c) => c.charCodeAt(0)));
}

export function encodeRing(ring) {
  if (!ring.token) return '';
  const pcs = Object.values(ring.pcs).map((p) => [p.id, p.name, p.addr || (p.addrs || [])[0] || '', p.https, p.alias || '']);
  return b64url(JSON.stringify([ring.group, ring.token, pcs]));
}

const addrsOf = (addr) => (addr ? [addr] : []);
const aliasOf = (alias) => (alias ? { alias } : {});

// ?k= 的内容：{ group, token, pcs }；解不开返回 null
export function decodeRing(text) {
  try {
    const [group, token, pcs] = JSON.parse(unb64url(text));
    return { group, token, pcs: pcs.map(([id, name, addr, https, alias]) => ({ id, name, addrs: addrsOf(addr), https, ...aliasOf(alias) })) };
  } catch {
    return null;
  }
}

// 1.0.0 的 ?ring=：每台电脑各带一个令牌
export function decodeLegacyRing(text) {
  try {
    return JSON.parse(unb64url(text)).map(([id, name, addr, https, token, alias]) => ({ id, name, addrs: addrsOf(addr), https, token, ...aliasOf(alias) }));
  } catch {
    return [];
  }
}

// 网址里带的钥匙串（主屏幕图标）
export function ringFromUrl(ring, search = location.search) {
  if (ring.token) return;
  const q = new URLSearchParams(search);
  const k = decodeRing(q.get('k') || '');
  if (k) {
    Object.assign(ring, { group: k.group, token: k.token });
    for (const p of k.pcs) upsert(ring, p);
  }
  for (const { token, ...pc } of decodeLegacyRing(q.get('ring') || '')) {
    ring.legacy.push({ id: pc.id, addrs: pc.addrs, https: pc.https, token });
    upsert(ring, pc);
  }
}

// 手机编号：第一次打开时随机生成，授权时发给电脑，用来认出“这还是同一部手机”。
// 浏览器拿不到真正的硬件编号，所以它跟着钥匙串一起写进网址（?p=），
// Safari 和主屏幕 App 之间来回跳转时也带着它。
export function ensurePhoneId(ring, fromUrl) {
  if (!ring.phone) ring.phone = fromUrl || randomHex(8);
  return ring.phone;
}

function randomHex(bytes) {
  const b = new Uint8Array(bytes);
  crypto.getRandomValues(b);
  return [...b].map((x) => x.toString(16).padStart(2, '0')).join('');
}

function writeRingToUrl(ring) {
  const enc = encodeRing(ring);
  const params = [];
  if (enc) params.push(`k=${enc}`);
  if (ring.phone) params.push(`p=${ring.phone}`);
  const url = params.length ? `${location.pathname}?${params.join('&')}` : location.pathname;
  if (location.pathname + location.search !== url) history.replaceState(null, '', url + location.hash);
}

// 手机上显示的名字：用户起的别名优先，否则用电脑自己的名字
export function label(p) {
  return (p && (p.alias || p.name || p.addr)) || '电脑';
}

// 合并一台电脑的信息；地址去重，最新的放前面（alias 只在用户改名时设置，这里不会被电脑名覆盖）
export function upsert(ring, pc) {
  const old = ring.pcs[pc.id] || { id: pc.id, addrs: [] };
  const merged = { ...old };
  for (const [k, v] of Object.entries(pc)) {
    if (v !== undefined && v !== '' && k !== 'addrs') merged[k] = v;
  }
  const addrs = [...(pc.addr ? [pc.addr] : []), ...(pc.addrs || []), ...(old.addrs || [])];
  merged.addrs = [...new Set(addrs.filter(Boolean))].slice(0, 6);
  ring.pcs[pc.id] = merged;
  return merged;
}

export function baseOf(addr, https) {
  return `https://${addr}:${https}`;
}

export async function fetchTimeout(url, opts = {}, ms = 2500) {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), ms);
  try {
    return await fetch(url, { ...opts, signal: ctl.signal, cache: 'no-store' });
  } finally {
    clearTimeout(timer);
  }
}

// 读一台电脑的 /api/info；连不上（没开机、或者手机还没信任它的证书）时返回 null
export async function getInfo(base, ms = 2000) {
  try {
    const res = await fetchTimeout(`${base}/api/info`, {}, ms);
    return res.ok ? await res.json() : null;
  } catch {
    return null;
  }
}

// 同时试这台电脑所有已知地址，找到编号对得上的那个
export async function probe(pc) {
  const tries = (pc.addrs || []).map(async (addr) => {
    const info = await getInfo(baseOf(addr, pc.https), 1500);
    if (!info || info.id !== pc.id) throw new Error('not this pc');
    return { addr, info };
  });
  try {
    const { addr, info } = await Promise.any(tries);
    pc.addr = addr;
    return info;
  } catch {
    return null;
  }
}

// 解析电脑上二维码里的网址：http://IP:47080/?pair=配对码&id=电脑编号&https=47443
export function parsePairUrl(text) {
  try {
    const u = new URL(text);
    const code = u.searchParams.get('pair');
    if (!code) return null;
    return {
      ip: u.hostname,
      http: Number(u.port) || 47080,
      https: Number(u.searchParams.get('https')) || 47443,
      id: u.searchParams.get('id') || '',
      code,
    };
  } catch {
    return null;
  }
}
