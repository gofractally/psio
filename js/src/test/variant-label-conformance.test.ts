// conformance/variant-label-vectors.json: the label of a variant
// alternative (docs/annotations.md "Variant alternative labels").
//
// The five variants of cpp/tests/variant_label_conformance_types.hpp are
// declared here in the typed layer. In TypeScript an alternative's label is
// the key it is declared under, so each declaration states the labels C++
// derives: the alternative's type name, its position where the type has no
// name or two alternatives share one, or the stated label. The generated
// Schema IR must be the fixture's byte for byte, and every row is replayed
// through the typed codec and through the fixture's own schema.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  PsszError, PsszSchema, array, bool, bytes, f32, f64, generateSchema, i16, i64, list, optional, rawString, record, tuple,
  u16, u32, u64, u8, variant, type JsonFormat, type PsszType, type TypeDef,
} from '../index.js';

interface VariantRow { name: string; labels: string[] }
interface Row { name: string; type: string; index: number; label: string; pssz: string; json: string; ijson: string }
interface Fixture { version: number; declaration: string[]; schema_ir: string; schema_json: string; variants: VariantRow[]; vectors: Row[] }
const fixture: Fixture = JSON.parse(readFileSync(new URL('../../../conformance/variant-label-vectors.json', import.meta.url), 'utf8'));
const fromHex = (hex: string) => Uint8Array.from(Buffer.from(hex, 'hex'));
const toHex = (b: Uint8Array) => Buffer.from(b).toString('hex');
const FORMATS: JsonFormat[] = ['json', 'ijson'];

const Circle = record('Circle', { radius: u32 });
const Square = record('Square', { side: u32 });
// Reflected types: the reflected name.
const Records = variant({ Circle, Square });
// Types without a declared name: the table.
const Scalars = variant({
  bool, uint8: u8, int16: i16, uint32: u32, int64: i64, uint64: u64, float32: f32, double: f64, string: rawString,
});
// Generic types: built from the names of what they hold.
const Generic = variant({
  'uint32[]': list(u32), 'uint32?': optional(u32), tuple_uint32_string: tuple(u32, rawString), 'uint16[2]': array(u16, 2),
  'Circle[]': list(Circle), 'uint8[]': bytes,
});
// One type held twice: those alternatives are labelled by position. `"1"`
// is not the first key, so the alternatives are given as pairs.
const Repeated = variant([['uint32', u32], ['1', rawString], ['2', rawString], ['Circle', Circle]]);
// Stated labels (C++ `psio::variant_labels`).
const Stated = variant({ circle: Circle, square: Square, side: u32 });

const TYPES: { [name: string]: TypeDef<any, any> } = { Records, Scalars, Generic, Repeated, Stated };

test('variant-label-vectors.json: the typed declarations are the C++ Schema IR', () => {
  assert.equal(fixture.version, 1);
  // The emitter's order: insert<V>(name) of each variant.
  const schema = generateSchema(TYPES);
  assert.equal(toHex(schema.encode()), fixture.schema_ir);
  assert.equal(schema.toJson('json'), fixture.schema_json);
  const fetched = PsszSchema.decode(fromHex(fixture.schema_ir));
  assert.deepEqual(fetched.ir, schema.ir);
  assert.deepEqual(fetched.validate(), { ok: true });
  assert.equal(toHex(PsszSchema.fromJson(fixture.schema_json, 'json').encode()), fixture.schema_ir);

  // Each alternative is named by its label, and carries nothing else.
  assert.deepEqual(fixture.variants.map((v) => v.name), Object.keys(TYPES));
  for (const row of fixture.variants) {
    let node = fetched.ir.types.find((m) => m.name === row.name)!.type.value;
    while (node.type === 'Type') {
      const target = node.value.type;
      node = fetched.ir.types.find((m) => m.name === target)!.type.value;
    }
    assert.equal(node.type, 'Variant', row.name);
    if (node.type !== 'Variant') continue;
    assert.deepEqual(node.value.members.map((m) => m.name), row.labels, row.name);
    for (const m of node.value.members) assert.deepEqual(m.attributes, [], `${row.name}.${m.name}`);
  }
  assert.deepEqual(fixture.variants.find((v) => v.name === 'Repeated')!.labels, ['uint32', '1', '2', 'Circle']);
});

test('variant-label-vectors.json: every row, typed and schema-driven', () => {
  const fetched = PsszSchema.decode(fromHex(fixture.schema_ir));
  const names = new Set<string>();
  let codecsRun = 0;
  for (const row of fixture.vectors) {
    assert.ok(!names.has(row.name), `${row.name}: duplicate`);
    names.add(row.name);
    const labels = fixture.variants.find((v) => v.name === row.type)!.labels;
    assert.equal(labels[row.index], row.label, row.name);
    const codecs: [string, PsszType<any, any>][] = [['typed', TYPES[row.type].type() as PsszType], ['schema', fetched.type(row.type)]];
    for (const [where, codec] of codecs) {
      const name = `${row.name} (${where})`;
      ++codecsRun;
      const bytes = fromHex(row.pssz);
      assert.deepEqual(codec.validate(bytes), { ok: true, hasUnknown: false }, name);
      const value = codec.decode(bytes) as { type: string; value: unknown };
      // The JavaScript value is tagged with the label.
      assert.equal(value.type, row.label, name);
      assert.equal(toHex(codec.encode(value)), row.pssz, `${name}: encode`);
      for (const format of FORMATS) {
        assert.equal(codec.valueToJson(value, format), row[format], `${name}: ${format} write`);
        assert.equal(codec.psszToJson(bytes, format), row[format], `${name}: ${format} from bytes`);
        assert.equal(toHex(codec.jsonToPssz(row[format], format)), row.pssz, `${name}: ${format} read`);
        assert.deepEqual(codec.jsonToValue(row[format], format), value, `${name}: ${format} value`);
      }
      // I-JSON is keyed by the label; a key that is not a label is refused.
      assert.ok(row.ijson.startsWith(`{${JSON.stringify(row.label)}:`), name);
      assert.throws(() => codec.jsonToValue('{"no such label":0}', 'ijson'), (e: unknown) => e instanceof PsszError && e.kind === 'json', name);
    }
  }
  console.log('variant-label-vectors.json:', JSON.stringify({ variants: fixture.variants.length, vectors: fixture.vectors.length, codecsRun }));
  // The file as checked in: 5 variants, 25 rows, each through two codecs.
  assert.equal(fixture.variants.length, 5);
  assert.equal(fixture.vectors.length, 25);
  assert.equal(codecsRun, 50);
});

test('variant labels: the key is the label; pairs keep integer labels in order', () => {
  // The default label of a C++ alternative's type is not read once another is stated.
  assert.throws(() => Stated.jsonToValue('{"uint32":5}', 'ijson'), (e: unknown) => e instanceof PsszError && e.kind === 'json');
  assert.deepEqual(Stated.jsonToValue('{"side":5}', 'ijson'), { type: 'side', value: 5 });
  // Pairs and an object with the same labels in the same order are one type.
  const pairs = variant([['circle', Circle], ['square', Square], ['side', u32]]);
  assert.equal(toHex(generateSchema({ V: pairs }).encode()), toHex(generateSchema({ V: Stated }).encode()));
  const value: { type: 'uint32'; value: number } | { type: '1'; value: Uint8Array } | { type: '2'; value: Uint8Array }
    | { type: 'Circle'; value: { radius: number } } = Repeated.decode(fromHex('0262'));
  assert.equal(value.type, '2');
  // Declarations that are refused.
  const schemaError = (run: () => unknown, message: RegExp) =>
    assert.throws(run, (e: unknown) => e instanceof PsszError && e.kind === 'schema' && message.test(e.message));
  schemaError(() => variant({ uint32: u32, 1: rawString } as never), /JavaScript would reorder/);
  schemaError(() => variant([['a', u32], ['a', u8]]), /label "a" is given twice/);
  schemaError(() => variant([['', u32]]), /label is not empty/);
  schemaError(() => variant([['a']] as never), /\[label, definition\] pair/);
  schemaError(() => variant([]), /1 to 256 alternatives/);
  // A schema that states a second name for an alternative is refused: there is none.
  const ir = structuredClone(generateSchema({ V: Stated }).ir);
  const node = ir.types.find((m) => m.type.value.type === 'Variant')!.type.value;
  if (node.type === 'Variant') node.value.members[0].attributes.push({ name: 'ijsonName', value: 'x' });
  assert.throws(() => new PsszSchema(ir).type('V'), (e: unknown) => e instanceof PsszError && e.kind === 'schema');
});
