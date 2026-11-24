// The record member attributes of the numbered formats (protobuf, Cap'n
// Proto, FlatBuffers): `field`, `default`, `deprecated`. The schema-driven
// codec accepts them on a record member and does not read them; the typed
// layer writes them from the member options `number`, `default` and
// `deprecated`.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  PsszError, PsszSchema, bool, enumeration, f32, f64, field, generateSchema, i16, i32, list, method, optional,
  psszEquivalent, psszEvolvesFrom, record, transparent, u128, u32, u64, u8, utf8,
  type PsszTypeInput,
} from '../index.js';

const U8: PsszTypeInput = { Int: { bits: 8, isSigned: false } };
const I32: PsszTypeInput = { Int: { bits: 32, isSigned: true } };
const STRING: PsszTypeInput = { Custom: { type: { List: U8 }, id: 'utf8' } };
const UNSUPPORTED = /^pssz IR: unsupported schema constraint or annotation$/;
type Attributes = { name: string; value: string }[];

function rejects(run: () => unknown, pattern: RegExp, what = ''): void {
  assert.throws(run, (e: unknown) => {
    assert.ok(e instanceof PsszError, `${what}: ${String(e)}`);
    assert.equal(e.kind, 'schema', `${what}: ${e.message}`);
    assert.match(e.message, pattern, what);
    return true;
  }, what);
}

const row = (id: Attributes, name: Attributes): PsszTypeInput => ({
  Object: { members: [{ name: 'id', type: I32, attributes: id }, { name: 'name', type: STRING, attributes: name }], attributes: [] },
});

test('schema-driven codec: field / default / deprecated on a record member change nothing', () => {
  const schema = new PsszSchema({});
  const stated = row(
    [{ name: 'field', value: '7' }, { name: 'deprecated', value: '' }, { name: 'default', value: '-1' }],
    [{ name: 'lengthBound', value: 'max=9' }, { name: 'field', value: '2' }, { name: 'deprecated', value: 'v2' }],
  );
  const bounded = row([], [{ name: 'lengthBound', value: 'max=9' }]);
  const plain = row([], []);
  const value = { id: -5, name: 'abc' };
  for (const [other, options] of [
    [bounded, undefined], [plain, { width: 1 }], [plain, { width: 2 }], [plain, { width: 4 }],
  ] as const) {
    const a = schema.type(stated, options);
    const b = schema.type(other, options);
    assert.equal(a.width, b.width);
    const bytes = a.encode(value);
    assert.deepEqual(bytes, b.encode(value));
    assert.deepEqual(a.decode(bytes), value);
    assert.deepEqual(a.validate(bytes), b.validate(bytes));
    for (const format of ['json', 'ijson'] as const) {
      assert.equal(a.valueToJson(value, format), b.valueToJson(value, format));
      assert.deepEqual(a.jsonToValue(a.valueToJson(value, format), format), value);
    }
    // The same type to a comparison, either way round.
    if (options) continue;
    for (const [x, y] of [[a, b], [b, a]] as const) {
      assert.deepEqual(psszEquivalent(x, y), { ok: true });
      assert.deepEqual(psszEvolvesFrom(x, y), { ok: true });
    }
  }
  assert.deepEqual(schema.type(stated).encodeKey(value), schema.type(bounded).encodeKey(value));
});

test('schema-driven codec: field / default / deprecated belong to record members only', () => {
  const schema = new PsszSchema({});
  const unit: PsszTypeInput = { Object: { members: [], attributes: [] } };
  for (const name of ['field', 'default', 'deprecated']) {
    const stated = [{ name, value: '1' }];
    const cases: [string, PsszTypeInput][] = [
      ['record', { Object: { members: [], attributes: stated } }],
      ['fixed record', { Struct: { members: [], attributes: stated } }],
      ['variant', { Variant: { members: [{ name: 'a', type: unit, attributes: [] }], attributes: stated } }],
      ['variant member', { Variant: { members: [{ name: 'a', type: unit, attributes: stated }], attributes: [] } }],
    ];
    for (const [what, type] of cases)
      for (const options of [undefined, { width: 4 }] as const)
        rejects(() => schema.type(type, options), UNSUPPORTED, `${name} on a ${what}`);
  }
  // An unknown name on a record member is rejected as before.
  rejects(() => schema.type(row([{ name: 'fieldNumber', value: '1' }], [])), UNSUPPORTED);
});

const Status = enumeration(i32, { pending: -1, idle: 0, active: 2 });
const attributesOf = (def: Parameters<typeof generateSchema>[0][string], name: string) => {
  const type = generateSchema({ [name]: def }).ir.types.find((t) => t.name === name)!.type.value;
  assert.ok(type.type === 'Object' || type.type === 'Struct');
  return Object.fromEntries(type.value.members.map((m) => [m.name, m.attributes.map((a) => [a.name, a.value])]));
};

test('typed layer: number / default / deprecated are the member attributes field / default / deprecated', () => {
  const Stated = record('Stated', {
    id: field(u32, { number: 3 }),
    old: field(u8, { deprecated: true }),
    count: field(u64, { default: 7n, deprecated: 'v2', number: 1, jsonInt: 'string' }),
    tags: field(list(u8), { lengthBound: { min: 1 }, number: 9 }),
    on: field(bool, { default: true }),
    low: field(i16, { default: -40 }),
    status: field(Status, { default: -1 }),
    half: field(f32, { default: 0.5 }),
    bottom: field(f64, { default: -Infinity }),
    // A zero default is the absence of one; so is `deprecated: false`.
    zero: field(u32, { default: 0 }),
    off: field(bool, { default: false, deprecated: false }),
    idle: field(Status, { default: 0n }),
    nothing: field(f64, { default: 0 }),
    plain: utf8,
  });
  assert.deepEqual(attributesOf(Stated, 'Stated'), {
    id: [['field', '3']],
    old: [['deprecated', '']],
    count: [['jsonInt', 'string'], ['field', '1'], ['deprecated', 'v2'], ['default', '7']],
    tags: [['lengthBound', 'min=1'], ['field', '9']],
    on: [['default', 'true']],
    low: [['default', '-40']],
    status: [['default', '-1']],
    half: [['default', '0.5']],
    bottom: [['default', '-inf']],
    zero: [], off: [], idle: [], nothing: [], plain: [],
  });

  // The typed codec and the schema-driven one take the declaration as it is:
  // the same bytes as the record without the options.
  const Plain = record('Stated', {
    id: u32, old: u8, count: field(u64, { jsonInt: 'string' }), tags: field(list(u8), { lengthBound: { min: 1 } }), on: bool,
    low: i16, status: Status, half: f32, bottom: f64, zero: u32, off: bool, idle: Status, nothing: f64, plain: utf8,
  });
  const value = {
    id: 1, old: 2, count: 3n, tags: [4], on: true, low: -5, status: 2, half: 1.5, bottom: 2.5, zero: 6, off: false,
    idle: -1, nothing: 0, plain: 'p',
  };
  const bytes = Stated.encode(value);
  assert.deepEqual(bytes, Plain.encode(value));
  assert.deepEqual(Stated.decode(bytes), value);
  assert.equal(Stated.valueToJson(value), Plain.valueToJson(value));
  assert.deepEqual(generateSchema({ Stated }).type('Stated').encode(value), bytes);
  assert.deepEqual(psszEquivalent(Stated, Plain), { ok: true });
});

test('typed layer: the text of a float default is the shortest (C++ std::to_chars)', () => {
  const text = (type: typeof f64, value: number) =>
    attributesOf(record('F', { x: field(type, { default: value }) }), 'F').x.map((a) => a[1])[0] ?? '';
  for (const [value, expected] of [
    [0, ''], [-0, '-0'], [NaN, 'nan'], [Infinity, 'inf'], [-Infinity, '-inf'], [1, '1'], [-2.5, '-2.5'], [0.1, '0.1'],
    [1234.5, '1234.5'], [10000, '10000'], [100000, '1e+05'], [123456, '123456'], [1e22, '1e+22'], [1.5e300, '1.5e+300'],
    [0.001, '0.001'], [0.0001, '1e-04'], [0.00015, '0.00015'], [1.25e-7, '1.25e-07'], [-3e-310, '-3e-310'],
    [12345678901234567890, '12345678901234567168'],
  ] as const) assert.equal(text(f64, value), expected, String(value));
  assert.equal(text(f32, 0.1), '0.1');
  assert.equal(text(f32, 16777216), '16777216');
  assert.equal(text(f32, 3.4028234663852886e38), '3.4028235e+38');
  assert.equal(text(f32, 1e-46), '');
});

test('typed layer: number / default / deprecated are checked, and are a record member\'s', () => {
  const bad = (run: () => unknown, pattern: RegExp) => rejects(run, pattern);
  bad(() => record('R', { a: field(u32, { number: -1 }) }), /number \(the field number\) must be a 32-bit unsigned integer/);
  bad(() => record('R', { a: field(u32, { deprecated: 1 as never }) }), /deprecated is true or the version text/);
  bad(() => record('R', { a: field(u32, { default: 'x' as never }) }), /default is a boolean, a number or a bigint/);
  const generate = (def: ReturnType<typeof field>) => () => generateSchema({ R: record('R', { a: def }) });
  bad(generate(field(utf8, { default: 1 })), /default applies to a scalar member/);
  bad(generate(field(optional(u32), { default: 1 })), /default applies to a scalar member/);
  bad(generate(field(u128, { default: 1n })), /default applies to a scalar member/);
  bad(generate(field(u8, { default: 256 })), /out of the range/);
  bad(generate(field(u8, { default: true })), /the default of an integer or an enum is an integer/);
  bad(generate(field(u32, { default: 1.5 })), /the default of an integer or an enum is an integer/);
  bad(generate(field(bool, { default: 1 })), /the default of a bool is true or false/);
  bad(generate(field(f64, { default: 1n })), /the default of a float is a number/);
  for (const options of [{ number: 1 }, { default: 1 }, { deprecated: true }]) {
    const key = Object.keys(options)[0];
    const belongs = new RegExp(`${key} belongs to record members`);
    bad(() => method({ a: field(u32, options) }), belongs);
    bad(() => method({}, { returns: field(u32, options) }), belongs);
    bad(() => transparent('T', field(u32, options)), new RegExp(`${key} is a record member's`));
  }
});
