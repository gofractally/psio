import { describe, it } from 'node:test';
import * as assert from 'node:assert/strict';
import {
  u32, u64, str, bool, vec, struct, optional, map,
  type Infer,
} from '../index.js';

// ========================= Map basics =========================

describe('map type', () => {
  const StringToU32 = map(u32);

  it('empty map', () => {
    const packed = StringToU32.pack({});
    const unpacked = StringToU32.unpack(packed);
    assert.deepStrictEqual(unpacked, {});
  });

  it('single entry', () => {
    const val = { alice: 42 };
    const packed = StringToU32.pack(val);
    const unpacked = StringToU32.unpack(packed);
    assert.deepStrictEqual(unpacked, val);
  });

  it('multiple entries', () => {
    const val = { alice: 1, bob: 2, charlie: 3 };
    const packed = StringToU32.pack(val);
    const unpacked = StringToU32.unpack(packed);
    assert.deepStrictEqual(unpacked, val);
  });

  it('map of strings', () => {
    const M = map(str);
    const val = { greeting: 'hello', farewell: 'goodbye' };
    const packed = M.pack(val);
    assert.deepStrictEqual(M.unpack(packed), val);
  });

  it('map of booleans', () => {
    const M = map(bool);
    const val = { active: true, deleted: false };
    const packed = M.pack(val);
    assert.deepStrictEqual(M.unpack(packed), val);
  });

  it('map of optionals', () => {
    const M = map(optional(u32));
    const val = { present: 42, absent: null };
    const packed = M.pack(val);
    assert.deepStrictEqual(M.unpack(packed), val);
  });

  it('map of vecs', () => {
    const M = map(vec(u32));
    const val = { primes: [2, 3, 5], evens: [2, 4, 6] };
    const packed = M.pack(val);
    assert.deepStrictEqual(M.unpack(packed), val);
  });

  it('map of structs', () => {
    const Person = struct({ name: str, age: u32 });
    const M = map(Person);
    const val = {
      alice: { name: 'Alice', age: 30 },
      bob: { name: 'Bob', age: 25 },
    };
    const packed = M.pack(val);
    assert.deepStrictEqual(M.unpack(packed), val);
  });
});

// ========================= Map view =========================

describe('map view', () => {
  const StringToU32 = map(u32);

  it('view returns correct values', () => {
    const val = { x: 10, y: 20, z: 30 };
    const packed = StringToU32.pack(val);
    const viewed = StringToU32.view(packed);
    assert.strictEqual(viewed.x, 10);
    assert.strictEqual(viewed.y, 20);
    assert.strictEqual(viewed.z, 30);
  });

  it('view of empty map', () => {
    const packed = StringToU32.pack({});
    const viewed = StringToU32.view(packed);
    assert.deepStrictEqual(viewed, {});
  });
});

// ========================= Map schema =========================

describe('map schema', () => {
  it('has correct schema node', () => {
    const M = map(u32);
    assert.strictEqual(M.schema.kind, 'map');
    if (M.schema.kind === 'map') {
      assert.strictEqual(M.schema.key.schema.kind, 'string');
      assert.strictEqual(M.schema.value.schema.kind, 'int');
    }
  });

  it('is variable size with fixedSize 4', () => {
    const M = map(u32);
    assert.strictEqual(M.isVariableSize, true);
    assert.strictEqual(M.fixedSize, 4);
  });
});

// ========================= Map in struct =========================

describe('map in struct', () => {
  it('struct containing map', () => {
    const S = struct({
      name: str,
      scores: map(u32),
    });
    const val = { name: 'test', scores: { math: 95, eng: 88 } };
    const packed = S.pack(val);
    assert.deepStrictEqual(S.unpack(packed), val);
  });

  it('map containing nested map values', () => {
    const Inner = map(u32);
    const Outer = struct({ data: Inner });
    const val = { data: { a: 1, b: 2 } };
    const packed = Outer.pack(val);
    assert.deepStrictEqual(Outer.unpack(packed), val);
  });
});

// ========================= Type inference =========================

describe('map type inference', () => {
  it('Infer works on map type', () => {
    const M = map(u32);
    type M = Infer<typeof M>;
    // Compile-time check
    const val: M = { a: 1, b: 2 };
    const packed = M.pack(val);
    const unpacked: M = M.unpack(packed);
    assert.deepStrictEqual(unpacked, val);
  });
});
