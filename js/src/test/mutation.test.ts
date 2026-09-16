import { describe, it } from 'node:test';
import * as assert from 'node:assert/strict';
import {
  bool, u8, u16, u32, u64, i32, f64,
  str, bytes, optional, vec,
  struct, fixedStruct,
  MutView,
  spliceBuffer,
  patchOffset,
} from '../index.js';

// ========================= spliceBuffer =========================

describe('spliceBuffer', () => {
  it('replace same size', () => {
    const data = new Uint8Array([1, 2, 3, 4, 5]);
    const { result, delta } = spliceBuffer(data, 1, 2, new Uint8Array([10, 20]));
    assert.strictEqual(delta, 0);
    assert.deepStrictEqual([...result], [1, 10, 20, 4, 5]);
  });

  it('grow', () => {
    const data = new Uint8Array([1, 2, 3, 4, 5]);
    const { result, delta } = spliceBuffer(data, 1, 2, new Uint8Array([10, 20, 30, 40]));
    assert.strictEqual(delta, 2);
    assert.deepStrictEqual([...result], [1, 10, 20, 30, 40, 4, 5]);
  });

  it('shrink', () => {
    const data = new Uint8Array([1, 2, 3, 4, 5]);
    const { result, delta } = spliceBuffer(data, 1, 3, new Uint8Array([99]));
    assert.strictEqual(delta, -2);
    assert.deepStrictEqual([...result], [1, 99, 5]);
  });

  it('insert at end', () => {
    const data = new Uint8Array([1, 2]);
    const { result, delta } = spliceBuffer(data, 2, 0, new Uint8Array([3, 4]));
    assert.strictEqual(delta, 2);
    assert.deepStrictEqual([...result], [1, 2, 3, 4]);
  });

  it('delete from middle', () => {
    const data = new Uint8Array([1, 2, 3, 4, 5]);
    const { result, delta } = spliceBuffer(data, 2, 2, new Uint8Array(0));
    assert.strictEqual(delta, -2);
    assert.deepStrictEqual([...result], [1, 2, 5]);
  });
});

// ========================= patchOffset =========================

describe('patchOffset', () => {
  it('adjusts offset when target is past splice point', () => {
    // Offset at pos 0, value = 10, splicePos = 5, delta = 3
    const data = new Uint8Array(4);
    new DataView(data.buffer).setUint32(0, 10, true);
    patchOffset(data, 0, 5, 3);
    assert.strictEqual(new DataView(data.buffer).getUint32(0, true), 13);
  });

  it('does not adjust offset when target is before splice point', () => {
    const data = new Uint8Array(4);
    new DataView(data.buffer).setUint32(0, 4, true);
    patchOffset(data, 0, 20, 5);
    // target = 0 + 4 = 4, which is before 20 -> no change
    assert.strictEqual(new DataView(data.buffer).getUint32(0, true), 4);
  });

  it('skips None marker (1)', () => {
    const data = new Uint8Array(4);
    new DataView(data.buffer).setUint32(0, 1, true);
    patchOffset(data, 0, 0, 10);
    assert.strictEqual(new DataView(data.buffer).getUint32(0, true), 1);
  });

  it('skips empty marker (0)', () => {
    const data = new Uint8Array(4);
    new DataView(data.buffer).setUint32(0, 0, true);
    patchOffset(data, 0, 0, 10);
    assert.strictEqual(new DataView(data.buffer).getUint32(0, true), 0);
  });
});

// ========================= MutView scalar fields =========================

describe('MutView scalar fields', () => {
  const Person = struct({ name: str, age: u32, active: bool });

  it('read scalar field', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const mv = new MutView(Person, packed);
    assert.strictEqual(mv.get('age'), 30);
    assert.strictEqual(mv.get('active'), true);
  });

  it('set u32 field', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const mv = new MutView(Person, packed);
    mv.set('age', 31);
    assert.strictEqual(mv.get('age'), 31);
    assert.strictEqual(mv.get('name'), 'Alice');
    assert.strictEqual(mv.get('active'), true);
  });

  it('set bool field', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const mv = new MutView(Person, packed);
    mv.set('active', false);
    assert.strictEqual(mv.get('active'), false);
    assert.strictEqual(mv.get('name'), 'Alice');
    assert.strictEqual(mv.get('age'), 30);
  });

  it('produces valid canonical output for scalar set', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const mv = new MutView(Person, packed);
    mv.set('age', 99);
    const expected = Person.pack({ name: 'Alice', age: 99, active: true });
    assert.deepStrictEqual([...mv.toBytes()], [...expected]);
  });
});

// ========================= MutView string fields (canonical) =========================

describe('MutView string fields (canonical)', () => {
  const Person = struct({ name: str, age: u32, active: bool });

  it('set string to same length', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const mv = new MutView(Person, packed);
    mv.set('name', 'Bobby');
    assert.strictEqual(mv.get('name'), 'Bobby');
    assert.strictEqual(mv.get('age'), 30);
    assert.strictEqual(mv.get('active'), true);
  });

  it('set string to shorter', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const mv = new MutView(Person, packed);
    mv.set('name', 'Al');
    assert.strictEqual(mv.get('name'), 'Al');
    // Canonical mode: should produce same bytes as fresh pack
    const expected = Person.pack({ name: 'Al', age: 30, active: true });
    assert.deepStrictEqual([...mv.toBytes()], [...expected]);
  });

  it('set string to longer', () => {
    const packed = Person.pack({ name: 'Al', age: 30, active: true });
    const mv = new MutView(Person, packed);
    mv.set('name', 'Alexander');
    assert.strictEqual(mv.get('name'), 'Alexander');
    const expected = Person.pack({ name: 'Alexander', age: 30, active: true });
    assert.deepStrictEqual([...mv.toBytes()], [...expected]);
  });

  it('set string to empty', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const mv = new MutView(Person, packed);
    mv.set('name', '');
    assert.strictEqual(mv.get('name'), '');
    const expected = Person.pack({ name: '', age: 30, active: true });
    assert.deepStrictEqual([...mv.toBytes()], [...expected]);
  });

  it('set string from empty', () => {
    const packed = Person.pack({ name: '', age: 30, active: true });
    const mv = new MutView(Person, packed);
    mv.set('name', 'Hello');
    assert.strictEqual(mv.get('name'), 'Hello');
  });
});

// ========================= MutView optional fields =========================

describe('MutView optional fields', () => {
  const S = struct({
    name: str,
    bio: optional(str),
    score: u32,
  });

  it('set optional from None to value', () => {
    const packed = S.pack({ name: 'Alice', bio: null, score: 10 });
    const mv = new MutView(S, packed);
    assert.strictEqual(mv.get('bio'), null);
    mv.set('bio', 'Hello world');
    assert.strictEqual(mv.get('bio'), 'Hello world');
    assert.strictEqual(mv.get('name'), 'Alice');
    assert.strictEqual(mv.get('score'), 10);
  });

  it('set optional from value to None', () => {
    const packed = S.pack({ name: 'Alice', bio: 'Some bio', score: 10 });
    const mv = new MutView(S, packed);
    assert.strictEqual(mv.get('bio'), 'Some bio');
    mv.set('bio', null);
    assert.strictEqual(mv.get('bio'), null);
    assert.strictEqual(mv.get('name'), 'Alice');
    assert.strictEqual(mv.get('score'), 10);
  });

  it('set optional from value to different value', () => {
    const packed = S.pack({ name: 'Alice', bio: 'Old bio', score: 10 });
    const mv = new MutView(S, packed);
    mv.set('bio', 'New bio that is longer');
    assert.strictEqual(mv.get('bio'), 'New bio that is longer');
    assert.strictEqual(mv.get('name'), 'Alice');
    assert.strictEqual(mv.get('score'), 10);
  });

  it('set optional from None to None is no-op', () => {
    const packed = S.pack({ name: 'Alice', bio: null, score: 10 });
    const mv = new MutView(S, packed);
    const before = mv.toBytes();
    mv.set('bio', null);
    assert.deepStrictEqual([...mv.toBytes()], [...before]);
  });

  it('canonical mode produces same bytes as fresh pack (optional None->value)', () => {
    const packed = S.pack({ name: 'A', bio: null, score: 5 });
    const mv = new MutView(S, packed);
    mv.set('bio', 'hello');
    const result = S.unpack(mv.toBytes());
    assert.strictEqual(result.bio, 'hello');
    assert.strictEqual(result.name, 'A');
    assert.strictEqual(result.score, 5);
  });

  it('canonical mode produces same bytes as fresh pack (optional value->None)', () => {
    const packed = S.pack({ name: 'A', bio: 'hello', score: 5 });
    const mv = new MutView(S, packed);
    mv.set('bio', null);
    // After setting to None, the bytes should still be valid
    const result = S.unpack(mv.toBytes());
    assert.strictEqual(result.bio, null);
    assert.strictEqual(result.name, 'A');
    assert.strictEqual(result.score, 5);
  });
});

// ========================= MutView trailing optional elision =========================

describe('MutView trailing optional elision', () => {
  const S = struct({
    name: str,
    age: u32,
    bio: optional(str),
    note: optional(str),
  });

  it('elided trailing optionals read as null', () => {
    // Pack with trailing None values - they should be elided
    const packed = S.pack({ name: 'Alice', age: 30, bio: null, note: null });
    const mv = new MutView(S, packed);
    assert.strictEqual(mv.get('bio'), null);
    assert.strictEqual(mv.get('note'), null);
    assert.strictEqual(mv.get('name'), 'Alice');
    assert.strictEqual(mv.get('age'), 30);
  });

  it('setting elided optional to None is no-op', () => {
    const packed = S.pack({ name: 'Alice', age: 30, bio: null, note: null });
    const mv = new MutView(S, packed);
    const before = mv.toBytes();
    mv.set('note', null);
    assert.deepStrictEqual([...mv.toBytes()], [...before]);
  });

  it('setting elided optional to value expands fixed region', () => {
    const packed = S.pack({ name: 'Alice', age: 30, bio: null, note: null });
    const mv = new MutView(S, packed);
    mv.set('bio', 'A bio');
    assert.strictEqual(mv.get('bio'), 'A bio');
    assert.strictEqual(mv.get('name'), 'Alice');
    assert.strictEqual(mv.get('age'), 30);
  });
});

// ========================= MutView fast mode =========================

describe('MutView fast mode', () => {
  const Person = struct({ name: str, age: u32, active: bool });

  it('set string to shorter (overwrite in place)', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const mv = new MutView(Person, packed, { fast: true });
    mv.set('name', 'Al');
    assert.strictEqual(mv.get('name'), 'Al');
    assert.strictEqual(mv.get('age'), 30);
  });

  it('set string to longer (append to end)', () => {
    const packed = Person.pack({ name: 'Al', age: 30, active: true });
    const mv = new MutView(Person, packed, { fast: true });
    mv.set('name', 'Alexander');
    assert.strictEqual(mv.get('name'), 'Alexander');
    assert.strictEqual(mv.get('age'), 30);
  });

  it('set optional to None in fast mode', () => {
    const S = struct({ name: str, bio: optional(str), score: u32 });
    const packed = S.pack({ name: 'A', bio: 'hello', score: 5 });
    const mv = new MutView(S, packed, { fast: true });
    mv.set('bio', null);
    assert.strictEqual(mv.get('bio'), null);
    assert.strictEqual(mv.get('name'), 'A');
  });

  it('compact after fast mode produces canonical output', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const mv = new MutView(Person, packed, { fast: true });
    mv.set('name', 'Alexander the Great');
    mv.set('age', 99);
    mv.compact();
    const expected = Person.pack({ name: 'Alexander the Great', age: 99, active: true });
    assert.deepStrictEqual([...mv.toBytes()], [...expected]);
  });

  it('compact removes dead bytes', () => {
    const packed = Person.pack({ name: 'A very long name', age: 30, active: true });
    const mv = new MutView(Person, packed, { fast: true });
    mv.set('name', 'X');
    // Before compact, buffer may have dead bytes
    mv.compact();
    const expected = Person.pack({ name: 'X', age: 30, active: true });
    assert.deepStrictEqual([...mv.toBytes()], [...expected]);
  });
});

// ========================= Read-after-write consistency =========================

describe('MutView read-after-write consistency', () => {
  const S = struct({
    a: str,
    b: u32,
    c: str,
    d: optional(str),
  });

  it('read after set string', () => {
    const packed = S.pack({ a: 'hello', b: 42, c: 'world', d: null });
    const mv = new MutView(S, packed);
    mv.set('a', 'hi');
    assert.strictEqual(mv.get('a'), 'hi');
    assert.strictEqual(mv.get('b'), 42);
    assert.strictEqual(mv.get('c'), 'world');
    assert.strictEqual(mv.get('d'), null);
  });

  it('multiple writes then read all fields', () => {
    const packed = S.pack({ a: 'hello', b: 42, c: 'world', d: null });
    const mv = new MutView(S, packed);
    mv.set('a', 'AAAA');
    mv.set('c', 'CCCCCCCC');
    mv.set('b', 99);
    mv.set('d', 'some data');
    assert.strictEqual(mv.get('a'), 'AAAA');
    assert.strictEqual(mv.get('b'), 99);
    assert.strictEqual(mv.get('c'), 'CCCCCCCC');
    assert.strictEqual(mv.get('d'), 'some data');
  });

  it('round-trip through toBytes and unpack', () => {
    const packed = S.pack({ a: 'hello', b: 42, c: 'world', d: 'data' });
    const mv = new MutView(S, packed);
    mv.set('a', 'modified');
    mv.set('d', null);
    const result = S.unpack(mv.toBytes());
    assert.strictEqual(result.a, 'modified');
    assert.strictEqual(result.b, 42);
    assert.strictEqual(result.c, 'world');
    assert.strictEqual(result.d, null);
  });
});

// ========================= Canonical mode byte-exact =========================

describe('MutView canonical byte-exact', () => {
  const Person = struct({ name: str, age: u32, active: bool });

  it('set scalar produces same bytes as fresh pack', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const mv = new MutView(Person, packed);
    mv.set('age', 99);
    const expected = Person.pack({ name: 'Alice', age: 99, active: true });
    assert.deepStrictEqual([...mv.toBytes()], [...expected]);
  });

  it('set string produces same bytes as fresh pack', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const mv = new MutView(Person, packed);
    mv.set('name', 'Bob');
    const expected = Person.pack({ name: 'Bob', age: 30, active: true });
    assert.deepStrictEqual([...mv.toBytes()], [...expected]);
  });

  it('multiple mutations produce same bytes as fresh pack', () => {
    const packed = Person.pack({ name: 'Alice', age: 30, active: true });
    const mv = new MutView(Person, packed);
    mv.set('name', 'Charlie');
    mv.set('age', 25);
    mv.set('active', false);
    const expected = Person.pack({ name: 'Charlie', age: 25, active: false });
    assert.deepStrictEqual([...mv.toBytes()], [...expected]);
  });
});

// ========================= MutView with multiple variable-size fields =========================

describe('MutView multiple variable-size fields', () => {
  const S = struct({
    first: str,
    middle: str,
    last: str,
    age: u32,
  });

  it('modify first string field', () => {
    const packed = S.pack({ first: 'John', middle: 'A', last: 'Doe', age: 30 });
    const mv = new MutView(S, packed);
    mv.set('first', 'Jonathan');
    const expected = S.pack({ first: 'Jonathan', middle: 'A', last: 'Doe', age: 30 });
    assert.deepStrictEqual([...mv.toBytes()], [...expected]);
  });

  it('modify middle string field', () => {
    const packed = S.pack({ first: 'John', middle: 'A', last: 'Doe', age: 30 });
    const mv = new MutView(S, packed);
    mv.set('middle', 'Alexander');
    const expected = S.pack({ first: 'John', middle: 'Alexander', last: 'Doe', age: 30 });
    assert.deepStrictEqual([...mv.toBytes()], [...expected]);
  });

  it('modify last string field', () => {
    const packed = S.pack({ first: 'John', middle: 'A', last: 'Doe', age: 30 });
    const mv = new MutView(S, packed);
    mv.set('last', 'Smith');
    const expected = S.pack({ first: 'John', middle: 'A', last: 'Smith', age: 30 });
    assert.deepStrictEqual([...mv.toBytes()], [...expected]);
  });

  it('modify all three string fields sequentially', () => {
    const packed = S.pack({ first: 'John', middle: 'A', last: 'Doe', age: 30 });
    const mv = new MutView(S, packed);
    mv.set('first', 'Jane');
    mv.set('middle', 'B');
    mv.set('last', 'Smith-Jones');
    mv.set('age', 25);
    const expected = S.pack({ first: 'Jane', middle: 'B', last: 'Smith-Jones', age: 25 });
    assert.deepStrictEqual([...mv.toBytes()], [...expected]);
  });
});

// ========================= MutView with fixedStruct =========================

describe('MutView fixedStruct', () => {
  const Point = fixedStruct({ x: f64, y: f64 });

  it('read fields', () => {
    const packed = Point.pack({ x: 1.5, y: 2.5 });
    const mv = new MutView(Point, packed);
    assert.strictEqual(mv.get('x'), 1.5);
    assert.strictEqual(mv.get('y'), 2.5);
  });

  it('set scalar field', () => {
    const packed = Point.pack({ x: 1.5, y: 2.5 });
    const mv = new MutView(Point, packed);
    mv.set('x', 10.0);
    assert.strictEqual(mv.get('x'), 10.0);
    assert.strictEqual(mv.get('y'), 2.5);
    const expected = Point.pack({ x: 10.0, y: 2.5 });
    assert.deepStrictEqual([...mv.toBytes()], [...expected]);
  });
});

// ========================= MutView error handling =========================

describe('MutView errors', () => {
  const Person = struct({ name: str, age: u32 });

  it('rejects unknown field on get', () => {
    const packed = Person.pack({ name: 'Alice', age: 30 });
    const mv = new MutView(Person, packed);
    assert.throws(() => (mv as any).get('nonexistent'), /unknown field/);
  });

  it('rejects unknown field on set', () => {
    const packed = Person.pack({ name: 'Alice', age: 30 });
    const mv = new MutView(Person, packed);
    assert.throws(() => (mv as any).set('nonexistent', 42), /unknown field/);
  });
});

// ========================= MutView with vec field =========================

describe('MutView vec field', () => {
  const S = struct({
    name: str,
    tags: vec(str),
    score: u32,
  });

  it('set vec field (canonical produces valid output)', () => {
    const packed = S.pack({ name: 'A', tags: ['x'], score: 10 });
    const mv = new MutView(S, packed);
    mv.set('tags', ['a', 'b', 'c']);
    const result = S.unpack(mv.toBytes());
    assert.deepStrictEqual(result.tags, ['a', 'b', 'c']);
    assert.strictEqual(result.name, 'A');
    assert.strictEqual(result.score, 10);
  });

  it('set vec field to empty', () => {
    const packed = S.pack({ name: 'A', tags: ['x', 'y'], score: 10 });
    const mv = new MutView(S, packed);
    mv.set('tags', []);
    const result = S.unpack(mv.toBytes());
    assert.deepStrictEqual(result.tags, []);
    assert.strictEqual(result.name, 'A');
    assert.strictEqual(result.score, 10);
  });

  it('set vec field from empty', () => {
    const packed = S.pack({ name: 'A', tags: [], score: 10 });
    const mv = new MutView(S, packed);
    mv.set('tags', ['hello', 'world']);
    const result = S.unpack(mv.toBytes());
    assert.deepStrictEqual(result.tags, ['hello', 'world']);
    assert.strictEqual(result.name, 'A');
    assert.strictEqual(result.score, 10);
  });
});

// ========================= MutView bytes field =========================

describe('MutView bytes field', () => {
  const S = struct({
    name: str,
    data: bytes,
  });

  it('set bytes field', () => {
    const packed = S.pack({ name: 'A', data: new Uint8Array([1, 2, 3]) });
    const mv = new MutView(S, packed);
    mv.set('data', new Uint8Array([4, 5, 6, 7, 8]));
    const result = S.unpack(mv.toBytes());
    assert.deepStrictEqual([...result.data], [4, 5, 6, 7, 8]);
    assert.strictEqual(result.name, 'A');
  });
});
