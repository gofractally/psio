import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  PsszSchema, bool, generateSchema, list, rawString, record, u32, type PsszType,
  PJSON_WIRE_REVISION, pjsonDecode, pjsonEncode, pjsonValidate, PjsonDecimal, PjsonFloat,
} from '../index.js';

// The checked-in copy (tools/check-cpp-golden.py keeps it equal to C++) is
// the shared fixture, byte for byte.
const fixtureText = readFileSync(new URL('../../src/test/cpp-golden.json', import.meta.url), 'utf8');
const fixtures: { format: string; id: string; wire_hex: string }[] = JSON.parse(fixtureText);
test('cpp-golden.json: the JS copy is conformance/cpp-golden.json', () => {
  assert.equal(fixtureText, readFileSync(new URL('../../../conformance/cpp-golden.json', import.meta.url), 'utf8'));
});

// Every row is run or accounted for. The formats this package implements:
const RUN = ['pssz', 'key', 'json', 'pjson', 'pjson_f64', 'pjson_extra', 'pjson_typed', 'pjson_invalid', 'pjson_to_json'];
// Formats of the C++ library with no JavaScript implementation (the legacy
// fracpack rows, frac16 / frac32*, among them).
const NOT_IN_JS = [
  'ssz', 'frac32', 'frac16', 'frac32_invalid_strings', 'frac32_invalid_optionals', 'bin', 'borsh', 'bincode',
  'avro', 'capnp', 'wit', 'bson', 'msgpack', 'protobuf', 'flatbuf',
];
const ran = new Map<string, number>();
const note = (format: string) => ran.set(format, (ran.get(format) ?? 0) + 1);
test('cpp-golden.json: every row is run or is a format JS does not implement', () => {
  const counts = new Map<string, number>();
  for (const f of fixtures) counts.set(f.format, (counts.get(f.format) ?? 0) + 1);
  for (const format of counts.keys()) assert.ok(RUN.includes(format) || NOT_IN_JS.includes(format), `unclassified format ${format}`);
  const sum = (formats: string[]) => formats.reduce((n, f) => n + (counts.get(f) ?? 0), 0);
  assert.equal(sum(RUN) + sum(NOT_IN_JS), fixtures.length);
  console.log('cpp-golden.json:', JSON.stringify({
    rows: fixtures.length, run: sum(RUN), notImplementedInJs: sum(NOT_IN_JS),
    byFormat: Object.fromEntries(counts),
  }));
  assert.equal(fixtures.length, 391);
  assert.equal(sum(RUN), 345);
});

// The C++ record { u32 id; std::string name; std::vector<u32> values; bool active; },
// declared in the typed layer and, for the schema-driven path, written as IR.
const Record = record('Record', { id: u32, name: rawString, values: list(u32), active: bool });
const U32 = { Int: { bits: 32, isSigned: false } };
const fromIr: PsszType = new PsszSchema({
  Record: {
    Object: {
      id: U32,
      name: { Custom: { type: { List: { Int: { bits: 8, isSigned: false } } }, id: 'string' } },
      values: { List: U32 },
      active: { Custom: { type: { Int: { bits: 1, isSigned: false } }, id: 'bool' } },
    },
  },
}).type('Record');
const text = new TextEncoder();
const records = {
  record: { id: 42, name: text.encode('Alice'), values: [1, 256, 65536], active: true },
  empty_record: { id: 0, name: new Uint8Array(0), values: [], active: false },
};
const hex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');
for (const fixture of fixtures.filter((f) => ['pssz', 'key', 'json'].includes(f.format))) {
  test(`C++ ${fixture.format}/${fixture.id}: typed layer and schema-driven`, () => {
    note(fixture.format);
    const value = records[fixture.id as keyof typeof records];
    assert.ok(value, fixture.id);
    const expected = Uint8Array.from(Buffer.from(fixture.wire_hex, 'hex'));
    const viaSchema = generateSchema({ Record }).type('Record');
    for (const type of [Record, fromIr, viaSchema] as PsszType[]) {
      if (fixture.format === 'pssz') {
        assert.equal(hex(type.encode(value)), fixture.wire_hex);
        assert.deepEqual(type.decode(expected), value);
        assert.deepEqual(type.validate(expected), { ok: true, hasUnknown: false });
      } else if (fixture.format === 'key') {
        assert.equal(hex(type.encodeKey(value)), fixture.wire_hex);
        assert.deepEqual(type.decodeKey(expected), value);
        assert.deepEqual(type.validateKey(expected), { ok: true });
      } else {
        const json = Buffer.from(expected).toString('utf8');
        assert.equal(type.valueToJson(value, 'json'), json);
        assert.deepEqual(type.jsonToValue(json, 'json'), value);
        assert.deepEqual(type.jsonToValue(expected, 'ijson'), value);
      }
    }
    if (fixture.format === 'pssz') {
      const view = Record.view(expected);
      assert.equal(view.get('id'), value.id);
      assert.deepEqual(view.get('name'), value.name);
      assert.deepEqual([...view.get('values')], value.values);
      assert.equal(view.get('active'), value.active);
    }
  });
}
const pjson = { encode: pjsonEncode, decode: pjsonDecode, validate: pjsonValidate };
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
const pvalues = {
  null: null, true: true, false: false, uint_inline: 5n, uint: 256n,
  negative: -5n, string: 'hello', decimal: new PjsonDecimal(12345n, -2),
  float: 1/7, bytes: Uint8Array.of(0,127,255), array: [1n,'hi',true],
  object: new Map<string, any>([['id',42n],['name','Alice']]),
};
for (const fixture of fixtures.filter(f => f.format === 'pjson')) {
  test(`C++ pjson/${fixture.id}`, () => {
    note('pjson');
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
    note('pjson_f64');
    const data = new DataView(new ArrayBuffer(8)); data.setBigUint64(0, BigInt(f.id), true);
    const input = data.getFloat64(0, true);
    assert.equal(Buffer.from(pjson.encode(input)).toString('hex'), f.wire_hex, `${input} (${f.id})`);
  }
});
for (const f of fixtures.filter(f => ['pjson_extra','pjson_typed'].includes(f.format))) {
  test(`C++ ${f.format}/${f.id}`, () => {
    note(f.format);
    const bytes = Buffer.from(f.wire_hex, 'hex');
    assert.equal(pjson.validate(bytes), true);
    assert.equal(Buffer.from(pjson.encode(pjson.decode(bytes))).toString('hex'), f.wire_hex);
  });
}
for (const f of fixtures.filter(f => f.format === 'pjson_invalid')) {
  test(`C++ rejection/${f.id}`, () => {
    note('pjson_invalid');
    const bytes = Buffer.from(f.wire_hex, 'hex');
    assert.equal(pjson.validate(bytes), false); assert.throws(() => pjson.decode(bytes));
  });
}
// The rows that pin the JSON text of a pjson value (gofractally/psio#17).
// This package has no pjson-to-JSON-text function, so what runs here is the
// byte side: each frame validates, decodes, and encodes back to itself.
for (const f of fixtures.filter(f => f.format === 'pjson_to_json')) {
  test(`C++ pjson_to_json/${f.id}`, () => {
    note('pjson_to_json');
    const bytes = Buffer.from(f.wire_hex, 'hex');
    assert.equal(pjson.validate(bytes), true);
    assert.equal(Buffer.from(pjson.encode(pjson.decode(bytes))).toString('hex'), f.wire_hex);
  });
}
test('pjson: minus zero keeps its sign; null has one byte value', () => {
  assert.deepEqual(pjson.encode(-0), Uint8Array.of(0x61, 0x00, 0x80));
  assert.deepEqual(pjson.encode(0), Uint8Array.of(0x20));
  assert.deepEqual(pjson.encode([-0]), Uint8Array.from(Buffer.from('b000610080000100', 'hex')));
  for (const hex of ['01', '0f', 'b00001000100']) {
    assert.equal(pjson.validate(Buffer.from(hex, 'hex')), false, hex);
    assert.throws(() => pjson.decode(Buffer.from(hex, 'hex')), hex);
  }
});

// Last: the tests above have run.
test('cpp-golden.json: every runnable row ran', () => {
  for (const format of RUN)
    assert.equal(ran.get(format) ?? 0, fixtures.filter((f) => f.format === format).length, format);
});
