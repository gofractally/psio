import { describe, it } from 'node:test';
import * as assert from 'node:assert/strict';
import {
  bool, u8, u16, u32, u64, i8, i16, i32, i64, f32, f64,
  str, bytes, optional, vec, array, tuple, variant,
  struct, fixedStruct,
  typeToAnyType, generateSchema,
  type AnyType,
} from '../index.js';

describe('typeToAnyType', () => {
  it('bool → Custom(Int{1,false}, "bool")', () => {
    const a = typeToAnyType(bool);
    assert.deepStrictEqual(a, {
      Custom: { type: { Int: { bits: 1, isSigned: false } }, id: 'bool' },
    });
  });

  it('u32 → Int{32,false}', () => {
    assert.deepStrictEqual(typeToAnyType(u32), {
      Int: { bits: 32, isSigned: false },
    });
  });

  it('i64 → Int{64,true}', () => {
    assert.deepStrictEqual(typeToAnyType(i64), {
      Int: { bits: 64, isSigned: true },
    });
  });

  it('f32 → Float{8,23}', () => {
    assert.deepStrictEqual(typeToAnyType(f32), {
      Float: { exp: 8, mantissa: 23 },
    });
  });

  it('f64 → Float{11,52}', () => {
    assert.deepStrictEqual(typeToAnyType(f64), {
      Float: { exp: 11, mantissa: 52 },
    });
  });

  it('str → Custom(List(u8), "string")', () => {
    assert.deepStrictEqual(typeToAnyType(str), {
      Custom: { type: { List: { Int: { bits: 8, isSigned: false } } }, id: 'string' },
    });
  });

  it('bytes → Custom(List(u8), "hex")', () => {
    assert.deepStrictEqual(typeToAnyType(bytes), {
      Custom: { type: { List: { Int: { bits: 8, isSigned: false } } }, id: 'hex' },
    });
  });

  it('optional(u32) → Option(Int{32,false})', () => {
    assert.deepStrictEqual(typeToAnyType(optional(u32)), {
      Option: { Int: { bits: 32, isSigned: false } },
    });
  });

  it('vec(u32) → List(Int{32,false})', () => {
    assert.deepStrictEqual(typeToAnyType(vec(u32)), {
      List: { Int: { bits: 32, isSigned: false } },
    });
  });

  it('array(u32, 3) → Array{Int{32,false}, 3}', () => {
    assert.deepStrictEqual(typeToAnyType(array(u32, 3)), {
      Array: { type: { Int: { bits: 32, isSigned: false } }, len: 3 },
    });
  });

  it('tuple(u32, str) → Tuple', () => {
    const a = typeToAnyType(tuple(u32, str));
    assert.deepStrictEqual(a, {
      Tuple: [
        { Int: { bits: 32, isSigned: false } },
        { Custom: { type: { List: { Int: { bits: 8, isSigned: false } } }, id: 'string' } },
      ],
    });
  });

  it('variant → Variant', () => {
    const Shape = variant({ Circle: f64, Rectangle: tuple(f64, f64) });
    const a = typeToAnyType(Shape) as { Variant: Record<string, AnyType> };
    assert.ok('Variant' in a);
    assert.ok('Circle' in a.Variant);
    assert.ok('Rectangle' in a.Variant);
  });

  it('struct → Object (extensible)', () => {
    const Person = struct({ name: str, age: u32 });
    const a = typeToAnyType(Person) as { Object: Record<string, AnyType> };
    assert.ok('Object' in a);
    assert.ok('name' in a.Object);
    assert.ok('age' in a.Object);
  });

  it('fixedStruct → Struct (fixed)', () => {
    const Point = fixedStruct({ x: f64, y: f64 });
    const a = typeToAnyType(Point) as { Struct: Record<string, AnyType> };
    assert.ok('Struct' in a);
    assert.ok('x' in a.Struct);
    assert.ok('y' in a.Struct);
    assert.deepStrictEqual(a.Struct.x, { Float: { exp: 11, mantissa: 52 } });
  });

  it('nested optional(vec(str))', () => {
    const a = typeToAnyType(optional(vec(str)));
    assert.deepStrictEqual(a, {
      Option: {
        List: {
          Custom: { type: { List: { Int: { bits: 8, isSigned: false } } }, id: 'string' },
        },
      },
    });
  });
});

describe('generateSchema', () => {
  it('produces registry with named types', () => {
    const Person = struct({ name: str, age: u32, active: bool });
    const Point = fixedStruct({ x: f64, y: f64 });
    const schema = generateSchema({ Person, Point });

    assert.ok('Person' in schema);
    assert.ok('Point' in schema);
    assert.ok('Object' in (schema.Person as any));
    assert.ok('Struct' in (schema.Point as any));
  });

  it('is JSON-serializable', () => {
    const Person = struct({ name: str, age: u32 });
    const schema = generateSchema({ Person });
    const json = JSON.stringify(schema);
    const parsed = JSON.parse(json);
    assert.deepStrictEqual(parsed, schema);
  });
});
