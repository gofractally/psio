import { describe, it } from 'node:test';
import * as assert from 'node:assert/strict';
import {
  bool, u8, u16, u32, u64, i32, f32, f64, str, vec, optional,
  struct, fixedStruct, StructType,
} from '../index.js';

describe('view correctness', () => {
  const Person = struct({ name: str, age: u32, active: bool });
  const packed = Person.pack({ name: 'Alice', age: 30, active: true });

  it('view and unpack produce identical field values', () => {
    const view = Person.view(packed);
    const obj = Person.unpack(packed);
    assert.strictEqual(view.name, obj.name);
    assert.strictEqual(view.age, obj.age);
    assert.strictEqual(view.active, obj.active);
  });

  it('spread produces identical plain object', () => {
    const view = Person.view(packed);
    const obj = Person.unpack(packed);
    assert.deepStrictEqual({ ...view }, obj);
  });

  it('Object.keys returns same field names', () => {
    const view = Person.view(packed);
    const obj = Person.unpack(packed);
    assert.deepStrictEqual(Object.keys(view), Object.keys(obj));
  });

  it('JSON.stringify matches', () => {
    const view = Person.view(packed);
    const obj = Person.unpack(packed);
    assert.strictEqual(JSON.stringify(view), JSON.stringify(obj));
  });

  it('"in" operator works for fields', () => {
    const view = Person.view(packed);
    assert.ok('name' in view);
    assert.ok('age' in view);
    assert.ok(!('nonexistent' in view));
  });

  it('view can be passed back to pack()', () => {
    const view = Person.view(packed);
    const repacked = Person.pack(view as any);
    const roundTripped = Person.unpack(repacked);
    assert.deepStrictEqual(roundTripped, { name: 'Alice', age: 30, active: true });
  });

  it('cached field returns same value on repeated access', () => {
    const view = Person.view(packed);
    const first = view.name;
    const second = view.name;
    assert.strictEqual(first, second);
    assert.strictEqual(first, 'Alice');
  });

  it('assignment works (write-through)', () => {
    const view = Person.view(packed);
    (view as any).name = 'Bob';
    assert.strictEqual(view.name, 'Bob');
  });

  it('readField returns correct values', () => {
    assert.strictEqual(Person.readField(packed, 'name'), 'Alice');
    assert.strictEqual(Person.readField(packed, 'age'), 30);
    assert.strictEqual(Person.readField(packed, 'active'), true);
  });

  it('rawField returns offset+length for strings, value for fixed', () => {
    const raw = Person.rawField(packed, 'name');
    assert.ok('offset' in raw && 'length' in raw, 'string field should return {offset, length}');
    assert.strictEqual((raw as any).length, 5); // 'Alice' = 5 bytes

    const rawAge = Person.rawField(packed, 'age');
    assert.ok('value' in rawAge, 'fixed field should return {value}');
    assert.strictEqual((rawAge as any).value, 30);

    const rawBool = Person.rawField(packed, 'active');
    assert.ok('value' in rawBool, 'bool field should return {value}');
    assert.strictEqual((rawBool as any).value, true);
  });
});

describe('fixedStruct view correctness', () => {
  const Point = fixedStruct({ x: f64, y: f64 });
  const packed = Point.pack({ x: 1.5, y: 2.5 });

  it('view and unpack produce identical results', () => {
    const view = Point.view(packed);
    const obj = Point.unpack(packed);
    assert.deepStrictEqual({ ...view }, obj);
  });

  it('partial access works', () => {
    const view = Point.view(packed);
    assert.strictEqual(view.x, 1.5);
    // y not accessed — no wasted decode
  });
});

describe('view performance: view beats unpack', () => {
  // Build a struct with many string fields — string decode is the expensive operation
  const BigStruct = struct({
    field0: str, field1: str, field2: str, field3: str, field4: str,
    field5: str, field6: str, field7: str, field8: str, field9: str,
  });

  const testData = {
    field0: 'The quick brown fox jumps over the lazy dog',
    field1: 'Lorem ipsum dolor sit amet, consectetur adipiscing elit',
    field2: 'Pack my box with five dozen liquor jugs',
    field3: 'How vexingly quick daft zebras jump',
    field4: 'The five boxing wizards jump quickly',
    field5: 'Sphinx of black quartz, judge my vow',
    field6: 'Two driven jocks help fax my big quiz',
    field7: 'The jay, pig, fox, zebra and my wolves quack',
    field8: 'Sympathizing would fix Quaker objectives',
    field9: 'A wizard\'s job is to vex chumps quickly in fog',
  };

  const packed = BigStruct.pack(testData);
  const ITERATIONS = 10_000;

  it('partial access: view is faster than unpack', () => {
    // unpack: decode ALL 10 string fields
    const unpackStart = performance.now();
    for (let i = 0; i < ITERATIONS; i++) {
      const obj = BigStruct.unpack(packed);
      void obj.field0; // only need one field
    }
    const unpackTime = performance.now() - unpackStart;

    // view: only decode field0 (9 fields untouched)
    const viewStart = performance.now();
    for (let i = 0; i < ITERATIONS; i++) {
      const v = BigStruct.view(packed);
      void v.field0; // only decode this one
    }
    const viewTime = performance.now() - viewStart;

    console.log(`  partial access (1/10 fields, ${ITERATIONS} iters): view=${viewTime.toFixed(2)}ms, unpack=${unpackTime.toFixed(2)}ms, ratio=${(unpackTime / viewTime).toFixed(2)}x`);
    assert.ok(viewTime < unpackTime, `view (${viewTime.toFixed(2)}ms) should be faster than unpack (${unpackTime.toFixed(2)}ms) for partial access`);
  });

  it('repeated access: cached view matches plain object speed', () => {
    const v = BigStruct.view(packed);
    void v.field0; // prime cache

    const start = performance.now();
    for (let i = 0; i < ITERATIONS * 10; i++) {
      void v.field0;
    }
    const cachedTime = performance.now() - start;

    const obj = BigStruct.unpack(packed);
    const objStart = performance.now();
    for (let i = 0; i < ITERATIONS * 10; i++) {
      void obj.field0;
    }
    const objTime = performance.now() - objStart;

    console.log(`  cached access (${ITERATIONS * 10} reads): view=${cachedTime.toFixed(2)}ms, plain=${objTime.toFixed(2)}ms`);
    // Proxy cached access has overhead; allow 10x
    assert.ok(cachedTime < objTime * 10, `cached view access (${cachedTime.toFixed(2)}ms) should be within 10x of plain object (${objTime.toFixed(2)}ms)`);
  });

  it('vec of structs: view beats unpack for partial access', () => {
    const PersonList = vec(BigStruct);
    const items = Array.from({ length: 100 }, () => testData);
    const packedVec = PersonList.pack(items);

    // unpack: decode all 100 elements, all 10 fields each
    const unpackStart = performance.now();
    for (let i = 0; i < 1000; i++) {
      const arr = PersonList.unpack(packedVec);
      void arr[0].field0;
    }
    const unpackTime = performance.now() - unpackStart;

    // view: create 100 proxy views but only decode field0 of element 0
    const viewStart = performance.now();
    for (let i = 0; i < 1000; i++) {
      const arr = PersonList.view(packedVec);
      void arr[0].field0;
    }
    const viewTime = performance.now() - viewStart;

    console.log(`  vec[100] single element access (1000 iters): view=${viewTime.toFixed(2)}ms, unpack=${unpackTime.toFixed(2)}ms, ratio=${(unpackTime / viewTime).toFixed(2)}x`);
    assert.ok(viewTime < unpackTime, `view (${viewTime.toFixed(2)}ms) should be faster than unpack (${unpackTime.toFixed(2)}ms) for vec partial access`);
  });

  it('readField: zero-allocation beats view and unpack', () => {
    // readField: no Proxy, no target object — just offset computation + decode
    const readFieldStart = performance.now();
    for (let i = 0; i < ITERATIONS; i++) {
      void BigStruct.readField(packed, 'field0');
    }
    const readFieldTime = performance.now() - readFieldStart;

    // view for comparison
    const viewStart = performance.now();
    for (let i = 0; i < ITERATIONS; i++) {
      const v = BigStruct.view(packed);
      void v.field0;
    }
    const viewTime = performance.now() - viewStart;

    // unpack for comparison
    const unpackStart = performance.now();
    for (let i = 0; i < ITERATIONS; i++) {
      const obj = BigStruct.unpack(packed);
      void obj.field0;
    }
    const unpackTime = performance.now() - unpackStart;

    console.log(`  readField vs view vs unpack (${ITERATIONS} iters): readField=${readFieldTime.toFixed(2)}ms, view=${viewTime.toFixed(2)}ms, unpack=${unpackTime.toFixed(2)}ms, readField/unpack=${(unpackTime / readFieldTime).toFixed(2)}x`);
    assert.ok(readFieldTime < viewTime, `readField (${readFieldTime.toFixed(2)}ms) should be faster than view (${viewTime.toFixed(2)}ms)`);
  });
});

describe('TypedArray vec views', () => {
  it('vec(u32) view returns correct values', () => {
    const IntList = vec(u32);
    const data = [1, 2, 3, 42, 0xFFFFFFFF];
    const packed = IntList.pack(data);
    const viewed = IntList.view(packed);
    assert.strictEqual(viewed.length, 5);
    assert.strictEqual(viewed[0], 1);
    assert.strictEqual(viewed[3], 42);
    assert.strictEqual(viewed[4], 0xFFFFFFFF);
  });

  it('vec(f64) view returns correct values', () => {
    const FloatList = vec(f64);
    const data = [1.5, -2.5, 0, Math.PI];
    const packed = FloatList.pack(data);
    const viewed = FloatList.view(packed);
    assert.strictEqual(viewed.length, 4);
    assert.strictEqual(viewed[0], 1.5);
    assert.strictEqual(viewed[1], -2.5);
    assert.strictEqual(viewed[3], Math.PI);
  });

  it('vec(i32) view returns correct signed values', () => {
    const IntList = vec(i32);
    const data = [-1, 0, 1, -2147483648, 2147483647];
    const packed = IntList.pack(data);
    const viewed = IntList.view(packed);
    assert.strictEqual(viewed[0], -1);
    assert.strictEqual(viewed[3], -2147483648);
    assert.strictEqual(viewed[4], 2147483647);
  });

  it('vec(u8) view returns correct values', () => {
    const ByteList = vec(u8);
    const data = [0, 127, 255];
    const packed = ByteList.pack(data);
    const viewed = ByteList.view(packed);
    assert.strictEqual(viewed.length, 3);
    assert.strictEqual(viewed[0], 0);
    assert.strictEqual(viewed[2], 255);
  });

  it('empty vec returns plain array', () => {
    const IntList = vec(u32);
    const packed = IntList.pack([]);
    const viewed = IntList.view(packed);
    assert.strictEqual(viewed.length, 0);
  });

  it('view is iterable with for...of', () => {
    const IntList = vec(u32);
    const packed = IntList.pack([10, 20, 30]);
    const viewed = IntList.view(packed);
    const result: number[] = [];
    for (const v of viewed) result.push(v);
    assert.deepStrictEqual(result, [10, 20, 30]);
  });

  it('vec(u32)[10000]: TypedArray view vs unpack', () => {
    const IntList = vec(u32);
    const items = Array.from({ length: 10_000 }, (_, i) => i);
    const packed = IntList.pack(items);

    const unpackStart = performance.now();
    for (let i = 0; i < 1000; i++) {
      const arr = IntList.unpack(packed);
      void arr[0];
    }
    const unpackTime = performance.now() - unpackStart;

    const viewStart = performance.now();
    for (let i = 0; i < 1000; i++) {
      const arr = IntList.view(packed);
      void arr[0];
    }
    const viewTime = performance.now() - viewStart;

    console.log(`  vec<u32>[10000] (1000 iters): view=${viewTime.toFixed(2)}ms, unpack=${unpackTime.toFixed(2)}ms, ratio=${(unpackTime / viewTime).toFixed(2)}x`);
    assert.ok(viewTime < unpackTime, `TypedArray view should be faster than unpack`);
  });
});
