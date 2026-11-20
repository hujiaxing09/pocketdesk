// 离线缓存页面本身（不缓存接口）。优先用网络上的最新版本，连不上这台电脑时用缓存，
// 这样主屏幕图标对应的电脑关机了，页面照样能打开，再去连其他电脑。
const CACHE = 'pocketdesk-shell-v6';
const SHELL = ['./', 'index.html', 'app.js', 'pcs.js', 'trackpad.js', 'keyboard.js', 'style.css',
  'icon.ico', 'icon-192.png', 'icon-256.png', 'icon-512.png', 'manifest.json', 'vendor/jsQR.js'];

self.addEventListener('install', (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', (e) => {
  e.waitUntil(caches.keys()
    .then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
    .then(() => self.clients.claim()));
});

self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);
  if (e.request.method !== 'GET' || url.origin !== location.origin || url.pathname.includes('/api/')) return;
  e.respondWith((async () => {
    try {
      const ctl = new AbortController();
      const timer = setTimeout(() => ctl.abort(), 2500);
      const res = await fetch(e.request, { signal: ctl.signal });
      clearTimeout(timer);
      if (res.ok) {
        const cache = await caches.open(CACHE);
        // 首页带着 ?ring= 参数，缓存时去掉参数
        await cache.put(e.request.mode === 'navigate' ? new Request(url.pathname) : e.request, res.clone());
      }
      return res;
    } catch {
      const hit = await caches.match(e.request, { ignoreSearch: true });
      if (hit) return hit;
      const shell = e.request.mode === 'navigate' ? await caches.match('./', { ignoreSearch: true }) : null;
      return shell || new Response('离线', { status: 503 });
    }
  })());
});
