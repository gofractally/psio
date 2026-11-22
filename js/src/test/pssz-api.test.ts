// Unit tests of the schema-driven PSSZ / JSON API on hand-written schemas:
// what the shared fixtures do not pin (big integers in text, float text,
// depth and allocation limits, JavaScript-specific values).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { PsszError, PsszNaN, PsszSchema, jsonToPssz, psszToJson, validatePssz } from '../index.js';

const hex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');
const fromHex = (text: string) => Uint8Array.from(Buffer.from(text, 'hex'));
const U8 = { Int: { bits: 8, isSigned: false } };
const U32 = { Int: { bits: 32, isSigned: false } };
const U64 = { Int: { bits: 64, isSigned: false } };
const I64 = { Int: { bits: 64, isSigned: true } };
const F32 = { Float: { exp: 8, mantissa: 23 } };
const F64 = { Float: { exp: 11, mantissa: 52 } };
const STRING = { Custom: { type: { List: U8 }, id: 'utf8' } };
const BYTES = { Custom: { type: { List: U8 }, id: 'hex' } };

const schema = new PsszSchema({
  Greeting: { Object: { number: U32, message: STRING } },
  Transfer: {
    Object: {
      members: [
        { name: 'from', type: STRING },
        { name: 'amount', type: U64 },
        { name: 'memo', type: { Option: STRING } },
        { name: 'id', type: U64, attributes: [{ name: 'ijsonInt', value: 'number' }] },
      ],
    },
  },
  Tree: { Object: { children: { List: 'Tree' } } },
  Shape: { Variant: { Circle: F64, Label: STRING } },
  Nested: { Option: { Option: U32 } },
  Odd: { Object: { members: [{ name: '__proto__', type: U32 }, { name: 'some', type: { Option: U32 } }] } },
});

function throwsTyped(run: () => unknown, kind: string, pattern?: RegExp): void {
  assert.throws(run, (e: unknown) => {
    assert.ok(e instanceof PsszError, String(e));
    assert.equal(e.kind, kind, e.message);
    if (pattern) assert.match(e.message, pattern);
    return true;
  });
}

test('pssz: the spec 5.1.4 example', () => {
  const Greeting = schema.type('Greeting');
  const bytes = Greeting.encode({ number: 42, message: 'hi' });
  assert.equal(hex(bytes), '080000002a000000080000006869');
  assert.deepEqual(Greeting.decode(bytes), { number: 42, message: 'hi' });
  assert.deepEqual(Greeting.validate(bytes), { ok: true, hasUnknown: false });
  assert.equal(Greeting.width, 4);
  assert.equal(hex(schema.type('Greeting', { width: 1 }).encode({ number: 42, message: 'hi' })), '052a000000056869');
});

test('pssz: pruning, header-driven reads and unknown fields', () => {
  const Transfer = schema.type('Transfer');
  const value = { from: 'a', amount: 1n, memo: null, id: 2n };
  const bytes = Transfer.encode(value);
  assert.deepEqual(Transfer.decode(bytes), value);
  // An older writer's frame (no `memo`, no `id`) is not a Transfer: `id` is required.
  const V1 = new PsszSchema({ T: { Object: { from: STRING, amount: U64, memo: { Option: STRING } } } }).type('T');
  const V2 = new PsszSchema({
    T: { Object: { from: STRING, amount: U64, memo: { Option: STRING }, extra: { Option: U32 } } },
  }).type('T');
  const old = V1.encode({ from: 'a', amount: 1n });
  assert.equal(hex(old), '0c0000000c000000010000000000000061');
  assert.deepEqual(V2.decode(old), { from: 'a', amount: 1n, memo: null, extra: null });
  const newer = V2.encode({ from: 'a', amount: 1n, memo: null, extra: 7 });
  assert.deepEqual(V1.validate(newer), { ok: true, hasUnknown: true });
  assert.deepEqual(V1.decode(newer), { from: 'a', amount: 1n, memo: null });
  throwsTyped(() => V1.decode(newer, { rejectUnknown: true }), 'bytes', /unknown fields/);
  // A stored trailing None is rejected by every reader.
  const stored = fromHex('10000000' + '10000000' + '0100000000000000' + '11000000' + '61' + '00');
  throwsTyped(() => V1.decode(stored), 'bytes', /not pruned/);
  assert.equal(V1.validate(stored).ok, false);
});

test('json: 64-bit integers are exact in both formats', () => {
  const Transfer = schema.type('Transfer');
  const max = 18446744073709551615n;
  const value = { from: 'alice', amount: max, memo: 'x', id: max };
  assert.equal(
    Transfer.valueToJson(value, 'json'),
    '{"from":"alice","amount":18446744073709551615,"memo":"x","id":18446744073709551615}',
  );
  assert.equal(
    Transfer.valueToJson(value, 'ijson'),
    '{"from":"alice","amount":"18446744073709551615","memo":"x","id":18446744073709551615}',
  );
  for (const format of ['json', 'ijson'] as const)
    assert.deepEqual(Transfer.jsonToValue(Transfer.valueToJson(value, format), format), value);
  // The safe-integer boundary of the natural I-JSON form.
  const amount = schema.type(U64);
  assert.equal(amount.valueToJson(9007199254740991n), '9007199254740991');
  assert.equal(amount.valueToJson(9007199254740992n), '"9007199254740992"');
  assert.equal(amount.jsonToValue('"7"'), 7n);
  assert.equal(amount.jsonToValue('9007199254740993', 'json'), 9007199254740993n);
  throwsTyped(() => amount.jsonToValue('9007199254740992'), 'json', /safe range/);
  throwsTyped(() => amount.jsonToValue('18446744073709551616', 'json'), 'json');
  throwsTyped(() => amount.jsonToValue('"7"', 'json'), 'json');
  for (const bad of ['-0', '01', '1.0', '1e2', '+1', '"\\u0037"', '" 7"'])
    throwsTyped(() => amount.jsonToValue(bad), 'json');
  const signed = schema.type(I64);
  assert.equal(signed.valueToJson(-9223372036854775808n), '"-9223372036854775808"');
  assert.equal(signed.valueToJson(-9223372036854775808n, 'json'), '-9223372036854775808');
  assert.equal(signed.jsonToValue('-9223372036854775808', 'json'), -9223372036854775808n);
  assert.equal(hex(signed.jsonToPssz('"-9223372036854775808"')), '0000000000000080');
  // A number is accepted where a bigint is expected when it is exact.
  assert.equal(hex(amount.encode(5)), '0500000000000000');
  throwsTyped(() => amount.encode(2 ** 60), 'value');
  throwsTyped(() => amount.encode(-1n), 'value');
  throwsTyped(() => schema.type(U32).encode(4294967296), 'value');
});

test('json: bytes, variants, duplicate keys, unknown keys', () => {
  const blob = schema.type(BYTES);
  assert.equal(blob.valueToJson(Uint8Array.of(1, 255)), '"01FF"');
  assert.equal(blob.valueToJson(Uint8Array.of(1, 255), 'json'), '[1,255]');
  assert.deepEqual(blob.jsonToValue('"01ff"'), Uint8Array.of(1, 255));
  throwsTyped(() => blob.jsonToValue('"01f"'), 'json', /odd-length/);
  const Shape = schema.type('Shape');
  assert.equal(Shape.valueToJson({ type: 'Label', value: 'x' }), '{"Label":"x"}');
  assert.equal(Shape.valueToJson({ type: 'Label', value: 'x' }, 'json'), '[1,"x"]');
  assert.deepEqual(Shape.jsonToValue('[0,1.5]', 'json'), { type: 'Circle', value: 1.5 });
  throwsTyped(() => Shape.jsonToValue('{"Label":"x","Label":"y"}'), 'json', /one key/);
  const Greeting = schema.type('Greeting');
  assert.deepEqual(Greeting.jsonToValue('{"number":1,"zzz":[{"a":null}],"message":"a","number":2}'), {
    number: 2,
    message: 'a',
  });
  throwsTyped(() => Greeting.jsonToValue('{"number":"x","number":2,"message":"a"}'), 'json');
  throwsTyped(() => Greeting.jsonToValue('{"number":1}'), 'json', /missing required member/);
  assert.equal(psszToJson(schema, 'Greeting', 'json', jsonToPssz(schema, 'Greeting', 'ijson', '{"message":"\\u00e9","number":3}')),
    '{"number":3,"message":"é"}');
});

test('json: float text is std::to_chars shortest round-trip', () => {
  const f64 = schema.type(F64);
  const f32 = schema.type(F32);
  const cases: [number, string][] = [
    [0.1, '0.1'], [1e21, '1e+21'], [1e-7, '1e-07'], [-0, '-0'], [0, '0'], [100000, '1e+05'], [123456, '123456'],
    [1.5e300, '1.5e+300'], [5e-324, '5e-324'], [2 ** 70, '1180591620717411303424'], [1e22, '1e+22'],
    [-2.5, '-2.5'], [2 ** -24, '5.960464477539063e-08'], [0.001, '0.001'], [0.0001, '1e-04'], [0.00001, '1e-05'], [1234.5, '1234.5'], [1 / 3, '0.3333333333333333'],
    [9007199254740993, '9007199254740992'], [1.7976931348623157e308, '1.7976931348623157e+308'],
  ];
  for (const [value, text] of cases) {
    assert.equal(f64.valueToJson(value), text);
    assert.ok(Object.is(f64.jsonToValue(text), value), text);
  }
  const singles: [number, string][] = [
    [Math.fround(0.1), '0.1'], [16777216, '16777216'], [3.4028234663852886e38, '3.4028235e+38'],
    [Math.fround(1e-45), '1e-45'], [Math.fround(9.5367431640625e-7), '9.536743e-07'], [1.5, '1.5'],
    [Math.fround(1 / 3), '0.33333334'],
  ];
  for (const [value, text] of singles) {
    assert.equal(f32.valueToJson(value), text);
    assert.ok(Object.is(f32.jsonToValue(text), value), text);
  }
  throwsTyped(() => f64.valueToJson(NaN), 'value', /no JSON text/);
  throwsTyped(() => f64.valueToJson(Infinity, 'json'), 'value', /no JSON text/);
  for (const bad of ['NaN', 'Infinity', '1e999', '"1"', '.5', '1.', '0x10'])
    throwsTyped(() => f64.jsonToValue(bad), 'json');
  assert.equal(f64.jsonToValue('1e-999'), 0);
  // binary32 is rounded once, from the decimal: a text a hair above the
  // midpoint of two floats rounds up although its double is the midpoint.
  assert.equal(hex(f32.jsonToPssz('1.0000000596046447753906250000000000001')), '0100803f');
  assert.equal(hex(f32.jsonToPssz('1.000000059604644775390625')), '0000803f');
  assert.equal(hex(f32.jsonToPssz('1.00000005960464477539062499999')), '0000803f');
  assert.equal(hex(f32.jsonToPssz('-1.0000000596046447753906250000000000001e0')), '010080bf');
  assert.equal(hex(f32.jsonToPssz('340282356779733661637539395458142568447')), 'ffff7f7f');
  throwsTyped(() => f32.jsonToPssz('340282356779733661637539395458142568448'), 'json', /out of range/);
  throwsTyped(() => f32.jsonToPssz('1e39'), 'json', /out of range/);
});

test('pssz: NaN payloads survive, plain NaN is the quiet NaN', () => {
  const f64 = schema.type(F64);
  const f32 = schema.type(F32);
  assert.equal(hex(f64.encode(NaN)), '000000000000f87f');
  assert.ok(Number.isNaN(f64.decode(fromHex('000000000000f87f'))));
  for (const [type, bits] of [[f64, '010000000000f07f'], [f64, '000000000000f8ff'], [f32, '0100807f'], [f32, '0000c0ff']] as const) {
    const value = type.decode(fromHex(bits));
    assert.ok(value instanceof PsszNaN && Number.isNaN(Number(value)));
    assert.equal(hex(type.encode(value)), bits);
  }
});

test('pssz: depth, cycles and claimed lengths are bounded', () => {
  const Tree = schema.type('Tree');
  // A frame nested `levels` deep: each level is header + one slot + a list of one.
  const frame = (levels: number): Uint8Array => {
    let bytes = fromHex('0400000004000000');
    for (let i = 0; i < levels; ++i) {
      const out = new Uint8Array(12 + bytes.length);
      out.set(fromHex('040000000400000004000000'));
      out.set(bytes, 12);
      bytes = out;
    }
    return bytes;
  };
  assert.equal(Tree.validate(frame(31)).ok, true);
  const deep = Tree.validate(frame(40));
  assert.ok(!deep.ok && /depth limit/.test(deep.error.message));
  assert.equal(Tree.validate(frame(3000)).ok, false);
  // A cyclic JavaScript value is an error, not a stack overflow.
  const cyclic: { children: unknown[] } = { children: [] };
  cyclic.children.push(cyclic);
  throwsTyped(() => Tree.encode(cyclic), 'value', /depth limit/);
  throwsTyped(() => Tree.valueToJson(cyclic), 'value', /depth limit/);
  // Depth bombs in text, typed and skipped.
  throwsTyped(() => Tree.jsonToValue('['.repeat(5000), 'json'), 'json');
  throwsTyped(() => Tree.jsonToValue('{"children":['.repeat(5000)), 'json', /max depth/);
  throwsTyped(() => Tree.jsonToValue('{"zzz":' + '['.repeat(5000)), 'json', /max depth/);
  assert.deepEqual(Tree.jsonToValue('{"zzz":' + '['.repeat(63) + ']'.repeat(63) + ',"children":[]}'), { children: [] });
  // A claimed element count is checked against the input before any work.
  const strings = schema.type({ List: STRING });
  assert.equal(strings.validate(fromHex('fcffffff')).ok, false);
  assert.equal(strings.validate(fromHex('04000000')).ok, true);
  const words = schema.type({ List: U64 });
  assert.equal(words.validate(new Uint8Array(7)).ok, false);
  // A schema that is infinitely large is an error at use, not a hang.
  const endless = new PsszSchema({ A: { Struct: { next: { Array: { type: 'A', len: 2 } } } } }).type('A');
  assert.equal(endless.validate(new Uint8Array(4)).ok, false);
  throwsTyped(() => endless.encode({ next: [] }), 'schema');
});

test('pssz: JavaScript-specific values', () => {
  const Nested = schema.type('Nested');
  assert.equal(hex(Nested.encode(null)), '00');
  assert.equal(hex(Nested.encode({ some: null })), '01');
  assert.equal(hex(Nested.encode({ some: 5 })), '0105000000');
  assert.deepEqual(Nested.decode(fromHex('01')), { some: null });
  assert.deepEqual(Nested.decode(fromHex('0105000000')), { some: 5 });
  throwsTyped(() => Nested.encode(5), 'value');
  const text = schema.type(STRING);
  throwsTyped(() => text.encode('\ud800'), 'value', /UTF-8/);
  throwsTyped(() => text.jsonToValue('"\ud800"'), 'json');
  throwsTyped(() => text.jsonToValue('"\\ud800"'), 'json', /surrogate/);
  assert.equal(text.jsonToValue('"\\ud83d\\ude00"'), '😀');
  assert.equal(text.decode(fromHex('efbbbf61')), '﻿a');
  assert.equal(hex(text.encode('﻿a')), 'efbbbf61');
  throwsTyped(() => text.decode(fromHex('c080')), 'bytes', /UTF-8/);
  throwsTyped(() => text.jsonToValue('"￾"'), 'json', /noncharacter/);
  assert.equal(text.jsonToValue('"￾"', 'json'), '￾');
  assert.equal(text.valueToJson('a"\\\n\u0001\u007f/'), '"a\\"\\\\\\n\\u0001\u007f/"');
  const Odd = schema.type('Odd');
  const odd = Odd.decode(Odd.jsonToPssz('{"__proto__":7,"some":null}')) as Record<string, unknown>;
  assert.equal(Object.getPrototypeOf(odd), Object.prototype);
  assert.deepEqual(Object.keys(odd), ['__proto__', 'some']);
  assert.equal(Odd.valueToJson(odd), '{"__proto__":7,"some":null}');
  throwsTyped(() => schema.type('Greeting').encode({ number: 1, message: 'a', extra: 1 }), 'value', /unknown record value/);
  throwsTyped(() => schema.type('Greeting').encode({ number: 1 }), 'value', /missing required/);
  throwsTyped(() => schema.type('Greeting').decode('00' as never), 'bytes');
  // Decoded bytes are copies, also when the input is a Node Buffer.
  const blob = schema.type(BYTES);
  const input = Buffer.from('0102', 'hex');
  const copy = blob.decode(input) as Uint8Array;
  input[0] = 9;
  assert.deepEqual(copy, Uint8Array.of(1, 2));
  assert.equal(copy.constructor, Uint8Array);
});

test('schema: errors are typed, schemas travel as PSSZ and I-JSON', () => {
  throwsTyped(() => schema.type('NoSuchType'), 'schema', /unresolved/);
  const missing = validatePssz(schema, 'NoSuchType', new Uint8Array(1));
  assert.ok(!missing.ok && missing.error.kind === 'schema');
  throwsTyped(() => new PsszSchema({ A: 'A' }).type('A'), 'schema', /cyclic/);
  throwsTyped(() => new PsszSchema({ A: { Int: { bits: 24, isSigned: false } } }).type('A'), 'schema');
  throwsTyped(() => new PsszSchema({ A: { Bogus: 1 } as never }), 'schema');
  throwsTyped(() => schema.type('Greeting', { width: 3 as never }), 'schema');
  const bytes = schema.encode();
  const again = PsszSchema.decode(bytes);
  assert.deepEqual(again.ir, schema.ir);
  const Schema = PsszSchema.meta().type('Schema');
  const text = Schema.psszToJson(bytes);
  assert.deepEqual(PsszSchema.fromJson(text).ir, schema.ir);
  assert.equal(hex(again.type('Transfer').jsonToPssz('{"from":"a","amount":1,"id":2}')),
    hex(schema.type('Transfer').encode({ from: 'a', amount: 1n, memo: null, id: 2n })));
  throwsTyped(() => PsszSchema.decode(bytes.subarray(0, bytes.length - 1)), 'bytes');
});
