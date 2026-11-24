// PocketDesk 手机页面：WebRTC 收画面，触控/触控板/键盘发回电脑。
import { Trackpad } from './trackpad.js';
import { Keyboard } from './keyboard.js';
import { loadRing, saveRing, resetRing, ringFromUrl, upsert, baseOf, fetchTimeout, getInfo, probe, parsePairUrl,
  label, ensurePhoneId } from './pcs.js';

const $ = (id) => document.getElementById(id);
const video = $('screen');
const stage = $('stage');
const statusEl = $('status');
const statsEl = $('stats');
const sheet = $('sheet');
const authEl = $('auth');

// ---------------- 偏好设置（存在本机浏览器里）----------------

const prefs = {
  mode: 'touch',
  sensitivity: 1,
  bitrate: 'auto',
  scale: 0,
  fps: 60,
  outputs: {},  // 每台电脑选的显示器
  stats: false,
  idleMinutes: 10, // 多久不操作就自动断开（0 = 从不）
  fab: { side: 'right', y: 0.42 }, // 浮动按钮的位置（贴哪一边、纵向位置占屏幕高度的比例）
};
try {
  Object.assign(prefs, JSON.parse(localStorage.getItem('pd-prefs') || '{}'));
} catch { /* 无痕模式等情况下存储不可用 */ }
function savePrefs() {
  try { localStorage.setItem('pd-prefs', JSON.stringify(prefs)); } catch { /* 同上 */ }
}

// ---------------- 电脑组和授权（钥匙串）----------------

const ring = loadRing();
let originInfo = null;            // 提供这个页面的那台电脑（可能已经关机，页面来自离线缓存）

// iPad 的 Safari 默认伪装成 Mac，用触摸点数区分
const isIOS = /iPhone|iPad|iPod/.test(navigator.userAgent) ||
  (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
const isAndroid = /Android/.test(navigator.userAgent);

// 授权时报给电脑的手机名字，显示在电脑的已授权列表里
async function deviceName() {
  const ua = navigator.userAgent;
  const ios = ua.match(/(iPhone|iPad)[^)]*OS (\d+)_(\d+)/);
  if (ios) return `${ios[1]}（iOS ${ios[2]}.${ios[3]}）`;
  if (isAndroid) {
    // 新版 Chrome 的 UA 里型号固定写成 “K”、版本固定写成 10，真实值要另外问
    try {
      const h = await navigator.userAgentData?.getHighEntropyValues(['model', 'platformVersion']);
      if (h?.model) return `${h.model}（Android ${(h.platformVersion || '').split('.')[0] || '?'}）`;
    } catch { /* 不支持就用 UA */ }
    const inner = (ua.match(/\(([^)]*Android[^)]*)\)/) || [])[1] || '';
    const parts = inner.split(';').map((p) => p.trim()).filter(Boolean);
    const ver = ((parts.find((p) => /^Android \d+/.test(p)) || '').match(/\d+/) || ['?'])[0];
    // 优先取带 “Build/” 的那段（型号 Build/版本号），否则跳过 Linux、U、wv、K、语言代码这类
    const built = parts.find((p) => /\bBuild\//.test(p));
    const model = built ? built.replace(/\s*Build\/.*$/, '').trim()
      : parts.find((p) => !/^(Linux|U|wv|K|Android.*|[a-z]{2,3}([-_][A-Za-z]{2,4})?)$/i.test(p));
    return model ? `${model}（Android ${ver}）` : `安卓手机（Android ${ver}）`;
  }
  return '浏览器';
}

const groupPcs = () => (ring.token ? Object.values(ring.pcs) : []);
const currentPc = () => (ring.token && ring.pcs[ring.current]) || null;
const authHeader = (token = ring.token) => ({ Authorization: `Bearer ${token}` });

// 连这台电脑用的地址：页面所在的电脑直接用当前网址，不用跨域
function baseFor(p) {
  if (originInfo && p.id === originInfo.id) return location.origin;
  return baseOf(p.addr || (p.addrs || [])[0], p.https);
}

// 电脑可能有好几个网段的地址（例如还连着 VPN），优先用和手机当前连着的电脑同一网段的
function sameSubnetFirst(ips) {
  const here = (currentPc()?.addr || location.hostname).split('.').slice(0, 3).join('.');
  return [...ips].sort((a, b) => (b.startsWith(`${here}.`) ? 1 : 0) - (a.startsWith(`${here}.`) ? 1 : 0));
}

// 电脑发来的组成员列表：已知的电脑更新地址，新加入组的电脑加进来，移出组的删掉
function applyGroup(data) {
  ring.group = data.group.id;
  ring.groupName = data.group.name;
  const ids = new Set();
  for (const p of data.pcs) {
    ids.add(p.id);
    // 已经连得上的地址留在最前面，新知道的地址排在后面备用
    const known = ring.pcs[p.id]?.addr;
    upsert(ring, { id: p.id, name: p.name, https: p.https, http: p.http, addrs: sameSubnetFirst(p.ips || []), online: p.online });
    if (known) upsert(ring, { id: p.id, addr: known });
  }
  for (const id of Object.keys(ring.pcs)) if (!ids.has(id)) delete ring.pcs[id];
}

// 向组里任意一台电脑要最新的电脑列表：先问页面所在的电脑和当前电脑，问不到就挨个问
async function refreshGroup() {
  if (!ring.token) return;
  const order = [];
  if (originInfo) order.push(ring.pcs[originInfo.id] || { id: originInfo.id });
  order.push(...[currentPc(), ...groupPcs()].filter(Boolean));
  const tried = new Set();
  for (const p of order) {
    if (tried.has(p.id)) continue;
    tried.add(p.id);
    if (p.id !== originInfo?.id && !(p.addrs || []).length) continue;
    try {
      const res = await fetchTimeout(`${baseFor(p)}/api/group`, { headers: authHeader() }, 2000);
      if (!res.ok) continue; // 401：不是这个组的电脑（或者授权被移除了，连接时会发现）
      applyGroup(await res.json());
      saveRing(ring);
      if (!sheet.hidden) renderPcs();
      return;
    } catch { /* 换下一台 */ }
  }
}

// 1.0.0 的钥匙串：每台电脑一个令牌。找到组里还认的那个，就是这部手机的组令牌
async function migrateLegacy() {
  for (const l of ring.legacy) {
    const base = originInfo && l.id === originInfo.id ? location.origin : baseOf((l.addrs || [])[0], l.https);
    try {
      const res = await fetchTimeout(`${base}/api/group`, { headers: authHeader(l.token) }, 2000);
      if (!res.ok) continue;
      ring.token = l.token;
      applyGroup(await res.json());
      ring.legacy = [];
      return;
    } catch { /* 下一个 */ }
  }
}

// 授权成功：令牌对整个组有效。在另一个组的电脑上授权，就换到那个组
function adoptToken(result, pc) {
  if (ring.group !== result.group.id) resetRing(ring, result.group, result.token);
  else ring.token = result.token;
  ring.legacy = [];
  upsert(ring, pc);
  ring.current = pc.id;
}

// 页面启动：整理钥匙串，确定要连哪台电脑
async function initRing() {
  ringFromUrl(ring);
  ensurePhoneId(ring, new URLSearchParams(location.search).get('p') || '');
  originInfo = await getInfo('', 2500);
  const self = originInfo && { id: originInfo.id, name: originInfo.name, https: originInfo.https, http: originInfo.http,
    addrs: [location.hostname, ...(originInfo.ips || [])].filter((a) => a !== '127.0.0.1'), online: true };
  if (originInfo) {
    // 本机调试入口（http://127.0.0.1）不需要授权，电脑对本机请求不检查令牌
    if (location.protocol === 'http:' && location.hostname === '127.0.0.1' && !ring.token) {
      Object.assign(ring, { token: 'local-debug', group: originInfo.group });
    }
    if (ring.token && originInfo.group === ring.group) upsert(ring, self);
    // 相机扫码打开的：#pair=配对码
    const m = location.hash.match(/pair=([0-9a-f]+)/);
    if (m) {
      history.replaceState(null, '', location.pathname + location.search);
      setStatus('正在授权这部手机…');
      const r = await requestPair(location.origin, m[1]);
      if (r) adoptToken(r, self);
      else $('auth-detail').textContent = '二维码已过期或已被使用，请在电脑上重新打开二维码，或者输入 6 位数字。';
    }
  }
  if (!ring.token) await migrateLegacy();
  if (ring.token) await refreshGroup();
  if (!currentPc()) {
    const byRecent = groupPcs().sort((a, b) => (b.lastUsed || 0) - (a.lastUsed || 0));
    ring.current = (originInfo && ring.pcs[originInfo.id]) ? originInfo.id : (byRecent[0]?.id || '');
  }
  saveRing(ring);
}

// 用配对码换令牌：返回 { token, group }，失败返回 null
async function requestPair(base, code) {
  try {
    const res = await fetchTimeout(`${base}/api/pair`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ code, name: await deviceName(), phone: ring.phone }),
    }, 5000);
    return res.ok ? await res.json() : null;
  } catch {
    return null;
  }
}

function switchTo(id) {
  if (!ring.token || !ring.pcs[id]) return;
  if (paused) {
    paused = false;
    $('paused').hidden = true;
  }
  ring.current = id;
  saveRing(ring);
  serverState = null;
  renderState(); // 新电脑的显示器信息还没来，先把上一台的清掉
  sheet.hidden = true;
  connect();
}

function showAuth(why) {
  closePc();
  setStatus('');
  if (why !== undefined) $('auth-detail').textContent = why;
  $('auth-others').hidden = groupPcs().length === 0;
  authEl.hidden = false;
}

// ---------------- 连接 ----------------

let pc = null;
let control = null;
let connectGen = 0;            // 每次发起连接加 1；切换电脑时旧的连接立刻作废
let paused = false;            // 手动断开或长时间不操作而暂停；点一下才继续
let lastActivity = Date.now(); // 最近一次操作（触摸、键盘、菜单）
let retryTimer = null;
let serverStats = null;
let serverState = null;
let notice = ''; // 电脑发来的提示（例如没有显示器），收到画面状态后清掉
let pcRemote = false; // 这台电脑的 PocketDesk 在远程桌面会话里运行

function setStatus(text) { statusEl.textContent = text || ''; }

function send(msg) {
  if (msg.t && msg.t !== 'hb') lastActivity = Date.now();
  if (control && control.readyState === 'open') control.send(JSON.stringify(msg));
}

function closePc() {
  releaseInput();
  control = null;
  if (!pc) return;
  const p = pc;
  pc = null;
  try { p.close(); } catch { /* 已关闭 */ }
}

// 让还在进行中的连接作废（手动断开、切到后台时）；它后面的结果不再生效
function cancelConnect() {
  connectGen += 1;
  clearTimeout(retryTimer);
}

function scheduleReconnect(ms) {
  clearTimeout(retryTimer);
  retryTimer = setTimeout(connect, ms);
}

function waitIceGathering(p, ms) {
  if (p.iceGatheringState === 'complete') return Promise.resolve();
  return new Promise((resolve) => {
    const timer = setTimeout(resolve, ms);
    p.addEventListener('icegatheringstatechange', () => {
      if (p.iceGatheringState === 'complete') { clearTimeout(timer); resolve(); }
    });
  });
}

// 显示器选择按电脑分别记
function outputFor(id) { return (prefs.outputs || {})[id] ?? -1; }

function sendSettings() {
  send({ type: 'settings', bitrate: prefs.bitrate, scale: prefs.scale, fps: prefs.fps, output: outputFor(ring.current) });
}

async function startSession(target, stale) {
  const opts = { method: 'POST', headers: authHeader() };
  try {
    return await fetchTimeout(`${baseFor(target)}/api/session`, opts, 4000);
  } catch {
    // 地址可能变了（换了 IP）：试所有已知地址，再问其他电脑
    if (stale()) throw new Error('stale');
    setStatus(`正在查找 ${label(target)}…`);
    if (!await probe(target)) {
      if (stale()) throw new Error('stale');
      await refreshGroup();
      if (stale()) throw new Error('stale');
      if (!await probe(target)) throw new Error(`找不到「${label(target)}」，确认它已开机并运行 PocketDesk`);
    }
    if (stale()) throw new Error('stale');
    saveRing(ring);
    return fetchTimeout(`${baseFor(target)}/api/session`, opts, 4000);
  }
}

// 可以随时再调用：新的一次会让还没完成的旧连接作废（比如上一台电脑关了还在查找中，就切到了别的电脑）
async function connect() {
  if (paused || document.hidden) return;
  const gen = ++connectGen;
  const stale = () => gen !== connectGen;
  clearTimeout(retryTimer);
  // 总的保险：不管卡在哪一步（请求一直没有回应、浏览器内部卡住），20 秒还没连上就从头再来
  setTimeout(() => {
    if (stale() || paused || document.hidden || !authEl.hidden) return;
    if (pc && pc.connectionState === 'connected') return;
    setStatus('连接超时，正在重试…');
    connect();
  }, 20000);
  closePc();
  serverStats = null;
  prevStats = null;
  pcRemote = false;
  const target = currentPc();
  try {
    if (!target) {
      showAuth();
      return;
    }
    authEl.hidden = true;
    setStatus(`正在连接 ${label(target)}…`);
    const res = await startSession(target, stale);
    if (stale()) return;
    if (res.status === 401) {
      ring.token = '';
      saveRing(ring);
      showAuth('电脑上已经移除了这部手机的授权，需要重新授权。');
      return;
    }
    if (!res.ok) throw new Error(`电脑没能创建连接（${res.status}）`);
    target.lastUsed = Date.now();
    saveRing(ring);
    const offer = await res.json();
    if (stale()) return;
    const base = baseFor(target);

    const p = new RTCPeerConnection({ iceServers: [] });
    pc = p;
    p.ontrack = (e) => {
      video.srcObject = e.streams[0] || new MediaStream([e.track]);
      video.play().catch(() => setStatus('点一下屏幕开始播放'));
    };
    p.ondatachannel = (e) => {
      const ch = e.channel;
      ch.onopen = () => {
        if (pc !== p) return;
        control = ch;
        sendSettings();
      };
      if (ch.readyState === 'open') ch.onopen();
      ch.onclose = () => { if (control === ch) control = null; };
      ch.onmessage = (m) => {
        let d;
        try { d = JSON.parse(m.data); } catch { return; }
        if (d.type === 'stats') serverStats = d;
        else if (d.type === 'state') {
          serverState = d;
          renderState();
          if (notice) { notice = ''; setStatus(''); }
        } else if (d.type === 'hello') {
          pcRemote = Boolean(d.remote);
        } else if (d.type === 'notice' && d.reason === 'encoder') {
          notice = d.reason;
          setStatus(`这台电脑没能启动硬件视频编码（${d.encoder || '未知'}），所以没有画面。\n` +
            '可以先更新这台电脑的显卡驱动再试；具体原因在电脑上 PocketDesk 的日志里。');
        } else if (d.type === 'notice' && d.reason === 'no-display') {
          notice = d.reason;
          setStatus('这台电脑现在没有可用的显示器，所以没有画面。\n' +
            '没接显示器的主机：插一个 HDMI 显卡欺骗器（假显示器，只接一根线没有用）；' +
            '或者用远程桌面连着它时也能看到。\n显示器关着的：打开显示器。');
        }
      };
    };
    p.onconnectionstatechange = () => {
      if (pc !== p) return;
      const s = p.connectionState;
      if (s === 'connected') setStatus('');
      else if (s === 'failed' || s === 'disconnected' || s === 'closed') {
        setStatus('连接断开，正在重连…');
        scheduleReconnect(1000);
      }
    };

    await p.setRemoteDescription({ type: 'offer', sdp: offer.sdp });
    await p.setLocalDescription(await p.createAnswer());
    await waitIceGathering(p, 2000);
    if (stale()) return;
    const res2 = await fetchTimeout(`${base}/api/answer`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id: offer.id, sdp: p.localDescription.sdp }),
    }, 5000);
    if (stale()) return;
    if (!res2.ok) throw new Error(`电脑没有接受应答（${res2.status}）`);
    setStatus(`正在建立视频连接（${label(target)}）…`);
    setTimeout(() => {
      if (pc === p && p.connectionState !== 'connected') {
        setStatus('视频连接超时，正在重试…（检查电脑防火墙是否放行 PocketDesk）');
        scheduleReconnect(500);
      }
    }, 10000);
  } catch (err) {
    if (stale()) return; // 已经换了别的连接，旧的失败不用管
    setStatus(`连接失败：${err.message}。3 秒后重试`);
    scheduleReconnect(3000);
  }
}

// ---------------- 画面位置换算 ----------------

// 视频在屏幕上实际显示的区域（object-fit: contain 留出的黑边不算）
function contentRect() {
  const vw = video.videoWidth;
  const vh = video.videoHeight;
  if (!vw || !vh) return null;
  const r = video.getBoundingClientRect();
  const s = Math.min(r.width / vw, r.height / vh);
  const w = vw * s;
  const h = vh * s;
  return { left: r.left + (r.width - w) / 2, top: r.top + (r.height - h) / 2, width: w, height: h };
}

function currentOutput() {
  if (!serverState) return null;
  return serverState.outputs.find((o) => o.index === serverState.output) || null;
}

// 触控板：手指移动 1 个 CSS 像素，光标移动多少个桌面像素
function trackpadGain() {
  const rect = contentRect();
  const out = currentOutput();
  const desktopWidth = out ? out.width : video.videoWidth;
  if (!rect || !desktopWidth) return prefs.sensitivity;
  return (desktopWidth / rect.width) * prefs.sensitivity;
}

// ---------------- 输入 ----------------

const canInput = () => Boolean(control) && sheet.hidden && authEl.hidden;

const trackpad = new Trackpad({ send, gain: trackpadGain, onKeyboard: () => keyboard.toggle() });
const keyboard = new Keyboard({
  compose: $('kb-compose'),
  input: $('kb-input'),
  bar: $('keybar'),
  send,
  onChange: (open) => $('kb-button').classList.toggle('on', open),
});

// 触控模式：手指原样交给 Windows（坐标是画面上的 0..1 比例）
const touchIds = new Set();      // 起点在画面内、已经交给电脑的手指
const pendingMoves = new Map();  // 本帧还没发的移动

function norm(t, rect) {
  return {
    x: Math.min(1, Math.max(0, (t.clientX - rect.left) / rect.width)),
    y: Math.min(1, Math.max(0, (t.clientY - rect.top) / rect.height)),
  };
}

function flushTouchMoves() {
  if (!pendingMoves.size) return;
  send({ t: 'touch', pts: [...pendingMoves.values()] });
  pendingMoves.clear();
}

function onTouch(e) {
  e.preventDefault();
  lastActivity = Date.now();
  if (!canInput()) return;
  if (video.paused) video.play().catch(() => {});
  const rect = contentRect();
  if (!rect) return;
  const kind = e.type;

  if (prefs.mode === 'trackpad') {
    for (const t of e.changedTouches) {
      if (kind === 'touchstart') trackpad.down(t.identifier, t.clientX, t.clientY);
      else if (kind === 'touchmove') trackpad.move(t.identifier, t.clientX, t.clientY);
      else if (kind === 'touchend') trackpad.up(t.identifier, t.clientX, t.clientY);
      else trackpad.cancel();
    }
    return;
  }

  const pts = [];
  for (const t of e.changedTouches) {
    const id = t.identifier;
    if (kind === 'touchstart') {
      // 起点在黑边里的手指不交给电脑，免得触发 Windows 的边缘滑动
      const inside = t.clientX >= rect.left && t.clientX <= rect.left + rect.width &&
        t.clientY >= rect.top && t.clientY <= rect.top + rect.height;
      if (!inside) continue;
      touchIds.add(id);
      pts.push({ id, ...norm(t, rect), s: 'down' });
    } else if (!touchIds.has(id)) {
      continue;
    } else if (kind === 'touchmove') {
      pendingMoves.set(id, { id, ...norm(t, rect), s: 'move' });
    } else {
      pendingMoves.delete(id);
      touchIds.delete(id);
      pts.push({ id, ...norm(t, rect), s: kind === 'touchend' ? 'up' : 'cancel' });
    }
  }
  if (kind !== 'touchmove') flushTouchMoves();
  if (pts.length) send({ t: 'touch', pts });
}

for (const name of ['touchstart', 'touchmove', 'touchend', 'touchcancel']) {
  stage.addEventListener(name, onTouch, { passive: false });
}
for (const name of ['gesturestart', 'gesturechange', 'gestureend', 'dblclick', 'contextmenu']) {
  document.addEventListener(name, (e) => e.preventDefault(), { passive: false });
}

// 每帧把累计的移动发出去
function frame() {
  flushTouchMoves();
  trackpad.flush();
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);

// 有手指按着或按键按着时发心跳；电脑 1.5 秒收不到消息会自动松开
setInterval(() => {
  if (touchIds.size || trackpad.active || trackpad.holding) send({ t: 'hb' });
}, 400);

function releaseInput() {
  if (touchIds.size) {
    send({ t: 'touch', pts: [...touchIds].map((id) => ({ id, x: 0, y: 0, s: 'cancel' })) });
    touchIds.clear();
  }
  pendingMoves.clear();
  trackpad.cancel();
}

window.addEventListener('blur', releaseInput);
// 锁屏或切到别的 App：马上断开，电脑立刻停止编码；回来时自动重连（约 1 秒）
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    cancelConnect();
    closePc();
  } else {
    lastActivity = Date.now();
    if (!paused) connect();
  }
});

// ---------------- 暂停：手动断开，或长时间不操作自动断开 ----------------

function pause(reason) {
  paused = true;
  cancelConnect();
  closePc();
  setStatus('');
  sheet.hidden = true;
  $('paused-reason').textContent = reason;
  $('paused').hidden = false;
}

function resume() {
  paused = false;
  lastActivity = Date.now();
  $('paused').hidden = true;
  connect();
}

$('paused').addEventListener('click', resume);
setInterval(() => {
  const limit = Number(prefs.idleMinutes) * 60 * 1000;
  if (!limit || paused || !pc || document.hidden) return;
  if (Date.now() - lastActivity > limit) {
    pause(`${prefs.idleMinutes} 分钟没有操作，已自动断开，电脑停止传画面。`);
  }
}, 15000);

// ---------------- 菜单 ----------------

const modeHints = {
  touch: '和 Windows 平板一样：点按是单击，长按是右键，拖动、双指缩放和滚动由 Windows 识别。',
  trackpad: '像笔记本触控板：单指移动光标，轻点单击，双指轻点右键，双指移动滚动，长按或点一下再按住可拖动，双指按住不动 0.5 秒后一根不动、另一根移动是右键拖动，三指轻点打开键盘。',
};

function bindSeg(id, get, set) {
  const el = $(id);
  el.addEventListener('click', (e) => {
    const b = e.target.closest('button');
    if (!b) return;
    set(b.dataset.value);
    renderSeg(el, get());
  });
  renderSeg(el, get());
}
function renderSeg(el, value) {
  for (const b of el.querySelectorAll('button')) b.classList.toggle('on', b.dataset.value === String(value));
}

function renderMode() {
  renderSeg($('mode'), prefs.mode);
  $('mode-hint').textContent = modeHints[prefs.mode];
  $('trackpad-settings').hidden = prefs.mode !== 'trackpad';
}

bindSeg('mode', () => prefs.mode, (v) => {
  releaseInput();
  prefs.mode = v;
  savePrefs();
  renderMode();
});
bindSeg('bitrate', () => prefs.bitrate, (v) => {
  prefs.bitrate = v === 'auto' ? 'auto' : Number(v);
  savePrefs();
  sendSettings();
});
bindSeg('scale', () => prefs.scale, (v) => { prefs.scale = Number(v); savePrefs(); sendSettings(); });
bindSeg('fps', () => prefs.fps, (v) => { prefs.fps = Number(v); savePrefs(); sendSettings(); });
$('output').addEventListener('click', (e) => {
  const b = e.target.closest('button');
  if (!b) return;
  prefs.outputs = { ...(prefs.outputs || {}), [ring.current]: Number(b.dataset.value) };
  savePrefs();
  sendSettings();
});

const sens = $('sensitivity');
sens.value = prefs.sensitivity;
const renderSens = () => { $('sens-value').textContent = `${Number(prefs.sensitivity).toFixed(1)}×`; };
sens.addEventListener('input', () => { prefs.sensitivity = Number(sens.value); savePrefs(); renderSens(); });
renderSens();

const showStats = $('show-stats');
showStats.checked = prefs.stats;
statsEl.hidden = !prefs.stats;
showStats.addEventListener('change', () => {
  prefs.stats = showStats.checked;
  statsEl.hidden = !prefs.stats;
  savePrefs();
});

bindSeg('idle', () => prefs.idleMinutes, (v) => { prefs.idleMinutes = Number(v); savePrefs(); });
$('disconnect').addEventListener('click', () => pause('已断开连接，电脑停止传画面。'));

function renderState() {
  const s = serverState;
  const section = $('output-section');
  const el = $('output');
  if (!s) {
    section.hidden = true;
    el.innerHTML = '';
    return;
  }
  section.hidden = s.outputs.length < 2;
  el.innerHTML = '';
  for (const o of s.outputs) {
    const b = document.createElement('button');
    b.type = 'button';
    b.dataset.value = o.index;
    b.textContent = `${o.primary ? '主屏' : `屏幕 ${o.index + 1}`} ${o.width}×${o.height}`;
    el.appendChild(b);
  }
  renderSeg(el, s.output);
}

// ---------------- 键盘和菜单按钮的位置 ----------------
// 有足够宽的黑边就放进黑边，不挡画面；否则贴在屏幕边上，可以拖到别处。

const fab = $('fab');
const safeProbe = document.createElement('div');
safeProbe.style.cssText = 'position:fixed;visibility:hidden;pointer-events:none;' +
  'padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)';
document.body.appendChild(safeProbe);
function safeInsets() {
  const cs = getComputedStyle(safeProbe);
  return { t: parseFloat(cs.paddingTop) || 0, r: parseFloat(cs.paddingRight) || 0,
    b: parseFloat(cs.paddingBottom) || 0, l: parseFloat(cs.paddingLeft) || 0 };
}

const FAB = 36;
const FAB_GAP = 6;
const FAB_PAD = 6;
let fabFloating = true;

// 横屏时顶部让出 iOS 系统手势占用的一条（竖屏时画面上下本来就有黑边）。
// 实测 iPhone 横屏全屏：屏幕最上沿约 20 点收不到触摸，留 24 点后画面第一行能点到。安卓没有这个问题。
const RESERVE_TOP = isIOS ? 24 : 0;
function layoutStage() {
  const landscape = window.innerWidth > window.innerHeight;
  document.documentElement.style.setProperty('--reserve-top', landscape ? `${RESERVE_TOP}px` : '0px');
}

function layoutFab() {
  layoutStage();
  const W = window.innerWidth;
  const H = window.innerHeight;
  const s = safeInsets();
  const r = contentRect();
  const long = FAB * 2 + FAB_GAP;
  let col = true;
  let x;
  let y;
  fabFloating = true;
  if (r) {
    const bottom = H - (r.top + r.height) - s.b;
    const right = W - (r.left + r.width) - s.r;
    if (bottom >= FAB + FAB_PAD * 2) {
      col = false;
      x = W - s.r - FAB_PAD - long;
      y = r.top + r.height + (bottom - FAB) / 2;
      fabFloating = false;
    } else if (right >= FAB + FAB_PAD * 2) {
      x = r.left + r.width + (right - FAB) / 2;
      y = (H - long) / 2;
      fabFloating = false;
    }
  }
  if (fabFloating) {
    const p = prefs.fab || { side: 'right', y: 0.42 };
    x = p.side === 'left' ? s.l + FAB_PAD : W - s.r - FAB_PAD - FAB;
    y = Math.min(Math.max(p.y * H, s.t + FAB_PAD), H - s.b - FAB_PAD - long);
  }
  fab.classList.toggle('col', col);
  fab.classList.toggle('floating', fabFloating);
  fab.style.left = `${x}px`;
  fab.style.top = `${y}px`;
  fab.style.right = 'auto';
}

// 拖动（只在浮动时）：移动超过 8 像素就算拖动，松手后贴到较近的一边
let fabDrag = null;
let fabJustDragged = false;
fab.addEventListener('pointerdown', (e) => {
  if (!fabFloating) return;
  const rect = fab.getBoundingClientRect();
  fabDrag = { id: e.pointerId, sx: e.clientX, sy: e.clientY, left: rect.left, top: rect.top, moved: false };
});
fab.addEventListener('pointermove', (e) => {
  if (!fabDrag || e.pointerId !== fabDrag.id) return;
  const dx = e.clientX - fabDrag.sx;
  const dy = e.clientY - fabDrag.sy;
  if (!fabDrag.moved && Math.hypot(dx, dy) < 8) return;
  if (!fabDrag.moved) {
    fabDrag.moved = true;
    fab.classList.add('dragging');
    try { fab.setPointerCapture(e.pointerId); } catch { /* 忽略 */ }
  }
  fab.style.left = `${fabDrag.left + dx}px`;
  fab.style.top = `${fabDrag.top + dy}px`;
});
function endFabDrag(e) {
  if (!fabDrag || e.pointerId !== fabDrag.id) return;
  if (fabDrag.moved) {
    const rect = fab.getBoundingClientRect();
    prefs.fab = {
      side: rect.left + rect.width / 2 < window.innerWidth / 2 ? 'left' : 'right',
      y: rect.top / window.innerHeight,
    };
    savePrefs();
    fabJustDragged = true;
    setTimeout(() => { fabJustDragged = false; }, 300);
  }
  fab.classList.remove('dragging');
  fabDrag = null;
  layoutFab();
}
fab.addEventListener('pointerup', endFabDrag);
fab.addEventListener('pointercancel', endFabDrag);

window.addEventListener('resize', layoutFab);
window.addEventListener('orientationchange', () => setTimeout(layoutFab, 300));
video.addEventListener('resize', layoutFab);
video.addEventListener('loadedmetadata', layoutFab);
layoutFab();

$('menu-button').addEventListener('click', () => {
  if (fabJustDragged) return;
  lastActivity = Date.now();
  releaseInput();
  keyboard.close();
  renderMode();
  pcListExpanded = false;
  renderPcs();
  sheet.hidden = false;
  refreshGroup();
});
$('sheet-close').addEventListener('click', () => { sheet.hidden = true; });
sheet.addEventListener('click', (e) => { if (e.target === sheet) sheet.hidden = true; });
$('kb-button').addEventListener('click', () => { if (!fabJustDragged) keyboard.toggle(); });

// ---------------- 全屏（安卓 Chrome 等；iPhone 不支持网页全屏，只能添加到主屏幕） ----------------

const fsButton = $('fullscreen');
const standaloneApp = matchMedia('(display-mode: fullscreen), (display-mode: standalone)').matches ||
  navigator.standalone === true;
function renderFullscreen() {
  // iPhone 上网页全屏没用（只能添加到主屏幕）；已经从主屏幕图标打开时也不需要
  fsButton.hidden = !document.fullscreenEnabled || standaloneApp || isIOS;
  fsButton.textContent = document.fullscreenElement ? '退出全屏' : '全屏';
}
fsButton.addEventListener('click', async () => {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else await document.documentElement.requestFullscreen({ navigationUI: 'hide' });
  } catch { /* 浏览器拒绝 */ }
  sheet.hidden = true;
});
document.addEventListener('fullscreenchange', () => { renderFullscreen(); setTimeout(layoutFab, 100); });
renderFullscreen();
// 添加到主屏幕的提示：已经是从主屏幕图标打开的就不占位置
if (standaloneApp || (!isIOS && !isAndroid)) {
  $('home-hint').hidden = true;
} else if (isAndroid) {
  $('home-hint').textContent =
    '全屏使用：点上面的“全屏”；或者在 Chrome 菜单里点“添加到主屏幕”（或“安装应用”），以后从桌面图标打开，自动全屏。';
}

// ---------------- 返回键 / 返回手势（安卓） ----------------
// 安卓的返回手势是从屏幕左右边缘往里划，操作电脑时很容易误触，直接退出页面会断开连接。
// 做法：在历史记录里多放一层，返回时先关掉菜单或键盘；什么都没开时提示“再返回一次退出”。
// Chrome 只承认用户点过屏幕之后放进去的历史记录，所以在触摸结束时补放一层。
let backTrap = false;
let backExitUntil = 0;
const BACK_HINT = '再返回一次退出 PocketDesk';
function armBackTrap() {
  if (backTrap || Date.now() < backExitUntil) return;
  backTrap = true;
  history.pushState({ pdTrap: true }, '', location.href);
}
for (const name of ['pointerup', 'touchend']) document.addEventListener(name, armBackTrap, true);
window.addEventListener('popstate', () => {
  if (!backTrap) return;
  backTrap = false;
  const rearm = () => setTimeout(armBackTrap, 0);
  if (!scanEl.hidden) { stopScan(); rearm(); return; }
  if (!addEl.hidden) { closeAdd(); rearm(); return; }
  if (!sheet.hidden) { sheet.hidden = true; rearm(); return; }
  if (keyboard.isOpen) { keyboard.close(); rearm(); return; }
  backExitUntil = Date.now() + 2000;
  const old = statusEl.textContent === BACK_HINT ? '' : statusEl.textContent;
  setStatus(BACK_HINT);
  setTimeout(() => { if (statusEl.textContent === BACK_HINT) setStatus(old); }, 2000);
});

// ---------------- 远程桌面最小化提示 ----------------
// 电脑上的 PocketDesk 跑在远程桌面会话里时，只要远程桌面窗口被最小化，Windows 就停止绘制这个会话，
// 手机上一片黑。这时提示原因，画面恢复后提示自动消失。

const probeCanvas = document.createElement('canvas');
probeCanvas.width = 16;
probeCanvas.height = 9;
const probeCtx = probeCanvas.getContext('2d', { willReadFrequently: true });

function videoLooksBlack() {
  if (!video.videoWidth) return true; // 一帧都还没有
  try {
    probeCtx.drawImage(video, 0, 0, probeCanvas.width, probeCanvas.height);
    const px = probeCtx.getImageData(0, 0, probeCanvas.width, probeCanvas.height).data;
    for (let i = 0; i < px.length; i += 4) {
      if (px[i] > 12 || px[i + 1] > 12 || px[i + 2] > 12) return false;
    }
    return true;
  } catch {
    return false;
  }
}

const RDP_HINT = '这台电脑正在被远程桌面使用。远程桌面窗口最小化时，Windows 会暂停这个画面，所以是黑的。\n' +
  '把远程桌面窗口还原，或者断开远程桌面，就能看到画面。';
let blackSince = 0;
setInterval(() => {
  const live = pc && pc.connectionState === 'connected' && !paused && !document.hidden;
  const black = live && pcRemote && videoLooksBlack();
  blackSince = black ? (blackSince || Date.now()) : 0;
  if (black && Date.now() - blackSince >= 3000 && notice !== 'rdp') {
    notice = 'rdp';
    setStatus(RDP_HINT);
  } else if (!black && notice === 'rdp') {
    notice = '';
    setStatus('');
  }
}, 1000);

// ---------------- 统计 ----------------

let prevStats = null;
let reportTick = 0;

function fmt(x, digits = 0) {
  return x === null || x === undefined || Number.isNaN(x) ? '—' : x.toFixed(digits);
}

async function updateStats() {
  if (!pc || pc.connectionState !== 'connected') return;
  let report;
  try { report = await pc.getStats(); } catch { return; }
  let inbound = null;
  let pair = null;
  let transport = null;
  report.forEach((s) => {
    if (s.type === 'inbound-rtp' && s.kind === 'video') inbound = s;
    if (s.type === 'transport') transport = s;
  });
  if (transport && transport.selectedCandidatePairId) pair = report.get(transport.selectedCandidatePairId);
  if (!pair) report.forEach((s) => { if (s.type === 'candidate-pair' && s.nominated && s.state === 'succeeded') pair = s; });
  if (!inbound) return;

  const cur = {
    t: performance.now(),
    bytes: inbound.bytesReceived || 0,
    frames: inbound.framesDecoded || 0,
    jbDelay: inbound.jitterBufferDelay,
    jbCount: inbound.jitterBufferEmittedCount,
    decode: inbound.totalDecodeTime,
  };
  if (prevStats) {
    const prev = prevStats;
    const dt = (cur.t - prev.t) / 1000;
    const df = cur.frames - prev.frames;
    const fps = df / dt;
    const kbps = (cur.bytes - prev.bytes) * 8 / 1000 / dt;
    const jb = cur.jbCount > prev.jbCount && cur.jbDelay !== undefined
      ? (cur.jbDelay - prev.jbDelay) / (cur.jbCount - prev.jbCount) * 1000 : null;
    const decode = df > 0 && cur.decode !== undefined ? (cur.decode - prev.decode) / df * 1000 : null;
    const rtt = pair && pair.currentRoundTripTime !== undefined ? pair.currentRoundTripTime * 1000 : null;
    const srv = serverStats || {};
    const est = [srv.captureToSendMs, rtt === null ? null : rtt / 2, jb, decode].reduce((a, b) => a + (b || 0), 0);

    if (!statsEl.hidden) {
      statsEl.textContent = [
        `${inbound.frameWidth || '—'}×${inbound.frameHeight || '—'} ${srv.encoder || ''}  目标 ${srv.bitrateKbps ?? '—'} kbps`,
        `帧率 ${fmt(fps, 1)}（电脑 ${fmt(srv.encodeFps, 1)}）  码率 ${fmt(kbps)} kbps`,
        `延迟≈${fmt(est)} ms：电脑 ${fmt(srv.captureToSendMs, 1)} 网络 ${fmt(rtt === null ? null : rtt / 2, 1)} 缓冲 ${fmt(jb, 1)} 解码 ${fmt(decode, 1)}`,
        `丢包 ${inbound.packetsLost ?? '—'}  重传 ${inbound.nackCount ?? '—'}  关键帧请求 ${inbound.pliCount ?? '—'}  电脑 CPU ${fmt(srv.cpuPercent, 1)}%`,
      ].join('\n');
    }

    // 每 2 秒发给电脑：写日志，也用来自动调码率
    if (++reportTick % 2 === 0) {
      const r = (x, d = 1) => (x === null || x === undefined || Number.isNaN(x) ? null : +x.toFixed(d));
      send({
        type: 'clientStats',
        w: inbound.frameWidth, h: inbound.frameHeight,
        fps: r(fps), kbps: r(kbps, 0), rttMs: r(rtt), jitterBufferMs: r(jb), decodeMs: r(decode), estMs: r(est),
        lost: inbound.packetsLost, received: inbound.packetsReceived,
        nack: inbound.nackCount, pli: inbound.pliCount, dropped: inbound.framesDropped,
        mode: prefs.mode,
      });
    }
  }
  prevStats = cur;
}
setInterval(updateStats, 1000);

// ---------------- 菜单里的电脑列表 ----------------

// 菜单里的电脑列表最多先显示这么多台：当前这台在最前，其余按最近用过排；点“更多”显示全部
const PC_LIST_LIMIT = 3;
let pcListExpanded = false;

function renderPcs() {
  const list = $('pc-list');
  list.innerHTML = '';
  const pcs = groupPcs().sort((a, b) => (b.id === ring.current) - (a.id === ring.current) ||
    (b.online !== false) - (a.online !== false) ||
    (b.lastUsed || 0) - (a.lastUsed || 0) || label(a).localeCompare(label(b), 'zh'));
  const shown = pcListExpanded ? pcs : pcs.slice(0, PC_LIST_LIMIT);
  for (const p of shown) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = `pc${p.id === ring.current ? ' on' : ''}${p.online === false ? ' off' : ''}`;
    b.innerHTML = '<span class="name"></span><span class="addr"></span>';
    b.querySelector('.name').textContent = label(p);
    b.querySelector('.addr').textContent = p.id === ring.current ? '当前'
      : p.online === false ? '不在线' : (p.addr || (p.addrs || [])[0] || '');
    b.addEventListener('click', () => { if (p.id !== ring.current) switchTo(p.id); });
    // 改名：只改这部手机上显示的名字，电脑本身的名称和 IP 不变
    const r = document.createElement('span');
    r.className = 'rename';
    r.textContent = '✎';
    r.setAttribute('role', 'button');
    r.setAttribute('aria-label', `给 ${label(p)} 改名`);
    r.addEventListener('click', (e) => {
      e.stopPropagation();
      const v = prompt(`给这台电脑起个名字（只在这部手机上显示）\n电脑本身的名字：${p.name}`, label(p));
      if (v === null) return;
      const name = v.trim().slice(0, 30);
      if (!name || name === p.name) delete p.alias;
      else p.alias = name;
      saveRing(ring);
      renderPcs();
    });
    b.appendChild(r);
    list.appendChild(b);
  }
  if (pcs.length > PC_LIST_LIMIT) {
    const more = document.createElement('button');
    more.type = 'button';
    more.className = 'pc more';
    more.textContent = pcListExpanded ? '收起' : `更多（还有 ${pcs.length - shown.length} 台）`;
    more.addEventListener('click', () => { pcListExpanded = !pcListExpanded; renderPcs(); });
    list.appendChild(more);
  }
}

// ---------------- 授权：输入 6 位数字，或者扫电脑上的二维码 ----------------

const addEl = $('add');
let addTarget = null; // 要授权的那台电脑 { id, name, ip, https, http, base }

// 输入页面所在电脑的数字；页面所在的电脑连不上时只能扫码
function openCode() {
  sheet.hidden = true;
  authEl.hidden = true;
  addEl.hidden = false;
  $('add-error').textContent = '';
  $('add-digits').value = '';
  if (!originInfo) {
    addTarget = null;
    $('add-form').hidden = true;
    $('add-text').textContent = '连不上打开这个页面的电脑。请用下面的按钮扫组里任意一台电脑上的二维码。';
    return;
  }
  addTarget = { id: originInfo.id, name: originInfo.name, ip: location.hostname, https: originInfo.https,
    http: originInfo.http, ips: originInfo.ips || [], base: location.origin };
  $('add-form').hidden = false;
  $('add-text').textContent = `在 ${originInfo.name} 上点 PocketDesk 托盘图标，输入页面上显示的 6 位数字：`;
  $('add-digits').focus();
}

function closeAdd() {
  addEl.hidden = true;
  stopScan();
  if (!currentPc()) showAuth();
}

async function submitCode(code) {
  const t = addTarget;
  if (!t || !code) return;
  $('add-error').textContent = '正在授权…';
  const r = await requestPair(t.base, code);
  if (!r) {
    $('add-error').textContent = '数字不对或已过期。请看电脑上最新显示的数字再输一次（输错 5 次会换新的）。';
    return;
  }
  adoptToken(r, { id: t.id, name: t.name, https: t.https, http: t.http, addrs: [t.ip, ...t.ips], online: true });
  addEl.hidden = true;
  switchTo(t.id);
  refreshGroup();
}

// 扫到电脑上的二维码：直接用里面的配对码向那台电脑授权
async function pairScanned(q) {
  openCode();
  const base = originInfo && q.id === originInfo.id ? location.origin : baseOf(q.ip, q.https);
  const info = await getInfo(base, 3000);
  if (!info) {
    addTarget = null;
    $('add-form').hidden = true;
    $('add-text').textContent = '';
    $('add-error').textContent = `连不上 ${q.ip}：可能是这部手机还没有信任它的证书。` +
      '请用手机相机扫那个二维码，在浏览器里按提示操作。';
    return;
  }
  addTarget = { id: info.id, name: info.name, ip: q.ip, https: info.https, http: info.http, ips: info.ips || [], base };
  submitCode(q.code);
}

$('add-cancel').addEventListener('click', closeAdd);
$('add-form').addEventListener('submit', (e) => {
  e.preventDefault();
  submitCode($('add-digits').value.replace(/\D/g, ''));
});
$('add-digits').addEventListener('input', (e) => {
  const v = e.target.value.replace(/\D/g, '').slice(0, 6);
  e.target.value = v;
  if (v.length === 6) submitCode(v);
});
$('auth-code').addEventListener('click', openCode);
$('auth-others').addEventListener('click', () => {
  authEl.hidden = true;
  renderPcs();
  sheet.hidden = false;
});

// ---------------- 页面内扫码 ----------------

const scanEl = $('scanner');
const scanVideo = $('scan-video');
let scanStream = null;
let scanTimer = null;

function loadScript(src) {
  return new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = src;
    s.onload = resolve;
    s.onerror = reject;
    document.head.appendChild(s);
  });
}

async function startScan() {
  $('add-error').textContent = '';
  try {
    if (!window.jsQR) await loadScript('vendor/jsQR.js');
    scanStream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: 'environment' }, audio: false });
  } catch {
    $('add-error').textContent = '无法打开摄像头，请直接输入电脑上显示的 6 位数字。';
    return;
  }
  scanEl.hidden = false;
  scanVideo.srcObject = scanStream;
  await scanVideo.play().catch(() => {});
  const canvas = document.createElement('canvas');
  const ctx = canvas.getContext('2d', { willReadFrequently: true });
  scanTimer = setInterval(() => {
    const w = scanVideo.videoWidth;
    const h = scanVideo.videoHeight;
    if (!w || !h) return;
    const scale = Math.min(1, 640 / Math.max(w, h));
    canvas.width = Math.round(w * scale);
    canvas.height = Math.round(h * scale);
    ctx.drawImage(scanVideo, 0, 0, canvas.width, canvas.height);
    const img = ctx.getImageData(0, 0, canvas.width, canvas.height);
    const code = window.jsQR(img.data, img.width, img.height, { inversionAttempts: 'dontInvert' });
    if (!code) return;
    const pair = parsePairUrl(code.data);
    if (!pair) {
      $('scan-hint').textContent = '这不是 PocketDesk 的二维码';
      return;
    }
    stopScan();
    pairScanned(pair);
  }, 200);
}

function stopScan() {
  clearInterval(scanTimer);
  scanTimer = null;
  if (scanStream) for (const t of scanStream.getTracks()) t.stop();
  scanStream = null;
  scanVideo.srcObject = null;
  scanEl.hidden = true;
  $('scan-hint').textContent = '对准电脑上的二维码';
}

for (const id of ['add-scan', 'add-scan-2']) $(id).addEventListener('click', () => {
  if (addEl.hidden) openCode();
  startScan();
});
$('scan-cancel').addEventListener('click', stopScan);

// 调试用：在浏览器控制台里可以直接发消息
window.pocketdesk = { send, prefs, ring };

// ---------------- 启动 ----------------

// 离线缓存：主屏幕图标对应的那台电脑关机时，页面照样能打开，再去连其他电脑
if ('serviceWorker' in navigator) navigator.serviceWorker.register('sw.js').catch(() => {});
// 尽量让浏览器不要清理本页存储（iOS 主屏幕 App 本来就不受 7 天清理限制）
navigator.storage?.persist?.().catch(() => {});

renderMode();
(async () => {
  try { await initRing(); } catch { /* 钥匙串整理失败时按未授权处理 */ }
  connect();
  setInterval(refreshGroup, 20000);
})();
