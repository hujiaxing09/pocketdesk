import assert from 'node:assert/strict';
import test from 'node:test';
import { encodeRing, decodeRing, upsert, parsePairUrl, label, ensurePhoneId } from '../web/pcs.js';

test('钥匙串编码进网址再解出来，内容不变（含中文名）', () => {
  const ring = { pcs: {}, current: '' };
  upsert(ring, { id: 'a1', name: '一楼电脑', addr: '192.168.12.53', https: 47443, token: 'tok1' });
  upsert(ring, { id: 'b2', name: 'zero', addrs: ['192.168.12.49'], https: 47443, token: 'tok2' });
  upsert(ring, { id: 'c3', name: '没授权的', addr: '192.168.12.9', https: 47443 });
  const back = decodeRing(encodeRing(ring));
  assert.deepEqual(back.map((p) => [p.id, p.name, p.addr, p.token]), [
    ['a1', '一楼电脑', '192.168.12.53', 'tok1'],
    ['b2', 'zero', '192.168.12.49', 'tok2'],
  ]);
  assert.match(encodeRing(ring), /^[A-Za-z0-9_-]+$/);
});

test('合并时地址去重、新的在前，空值不覆盖旧值', () => {
  const ring = { pcs: {}, current: '' };
  upsert(ring, { id: 'a1', name: 'A', addrs: ['10.0.0.2'], https: 47443, token: 't' });
  const m = upsert(ring, { id: 'a1', name: '', addr: '192.168.1.5', addrs: ['10.0.0.2'], token: '' });
  assert.equal(m.name, 'A');
  assert.equal(m.token, 't');
  assert.deepEqual(m.addrs, ['192.168.1.5', '10.0.0.2']);
});

test('解析二维码网址', () => {
  assert.deepEqual(parsePairUrl('http://192.168.12.53:47080/?pair=0011aabb&id=deadbeef&https=47443'), {
    ip: '192.168.12.53', http: 47080, https: 47443, id: 'deadbeef', code: '0011aabb',
  });
  assert.equal(parsePairUrl('https://example.com/'), null);
  assert.equal(parsePairUrl('不是网址'), null);
});

test('坏掉的 ?ring= 不会抛异常', () => {
  assert.deepEqual(decodeRing('!!!'), []);
});

test('手机上的别名：写进网址能恢复，电脑名更新时不会覆盖别名', () => {
  const ring = { pcs: {}, current: '' };
  const p = upsert(ring, { id: 'a1', name: 'DESKTOP-7X', addr: '192.168.12.53', https: 47443, token: 't' });
  p.alias = '一楼书房';
  assert.equal(label(p), '一楼书房');
  upsert(ring, { id: 'a1', name: 'DESKTOP-7X-NEW', addrs: ['192.168.12.53'] });
  assert.equal(label(ring.pcs.a1), '一楼书房');
  assert.equal(ring.pcs.a1.name, 'DESKTOP-7X-NEW');
  const back = decodeRing(encodeRing(ring));
  assert.equal(back[0].alias, '一楼书房');
  delete ring.pcs.a1.alias;
  assert.equal(label(ring.pcs.a1), 'DESKTOP-7X-NEW');
});

test('手机编号：只生成一次，网址里带来的优先', () => {
  const a = { pcs: {}, current: '' };
  const id = ensurePhoneId(a, '');
  assert.match(id, /^[0-9a-f]{16}$/);
  assert.equal(ensurePhoneId(a, 'ffff'), id);
  const b = { pcs: {}, current: '' };
  assert.equal(ensurePhoneId(b, '0123abcd'), '0123abcd');
});
