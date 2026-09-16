import { describe, it } from 'node:test';
import * as assert from 'node:assert/strict';
import {
  bool, u32, u64, f64, str, optional, vec,
  struct, fixedStruct,
  type StructType, type Infer,
} from '../index.js';

// ========================= setField on struct =========================

describe('struct setField', () => {
  const Person = struct({ name: str, age: u32, active: bool });

  it('set fixed-size field (u32)', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const updated = Person.setField(packed, 'age', 31);
    const result = Person.unpack(updated);
    assert.strictEqual(result.age, 31);
    assert.strictEqual(result.name, 'Alice');
    assert.strictEqual(result.active, true);
  });

  it('set fixed-size field (bool)', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const updated = Person.setField(packed, 'active', false);
    const result = Person.unpack(updated);
    assert.strictEqual(result.active, false);
    assert.strictEqual(result.name, 'Alice');
    assert.strictEqual(result.age, 30);
  });

  it('set variable-size field (string) triggers repack', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const updated = Person.setField(packed, 'name', 'Bob');
    const result = Person.unpack(updated);
    assert.strictEqual(result.name, 'Bob');
    assert.strictEqual(result.age, 30);
    assert.strictEqual(result.active, true);
  });

  it('set string to longer value', () => {
    const packed = Person.pack({ name: 'Al', age: 30, active: true });
    const updated = Person.setField(packed, 'name', 'Alexander');
    const result = Person.unpack(updated);
    assert.strictEqual(result.name, 'Alexander');
  });

  it('set string to shorter value', () => {
    const packed = Person.pack({ name: 'Alexander', age: 30, active: true });
    const updated = Person.setField(packed, 'name', 'Al');
    const result = Person.unpack(updated);
    assert.strictEqual(result.name, 'Al');
  });

  it('does not modify original buffer', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const original = new Uint8Array(packed);
    Person.setField(packed, 'age', 99);
    assert.deepStrictEqual(packed, original);
  });

  it('rejects unknown field', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    assert.throws(
      () => (Person as any).setField(packed, 'nonexistent', 42),
      /Unknown field/,
    );
  });
});

// ========================= setField on struct with complex fields =========================

describe('struct setField complex', () => {
  const S = struct({
    name: str,
    tags: vec(str),
    score: u32,
    bio: optional(str),
  });

  it('set vec field', () => {
    const packed = S.pack({ name: 'A', tags: ['x'], score: 10, bio: null });
    const updated = S.setField(packed, 'tags', ['a', 'b', 'c']);
    const result = S.unpack(updated);
    assert.deepStrictEqual(result.tags, ['a', 'b', 'c']);
    assert.strictEqual(result.name, 'A');
    assert.strictEqual(result.score, 10);
  });

  it('set optional field from null to value', () => {
    const packed = S.pack({ name: 'A', tags: [], score: 10, bio: null });
    const updated = S.setField(packed, 'bio', 'hello');
    const result = S.unpack(updated);
    assert.strictEqual(result.bio, 'hello');
  });

  it('set optional field from value to null', () => {
    const packed = S.pack({ name: 'A', tags: [], score: 10, bio: 'hello' });
    const updated = S.setField(packed, 'bio', null);
    const result = S.unpack(updated);
    assert.strictEqual(result.bio, null);
  });
});

// ========================= setField on fixedStruct =========================

describe('fixedStruct setField', () => {
  const Point = fixedStruct({ x: f64, y: f64 });

  it('set x', () => {
    const packed = Point.pack({ x: 1.5, y: 2.5 });
    const updated = Point.setField(packed, 'x', 10.0);
    const result = Point.unpack(updated);
    assert.strictEqual(result.x, 10.0);
    assert.strictEqual(result.y, 2.5);
  });

  it('set y', () => {
    const packed = Point.pack({ x: 1.5, y: 2.5 });
    const updated = Point.setField(packed, 'y', 99.9);
    const result = Point.unpack(updated);
    assert.strictEqual(result.x, 1.5);
    assert.strictEqual(result.y, 99.9);
  });

  it('does not modify original buffer', () => {
    const packed = Point.pack({ x: 1.5, y: 2.5 });
    const original = new Uint8Array(packed);
    Point.setField(packed, 'x', 999.0);
    assert.deepStrictEqual(packed, original);
  });

  it('rejects unknown field', () => {
    const packed = Point.pack({ x: 1.5, y: 2.5 });
    assert.throws(
      () => (Point as any).setField(packed, 'z', 3.5),
      /Unknown field/,
    );
  });
});

// ========================= readField =========================

describe('struct readField', () => {
  const Person = struct({ name: str, age: u32, active: bool });

  it('read string field', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    assert.strictEqual(Person.readField(packed, 'name'), 'Alice');
  });

  it('read u32 field', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    assert.strictEqual(Person.readField(packed, 'age'), 30);
  });

  it('read bool field', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    assert.strictEqual(Person.readField(packed, 'active'), true);
  });

  it('rejects unknown field', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    assert.throws(
      () => (Person as any).readField(packed, 'nonexistent'),
      /Unknown field/,
    );
  });
});

describe('fixedStruct readField', () => {
  const Point = fixedStruct({ x: f64, y: f64 });

  it('read x', () => {
    const packed = Point.pack({ x: 1.5, y: 2.5 });
    assert.strictEqual(Point.readField(packed, 'x'), 1.5);
  });

  it('read y', () => {
    const packed = Point.pack({ x: 1.5, y: 2.5 });
    assert.strictEqual(Point.readField(packed, 'y'), 2.5);
  });
});

// ========================= setField + readField consistency =========================

describe('setField + readField consistency', () => {
  const Person = struct({ name: str, age: u32, active: bool });

  it('readField after setField returns new value', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const updated = Person.setField(packed, 'age', 31);
    assert.strictEqual(Person.readField(updated, 'age'), 31);
  });

  it('multiple setField calls', () => {
    let packed = Person.pack({ name: 'Alice', age: 30, active: true });
    packed = Person.setField(packed, 'age', 31);
    packed = Person.setField(packed, 'active', false);
    packed = Person.setField(packed, 'name', 'Bob');
    const result = Person.unpack(packed);
    assert.strictEqual(result.name, 'Bob');
    assert.strictEqual(result.age, 31);
    assert.strictEqual(result.active, false);
  });
});
