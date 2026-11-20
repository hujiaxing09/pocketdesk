'use strict';
// PocketDesk 阶段 0：只看画面，测延迟和帧率。点屏幕显示或隐藏统计。

const video = document.getElementById('screen');
const statusEl = document.getElementById('status');
const statsEl = document.getElementById('stats');
const playBtn = document.getElementById('play');

let pc = null;
let control = null;
let reportTick = 0;
let connecting = false;
let retryTimer = null;
let serverStats = null;
let prev = null;

function setStatus(text) {
  statusEl.textContent = text || '';
}

function closePc() {
  if (!pc) return;
  const p = pc;
  pc = null;
  try { p.close(); } catch (e) { /* 已关闭 */ }
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
      if (p.iceGatheringState === 'complete') {
        clearTimeout(timer);
        resolve();
      }
    });
  });
}

function tryPlay() {
  video.play()
    .then(() => { playBtn.hidden = true; })
    .catch(() => { playBtn.hidden = false; });
}

async function connect() {
  if (connecting) return;
  connecting = true;
  clearTimeout(retryTimer);
  closePc();
  prev = null;
  serverStats = null;
  setStatus('正在连接…');
  try {
    const res = await fetch('/api/session', { method: 'POST' });
    if (!res.ok) throw new Error('电脑没能创建连接（' + res.status + '）');
    const offer = await res.json();

    const p = new RTCPeerConnection({ iceServers: [] });
    pc = p;
    p.ontrack = (e) => {
      video.srcObject = e.streams[0] || new MediaStream([e.track]);
      tryPlay();
    };
    p.ondatachannel = (e) => {
      control = e.channel;
      e.channel.onmessage = (m) => {
        try {
          const d = JSON.parse(m.data);
          if (d.type === 'stats') serverStats = d;
        } catch (err) { /* 忽略 */ }
      };
    };
    p.onconnectionstatechange = () => {
      if (pc !== p) return;
      const s = p.connectionState;
      if (s === 'connected') {
        setStatus('');
      } else if (s === 'failed' || s === 'disconnected' || s === 'closed') {
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
    if (!res2.ok) throw new Error('电脑没有接受应答（' + res2.status + '）');
    setStatus('正在建立视频连接…');
    // 10 秒还没连上就重来
    setTimeout(() => {
      if (pc === p && p.connectionState !== 'connected') {
        setStatus('视频连接超时，正在重试…（检查电脑防火墙是否放行 PocketDesk）');
        scheduleReconnect(500);
      }
    }, 10000);
  } catch (err) {
    setStatus('连接失败：' + err.message + '。3 秒后重试');
    scheduleReconnect(3000);
  } finally {
    connecting = false;
  }
}

function fmt(x, digits = 0) {
  return x === null || x === undefined || Number.isNaN(x) ? '—' : x.toFixed(digits);
}

async function updateStats() {
  if (!pc || pc.connectionState !== 'connected') return;
  let report;
  try { report = await pc.getStats(); } catch (e) { return; }
  let inbound = null;
  let pair = null;
  let transport = null;
  report.forEach((s) => {
    if (s.type === 'inbound-rtp' && s.kind === 'video') inbound = s;
    if (s.type === 'transport') transport = s;
  });
  if (transport && transport.selectedCandidatePairId) pair = report.get(transport.selectedCandidatePairId);
  if (!pair) {
    report.forEach((s) => {
      if (s.type === 'candidate-pair' && s.nominated && s.state === 'succeeded') pair = s;
    });
  }
  if (!inbound) return;

  const cur = {
    t: performance.now(),
    bytes: inbound.bytesReceived || 0,
    frames: inbound.framesDecoded || 0,
    jbDelay: inbound.jitterBufferDelay,
    jbCount: inbound.jitterBufferEmittedCount,
    decode: inbound.totalDecodeTime,
  };
  if (prev) {
    const dt = (cur.t - prev.t) / 1000;
    const df = cur.frames - prev.frames;
    const fps = df / dt;
    const kbps = (cur.bytes - prev.bytes) * 8 / 1000 / dt;
    const jb = cur.jbCount > prev.jbCount && cur.jbDelay !== undefined
      ? (cur.jbDelay - prev.jbDelay) / (cur.jbCount - prev.jbCount) * 1000 : null;
    const decode = df > 0 && cur.decode !== undefined ? (cur.decode - prev.decode) / df * 1000 : null;
    const rtt = pair && pair.currentRoundTripTime !== undefined ? pair.currentRoundTripTime * 1000 : null;
    const srv = serverStats || {};
    const parts = [srv.captureToSendMs, rtt === null ? null : rtt / 2, jb, decode];
    const est = parts.reduce((a, b) => a + (b || 0), 0);

    statsEl.textContent = [
      `画面 ${inbound.frameWidth || '—'}×${inbound.frameHeight || '—'}  编码器 ${srv.encoder || '—'}`,
      `帧率 手机解码 ${fmt(fps, 1)} / 电脑编码 ${fmt(srv.encodeFps, 1)} / 截屏 ${fmt(srv.captureFps, 0)}`,
      `码率 ${fmt(kbps)} kbps`,
      `延迟估算 ≈ ${fmt(est)} ms`,
      `  电脑 ${fmt(srv.captureToSendMs, 1)}（编码 ${fmt(srv.encodeMs, 1)}）+ 网络 ${fmt(rtt === null ? null : rtt / 2, 1)}`,
      `  + 缓冲 ${fmt(jb, 1)} + 解码 ${fmt(decode, 1)}（不含屏幕显示）`,
      `丢包 ${inbound.packetsLost ?? '—'}  重传请求 ${inbound.nackCount ?? '—'}  关键帧请求 ${inbound.pliCount ?? '—'}  丢帧 ${inbound.framesDropped ?? '—'}`,
      `电脑 CPU ${fmt(srv.cpuPercent, 1)}%`,
    ].join('\n');

    // 每 2 秒把手机这边的统计发回电脑，写进电脑日志
    if (control && control.readyState === 'open' && ++reportTick % 2 === 0) {
      const r = (x, d = 1) => (x === null || x === undefined || Number.isNaN(x) ? null : +x.toFixed(d));
      control.send(JSON.stringify({
        type: 'clientStats',
        w: inbound.frameWidth, h: inbound.frameHeight,
        fps: r(fps), kbps: r(kbps, 0), rttMs: r(rtt), jitterBufferMs: r(jb), decodeMs: r(decode),
        estMs: r(est), lost: inbound.packetsLost, nack: inbound.nackCount, pli: inbound.pliCount,
        dropped: inbound.framesDropped, freeze: inbound.freezeCount,
        ua: navigator.userAgent.replace(/^.*?\(([^)]*)\).*$/, '$1'),
      }));
    }
  }
  prev = cur;
}

document.addEventListener('click', (e) => {
  if (e.target === playBtn) return;
  statsEl.classList.toggle('off');
});
playBtn.addEventListener('click', tryPlay);
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible' && (!pc || pc.connectionState !== 'connected')) connect();
});

setInterval(updateStats, 1000);
connect();
