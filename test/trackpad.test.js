import assert from 'node:assert/strict';
import test from 'node:test';
import { Trackpad, LONG_PRESS_MS } from '../web/trackpad.js';

function harness(gain = 2) {
  let time = 0;
  let timer = null;
  const sent = [];
  let keyboard = 0;
  const pad = new Trackpad({
    send: (m) => sent.push(m),
    onKeyboard: () => { keyboard++; },
    gain: () => gain,
    now: () => time,
    timers: {
      setTimeout: (fn, ms) => { timer = { fn, at: time + ms }; return 1; },
      clearTimeout: () => { timer = null; },
    },
  });
  return {
    pad, sent,
    keyboardCount: () => keyboard,
    advance(ms) {
      time += ms;
      if (timer && time >= timer.at) { const t = timer; timer = null; t.fn(); }
    },
    buttons: () => sent.filter((m) => m.t === 'btn').map((m) => `${m.b}${m.d ? 'v' : '^'}`),
  };
}

test('单指轻点是左键单击', () => {
  const h = harness();
  h.pad.down(1, 100, 100);
  h.advance(80);
  h.pad.up(1, 102, 101);
  assert.deepEqual(h.buttons(), ['1v', '1^']);
});

test('单指移动按增益发送相对移动，余数留到下一帧', () => {
  const h = harness(2);
  h.pad.down(1, 100, 100);
  h.pad.move(1, 115, 100);   // 超过阈值，从这一步开始计入
  h.pad.move(1, 115.75, 99);
  h.pad.flush();
  const moves = h.sent.filter((m) => m.t === 'mrel');
  assert.deepEqual(moves, [{ t: 'mrel', dx: 31, dy: -2 }]);
  h.pad.up(1, 115.75, 99);
  assert.deepEqual(h.buttons(), []); // 移动过就不算点击
});

test('双指轻点是右键，三指轻点打开键盘', () => {
  const h = harness();
  h.pad.down(1, 100, 100);
  h.pad.down(2, 140, 100);
  h.advance(100);
  h.pad.up(1, 100, 100);
  h.pad.up(2, 140, 100);
  assert.deepEqual(h.buttons(), ['3v', '3^']);

  h.pad.down(1, 100, 100);
  h.pad.down(2, 140, 100);
  h.pad.down(3, 180, 100);
  h.advance(150);
  h.pad.up(1, 100, 100);
  h.pad.up(2, 140, 100);
  h.pad.up(3, 180, 100);
  assert.equal(h.keyboardCount(), 1);
});

test('双指移动是自然方向的滚轮', () => {
  const h = harness();
  h.pad.down(1, 100, 200);
  h.pad.down(2, 140, 200);
  // 两根手指一起往上推 30 像素：内容往上走 = 滚轮向下（负数）
  h.pad.move(1, 100, 170);
  h.pad.move(2, 140, 170);
  h.pad.flush();
  const wheels = h.sent.filter((m) => m.t === 'wheel');
  assert.equal(wheels.length, 1);
  assert.ok(wheels[0].dy < 0);
  h.pad.up(1, 100, 170);
  h.pad.up(2, 140, 170);
  assert.deepEqual(h.buttons(), []);
});

test('长按后移动是拖动', () => {
  const h = harness();
  h.pad.down(1, 100, 100);
  h.advance(LONG_PRESS_MS + 10);
  assert.deepEqual(h.buttons(), ['1v']);
  h.pad.move(1, 160, 100);
  h.pad.flush();
  h.pad.up(1, 160, 100);
  assert.deepEqual(h.buttons(), ['1v', '1^']);
  assert.ok(h.sent.some((m) => m.t === 'mrel'));
});

test('轻点后马上按住移动是拖动；直接抬起是双击', () => {
  const h = harness();
  h.pad.down(1, 100, 100);
  h.advance(60);
  h.pad.up(1, 100, 100);
  h.advance(120);
  h.pad.down(1, 103, 102);
  h.advance(60);
  h.pad.up(1, 103, 102);
  assert.deepEqual(h.buttons(), ['1v', '1^', '1v', '1^']);

  h.advance(1000);
  h.sent.length = 0;
  h.pad.down(1, 100, 100);
  h.advance(60);
  h.pad.up(1, 100, 100);
  h.advance(100);
  h.pad.down(1, 100, 100);
  h.pad.move(1, 100, 180);
  h.pad.flush();
  h.pad.up(1, 100, 180);
  assert.deepEqual(h.buttons(), ['1v', '1^', '1v', '1^']);
  assert.ok(h.sent.some((m) => m.t === 'mrel' && m.dy > 0));
});

test('取消时松开拖动中的左键', () => {
  const h = harness();
  h.pad.down(1, 100, 100);
  h.advance(LONG_PRESS_MS + 10);
  h.pad.cancel();
  assert.deepEqual(h.buttons(), ['1v', '1^']);
});
