// Offset widths of the automatic profile (docs/pssz-spec.md 3.1, 3.2, 3.6):
//
// - every type that has offsets takes its own width from its own bound,
//   `fit(k, r)`: a bounded list / map / set / array of variable elements
//   narrows its offset table;
// - a record's explicit cap bounds the positions beneath it (the position
//   cap): a list, map, set, array or tuple there has the width class of
//   min(its own bound, the cap); a record keeps its own width; the walk
//   stops at a record and at a nested value;
// - bit types have a bound; a tuple has at most 255 elements.
//
// The widths are those of cpp/tests/annotation_tree_tests.cpp (`cap_test`)
// and cpp/tests/pssz_inferred_bound_tests.cpp. Every case runs on the typed
// layer and on the schema-driven codec over the schema as it travels.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  ListView, PsszError, PsszSchema, RecordView, TupleView, argumentTuple, array, bitlist, bitvector, boundedList,
  boundedUtf8, box, field, generateSchema, list, map, nested, optional, pair, rawString, record, set, toHex, tuple, u32,
  u8, utf8, variant, type PsszType, type TypeDef,
} from '../index.js';

const fromHex = (hex: string) => Uint8Array.from(Buffer.from(hex.replace(/ /g, ''), 'hex'));

/** The type as a reader that fetched the schema has it. */
function fetched(def: TypeDef<any, any>): PsszType {
  return PsszSchema.decode(generateSchema({ Root: def }).encode()).type('Root');
}

/** One value: the bytes, on the typed and the schema-driven codec, and back. */
function agree<T>(def: TypeDef<T, any>, value: T, wire: string, text = true): void {
  const hex = wire.replace(/ /g, '');
  assert.equal(toHex(def.encode(value)), hex);
  assert.deepEqual(def.validate(fromHex(hex)), { ok: true, hasUnknown: false });
  assert.deepEqual(def.decode(fromHex(hex), { rejectUnknown: true }), value);
  const other = fetched(def);
  assert.equal(other.width, def.width);
  assert.equal(other.maxEncodedSize, def.maxEncodedSize);
  assert.deepEqual(other.validate(fromHex(hex)), { ok: true, hasUnknown: false });
  assert.equal(toHex(other.encode(other.decode(fromHex(hex)))), hex);
  // Text and back derives the same widths.
  if (text) assert.equal(toHex(def.jsonToPssz(def.psszToJson(fromHex(hex)))), hex);
}

function refuses(run: () => unknown, kind: string, message?: RegExp): void {
  try {
    run();
  } catch (e) {
    assert.ok(e instanceof PsszError, String(e));
    assert.equal(e.kind, kind, e.message);
    if (message) assert.match(e.message, message);
    return;
  }
  assert.fail('accepted');
}

const bound = (def: TypeDef<any, any>) => [def.maxEncodedSize, def.width];

test('list width: fit(k, r) — the bound and the width of a bounded list', () => {
  // docs/pssz-spec.md 3.2, the examples.
  assert.deepEqual(bound(boundedList(boundedUtf8(16), 8)), [136n, 1]);
  assert.deepEqual(bound(boundedList(boundedUtf8(16), 15)), [255n, 1]);
  assert.deepEqual(bound(boundedList(boundedUtf8(17), 15)), [285n, 2]);
  assert.deepEqual(bound(boundedList(boundedUtf8(255), 255)), [65535n, 2]);
  assert.deepEqual(bound(boundedList(boundedUtf8(255), 256)), [66304n, 4]);
  assert.deepEqual(bound(record('R', { id: u32, tags: boundedList(boundedUtf8(16), 8) })), [142n, 1]);
  // A count but unbounded elements, and no count: unbounded, 4.
  assert.deepEqual(bound(boundedList(utf8, 8)), [null, 4]);
  assert.deepEqual(bound(list(boundedUtf8(16))), [null, 4]);
  // Fixed-size elements have no table: N * size.
  assert.deepEqual(bound(boundedList(u32, 10)), [40n, 1]);
  assert.deepEqual(bound(array(u32, 3)), [12n, 1]);
  // An array of variable elements has a table of N slots.
  assert.deepEqual(bound(array(boundedUtf8(4), 3)), [15n, 1]);
  assert.deepEqual(bound(array(boundedUtf8(100), 3)), [306n, 2]);
  // Optional: 1 + T. Variant: 1 + the widest alternative.
  assert.deepEqual(bound(tuple(optional(boundedList(boundedUtf8(16), 8)))), [1n + 1n + 137n, 1]);
  // Maps and sets: the list of the entries / elements.
  const entry = 1n + (1n + 16n) + 4n;                       // tuple(string<=16, u32)
  assert.equal(pair(boundedUtf8(16), u32).maxEncodedSize, entry);
  const Counts = record('Counts', { m: field(map(rawString, u32), { max: 4, keys: { utf8: true, max: 16 } }) }, { fixed: true });
  assert.deepEqual(bound(Counts), [1n + 4n * (1n + entry), 1]);
  const Names = record('Names', { s: field(set(rawString), { max: 4, element: { utf8: true, max: 16 } }) }, { fixed: true });
  assert.deepEqual(bound(Names), [1n + 4n * 17n, 1]);
  // Bit types.
  assert.deepEqual(bound(bitvector(12)), [2n, 1]);
  assert.deepEqual(bound(bitlist(12)), [3n, 1]);
  assert.deepEqual(bound(optional(bitlist(12))), [4n, 1]);
  assert.deepEqual(bound(record('BitFlags', { id: u8, flags: bitvector(12), seen: bitlist(12) })), [8n, 1]);
  assert.deepEqual(bound(record('BitFixed', { flags: bitvector(12) }, { fixed: true })), [2n, 1]);
  assert.deepEqual(bound(boundedList(bitvector(12), 10)), [20n, 1]);
  assert.deepEqual(bound(boundedList(bitlist(12), 10)), [40n, 1]);
  assert.deepEqual(bound(tuple(bitvector(12), bitlist(12))), [7n, 1]);
  // A boxed value has no inferred bound.
  assert.deepEqual(bound(record('B', { b: box(u32) })), [null, 4]);
  // A capped child contributes its cap; a type's own cap is not its bound.
  const Capped = record('Capped', { text: utf8 }, { maxDynamicData: 300 });
  assert.deepEqual(bound(Capped), [null, 2]);
  assert.deepEqual(bound(tuple(Capped, u8)), [2n + 2n + 300n + 1n, 2]);
});

test('list width: the bytes of narrow tables', () => {
  // The same two strings behind a 1-, 2- and 4-byte table.
  agree(boundedList(boundedUtf8(16), 15), ['ab', 'c'], '02 04 6162 63');
  agree(boundedList(boundedUtf8(17), 15), ['ab', 'c'], '0400 0600 6162 63');
  agree(boundedList(boundedUtf8(255), 256), ['ab', 'c'], '08000000 0a000000 6162 63');
  agree(boundedList(utf8, 8), ['ab', 'c'], '08000000 0a000000 6162 63');
  agree(list(utf8), ['ab', 'c'], '08000000 0a000000 6162 63');
  agree(boundedList(boundedUtf8(16), 15), [], '');
  // As a member: the record has its own width (259: 2), the list its own (1).
  const W1 = record('W1', { items: field(list(rawString), { max: 15, element: { max: 16 } }) });
  assert.deepEqual(bound(W1), [259n, 2]);
  agree(W1, { items: [Uint8Array.of(0x61, 0x62), Uint8Array.of(0x63)] }, '0200 0200 02 04 6162 63');
  // Nested: rows with 1-byte tables behind a 2-byte table of rows.
  const Grid = boundedList(boundedList(boundedUtf8(16), 8), 4);
  assert.deepEqual(bound(Grid), [8n + 4n * 136n, 2]);
  agree(Grid, [['ab'], [], ['c', 'd']], '0600 0900 0900 01 6162 02 03 63 64');
  // An array of bounded strings has a 1-byte table too.
  agree(array(boundedUtf8(4), 3), ['ab', '', 'c'], '03 05 05 6162 63');
  // A bounded map: a 1-byte table, and each entry a tuple at its own width.
  const Counts = record('Counts', { m: field(map(rawString, u32), { max: 4, keys: { utf8: true, max: 16 } }) }, { fixed: true });
  agree(Counts, { m: new Map([['a', 1], ['b', 2]]) }, '01 02 09 05 05 01000000 61 05 05 02000000 62');
  // A list of bitlists (bit types have no JSON form).
  agree(boundedList(bitlist(12), 10), [{ bytes: Uint8Array.of(5), bitCount: 3 }], '01 0d', false);
});

test('list width: a narrow table is checked like a wide one', () => {
  const L = boundedList(boundedUtf8(16), 15);
  for (const reader of [L.type(), fetched(L)]) {
    // The 4-byte table read as the 1-byte type, and malformed narrow tables.
    for (const bad of ['080000000a000000616263', '00 61', '05 6162', '03 02 01 61', '02 05 6162'])
      assert.equal(reader.validate(fromHex(bad)).ok, false, bad);
    // One element over the count, behind a valid 1-byte table; one byte over an element.
    const sixteen = toHex(Uint8Array.from({ length: 16 }, () => 16));
    const refused = reader.validate(fromHex(sixteen));
    assert.ok(!refused.ok && /sequence bound exceeded/.test(refused.error.message));
    const long = reader.validate(fromHex('01' + '61'.repeat(17)));
    assert.ok(!long.ok && /byte count bound exceeded/.test(long.error.message));
  }
  refuses(() => L.encode(Array(16).fill('')), 'value', /sequence bound exceeded/);
  // The largest value fits one-byte offsets: 15 + 15 * 16 = 255.
  const full = Array(15).fill('x'.repeat(16));
  assert.equal(L.encode(full).length, 255);
  assert.deepEqual(L.decode(L.encode(full)), full);
});

// cpp/tests/annotation_tree_tests.cpp `cap_test::Capped<N>`, without the
// child records (below): everything beneath the record is as wide as its cap.
const cappedFields = {
  list: list(rawString),
  rows: list(list(rawString)),
  map: map(rawString, list(u32)),
  tuple: tuple(rawString, list(rawString)),
  opt: optional(list(rawString)),
  var: variant({ n: u32, names: list(rawString) }),
  arr: array(rawString, 2),
};
const raw = (text: string) => Uint8Array.from(Buffer.from(text, 'utf8'));
const cappedValue = {
  list: [raw('ab'), raw('c')],
  rows: [[raw('x')], [], [raw('y'), raw('zz')]],
  map: new Map([[raw('k'), [1, 2]], [raw('l'), []]]),
  tuple: [raw('t'), [raw('u'), raw('v')]] as [Uint8Array, Uint8Array[]],
  opt: [raw('o')],
  var: { type: 'names' as const, value: [raw('p'), raw('q')] },
  arr: [raw('m'), raw('n')],
};

test('position cap: every list, map, tuple and array beneath a capped record takes the cap\'s width', () => {
  const Free = record('Capped', cappedFields);
  for (const [cap, w] of [[255, 1], [256, 2], [65535, 2], [65536, 4]] as const) {
    const Capped = record('Capped', cappedFields, { maxDynamicData: cap });
    assert.equal(Capped.width, w);
    // The cap changes no bound and no schema: the members are the plain types.
    assert.equal(Capped.maxEncodedSize, null);
    assert.equal(Capped.childWiderThanCap, false);
    // With no record beneath it, the frame is the explicit profile's of the
    // same width: every header, slot and table is as wide as the cap allows.
    const bytes = Capped.encode(cappedValue);
    assert.equal(toHex(bytes), toHex(Free.type({ width: w }).encode(cappedValue)), `cap ${cap}`);
    assert.deepEqual(Capped.decode(bytes, { rejectUnknown: true }), cappedValue);
    for (const type of [Capped.type(), fetched(Capped)]) {
      assert.deepEqual(type.validate(bytes), { ok: true, hasUnknown: false });
      assert.equal(toHex(type.encode(type.decode(bytes))), toHex(bytes));
      assert.equal(toHex(type.jsonToPssz(type.psszToJson(bytes))), toHex(bytes));
      // A view finds every position with the widths of the cap.
      const view = type.view(bytes) as RecordView;
      const rows = view.get('rows') as ListView;
      assert.equal(rows.length, 3);
      assert.deepEqual(((rows.at(2) as ListView).at(1)), raw('zz'));
      const entry = (view.get('map') as ListView).at(0) as TupleView;
      assert.deepEqual(entry.at(0), raw('k'));
      assert.deepEqual((entry.at(1) as ListView).toValue(), [1, 2]);
      assert.deepEqual(((view.get('tuple') as TupleView).at(1) as ListView).at(1), raw('v'));
      assert.deepEqual((view.get('opt') as ListView).at(0), raw('o'));
      assert.deepEqual(((view.get('var') as { value: ListView }).value).at(1), raw('q'));
      assert.deepEqual((view.get('arr') as ListView).at(1), raw('n'));
      assert.deepEqual((view.get('rows') as ListView).toValue(), cappedValue.rows);
      assert.deepEqual(view.toValue(), cappedValue);
      // The frame of the uncapped type (4-byte tables) is not this type's.
      if (w !== 4) assert.equal(type.validate(Free.encode(cappedValue)).ok, false);
    }
  }
  // No cap: the member types alone (4).
  assert.equal(toHex(Free.encode(cappedValue)), toHex(Free.type({ width: 4 }).encode(cappedValue)));
  // An explicit profile ignores the cap: one width for the whole frame.
  const Capped = record('Capped', cappedFields, { maxDynamicData: 255 });
  for (const w of [1, 2, 4] as const)
    assert.equal(toHex(Capped.type({ width: w }).encode(cappedValue)), toHex(Free.type({ width: w }).encode(cappedValue)));
});

test('position cap: pinned bytes (docs/pssz-spec.md 3.6)', () => {
  const fields = { token: u32, memo: rawString, tags: list(rawString), rows: list(list(rawString)) };
  const value = { token: 1, memo: raw('hi'), tags: [raw('ab'), raw('c')], rows: [[raw('x')]] };
  agree(record('Transfer', fields, { maxDynamicData: 255 }), value,
    '07 01000000 07 09 0e  6869  02 04 6162 63  01 01 78');
  agree(record('Transfer', fields, { maxDynamicData: 256 }), value,
    '0a00 01000000 0a00 0c00 1300  6869  0400 0600 6162 63  0200 0200 78');
  agree(record('Transfer', fields), value,
    '10000000 01000000 10000000 12000000 1d000000  6869  08000000 0a000000 6162 63  04000000 04000000 78');
});

test('position cap: a tighter bound wins, a looser one is clamped', () => {
  const items = [raw('ab'), raw('c')];
  // Its own bound (136) under a cap of 60000: a 1-byte table behind 2-byte slots.
  const Tight = record('Tight', { list: field(list(rawString), { max: 8, element: { max: 16 } }) }, { maxDynamicData: 60000 });
  assert.equal(Tight.width, 2);
  agree(Tight, { list: items }, '0200 0200 02 04 6162 63');
  // A bound of 1000 * (4 + 1000) under a cap of 255: clamped to one byte.
  const Loose = record('Loose', { list: field(list(rawString), { max: 1000, element: { max: 1000 } }) }, { maxDynamicData: 255 });
  agree(Loose, { list: items }, '01 01 02 04 6162 63');
  // The looser bound is legal and still limits what the cap does not.
  refuses(() => Loose.encode({ list: [new Uint8Array(1001)] }), 'value', /byte count bound exceeded/);
  refuses(() => Loose.encode({ list: [new Uint8Array(300)] }), 'value', /maxDynamicData/);
  // Without the cap that list is 4 wide (its bound is over 65535).
  const Wide = record('Wide', { list: field(list(rawString), { max: 1000, element: { max: 1000 } }) });
  agree(Wide, { list: items }, '04000000 04000000 08000000 0a000000 6162 63');
});

test('position cap: a record keeps its own width, and the cap stops there', () => {
  const Child = record('Child', { names: list(rawString) });
  const Parent = record('Parent', { child: Child, kids: list(Child), names: list(rawString) }, { maxDynamicData: 255 });
  assert.equal(Child.width, 4);
  assert.equal(Parent.width, 1);
  const child = { names: [raw('a')] };
  // Child is the same bytes wherever it appears: 4-byte header, slot and table.
  const childHex = '04000000 04000000 04000000 61';
  agree(Child, child, childHex);
  // The table of the list of records, and the plain list beside it, are 1 wide.
  agree(Parent, { child, kids: [child], names: [raw('a')] }, `03 03 10 1e  ${childHex}  01 ${childHex}  01 61`);
  // The query: a child that cannot use its width.
  assert.equal(Parent.childWiderThanCap, true);
  assert.equal(fetched(Parent).childWiderThanCap, true);
  assert.equal(Child.childWiderThanCap, false);                                            // no cap
  assert.equal(record('P', { child: Child }, { maxDynamicData: 65536 }).childWiderThanCap, false);
  assert.equal(record('P', { kids: optional(list(tuple(u8, Child))) }, { maxDynamicData: 255 }).childWiderThanCap, true);
  const Narrow = record('Narrow', { names: list(rawString) }, { maxDynamicData: 100 });
  assert.equal(record('P', { child: Narrow }, { maxDynamicData: 255 }).childWiderThanCap, false);
  assert.equal(record('P', { child: Narrow }, { maxDynamicData: 255 }).type({ width: 1 }).childWiderThanCap, false);
  // Only the nearest record's cap counts: beneath a capped child, the child's.
  agree(record('P', { child: Narrow }, { maxDynamicData: 70000 }), { child: { names: [raw('a')] } },
    '04000000 04000000  01 01 01 61');
  // A cap does not reach out of its record: the outer list is unbounded.
  const Outer = record('Outer', { inner: record('In', { names: list(rawString) }, { maxDynamicData: 255 }), names: list(rawString) });
  agree(Outer, { inner: { names: [raw('a')] }, names: [raw('a')] },
    '08000000 08000000 0c000000  01 01 01 61  04000000 61');
});

test('position cap: a nested value is a root of its own', () => {
  const Inner = list(rawString);
  const Holder = record('Holder', { n: nested(Inner), direct: list(rawString) }, { maxDynamicData: 255 });
  const value = { n: [raw('a')], direct: [raw('a')] };
  // The nested bytes are the root encoding of the inner type (a 4-byte
  // table); the list beside it is under the cap (1 byte).
  agree(Holder, value, `02 02 07  ${toHex(Inner.encode([raw('a')]))}  01 61`);
  assert.equal(toHex(Inner.encode([raw('a')])), '0400000061');
  const view = Holder.view(Holder.encode(value));
  assert.deepEqual((view.get('n') as ListView).at(0), raw('a'));
  assert.deepEqual((view.get('direct') as ListView).at(0), raw('a'));
});

test('position cap: a method argument tuple with a cap is a record', () => {
  const parameters = { names: list(rawString), pairs: map(u8, list(rawString)) };
  const value = { names: [raw('ab'), raw('c')], pairs: new Map([[7, [raw('z')]]]) };
  const Free = argumentTuple(parameters);
  const Capped = argumentTuple(parameters, { maxDynamicData: 255 });
  assert.deepEqual([Free.width, Capped.width], [4, 1]);
  agree(Capped, value, '02 02 07  02 04 6162 63  01 02 07 02 01 7a');
  assert.equal(toHex(Free.encode(value)), toHex(Free.type({ width: 4 }).encode(value)));
  assert.equal(toHex(Capped.encode(value)), toHex(Free.type({ width: 1 }).encode(value)));
  // A plain tuple is not a record: it has no cap and passes one through.
  const Under = record('Under', { t: tuple(list(rawString)) }, { maxDynamicData: 255 });
  agree(Under, { t: [[raw('a')]] }, '01 01  01 01  01 61');
});

test('list width: a member length bound that stays an attribute bounds the list', () => {
  // `exact` / `min` are kept as the member's lengthBound attribute, and the
  // list as a plain List: its count, and so its table width, comes from there.
  const Exact = record('Exact', { pair: field(list(rawString), { lengthBound: { exact: 2 }, element: { max: 16 } }) }, { fixed: true });
  const member = generateSchema({ Exact }).ir.types.find((m) => m.name === 'Exact')!.type.value;
  assert.ok(member.type === 'Struct' || member.type === 'Object');
  assert.equal(member.value.members[0].attributes.find((a) => a.name === 'lengthBound')?.value, 'exact=2');
  assert.deepEqual(bound(Exact), [1n + 2n * 17n, 1]);
  agree(Exact, { pair: [raw('ab'), raw('c')] }, '01 02 04 6162 63');
  refuses(() => Exact.encode({ pair: [raw('ab')] }), 'value', /member length bound violated/);
  // Through an optional.
  const Maybe = record('Maybe', { pair: field(optional(list(rawString)), { lengthBound: { exact: 2 }, element: { max: 16 } }) });
  agree(Maybe, { pair: [raw('ab'), raw('c')] }, '01 01 01 02 04 6162 63');
  agree(Maybe, { pair: null }, '00');
  const view = Maybe.view(fromHex('01 01 01 02 04 6162 63'));
  assert.deepEqual((view.get('pair') as ListView).at(1), raw('c'));
  // `min` alone gives no count: unbounded.
  const Min = record('Min', { items: field(list(rawString), { lengthBound: { min: 1 }, element: { max: 16 } }) }, { fixed: true });
  assert.deepEqual(bound(Min), [null, 4]);
  agree(Min, { items: [raw('ab')] }, '04000000 04000000 6162');
});

test('a tuple has at most 255 elements', () => {
  const members = (n: number) => Array.from({ length: n }, () => ({ Int: { bits: 8, isSigned: false } }));
  // Schema-driven: a schema holding a Tuple of more than 255 members is invalid.
  const ok = new PsszSchema({ T: { Tuple: members(255) } }).type('T');
  assert.equal(ok.encode(Array(255).fill(1)).length, 2 + 255);                         // header (2 bytes: 257) + 255
  assert.equal(ok.width, 2);
  refuses(() => new PsszSchema({ T: { Tuple: members(256) } }).type('T'), 'schema', /a tuple has more than 255 elements/);
  refuses(() => new PsszSchema({ R: { Struct: { t: { List: { Tuple: members(256) } } } } }).type('R'), 'schema', /255/);
  // Typed: refused when the type is built.
  assert.equal(tuple(...Array(255).fill(u8)).width, 2);
  refuses(() => tuple(...Array(256).fill(u8)), 'schema', /at most 255 elements/);
  const parameters = (n: number) => Object.fromEntries(Array.from({ length: n }, (_, i) => [`p${i}`, u8]));
  assert.equal(argumentTuple(parameters(255)).width, 2);
  refuses(() => argumentTuple(parameters(256)), 'schema', /at most 255 parameters/);
  // A record has no such limit in the format.
  const big = record('Big', parameters(300), { fixed: true });
  assert.equal(big.encode(Object.fromEntries(Object.keys(parameters(300)).map((k) => [k, 1]))).length, 300);
});
