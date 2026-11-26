// The two write modes (docs/annotations.md 7.2; gofractally/psio#14):
// `encode` / `encodeKey` / `valueToJson` are the FAST write and hold nothing
// against what a type declares; `encodeChecked` / `encodeKeyChecked` /
// `valueToJsonChecked` are the VALIDATED write and refuse exactly what the
// validating readers refuse.
import test from 'node:test';
import assert from 'node:assert/strict';
import {
  PsszError, boundedList, boundedUtf8, custom, field, list, record, u32,
} from '../index.js';

const Even = custom('Even', u32, { check: (v: number) => (v % 2 === 0 ? undefined : 'Even: odd value') });
const Item = record('Item', { code: boundedUtf8(3), qty: u32 });
const Prop = record('Prop', {
  ids: field(list(u32), { max: 3, sorted: true }),
  name: boundedUtf8(6),
  tags: field(list(boundedUtf8(3)), { max: 2 }),
  items: boundedList(Item, 2),
  even: Even,
});
type PropValue = { ids: number[]; name: string; tags: string[]; items: { code: string; qty: number }[]; even: number };

class Rng {
  constructor(private state: bigint) {}
  next(): bigint {
    this.state = (this.state + 0x9e3779b97f4a7c15n) & 0xffffffffffffffffn;
    let z = this.state;
    z = ((z ^ (z >> 30n)) * 0xbf58476d1ce4e5b9n) & 0xffffffffffffffffn;
    z = ((z ^ (z >> 27n)) * 0x94d049bb133111ebn) & 0xffffffffffffffffn;
    return z ^ (z >> 31n);
  }
  below(n: number): number { return Number(this.next() % BigInt(n)); }
  text(max: number): string {
    let s = '';
    for (let i = this.below(max + 1); i > 0; --i) s += String.fromCharCode(97 + this.below(26));
    return s;
  }
}

function draw(r: Rng): PropValue {
  const ids = Array.from({ length: r.below(6) }, () => r.below(4));
  if (r.below(2)) ids.sort((a, b) => a - b);
  return {
    ids,
    name: r.text(9),
    tags: Array.from({ length: r.below(4) }, () => r.text(5)),
    items: Array.from({ length: r.below(4) }, () => ({ code: r.text(5), qty: r.below(100) })),
    even: r.below(6),
  };
}

const attempt = <T>(run: () => T): T | undefined => {
  try {
    return run();
  } catch (e) {
    assert.ok(e instanceof PsszError, String(e));
    return undefined;
  }
};

test('write modes: the validated write refuses exactly when the validating read of the fast bytes refuses', () => {
  const r = new Rng(1n);
  let valid = 0, invalid = 0;
  for (let i = 0; i < 2000; ++i) {
    const value = draw(r);
    // pssz
    const fast = Prop.encode(value);
    const checked = attempt(() => Prop.encodeChecked(value));
    assert.equal(checked !== undefined, Prop.validate(fast).ok, JSON.stringify(value));
    if (checked) assert.deepEqual(checked, fast);
    // key
    const key = Prop.encodeKey(value);
    const keyChecked = attempt(() => Prop.encodeKeyChecked(value));
    assert.equal(keyChecked !== undefined, Prop.validateKey(key).ok, JSON.stringify(value));
    assert.equal(keyChecked !== undefined, checked !== undefined);
    if (keyChecked) assert.deepEqual(keyChecked, key);
    // json, ijson
    for (const format of ['json', 'ijson'] as const) {
      const text = Prop.valueToJson(value, format);
      const textChecked = attempt(() => Prop.valueToJsonChecked(value, format));
      const read = attempt(() => Prop.jsonToValue(text, format));
      assert.equal(textChecked !== undefined, read !== undefined, JSON.stringify(value));
      assert.equal(textChecked !== undefined, checked !== undefined);
      if (textChecked) assert.equal(textChecked, text);
    }
    if (checked) ++valid; else ++invalid;
  }
  assert.ok(valid > 5 && invalid > 1000, `${valid} valid, ${invalid} invalid`);
});

test('write modes: the fast write still refuses what the format cannot represent', () => {
  // The offsets of Pair are one byte wide (the bounds add up to less than 256).
  const Pair = record('Pair', { a: boundedUtf8(100), b: boundedUtf8(100) });
  assert.equal(Pair.width, 1);
  // The last member ends where the record ends: far over its bound, and a frame.
  const lastLong = { a: 'a', b: 'b'.repeat(300) };
  assert.throws(() => Pair.encodeChecked(lastLong), PsszError);
  assert.equal(Pair.validate(Pair.encode(lastLong)).ok, false);
  // An offset that does not fit: no frame, in either mode.
  const firstLong = { a: 'a'.repeat(300), b: 'b' };
  assert.throws(() => Pair.encodeChecked(firstLong), PsszError);
  assert.throws(() => Pair.encode(firstLong), (e) => e instanceof PsszError && /offset width overflow/.test(e.message));
  // A wrong JavaScript type is not a value in any mode.
  assert.throws(() => Pair.encode({ a: 1 as unknown as string, b: '' }), PsszError);
});
