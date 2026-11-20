// PocketDesk 手机页面：WebRTC 收画面，触控/触控板/键盘发回电脑。
import { Trackpad } from './trackpad.js';
import { Keyboard } from './keyboard.js';

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
  output: -1,
  stats: false,
  fab: { side: 'right', y: 0.42 }, // 浮动按钮的位置（贴哪一边、纵向位置占屏幕高度的比例）
};
try {
  Object.assign(prefs, JSON.parse(localStorage.getItem('pd-prefs') || '{}'));
} catch { /* 无痕模式等情况下存储不可用 */ }
function savePrefs() {
  try { localStorage.setItem('pd-prefs', JSON.stringify(prefs)); } catch { /* 同上 */ }
}

// 授权令牌。除了存在本机浏览器里，还放在网址的 ?k= 里：
// iPhone 从主屏幕打开的网页 App 和 Safari 存储不互通，但“添加到主屏幕”会保存当时的完整网址，
// 这样添加到主屏幕的图标自带授权，不用再扫一次码。
function getToken() {
  const fromUrl = new URLSearchParams(location.search).get('k');
  let stored = '';
  try { stored = localStorage.getItem('pd-token') || ''; } catch { /* 存储不可用 */ }
  if (fromUrl && fromUrl !== stored) {
    try { localStorage.setItem('pd-token', fromUrl); } catch { /* 同上 */ }
  }
  return fromUrl || stored;
}
function setToken(token) {
  try {
    if (token) localStorage.setItem('pd-token', token);
    else localStorage.removeItem('pd-token');
  } catch { /* 同上 */ }
  showTokenInUrl(token);
}
function showTokenInUrl(token) {
  const url = token ? `${location.pathname}?k=${encodeURIComponent(token)}` : location.pathname;
  if (location.pathname + location.search !== url) history.replaceState(null, '', url);
}

function deviceName() {
  const ua = navigator.userAgent;
  const ios = ua.match(/(iPhone|iPad)[^)]*OS (\d+)_(\d+)/);
  if (ios) return `${ios[1]}（iOS ${ios[2]}.${ios[3]}）`;
  const android = ua.match(/Android (\d+)[^;]*;\s*([^;)]+)/);
  if (android) return `${android[2].trim()}（Android ${android[1]}）`;
  return '浏览器';
}

// ---------------- 连接 ----------------

let pc = null;
let control = null;
let connecting = false;
let retryTimer = null;
let serverStats = null;
let serverState = null;

function setStatus(text) { statusEl.textContent = text || ''; }

function send(msg) {
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

function sendSettings() {
  send({ type: 'settings', bitrate: prefs.bitrate, scale: prefs.scale, fps: prefs.fps, output: prefs.output });
}

async function pairIfNeeded() {
  const m = location.hash.match(/pair=([0-9a-f]+)/);
  if (!m) return;
  history.replaceState(null, '', location.pathname + location.search);
  setStatus('正在授权这部手机…');
  const res = await fetch('/api/pair', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ code: m[1], name: deviceName() }),
  });
  if (res.ok) {
    setToken((await res.json()).token);
  } else {
    $('auth-detail').textContent = '二维码已过期或已被使用，请在电脑上重新打开二维码再扫一次。';
  }
}

async function connect() {
  if (connecting) return;
  connecting = true;
  clearTimeout(retryTimer);
  closePc();
  serverStats = null;
  prevStats = null;
  setStatus('正在连接…');
  try {
    const res = await fetch('/api/session', {
      method: 'POST',
      headers: { Authorization: `Bearer ${getToken()}` },
    });
    if (res.status === 401) {
      setToken('');
      setStatus('');
      authEl.hidden = false;
      return;
    }
    if (!res.ok) throw new Error(`电脑没能创建连接（${res.status}）`);
    authEl.hidden = true;
    showTokenInUrl(getToken());
    const offer = await res.json();

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
        else if (d.type === 'state') { serverState = d; renderState(); }
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
    const res2 = await fetch('/api/answer', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id: offer.id, sdp: p.localDescription.sdp }),
    });
    if (!res2.ok) throw new Error(`电脑没有接受应答（${res2.status}）`);
    setStatus('正在建立视频连接…');
    setTimeout(() => {
      if (pc === p && p.connectionState !== 'connected') {
        setStatus('视频连接超时，正在重试…（检查电脑防火墙是否放行 PocketDesk）');
        scheduleReconnect(500);
      }
    }, 10000);
  } catch (err) {
    setStatus(`连接失败：${err.message}。3 秒后重试`);
    scheduleReconnect(3000);
  } finally {
    connecting = false;
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
document.addEventListener('visibilitychange', () => {
  if (document.hidden) releaseInput();
  else if (!pc || pc.connectionState !== 'connected') connect();
});

// ---------------- 菜单 ----------------

const modeHints = {
  touch: '和 Windows 平板一样：点按是单击，长按是右键，拖动、双指缩放和滚动由 Windows 识别。',
  trackpad: '像笔记本触控板：单指移动光标，轻点单击，双指轻点右键，双指移动滚动，长按或点一下再按住可拖动，三指轻点打开键盘。',
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
  prefs.output = Number(b.dataset.value);
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

function renderState() {
  const s = serverState;
  if (!s) return;
  const section = $('output-section');
  const el = $('output');
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
const probe = document.createElement('div');
probe.style.cssText = 'position:fixed;visibility:hidden;pointer-events:none;' +
  'padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)';
document.body.appendChild(probe);
function safeInsets() {
  const cs = getComputedStyle(probe);
  return { t: parseFloat(cs.paddingTop) || 0, r: parseFloat(cs.paddingRight) || 0,
    b: parseFloat(cs.paddingBottom) || 0, l: parseFloat(cs.paddingLeft) || 0 };
}

const FAB = 36;
const FAB_GAP = 6;
const FAB_PAD = 6;
let fabFloating = true;

// 横屏时顶部让出 iOS 系统手势占用的一条（竖屏时画面上下本来就有黑边）。
// 实测 iPhone 横屏全屏：屏幕最上沿约 20 点收不到触摸，留 24 点后画面第一行能点到。
const RESERVE_TOP = 24;
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
  releaseInput();
  keyboard.close();
  renderMode();
  sheet.hidden = false;
});
$('sheet-close').addEventListener('click', () => { sheet.hidden = true; });
sheet.addEventListener('click', (e) => { if (e.target === sheet) sheet.hidden = true; });
$('kb-button').addEventListener('click', () => { if (!fabJustDragged) keyboard.toggle(); });

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

// 调试用：在浏览器控制台里可以直接发消息
window.pocketdesk = { send, prefs };

// ---------------- 启动 ----------------

renderMode();
(async () => {
  try { await pairIfNeeded(); } catch { /* 配对失败时下面会显示未授权 */ }
  connect();
})();
