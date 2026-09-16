import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { struct, u32, str, vec, bool, optional, variant, validate, PJSON_WIRE_REVISION, createPjson, PjsonDecimal, PjsonFloat } from '../index.js';

const fixtures: { format: string; id: string; wire_hex: string }[] = JSON.parse(
  readFileSync(new URL('../../src/test/cpp-golden.json', import.meta.url), 'utf8'));
const record = struct({ id: u32, name: str, values: vec(u32), active: bool });
const envelope = struct({ head: record, rows: vec(record), note: optional(str), choice: variant({ Number: u32, Text: str }) });
const cases: Record<string, [any, any]> = {
  record: [record, { id: 42, name: 'Alice', values: [1,256,65536], active: true }],
  empty_record: [record, { id: 0, name: '', values: [], active: false }],
  u32: [u32, 0xdeadbeef], string: [str, 'hello'], vector: [vec(u32), [1,2,3]],
  nested: [envelope, { head: {id:42,name:'Alice',values:[1,256,65536],active:true},
    rows: [{id:0,name:'',values:[],active:false},{id:7,name:'Bob',values:[9],active:true}],
    note: 'hello', choice: { type:'Text', value:'chosen' } }],
  nested_empty: [envelope, { head:{id:0,name:'',values:[],active:false}, rows:[], note:null, choice:{type:'Number',value:7} }],
  string_vector: [vec(str), ['', 'hello', 'world']],
  optional_vector: [vec(optional(str)), [null, '', 'hello']],
  optional_empty_string: [optional(str), ''], optional_empty_vector: [optional(vec(u32)), []],
  optional_missing_vector: [optional(vec(u32)), null],
  optional_some: [optional(u32), 42], optional_none: [optional(u32), null],
};
for (const fixture of fixtures.filter(f => f.format === 'frac32')) {
  test(`C++ fracpack/${fixture.id}`, () => {
    const [type, value] = cases[fixture.id];
    const expected = Uint8Array.from(Buffer.from(fixture.wire_hex, 'hex'));
    assert.deepEqual(type.pack(value), expected);
    assert.deepEqual(type.unpack(expected), value);
    assert.deepEqual(type.pack(type.view(expected)), expected);
    assert.equal(validate(type, expected).status, 'valid');
  });
}
const pjson = await createPjson();
test('pjson revision 2 explicit integer tags', () => {
  assert.equal(PJSON_WIRE_REVISION, 2);
  for (let n = -15; n <= 15; ++n) {
    const tag = n < 0 ? 0x30 | -n : 0x20 | n;
    assert.deepEqual(pjson.encode(BigInt(n)), Uint8Array.of(tag));
    assert.deepEqual(pjson.encode(n), Uint8Array.of(tag));
    assert.equal(pjson.decode(Uint8Array.of(tag)), BigInt(n));
  }
  assert.equal(pjson.decode(Uint8Array.of(0x35)), -5n);
});
for (const fixture of fixtures.filter(f => f.format.startsWith('frac32_invalid_'))) {
  test(`C++ ${fixture.format}/${fixture.id}`, () => {
    const type = fixture.format === 'frac32_invalid_strings' ? vec(str) : vec(optional(str));
    const bytes = Uint8Array.from(Buffer.from(fixture.wire_hex, 'hex'));
    assert.equal(validate(type, bytes).status, 'invalid');
    assert.throws(() => type.unpack(bytes));
  });
}
const pvalues = {
  null: null, true: true, false: false, uint_inline: 5n, uint: 256n,
  negative: -5n, string: 'hello', decimal: new PjsonDecimal(12345n, -2),
  float: 1/7, bytes: Uint8Array.of(0,127,255), array: [1n,'hi',true],
  object: new Map<string, any>([['id',42n],['name','Alice']]),
};
for (const fixture of fixtures.filter(f => f.format === 'pjson')) {
  test(`C++ pjson/${fixture.id}`, () => {
    const expected = Uint8Array.from(Buffer.from(fixture.wire_hex, 'hex'));
    const value = pvalues[fixture.id as keyof typeof pvalues];
    assert.deepEqual(pjson.encode(value), expected);
    assert.equal(pjson.validate(expected), true);
    assert.deepEqual(pjson.encode(pjson.decode(expected)), expected);
  });
}
test('pjson boundary and malformed input', () => {
  for (const value of [-(1n<<127n), (1n<<128n)-1n, 0n, -1n, 15n, 16n])
    assert.equal(pjson.decode(pjson.encode(value)), value);
  for (const hex of ['', '30', '5000', '3500', '8025', 'd0', 'e0', '1200', 'a100', '6400000000000000000000000000000000', 'b0000100', 'c0000100'])
    assert.equal(pjson.validate(Buffer.from(hex, 'hex')), false, hex);
  assert.throws(() => pjson.encode((1n<<128n)), /128/);
  assert.throws(() => pjson.encode(new Array(65536).fill(null)), /65535/);
  let nested: any = null; for (let n = 0; n < 65; ++n) nested = [nested];
  assert.throws(() => pjson.encode(nested), /depth/);
  const nan = pjson.encode(new PjsonFloat(0x7ff0000000000001n));
  assert.equal(Buffer.from(nan).toString('hex'), '63000000000000f87f');
});

test('C++ pjson numeric encoding across exponents', () => {
  for (const f of fixtures.filter(f => f.format === 'pjson_f64')) {
    const data = new DataView(new ArrayBuffer(8)); data.setBigUint64(0, BigInt(f.id), true);
    const input = data.getFloat64(0, true);
    assert.equal(Buffer.from(pjson.encode(input)).toString('hex'), f.wire_hex, `${input} (${f.id})`);
  }
});
for (const f of fixtures.filter(f => ['pjson_extra','pjson_typed'].includes(f.format))) {
  test(`C++ ${f.format}/${f.id}`, () => {
    const bytes = Buffer.from(f.wire_hex, 'hex');
    assert.equal(pjson.validate(bytes), true);
    assert.equal(Buffer.from(pjson.encode(pjson.decode(bytes))).toString('hex'), f.wire_hex);
  });
}
for (const f of fixtures.filter(f => f.format === 'pjson_invalid')) {
  test(`C++ rejection/${f.id}`, () => {
    const bytes = Buffer.from(f.wire_hex, 'hex');
    assert.equal(pjson.validate(bytes), false); assert.throws(() => pjson.decode(bytes));
  });
}
