import assert from 'node:assert/strict';
import test from 'node:test';
import { encodeRing, decodeRing, decodeLegacyRing, ringFromUrl, loadRing, resetRing, upsert, parsePairUrl, label,
  ensurePhoneId } from '../web/pcs.js';

const emptyRing = () => ({ group: '', groupName: '', token: '', phone: '', current: '', pcs: {}, legacy: [] });

// 1.0.0 的 ?ring= 格式，用来测迁移
function legacyText(list) {
  return Buffer.from(JSON.stringify(list)).toString('base64').replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

test('钥匙串编码进网址再解出来，内容不变（含中文名和别名）', () => {
  const ring = { ...emptyRing(), group: 'g1', token: 'tok' };
  upsert(ring, { id: 'a1', name: '一楼电脑', addr: '192.168.12.53', https: 47443 });
  const b = upsert(ring, { id: 'b2', name: 'zero', addrs: ['192.168.12.49'], https: 47443 });
  b.alias = '书房';
  const text = encodeRing(ring);
  assert.match(text, /^[A-Za-z0-9_-]+$/);
  const back = decodeRing(text);
  assert.equal(back.group, 'g1');
  assert.equal(back.token, 'tok');
  assert.deepEqual(back.pcs.map((p) => [p.id, p.name, p.addrs[0], p.alias]), [
    ['a1', '一楼电脑', '192.168.12.53', undefined],
    ['b2', 'zero', '192.168.12.49', '书房'],
  ]);
});

test('没有令牌时不写进网址；坏掉的网址参数不会抛异常', () => {
  assert.equal(encodeRing(emptyRing()), '');
  assert.equal(decodeRing('!!!'), null);
  assert.deepEqual(decodeLegacyRing('!!!'), []);
});

test('从网址恢复：新格式直接用，1.0.0 的每台电脑令牌放进待迁移列表', () => {
  const src = { ...emptyRing(), group: 'g1', token: 'tok' };
  upsert(src, { id: 'a1', name: 'A', addr: '192.168.12.53', https: 47443 });
  const ring = emptyRing();
  ringFromUrl(ring, `?k=${encodeRing(src)}&p=ffff`);
  assert.equal(ring.token, 'tok');
  assert.equal(ring.pcs.a1.name, 'A');

  const old = emptyRing();
  ringFromUrl(old, `?ring=${legacyText([['a1', 'A', '192.168.12.53', 47443, 'oldtok', '别名']])}`);
  assert.equal(old.token, '');
  assert.deepEqual(old.legacy, [{ id: 'a1', addrs: ['192.168.12.53'], https: 47443, token: 'oldtok' }]);
  assert.equal(old.pcs.a1.token, undefined);
  assert.equal(label(old.pcs.a1), '别名');
});

test('读取 1.0.0 存的钥匙串：每台电脑的令牌移到待迁移列表', () => {
  globalThis.localStorage = {
    getItem: () => JSON.stringify({ pcs: { a1: { id: 'a1', name: 'A', addrs: ['10.0.0.2'], https: 47443, token: 't1' } }, current: 'a1' }),
  };
  try {
    const ring = loadRing();
    assert.equal(ring.token, '');
    assert.deepEqual(ring.legacy, [{ id: 'a1', addrs: ['10.0.0.2'], https: 47443, token: 't1' }]);
    assert.equal(ring.pcs.a1.token, undefined);
  } finally {
    delete globalThis.localStorage;
  }
});

test('换到另一个组：电脑列表清空，手机编号保留', () => {
  const ring = { ...emptyRing(), phone: 'p1', group: 'g1', token: 'old' };
  upsert(ring, { id: 'a1', name: 'A' });
  resetRing(ring, { id: 'g2', name: '公司' }, 'new');
  assert.deepEqual([ring.group, ring.groupName, ring.token, ring.phone], ['g2', '公司', 'new', 'p1']);
  assert.deepEqual(ring.pcs, {});
});

test('合并时地址去重、新的在前，空值不覆盖旧值，电脑名更新不覆盖别名', () => {
  const ring = emptyRing();
  const p = upsert(ring, { id: 'a1', name: 'A', addrs: ['10.0.0.2'], https: 47443 });
  p.alias = '一楼书房';
  const m = upsert(ring, { id: 'a1', name: '', addr: '192.168.1.5', addrs: ['10.0.0.2'] });
  assert.equal(m.name, 'A');
  assert.deepEqual(m.addrs, ['192.168.1.5', '10.0.0.2']);
  upsert(ring, { id: 'a1', name: 'A-NEW' });
  assert.equal(label(ring.pcs.a1), '一楼书房');
  delete ring.pcs.a1.alias;
  assert.equal(label(ring.pcs.a1), 'A-NEW');
});

test('解析二维码网址', () => {
  assert.deepEqual(parsePairUrl('http://192.168.12.53:47080/?pair=0011aabb&id=deadbeef&https=47443'), {
    ip: '192.168.12.53', http: 47080, https: 47443, id: 'deadbeef', code: '0011aabb',
  });
  assert.equal(parsePairUrl('https://example.com/'), null);
  assert.equal(parsePairUrl('不是网址'), null);
});

test('手机编号：只生成一次，网址里带来的优先', () => {
  const a = emptyRing();
  const id = ensurePhoneId(a, '');
  assert.match(id, /^[0-9a-f]{16}$/);
  assert.equal(ensurePhoneId(a, 'ffff'), id);
  const b = emptyRing();
  assert.equal(ensurePhoneId(b, '0123abcd'), '0123abcd');
});
