// 触控板模式，参考 Moonlight 的相对触控：手机屏幕当作笔记本触控板用。
//   单指移动：移动光标（相对移动，可以抬起手指接着划）
//   单指轻点：左键单击；连点两下：双击
//   轻点后马上按住再移动，或者长按 0.5 秒后移动：按住左键拖动
//   双指轻点：右键单击
//   双指移动：滚轮（上下、左右，自然滚动方向）
//   三指轻点：打开或收起键盘
//   屏幕下方的左键、右键按钮按着时（buttonHeld），手指只移动光标，不再当成点击或拖动，像笔记本那样一只手按键一只手拖

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
    this.buttonHeld = false;
  }

  get active() { return this.pts.size > 0; }
  get holding() { return Boolean(this.session && this.session.dragging); }

  down(id, x, y) {
    const t = this.now();
    if (!this.pts.size) {
      this.session = { start: t, max: 0, moved: false, dragging: false, fromDoubleTap: false, timer: undefined,
        moveOnly: this.buttonHeld };
      const last = this.lastTap;
      if (this.session.moveOnly) {
        // 按钮已经按着：这根手指只负责移动
      } else if (last && t - last.time < DOUBLE_TAP_MS && Math.hypot(x - last.x, y - last.y) < DOUBLE_TAP_SLOP) {
        // 轻点后马上又按下：按住左键，接下来移动就是拖动，直接抬起就构成双击
        this.lastTap = null;
        this.session.dragging = true;
        this.session.fromDoubleTap = true;
        this.send({ t: 'btn', b: LEFT, d: true });
      } else {
        const s = this.session;
        s.timer = this.timers.setTimeout(() => {
          s.timer = undefined;
          if (this.session === s && this.pts.size === 1 && !s.moved && !s.dragging) {
            s.dragging = true;
            this.send({ t: 'btn', b: LEFT, d: true });
          }
        }, LONG_PRESS_MS);
      }
    }
    this.pts.set(id, { x, y, sx: x, sy: y });
    const s = this.session;
    s.max = Math.max(s.max, this.pts.size);
    if (this.pts.size > 1) this.clearTimer();
  }

  move(id, x, y) {
    const p = this.pts.get(id);
    const s = this.session;
    if (!p || !s) return;
    const dx = x - p.x;
    const dy = y - p.y;
    p.x = x;
    p.y = y;
    if (!s.moved && Math.hypot(x - p.sx, y - p.sy) > TAP_SLOP) {
      s.moved = true;
      this.clearTimer();
    }
    if (!s.moved) return;
    if (this.pts.size === 1 && s.max === 1) {
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
    if (s.dragging) {
      this.send({ t: 'btn', b: LEFT, d: false });
      return;
    }
    if (s.moved || s.moveOnly) return;
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
    if (s && s.dragging) this.send({ t: 'btn', b: LEFT, d: false });
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
