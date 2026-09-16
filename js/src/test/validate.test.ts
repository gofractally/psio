import { describe, it } from 'node:test';
import * as assert from 'node:assert/strict';
import {
  bool, u8, u16, u32, u64, i32, i64, f32, f64,
  str, bytes, optional, vec, array, tuple, variant,
  struct, fixedStruct, map,
  validate,
} from '../index.js';

// ========================= Valid data =========================

describe('validate valid data', () => {
  it('bool', () => {
    assert.deepStrictEqual(validate(bool, bool.pack(true)), { status: 'valid' });
    assert.deepStrictEqual(validate(bool, bool.pack(false)), { status: 'valid' });
  });

  it('scalars', () => {
    assert.deepStrictEqual(validate(u32, u32.pack(42)), { status: 'valid' });
    assert.deepStrictEqual(validate(u64, u64.pack(123n)), { status: 'valid' });
    assert.deepStrictEqual(validate(i32, i32.pack(-1)), { status: 'valid' });
    assert.deepStrictEqual(validate(f64, f64.pack(Math.PI)), { status: 'valid' });
  });

  it('string', () => {
    assert.deepStrictEqual(validate(str, str.pack('')), { status: 'valid' });
    assert.deepStrictEqual(validate(str, str.pack('hello')), { status: 'valid' });
    assert.deepStrictEqual(validate(str, str.pack('日本語')), { status: 'valid' });
  });

  it('bytes', () => {
    assert.deepStrictEqual(validate(bytes, bytes.pack(new Uint8Array([1, 2, 3]))), { status: 'valid' });
  });

  it('optional none', () => {
    assert.deepStrictEqual(validate(optional(u32), optional(u32).pack(null)), { status: 'valid' });
  });

  it('optional some', () => {
    assert.deepStrictEqual(validate(optional(u32), optional(u32).pack(42)), { status: 'valid' });
  });

  it('vec', () => {
    const v = vec(u32);
    assert.deepStrictEqual(validate(v, v.pack([])), { status: 'valid' });
    assert.deepStrictEqual(validate(v, v.pack([1, 2, 3])), { status: 'valid' });
  });

  it('vec of strings', () => {
    const v = vec(str);
    assert.deepStrictEqual(validate(v, v.pack(['hello', 'world'])), { status: 'valid' });
  });

  it('array', () => {
    const a = array(u32, 3);
    assert.deepStrictEqual(validate(a, a.pack([1, 2, 3])), { status: 'valid' });
  });

  it('tuple', () => {
    const t = tuple(u32, str);
    assert.deepStrictEqual(validate(t, t.pack([42, 'hello'])), { status: 'valid' });
  });

  it('variant', () => {
    const V = variant({ A: u32, B: str });
    assert.deepStrictEqual(validate(V, V.pack({ type: 'A', value: 42 })), { status: 'valid' });
    assert.deepStrictEqual(validate(V, V.pack({ type: 'B', value: 'hi' })), { status: 'valid' });
  });

  it('extensible struct', () => {
    const S = struct({ name: str, age: u32 });
    assert.deepStrictEqual(validate(S, S.pack({ name: 'Alice', age: 30 })), { status: 'valid' });
  });

  it('fixed struct', () => {
    const P = fixedStruct({ x: f64, y: f64 });
    assert.deepStrictEqual(validate(P, P.pack({ x: 1.5, y: 2.5 })), { status: 'valid' });
  });
});

// ========================= Invalid data =========================

describe('validate invalid data', () => {
  it('bool invalid value', () => {
    const result = validate(bool, new Uint8Array([2]));
    assert.strictEqual(result.status, 'invalid');
  });

  it('truncated u32', () => {
    const result = validate(u32, new Uint8Array([1, 0]));
    assert.strictEqual(result.status, 'invalid');
  });

  it('empty buffer for u32', () => {
    const result = validate(u32, new Uint8Array([]));
    assert.strictEqual(result.status, 'invalid');
  });

  it('extra data after u32', () => {
    const result = validate(u32, new Uint8Array([42, 0, 0, 0, 0xFF]));
    assert.strictEqual(result.status, 'invalid');
  });

  it('invalid UTF-8 string', () => {
    // Craft a string with invalid UTF-8 lead byte
    const buf = new Uint8Array([3, 0, 0, 0, 0xFF, 0x00, 0x00]);
    const result = validate(str, buf);
    assert.strictEqual(result.status, 'invalid');
  });

  it('truncated string', () => {
    // Length says 10 but only 2 bytes follow
    const buf = new Uint8Array([10, 0, 0, 0, 0x41, 0x42]);
    const result = validate(str, buf);
    assert.strictEqual(result.status, 'invalid');
  });

  it('variant with out-of-range index', () => {
    const V = variant({ A: u32, B: str });
    // Craft: index=5, size=4, value=0
    const buf = new Uint8Array([5, 4, 0, 0, 0, 0, 0, 0, 0]);
    const result = validate(V, buf);
    assert.strictEqual(result.status, 'invalid');
  });

  it('vec with bad element size alignment', () => {
    // vec<u32>: numBytes=3 is not divisible by 4
    const buf = new Uint8Array([3, 0, 0, 0, 0, 0, 0]);
    const result = validate(vec(u32), buf);
    assert.strictEqual(result.status, 'invalid');
  });
});

// ========================= Extended data =========================

describe('validate extended data', () => {
  it('struct with extra fixed bytes reports extended', () => {
    const S1 = struct({ x: u32 });
    // Pack with a wider schema that has extra fields
    const S2 = struct({ x: u32, y: u32 });
    const packed = S2.pack({ x: 1, y: 2 });
    // Validate against the smaller schema
    const result = validate(S1, packed);
    assert.strictEqual(result.status, 'extended');
  });
});

// ========================= Complex types =========================

describe('validate complex types', () => {
  it('nested struct', () => {
    const Inner = struct({ value: u32 });
    const Outer = struct({ label: str, inner: Inner });
    const packed = Outer.pack({ label: 'hello', inner: { value: 42 } });
    assert.deepStrictEqual(validate(Outer, packed), { status: 'valid' });
  });

  it('struct with optional', () => {
    const S = struct({ name: str, email: optional(str) });
    const packed1 = S.pack({ name: 'Alice', email: 'alice@test.com' });
    assert.deepStrictEqual(validate(S, packed1), { status: 'valid' });
    const packed2 = S.pack({ name: 'Alice', email: null });
    assert.deepStrictEqual(validate(S, packed2), { status: 'valid' });
  });

  it('vec of structs', () => {
    const Person = struct({ name: str, age: u32 });
    const People = vec(Person);
    const packed = People.pack([
      { name: 'Alice', age: 30 },
      { name: 'Bob', age: 25 },
    ]);
    assert.deepStrictEqual(validate(People, packed), { status: 'valid' });
  });
});
