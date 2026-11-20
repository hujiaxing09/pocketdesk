// 手机端的“钥匙串”：记住每台电脑的编号、名字、地址和授权令牌。
// 不管页面是从哪台电脑打开的，都用这一份去连任意一台电脑，所以没有“入口电脑”。
//
// 存两处：
//   localStorage：平时用。
//   网址的 ?ring=：iPhone 主屏幕 App 和 Safari 存储不互通，但“添加到主屏幕”会保存完整网址，
//                  图标就自带全部授权；万一 App 的数据被清，也能从网址恢复。

const KEY = 'pd-ring';

export function loadRing() {
  try {
    const r = JSON.parse(localStorage.getItem(KEY) || 'null');
    if (r && typeof r.pcs === 'object') return r;
  } catch { /* 存储不可用或内容损坏 */ }
  return { pcs: {}, current: '' };
}

export function saveRing(ring) {
  try { localStorage.setItem(KEY, JSON.stringify(ring)); } catch { /* 同上 */ }
  writeRingToUrl(ring);
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
  const list = Object.values(ring.pcs)
    .filter((p) => p.token)
    .map((p) => [p.id, p.name, p.addr || (p.addrs || [])[0] || '', p.https, p.token, p.alias || '']);
  return list.length ? b64url(JSON.stringify(list)) : '';
}

export function decodeRing(text) {
  try {
    return JSON.parse(unb64url(text)).map(([id, name, addr, https, token, alias]) => ({
      id, name, addr, addrs: addr ? [addr] : [], https, token, ...(alias ? { alias } : {}),
    }));
  } catch {
    return [];
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

export function writeRingToUrl(ring) {
  const enc = encodeRing(ring);
  const params = [];
  if (enc) params.push(`ring=${enc}`);
  if (ring.phone) params.push(`p=${ring.phone}`);
  const url = params.length ? `${location.pathname}?${params.join('&')}` : location.pathname;
  if (location.pathname + location.search !== url) history.replaceState(null, '', url + location.hash);
}

export function ringFromUrl() {
  const v = new URLSearchParams(location.search).get('ring');
  return v ? decodeRing(v) : [];
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
