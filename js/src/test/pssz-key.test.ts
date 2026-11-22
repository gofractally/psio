// The `key` format (docs/key-spec.md), schema-driven: encodings per node,
// the order / prefix-free properties, composite-key helpers, checked reading.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { PsszError, PsszNaN, PsszSchema, keyPrefixUpperBound, type PsszType, type PsszTypeInput } from '../index.js';

const hex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');
const unhex = (text: string) => Uint8Array.from(Buffer.from(text, 'hex'));
const throwsPssz = (run: () => unknown, what?: RegExp) =>
  assert.throws(run, (e: unknown) => e instanceof PsszError && e.kind !== 'internal' && (!what || what.test(e.message)));

const int = (bits: number, isSigned: boolean): PsszTypeInput => ({ Int: { bits, isSigned } });
const U8 = int(8, false), I8 = int(8, true), U16 = int(16, false), I16 = int(16, true);
const U32 = int(32, false), I32 = int(32, true), U64 = int(64, false), I64 = int(64, true);
const BOOL: PsszTypeInput = { Custom: { type: int(1, false), id: 'bool' } };
const CHAR: PsszTypeInput = { Custom: { type: U8, id: 'char' } };
// Text is "utf8"; "string" is a raw string (any bytes).
const STRING: PsszTypeInput = { Custom: { type: { List: U8 }, id: 'utf8' } };
const RAW: PsszTypeInput = { Custom: { type: { List: U8 }, id: 'string' } };
const BYTES: PsszTypeInput = { Custom: { type: { List: U8 }, id: 'hex' } };
const F32: PsszTypeInput = { Float: { exp: 8, mantissa: 24 } };
const F64: PsszTypeInput = { Float: { exp: 11, mantissa: 53 } };
const opt = (type: PsszTypeInput): PsszTypeInput => ({ Option: type });
const list = (type: PsszTypeInput): PsszTypeInput => ({ List: type });
const tuple = (...types: PsszTypeInput[]): PsszTypeInput => ({ Tuple: types });

const schema = new PsszSchema({
  Account: { Object: { shard: U32, owner: STRING, seq: opt(I64) } },
});
const T = (type: PsszTypeInput | string) => schema.type(type);
const key = (type: PsszTypeInput | string, value: unknown) => hex(T(type).encodeKey(value));

test('key: encodings per node', () => {
  assert.equal(key(BOOL, true), '01');
  assert.equal(key(U8, 200), 'c8');
  assert.equal(key(I8, -128), '00');
  assert.equal(key(U16, 0x0102), '0102');
  assert.equal(key(I16, -5), '7ffb');
  assert.equal(key(I32, -2), '7ffffffe');
  assert.equal(key(U64, 1n), '0000000000000001');
  assert.equal(key(I64, -1n), '7fffffffffffffff');
  assert.equal(key(int(128, false), 1n << 127n), '80' + '00'.repeat(15));
  assert.equal(key(int(128, true), -1n), '7f' + 'ff'.repeat(15));
  assert.equal(key(int(256, false), 1n), '00'.repeat(31) + '01');
  assert.equal(key(CHAR, 0xff), 'ff');
  assert.equal(key(F64, 0), '8000000000000000');
  assert.equal(key(F64, -0), '8000000000000000'); // -0 is +0
  assert.equal(key(F64, 1), 'bff0000000000000');
  assert.equal(key(F64, -1), '400fffffffffffff');
  assert.equal(key(F32, 1.5), 'bfc00000');
  assert.equal(key(F64, Infinity), 'fff0000000000000');
  assert.equal(key(F64, -Infinity), '000fffffffffffff');
  assert.equal(key(STRING, ''), '0000');
  assert.equal(key(STRING, 'a\0b'), '610001620000');
  assert.equal(key(STRING, 'é'), 'c3a90000');
  assert.equal(key(BYTES, Uint8Array.of(0x61, 0, 0xff)), '610001ff0000');
  assert.equal(key(list(U8), [0x61, 0]), '6100010000');
  assert.equal(key(list(I8), [-128, 0, 127]), '000180ff0000');
  assert.equal(key(list(CHAR), [0x61]), '610000');
  assert.equal(key(list(U16), [1, 2]), '01000101000200');
  assert.equal(key(list(BOOL), [true, false]), '0101010000');
  assert.equal(key({ BoundedList: { type: U16, maxCount: 2 } }, [1, 2]), '01000101000200');
  assert.equal(key({ Array: { type: U16, len: 2 } }, [1, 2]), '00010002');
  assert.equal(key({ Custom: { type: { Array: { type: U8, len: 3 } }, id: 'hex' } }, Uint8Array.of(0, 0xff, 7)), '00ff07');
  assert.equal(key(opt(STRING), null), '00');
  assert.equal(key(opt(STRING), ''), '010000');
  assert.equal(key(opt(STRING), '\0'), '0100010000');
  assert.equal(key(opt(opt(U8)), null), '00');
  assert.equal(key(opt(opt(U8)), { some: null }), '0100');
  assert.equal(key(opt(opt(U8)), { some: 0 }), '010100');
  assert.equal(key(tuple(), []), '');
  assert.equal(key(tuple(U16, STRING, I32, opt(BOOL)), [0x0102, 'a\0b', -2, true]), '0102610001620000' + '7ffffffe0101');
  assert.equal(key(tuple(tuple(U8, tuple()), STRING), [[1, []], 'x']), '01780000');
  assert.equal(key('Account', { shard: 7, owner: 'bob', seq: -1n }), key(tuple(U32, STRING, opt(I64)), [7, 'bob', -1n]));
  assert.equal(key('Account', { shard: 7, owner: 'bob' }), '00000007626f62000000'); // absent optional
  const V: PsszTypeInput = { Variant: { n: U8, s: STRING } };
  assert.equal(key(V, { type: 'n', value: 9 }), '0009');
  assert.equal(key(V, { type: 's', value: 'a' }), '01610000');
  const BV: PsszTypeInput = { Custom: { type: { Array: { type: int(1, false), len: 12 } }, id: 'bitvector' } };
  assert.equal(key(BV, { bytes: Uint8Array.of(0xff, 0x0f), bitCount: 12 }), 'ff0f');

  // No key form: clear schema errors.
  const BL: PsszTypeInput = { Custom: { type: { BoundedList: { type: int(1, false), maxCount: 8 } }, id: 'bitlist' } };
  throwsPssz(() => T(BL).encodeKey({ bytes: Uint8Array.of(1), bitCount: 1 }), /bitlist has no key encoding/);
  throwsPssz(() => T(BL).decodeKey(Uint8Array.of(1)), /bitlist has no key encoding/);
  throwsPssz(() => T(tuple(U8, BL)).decodeKey(Uint8Array.of(1, 1)), /bitlist/);

  // Values that are not of the type.
  throwsPssz(() => T(U8).encodeKey(256));
  throwsPssz(() => T(I8).encodeKey(-129));
  throwsPssz(() => T(U64).encodeKey(-1n));
  throwsPssz(() => T(U8).encodeKey('1'));
  throwsPssz(() => T(STRING).encodeKey('\ud800'));
  throwsPssz(() => T(list(I8)).encodeKey([128]));
  throwsPssz(() => T({ BoundedList: { type: U16, maxCount: 2 } }).encodeKey([1, 2, 3]), /bound/);
  throwsPssz(() => T({ Array: { type: U16, len: 2 } }).encodeKey([1]));
  throwsPssz(() => T(tuple(U8, U8)).encodeKey([1]));
  throwsPssz(() => T(V).encodeKey({ type: 'zzz', value: 1 }));
  throwsPssz(() => T(BV).encodeKey({ bytes: Uint8Array.of(0xff, 0x1f), bitCount: 12 }));
  const cyclic: unknown[] = [];
  cyclic.push(cyclic);
  throwsPssz(() => schema.type({ List: 'Self' }));
});

test('key: floats — zero and NaN', () => {
  const f64 = T(F64), f32 = T(F32);
  assert.ok(Object.is(f64.decodeKey(f64.encodeKey(-0)), 0));
  throwsPssz(() => f64.decodeKey(unhex('7fffffffffffffff')), /negative zero/);
  throwsPssz(() => f32.decodeKey(unhex('7fffffff')), /negative zero/);
  // NaN has a place: sign clear above +inf, sign set below -inf; payloads kept.
  assert.equal(hex(f64.encodeKey(NaN)), 'fff8000000000000');
  assert.ok(Number.isNaN(f64.decodeKey(unhex('fff8000000000000'))));
  const negative = new PsszNaN(0xfff8000000000001n, 8);
  assert.equal(hex(f64.encodeKey(negative)), '0007fffffffffffe');
  assert.deepEqual(f64.decodeKey(unhex('0007fffffffffffe')), negative);
  assert.ok(Buffer.compare(f64.encodeKey(Infinity), f64.encodeKey(NaN)) < 0);
  assert.ok(Buffer.compare(f64.encodeKey(negative), f64.encodeKey(-Infinity)) < 0);
  const payload = new PsszNaN(0x7fc00001n, 4);
  assert.deepEqual(f32.decodeKey(f32.encodeKey(payload)), payload);
  throwsPssz(() => f64.encodeKey(payload)); // wrong width
});

test('key: bounds and UTF-8 are checked on read', () => {
  const S4: PsszTypeInput = { Custom: { type: { BoundedList: { type: U8, maxCount: 4 } }, id: 'utf8' } };
  assert.equal(T(S4).decodeKey(unhex('616263640000')), 'abcd');
  throwsPssz(() => T(S4).decodeKey(unhex('61626364650000')), /bound/);
  throwsPssz(() => T(S4).encodeKey('abcde'), /bound/);
  throwsPssz(() => T(STRING).decodeKey(unhex('ff0000')), /UTF-8/);
  assert.deepEqual(T(BYTES).decodeKey(unhex('ff0000')), Uint8Array.of(0xff));
  const B2: PsszTypeInput = { BoundedList: { type: U16, maxCount: 2 } };
  assert.deepEqual(T(B2).decodeKey(unhex('01000101000200')), [1, 2]);
  throwsPssz(() => T(B2).decodeKey(unhex('01000101000201000300')), /bound/);
  const O2: PsszTypeInput = { BoundedList: { type: I8, maxCount: 2 } };
  assert.deepEqual(T(O2).decodeKey(unhex('000180' + '0000')), [-128, 0]);
  throwsPssz(() => T(O2).decodeKey(unhex('8081820000')), /bound/);
  const bounded = new PsszSchema({
    R: { Object: { members: [{ name: 's', type: STRING, attributes: [{ name: 'lengthBound', value: 'max=2' }] }] } },
  }).type('R');
  assert.deepEqual(bounded.decodeKey(unhex('61620000')), { s: 'ab' });
  throwsPssz(() => bounded.decodeKey(unhex('6162630000')));
  throwsPssz(() => bounded.encodeKey({ s: 'abc' }));
});

test('key: reading is checked', () => {
  const K = T(tuple(U16, STRING, opt(BOOL), list(U16)));
  const good = '0001' + '61620000' + '0101' + '010005' + '00';
  assert.equal(hex(K.encodeKey([1, 'ab', true, [5]])), good);
  assert.deepEqual(K.decodeKey(unhex(good)), [1, 'ab', true, [5]]);
  assert.deepEqual(K.validateKey(unhex(good)), { ok: true });
  for (let n = 0; n < good.length / 2; ++n) {
    throwsPssz(() => K.decodeKey(unhex(good).subarray(0, n)));
    assert.equal(K.validateKey(unhex(good).subarray(0, n)).ok, false);
  }
  for (const bad of [
    good + '00', // trailing
    '0001' + '61620002' + '0101' + '010005' + '00', // 00 02: no such escape
    '0001' + '616200', // unterminated
    '0001' + '61620000' + '02', // optional marker
    '0001' + '61620000' + '0102' + '00', // bool
    '0001' + '61620000' + '0101' + '020005' + '00', // list marker
    '0001' + '61620000' + '0101' + '0100', // truncated element
  ])
    throwsPssz(() => K.decodeKey(unhex(bad)));
  throwsPssz(() => T(BOOL).decodeKey(unhex('02')));
  throwsPssz(() => T(U32).decodeKey(unhex('000000')));
  throwsPssz(() => T(U32).decodeKey(unhex('0000000000')));
  throwsPssz(() => T({ Variant: { a: U8, b: BOOL } }).decodeKey(unhex('0200')));
  throwsPssz(() => T(tuple()).decodeKey(unhex('00')));
  const BV: PsszTypeInput = { Custom: { type: { Array: { type: int(1, false), len: 4 } }, id: 'bitvector' } };
  throwsPssz(() => T(BV).decodeKey(unhex('10')), /padding/);
  assert.deepEqual(T(BV).decodeKey(unhex('0f')), { bytes: Uint8Array.of(0x0f), bitCount: 4 });
  throwsPssz(() => K.decodeKey('00' as unknown as Uint8Array));
  // A Buffer (a Uint8Array subclass) is accepted; decoded bytes are copies.
  const buf = Buffer.from('ff0000', 'hex');
  const out = T(BYTES).decodeKey(buf) as Uint8Array;
  assert.equal(out.constructor, Uint8Array);
  buf[0] = 0;
  assert.equal(out[0], 0xff);
  // Deep nesting is an error, not a stack overflow.
  let deep: PsszTypeInput = U8;
  for (let i = 0; i < 100; ++i) deep = opt(deep);
  throwsPssz(() => T(deep).decodeKey(new Uint8Array(101).fill(1)), /depth/);
  // A huge array of empty elements does not spin.
  throwsPssz(() => T({ Array: { type: tuple(), len: 2n ** 40n } }).decodeKey(new Uint8Array(0)));
});

// ── Properties over random values ───────────────────────────────────────────

let seed = 0x9e3779b97f4a7c15n;
function rnd(n: number): number {
  seed ^= seed << 13n; seed &= (1n << 64n) - 1n; seed ^= seed >> 7n; seed ^= seed << 17n; seed &= (1n << 64n) - 1n;
  return Number(seed % BigInt(n));
}
const pick = <X>(xs: X[]): X => xs[rnd(xs.length)];
type Gen = () => unknown;
const gU8: Gen = () => pick([0, 1, 2, 0x7f, 0x80, 0xfe, 0xff]);
const gI8: Gen = () => pick([-128, -127, -1, 0, 1, 126, 127]);
const gI16: Gen = () => pick([-32768, -1, 0, 1, 255, 256, 32767]);
const gU32: Gen = () => pick([0, 1, 255, 256, 0x7fffffff, 0x80000000, 0xffffffff]);
const gI64: Gen = () => pick([-(1n << 63n), -1n, 0n, 1n, (1n << 63n) - 1n, BigInt(rnd(1000)) - 500n]);
const gF64: Gen = () => pick([0, 1, -1, 0.5, -0.5, 1e300, -1e300, 5e-324, -5e-324, Infinity, -Infinity, rnd(1000) / 7 - 70]);
// Code-point order equals UTF-8 byte order, so strings are compared by code point.
const gStr: Gen = () => Array.from({ length: rnd(4) }, () => pick(['\0', '\x01', 'a', 'b', '\x7f', '\u0080', 'ÿ', '￿', '\u{10000}'])).join('');
const gBytes: Gen = () => Uint8Array.from({ length: rnd(4) }, () => gU8() as number);
const gOpt = (g: Gen): Gen => () => (rnd(3) === 0 ? null : g());
const gList = (g: Gen): Gen => () => Array.from({ length: rnd(4) }, g);
const gTuple = (...gs: Gen[]): Gen => () => gs.map((g) => g());

// Value order, as docs/key-spec.md defines it per node.
function compare(a: unknown, b: unknown): number {
  if (a === null || b === null) return a === b ? 0 : a === null ? -1 : 1;
  if (typeof a === 'string' && typeof b === 'string') {
    const x = Array.from(a, (c) => c.codePointAt(0)!), y = Array.from(b, (c) => c.codePointAt(0)!);
    return compare(x, y);
  }
  if (a instanceof Uint8Array && b instanceof Uint8Array) return compare(Array.from(a), Array.from(b));
  if (Array.isArray(a) && Array.isArray(b)) {
    for (let i = 0; i < Math.min(a.length, b.length); ++i) {
      const c = compare(a[i], b[i]);
      if (c) return c;
    }
    return a.length - b.length;
  }
  return (a as number) < (b as number) ? -1 : (a as number) > (b as number) ? 1 : 0;
}

const CASES: [string, PsszTypeInput, Gen][] = [
  ['string,u8', tuple(STRING, U8), gTuple(gStr, gU8)],
  ['string,string', tuple(STRING, STRING), gTuple(gStr, gStr)],
  ['bytes,i8', tuple(BYTES, I8), gTuple(gBytes, gI8)],
  ['list<u8>,u8', tuple(list(U8), U8), gTuple(gList(gU8), gU8)],
  ['list<i8>,i8', tuple(list(I8), I8), gTuple(gList(gI8), gI8)],
  ['list<i16>,i8', tuple(list(I16), I8), gTuple(gList(gI16), gI8)],
  ['list<string>,string', tuple(list(STRING), STRING), gTuple(gList(gStr), gStr)],
  ['list<bytes>,bytes', tuple(list(BYTES), BYTES), gTuple(gList(gBytes), gBytes)],
  ['optional<string>,string', tuple(opt(STRING), STRING), gTuple(gOpt(gStr), gStr)],
  ['optional<u8>,u8', tuple(opt(U8), U8), gTuple(gOpt(gU8), gU8)],
  ['u32,string,optional<i64>', tuple(U32, STRING, opt(I64)), gTuple(gU32, gStr, gOpt(gI64))],
  ['f64,string,i64', tuple(F64, STRING, I64), gTuple(gF64, gStr, gI64)],
  ['list<optional<u8>>,u8', tuple(list(opt(U8)), U8), gTuple(gList(gOpt(gU8)), gU8)],
  ['tuple<string,u8>,string', tuple(tuple(STRING, U8), STRING), gTuple(gTuple(gStr, gU8), gStr)],
  ['array<i16,2>,string', tuple({ Array: { type: I16, len: 2 } }, STRING), gTuple(gTuple(gI16, gI16), gStr)],
  ['time,string', tuple({ Custom: { type: I64, id: 'time-point-micros' } }, STRING), gTuple(gI64, gStr)],
];

function startsWith(k: Uint8Array, p: Uint8Array): boolean {
  return k.length >= p.length && Buffer.compare(k.subarray(0, p.length), p) === 0;
}

test('key: byte order is lexicographic value order; keys are prefix-free', () => {
  for (const [name, input, gen] of CASES) {
    const type = T(input);
    const items = Array.from({ length: 300 }, () => {
      const value = gen();
      const k = type.encodeKey(value);
      assert.deepEqual(type.decodeKey(k), value, name);
      if (k.length) throwsPssz(() => type.decodeKey(k.subarray(0, k.length - 1)));
      return { value, k };
    });
    for (let i = 0; i < items.length; ++i)
      for (let j = 0; j < items.length; j += 1 + ((i + j) % 5)) {
        const a = items[i], b = items[j];
        const byValue = Math.sign(compare(a.value, b.value));
        const byBytes = Math.sign(Buffer.compare(a.k, b.k));
        assert.equal(byBytes, byValue, `${name}: ${hex(a.k)} vs ${hex(b.k)}`);
        if (byBytes !== 0) assert.ok(!startsWith(b.k, a.k), `${name}: prefix-free`);
      }
    const byBytes = [...items].sort((x, y) => Buffer.compare(x.k, y.k)).map((x) => hex(x.k));
    const byValue = [...items].sort((x, y) => compare(x.value, y.value)).map((x) => hex(x.k));
    assert.deepEqual(byBytes, byValue, name);
  }
});

test('key: the key of a leading sub-tuple is a prefix of the full key', () => {
  for (const [name, input, gen] of CASES) {
    const type = T(input);
    const members = (input as { Tuple: PsszTypeInput[] }).Tuple;
    for (let i = 0; i < 100; ++i) {
      const value = gen() as unknown[];
      const full = type.encodeKey(value);
      for (let k = 0; k <= members.length; ++k) {
        const head = value.slice(0, k);
        const prefix = type.encodeKeyPrefix(head);
        assert.deepEqual(prefix, T(tuple(...members.slice(0, k))).encodeKey(head), name);
        assert.ok(startsWith(full, prefix), name);
        assert.ok(Buffer.compare(prefix, full) <= 0);
        const bound = keyPrefixUpperBound(prefix);
        if (bound) assert.ok(Buffer.compare(full, bound) < 0, name);
      }
      assert.deepEqual(type.encodeKeyPrefix(value), full);
      throwsPssz(() => type.encodeKeyPrefix([...value, 0]), /more prefix elements/);
    }
  }
  const K = T(tuple(U32, STRING, opt(I64)));
  assert.equal(hex(K.encodeKeyPrefix([])), '');
  assert.equal(hex(K.encodeKeyPrefix([7])), '00000007');
  assert.equal(hex(K.encodeKeyPrefix([7, 'bob'])), '00000007626f620000');
  assert.equal(hex(K.encodeKeyPrefix([7, 'bob', null])), '00000007626f62000000');
  const A = T('Account');
  assert.deepEqual(A.encodeKeyPrefix([7, 'bob']), K.encodeKeyPrefix([7, 'bob']));
  assert.deepEqual(A.encodeKeyPrefix([7, 'bob', 5n]), A.encodeKey({ shard: 7, owner: 'bob', seq: 5n }));
  throwsPssz(() => A.encodeKeyPrefix([7, 'bob', 5n, 1]));
  throwsPssz(() => K.encodeKeyPrefix(['bob'])); // typed per position
  throwsPssz(() => T(U32).encodeKeyPrefix([1]), /tuple or record/);
  throwsPssz(() => K.encodeKeyPrefix(7 as unknown as unknown[]));
});

test('keyPrefixUpperBound: the exclusive end of a prefix scan', () => {
  const bound = (h: string) => {
    const b = keyPrefixUpperBound(unhex(h));
    return b === null ? null : hex(b);
  };
  assert.equal(bound(''), null);
  assert.equal(bound('ff'), null);
  assert.equal(bound('ffffff'), null);
  assert.equal(bound('00'), '01');
  assert.equal(bound('7f'), '80');
  assert.equal(bound('fe'), 'ff');
  assert.equal(bound('1234'), '1235');
  assert.equal(bound('12ff'), '13');
  assert.equal(bound('12ffff'), '13');
  assert.equal(bound('fffe'), 'ffff');
  assert.equal(bound('ff00ff'), 'ff01');
  const input = unhex('12ff');
  keyPrefixUpperBound(input);
  assert.equal(hex(input), '12ff'); // not modified
  assert.equal(keyPrefixUpperBound(Buffer.from('12ff', 'hex'))!.constructor, Uint8Array);
  throwsPssz(() => keyPrefixUpperBound('12' as unknown as Uint8Array));

  // Definition: startsWith(k, p)  <=>  p <= k < bound(p).
  const bytes = (max: number) => Uint8Array.from({ length: rnd(max + 1) }, () => pick([0, 1, 0x7f, 0x80, 0xfe, 0xff, 0xff]));
  for (let i = 0; i < 20000; ++i) {
    const p = bytes(3), k = bytes(5);
    const b = keyPrefixUpperBound(p);
    const inRange = Buffer.compare(p, k) <= 0 && (b === null || Buffer.compare(k, b) < 0);
    assert.equal(startsWith(k, p), inRange, `${hex(p)} / ${hex(k)}`);
    if (b) {
      assert.ok(Buffer.compare(p, b) < 0);
      assert.ok(!startsWith(b, p));
    }
  }
});

test('key: a prefix scan over sorted keys returns exactly the matching rows', () => {
  const K = T(tuple(U32, STRING, opt(I64)));
  const gen = gTuple(() => rnd(3), gStr, gOpt(gI64));
  const rows = Array.from({ length: 1500 }, () => gen() as [number, string, bigint | null]);
  const keys = rows.map((r) => Buffer.from(K.encodeKey(r))).sort(Buffer.compare);
  const scan = (prefix: Uint8Array) => {
    const bound = keyPrefixUpperBound(prefix);
    return keys.filter((k) => Buffer.compare(prefix, k) <= 0 && (!bound || Buffer.compare(k, bound) < 0)).map(hex);
  };
  const expect = (pred: (r: [number, string, bigint | null]) => boolean) =>
    rows.filter(pred).map((r) => Buffer.from(K.encodeKey(r))).sort(Buffer.compare).map(hex);
  assert.equal(scan(K.encodeKeyPrefix([])).length, rows.length);
  for (let shard = 0; shard < 4; ++shard) {
    assert.deepEqual(scan(K.encodeKeyPrefix([shard])), expect((r) => r[0] === shard));
    for (const owner of ['', 'a', '\0', 'ÿ', '\u{10000}', 'a\0'])
      assert.deepEqual(scan(K.encodeKeyPrefix([shard, owner])), expect((r) => r[0] === shard && r[1] === owner));
  }
});

test('key: corrupt bytes yield a PsszError or a value that re-encodes to them', () => {
  const run = (type: PsszType, good: Uint8Array) => {
    let accepted = 0;
    for (let i = 0; i < 4000; ++i) {
      let b = Uint8Array.from(good);
      for (let flips = 1 + rnd(3); flips > 0 && b.length; --flips)
        b[rnd(b.length)] = rnd(4) === 0 ? rnd(256) : rnd(3);
      if (rnd(4) === 0) b = b.subarray(0, rnd(b.length + 1));
      let value: unknown;
      try {
        value = type.decodeKey(b);
      } catch (e) {
        assert.ok(e instanceof PsszError && e.kind === 'bytes', String(e));
        assert.equal(type.validateKey(b).ok, false);
        continue;
      }
      ++accepted;
      assert.equal(hex(type.encodeKey(value)), hex(b));
    }
    assert.ok(accepted > 0);
  };
  run(T(tuple(U16, STRING, opt(BOOL), list(U16))), unhex('0001' + '61620000' + '0101' + '010005' + '00'));
  run(T(tuple(list(I8), opt(opt(U8)), F64, BYTES, { Variant: { n: U8, s: STRING } })),
      T(tuple(list(I8), opt(opt(U8)), F64, BYTES, { Variant: { n: U8, s: STRING } }))
        .encodeKey([[-128, 5], { some: 3 }, 1.5, Uint8Array.of(0, 9), { type: 's', value: 'a\0' }]));
  run(T('Account'), T('Account').encodeKey({ shard: 7, owner: 'bob', seq: 5n }));
});

test('key: a raw string is bytes, a UTF-8 string is text; both share one key form', () => {
  // The same bytes give the same key under either marker.
  assert.equal(key(RAW, 'a\0b'), key(STRING, 'a\0b'));
  assert.equal(key(RAW, Uint8Array.of(0x61, 0, 0x62)), '610001620000');
  // Raw: any bytes, decoded to a Uint8Array, never validated as UTF-8.
  assert.equal(key(RAW, Uint8Array.of(0xff, 0xfe)), 'fffe0000');
  assert.deepEqual(T(RAW).decodeKey(unhex('fffe0000')), Uint8Array.of(0xff, 0xfe));
  assert.deepEqual(T(RAW).decodeKey(unhex('610001620000')), Uint8Array.of(0x61, 0, 0x62));
  // UTF-8: a string; bytes that are not UTF-8 are rejected on read, and a
  // Uint8Array is not a text value.
  assert.equal(T(STRING).decodeKey(unhex('c3a90000')), 'é');
  throwsPssz(() => T(STRING).decodeKey(unhex('fffe0000')), /UTF-8/);
  throwsPssz(() => T(STRING).encodeKey(Uint8Array.of(0x61)));
  throwsPssz(() => T(RAW).encodeKey(7));
  // An enum is its underlying integer; a boxed or custom-presented value is its inner type.
  const E: PsszTypeInput = { Enum: { type: I32, members: { pending: -1, active: 2 } } };
  assert.equal(key(E, 2), key(I32, 2));
  assert.equal(key(E, 7), key(I32, 7));
  assert.equal(T(E).decodeKey(unhex(key(I32, -1))), -1);
  throwsPssz(() => T(E).encodeKey('active'));
  assert.equal(key({ Custom: { type: STRING, id: 'box' } }, 'x'), key(STRING, 'x'));
  assert.equal(key({ Custom: { type: U32, id: 'AccountNumber' } }, 9), key(U32, 9));
});
