import { describe, it } from 'node:test';
import * as assert from 'node:assert/strict';
import {
  bool, u8, u16, u32, u64, i8, i16, i32, i64, f32, f64,
  str, bytes, optional, vec, array, tuple, variant,
  struct, fixedStruct,
  typeToAnyType, generateSchema, PsszSchema, PsszError,
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

  // Text is the IR's "utf8"; "string" is the raw string no combinator emits.
  it('str → Custom(List(u8), "utf8")', () => {
    assert.deepStrictEqual(typeToAnyType(str), {
      Custom: { type: { List: { Int: { bits: 8, isSigned: false } } }, id: 'utf8' },
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
        { Custom: { type: { List: { Int: { bits: 8, isSigned: false } } }, id: 'utf8' } },
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
          Custom: { type: { List: { Int: { bits: 8, isSigned: false } } }, id: 'utf8' },
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

describe('generateSchema is the Schema IR of the PSSZ codecs', () => {
  const Person = struct({ name: str, avatar: bytes, age: u32, tags: vec(str), nick: optional(str) });
  const Point = fixedStruct({ x: f64, y: f64 });
  const schema = generateSchema({ Person, Point });
  const pssz = new PsszSchema(schema);

  it('text is a string, bytes are a Uint8Array', () => {
    const type = pssz.type('Person');
    const value = { name: 'Zoë', avatar: Uint8Array.of(0xff, 0, 1), age: 30, tags: ['a', 'é'], nick: null };
    const encoded = type.encode(value);
    const decoded = type.decode(encoded) as typeof value;
    assert.deepStrictEqual(decoded, value);
    assert.strictEqual(typeof decoded.name, 'string');
    assert.ok(decoded.avatar instanceof Uint8Array);
    // Bytes are an array of numbers in JSON and a hex string in I-JSON.
    assert.strictEqual(type.valueToJson(value, 'json'), '{"name":"Zoë","avatar":[255,0,1],"age":30,"tags":["a","é"],"nick":null}');
    assert.strictEqual(type.valueToJson(value, 'ijson'), '{"name":"Zoë","avatar":"FF0001","age":30,"tags":["a","é"],"nick":null}');
    assert.deepStrictEqual(type.jsonToValue(type.valueToJson(value, 'json'), 'json'), value);
    assert.deepStrictEqual(type.jsonToValue(type.valueToJson(value, 'ijson'), 'ijson'), value);
    // Text is validated: bytes are not a text value, and text is not bytes.
    assert.throws(() => type.encode({ ...value, name: Uint8Array.of(0x61) }), PsszError);
    assert.throws(() => type.encode({ ...value, name: '\ud800' }), PsszError);
    assert.throws(() => type.encode({ ...value, avatar: 'ff0001' }), PsszError);
    // Bytes that are not UTF-8 are not a `str`.
    const Name = new PsszSchema(generateSchema({ Name: str })).type('Name');
    assert.strictEqual(Name.decode(Uint8Array.of(0xc3, 0xa9)), 'é');
    assert.throws(() => Name.decode(Uint8Array.of(0xff)), PsszError);
  });

  it('a fixedStruct is a Struct, a struct an Object', () => {
    const point = pssz.type('Point');
    assert.strictEqual(point.encode({ x: 1.5, y: -2 }).length, 16);
    assert.deepStrictEqual(point.decode(point.encode({ x: 1.5, y: -2 })), { x: 1.5, y: -2 });
  });

  it('the raw string of the IR is bytes, and no combinator produces it', () => {
    assert.ok(!JSON.stringify(schema).includes('"string"'));
    const raw = new PsszSchema({ Raw: { Custom: { type: { List: { Int: { bits: 8, isSigned: false } } }, id: 'string' } } }).type('Raw');
    assert.deepStrictEqual(raw.decode(Uint8Array.of(0xff, 0x61)), Uint8Array.of(0xff, 0x61));
    assert.deepStrictEqual(raw.encode('a'), Uint8Array.of(0x61));
  });
});
