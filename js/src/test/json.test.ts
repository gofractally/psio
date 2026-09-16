import { describe, it } from 'node:test';
import * as assert from 'node:assert/strict';
import {
  bool, u8, u16, u32, u64, i32, i64, f32, f64,
  str, bytes, optional, vec, array, tuple, variant,
  struct, fixedStruct, map,
  fracToJson, jsonToFrac, valueToJson, jsonToValue,
  toHex, fromHex,
} from '../index.js';

// ========================= Hex encoding =========================

describe('hex encoding', () => {
  it('toHex', () => {
    assert.strictEqual(toHex(new Uint8Array([])), '');
    assert.strictEqual(toHex(new Uint8Array([0x00])), '00');
    assert.strictEqual(toHex(new Uint8Array([0xff])), 'ff');
    assert.strictEqual(toHex(new Uint8Array([0xde, 0xad, 0xbe, 0xef])), 'deadbeef');
  });

  it('fromHex', () => {
    assert.deepStrictEqual(fromHex(''), new Uint8Array([]));
    assert.deepStrictEqual(fromHex('00'), new Uint8Array([0x00]));
    assert.deepStrictEqual(fromHex('deadbeef'), new Uint8Array([0xde, 0xad, 0xbe, 0xef]));
    assert.deepStrictEqual(fromHex('DEADBEEF'), new Uint8Array([0xde, 0xad, 0xbe, 0xef]));
  });

  it('fromHex rejects odd length', () => {
    assert.throws(() => fromHex('abc'), /even length/);
  });

  it('fromHex rejects invalid chars', () => {
    assert.throws(() => fromHex('zz'), /Invalid hex/);
  });

  it('round-trip', () => {
    const data = new Uint8Array([1, 2, 3, 255, 0, 128]);
    assert.deepStrictEqual(fromHex(toHex(data)), data);
  });
});

// ========================= Scalars to JSON =========================

describe('scalar to JSON', () => {
  it('bool', () => {
    assert.strictEqual(valueToJson(bool, true), true);
    assert.strictEqual(valueToJson(bool, false), false);
  });

  it('u32', () => {
    assert.strictEqual(valueToJson(u32, 42), 42);
  });

  it('i32', () => {
    assert.strictEqual(valueToJson(i32, -1), -1);
  });

  it('u64 → string', () => {
    assert.strictEqual(valueToJson(u64, 123456789012345678n), '123456789012345678');
  });

  it('i64 → string', () => {
    assert.strictEqual(valueToJson(i64, -9223372036854775808n), '-9223372036854775808');
  });

  it('f64', () => {
    assert.strictEqual(valueToJson(f64, Math.PI), Math.PI);
  });

  it('string', () => {
    assert.strictEqual(valueToJson(str, 'hello'), 'hello');
  });

  it('bytes → hex', () => {
    assert.strictEqual(valueToJson(bytes, new Uint8Array([0xde, 0xad])), 'dead');
  });
});

// ========================= JSON to scalar =========================

describe('JSON to scalar', () => {
  it('bool from JSON', () => {
    assert.strictEqual(jsonToValue(bool, true), true);
    assert.strictEqual(jsonToValue(bool, false), false);
  });

  it('u32 from JSON', () => {
    assert.strictEqual(jsonToValue(u32, 42), 42);
  });

  it('u64 from string', () => {
    assert.strictEqual(jsonToValue(u64, '123456789012345678'), 123456789012345678n);
  });

  it('i64 from string', () => {
    assert.strictEqual(jsonToValue(i64, '-9223372036854775808'), -9223372036854775808n);
  });

  it('bytes from hex', () => {
    assert.deepStrictEqual(jsonToValue(bytes, 'dead'), new Uint8Array([0xde, 0xad]));
  });
});

// ========================= Optional =========================

describe('JSON optional', () => {
  it('none → null', () => {
    assert.strictEqual(valueToJson(optional(u32), null), null);
  });

  it('some → value', () => {
    assert.strictEqual(valueToJson(optional(u32), 42), 42);
  });

  it('null → none', () => {
    assert.strictEqual(jsonToValue(optional(u32), null), null);
  });

  it('value → some', () => {
    assert.strictEqual(jsonToValue(optional(u32), 42), 42);
  });
});

// ========================= Vec/Array =========================

describe('JSON vec/array', () => {
  it('vec<u32>', () => {
    const v = vec(u32);
    assert.deepStrictEqual(valueToJson(v, [1, 2, 3]), [1, 2, 3]);
    assert.deepStrictEqual(jsonToValue(v, [1, 2, 3]), [1, 2, 3]);
  });

  it('vec<u64> → string[]', () => {
    const v = vec(u64);
    assert.deepStrictEqual(valueToJson(v, [1n, 2n]), ['1', '2']);
    assert.deepStrictEqual(jsonToValue(v, ['1', '2']), [1n, 2n]);
  });

  it('array<u32, 3>', () => {
    const a = array(u32, 3);
    assert.deepStrictEqual(valueToJson(a, [1, 2, 3]), [1, 2, 3]);
  });
});

// ========================= Tuple =========================

describe('JSON tuple', () => {
  it('tuple<u32, str>', () => {
    const t = tuple(u32, str);
    assert.deepStrictEqual(valueToJson(t, [42, 'hello']), [42, 'hello']);
    assert.deepStrictEqual(jsonToValue(t, [42, 'hello']), [42, 'hello']);
  });
});

// ========================= Variant =========================

describe('JSON variant', () => {
  const Shape = variant({
    Circle: f64,
    Rectangle: tuple(f64, f64),
  });

  it('variant → { CaseName: value }', () => {
    assert.deepStrictEqual(
      valueToJson(Shape, { type: 'Circle', value: 3.14 }),
      { Circle: 3.14 },
    );
  });

  it('{ CaseName: value } → variant', () => {
    assert.deepStrictEqual(
      jsonToValue(Shape, { Circle: 3.14 }),
      { type: 'Circle', value: 3.14 },
    );
  });

  it('variant rejects multiple keys', () => {
    assert.throws(() => jsonToValue(Shape, { Circle: 1, Rectangle: [2, 3] }), /exactly one key/);
  });

  it('variant rejects unknown case', () => {
    assert.throws(() => jsonToValue(Shape, { Triangle: 1 }), /Unknown variant case/);
  });
});

// ========================= Struct =========================

describe('JSON struct', () => {
  const Person = struct({ name: str, age: u32, active: bool });

  it('struct → object', () => {
    assert.deepStrictEqual(
      valueToJson(Person, { name: 'Alice', age: 30, active: true }),
      { name: 'Alice', age: 30, active: true },
    );
  });

  it('object → struct', () => {
    assert.deepStrictEqual(
      jsonToValue(Person, { name: 'Alice', age: 30, active: true }),
      { name: 'Alice', age: 30, active: true },
    );
  });

  it('fixed struct', () => {
    const Point = fixedStruct({ x: f64, y: f64 });
    assert.deepStrictEqual(
      valueToJson(Point, { x: 1.5, y: 2.5 }),
      { x: 1.5, y: 2.5 },
    );
  });
});

// ========================= Map =========================

describe('JSON map', () => {
  it('map<u32> → object', () => {
    const M = map(u32);
    assert.deepStrictEqual(
      valueToJson(M, { a: 1, b: 2 }),
      { a: 1, b: 2 },
    );
  });

  it('object → map<u32>', () => {
    const M = map(u32);
    assert.deepStrictEqual(
      jsonToValue(M, { a: 1, b: 2 }),
      { a: 1, b: 2 },
    );
  });
});

// ========================= fracToJson / jsonToFrac round-trip =========================

describe('fracToJson / jsonToFrac round-trip', () => {
  it('struct round-trip through JSON', () => {
    const Person = struct({ name: str, age: u32 });
    const original = { name: 'Alice', age: 30 };
    const packed = Person.pack(original);
    const json = fracToJson(Person, packed);
    assert.deepStrictEqual(json, original);
    const repacked = jsonToFrac(Person, json);
    assert.deepStrictEqual(Person.unpack(repacked), original);
  });

  it('u64 survives JSON round-trip as string', () => {
    const packed = u64.pack(18446744073709551615n);
    const json = fracToJson(u64, packed);
    assert.strictEqual(json, '18446744073709551615');
    const repacked = jsonToFrac(u64, json);
    assert.strictEqual(u64.unpack(repacked), 18446744073709551615n);
  });

  it('bytes survives JSON round-trip as hex', () => {
    const data = new Uint8Array([0xca, 0xfe, 0xba, 0xbe]);
    const packed = bytes.pack(data);
    const json = fracToJson(bytes, packed);
    assert.strictEqual(json, 'cafebabe');
    const repacked = jsonToFrac(bytes, json);
    assert.deepStrictEqual(bytes.unpack(repacked), data);
  });

  it('complex nested type', () => {
    const T = struct({
      id: u64,
      data: bytes,
      tags: vec(str),
      meta: optional(str),
    });
    const original = {
      id: 999n,
      data: new Uint8Array([1, 2, 3]),
      tags: ['a', 'b'],
      meta: 'info',
    };
    const packed = T.pack(original);
    const json = fracToJson(T, packed) as any;
    assert.strictEqual(json.id, '999');
    assert.strictEqual(json.data, '010203');
    assert.deepStrictEqual(json.tags, ['a', 'b']);
    assert.strictEqual(json.meta, 'info');

    const repacked = jsonToFrac(T, json);
    const restored = T.unpack(repacked);
    assert.strictEqual(restored.id, 999n);
    assert.deepStrictEqual(restored.data, new Uint8Array([1, 2, 3]));
  });
});
