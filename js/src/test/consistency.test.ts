// The consistency round (docs/pssz-spec.md 3.5, 4.5, 5.10, 7.5, 8.4, 16;
// docs/json.md "Method arguments"): the JavaScript side of
// cpp/tests/consistency_tests.cpp. The fixtures pin the bytes; this file
// pins the rules that have no fixture rows (order constraints, the field
// count cap) and re-states the ones that do, on the typed layer and on the
// schema as it travels.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  PsszError, PsszSchema, RecordView, argumentTuple, array, bitlist, bytes, custom, enumeration, field, generateSchema, i16,
  i32, list, map, nested, optional, pair, rawString, record, set, toHex, transparent, tuple, u32, u64, u8, utf8, variant,
  boundedUtf8, type Infer, type PsszType, type PsszTypeInput, type TypeDef,
} from '../index.js';

const fromHex = (hex: string) => Uint8Array.from(Buffer.from(hex.replace(/ /g, ''), 'hex'));

function rejects(run: () => unknown, kind: string, message?: string | RegExp): PsszError {
  try {
    run();
  } catch (e) {
    assert.ok(e instanceof PsszError, String(e));
    assert.equal(e.kind, kind, e.message);
    if (typeof message === 'string') assert.equal(e.message, message);
    else if (message) assert.match(e.message, message);
    return e;
  }
  return assert.fail('accepted');
}

// ── sorted / unique / uniqueKeys ────────────────────────────────────────────

const rankedFields = {
  scores: list(u32),                 // sorted
  names: list(utf8),                 // strictly ascending
  falling: list(i32),                // descending
  entries: list(pair(utf8, u32)),    // unique by element 0
  rows: list(list(u8)),              // every row sorted
  triple: array(i16, 3),             // sorted
};
const Ranked = record('Ranked', {
  scores: field(rankedFields.scores, { sorted: true, max: 8 }),
  names: field(rankedFields.names, { unique: true }),
  falling: field(rankedFields.falling, { sorted: 'descending' }),
  entries: field(rankedFields.entries, { uniqueKeys: true }),
  rows: field(rankedFields.rows, { element: { sorted: true } }),
  triple: field(rankedFields.triple, { sorted: true }),
});
type Ranked = Infer<typeof Ranked>;
// The same record with no constraint: what an unchecked writer produces.
const Loose = record('Ranked', { ...rankedFields, scores: field(rankedFields.scores, { max: 8 }) });
const good = (): Ranked => ({
  scores: [1, 1, 2, 300], names: ['a', 'ab', 'b'], falling: [5, 5, -1], entries: [['x', 1], ['a', 1]],
  rows: [[1, 2, 2], [], [9]], triple: [-5, 0, 0],
});
const ORDER = 'sorted: elements are not in order';
const STRICT = 'sorted: elements are not in strictly ascending order';
const KEYS = 'unique_keys: two elements have the same key';

test('sorted / unique / uniqueKeys: writers and checking readers (C++ "[consistency][order]")', () => {
  const ok = good();
  const frame = Ranked.encode(ok);
  const text = Ranked.valueToJson(ok, 'json');
  const key = Ranked.encodeKey(ok);
  assert.deepEqual(Ranked.validate(frame), { ok: true, hasUnknown: false });
  assert.deepEqual(Ranked.decode(frame, { rejectUnknown: true }), ok);
  assert.deepEqual(Ranked.jsonToValue(text, 'json'), ok);
  assert.deepEqual(Ranked.jsonToValue(Ranked.valueToJson(ok)), ok);
  assert.deepEqual(Ranked.validateKey(key), { ok: true });
  assert.deepEqual(Ranked.decodeKey(key), ok);
  assert.deepEqual((Ranked.view(frame) as RecordView).toValue(), ok);
  // The constraints change no byte, no width, no bound — and are not in the Schema IR.
  assert.equal(toHex(frame), toHex(Loose.encode(ok)));
  assert.equal(toHex(key), toHex(Loose.encodeKey(ok)));
  assert.equal(Ranked.inferredBound, Loose.inferredBound);
  assert.equal(Ranked.width, Loose.width);
  assert.equal(toHex(generateSchema({ Ranked }).encode()), toHex(generateSchema({ Ranked: Loose }).encode()));

  let cases = 0;
  const refused = (what: string, change: (r: Ranked) => void, message: string) => {
    const bad = good();
    change(bad);
    ++cases;
    // Writers.
    assert.equal(rejects(() => Ranked.encode(bad), 'value').message, message, what);
    for (const format of ['json', 'ijson'] as const) rejects(() => Ranked.valueToJson(bad, format), 'value', message);
    rejects(() => Ranked.encodeKey(bad), 'value', message);
    for (const width of [1, 2, 4] as const) rejects(() => Ranked.type({ width }).encode(bad), 'value', message);
    // Readers, on what an unchecked writer produces.
    const raw = Loose.encode(bad);
    const refusal = Ranked.validate(raw);
    assert.ok(!refusal.ok && refusal.error.kind === 'bytes', what);
    assert.equal(refusal.error.message, message, what);
    rejects(() => Ranked.decode(raw), 'bytes', message);
    rejects(() => Ranked.decode(raw, { rejectUnknown: true }), 'bytes', message);
    rejects(() => Ranked.psszToJson(raw), 'bytes', message);
    rejects(() => (Ranked.view(raw) as RecordView).toValue(), 'bytes', message);
    for (const width of [1, 2, 4] as const)
      rejects(() => Ranked.type({ width }).decode(Loose.type({ width }).encode(bad)), 'bytes', message);
    for (const format of ['json', 'ijson'] as const) {
      rejects(() => Ranked.jsonToValue(Loose.valueToJson(bad, format), format), 'json', message);
      rejects(() => Ranked.jsonToPssz(Loose.valueToJson(bad, format), format), 'json', message);
    }
    const rawKey = Loose.encodeKey(bad);
    const keyRefusal = Ranked.validateKey(rawKey);
    assert.ok(!keyRefusal.ok && keyRefusal.error.message === message, what);
    rejects(() => Ranked.decodeKey(rawKey), 'bytes', message);
    // A reader of the schema alone does not know the constraints.
    const fetched = PsszSchema.decode(generateSchema({ Ranked }).encode()).type('Ranked');
    assert.deepEqual(fetched.validate(raw), { ok: true, hasUnknown: false }, what);
  };
  refused('scores descend', (r) => { r.scores = [2, 1]; }, ORDER);
  refused('names repeat', (r) => { r.names = ['a', 'a']; }, STRICT);
  refused('names descend', (r) => { r.names = ['b', 'a']; }, STRICT);
  refused('falling ascends', (r) => { r.falling = [1, 2]; }, ORDER);
  refused('key order, not signed order', (r) => { r.falling = [-1, 1]; }, ORDER);
  refused('entries repeat a key', (r) => { r.entries = [['a', 1], ['b', 2], ['a', 3]]; }, KEYS);
  refused('a row descends', (r) => { r.rows = [[1], [3, 2]]; }, ORDER);
  refused('the array descends', (r) => { r.triple = [1, 0, 2]; }, ORDER);
  assert.equal(cases, 8);
});

test('sorted / unique / uniqueKeys: order is that of the key bytes, at any position', () => {
  // Strings order bytewise (UTF-8), not by UTF-16 code unit; a prefix sorts first.
  const Words = record('Words', { words: field(list(utf8), { unique: true }) });
  assert.ok(Words.encode({ words: ['a', 'ab', 'b', '\u{ffff}', '\u{10000}'] }));
  rejects(() => Words.encode({ words: ['\u{10000}', '\u{ffff}'] }), 'value', STRICT);
  // Strictly descending.
  const Down = record('Down', { v: field(list(u32), { sorted: 'descending', unique: true }) });
  assert.ok(Down.encode({ v: [3, 2, 1] }));
  rejects(() => Down.encode({ v: [3, 3] }), 'value', STRICT);
  rejects(() => Down.encode({ v: [1, 2] }), 'value', STRICT);
  // uniqueKeys on plain elements, on tuples (element 0), without order.
  const Ids = record('Ids', { ids: field(list(u32), { uniqueKeys: true }), rows: field(list(tuple(u32, utf8, u8)), { uniqueKeys: true }) });
  assert.ok(Ids.encode({ ids: [3, 1, 2], rows: [[2, 'a', 0], [1, 'a', 0]] }));
  rejects(() => Ids.encode({ ids: [3, 1, 3], rows: [] }), 'value', KEYS);
  rejects(() => Ids.encode({ ids: [], rows: [[1, 'a', 0], [1, 'b', 1]] }), 'value', KEYS);
  // Through an optional, in a map's values, in a tuple element, in a variant alternative, in a set's elements.
  const Deep = record('Deep', {
    maybe: field(optional(list(u32)), { sorted: true }),
    by: field(map(utf8, list(u32)), { values: { unique: true } }),
    at: field(tuple(u8, list(u32)), { at: { 1: { sorted: true } } }),
    alt: field(variant({ One: u32, Many: list(u32) }), { at: { Many: { sorted: true } } }),
    sets: field(set(list(u8)), { element: { sorted: true } }),
    grid: field(list(list(list(u8))), { element: { element: { unique: true } } }),
  });
  const deep = { maybe: null, by: new Map([['k', [1, 2]]]), at: [0, [1, 1]] as [number, number[]], alt: { type: 'Many' as const, value: [1, 2] }, sets: new Set([[1, 2]]), grid: [[[1, 2]]] };
  assert.deepEqual(Deep.decode(Deep.encode(deep)), deep);
  rejects(() => Deep.encode({ ...deep, maybe: [2, 1] }), 'value', ORDER);
  rejects(() => Deep.encode({ ...deep, by: new Map([['k', [1, 1]]]) }), 'value', STRICT);
  rejects(() => Deep.encode({ ...deep, at: [0, [2, 1]] }), 'value', ORDER);
  rejects(() => Deep.encode({ ...deep, alt: { type: 'Many', value: [2, 1] } }), 'value', ORDER);
  assert.ok(Deep.encode({ ...deep, alt: { type: 'One', value: 7 } }));
  rejects(() => Deep.encode({ ...deep, sets: new Set([[2, 1]]) }), 'value', ORDER);
  rejects(() => Deep.encode({ ...deep, grid: [[[1, 2], [2, 2]]] }), 'value', STRICT);
  // A method's parameters take them too; a custom type over the record keeps them.
  const Args = argumentTuple({ ids: field(list(u32), { unique: true }) });
  rejects(() => Args.encode({ ids: [1, 1] }), 'value', STRICT);
  rejects(() => Args.jsonToValue('[[2,1]]'), 'json', STRICT);
  const Checked = custom('Checked', record('Checked', { v: field(list(u32), { sorted: true }) }), {});
  rejects(() => Checked.encode({ v: [2, 1] }), 'value', ORDER);
  rejects(() => generateSchema({ Checked }).type('Checked').encode({ v: [2, 1] }), 'value', ORDER);

  // Where they do not apply.
  const schemaError = (run: () => unknown, message: RegExp) => rejects(run, 'schema', message);
  schemaError(() => record('R', { a: field(u32, { sorted: true }) }), /sorted \/ unique \/ uniqueKeys constrain the elements of a list or an array/);
  schemaError(() => record('R', { a: field(map(u32, u32), { sorted: true }) }), /a map or set is ordered by type/);
  schemaError(() => record('R', { a: field(set(u32), { unique: true }) }), /a map or set is ordered by type/);
  schemaError(() => record('R', { a: field(list(u32), { element: { sorted: true } }) }), /sorted \/ unique \/ uniqueKeys constrain/);
  schemaError(() => record('R', { a: field(list(u32), { sorted: 'up' as never }) }), /sorted is true, false or "descending"/);
  schemaError(() => record('R', { a: field(list(u32), { unique: 1 as never }) }), /unique is true or false/);
  schemaError(() => transparent('T', field(list(u32), { sorted: true })), /sorted \/ unique \/ uniqueKeys are a record member's/);
  // (elements without a key encoding cannot be compared)
  schemaError(() => record('R', { a: field(list(bitlist(8)), { sorted: true }) }).encode({ a: [] }), /no key encoding/);
});

// ── maxFields ───────────────────────────────────────────────────────────────

test('maxFields: the cap on the number of fields is enforced (C++ "[consistency][fields]")', () => {
  // The declaration.
  const Capped = record('Capped', { a: u32, b: optional(utf8) }, { maxFields: 4 });
  const PutArgs = argumentTuple({ id: u32, text: field(rawString, { max: 8 }) }, { maxFields: 4, maxDynamicData: 64 });
  const schema = generateSchema({ Capped, PutArgs });
  const attributes = (name: string) => {
    let type = schema.ir.types.find((m) => m.name === name)!.type.value;
    while (type.type === 'Type') type = schema.ir.types.find((m) => m.name === (type.value as { type: string }).type)!.type.value;
    return Object.fromEntries((type.value as { attributes: { name: string; value: string }[] }).attributes.map((a) => [a.name, a.value]));
  };
  assert.deepEqual(attributes('Capped'), { maxFields: '4' });
  assert.deepEqual(attributes('PutArgs'), { tuple: 'named', maxFields: '4', maxDynamicData: '64' });
  assert.deepEqual(schema.validate(), { ok: true });
  const schemaError = (run: () => unknown, message: RegExp) => rejects(run, 'schema', message);
  schemaError(() => record('R', { a: u32, b: u32, c: u32 }, { maxFields: 2 }), /record "R" has more fields than its maxFields \(2\)/);
  schemaError(() => record('R', { a: u32 }, { maxFields: 256 }), /maxFields of record "R" must be an integer from 0 to 255/);
  schemaError(() => record('R', { a: u32 }, { maxFields: 1.5 }), /maxFields/);
  schemaError(() => record('R', { a: u32 }, { maxFields: -1 }), /maxFields/);
  assert.ok(record('R', { a: u32 }, { maxFields: 255 }).encode({ a: 1 }));
  assert.ok(record('R', {}, { maxFields: 0 }).encode({}));
  schemaError(() => argumentTuple({ a: u32, b: u32 }, { maxFields: 1 }), /more parameters than its maxFields \(1\)/);
  schemaError(() => argumentTuple([u32], { maxFields: 256 }), /maxFields/);
  // Nothing on the wire counts fields: it adds no check of a value.
  assert.equal(toHex(Capped.encode({ a: 1, b: null })), toHex(record('Capped', { a: u32, b: optional(utf8) }).encode({ a: 1, b: null })));

  // A schema-driven reader refuses a record with more members than its
  // maxFields, more than 255 members, or a maxFields above 255.
  const recordOf = (members: number, maxFields?: string): PsszTypeInput => ({
    value: {
      type: 'Object',
      value: {
        members: Array.from({ length: members }, (_, i) => ({ name: `m${i}`, type: { Int: { bits: 8, isSigned: false } }, attributes: [] })),
        attributes: maxFields === undefined ? [] : [{ name: 'maxFields', value: maxFields }],
      },
    },
  } as unknown as PsszTypeInput);
  const reads = (root: PsszTypeInput) => {
    try {
      new PsszSchema({}).type(root);
      return true;
    } catch (e) {
      assert.ok(e instanceof PsszError && e.kind === 'schema', String(e));
      return false;
    }
  };
  assert.equal(reads(recordOf(3)), true);
  assert.equal(reads(recordOf(3, '3')), true);
  assert.equal(reads(recordOf(3, '255')), true);
  assert.equal(reads(recordOf(4, '3')), false);
  assert.equal(reads(recordOf(3, '256')), false);
  assert.equal(reads(recordOf(3, 'many')), false);
  assert.equal(reads(recordOf(255)), true);
  assert.equal(reads(recordOf(256)), false);
  rejects(() => new PsszSchema({}).type(recordOf(4, '3')), 'schema', 'pssz IR: a record has more fields than its maxFields');
  rejects(() => new PsszSchema({}).type(recordOf(3, '256')), 'schema', 'pssz IR: malformed maxFields attribute');
  rejects(() => new PsszSchema({}).type(recordOf(256)), 'schema', 'pssz IR: a record has more than 255 fields');
  // (an explicit width profile takes no maxFields, as before)
  rejects(() => new PsszSchema({}).type(recordOf(3, '3'), { width: 2 }), 'schema', 'pssz IR: unsupported schema constraint or annotation');

  // PsszSchema.validate (C++ validate_schema): a function's parameter list is a tuple.
  const withParams = (params: number, attributes: { name: string; value: string }[]) => new PsszSchema({
    ...schema.ir,
    interfaces: [{
      name: 'api', type_names: [], attributes: [],
      funcs: [{
        name: 'call', result: null, attributes,
        params: Array.from({ length: params }, (_, i) => ({ name: `p${i}`, type: { value: { type: 'Int', value: { bits: 32, isSigned: false } } }, attributes: [] })),
      }],
    }],
  } as never);
  const valid = (s: PsszSchema) => {
    const result = s.validate();
    // What validate() refuses, the lookup refuses.
    if (!result.ok) rejects(() => s.interface('api'), 'schema', result.error.message);
    else assert.equal(s.interface('api').method('call').name, 'call');
    return result.ok;
  };
  assert.equal(valid(withParams(255, [])), true);
  assert.equal(valid(withParams(256, [])), false);
  assert.equal(valid(withParams(2, [{ name: 'maxFields', value: '2' }])), true);
  assert.equal(valid(withParams(3, [{ name: 'maxFields', value: '2' }])), false);
  assert.equal(valid(withParams(1, [{ name: 'maxFields', value: '256' }])), false);
  assert.equal(valid(withParams(1, [{ name: 'maxDynamicData', value: 'x' }])), false);
  const refusal = withParams(256, []).validate();
  assert.ok(!refusal.ok && refusal.error.message === 'pssz IR: a function has more than 255 parameters');
  // ... and every named type, reached from a root or not.
  const broken = new PsszSchema({ Fine: { Int: { bits: 8, isSigned: false } }, Bad: recordOf(4, '3') });
  assert.equal(broken.type('Fine').width, 1);
  assert.equal(broken.validate().ok, false);
});

// ── Argument lists in text ──────────────────────────────────────────────────

test('argument lists: the tuple attribute rules the text form, schema-driven', () => {
  const U32: PsszTypeInput = { Int: { bits: 32, isSigned: false } };
  const args = (form: string | null, names: string[]): PsszTypeInput => ({
    value: {
      type: 'Object',
      value: {
        members: names.map((name) => ({ name, type: U32, attributes: [] })),
        attributes: form === null ? [] : [{ name: 'tuple', value: form }],
      },
    },
  } as unknown as PsszTypeInput);
  const schema = new PsszSchema({});
  const named = schema.type(args('named', ['a', 'b']));
  const positional = schema.type(args('positional', ['0', '1']));
  const plain = schema.type(args(null, ['a', 'b']));
  // One wire form.
  const wire = '08 01000000 02000000';
  for (const [type, value] of [[named, { a: 1, b: 2 }], [positional, [1, 2]], [plain, { a: 1, b: 2 }]] as [PsszType, unknown][]) {
    assert.equal(toHex(type.encode(value)), wire.replace(/ /g, ''));
    assert.deepEqual(type.decode(fromHex(wire)), value);
  }
  for (const format of ['json', 'ijson'] as const) {
    // named: writes the object, reads the object or the array.
    assert.equal(named.valueToJson({ a: 1, b: 2 }, format), '{"a":1,"b":2}');
    assert.deepEqual(named.jsonToValue('{"b":2,"a":1}', format), { a: 1, b: 2 });
    assert.deepEqual(named.jsonToValue('[1,2]', format), { a: 1, b: 2 });
    rejects(() => named.jsonToValue('[1]', format), 'json', 'json: array has too few elements');
    rejects(() => named.jsonToValue('[1,2,3]', format), 'json', 'json: array has too many elements');
    // positional: writes and reads the array only.
    assert.equal(positional.valueToJson([1, 2], format), '[1,2]');
    assert.deepEqual(positional.jsonToValue('[1,2]', format), [1, 2]);
    rejects(() => positional.jsonToValue('{"0":1,"1":2}', format), 'json');
    // An Object without the attribute is a record: an object, never an array.
    assert.equal(plain.valueToJson({ a: 1, b: 2 }, format), '{"a":1,"b":2}');
    rejects(() => plain.jsonToValue('[1,2]', format), 'json');
  }
  rejects(() => schema.type(args('array', ['a'])), 'schema', 'pssz IR: malformed tuple attribute');
  rejects(() => schema.type({ value: { type: 'Object', value: { members: [], attributes: [{ name: 'tuple', value: null }] } } } as never), 'schema', 'pssz IR: malformed tuple attribute');
  // The typed spelling of the same three types.
  const attributesOf = (def: TypeDef<any, any>) =>
    (def.schema.ir.types.find((m) => m.name === def.name)!.type.value.value as { attributes: unknown[]; members: { name: string }[] });
  assert.deepEqual(attributesOf(argumentTuple({ a: u32, b: u32 })).attributes, [{ name: 'tuple', value: 'named' }]);
  assert.deepEqual(attributesOf(argumentTuple([u32, u32])).attributes, [{ name: 'tuple', value: 'positional' }]);
  assert.deepEqual(attributesOf(argumentTuple([u32, u32])).members.map((m) => m.name), ['0', '1']);
  assert.deepEqual(attributesOf(argumentTuple({})).attributes, [{ name: 'tuple', value: 'named' }]);
  const typedPositional = argumentTuple([u32, optional(utf8)]);
  assert.deepEqual(typedPositional.decode(typedPositional.encode([7, null])), [7, null]);
  assert.equal(typedPositional.valueToJson([7, 'x']), '[7,"x"]');
  assert.deepEqual(typedPositional.jsonToValue('[7,null]'), [7, null]);
  rejects(() => typedPositional.jsonToValue('[7]'), 'json', 'json: array has too few elements');   // exactly one element per parameter
  // A trailing absent optional may be left out of the value, as in a record.
  assert.equal(toHex(typedPositional.encode([7] as never)), toHex(typedPositional.encode([7, null])));
});

// ── What the fixtures pin, restated (items 3, 5, 6, 8, 14, 19) ─────────────

test('strict reads accept a raw string that is not UTF-8; a UTF-8 string that is not is refused', () => {
  const Raw = record('Raw', { s: rawString, inner: nested(record('Inner', { s: rawString })) });
  const value = { s: Uint8Array.of(0xff, 0xfe), inner: { s: Uint8Array.of(0xc0, 0x80) } };
  const frame = Raw.encode(value);
  assert.deepEqual(Raw.validate(frame), { ok: true, hasUnknown: false });
  assert.deepEqual(Raw.decode(frame, { rejectUnknown: true }), value);
  assert.deepEqual((Raw.view(frame) as RecordView).toValue({ rejectUnknown: true }), value);
  assert.deepEqual(Raw.decodeKey(Raw.encodeKey(value)), value);
  const Text = record('Raw', { s: utf8, inner: nested(record('Inner', { s: utf8 })) });
  rejects(() => Text.decode(frame, { rejectUnknown: true }), 'bytes', 'pssz IR: string is not valid UTF-8');
  // (its text is the bytes' UTF-8: a raw string that is not UTF-8 has none)
  rejects(() => Raw.valueToJson(value), 'value', 'json: string is not valid UTF-8');
});

test('the key writer checks member bounds: time-key-vectors.json KBound', () => {
  const fx: { schema_ir: string; vectors: { name: string; kind: string; type: string; key?: string; pssz?: string }[] } =
    JSON.parse(readFileSync(new URL('../../../conformance/time-key-vectors.json', import.meta.url), 'utf8'));
  const rows = fx.vectors.filter((v) => v.type === 'KBound');
  const kinds: { [kind: string]: number } = {};
  for (const row of rows) kinds[row.kind] = (kinds[row.kind] ?? 0) + 1;
  assert.deepEqual(kinds, { value: 5, 'key-prefix': 4, 'key-order': 1, 'key-reject': 7 });
  // KBound { shard: u32, name: utf8 <= 4, tags: at most 2 raw strings of at most 3 bytes }
  const KBound = record('KBound', {
    shard: u32, name: field(rawString, { utf8: true, max: 4 }), tags: field(list(rawString), { max: 2, element: { max: 3 } }),
  });
  const fetched = PsszSchema.decode(fromHex(fx.schema_ir)).type('KBound');
  // The same record without bounds: what an unchecked writer writes.
  const Plain = record('KBound', { shard: u32, name: rawString, tags: list(rawString) });
  let refusedByWriters = 0;
  let noValue = 0;
  for (const row of rows.filter((v) => v.kind === 'key-reject')) {
    const key = fromHex(row.key!);
    for (const reader of [KBound.type() as PsszType, fetched]) {
      const refusal = reader.validateKey(key);
      assert.ok(!refusal.ok && refusal.error.kind === 'bytes', row.name);
    }
    const loose = Plain.decodeKey(key);
    assert.equal(toHex(Plain.encodeKey(loose)), row.key, row.name);
    let name: string;
    try {
      name = new TextDecoder('utf-8', { fatal: true }).decode(loose.name);
    } catch {
      // A JavaScript string is always text: there is no value to hand a writer.
      assert.match(row.name, /utf8/, row.name);
      ++noValue;
      continue;
    }
    const value = { shard: loose.shard, name, tags: loose.tags };
    for (const writer of [KBound.type() as PsszType, fetched]) {
      const refusal = rejects(() => writer.encodeKey(value), 'value');
      assert.match(refusal.message, /^key: (string exceeds its bound|bytes exceed their bound|list exceeds its bound|member length bound violated)$/, row.name);
      rejects(() => writer.encode(value), 'value');
      rejects(() => writer.encodeKeyPrefix([value.shard, value.name, value.tags]), 'value');
      rejects(() => writer.valueToJson(value), 'value');
    }
    ++refusedByWriters;
  }
  assert.deepEqual([refusedByWriters, noValue], [4, 3]);
  // A value at the bounds has a key.
  for (const row of rows.filter((v) => v.kind === 'value')) {
    assert.equal(toHex(KBound.encodeKey(KBound.decode(fromHex(row.pssz!)))), row.key, row.name);
    assert.equal(toHex(fetched.encodeKey(fetched.decode(fromHex(row.pssz!)))), row.key, row.name);
  }
});

test('reserved custom ids are refused; a reserved id on another wire shape is a malformed schema', () => {
  for (const id of ['bool', 'char', 'string', 'utf8', 'hex', 'bitvector', 'bitlist', 'map', 'set', 'box', 'unknown',
    'time-point', 'time-point-seconds', 'duration', 'duration-1/60', 'monotonic-millis']) {
    rejects(() => custom(id, u64, {}), 'schema', /is a built-in id, not a custom type/);
    rejects(() => new PsszSchema({ T: { Int: { bits: 64, isSigned: false } } }, { handlers: { [id]: {} } }), 'schema');
  }
  for (const id of ['Bool', 'AccountNumber', 'time', 'durations', 'time-points', 'monotonically', 'hexadecimal', 'Memo']) {
    assert.ok(custom(id, u64, {}).encode(1n), id);
    assert.ok(new PsszSchema({ T: { Int: { bits: 64, isSigned: false } } }, { handlers: { [id]: {} } }), id);
  }
  const U8LIST: PsszTypeInput = { List: { Int: { bits: 8, isSigned: false } } };
  const int = (bits: number, isSigned = false): PsszTypeInput => ({ Int: { bits, isSigned } });
  const shapes: [PsszTypeInput, string][] = [
    [U8LIST, 'bool'], [int(32), 'bool'], [int(32), 'char'], [int(64), 'string'], [int(64), 'utf8'], [int(64), 'hex'],
    [int(64), 'bitlist'], [int(64), 'bitvector'], [int(64), 'map'], [U8LIST, 'map'], [int(64), 'set'],
    [U8LIST, 'time-point-seconds'], [{ Float: { exp: 11, mantissa: 52 } }, 'duration-millis'], [int(64, true), 'time-point'],
    [int(64, true), 'duration-bogus'], [int(32), 'unknown'],
  ];
  for (const [type, id] of shapes) rejects(() => new PsszSchema({}).type({ Custom: { type, id } }), 'schema');
});

test('enums: any value of the underlying integer is carried; text takes the name or the number', () => {
  const Level = enumeration(u8, { low: 0, high: 2 });
  const Holder = record('Holder', { level: Level, more: list(Level) });
  const value = { level: 1, more: [0, 2, 7] };
  const frame = Holder.encode(value);
  // (the underlying integers: 1, then the three)
  assert.equal(toHex(frame), '0500000001' + '05000000' + '000207');
  assert.deepEqual(Holder.decode(frame, { rejectUnknown: true }), value);
  assert.deepEqual(Holder.decodeKey(Holder.encodeKey(value)), value);
  for (const format of ['json', 'ijson'] as const) {
    assert.equal(Holder.valueToJson(value, format), '{"level":1,"more":["low","high",7]}');
    assert.deepEqual(Holder.jsonToValue('{"level":"high","more":[0,"low",7,"7"]}', format), { level: 2, more: [0, 0, 7, 7] });
    rejects(() => Holder.jsonToValue('{"level":"middle","more":[]}', format), 'json');
    rejects(() => Holder.jsonToValue('{"level":256,"more":[]}', format), 'json');
  }
});

test('JSON: a missing required member is refused, a missing optional is null; a cap is not a limit on the text', () => {
  const R = record('R', { id: u32, note: optional(utf8), inner: optional(record('In', { a: u8, b: optional(u8) })) });
  for (const format of ['json', 'ijson'] as const) {
    assert.deepEqual(R.jsonToValue('{"id":1}', format), { id: 1, note: null, inner: null });
    assert.deepEqual(R.jsonToValue('{"id":1,"inner":{"a":2}}', format), { id: 1, note: null, inner: { a: 2, b: null } });
    rejects(() => R.jsonToValue('{"note":"x"}', format), 'json', 'json: missing required member');
    rejects(() => R.jsonToValue('{"id":1,"inner":{}}', format), 'json', 'json: missing required member');
    rejects(() => R.jsonToPssz('{}', format), 'json', 'json: missing required member');
  }
  // maxDynamicData caps the encoded PSSZ size, not the JSON text.
  const Capped = record('Capped', { a: u64, b: u64 }, { fixed: true, maxDynamicData: 16 });
  const value = { a: 0xffffffffffffffffn, b: 2n };
  assert.equal(Capped.encode(value).length, 16);
  for (const format of ['json', 'ijson'] as const) {
    const text = Capped.valueToJson(value, format);
    assert.ok(text.length > 16);
    assert.deepEqual(Capped.jsonToValue(text, format), value);
    assert.deepEqual(Capped.jsonToValue(`   ${text}${' '.repeat(64)}`, format), value);
    assert.equal(Capped.jsonToPssz(`   ${text}${' '.repeat(64)}`, format).length, 16);
  }
});

// ── Transparent types with a bounded member ─────────────────────────────────

test('transparent: a bounded member makes the wrapper that bounded type (C++ "[consistency][transparent]")', () => {
  // psibase's Memo: UTF-8 and at most 80 bytes; the same type spelled with a wrapper type.
  const Memo = transparent('Memo', field(rawString, { utf8: true, max: 80 }));
  const WrapMemo = transparent('Memo', boundedUtf8(80));
  const Tags = transparent('Tags', field(list(rawString), { max: 4, element: { utf8: true, max: 8 } }));
  const holder = (M: TypeDef<string, string>, T: TypeDef<string[], any>) => record('Holder', {
    id: u32, memo: M, maybe: optional(M), many: list(M), by_id: map(u8, M), tags: T,
  });
  const Ann = holder(Memo, Tags);
  const Wrap = holder(WrapMemo, transparent('Tags', list(boundedUtf8(8)) as never));
  const value = { id: 1, memo: 'hi', maybe: 'x'.repeat(80), many: ['a', ''], by_id: new Map([[1, 'm']]), tags: ['t'.repeat(8)] };
  // The wrapper is the bounded string: its bytes, bound, width and key.
  const memo: string = Memo.decode(fromHex('6869'));
  assert.equal(memo, 'hi');
  assert.equal(toHex(Memo.encode('hi')), '6869');
  assert.equal(Memo.inferredBound, 80n);
  assert.equal(Memo.width, 1);
  assert.equal(toHex(Memo.encodeKey('hi')), toHex(boundedUtf8(80).encodeKey('hi')));
  assert.equal(Memo.valueToJson('hi'), '"hi"');
  assert.equal(Tags.inferredBound, 4n * (1n + 8n));
  // Both spellings: the same bytes, widths and bounds, wherever the type appears.
  assert.equal(toHex(Ann.encode(value)), toHex(holder(WrapMemo, Tags).encode(value)));
  assert.equal(Ann.width, holder(WrapMemo, Tags).width);
  assert.equal(Ann.inferredBound, holder(WrapMemo, Tags).inferredBound);
  assert.deepEqual(Ann.decode(Ann.encode(value), { rejectUnknown: true }), value);
  void Wrap;
  // The Schema IR: a named alias of the bounded type, an anonymous type of its own.
  const text = { value: { type: 'Custom', value: { id: 'utf8', type: { value: { type: 'BoundedList', value: { type: { value: { type: 'Type', value: { type: '@2' } } }, maxCount: 80n } } } } } };
  for (const def of [Memo, WrapMemo])
    assert.deepEqual(generateSchema({ Memo: def }).ir.types.map((m) => [m.name, m.type]), [
      ['Memo', { value: { type: 'Type', value: { type: '@1' } } }], ['@1', text],
      ['@2', { value: { type: 'Int', value: { bits: 8, isSigned: false } } }],
    ]);
  // Every writer refuses a Memo over 80 bytes; every reader rejects its bytes: bare, a member, an optional payload, a list element, a mapped value.
  const long = 'x'.repeat(81);
  for (const M of [Memo, WrapMemo]) {
    rejects(() => M.encode(long), 'value', 'pssz IR: byte count bound exceeded');
    rejects(() => M.encodeKey(long), 'value', 'key: string exceeds its bound');
    rejects(() => M.valueToJson(long), 'value', 'json: string exceeds its bound');
    rejects(() => M.jsonToValue(JSON.stringify(long)), 'json', 'json: string exceeds its bound');
    assert.equal(M.validate(Buffer.from(long)).ok, false);
    assert.equal(M.validate(Uint8Array.of(0xff)).ok, false);
    assert.equal(M.validateKey(utf8.encodeKey(long)).ok, false);
    const H = holder(M, Tags);
    for (const bad of [{ ...value, memo: long }, { ...value, maybe: long }, { ...value, many: ['a', long] }, { ...value, by_id: new Map([[1, long]]) }]) {
      rejects(() => H.encode(bad), 'value', 'pssz IR: byte count bound exceeded');
      rejects(() => H.encodeKey(bad), 'value');
      rejects(() => H.valueToJson(bad), 'value');
    }
    rejects(() => H.encode({ ...value, tags: ['t'.repeat(9)] }), 'value');
    rejects(() => H.encode({ ...value, tags: ['a', 'b', 'c', 'd', 'e'] }), 'value');
  }
  // What a type cannot say is refused where it is declared.
  const schemaError = (run: () => unknown, message: RegExp) => rejects(run, 'schema', message);
  schemaError(() => transparent('M', field(rawString, { lengthBound: { min: 1 } })), /lengthBound is a record member's/);
  schemaError(() => transparent('M', field(u64, { jsonInt: 'string' })), /jsonInt is a record member's/);
  schemaError(() => transparent('M', field(u32, { max: 4 })), /max bounds a rawString or bytes/);
  schemaError(() => transparent('M', field(optional(rawString), { max: 4 })), /not an optional/);
  // bytes and maps take a bound too.
  assert.equal(transparent('Blob', field(bytes, { max: 4 })).inferredBound, 4n);
  assert.equal(transparent('Votes', field(map(u8, u8), { max: 3 })).inferredBound, map(u8, u8, { max: 3 }).inferredBound);
  assert.notEqual(map(u8, u8, { max: 3 }).inferredBound, null);
});
