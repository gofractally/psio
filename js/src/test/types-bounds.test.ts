// Bounds at any depth and method argument tuples in the typed layer
// (`docs/pssz-spec.md` §3.5, §16): declarations written by hand, as an
// application would write them, against the C++ fixture's types of the same
// names — their Schema IR, their static types, every fixture vector, and the
// checks of every writer and reader.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  ListView, PsszError, PsszSchema, RecordView, TupleView, TypeDef, VariantView, alt, argumentTuple, array, boundedList,
  boundedUtf8, bytes, field, generateSchema, list, map, nested, optional, pair, rawString, record, set, tuple, u16, u32,
  u64, u8, utf8, variant,
  type Bounded, type FieldOptions, type Infer, type InferView, type IrAnyType, type IrSchema, type PsszWidth,
} from '../index.js';

const fixture = (name: string) =>
  JSON.parse(readFileSync(new URL(`../../../conformance/${name}`, import.meta.url), 'utf8'));
const fromHex = (hex: string) => Uint8Array.from(Buffer.from(hex, 'hex'));
const toHex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');
const raw = (text: string) => new TextEncoder().encode(text);

type Equal<A, B> = (<X>() => X extends A ? 1 : 2) extends <X>() => X extends B ? 1 : 2 ? true : false;
const sameType = <A, B>(_ok: Equal<A, B>) => undefined;

function throwsKind(run: () => unknown, kind: string, pattern?: RegExp): PsszError {
  try {
    run();
  } catch (e) {
    assert.ok(e instanceof PsszError, `threw ${String(e)}`);
    assert.equal(e.kind, kind, e.message);
    if (pattern) assert.match(e.message, pattern);
    return e;
  }
  return assert.fail('did not throw');
}

function thrown(run: () => unknown): PsszError {
  try {
    run();
  } catch (e) {
    assert.ok(e instanceof PsszError, `threw ${String(e)}`);
    assert.ok(e.kind === 'value' || e.kind === 'json', e.kind);
    return e;
  }
  return assert.fail('did not throw');
}

// ── The types of cpp/tests/pssz_conformance_types.hpp, written by hand ──────

const TEXT16 = { utf8: true, max: 16 } as const;

// attr(tags, max<8> | element(utf8 | max<16>)) on a std::vector<std::string>
const AnnTags = record('AnnTags', { tags: field(list(rawString), { max: 8, element: { utf8: true, max: 16 } }) });
// psio::bounded<psio::utf8_string<16>, 8>: the same constraint in the type
const WrapTags = record('WrapTags', { tags: boundedList(boundedUtf8(16), 8) });
const AnnRawTags = record('AnnRawTags', { tags: field(list(rawString), { max: 8, element: { max: 16 } }) });
const AnnGrid = record('AnnGrid', { rows: field(list(list(u16)), { max: 3, element: { max: 4 } }) });
const AnnOpt = record('AnnOpt', { names: field(optional(list(rawString)), { max: 2, element: TEXT16 }) });
const AnnArr = record('AnnArr', { names: field(array(rawString, 3), { element: TEXT16 }) });
const AnnMap = record('AnnMap', {
  m: field(map(rawString, list(u32)), { max: 4, keys: TEXT16, values: { max: 2 } }),
});
const AnnSet = record('AnnSet', { s: field(set(rawString), { max: 4, element: TEXT16 }) });
const AnnTup = record('AnnTup', {
  t: field(tuple(rawString, u32, bytes), { at: { 0: TEXT16, 2: { max: 5 } } }),
  p: field(pair(rawString, rawString), { at: { 0: TEXT16, 1: TEXT16 } }),
});
const AnnVar = record('AnnVar', {
  v: field(
    variant({
      0: alt(u32, { ijsonName: 'uint32' }), 1: alt(rawString, { ijsonName: 'string' }),
      2: alt(list(rawString), { ijsonName: 'string[]' }),
    }),
    { at: { 1: TEXT16, 2: { max: 2, element: TEXT16 } } },
  ),
});

// method(transfer, attr(to, utf8 | max<16>), amount, attr(memo, utf8 | max<80>), maxDynamicData(256))
const transferParameters = {
  to: field(rawString, { utf8: true, max: 16 }), amount: u64, memo: field(rawString, { utf8: true, max: 80 }),
};
const ArgsTransfer = argumentTuple(transferParameters, { maxDynamicData: 256 });
const ArgsTransferUncapped = argumentTuple(transferParameters);
const ArgsBatch = argumentTuple({ names: field(list(rawString), { max: 4, element: { max: 8 } }), tip: optional(u32) });
const ArgsNote = argumentTuple({ text: rawString }, { maxDynamicData: 64 });
const ArgsTransfer2 = argumentTuple({ ...transferParameters, fee: optional(u32) }, { maxDynamicData: 256 });

const HAND: { [id: string]: TypeDef<any, any> } = {
  AnnTags, WrapTags, AnnRawTags, AnnGrid, AnnOpt, AnnArr, AnnMap, AnnSet, AnnTup, AnnVar,
  'tuple<string<=16,u64,string<=80>@cap=256': ArgsTransfer,
  'tuple<string<=16,u64,string<=80>': ArgsTransferUncapped,
  'tuple<list<raw_string<=8><=4,optional<u32>>': ArgsBatch,
  'tuple<raw_string>@cap=64': ArgsNote,
  'tuple<string<=16,u64,string<=80,optional<u32>>@cap=256': ArgsTransfer2,
};

// A type with every reference replaced by what it refers to.
function expanded(schema: IrSchema, type: IrAnyType): unknown {
  const byName = new Map(schema.types.map((m) => [m.name, m.type]));
  const walk = (x: unknown): unknown => {
    if (typeof x === 'bigint') return String(x);
    if (Array.isArray(x)) return x.map(walk);
    if (typeof x !== 'object' || x === null) return x;
    const node = x as { type?: string; value?: { type?: string } };
    if (node.type === 'Type' && typeof node.value?.type === 'string') return walk(byName.get(node.value.type)!.value);
    return Object.fromEntries(Object.entries(x).map(([key, value]) => [key, walk(value)]));
  };
  return walk(type.value);
}
const named = (name: string): IrAnyType => ({ value: { type: 'Type', value: { type: name } } } as IrAnyType);

test('bounds at any depth: the hand-written declarations are the C++ types', () => {
  const fx: { schema_ir: string; types: { id: string; auto_width: number }[] } = fixture('pssz-vectors.json');
  const cpp = PsszSchema.decode(fromHex(fx.schema_ir)).ir;
  const rows = new Map(fx.types.map((t) => [t.id, t]));
  for (const [id, def] of Object.entries(HAND)) {
    assert.deepEqual(expanded(def.schema.ir, named(def.name)), expanded(cpp, named(id)), id);
    assert.equal(def.width, rows.get(id)!.auto_width, `width of ${id}`);
  }
  // The annotation and the wrapper spelling describe one type.
  const member = (def: TypeDef<any, any>) =>
    expanded(def.schema.ir, (def.schema.ir.types.find((m) => m.name === def.name)!.type.value.value as { members: { type: IrAnyType }[] }).members[0].type);
  assert.deepEqual(member(AnnTags), member(WrapTags));

  // As C++: the member's plain type is registered, then the member is
  // written as the type its bounds describe, in place.
  const U8 = { value: { type: 'Int', value: { bits: 8, isSigned: false } } };
  const U16 = { value: { type: 'Int', value: { bits: 16, isSigned: false } } };
  const ref = (type: string) => ({ value: { type: 'Type', value: { type } } });
  assert.deepEqual(generateSchema({ AnnGrid }).ir.types, [
    { name: 'AnnGrid', attributes: [], type: { value: { type: 'Object', value: { attributes: [], members: [{
      name: 'rows', attributes: [],
      type: { value: { type: 'BoundedList', value: { maxCount: 3n, type: { value: { type: 'BoundedList', value: { maxCount: 4n, type: ref('@3') } } } } } },
    }] } } } },
    { name: '@1', attributes: [], type: { value: { type: 'List', value: { type: ref('@2') } } } },
    { name: '@2', attributes: [], type: { value: { type: 'List', value: { type: ref('@3') } } } },
    { name: '@3', attributes: [], type: U16 },
  ]);
  // A parameter's plain type is not registered; every argument tuple is a
  // type of its own, an unnamed Object carrying `tuple` and the cap.
  const two = generateSchema({ transfer: ArgsTransfer, uncapped: ArgsTransferUncapped }).ir.types;
  assert.deepEqual(two.map((m) => m.name), ['@0', '@1', '@2', 'transfer', '@3', 'uncapped']);
  assert.deepEqual(two[1].type, U8);
  const text = (max: bigint) => ({ value: { type: 'Custom', value: { id: 'utf8', type: { value: { type: 'BoundedList', value: { type: ref('@1'), maxCount: max } } } } } });
  const parameters = [
    { name: 'to', type: text(16n), attributes: [] }, { name: 'amount', type: ref('@2'), attributes: [] },
    { name: 'memo', type: text(80n), attributes: [] },
  ];
  assert.deepEqual(two[0].type, { value: { type: 'Object', value: { members: parameters, attributes: [{ name: 'tuple', value: 'named' }, { name: 'maxDynamicData', value: '256' }] } } });
  assert.deepEqual(two[4].type, { value: { type: 'Object', value: { members: parameters, attributes: [{ name: 'tuple', value: 'named' }] } } });
  // (the schema travels and comes back the same)
  const travelled = generateSchema({ AnnTags, AnnMap, AnnTup, AnnVar, transfer: ArgsTransfer });
  assert.deepEqual(PsszSchema.decode(travelled.encode()).ir, travelled.ir);
});

test('bounds at any depth: the decoded value is statically typed', () => {
  // `utf8: true` makes a raw string a string, at the position it is written for.
  sameType<Infer<typeof AnnTags>, { tags: string[] }>(true);
  sameType<Infer<typeof WrapTags>, { tags: string[] }>(true);
  sameType<Infer<typeof AnnRawTags>, { tags: Uint8Array[] }>(true);
  sameType<Infer<typeof AnnGrid>, { rows: number[][] }>(true);
  sameType<Infer<typeof AnnOpt>, { names: string[] | null }>(true);
  sameType<Infer<typeof AnnArr>, { names: string[] }>(true);
  sameType<Infer<typeof AnnMap>, { m: Map<string, number[]> }>(true);
  sameType<Infer<typeof AnnSet>, { s: Set<string> }>(true);
  sameType<Infer<typeof AnnTup>, { t: [string, number, Uint8Array]; p: [string, string] }>(true);
  sameType<
    Infer<typeof AnnVar>,
    { v: { type: '0'; value: number } | { type: '1'; value: string } | { type: '2'; value: string[] } }
  >(true);
  sameType<Infer<typeof ArgsTransfer>, { to: string; amount: bigint; memo: string }>(true);
  sameType<Infer<typeof ArgsBatch>, { names: Uint8Array[]; tip: number | null }>(true);
  sameType<Infer<typeof ArgsTransfer2>, { to: string; amount: bigint; memo: string; fee: number | null }>(true);
  // Only where it is said: a key made text, the mapped bytes left alone; an
  // alternative by name; a tuple element by index.
  sameType<Bounded<Map<Uint8Array, Uint8Array[]>, { keys: { utf8: true } }>, Map<string, Uint8Array[]>>(true);
  sameType<Bounded<[Uint8Array, Uint8Array], { at: { 1: { utf8: true } } }>, [Uint8Array, string]>(true);
  sameType<
    Bounded<{ type: 'Text'; value: Uint8Array } | { type: 'Blob'; value: Uint8Array }, { at: { Text: { utf8: true } } }>,
    { type: 'Text'; value: string } | { type: 'Blob'; value: Uint8Array }
  >(true);
  sameType<Bounded<Uint8Array | null, { utf8: true; max: 4 }>, string | null>(true);
  sameType<Bounded<Uint8Array, { max: 4 }>, Uint8Array>(true);
  sameType<Bounded<Uint8Array, { utf8: false }>, Uint8Array>(true);
  sameType<Bounded<{ type: Uint8Array; value: Uint8Array }, { max: 4 }>, { type: Uint8Array; value: Uint8Array }>(true);
  // Views follow.
  sameType<InferView<typeof AnnTags>, RecordView<{ tags: string[] }, { tags: ListView<string[], string> }>>(true);
  sameType<
    InferView<typeof AnnMap>,
    RecordView<{ m: Map<string, number[]> }, {
      m: ListView<Map<string, number[]>, TupleView<[string, number[]], [string, ListView<number[], number>]>>;
    }>
  >(true);
  sameType<
    InferView<typeof AnnTup>,
    RecordView<Infer<typeof AnnTup>, {
      t: TupleView<[string, number, Uint8Array], [string, number, Uint8Array]>;
      p: TupleView<[string, string], [string, string]>;
    }>
  >(true);
  sameType<
    InferView<typeof AnnVar>,
    RecordView<Infer<typeof AnnVar>, {
      v:
        | VariantView<{ type: '0'; value: number }, '0', number>
        | VariantView<{ type: '1'; value: string }, '1', string>
        | VariantView<{ type: '2'; value: string[] }, '2', ListView<string[], string>>;
    }>
  >(true);
  sameType<
    InferView<typeof AnnOpt>,
    RecordView<{ names: string[] | null }, { names: ListView<string[], string> | null }>
  >(true);

  // The values are what the types say.
  const tags = AnnTags.decode(AnnTags.encode({ tags: ['ab', 'c'] }));
  assert.deepEqual(tags, { tags: ['ab', 'c'] });
  const rawTags = AnnRawTags.decode(AnnRawTags.encode({ tags: [raw('ab'), Uint8Array.of(0xff)] }));
  assert.deepEqual(rawTags, { tags: [raw('ab'), Uint8Array.of(0xff)] });
  const m = AnnMap.decode(AnnMap.encode({ m: new Map([['k', [7]]]) }));
  assert.deepEqual([...m.m], [['k', [7]]]);
  const t = AnnTup.decode(AnnTup.encode({ t: ['x', 1, Uint8Array.of(1, 2)], p: ['a', 'b'] }));
  assert.deepEqual(t, { t: ['x', 1, Uint8Array.of(1, 2)], p: ['a', 'b'] });
  const v = AnnVar.decode(AnnVar.encode({ v: { type: '2', value: ['x'] } }));
  assert.deepEqual(v, { v: { type: '2', value: ['x'] } });
  assert.equal(AnnTags.view(AnnTags.encode({ tags: ['ab', 'c'] })).get('tags').at(1), 'c');
  assert.deepEqual(ArgsTransfer.decode(ArgsTransfer.encode({ to: 'al', amount: 5n, memo: 'hi' })), { to: 'al', amount: 5n, memo: 'hi' });
  assert.equal(ArgsTransfer.view(ArgsTransfer.encode({ to: 'al', amount: 5n, memo: 'hi' })).get('amount'), 5n);
});

test('bounds at any depth: every fixture vector of the types, through the hand-written declarations', () => {
  const fx: { vectors: { name: string; class: string; type: string; format: string; hex: string; expect: string; reencoded: string }[] } =
    fixture('pssz-vectors.json');
  const count = { vectors: 0, canonical: 0, unknown: 0, reject: 0, text: 0 };
  for (const row of fx.vectors) {
    const def = HAND[row.type];
    if (!def) continue;
    ++count.vectors;
    const width: PsszWidth = row.format === 'auto' ? 'auto' : row.format === 'pssz8' ? 1 : row.format === 'pssz16' ? 2 : 4;
    const type = def.type({ width });
    const frame = fromHex(row.hex);
    const result = type.validate(frame);
    if (row.expect === 'reject') {
      assert.equal(result.ok, false, row.name);
      throwsKind(() => type.decode(frame), 'bytes');
      ++count.reject;
      continue;
    }
    assert.deepEqual(result, { ok: true, hasUnknown: row.expect === 'unknown' }, row.name);
    const value = type.decode(frame);
    assert.equal(toHex(type.encode(value)), row.expect === 'unknown' ? row.reencoded : row.hex, row.name);
    ++count[row.expect as 'canonical' | 'unknown'];
    if (row.expect !== 'canonical' || row.class === 'string-raw-non-utf8') continue;
    for (const format of ['json', 'ijson'] as const)
      assert.equal(toHex(type.jsonToPssz(type.valueToJson(value, format), format)), row.hex, `${row.name}: ${format}`);
    assert.deepEqual(type.decodeKey(type.encodeKey(value)), value, `${row.name}: key`);
    ++count.text;
  }
  // Every vector of the 15 types: classes annotated-bounds (148) and
  // method-args (62), and annotated_raw_tags of string-raw-non-utf8 (4).
  assert.deepEqual(count, { vectors: 214, canonical: 101, unknown: 4, reject: 109, text: 97 });

  // The pinned bytes (conformance/pssz-vectors.md). A bounded list's own
  // offset table is as wide as its bound allows (02 04: two 1-byte offsets).
  assert.equal(toHex(AnnTags.encode({ tags: ['ab', 'c'] })), '01010204616263');
  assert.equal(toHex(WrapTags.encode({ tags: ['ab', 'c'] })), '01010204616263');
  // an array of bounded strings: its own offset table is 1 wide
  assert.equal(toHex(AnnArr.encode({ names: ['ab', '', 'c'] })), '0101030505616263');
  // a map entry of a bounded key and value: a 1-wide header in the map's own 1-wide table
  assert.equal(toHex(AnnMap.encode({ m: new Map([['k', [7]]]) })), '0101010202036b07000000');
  // the inferred bound (107: 1-byte offsets), and the explicit cap that rules (256: 2-byte)
  const transfer = { to: 'al', amount: 5n, memo: 'hi' };
  assert.equal(toHex(ArgsTransferUncapped.encode(transfer)), '0a0a05000000000000000c616c6869');
  assert.equal(toHex(ArgsTransfer.encode(transfer)), '0c000c0005000000000000000e00616c6869');
  assert.equal(toHex(ArgsNote.encode({ text: raw('hey') })), '0101686579');
  assert.deepEqual([ArgsTransferUncapped.width, ArgsTransfer.width, ArgsBatch.width, ArgsNote.width, ArgsTransfer2.width], [1, 2, 1, 1, 2]);
  // The arguments are the bytes of the tuple of the same bounded types.
  assert.equal(
    toHex(tuple(boundedUtf8(16), u64, boundedUtf8(80)).encode(['al', 5n, 'hi'])),
    toHex(ArgsTransferUncapped.encode(transfer)),
  );
});

test('bounds at any depth: every writer and reader refuses a value outside a bound', () => {
  const s16 = 'a'.repeat(16);
  const s17 = 'a'.repeat(17);
  // Each type, its twin without the bounds, a value at the bounds and the
  // values one past each of them.
  const cases: { name: string; type: TypeDef<any, any>; twin: TypeDef<any, any>; at: unknown; past: unknown[] }[] = [
    {
      name: 'AnnTags', type: AnnTags, twin: record('AnnTags', { tags: list(utf8) }),
      at: { tags: Array(8).fill(s16) }, past: [{ tags: Array(9).fill('a') }, { tags: ['a', s17] }],
    },
    {
      name: 'WrapTags', type: WrapTags, twin: record('WrapTags', { tags: list(utf8) }),
      at: { tags: Array(8).fill(s16) }, past: [{ tags: Array(9).fill('a') }, { tags: ['a', s17] }],
    },
    {
      name: 'AnnRawTags', type: AnnRawTags, twin: record('AnnRawTags', { tags: list(rawString) }),
      at: { tags: [raw(s16)] }, past: [{ tags: [raw(s17)] }],
    },
    {
      name: 'AnnGrid', type: AnnGrid, twin: record('AnnGrid', { rows: list(list(u16)) }),
      at: { rows: [[1, 2, 3, 4], [], [5]] }, past: [{ rows: [[1], [1], [1], [1]] }, { rows: [[], [1, 2, 3, 4, 5]] }],
    },
    {
      name: 'AnnOpt', type: AnnOpt, twin: record('AnnOpt', { names: optional(list(utf8)) }),
      at: { names: ['x', s16] }, past: [{ names: ['a', 'b', 'c'] }, { names: [s17] }],
    },
    {
      name: 'AnnArr', type: AnnArr, twin: record('AnnArr', { names: array(utf8, 3) }),
      at: { names: [s16, s16, s16] }, past: [{ names: ['', s17, ''] }],
    },
    {
      name: 'AnnMap', type: AnnMap, twin: record('AnnMap', { m: map(utf8, list(u32)) }),
      at: { m: new Map([['a', [1, 2]], ['b', []], ['c', [3]], [s16, [4, 5]]]) },
      past: [
        { m: new Map(['a', 'b', 'c', 'd', 'e'].map((k) => [k, []])) }, { m: new Map([[s17, []]]) },
        { m: new Map([['k', [1, 2, 3]]]) },
      ],
    },
    {
      name: 'AnnSet', type: AnnSet, twin: record('AnnSet', { s: set(utf8) }),
      at: { s: new Set(['a', 'b', s16]) }, past: [{ s: new Set([s17]) }, { s: new Set(['a', 'b', 'c', 'd', 'e']) }],
    },
    {
      name: 'AnnTup', type: AnnTup, twin: record('AnnTup', { t: tuple(utf8, u32, bytes), p: pair(utf8, utf8) }),
      at: { t: [s16, 7, Uint8Array.of(1, 2, 3, 4, 5)], p: ['p', s16] },
      past: [
        { t: [s17, 1, new Uint8Array(0)], p: ['', ''] }, { t: ['', 1, new Uint8Array(6)], p: ['', ''] },
        { t: ['', 1, new Uint8Array(0)], p: ['', s17] },
      ],
    },
    {
      name: 'AnnVar', type: AnnVar,
      twin: record('AnnVar', { v: variant({ 0: alt(u32, { ijsonName: 'uint32' }), 1: alt(utf8, { ijsonName: 'string' }), 2: alt(list(utf8), { ijsonName: 'string[]' }) }) }),
      at: { v: { type: '2', value: ['x', s16] } },
      past: [{ v: { type: '1', value: s17 } }, { v: { type: '2', value: ['a', 'b', 'c'] } }, { v: { type: '2', value: [s17] } }],
    },
    {
      name: 'transfer', type: ArgsTransfer, twin: argumentTuple({ to: utf8, amount: u64, memo: utf8 }, { maxDynamicData: 256 }),
      at: { to: s16, amount: 1n, memo: 'm'.repeat(80) },
      past: [{ to: s17, amount: 1n, memo: '' }, { to: '', amount: 1n, memo: 'm'.repeat(81) }],
    },
    {
      name: 'batch', type: ArgsBatch, twin: argumentTuple({ names: list(rawString), tip: optional(u32) }),
      at: { names: Array(4).fill(raw('12345678')), tip: 9 },
      past: [{ names: Array(5).fill(raw('a')), tip: null }, { names: [raw('123456789')], tip: 1 }],
    },
  ];
  let refused = 0;
  for (const { name, type, twin, at, past } of cases) {
    assert.deepEqual(type.validate(type.encode(at)), { ok: true, hasUnknown: false }, name);
    assert.deepEqual(type.decodeKey(type.encodeKey(at)), at, name);
    for (const format of ['json', 'ijson'] as const) assert.deepEqual(type.jsonToValue(type.valueToJson(at, format), format), at, name);
    for (const value of past) {
      // The writers: binary, text, key.
      throwsKind(() => type.encode(value), 'value', /bound/);
      throwsKind(() => type.encodeKey(value), 'value', /bound/);
      for (const format of ['json', 'ijson'] as const) {
        throwsKind(() => type.valueToJson(value, format), 'value', /bound/);
        const text = twin.valueToJson(value, format);
        // The text readers.
        assert.match(thrown(() => type.jsonToValue(text, format)).message, /bound/);
        assert.match(thrown(() => type.jsonToPssz(text, format)).message, /bound/);
      }
      // The key reader.
      const key = twin.encodeKey(value);
      const keyResult = type.validateKey(key);
      assert.ok(!keyResult.ok && keyResult.error.kind === 'bytes' && /bound/.test(keyResult.error.message), name);
      throwsKind(() => type.decodeKey(key), 'bytes', /bound/);
      // The binary readers, under every profile: the frame a writer that
      // skips the check would produce.
      for (const width of [1, 2, 4] as const) {
        const frame = twin.type({ width }).encode(value);
        const reader = type.type({ width });
        assert.equal(reader.validate(frame).ok, false, name);
        throwsKind(() => reader.decode(frame), 'bytes', /bound exceeded/);
        throwsKind(() => reader.decode(frame, { rejectUnknown: true }), 'bytes', /bound exceeded/);
        throwsKind(() => reader.psszToJson(frame), 'bytes', /bound exceeded/);
        throwsKind(() => reader.view(frame).toValue(), 'bytes', /bound exceeded/);
      }
      ++refused;
    }
  }
  assert.equal(refused, 25);
  // A UTF-8 position holds UTF-8: bytes that are not are refused by the
  // readers (a JavaScript string is always text, so no writer can be asked).
  const notText = record('AnnTags', { tags: list(rawString) });
  const bad = { tags: [raw('ok'), Uint8Array.of(0xff)] };
  for (const width of [1, 2, 4] as const)
    throwsKind(() => AnnTags.type({ width }).decode(notText.type({ width }).encode(bad)), 'bytes', /not valid UTF-8/);
  throwsKind(() => AnnTags.decodeKey(notText.encodeKey(bad)), 'bytes', /not valid UTF-8/);
  throwsKind(() => AnnTags.encode({ tags: [Uint8Array.of(0xff)] as never }), 'value');
  // A raw bounded position takes any bytes.
  assert.deepEqual(AnnRawTags.decode(AnnRawTags.encode(bad)), bad);
});

test('argument tuples: the cap, evolution and identity', () => {
  // The cap is checked on both sides (the fixture's note_over_cap frame).
  const note = { text: raw('n'.repeat(62)) };
  assert.equal(ArgsNote.encode(note).length, 64);
  throwsKind(() => ArgsNote.encode({ text: raw('n'.repeat(63)) }), 'value', /maxDynamicData/);
  const over = new Uint8Array(65).fill(0x6e);
  over.set([1, 1]);
  const refused = ArgsNote.validate(over);
  assert.ok(!refused.ok && /maxDynamicData/.test(refused.error.message));
  throwsKind(() => ArgsNote.decode(over), 'bytes', /maxDynamicData/);
  throwsKind(() => ArgsNote.jsonToPssz(JSON.stringify({ text: 'n'.repeat(63) })), 'value', /maxDynamicData/);
  // An explicit cap rules the width, wider or narrower than the bound.
  assert.equal(argumentTuple({ text: rawString }).width, 4);
  assert.equal(argumentTuple({ text: rawString }, { maxDynamicData: 255 }).width, 1);
  assert.equal(argumentTuple({ text: field(rawString, { max: 8 }) }, { maxDynamicData: 70000 }).width, 4);
  assert.equal(argumentTuple({ text: field(rawString, { max: 300 }) }).width, 2);

  // Evolution under the cap: a trailing optional argument.
  const old = ArgsTransfer.encode({ to: 'al', amount: 5n, memo: 'hi' });
  assert.deepEqual(ArgsTransfer2.decode(old), { to: 'al', amount: 5n, memo: 'hi', fee: null });
  assert.equal(toHex(ArgsTransfer2.encode({ to: 'al', amount: 5n, memo: 'hi', fee: null })), toHex(old));
  const withFee = ArgsTransfer2.encode({ to: 'al', amount: 5n, memo: 'hi', fee: 3 });
  assert.deepEqual(ArgsTransfer.validate(withFee), { ok: true, hasUnknown: true });
  assert.equal(toHex(ArgsTransfer.encode(ArgsTransfer.decode(withFee))), toHex(old));
  throwsKind(() => ArgsTransfer.decode(withFee, { rejectUnknown: true }), 'bytes');

  // Arguments in text are the object of the parameters; their key is the tuple's.
  assert.equal(ArgsTransfer.valueToJson({ to: 'al', amount: 5n, memo: 'hi' }, 'json'), '{"to":"al","amount":5,"memo":"hi"}');
  assert.deepEqual(ArgsTransfer.jsonToValue('{"to":"al","amount":"5","memo":"hi"}'), { to: 'al', amount: 5n, memo: 'hi' });
  assert.equal(
    toHex(ArgsTransfer.encodeKey({ to: 'al', amount: 5n, memo: 'hi' })),
    toHex(tuple(utf8, u64, utf8).encodeKey(['al', 5n, 'hi'])),
  );

  // Two tuples of the same parameters are two types; one tuple is one.
  const same = argumentTuple({ a: u8 });
  const ir = generateSchema({ x: same, y: same, z: argumentTuple({ a: u8 }) }).ir.types;
  assert.deepEqual(ir.map((m) => m.name), ['@0', '@1', 'x', 'y', '@2', 'z']);
  assert.deepEqual(ir[3].type, named('@0'));
  // A member of a record, an element of a list.
  const Call = record('Call', { method: u16, args: nested(ArgsTransfer), queued: list(ArgsNote) });
  const call = { method: 7, args: { to: 'al', amount: 5n, memo: 'hi' }, queued: [{ text: raw('x') }] };
  assert.deepEqual(Call.decode(Call.encode(call)), call);
});

test('bounds at any depth: malformed declarations are schema errors', () => {
  const bad = (type: TypeDef<any, any>, options: FieldOptions, pattern: RegExp) => {
    throwsKind(() => record('R', { a: field(type, options) }).schema, 'schema', pattern);
    const result = record('R', { a: field(type, options) }).validate(new Uint8Array(4));
    assert.ok(!result.ok && result.error.kind === 'schema');
  };
  // max: a raw string, bytes, a list, a map or a set.
  bad(list(u32), { element: { max: 3 } }, /member "a", element: max bounds/);
  bad(list(utf8), { element: { max: 3 } }, /max bounds/);
  bad(list(boundedList(u8, 4)), { element: { max: 3 } }, /max bounds/);
  bad(list(nested(u32)), { element: { max: 3 } }, /max bounds/);
  bad(list(map(u8, u8, { max: 2 })), { element: { max: 3 } }, /max bounds/);
  bad(u32, { max: 3 }, /lengthBound does not apply/);
  // utf8: a raw string.
  bad(list(bytes), { element: { utf8: true } }, /utf8 applies to a rawString/);
  bad(u32, { utf8: true }, /utf8 applies to a rawString/);
  // element / keys / values / at: the shapes they speak of.
  bad(map(u8, rawString), { element: { max: 3 } }, /element bounds the elements/);
  bad(tuple(rawString), { element: { max: 3 } }, /element bounds the elements/);
  bad(bytes, { element: { max: 3 } }, /element bounds the elements/);
  bad(set(rawString), { keys: { max: 3 } }, /keys \/ values bound/);
  bad(list(rawString), { values: { max: 3 } }, /keys \/ values bound/);
  bad(list(rawString), { at: { 0: { max: 3 } } }, /at bounds an element of a tuple/);
  bad(tuple(rawString, rawString), { at: { 2: { max: 3 } } }, /at 2: not an element/);
  bad(variant({ A: rawString, B: u8 }), { at: { C: { max: 3 } } }, /at C: not an alternative/);
  bad(variant({ A: rawString, B: u8 }), { at: { A: { max: 3 }, 0: { max: 4 } } }, /named twice/);
  bad(variant({ A: rawString, B: u8 }), { at: { B: { max: 3 } } }, /at B: max bounds/);
  bad(map(rawString, u8), { keys: { element: { max: 1 } } }, /keys: element bounds/);
  // The options themselves.
  const declare = (options: unknown) => () => record('R', { a: field(list(rawString), options as FieldOptions) });
  throwsKind(declare({ max: -1 }), 'schema', /32-bit unsigned/);
  throwsKind(declare({ element: { max: 1.5 } }), 'schema', /element: max must be/);
  throwsKind(declare({ element: { utf8: 'yes' } }), 'schema', /utf8 is true or false/);
  throwsKind(declare({ element: { lengthBound: { max: 3 } } }), 'schema', /below the member itself a length bound is max/);
  throwsKind(declare({ element: { min: 1 } }), 'schema', /unknown option "min"/);
  throwsKind(declare({ elements: { max: 1 } }), 'schema', /unknown option "elements"/);
  throwsKind(declare({ element: 5 }), 'schema', /bounds must be an object/);
  throwsKind(declare({ at: 5 }), 'schema');
  throwsKind(declare({ max: 3, lengthBound: { max: 4 } }), 'schema', /states max twice/);
  // max and lengthBound.max are one bound; min / exact stay an attribute
  // and the positions below are still bounded.
  const both = record('R', { a: field(list(rawString), { max: 3, lengthBound: { max: 3 } }) });
  assert.deepEqual(both.schema.ir, record('R', { a: field(list(rawString), { max: 3 }) }).schema.ir);
  assert.deepEqual(both.schema.ir, record('R', { a: field(list(rawString), { lengthBound: { max: 3 } }) }).schema.ir);
  const ranged = record('R', { a: field(list(rawString), { lengthBound: { min: 1, max: 2 }, element: { max: 4 } }) });
  assert.equal(ranged.width, 1);
  throwsKind(() => ranged.encode({ a: [] }), 'value', /length bound/);
  throwsKind(() => ranged.encode({ a: [raw('12345')] }), 'value', /bound/);
  assert.deepEqual(ranged.decode(ranged.encode({ a: [raw('1234')] })), { a: [raw('1234')] });
  // Bounds that say nothing change nothing.
  const plain = record('R', { a: list(rawString) }).schema.ir;
  assert.deepEqual(record('R', { a: field(list(rawString), {}) }).schema.ir, plain);
  assert.deepEqual(record('R', { a: field(list(rawString), { element: {}, at: {} } as FieldOptions) }).schema.ir, plain);
  assert.deepEqual(record('R', { a: field(list(rawString), { element: { utf8: false } }) }).schema.ir, plain);
  // utf8 on the member itself: text without a bound.
  const Text = record('Text', { s: field(rawString, { utf8: true }), o: field(optional(rawString), { utf8: true, max: 4 }) });
  sameType<Infer<typeof Text>, { s: string; o: string | null }>(true);
  assert.deepEqual(Text.decode(Text.encode({ s: 'é', o: 'abcd' })), { s: 'é', o: 'abcd' });
  assert.equal(toHex(Text.encode({ s: 'é', o: null })), toHex(record('Text', { s: utf8, o: optional(boundedUtf8(4)) }).encode({ s: 'é', o: null })));
  throwsKind(() => Text.encode({ s: '', o: 'abcde' }), 'value', /bound/);

  // Parameters: what the Schema IR of an argument tuple can say.
  throwsKind(() => argumentTuple({ a: field(rawString, { lengthBound: { min: 1 } }) }).schema, 'schema', /length bound is max/);
  throwsKind(() => argumentTuple({ a: field(u64, { jsonInt: 'string' }) }).schema, 'schema', /belong to record members/);
  throwsKind(() => argumentTuple({ a: field(utf8, { max: 3 }) }).schema, 'schema', /max bounds/);
  throwsKind(() => argumentTuple({ a: field(nested(u32), { max: 3 }) }).schema, 'schema', /max bounds/);
  throwsKind(() => argumentTuple({ a: 5 as never }), 'schema');
  throwsKind(() => argumentTuple({ 3: u8 }), 'schema', /JavaScript would reorder/);
  throwsKind(() => argumentTuple({ a: u8 }, { maxDynamicData: -1 }), 'schema');
  assert.equal(argumentTuple({ a: field(rawString, { lengthBound: { max: 3 } }) }).width, 1);
});
