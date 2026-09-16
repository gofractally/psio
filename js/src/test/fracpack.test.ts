import { describe, it } from 'node:test';
import * as assert from 'node:assert/strict';
import {
  bool, u8, u16, u32, u64, i8, i16, i32, i64, f32, f64,
  str, bytes, optional, vec, array, tuple, variant,
  struct, fixedStruct,
  type Infer,
} from '../index.js';

// Helper: pack then unpack, assert round-trip
function roundTrip<T>(type: { pack(v: T): Uint8Array; unpack(d: Uint8Array): T }, value: T): T {
  const packed = type.pack(value);
  return type.unpack(packed);
}

function assertRoundTrip<T>(type: { pack(v: T): Uint8Array; unpack(d: Uint8Array): T }, value: T): void {
  assert.deepStrictEqual(roundTrip(type, value), value);
}

// Helper: check exact bytes
function assertBytes(type: { pack(v: any): Uint8Array }, value: any, expected: number[]): void {
  const packed = type.pack(value);
  assert.deepStrictEqual([...packed], expected);
}

// ========================= Scalars =========================

describe('scalar types', () => {
  it('bool', () => {
    assertRoundTrip(bool, true);
    assertRoundTrip(bool, false);
    assertBytes(bool, true, [1]);
    assertBytes(bool, false, [0]);
  });

  it('u8', () => {
    assertRoundTrip(u8, 0);
    assertRoundTrip(u8, 255);
    assertBytes(u8, 42, [42]);
  });

  it('u16', () => {
    assertRoundTrip(u16, 0);
    assertRoundTrip(u16, 0xFFFF);
    assertBytes(u16, 0x0102, [0x02, 0x01]); // little-endian
  });

  it('u32', () => {
    assertRoundTrip(u32, 0);
    assertRoundTrip(u32, 0xFFFFFFFF);
    assertBytes(u32, 1, [1, 0, 0, 0]);
  });

  it('u64', () => {
    assertRoundTrip(u64, 0n);
    assertRoundTrip(u64, 0xFFFFFFFFFFFFFFFFn);
    assertBytes(u64, 1n, [1, 0, 0, 0, 0, 0, 0, 0]);
  });

  it('i8', () => {
    assertRoundTrip(i8, -128);
    assertRoundTrip(i8, 127);
  });

  it('i16', () => {
    assertRoundTrip(i16, -32768);
    assertRoundTrip(i16, 32767);
  });

  it('i32', () => {
    assertRoundTrip(i32, -2147483648);
    assertRoundTrip(i32, 2147483647);
    assertRoundTrip(i32, -1);
  });

  it('i64', () => {
    assertRoundTrip(i64, -9223372036854775808n);
    assertRoundTrip(i64, 9223372036854775807n);
    assertRoundTrip(i64, -1n);
  });

  it('f32', () => {
    assertRoundTrip(f32, 0);
    assertRoundTrip(f32, 1.5);
    assertRoundTrip(f32, -1.5);
  });

  it('f64', () => {
    assertRoundTrip(f64, 0);
    assertRoundTrip(f64, Math.PI);
    assertRoundTrip(f64, -Infinity);
    assertRoundTrip(f64, Number.MAX_VALUE);
  });
});

// ========================= String =========================

describe('string', () => {
  it('empty', () => {
    assertRoundTrip(str, '');
    assertBytes(str, '', [0, 0, 0, 0]); // u32 length = 0
  });

  it('ascii', () => {
    assertRoundTrip(str, 'hello');
    assertBytes(str, 'hi', [2, 0, 0, 0, 0x68, 0x69]);
  });

  it('unicode', () => {
    assertRoundTrip(str, '日本語');
    assertRoundTrip(str, '🎉');
  });
});

// ========================= Bytes =========================

describe('bytes', () => {
  it('empty', () => {
    assertRoundTrip(bytes, new Uint8Array(0));
  });

  it('data', () => {
    const data = new Uint8Array([1, 2, 3, 4, 5]);
    assertRoundTrip(bytes, data);
  });
});

// ========================= Optional =========================

describe('optional', () => {
  const optU32 = optional(u32);

  it('none', () => {
    assertRoundTrip(optU32, null);
    assertBytes(optU32, null, [1, 0, 0, 0]); // offset = 1 = absent
  });

  it('some', () => {
    assertRoundTrip(optU32, 42);
  });

  it('some(0)', () => {
    assertRoundTrip(optU32, 0);
  });

  it('optional string none', () => {
    const optStr = optional(str);
    assertRoundTrip(optStr, null);
  });

  it('optional string some', () => {
    const optStr = optional(str);
    assertRoundTrip(optStr, 'hello');
  });

  it('optional string empty', () => {
    const optStr = optional(str);
    // Some("") is different from None
    const packed = optStr.pack('');
    const unpacked = optStr.unpack(packed);
    assert.strictEqual(unpacked, '');
  });
});

// ========================= Vec =========================

describe('vec', () => {
  const vecU32 = vec(u32);

  it('empty', () => {
    assertRoundTrip(vecU32, []);
  });

  it('single element', () => {
    assertRoundTrip(vecU32, [42]);
    // [num_bytes=4, value=42]
    assertBytes(vecU32, [42], [4, 0, 0, 0, 42, 0, 0, 0]);
  });

  it('multiple elements', () => {
    assertRoundTrip(vecU32, [1, 2, 3]);
  });

  it('vec of strings', () => {
    const vecStr = vec(str);
    assertRoundTrip(vecStr, []);
    assertRoundTrip(vecStr, ['hello']);
    assertRoundTrip(vecStr, ['hello', 'world', '']);
  });

  it('vec of vec', () => {
    const vv = vec(vec(u32));
    assertRoundTrip(vv, []);
    assertRoundTrip(vv, [[1, 2], [3]]);
  });
});

// ========================= Array =========================

describe('array', () => {
  it('fixed element array', () => {
    const arr3 = array(u32, 3);
    assertRoundTrip(arr3, [1, 2, 3]);
    // 3 * 4 bytes = 12 bytes, no size prefix
    assertBytes(arr3, [1, 2, 3], [1,0,0,0, 2,0,0,0, 3,0,0,0]);
  });

  it('variable element array', () => {
    const arr2str = array(str, 2);
    assertRoundTrip(arr2str, ['hello', 'world']);
  });
});

// ========================= Tuple =========================

describe('tuple', () => {
  it('basic tuple', () => {
    const t = tuple(u32, str);
    assertRoundTrip(t, [42, 'hello']);
  });

  it('empty tuple', () => {
    const t = tuple();
    assertRoundTrip(t, []);
  });

  it('single element', () => {
    const t = tuple(u32);
    assertRoundTrip(t, [42]);
  });

  it('trailing optional elision', () => {
    const t = tuple(u32, optional(str));
    // With value
    assertRoundTrip(t, [42, 'hello']);
    // With null — trailing optional is elided
    assertRoundTrip(t, [42, null]);
  });
});

// ========================= Variant =========================

describe('variant', () => {
  const Shape = variant({
    Circle: f64,
    Rectangle: tuple(f64, f64),
  });

  it('first variant', () => {
    assertRoundTrip(Shape, { type: 'Circle', value: 3.14 });
  });

  it('second variant', () => {
    assertRoundTrip(Shape, { type: 'Rectangle', value: [10, 20] });
  });
});

// ========================= Struct =========================

describe('struct', () => {
  const Point = fixedStruct({ x: f64, y: f64 });

  it('fixed struct', () => {
    assertRoundTrip(Point, { x: 1.5, y: 2.5 });
  });

  it('fixed struct is not variable-size when all fields are fixed', () => {
    assert.strictEqual(Point.isVariableSize, false);
    assert.strictEqual(Point.fixedSize, 16); // 2 * 8
  });

  const Person = struct({
    name: str,
    age: u32,
    active: bool,
  });

  it('extensible struct basic', () => {
    assertRoundTrip(Person, { name: 'Alice', age: 30, active: true });
  });

  it('extensible struct is always variable-size', () => {
    assert.strictEqual(Person.isVariableSize, true);
    assert.strictEqual(Person.fixedSize, 4);
  });

  it('struct with optional trailing field', () => {
    const PersonV2 = struct({
      name: str,
      age: u32,
      active: bool,
      email: optional(str),
    });

    // With email
    assertRoundTrip(PersonV2, { name: 'Alice', age: 30, active: true, email: 'alice@example.com' });
    // Without email — trailing optional elided
    assertRoundTrip(PersonV2, { name: 'Alice', age: 30, active: true, email: null });
  });

  it('struct containing vec', () => {
    const S = struct({
      name: str,
      tags: vec(str),
    });
    assertRoundTrip(S, { name: 'test', tags: ['a', 'b', 'c'] });
    assertRoundTrip(S, { name: 'test', tags: [] });
  });

  it('nested structs', () => {
    const Inner = struct({ value: u32 });
    const Outer = struct({
      label: str,
      inner: Inner,
    });
    assertRoundTrip(Outer, { label: 'hello', inner: { value: 42 } });
  });
});

// ========================= View =========================

describe('view', () => {
  it('scalar view', () => {
    const packed = u32.pack(42);
    assert.strictEqual(u32.view(packed), 42);
  });

  it('string view', () => {
    const packed = str.pack('hello');
    assert.strictEqual(str.view(packed), 'hello');
  });

  it('struct view with lazy getters', () => {
    const Person = struct({
      name: str,
      age: u32,
      active: bool,
    });

    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const view = Person.view(packed);

    assert.strictEqual(view.name, 'Alice');
    assert.strictEqual(view.age, 30);
    assert.strictEqual(view.active, true);
  });

  it('fixed struct view', () => {
    const Point = fixedStruct({ x: f64, y: f64 });
    const packed = Point.pack({ x: 1.5, y: 2.5 });
    const view = Point.view(packed);
    assert.strictEqual(view.x, 1.5);
    assert.strictEqual(view.y, 2.5);
  });

  it('vec view', () => {
    const v = vec(u32);
    const packed = v.pack([1, 2, 3]);
    const viewed = v.view(packed);
    // Scalar vecs return TypedArray views (zero-copy)
    assert.strictEqual(viewed.length, 3);
    assert.strictEqual(viewed[0], 1);
    assert.strictEqual(viewed[1], 2);
    assert.strictEqual(viewed[2], 3);
  });

  it('optional view', () => {
    const opt = optional(u32);
    assert.strictEqual(opt.view(opt.pack(null)), null);
    assert.strictEqual(opt.view(opt.pack(42)), 42);
  });
});

// ========================= Type inference =========================

describe('type inference', () => {
  it('Infer extracts correct types', () => {
    const S = struct({
      name: str,
      age: u32,
      tags: vec(str),
      bio: optional(str),
    });

    type S = Infer<typeof S>;

    // This is a compile-time check — if it compiles, the types are correct
    const val: S = { name: 'test', age: 42, tags: ['a'], bio: null };
    assertRoundTrip(S, val);
  });
});

// ========================= Cross-language byte patterns =========================

describe('cross-language compatibility', () => {
  it('u32 matches known encoding', () => {
    assertBytes(u32, 0x01020304, [0x04, 0x03, 0x02, 0x01]);
  });

  it('bool matches known encoding', () => {
    assertBytes(bool, true, [1]);
    assertBytes(bool, false, [0]);
  });

  it('string matches known encoding', () => {
    // "hi" = [len=2, 'h', 'i']
    assertBytes(str, 'hi', [2, 0, 0, 0, 0x68, 0x69]);
  });

  it('vec<u32> matches known encoding', () => {
    // [1, 2] = [num_bytes=8, 1, 0, 0, 0, 2, 0, 0, 0]
    assertBytes(vec(u32), [1, 2], [8, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0]);
  });

  it('optional<u32> None matches', () => {
    assertBytes(optional(u32), null, [1, 0, 0, 0]);
  });

  it('fixed struct Point matches', () => {
    const Point = fixedStruct({ x: u32, y: u32 });
    // {x:1, y:2} = [1,0,0,0, 2,0,0,0]
    assertBytes(Point, { x: 1, y: 2 }, [1, 0, 0, 0, 2, 0, 0, 0]);
  });

  it('extensible struct with u16 header', () => {
    const S = struct({ x: u32 });
    const packed = S.pack({ x: 42 });
    // u16 fixed_size=4, then u32 value=42
    assert.deepStrictEqual([...packed], [4, 0, 42, 0, 0, 0]);
  });
});
