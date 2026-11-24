// 触控板模式，参考 Moonlight 的相对触控：手机屏幕当作笔记本触控板用。
//   单指移动：移动光标（相对移动，可以抬起手指接着划）
//   单指轻点：左键单击；连点两下：双击
//   轻点后马上按住再移动，或者长按 0.5 秒后移动：按住左键拖动
//   双指轻点：右键单击
//   双指按住 0.5 秒不动，然后一根手指不动、另一根移动：右键拖动（像笔记本：一根手指按着右键，另一根拖），全部抬起松开。
//     按住后两根一起动仍然是滚动（缩放也是两根一起动），按住后直接抬起是右键单击
//   双指移动：滚轮（上下、左右，自然滚动方向）
//   三指轻点：打开或收起键盘

export const TAP_MAX_MS = 250;
export const TAP_SLOP = 10;          // CSS 像素，移动超过这个距离就不算轻点
export const DOUBLE_TAP_MS = 300;
export const DOUBLE_TAP_SLOP = 40;
export const LONG_PRESS_MS = 500;
export const SCROLL_PER_PX = 4;      // 滚轮单位/CSS 像素；120 是一格，约 30 像素

const LEFT = 1;
const RIGHT = 3;

export class Trackpad {
  constructor({ send, onKeyboard = () => {}, gain = () => 1, now = () => performance.now(), timers = globalThis }) {
    this.send = send;
    this.onKeyboard = onKeyboard;
    this.gain = gain;
    this.now = now;
    this.timers = timers;
    this.pts = new Map();
    this.session = null;
    this.lastTap = null;
    this.mx = this.my = this.sx = this.sy = 0;
  }

  get active() { return this.pts.size > 0; }
  get holding() { return Boolean(this.session && this.session.button); }

  down(id, x, y) {
    const t = this.now();
    if (!this.pts.size) {
      // button：正在按住拖动的键（0 = 没有）；armed：双指按住够久了，看第一下怎么动再决定是不是右键拖动
      this.session = { start: t, max: 0, moved: false, button: 0, armed: false, timer: undefined };
      const last = this.lastTap;
      if (last && t - last.time < DOUBLE_TAP_MS && Math.hypot(x - last.x, y - last.y) < DOUBLE_TAP_SLOP) {
        // 轻点后马上又按下：按住左键，接下来移动就是拖动，直接抬起就构成双击
        this.lastTap = null;
        this.press(LEFT);
      } else {
        this.holdAfter(1, () => this.press(LEFT));
      }
    }
    this.pts.set(id, { x, y, sx: x, sy: y });
    const s = this.session;
    s.max = Math.max(s.max, this.pts.size);
    if (this.pts.size > 1) this.clearTimer();
    if (this.pts.size === 2 && !s.moved && !s.button) this.holdAfter(2, () => { s.armed = true; });
  }

  // fingers 根手指按住不动 LONG_PRESS_MS 后执行 action
  holdAfter(fingers, action) {
    const s = this.session;
    s.timer = this.timers.setTimeout(() => {
      s.timer = undefined;
      if (this.session === s && this.pts.size === fingers && !s.moved && !s.button) action();
    }, LONG_PRESS_MS);
  }

  press(button) {
    this.session.button = button;
    this.send({ t: 'btn', b: button, d: true });
  }

  move(id, x, y) {
    const p = this.pts.get(id);
    const s = this.session;
    if (!p || !s) return;
    const dx = x - p.x;
    const dy = y - p.y;
    p.x = x;
    p.y = y;
    const dist = Math.hypot(x - p.sx, y - p.sy);
    if (!s.moved && dist > TAP_SLOP) {
      s.moved = true;
      this.clearTimer();
    }
    // 双指按住后：等这根手指移动够 2 倍阈值再决定（两根一起动时另一根这时也早动了），决定前什么都不发。
    // 另一根手指没动才是右键拖动；两根一起动（滚动、缩放）还是原来的手势
    if (s.armed) {
      if (dist <= TAP_SLOP * 2) return;
      s.armed = false;
      const othersStill = [...this.pts].every(([k, q]) => k === id || Math.hypot(q.x - q.sx, q.y - q.sy) <= TAP_SLOP);
      if (othersStill) this.press(RIGHT);
    }
    if (!s.moved) return;
    // 右键拖动：哪根手指动就跟着哪根（按着不动的那根相当于右键）
    if (s.button === RIGHT || (this.pts.size === 1 && s.max === 1)) {
      const g = this.gain();
      this.mx += dx * g;
      this.my += dy * g;
    } else if (this.pts.size === 2) {
      // 两根手指各自上报移动，取平均
      this.sx += dx * SCROLL_PER_PX / 2;
      this.sy += dy * SCROLL_PER_PX / 2;
    }
  }

  up(id, x, y) {
    if (!this.pts.has(id)) return;
    this.move(id, x, y);
    this.pts.delete(id);
    if (this.pts.size) return;
    const s = this.session;
    this.session = null;
    this.clearTimer(s);
    this.flush();
    const t = this.now();
    const duration = t - s.start;
    if (s.button) {
      this.send({ t: 'btn', b: s.button, d: false });
      return;
    }
    if (s.moved) return;
    if (s.armed) {
      this.click(RIGHT); // 双指按住后没动就抬起：右键单击
      return;
    }
    if (s.max === 1 && duration < TAP_MAX_MS) {
      this.click(LEFT);
      this.lastTap = { time: t, x, y };
    } else if (s.max === 2 && duration < TAP_MAX_MS * 1.6) {
      this.click(RIGHT);
    } else if (s.max === 3 && duration < 500) {
      this.onKeyboard();
    }
  }

  cancel() {
    const s = this.session;
    this.pts.clear();
    this.session = null;
    this.clearTimer(s);
    this.mx = this.my = this.sx = this.sy = 0;
    if (s && s.button) this.send({ t: 'btn', b: s.button, d: false });
  }

  // 每帧调用一次：把累计的移动和滚动发出去（只发整数部分，余数留到下一帧）
  flush() {
    const ix = Math.trunc(this.mx);
    const iy = Math.trunc(this.my);
    if (ix || iy) {
      this.send({ t: 'mrel', dx: ix, dy: iy });
      this.mx -= ix;
      this.my -= iy;
    }
    const wy = Math.trunc(this.sy);
    const wx = Math.trunc(this.sx);
    if (wy || wx) {
      // 自然滚动：手指往上推，内容往上走，相当于滚轮向下（Windows 里是负数）
      this.send({ t: 'wheel', dy: wy, dx: -wx });
      this.sy -= wy;
      this.sx -= wx;
    }
  }

  click(button) {
    this.send({ t: 'btn', b: button, d: true });
    this.send({ t: 'btn', b: button, d: false });
  }

  clearTimer(s = this.session) {
    if (s && s.timer !== undefined) {
      this.timers.clearTimeout(s.timer);
      s.timer = undefined;
    }
  }
}
