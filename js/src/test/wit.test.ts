import { describe, it } from 'node:test';
import * as assert from 'node:assert/strict';
import {
  bool, u8, u16, u32, u64, i8, i16, i32, i64, f32, f64,
  str, bytes, optional, vec, tuple, variant,
  struct, fixedStruct,
  generateWit, typeToWitExpr, toKebabCase,
} from '../index.js';

describe('toKebabCase', () => {
  it('snake_case → kebab-case', () => {
    assert.strictEqual(toKebabCase('fire_and_forget'), 'fire-and-forget');
  });

  it('camelCase → kebab-case', () => {
    assert.strictEqual(toKebabCase('myFunction'), 'my-function');
  });

  it('PascalCase → kebab-case', () => {
    assert.strictEqual(toKebabCase('MyFunction'), 'my-function');
  });

  it('already kebab → unchanged', () => {
    assert.strictEqual(toKebabCase('my-function'), 'my-function');
  });

  it('simple lowercase → unchanged', () => {
    assert.strictEqual(toKebabCase('greet'), 'greet');
  });
});

describe('typeToWitExpr', () => {
  it('bool → bool', () => {
    assert.strictEqual(typeToWitExpr(bool), 'bool');
  });

  it('u32 → u32', () => {
    assert.strictEqual(typeToWitExpr(u32), 'u32');
  });

  it('i32 → s32', () => {
    assert.strictEqual(typeToWitExpr(i32), 's32');
  });

  it('f64 → f64', () => {
    assert.strictEqual(typeToWitExpr(f64), 'f64');
  });

  it('str → string', () => {
    assert.strictEqual(typeToWitExpr(str), 'string');
  });

  it('bytes → list<u8>', () => {
    assert.strictEqual(typeToWitExpr(bytes), 'list<u8>');
  });

  it('vec(u32) → list<u32>', () => {
    assert.strictEqual(typeToWitExpr(vec(u32)), 'list<u32>');
  });

  it('optional(str) → option<string>', () => {
    assert.strictEqual(typeToWitExpr(optional(str)), 'option<string>');
  });

  it('tuple(u32, str) → tuple<u32, string>', () => {
    assert.strictEqual(typeToWitExpr(tuple(u32, str)), 'tuple<u32, string>');
  });
});

describe('generateWit', () => {
  it('empty world', () => {
    const wit = generateWit('empty');
    assert.strictEqual(wit, 'world empty {\n}');
  });

  it('world with exports', () => {
    const wit = generateWit('test-exports', {
      exports: {
        greet: { params: { name: str }, result: str },
        add: { params: { a: u32, b: u32 }, result: u32 },
        fireAndForget: { params: {} },
      },
    });
    assert.ok(wit.includes('world test-exports {'));
    assert.ok(wit.includes('export greet: func(name: string) -> string;'));
    assert.ok(wit.includes('export add: func(a: u32, b: u32) -> u32;'));
    assert.ok(wit.includes('export fire-and-forget: func();'));
  });

  it('world with imports', () => {
    const wit = generateWit('test-imports', {
      imports: {
        log: { params: { message: str } },
      },
    });
    assert.ok(wit.includes('import log: func(message: string);'));
  });

  it('record type definition', () => {
    const Person = struct({ name: str, age: u32 });
    const wit = generateWit('test-record', {
      types: { Person },
      exports: {
        getPerson: { params: {}, result: Person },
      },
    });
    assert.ok(wit.includes('record person {'));
    assert.ok(wit.includes('  name: string,'));
    assert.ok(wit.includes('  age: u32,'));
    assert.ok(wit.includes('export get-person: func() -> person;'));
  });

  it('fixed struct as record', () => {
    const Point = fixedStruct({ x: f64, y: f64 });
    const wit = generateWit('test-fixed', {
      types: { Point },
    });
    assert.ok(wit.includes('record point {'));
    assert.ok(wit.includes('  x: f64,'));
    assert.ok(wit.includes('  y: f64,'));
  });

  it('variant type definition', () => {
    const Shape = variant({ Circle: f64, Rectangle: tuple(f64, f64) });
    const wit = generateWit('test-variant', {
      types: { Shape },
    });
    assert.ok(wit.includes('variant shape {'));
    assert.ok(wit.includes('  circle(f64),'));
    assert.ok(wit.includes('  rectangle(tuple<f64, f64>),'));
  });

  it('complex nested types', () => {
    const Address = struct({ street: str, city: str });
    const Person = struct({
      name: str,
      age: u32,
      home: Address,
    });
    const wit = generateWit('test-nested', {
      types: { Person, Address },
      exports: {
        getPerson: { params: {}, result: Person },
        setPerson: { params: { p: Person } },
      },
    });
    assert.ok(wit.includes('record person {'));
    assert.ok(wit.includes('record address {'));
    assert.ok(wit.includes('  home: address,'));
    assert.ok(wit.includes('export get-person: func() -> person;'));
    assert.ok(wit.includes('export set-person: func(p: person);'));
  });

  it('discovers named types nested inside containers', () => {
    const Tag = struct({ label: str, weight: f32 });
    const Item = struct({ name: str, tags: vec(Tag), primary: optional(Tag) });
    const wit = generateWit('test-deep', {
      types: { Item, Tag },
    });
    // Tag should be discovered even though it's inside vec() and optional()
    assert.ok(wit.includes('record tag {'), 'Tag record should be emitted');
    assert.ok(wit.includes('  tags: list<tag>,'), 'vec(Tag) should reference tag');
    assert.ok(wit.includes('  primary: option<tag>,'), 'optional(Tag) should reference tag');
  });

  it('optional and list fields in record', () => {
    const Config = struct({
      name: str,
      tags: vec(str),
      email: optional(str),
    });
    const wit = generateWit('test-containers', {
      types: { Config },
    });
    assert.ok(wit.includes('record config {'));
    assert.ok(wit.includes('  tags: list<string>,'));
    assert.ok(wit.includes('  email: option<string>,'));
  });
});
