// Enums, 128 / 256-bit integers and non-finite floats through the Schema IR:
// every text input form and every error (docs/json.md "Enums", "Rules",
// "Writing can fail"), and a schema written before the Enum alternative
// existed.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { IR_VERSION, PsszError, PsszNaN, PsszSchema, type IrSchema, type JsonFormat } from '../index.js';

const hex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');
const fromHex = (text: string) => Uint8Array.from(Buffer.from(text, 'hex'));
const FORMATS: JsonFormat[] = ['json', 'ijson'];

const U8 = { Int: { bits: 8, isSigned: false } };
const I32 = { Int: { bits: 32, isSigned: true } };
const U64 = { Int: { bits: 64, isSigned: false } };
const I64 = { Int: { bits: 64, isSigned: true } };
const U128 = { Int: { bits: 128, isSigned: false } };
const I128 = { Int: { bits: 128, isSigned: true } };
const U256 = { Int: { bits: 256, isSigned: false } };

const U64_MAX = (1n << 64n) - 1n;
const U128_MAX = (1n << 128n) - 1n;
const I128_MAX = (1n << 127n) - 1n;
const I128_MIN = -(1n << 127n);
const U256_MAX = (1n << 256n) - 1n;

const schema = new PsszSchema({
  // Compact enumerators: a { name: value } map ...
  Color: { Enum: { type: U8, members: { red: 0, green: 1, blue: 7 } } },
  // ... or a list; a negative value is stored sign-extended to 64 bits.
  Status: {
    Enum: {
      type: I32,
      members: [{ name: 'pending', value: -1 }, { name: 'active', value: 2 }, { name: 'closed', value: 3 }],
    },
  },
  // Two names for one value: the first is written, both are read.
  Alias: { Enum: { type: U8, members: { first: 4, second: 4 } } },
  Big: { Enum: { type: U64, members: [{ name: 'top', value: U64_MAX }, { name: 'one', value: 1 }] } },
  SignedBig: { Enum: { type: I64, members: [{ name: 'low', value: -(1n << 63n) }] } },
  Named: { Enum: { type: 'Byte', members: { on: 1 } } },
  Byte: U8,
  U128,
  I128,
  U256,
  Record: {
    Object: {
      members: [
        { name: 'color', type: 'Color' },
        { name: 'colors', type: { List: 'Color' } },
        { name: 'maybe', type: { Option: 'Status' } },
        { name: 'quoted', type: 'Color', attributes: [{ name: 'jsonInt', value: 'string' }, { name: 'ijsonInt', value: 'string' }] },
      ],
    },
  },
  Forced: {
    Object: {
      members: [
        { name: 'asString', type: U128, attributes: [{ name: 'jsonInt', value: 'string' }] },
        { name: 'asNumber', type: U128, attributes: [{ name: 'ijsonInt', value: 'number' }] },
        { name: 'list', type: { List: U256 }, attributes: [{ name: 'ijsonInt', value: 'number' }] },
      ],
    },
  },
  F32: { Float: { exp: 8, mantissa: 23 } },
  F64: { Float: { exp: 11, mantissa: 52 } },
  Floats: { Object: { a: { Float: { exp: 11, mantissa: 52 } }, b: { List: { Float: { exp: 8, mantissa: 23 } } } } },
});

function rejects(run: () => unknown, kind: 'json' | 'value' | 'schema' | 'bytes', pattern: RegExp, what = ''): void {
  assert.throws(run, (e: unknown) => {
    assert.ok(e instanceof PsszError, `${what}: ${String(e)}`);
    assert.equal(e.kind, kind, `${what}: ${e.message}`);
    assert.match(e.message, pattern, what);
    return true;
  }, what);
}

test('enum: the wire form is the underlying integer; the value is its number', () => {
  const color = schema.type('Color');
  const status = schema.type('Status');
  // Same bytes, size and width as the integer; the JS value is the number.
  assert.equal(hex(color.encode(7)), '07');
  assert.equal(color.decode(fromHex('07')), 7);
  assert.equal(hex(status.encode(-1)), 'ffffffff');
  assert.equal(status.decode(fromHex('ffffffff')), -1);
  assert.equal(hex(schema.type('Big').encode(U64_MAX)), 'ffffffffffffffff');
  assert.equal(schema.type('Big').decode(fromHex('ffffffffffffffff')), U64_MAX);
  // Any in-range value is structurally valid, declared or not.
  for (let v = 0; v < 256; ++v) {
    const bytes = Uint8Array.of(v);
    assert.deepEqual(color.validate(bytes), { ok: true, hasUnknown: false });
    assert.equal(color.decode(bytes), v);
    assert.equal(hex(color.encode(v)), hex(bytes));
  }
  assert.equal(color.validate(fromHex('0700')).ok, false);
  rejects(() => color.encode(256), 'value', /overflow/);
  rejects(() => color.encode('red'), 'value', /integer value required/);
  // In a record and a list an enum lays out exactly like its integer.
  const asEnum = schema.type({ Struct: { a: 'Color', b: { Array: { type: 'Status', len: 2 } } } });
  const asInt = schema.type({ Struct: { a: U8, b: { Array: { type: I32, len: 2 } } } });
  const value = { a: 7, b: [-1, 5] };
  assert.equal(hex(asEnum.encode(value)), hex(asInt.encode(value)));
  assert.equal(asEnum.width, asInt.width);
  assert.deepEqual(asEnum.decode(asInt.encode(value)), value);
  // The underlying integer may be a named type.
  assert.equal(schema.type('Named').valueToJson(1), '"on"');
});

test('enum text: a listed value is its name, any other its number', () => {
  const color = schema.type('Color');
  const status = schema.type('Status');
  for (const format of FORMATS) {
    assert.equal(color.valueToJson(0, format), '"red"');
    assert.equal(color.valueToJson(7, format), '"blue"');
    assert.equal(color.valueToJson(2, format), '2');
    assert.equal(color.valueToJson(255, format), '255');
    assert.equal(color.psszToJson(fromHex('01'), format), '"green"');
    assert.equal(status.valueToJson(-1, format), '"pending"');
    assert.equal(status.valueToJson(0, format), '0');
    assert.equal(status.valueToJson(-2147483648, format), '-2147483648');
    // When two enumerators share a value the first listed is written.
    assert.equal(schema.type('Alias').valueToJson(4, format), '"first"');
    assert.equal(schema.type('Alias').jsonToValue('"second"', format), 4);
    assert.equal(schema.type('Big').valueToJson(U64_MAX, format), '"top"');
    assert.equal(schema.type('SignedBig').valueToJson(-(1n << 63n), format), '"low"');
    assert.equal(schema.type('SignedBig').jsonToValue('"low"', format), -(1n << 63n));
  }
  // An unnamed value follows the integer rule of its width and format.
  const big = schema.type('Big');
  assert.equal(big.valueToJson(9007199254740991n, 'ijson'), '9007199254740991');
  assert.equal(big.valueToJson(9007199254740992n, 'ijson'), '"9007199254740992"');
  assert.equal(big.valueToJson(9007199254740992n, 'json'), '9007199254740992');
  assert.equal(big.jsonToValue('"9007199254740992"', 'ijson'), 9007199254740992n);
  assert.equal(big.jsonToValue('9007199254740992', 'json'), 9007199254740992n);
  rejects(() => big.jsonToValue('9007199254740992', 'ijson'), 'json', /safe range/);
  rejects(() => big.valueToJson('top'), 'value', /integer value/);
});

test('enum text: every input form, in both formats', () => {
  const color = schema.type('Color');
  const status = schema.type('Status');
  for (const format of FORMATS) {
    // name, number, numeric string, name:number
    for (const text of ['"blue"', '7', '"7"', '"blue:7"', ' "blue" ', '"\\u0062lue"']) {
      assert.equal(color.jsonToValue(text, format), 7, text);
      assert.equal(hex(color.jsonToPssz(text, format)), '07', text);
      assert.equal(color.psszToJson(color.jsonToPssz(text, format), format), '"blue"', text);
    }
    assert.equal(color.jsonToValue('"red:0"', format), 0);
    for (const text of ['"pending"', '-1', '"-1"', '"pending:-1"']) assert.equal(status.jsonToValue(text, format), -1, text);
    // A number that names no enumerator is accepted.
    for (const text of ['9', '"9"']) {
      assert.equal(color.jsonToValue(text, format), 9, text);
      assert.equal(color.psszToJson(color.jsonToPssz(text, format), format), '9', text);
    }
    assert.equal(status.jsonToValue('-5', format), -5);
    assert.equal(status.jsonToValue('"2147483647"', format), 2147483647);
    assert.equal(schema.type('Big').jsonToValue('"top:18446744073709551615"', format), U64_MAX);
    assert.equal(schema.type('Big').jsonToValue('"18446744073709551615"', format), U64_MAX);
  }
});

test('enum text: every error, in both formats', () => {
  const color = schema.type('Color');
  const status = schema.type('Status');
  const bad: [string, RegExp][] = [
    ['"purple"', /unknown enumerator name/],
    ['"Red"', /unknown enumerator name/],
    ['" red"', /unknown enumerator name/],
    ['"red "', /unknown enumerator name/],
    ['""', /unknown enumerator name/],
    ['":0"', /unknown enumerator name/],
    ['"purple:0"', /unknown enumerator name/],
    ['"red:1"', /name and number disagree/],
    ['"blue:0"', /name and number disagree/],
    ['"red:"', /enum number is not a canonical/],
    ['"red:00"', /enum number is not a canonical/],
    ['"red:-0"', /enum number is not a canonical/],
    ['"red:+0"', /enum number is not a canonical/],
    ['"red: 0"', /enum number is not a canonical/],
    ['"red:256"', /enum number is not a canonical/],
    ['"red:0:0"', /enum number is not a canonical/],
    ['"red:0x0"', /enum number is not a canonical/],
    ['"0:red"', /enum number is not a canonical/],
    ['"02"', /enum number is not a canonical/],
    ['"07"', /enum number is not a canonical/],
    ['"-0"', /enum number is not a canonical/],
    ['"-1"', /enum number is not a canonical/],
    ['"256"', /enum number is not a canonical/],
    ['"7 "', /enum number is not a canonical/],
    ['"1.0"', /enum number is not a canonical/],
    ['"1e0"', /enum number is not a canonical/],
    ['"+1"', /unknown enumerator name/],
    ['256', /canonical in-range/],
    ['-1', /canonical in-range/],
    ['02', /./],
    ['-0', /canonical in-range/],
    ['1.0', /canonical in-range/],
    ['1e0', /canonical in-range/],
    ['true', /expected an integer/],
    ['null', /expected an integer/],
    ['["red"]', /expected an integer/],
    ['{"red":0}', /expected an integer/],
    ['red', /./],
    ['"red', /./],
  ];
  for (const format of FORMATS) {
    for (const [text, pattern] of bad) {
      rejects(() => color.jsonToValue(text, format), 'json', pattern, `${format} ${text}`);
      rejects(() => color.jsonToPssz(text, format), 'json', pattern, `${format} ${text}`);
    }
    rejects(() => status.jsonToValue('2147483648', format), 'json', /canonical in-range/);
    rejects(() => status.jsonToValue('"-2147483649"', format), 'json', /enum number/);
    rejects(() => status.jsonToValue('-0', format), 'json', /canonical in-range/);
    rejects(() => status.jsonToValue('"pending:1"', format), 'json', /disagree/);
    rejects(() => status.jsonToValue('"active:-2"', format), 'json', /disagree/);
    // Writing: the value must be an in-range integer.
    rejects(() => color.valueToJson(256, format), 'value', /does not match the schema/);
    rejects(() => color.valueToJson('red', format), 'value', /does not match the schema/);
    rejects(() => color.valueToJson(1.5, format), 'value', /does not match the schema/);
  }
});

test('enum text: inside records, lists and optionals; a forced integer form', () => {
  const record = schema.type('Record');
  const value = { color: 7, colors: [0, 1, 2, 7], maybe: -1, quoted: 9 };
  for (const format of FORMATS) {
    const text = '{"color":"blue","colors":["red","green",2,"blue"],"maybe":"pending","quoted":"9"}';
    assert.equal(record.valueToJson(value, format), text);
    assert.deepEqual(record.jsonToValue(text, format), value);
    assert.deepEqual(
      record.jsonToValue('{"color":7,"colors":["red:0","1",2,"blue:7"],"maybe":"-1","quoted":"9"}', format),
      value,
    );
    assert.equal(record.psszToJson(record.encode(value), format), text);
    assert.equal(
      record.valueToJson({ color: 0, colors: [], maybe: null, quoted: 0 }, format),
      '{"color":"red","colors":[],"maybe":null,"quoted":"red"}',
    );
    // A forced string form refuses a bare number; a name is always read.
    rejects(
      () => record.jsonToValue('{"color":"red","colors":[],"maybe":null,"quoted":9}', format),
      'json', /expected an integer string/,
    );
    assert.equal(
      (record.jsonToValue('{"color":"red","colors":[],"maybe":null,"quoted":"blue"}', format) as { quoted: number }).quoted,
      7,
    );
  }
});

test('enum schema: IR form, PSSZ and JSON round trips, malformed declarations', () => {
  const ir = schema.ir.types.find((m) => m.name === 'Status')!.type.value;
  assert.deepEqual(ir, {
    type: 'Enum',
    value: {
      type: { value: { type: 'Int', value: { bits: 32, isSigned: true } } },
      members: [
        // Sign-extended to 64 bits, read as unsigned.
        { name: 'pending', value: U64_MAX },
        { name: 'active', value: 2n },
        { name: 'closed', value: 3n },
      ],
    },
  });
  // The schema travels as PSSZ and as text with its enums.
  const again = PsszSchema.decode(schema.encode());
  assert.deepEqual(again.ir, schema.ir);
  assert.equal(again.type('Status').valueToJson(-1), '"pending"');
  const meta = PsszSchema.meta().type('Schema');
  for (const format of FORMATS) {
    const viaText = PsszSchema.fromJson(meta.valueToJson(schema.ir, format), format);
    assert.deepEqual(viaText.ir, schema.ir);
  }
  // Enum is the last AnyType alternative: selector 14.
  const anyType = PsszSchema.meta().ir.types.find((m) => m.name === 'AnyType')!.type.value;
  const variant = (anyType.value as { members: { type: { value: { value: { members: { name: string }[] } } } }[] })
    .members[0].type.value.value.members;
  assert.equal(variant.length, 15);
  assert.equal(variant[13].name, 'Type');
  assert.equal(variant[14].name, 'Enum');
  // On the wire: header, one offset slot, then the selector byte.
  const any = PsszSchema.meta().type('AnyType');
  assert.equal(any.encode({ value: ir })[2 * any.width], 14);
  assert.equal(any.encode({ value: { type: 'Type', value: { type: 'x' } } })[2 * any.width], 13);
  assert.deepEqual(any.decode(any.encode({ value: ir })), { value: ir });

  const build = (type: unknown) => new PsszSchema({ T: type } as never).type('T');
  rejects(() => build({ Enum: { type: { List: U8 }, members: [] } }), 'schema', /underlying integer/);
  rejects(() => build({ Enum: { type: 'T', members: [] } }), 'schema', /underlying integer/);
  rejects(() => build({ Enum: { type: U8 } }), 'schema', /malformed enum/);
  rejects(() => build({ Enum: { type: U8, members: [{ name: 1, value: 0 }] } }), 'schema', /Enumerator.name/);
  rejects(() => build({ Enum: { type: U8, members: [{ name: 'a', value: 1.5 }] } }), 'schema', /Enumerator.value/);
  rejects(() => build({ Enum: { type: U8, members: { a: -(1n << 63n) - 1n } } }), 'schema', /out of range/);
  // An enum over a wide integer is that integer on the wire and has no text.
  const wide = build({ Enum: { type: U128, members: { a: 1 } } });
  assert.equal(wide.decode(wide.encode(1n)), 1n);
  rejects(() => wide.valueToJson(1n), 'value', /8 to 64 bits/);
  rejects(() => wide.jsonToValue('"a"'), 'json', /8 to 64 bits/);
  // An enumerator the underlying integer cannot hold is never written and
  // cannot be read by name.
  const narrow = build({ Enum: { type: U8, members: { huge: 300 } } });
  assert.equal(narrow.valueToJson(44), '44');
  rejects(() => narrow.jsonToValue('"huge"'), 'json', /outside its underlying integer/);
});

test('a schema written before the Enum alternative existed still decodes', () => {
  // The meta schema as it was: no Enum alternative, no Enumerator type, and
  // no ir_version member on Schema.
  const metaIr: IrSchema = structuredClone(PsszSchema.meta().ir);
  metaIr.types = metaIr.types.filter((m) => m.name !== 'Enumerator');
  const schemaRecord = metaIr.types.find((m) => m.name === 'Schema')!.type.value;
  assert.equal(schemaRecord.type, 'Object');
  const schemaMembers = (schemaRecord.value as { members: { name: string }[] }).members;
  assert.equal(schemaMembers.pop()!.name, 'ir_version');
  assert.deepEqual(schemaMembers.map((m) => m.name), ['package', 'interfaces', 'worlds', 'uses', 'types']);
  const anyType = metaIr.types.find((m) => m.name === 'AnyType')!.type.value;
  assert.equal(anyType.type, 'Object');
  const alternatives = (anyType.value as { members: { name: string; type: { value: { value: { members: { name: string }[] } } } }[] })
    .members[0].type.value.value;
  assert.equal(alternatives.members.length, 15);
  assert.equal(alternatives.members.pop()!.name, 'Enum');
  assert.deepEqual(
    alternatives.members.map((m) => m.name),
    ['Struct', 'Object', 'Array', 'List', 'BoundedList', 'Option', 'Variant', 'Tuple', 'Int', 'Float', 'FracPack',
      'Custom', 'Resource', 'Type'],
  );
  const oldMeta = new PsszSchema(metaIr).type('Schema');

  const old = new PsszSchema({
    Point: { Struct: { x: I32, y: I32 } },
    Shape: { Variant: { Dot: 'Point', Label: { Custom: { type: { List: U8 }, id: 'string' } } } },
    Row: { Object: { id: U64, shapes: { List: 'Shape' }, wide: { Option: U128 }, pair: { Tuple: [U8, 'Point'] } } },
  });
  const { ir_version: _version, ...unversioned } = old.ir;
  const bytes = oldMeta.encode(unversioned);
  // The current reader takes the old bytes unchanged and writes them back.
  // What it reads has no IR version: the bytes predate the marker.
  const decoded = PsszSchema.decode(bytes);
  assert.deepEqual(decoded.ir, { ...old.ir, ir_version: null });
  assert.equal(hex(decoded.encode()), hex(bytes));
  assert.equal(old.ir.ir_version, IR_VERSION);
  assert.equal(hex(new PsszSchema({ ...old.ir, ir_version: null }).encode()), hex(bytes));
  assert.notEqual(hex(old.encode()), hex(bytes));
  // ... and so no codec is derived from it (the widths and the meaning of
  // "string" could differ) until the caller states the version it is in.
  rejects(() => decoded.type('Row'), 'schema', /no ir_version marker/);
  const stated = new PsszSchema({ ...decoded.ir, ir_version: IR_VERSION });
  const row = { id: 7n, shapes: [{ type: 'Dot', value: { x: 1, y: -2 } }], wide: 5n, pair: [1, { x: 0, y: 0 }] };
  assert.deepEqual(stated.type('Row').decode(old.type('Row').encode(row)), row);
  // An old reader refuses a schema that uses the new alternative.
  rejects(() => oldMeta.decode(schema.encode()), 'bytes', /variant selector out of range/);

  // Pinned bytes of a schema as the previous format wrote it.
  const pinned = new PsszSchema({ P: { Struct: { x: I32 } } });
  const { ir_version: _pinnedVersion, ...pinnedIr } = pinned.ir;
  const pinnedHex = hex(oldMeta.encode(pinnedIr));
  assert.equal(pinnedHex, OLD_FORMAT_SCHEMA);
  assert.deepEqual(PsszSchema.decode(fromHex(OLD_FORMAT_SCHEMA)).ir, { ...pinned.ir, ir_version: null });
});

const OLD_FORMAT_SCHEMA =
  '1400000014000000240000002400000024000000240000000c0000000c0000000c0000000c000000040000000c0000000c0000000d0000004600' +
  '00005004000000040000000008000000080000002c000000040000000c0000000c0000000d0000001c00000078040000000400000008052000' +
  '000001';

test('128 / 256-bit integers: bigint values, binary form unchanged', () => {
  const u128 = schema.type('U128');
  const i128 = schema.type('I128');
  const u256 = schema.type('U256');
  assert.equal(hex(u128.encode(1n)), '01' + '00'.repeat(15));
  assert.equal(hex(i128.encode(-2n)), 'fe' + 'ff'.repeat(15));
  // 256 bits: 32 little-endian bytes on the wire, a bigint in JavaScript.
  assert.equal(hex(u256.encode(0x0102n)), '0201' + '00'.repeat(30));
  assert.equal(u256.decode(fromHex('0201' + '00'.repeat(30))), 0x0102n);
  assert.equal(u256.decode(fromHex('ff'.repeat(32))), U256_MAX);
  assert.equal(i128.decode(fromHex('00'.repeat(15) + '80')), I128_MIN);
  assert.equal(hex(u128.encode(7)), hex(u128.encode(7n)));
  rejects(() => u256.encode(U256_MAX + 1n), 'value', /overflow/);
  rejects(() => u128.encode(-1n), 'value', /overflow/);
  rejects(() => new PsszSchema({ T: { Int: { bits: 256, isSigned: true } } }).type('T'), 'schema', /integer width/);
});

test('128 / 256-bit integers: json writes a bare number, ijson always a string', () => {
  const rows: [string, bigint][] = [
    ['U128', 0n], ['U128', 7n], ['U128', 9007199254740991n], ['U128', 9007199254740992n], ['U128', U128_MAX],
    ['I128', 0n], ['I128', -1n], ['I128', -9007199254740992n], ['I128', I128_MAX], ['I128', I128_MIN],
    ['U256', 0n], ['U256', 12n], ['U256', 1n << 128n], ['U256', U256_MAX],
  ];
  for (const [name, value] of rows) {
    const type = schema.type(name);
    assert.equal(type.valueToJson(value, 'json'), value.toString());
    assert.equal(type.valueToJson(value, 'ijson'), `"${value}"`);
    assert.equal(type.valueToJson(value), `"${value}"`); // ijson is the default
    for (const format of FORMATS) {
      const text = type.valueToJson(value, format);
      assert.equal(type.jsonToValue(text, format), value);
      assert.equal(hex(type.jsonToPssz(text, format)), hex(type.encode(value)));
      assert.equal(type.psszToJson(type.encode(value), format), text);
      // The decimal string is read in both formats.
      assert.equal(type.jsonToValue(`"${value}"`, format), value);
    }
    // A bare number: any in-range value under json.
    assert.equal(type.jsonToValue(value.toString(), 'json'), value);
  }
  // A small number value is accepted on encode-to-text, as for 64 bits.
  assert.equal(schema.type('U128').valueToJson(7, 'ijson'), '"7"');
  assert.equal(schema.type('U256').valueToJson(7, 'json'), '7');
});

test('128 / 256-bit integers: the ijson bare-number limit', () => {
  const safe = 9007199254740991n;
  for (const name of ['U128', 'I128', 'U256']) {
    const type = schema.type(name);
    assert.equal(type.jsonToValue(safe.toString(), 'ijson'), safe);
    assert.equal(type.jsonToValue('0', 'ijson'), 0n);
    rejects(() => type.jsonToValue((safe + 1n).toString(), 'ijson'), 'json', /safe range must be a string/, name);
    assert.equal(type.jsonToValue((safe + 1n).toString(), 'json'), safe + 1n);
    assert.equal(type.jsonToValue(`"${safe + 1n}"`, 'ijson'), safe + 1n);
  }
  const i128 = schema.type('I128');
  assert.equal(i128.jsonToValue((-safe).toString(), 'ijson'), -safe);
  rejects(() => i128.jsonToValue((-safe - 1n).toString(), 'ijson'), 'json', /safe range must be a string/);
  assert.equal(i128.jsonToValue((-safe - 1n).toString(), 'json'), -safe - 1n);
});

test('128 / 256-bit integers: the legacy "0x" string is read, never written', () => {
  const u128 = schema.type('U128');
  const i128 = schema.type('I128');
  const u256 = schema.type('U256');
  for (const format of FORMATS) {
    const q = (v: bigint) => (format === 'ijson' ? `"${v}"` : v.toString());
    const reads = (type: typeof u128, text: string, value: bigint) => {
      assert.equal(type.jsonToValue(text, format), value, text);
      // Output is always decimal.
      assert.equal(type.psszToJson(type.jsonToPssz(text, format), format), q(value), text);
    };
    reads(u128, '"0xff"', 255n);
    reads(u128, '"0XFF"', 255n);
    reads(u128, '"0xAbCd"', 0xabcdn);
    reads(u128, '"0x0"', 0n);
    reads(u128, '"0x00ff"', 255n);
    reads(u128, `"0x${'f'.repeat(32)}"`, U128_MAX);
    reads(i128, '"-0x2a"', -42n);
    reads(i128, '"0x2a"', 42n);
    reads(i128, `"0x7${'f'.repeat(31)}"`, I128_MAX);
    reads(i128, `"-0x8${'0'.repeat(31)}"`, I128_MIN);
    reads(u256, '"0x100"', 256n);
    reads(u256, `"0x${'F'.repeat(64)}"`, U256_MAX);

    const bad: [typeof u128, string][] = [
      [u128, '"0x"'], [u128, '"0xfg"'], [u128, '"0x 1"'], [u128, '"x1"'], [u128, '"0x-1"'], [u128, '"ff"'],
      [u128, '"-0x1"'], [u128, `"0x1${'0'.repeat(32)}"`], [u128, `"0x0${'f'.repeat(32)}"`], [u128, '"\\u0030x1"'],
      [i128, '"-0x"'], [i128, `"0x8${'0'.repeat(31)}"`], [i128, `"-0x8${'0'.repeat(30)}1"`], [i128, '"--0x1"'],
      [u256, '"-0x1"'], [u256, `"0x1${'0'.repeat(64)}"`], [u256, '"0x"'],
    ];
    for (const [type, text] of bad)
      rejects(() => type.jsonToValue(text, format), 'json', /integer string is not a canonical in-range decimal/, text);
    // A bare hex token is not JSON.
    rejects(() => u128.jsonToValue('0xff', format), 'json', /./);
  }
});

test('128 / 256-bit integers: canonical decimal only, in range, numbers or strings only', () => {
  const cases: [string, string[]][] = [
    ['U128', [
      '"340282366920938463463374607431768211456"', '340282366920938463463374607431768211456', '"-1"', '-1', '"01"',
      '01', '"00"', '""', '"+1"', '" 1"', '"1 "', '"-0"', '-0', '1.5', '1e3', '1.0', '"1.0"', '"1e3"', 'true', 'null',
      '[1]', '{}', '"\\u0031"', `"${'9'.repeat(200)}"`, '9'.repeat(200),
    ]],
    ['I128', [
      '"170141183460469231731687303715884105728"', '170141183460469231731687303715884105728',
      '"-170141183460469231731687303715884105729"', '-170141183460469231731687303715884105729', '"-0"', '-0',
      '"-01"', '"--1"', '"-"',
    ]],
    ['U256', [
      '"115792089237316195423570985008687907853269984665640564039457584007913129639936"',
      '115792089237316195423570985008687907853269984665640564039457584007913129639936', '"-1"', '-1', '"00"', '1.5',
    ]],
  ];
  for (const format of FORMATS)
    for (const [name, texts] of cases)
      for (const text of texts) {
        rejects(() => schema.type(name).jsonToValue(text, format), 'json', /./, `${name} ${format} ${text.slice(0, 50)}`);
        rejects(() => schema.type(name).jsonToPssz(text, format), 'json', /./, `${name} ${format} ${text.slice(0, 50)}`);
      }
  for (const format of FORMATS) {
    rejects(() => schema.type('U128').valueToJson(-1n, format), 'value', /does not match the schema/);
    rejects(() => schema.type('U128').valueToJson(U128_MAX + 1n, format), 'value', /does not match the schema/);
    rejects(() => schema.type('I128').valueToJson(I128_MAX + 1n, format), 'value', /does not match the schema/);
    rejects(() => schema.type('U256').valueToJson(U256_MAX + 1n, format), 'value', /does not match the schema/);
    rejects(() => schema.type('U256').valueToJson('7', format), 'value', /does not match the schema/);
  }
});

test('128 / 256-bit integers: jsonInt / ijsonInt force a form', () => {
  const forced = schema.type('Forced');
  const value = { asString: 7n, asNumber: U128_MAX, list: [1n, U256_MAX] };
  const json = `{"asString":"7","asNumber":${U128_MAX},"list":[1,${U256_MAX}]}`;
  const ijson = `{"asString":"7","asNumber":${U128_MAX},"list":[1,${U256_MAX}]}`;
  assert.equal(forced.valueToJson(value, 'json'), json);
  assert.equal(forced.valueToJson(value, 'ijson'), ijson);
  assert.deepEqual(forced.jsonToValue(json, 'json'), value);
  // A forced number has no bare-number limit under ijson.
  assert.deepEqual(forced.jsonToValue(ijson, 'ijson'), value);
  // A forced form reads exactly that form; "0x" is a string form.
  assert.deepEqual(
    forced.jsonToValue('{"asString":"0x7","asNumber":1,"list":[]}', 'json'),
    { asString: 7n, asNumber: 1n, list: [] },
  );
  rejects(() => forced.jsonToValue('{"asString":7,"asNumber":1,"list":[]}', 'json'), 'json', /expected an integer string/);
  rejects(() => forced.jsonToValue('{"asString":"7","asNumber":"1","list":[]}', 'ijson'), 'json', /expected a number/);
  rejects(() => forced.jsonToValue('{"asString":"7","asNumber":1,"list":["1"]}', 'ijson'), 'json', /expected a number/);
  // Without the attribute ijson reads both forms.
  assert.deepEqual(
    forced.jsonToValue('{"asString":"7","asNumber":1,"list":[]}', 'ijson'),
    { asString: 7n, asNumber: 1n, list: [] },
  );
});

test('non-finite floats: no text in either format, never read', () => {
  const f32 = schema.type('F32');
  const f64 = schema.type('F64');
  const floats = schema.type('Floats');
  for (const format of FORMATS) {
    for (const type of [f32, f64])
      for (const value of [NaN, Infinity, -Infinity])
        rejects(() => type.valueToJson(value, format), 'value', /non-finite float has no JSON text/, `${format} ${value}`);
    // A NaN with a payload, and non-finite bytes on the way to text.
    rejects(() => f64.valueToJson(new PsszNaN(0x7ff0000000000001n, 8), format), 'value', /non-finite/);
    rejects(() => f32.valueToJson(new PsszNaN(0x7f800001n, 4), format), 'value', /non-finite/);
    for (const bytes of ['000000000000f07f', '000000000000f0ff', '000000000000f87f', '010000000000f07f']) {
      assert.equal(f64.validate(fromHex(bytes)).ok, true);
      rejects(() => f64.psszToJson(fromHex(bytes), format), 'value', /non-finite/, bytes);
    }
    for (const bytes of ['0000807f', '000080ff', '0000c07f', '0100807f'])
      rejects(() => f32.psszToJson(fromHex(bytes), format), 'value', /non-finite/, bytes);
    // A float that overflows binary32 is not finite there.
    rejects(() => f32.valueToJson(1e39, format), 'value', /non-finite/);
    // Nested: the whole write fails, nothing partial is returned.
    rejects(() => floats.valueToJson({ a: 1, b: [1, NaN] }, format), 'value', /non-finite/);
    rejects(() => floats.valueToJson({ a: -Infinity, b: [] }, format), 'value', /non-finite/);

    for (const text of [
      'NaN', 'nan', 'Infinity', '-Infinity', '+Infinity', 'inf', '-inf', '"NaN"', '"Infinity"', '"-Infinity"', 'null',
      '1e999', '-1e999', '1e400', '"1"',
    ]) {
      rejects(() => f64.jsonToValue(text, format), 'json', /./, `${format} ${text}`);
      rejects(() => f32.jsonToPssz(text, format), 'json', /./, `${format} ${text}`);
    }
    rejects(() => f32.jsonToValue('3.5e38', format), 'json', /out of range/);
    rejects(() => floats.jsonToValue('{"a":NaN,"b":[]}', format), 'json', /./);
    rejects(() => floats.jsonToValue('{"a":1,"b":[Infinity]}', format), 'json', /./);
  }
});

test('float text is the shortest round trip in the layout of std::to_chars', () => {
  const f32 = schema.type('F32');
  const f64 = schema.type('F64');
  const doubles: [number, string][] = [
    [2 ** -24, '5.960464477539063e-08'], [-(2 ** -24), '-5.960464477539063e-08'],
    [5e-324, '5e-324'], [1e-323, '1e-323'], [1.5e-323, '1.5e-323'],
    [2.225073858507201e-308, '2.225073858507201e-308'], [2.2250738585072014e-308, '2.2250738585072014e-308'],
    [999999999999999868928, '999999999999999868928'], [1e21, '1e+21'], [1000000000000000131072, '1000000000000000131072'],
    [1e20, '1e+20'], [1.5e21, '1.5e+21'], [123456789012345680000, '123456789012345683968'],
    [1e23, '1e+23'], [1e-7, '1e-07'], [1.5e-7, '1.5e-07'], [1.0000000000000001e-7, '1.0000000000000001e-07'],
    [9.99999999999999e-7, '9.99999999999999e-07'], [1e-6, '1e-06'], [1e-5, '1e-05'], [0.000001234, '1.234e-06'],
    [2 ** 63, '9223372036854775808'], [2 ** 64, '18446744073709551616'], [2 ** 70, '1180591620717411303424'],
    [0.3, '0.3'], [2 ** -10, '0.0009765625'], [-0, '-0'],
  ];
  for (const format of FORMATS)
    for (const [value, text] of doubles) {
      assert.equal(f64.valueToJson(value, format), text);
      assert.ok(Object.is(f64.jsonToValue(text, format), value), text);
    }
  const singles: [number, string][] = [
    [2 ** -24, '5.9604645e-08'], [2 ** -149, '1e-45'], [1.1754942106924411e-38, '1.1754942e-38'],
    [2 ** -126, '1.1754944e-38'], [Math.fround(1e21), '1e+21'], [Math.fround(1e20), '1e+20'],
    [Math.fround(1e-7), '1e-07'], [Math.fround(1e-6), '1e-06'], [Math.fround(9.999999e-8), '9.999999e-08'],
    [2 ** 63, '9.223372e+18'], [2 ** -10, '0.0009765625'], [Math.fround(0.3), '0.3'],
    [16777216, '16777216'], [Math.fround(3.4e38), '3.4e+38'],
  ];
  for (const format of FORMATS)
    for (const [value, text] of singles) {
      assert.equal(f32.valueToJson(value, format), text);
      assert.ok(Object.is(f32.jsonToValue(text, format), value), text);
    }
});
