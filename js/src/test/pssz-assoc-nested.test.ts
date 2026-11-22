// Pairs, maps and sets (docs/pssz-spec.md §5.9), nested values (§5.8),
// transparent types (§5.10) and type invariants (§18): the rules the shared
// fixtures pin, stated one by one for the schema-driven codec and for the
// typed layer, together with what is specific to JavaScript (Map / Set
// values, iterables, the −0 key, the typed declarations and their types).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  ListView, PsszError, PsszNegativeZero, PsszSchema, RecordView, TupleView, bitlist, bytes, custom, enumeration, field,
  generateSchema, list, map, nested, optional, pair, rawString, record, set, transparent, tuple, u16, u32, u64, u8, utf8,
  variant, type Infer, type InferView, type PsszCustomHandler, type PsszErrorKind,
} from '../index.js';

const fromHex = (hex: string) => Uint8Array.from(Buffer.from(hex, 'hex'));
const toHex = (b: Uint8Array) => Buffer.from(b).toString('hex');

function rejects(run: () => unknown, kind: PsszErrorKind, message: RegExp | string): PsszError {
  try {
    run();
  } catch (e) {
    assert.ok(e instanceof PsszError, `threw ${String(e)}`);
    assert.equal(e.kind, kind, e.message);
    if (typeof message === 'string') assert.equal(e.message, message);
    else assert.match(e.message, message);
    return e;
  }
  return assert.fail('accepted');
}

const U8 = { Int: { bits: 8, isSigned: false } };
const U16 = { Int: { bits: 16, isSigned: false } };
const U32 = { Int: { bits: 32, isSigned: false } };
const U64 = { Int: { bits: 64, isSigned: false } };
const F64 = { Float: { exp: 11, mantissa: 53 } };
const UTF8 = { Custom: { type: { List: U8 }, id: 'utf8' } };
const RAW = { Custom: { type: { List: U8 }, id: 'string' } };
const mapOf = (k: unknown, v: unknown) => ({ Custom: { type: { List: { Tuple: [k, v] } }, id: 'map' } });
const setOf = (k: unknown) => ({ Custom: { type: { List: k }, id: 'set' } });

// ── Maps and sets ───────────────────────────────────────────────────────────

test('map / set: the bytes of the sorted list, whatever order and container the value has', () => {
  const schema = new PsszSchema({
    Counts: mapOf(UTF8, U32) as never,
    Entries: { List: { Tuple: [UTF8, U32] } } as never,
    Ids: setOf(U16) as never,
    IdList: { List: U16 },
  });
  const counts = schema.type<Map<string, number>>('Counts');
  // docs/pssz-spec.md §5.9.3.
  const frame =
    '08000000' + '15000000' +
    '08000000' + '08000000' + '01000000' + '61' +
    '08000000' + '08000000' + '02000000' + '62';
  assert.equal(frame.length, 2 * 34);
  const value = new Map([['b', 2], ['a', 1]]);
  assert.equal(toHex(counts.encode(value)), frame);
  assert.equal(toHex(schema.type('Entries').encode([['a', 1], ['b', 2]])), frame);
  // Any iterable of entries, in any order: an array, a generator.
  assert.equal(toHex(counts.encode([['b', 2], ['a', 1]] as never)), frame);
  assert.equal(toHex(counts.encode((function* () { yield ['b', 2]; yield ['a', 1]; })() as never)), frame);
  // A Map in canonical order comes back.
  const decoded = counts.decode(fromHex(frame));
  assert.ok(decoded instanceof Map);
  assert.deepEqual([...decoded], [['a', 1], ['b', 2]]);
  assert.equal(decoded.get('b'), 2);
  assert.equal(counts.width, 4);

  // The frame with the two entries exchanged is a list, not a map.
  const swapped = toHex(schema.type('Entries').encode([['b', 2], ['a', 1]]));
  assert.notEqual(swapped, frame);
  assert.deepEqual(schema.type('Entries').validate(fromHex(swapped)), { ok: true, hasUnknown: false });
  rejects(() => counts.decode(fromHex(swapped)), 'bytes', 'pssz IR: map entries are not in strictly ascending key order');
  const repeated = toHex(schema.type('Entries').encode([['a', 1], ['a', 2]]));
  rejects(() => counts.decode(fromHex(repeated)), 'bytes', /not in strictly ascending key order/);
  rejects(() => counts.encode([['a', 1], ['a', 2]] as never), 'value', 'pssz IR: map has duplicate keys');
  rejects(() => counts.encode([['a']] as never), 'value', /map entry is a \[key, value\] pair/);
  rejects(() => counts.encode(5 as never), 'value', /sequence value required/);

  // Set(u16) {513, 2}: 2 then 513 by key, though the wire bytes of 513 sort
  // below those of 2.
  const ids = schema.type<Set<number>>('Ids');
  assert.equal(toHex(ids.encode(new Set([513, 2]))), '02000102');
  assert.equal(toHex(ids.encode([513, 2] as never)), '02000102');
  const back = ids.decode(fromHex('02000102'));
  assert.ok(back instanceof Set);
  assert.deepEqual([...back], [2, 513]);
  rejects(() => ids.decode(fromHex('01020200')), 'bytes', 'pssz IR: set elements are not in strictly ascending order');
  rejects(() => ids.decode(fromHex('02000200')), 'bytes', /not in strictly ascending order/);
  rejects(() => ids.encode([2, 2] as never), 'value', 'pssz IR: set has duplicate elements');
  assert.deepEqual(schema.type('IdList').decode(fromHex('01020200')), [513, 2]);
});

test('map / set: text is an object for a string key, pairs otherwise; any order in, canonical out', () => {
  const schema = new PsszSchema({
    ByName: mapOf(UTF8, U32) as never,
    ByRaw: mapOf(RAW, U8) as never,
    ById: mapOf(U64, UTF8) as never,
    Names: setOf(UTF8) as never,
    Bytes: setOf(U8) as never,
  });
  for (const format of ['json', 'ijson'] as const) {
    const byName = schema.type<Map<string, number>>('ByName');
    assert.equal(byName.valueToJson(new Map([['pear', 3], ['apple', 1]]), format), '{"apple":1,"pear":3}');
    assert.deepEqual([...byName.jsonToValue('{"pear":3,"apple":1}', format)], [['apple', 1], ['pear', 3]]);
    rejects(() => byName.jsonToValue('{"a":1,"a":2}', format), 'json', 'json: duplicate map key');
    rejects(() => byName.jsonToValue('[["a",1]]', format), 'json', /expected an object/);
    rejects(() => byName.valueToJson([['a', 1], ['a', 2]] as never, format), 'value', 'json: map has duplicate keys');
    // A raw string key is a string by type as well.
    assert.equal(schema.type('ByRaw').psszToJson(schema.type('ByRaw').jsonToPssz('{"b":2,"a":1}', format), format), '{"a":1,"b":2}');
    // An integer key is not a string by type, also where it is written as one.
    const byId = schema.type<Map<bigint, string>>('ById');
    const big = 18446744073709551615n;
    assert.equal(byId.valueToJson(new Map([[big, 'max'], [5n, 'five']]), format),
      format === 'json' ? '[[5,"five"],[18446744073709551615,"max"]]' : '[[5,"five"],["18446744073709551615","max"]]');
    rejects(() => byId.jsonToValue('{"5":"five"}', format), 'json', /expected '\['/);
    rejects(() => byId.jsonToValue('[[5,"a"],[5,"b"]]', format), 'json', 'json: duplicate map key');
    rejects(() => byId.jsonToValue('[[5,"a",1]]', format), 'json', /too many elements/);
    const names = schema.type<Set<string>>('Names');
    assert.equal(names.valueToJson(new Set(['b', 'a']), format), '["a","b"]');
    rejects(() => names.jsonToValue('["a","a"]', format), 'json', 'json: duplicate set element');
    rejects(() => names.valueToJson(['a', 'a'] as never, format), 'value', 'json: set has duplicate elements');
    // A set of bytes is an array of numbers in both formats, not a byte string.
    assert.equal(schema.type('Bytes').valueToJson(new Set([7, 1]), format), '[1,7]');
  }
  // Two keys are the same when their key bytes are: "5" and 5 under ijson.
  rejects(() => schema.type('ById').jsonToValue('[["5","a"],[5,"b"]]', 'ijson'), 'json', 'json: duplicate map key');
});

test('map / set: the float −0 key keeps its sign bit', () => {
  const schema = new PsszSchema({ Zeros: setOf(F64) as never, ByFloat: mapOf(F64, U8) as never });
  const zeros = schema.type<Set<number | PsszNegativeZero>>('Zeros');
  const negative = '0000000000000080';
  const decoded = zeros.decode(fromHex(negative));
  // A JavaScript Set stores +0 for -0: the element is held apart.
  assert.deepEqual([...decoded], [PsszNegativeZero.value]);
  assert.ok(Object.is(Number([...decoded][0]), -0));
  assert.equal(toHex(zeros.encode(decoded)), negative);
  assert.equal(toHex(zeros.encode(new Set([0]))), '0000000000000000');
  assert.equal(toHex(zeros.encode([-0] as never)), negative);
  assert.equal(zeros.valueToJson(decoded, 'json'), '[-0]');
  assert.deepEqual(zeros.jsonToValue('[-0]', 'json'), decoded);
  // -0 and +0 are one key: they cannot both be in a set.
  rejects(() => zeros.encode([0, -0] as never), 'value', /duplicate elements/);
  rejects(() => zeros.decode(fromHex('0000000000000000' + negative)), 'bytes', /ascending/);
  const byFloat = schema.type<Map<number | PsszNegativeZero, number>>('ByFloat');
  const frame = byFloat.encode([[-0, 7]] as never);
  assert.deepEqual([...byFloat.decode(frame)], [[PsszNegativeZero.value, 7]]);
  assert.equal(toHex(byFloat.encode(byFloat.decode(frame))), toHex(frame));
  // The key encoding has one zero.
  assert.equal(toHex(zeros.encodeKey(decoded)), toHex(zeros.encodeKey(new Set([0]))));
});

test('map / set: keys, key order of nested maps, and what is not a key type', () => {
  const schema = new PsszSchema({
    Groups: mapOf(UTF8, setOf(U8)) as never,
    Ids: setOf(U8) as never,
  });
  const groups = schema.type<Map<string, Set<number>>>('Groups');
  const value = new Map([['b', new Set([2, 1])], ['a', new Set<number>()]]);
  // 01 + key(k) + key(v) per entry, then 00; a set of octets has the octet framing.
  assert.equal(toHex(groups.encodeKey(value)), '01' + '610000' + '0000' + '01' + '620000' + '01020000' + '00');
  const back = groups.decodeKey(groups.encodeKey(value));
  assert.deepEqual([...back].map(([k, v]) => [k, [...v]]), [['a', []], ['b', [1, 2]]]);
  rejects(() => groups.decodeKey(fromHex('01' + '620000' + '0000' + '01' + '610000' + '0000' + '00')), 'bytes',
    'key: map entries are not in strictly ascending key order');
  rejects(() => schema.type('Ids').decodeKey(fromHex('02010000')), 'bytes', 'key: set elements are not in strictly ascending order');
  rejects(() => schema.type('Ids').decodeKey(fromHex('01010000')), 'bytes', /ascending/);
  rejects(() => groups.encodeKey([['a', []], ['a', []]] as never), 'value', 'key: map has duplicate keys');

  // Malformed schemas (the JSON fixture's schema_rejects hold the full list).
  const bad = (type: unknown, what: RegExp) =>
    rejects(() => new PsszSchema({ T: type as never }).type('T'), 'schema', what);
  bad({ Custom: { type: { List: U8 }, id: 'map' } }, /a map must sit directly on a list of two-member tuples/);
  bad({ Custom: { type: { Array: { type: U8, len: 2 } }, id: 'set' } }, /a set must sit directly on a list/);
  bad(setOf({ Custom: { type: { BoundedList: { type: { Int: { bits: 1, isSigned: false } }, maxCount: 8 } }, id: 'bitlist' } }),
    /has no key encoding/);
  bad(mapOf({ Custom: { type: U8, id: 'box' } }, U8), /has no key encoding/);
  // "map" and "set" are part of the binary form: they take no handler.
  rejects(() => new PsszSchema({ T: U8 }, { handlers: { map: {} } }), 'schema', /part of the binary form/);
});

// ── Nested values ───────────────────────────────────────────────────────────

const NESTED_SCHEMA = {
  Args: { Object: { to: UTF8, amount: U64, memo: { Option: UTF8 } } },
  Args2: { Object: { to: UTF8, amount: U64, memo: { Option: UTF8 }, fee: { Option: U32 } } },
  Action: { Object: { method: U32, args: { Nested: 'Args' } } },
  Action2: { Object: { method: U32, args: { Nested: 'Args2' } } },
  Raw: { Object: { method: U32, args: { Custom: { type: { List: U8 }, id: 'hex' } } } },
};

test('nested value: the bytes of a byte list holding the inner encoding, validated as the inner type', () => {
  const schema = new PsszSchema(NESTED_SCHEMA as never);
  const args = { to: 'bob', amount: 5n, memo: 'rent' };
  const inner = schema.type('Args').encode(args);
  const action = schema.type('Action');
  const frame = action.encode({ method: 9, args });
  // The typed member and the bytes member are byte-identical.
  assert.equal(toHex(frame), toHex(schema.type('Raw').encode({ method: 9, args: inner })));
  assert.deepEqual(action.decode(frame), { method: 9, args });
  assert.deepEqual(schema.type('Raw').decode(frame), { method: 9, args: inner });
  // The inner bytes are always the automatic profile, whatever the frame's.
  for (const width of [1, 2, 4] as const) {
    const narrow = schema.type('Action', { width });
    const bytes = narrow.encode({ method: 9, args });
    assert.equal(toHex(bytes.subarray(bytes.length - inner.length)), toHex(inner));
    assert.deepEqual(narrow.decode(bytes), { method: 9, args });
    assert.equal(toHex(schema.type('Raw', { width }).encode({ method: 9, args: inner })), toHex(bytes));
  }
  // Bytes that are not the inner type are a rejection of the enclosing value.
  const garbage = schema.type('Raw').encode({ method: 9, args: fromHex('010203') });
  assert.equal(action.validate(garbage).ok, false);
  assert.equal(action.validate(schema.type('Raw').encode({ method: 9, args: new Uint8Array(0) })).ok, false);
  // Text is the inner value's own text, not hex.
  assert.equal(action.psszToJson(frame), '{"method":9,"args":{"to":"bob","amount":5,"memo":"rent"}}');
  assert.equal(toHex(action.jsonToPssz('{"args":{"memo":"rent","amount":5,"to":"bob"},"method":9}')), toHex(frame));
  rejects(() => action.jsonToPssz('{"method":9,"args":"0102"}'), 'json', /./);
});

test('nested value: unknown inner fields, and the opaque option that keeps the bytes', () => {
  const schema = new PsszSchema(NESTED_SCHEMA as never);
  const args = { to: 'bob', amount: 5n, memo: 'rent' };
  const newer = schema.type('Action2').encode({ method: 9, args: { ...args, fee: 7 } });
  const action = schema.type('Action');
  // A compatible read accepts and reports the fields a newer writer appended.
  assert.deepEqual(action.validate(newer), { ok: true, hasUnknown: true });
  rejects(() => action.decode(newer, { rejectUnknown: true }), 'bytes', /unknown fields/);
  // The value is the inner value: re-encoding writes its canonical encoding.
  assert.equal(toHex(action.encode(action.decode(newer))), toHex(action.encode({ method: 9, args })));
  // Opaque: the value is the bytes, read and written as they are.
  for (const opaque of [schema.type('Action', { nestedBytes: 'opaque' }), schema.withHandlers({}, { nestedBytes: 'opaque' }).type('Action')]) {
    assert.deepEqual(opaque.validate(newer), { ok: true, hasUnknown: false });
    const held = opaque.decode(newer) as { method: number; args: Uint8Array };
    assert.ok(held.args instanceof Uint8Array);
    assert.equal(toHex(held.args), toHex(schema.type('Args2').encode({ ...args, fee: 7 })));
    assert.equal(toHex(opaque.encode(held)), toHex(newer));
    // Any bytes are valid, and the holder validates where it reads them.
    const garbage = schema.type('Raw').encode({ method: 9, args: fromHex('010203') });
    assert.deepEqual(opaque.validate(garbage), { ok: true, hasUnknown: false });
    assert.equal(schema.type('Args').validate((opaque.decode(garbage) as { args: Uint8Array }).args).ok, false);
    rejects(() => opaque.encode({ method: 9, args }), 'value', /bytes value required for an opaque nested value/);
    // The text is the inner value's all the same; the writer validates the bytes.
    assert.equal(opaque.valueToJson(held), '{"method":9,"args":{"to":"bob","amount":5,"memo":"rent"}}');
    rejects(() => opaque.psszToJson(garbage), 'value', 'json: nested bytes are not a valid encoding of their type');
    const read = opaque.jsonToValue('{"method":9,"args":{"to":"bob","amount":5}}') as { args: Uint8Array };
    assert.equal(toHex(read.args), toHex(schema.type('Args').encode({ to: 'bob', amount: 5n, memo: null })));
  }
  rejects(() => schema.type('Action', { nestedBytes: 'other' as never }), 'schema', /nestedBytes/);
});

test('nested value: a member length bound counts the bytes of the encoding; keys order by the inner bytes', () => {
  const schema = new PsszSchema({
    types: [
      { name: 'Note', type: { Object: { members: [
        { name: 'id', type: U8 },
        { name: 'note', type: { Nested: RAW }, attributes: [{ name: 'lengthBound', value: 'max=3' }] },
        { name: 'maybe', type: { Option: { Nested: RAW } }, attributes: [{ name: 'lengthBound', value: 'max=3' }] },
      ] } } },
      { name: 'Numbers', type: setOf({ Nested: U32 }) as never },
      { name: 'ByNumber', type: mapOf({ Nested: U32 }, U8) as never },
    ],
    ir_version: 1,
  });
  const note = schema.type('Note');
  const ok = note.encode({ id: 1, note: 'abc', maybe: 'abc' });
  assert.deepEqual(note.validate(ok), { ok: true, hasUnknown: false });
  // The bound narrows the record's offset width, as on a byte list.
  assert.equal(note.width, 1);
  rejects(() => note.encode({ id: 1, note: 'abcd', maybe: null }), 'value', /member length bound violated/);
  rejects(() => note.encode({ id: 1, note: 'abc', maybe: 'abcd' }), 'value', /member length bound violated/);
  rejects(() => note.jsonToPssz('{"id":1,"note":"abcd"}'), 'value', /member length bound violated/);

  // A nested key is ordered by its inner bytes as stored: 256 (00 01 00 00)
  // sorts before 1 (01 00 00 00).
  const numbers = schema.type<Set<number>>('Numbers');
  assert.deepEqual([...numbers.decode(numbers.encode(new Set([1, 256])))], [256, 1]);
  assert.equal(numbers.valueToJson(new Set([1, 256])), '[256,1]');
  assert.equal(toHex(numbers.encodeKey(new Set([1, 256]))), '01' + '000101000100010000' + '01' + '01000100010001' + '0000' + '00');
  assert.deepEqual([...numbers.decodeKey(numbers.encodeKey(new Set([1, 256])))], [256, 1]);
  const byNumber = schema.type<Map<number, number>>('ByNumber');
  assert.equal(byNumber.valueToJson(new Map([[1, 7], [256, 8]])), '[[256,8],[1,7]]');
  // The numeric order is not the order.
  const numeric = new PsszSchema({ L: { List: { Custom: { type: { List: U8 }, id: 'hex' } } } }).type('L')
    .encode([fromHex('01000000'), fromHex('00010000')]);
  rejects(() => numbers.decode(numeric), 'bytes', /ascending/);
  // Inner bytes that are not a u32.
  const short = new PsszSchema({ L: { List: { Custom: { type: { List: U8 }, id: 'hex' } } } }).type('L').encode([fromHex('0100')]);
  rejects(() => numbers.decode(short), 'bytes', /integer span mismatch/);
});

// ── Invariants and handlers ─────────────────────────────────────────────────

const even: PsszCustomHandler = { check: (v) => ((v as number) % 2 === 0 ? undefined : 'Even: odd value') };
const ordered: PsszCustomHandler = {
  check(v) {
    const r = v as { lo: number; hi: number };
    if (r.lo > r.hi) throw new Error('Range: lo is greater than hi');
  },
};
const tagText: PsszCustomHandler = {
  toText: (v) => `#${v as number}`,
  fromText(text) {
    if (!/^#(0|[1-9][0-9]{0,2})$/.test(text)) throw new Error('not a tag');
    return Number(text.slice(1));
  },
  check: (v) => ((v as number) !== 13 ? null : 'Tag: unlucky'),
};
const INVARIANT_SCHEMA = {
  Even: { Custom: { type: U8, id: 'Even' } },
  Range: { Custom: { type: { Object: { lo: { Custom: { type: U8, id: 'Even' } }, hi: U8 } }, id: 'Range' } },
  Tag: { Custom: { type: U8, id: 'Tag' } },
  Ranges: { List: 'Range' },
  ByEven: mapOf('Even', 'Range'),
  ByTag: mapOf('Tag', U8),
  Evens: setOf('Even'),
  Held: { Nested: 'Range' },
  // A transparent string with an invariant: a named Custom over the string.
  Label: { Custom: { type: UTF8, id: 'Label' } },
  ByLabel: mapOf('Label', U8),
};

test('invariant: a handler check runs on every entry point, binary and text, innermost first', () => {
  const plain = new PsszSchema(INVARIANT_SCHEMA as never);
  const handlers = { Even: even, Range: ordered, Tag: tagText, Label: { check: (v: unknown) => (v === '' ? 'Label: empty' : undefined) } };
  const checked = plain.withHandlers(handlers);
  const strict = plain.withHandlers({}, { requireHandlers: true });

  for (const width of ['auto', 1, 2, 4] as const) {
    const range = checked.type('Range', { width });
    const wire = plain.type('Range', { width });
    const good = wire.encode({ lo: 2, hi: 4 });
    assert.deepEqual(range.validate(good), { ok: true, hasUnknown: false });
    assert.deepEqual(range.decode(good), { lo: 2, hi: 4 });
    assert.equal(toHex(range.encode({ lo: 2, hi: 4 })), toHex(good));
    // The message is the invariant's own, and the error says so.
    const reversed = wire.encode({ lo: 4, hi: 2 });
    const refused = rejects(() => range.decode(reversed), 'bytes', 'Range: lo is greater than hi');
    assert.equal(refused.invariant, true);
    const validation = range.validate(reversed);
    assert.ok(!validation.ok && validation.error.message === 'Range: lo is greater than hi' && validation.error.invariant);
    assert.equal(rejects(() => range.encode({ lo: 4, hi: 2 }), 'value', 'Range: lo is greater than hi').invariant, true);
    // Innermost first: the member's invariant is reported before its record's.
    rejects(() => range.decode(wire.encode({ lo: 5, hi: 2 })), 'bytes', 'Even: odd value');
    rejects(() => range.encode({ lo: 5, hi: 2 }), 'value', 'Even: odd value');
    // No handler: the value is its wire type. A strict registry refuses the type.
    assert.deepEqual(wire.validate(reversed), { ok: true, hasUnknown: false });
    const required = rejects(() => strict.type('Range', { width }).decode(good), 'bytes', 'pssz IR: no handler registered for a custom type');
    assert.equal(required.invariant, false);
  }
  // A structural error is reported as such: the check never sees a malformed value.
  let seen = 0;
  const counting = plain.withHandlers({ Even: { check: () => { ++seen; return undefined; } } });
  rejects(() => counting.type('Even').decode(new Uint8Array(0)), 'bytes', /integer span mismatch/);
  assert.equal(seen, 0);
  counting.type('Even').decode(fromHex('02'));
  assert.equal(seen, 1);

  // Text: the same message, after the text was read; a value has no text.
  const range = checked.type('Range');
  for (const format of ['json', 'ijson'] as const) {
    assert.equal(rejects(() => range.jsonToValue('{"lo":4,"hi":2}', format), 'json', 'Range: lo is greater than hi').invariant, true);
    rejects(() => range.jsonToPssz('{"lo":5,"hi":2}', format), 'json', 'Even: odd value');
    rejects(() => range.valueToJson({ lo: 4, hi: 2 }, format), 'value', 'Range: lo is greater than hi');
    rejects(() => range.psszToJson(plain.type('Range').encode({ lo: 4, hi: 2 }), format), 'bytes', 'Range: lo is greater than hi');
    assert.equal(range.valueToJson({ lo: 2, hi: 4 }, format), '{"lo":2,"hi":4}');
    // A text form and an invariant: the text is read first, then the value checked.
    const tag = checked.type('Tag');
    assert.equal(tag.valueToJson(7, format), '"#7"');
    assert.equal(tag.jsonToValue('"#7"', format), 7);
    rejects(() => tag.jsonToValue('"7"', format), 'json', 'json: text is not a value of the custom type');
    rejects(() => tag.jsonToValue('"#13"', format), 'json', 'Tag: unlucky');
    rejects(() => tag.valueToJson(13, format), 'value', 'Tag: unlucky');
    rejects(() => tag.jsonToValue('7', format), 'json', /expected a string \(custom text form\)/);
  }
  // In lists, in nested bytes, in keys.
  const ranges = plain.type('Ranges').encode([{ lo: 2, hi: 4 }, { lo: 4, hi: 2 }]);
  rejects(() => checked.type('Ranges').decode(ranges), 'bytes', 'Range: lo is greater than hi');
  const held = plain.type('Held').encode({ lo: 4, hi: 2 });
  rejects(() => checked.type('Held').decode(held), 'bytes', 'Range: lo is greater than hi');
  assert.deepEqual(checked.type('Held', { nestedBytes: 'opaque' }).validate(held), { ok: true, hasUnknown: false });
  rejects(() => checked.type('Even').encodeKey(3), 'value', 'Even: odd value');
  rejects(() => checked.type('Even').decodeKey(fromHex('03')), 'bytes', 'Even: odd value');
  assert.equal(plain.type('Even').decodeKey(fromHex('03')), 3);
  assert.equal(checked.type('Even').validateKey(fromHex('03')).ok, false);
  rejects(() => checked.type('Range').encodeKeyPrefix([3]), 'value', 'Even: odd value');
  assert.equal(toHex(checked.type('Range').encodeKeyPrefix([2])), '02');
});

test('invariant: in maps and sets — the key before its value, the invariant before the order, member names', () => {
  const plain = new PsszSchema(INVARIANT_SCHEMA as never);
  const label: PsszCustomHandler = { check: (v) => (v === '' ? 'Label: empty' : undefined) };
  const checked = plain.withHandlers({ Even: even, Range: ordered, Tag: tagText, Label: label });
  const byEven = checked.type('ByEven');
  const wire = plain.type('ByEven');
  // The key's violation is reported before its mapped value's.
  rejects(() => byEven.decode(wire.encode([[3, { lo: 4, hi: 2 }]])), 'bytes', 'Even: odd value');
  rejects(() => byEven.decode(wire.encode([[2, { lo: 4, hi: 2 }]])), 'bytes', 'Range: lo is greater than hi');
  rejects(() => byEven.encode([[3, { lo: 2, hi: 4 }]]), 'value', 'Even: odd value');
  // The order is defined for every key, and computing it runs no check: an
  // entry whose key violates its invariant and is in its place is refused
  // for the invariant.
  assert.deepEqual([...(wire.decode(wire.encode([[4, { lo: 0, hi: 0 }], [3, { lo: 0, hi: 0 }]])) as Map<number, unknown>).keys()], [3, 4]);
  rejects(() => byEven.decode(wire.encode([[3, { lo: 0, hi: 0 }], [4, { lo: 0, hi: 0 }]])), 'bytes', 'Even: odd value');
  rejects(() => checked.type('Evens').decode(plain.type('Evens').encode([2, 3])), 'bytes', 'Even: odd value');

  for (const format of ['json', 'ijson'] as const) {
    // A key with a text form keys an object; without the handler the map
    // has the pairs form of the wire type.
    const byTag = checked.type('ByTag');
    assert.equal(byTag.valueToJson([[9, 1], [7, 2]], format), '{"#7":2,"#9":1}');
    assert.equal(plain.type('ByTag').valueToJson([[9, 1], [7, 2]], format), '[[7,2],[9,1]]');
    assert.deepEqual([...(byTag.jsonToValue('{"#9":1,"#7":2}', format) as Map<number, number>)], [[7, 2], [9, 1]]);
    rejects(() => byTag.jsonToValue('{"#7":1,"#7":2}', format), 'json', 'json: duplicate map key');
    rejects(() => byTag.jsonToValue('{"7":1}', format), 'json', 'json: text is not a value of the custom type');
    // An invariant runs on a key read from, or written as, a member name.
    assert.equal(rejects(() => byTag.jsonToValue('{"#13":1}', format), 'json', 'Tag: unlucky').invariant, true);
    rejects(() => byTag.valueToJson([[13, 1]], format), 'value', 'Tag: unlucky');
    // A strict registry that lacks the key's handler refuses the map.
    rejects(() => plain.withHandlers({}, { requireHandlers: true }).type('ByTag').valueToJson([], format), 'value', /no handler registered/);
    rejects(() => plain.withHandlers({}, { requireHandlers: true }).type('ByTag').jsonToValue('[]', format), 'json', /no handler registered/);
    // A handler that only checks does not decide the form: the string under
    // it does, with or without the handler.
    for (const schema of [plain, checked]) {
      assert.equal(schema.type('ByLabel').valueToJson([['b', 2], ['a', 1]], format), '{"a":1,"b":2}');
      assert.deepEqual([...(schema.type('ByLabel').jsonToValue('{"b":2,"a":1}', format) as Map<string, number>)], [['a', 1], ['b', 2]]);
    }
    assert.deepEqual([...(plain.type('ByLabel').jsonToValue('{"":1}', format) as Map<string, number>)], [['', 1]]);
    rejects(() => checked.type('ByLabel').jsonToValue('{"":1}', format), 'json', 'Label: empty');
    rejects(() => checked.type('ByLabel').valueToJson([['', 1]], format), 'value', 'Label: empty');
  }
});

// ── The typed layer ─────────────────────────────────────────────────────────

type Same<A, B> = (<T>() => T extends A ? 1 : 2) extends (<T>() => T extends B ? 1 : 2) ? true : false;
const same = <A, B>(_ok: Same<A, B>): void => undefined;

test('typed layer: pair, map, set, nested and transparent — values, views and their types', () => {
  const Args = record('Args', { to: utf8, amount: u64, memo: optional(utf8) });
  const Id = transparent('Id', u64);
  const Book = record('Book', {
    owner: Id,
    head: pair(u16, utf8),
    counts: map(utf8, u32),
    ids: set(u16),
    byPoint: map(tuple(u8, u8), list(utf8)),
    call: nested(Args),
    later: nested(Args, { deferred: true }),
    limited: field(map(u8, u8), { lengthBound: { max: 2 } }),
  });
  type Book = Infer<typeof Book>;
  same<Book['owner'], bigint>(true);
  same<Book['head'], [number, string]>(true);
  same<Book['counts'], Map<string, number>>(true);
  same<Book['ids'], Set<number>>(true);
  same<Book['byPoint'], Map<[number, number], string[]>>(true);
  same<Book['call'], { to: string; amount: bigint; memo: string | null }>(true);
  same<Book['later'], Uint8Array>(true);
  // What a view returns for each: the member's value or view.
  type Fields = InferView<typeof Book> extends RecordView<Book, infer F> ? F : never;
  same<Fields['owner'], bigint>(true);
  same<Fields['later'], Uint8Array>(true);
  same<Fields['ids'], ListView<Set<number>, number>>(true);
  same<Fields['counts'], ListView<Map<string, number>, TupleView<[string, number], [string, number]>>>(true);

  const args = { to: 'bob', amount: 5n, memo: null };
  const value: Book = {
    owner: 7n,
    head: [513, 'x'],
    counts: new Map([['b', 2], ['a', 1]]),
    ids: new Set([513, 2]),
    byPoint: new Map<[number, number], string[]>([[[1, 2], ['p']], [[0, 9], []]]),
    call: args,
    later: Args.encode(args),
    limited: new Map([[2, 2], [1, 1]]),
  };
  const frame = Book.encode(value);
  const back = Book.decode(frame);
  assert.equal(back.owner, 7n);
  assert.deepEqual(back.head, [513, 'x']);
  assert.deepEqual([...back.counts], [['a', 1], ['b', 2]]);
  assert.deepEqual([...back.ids], [2, 513]);
  assert.deepEqual([...back.byPoint], [[[0, 9], []], [[1, 2], ['p']]]);
  assert.deepEqual(back.call, args);
  assert.equal(toHex(back.later), toHex(Args.encode(args)));
  assert.equal(toHex(Book.encode(back)), toHex(frame));
  // The same bytes as the lists, the tuple and the byte lists it is on the wire.
  const Lists = record('Lists', {
    owner: u64,
    head: tuple(u16, utf8),
    counts: list(tuple(utf8, u32)),
    ids: list(u16),
    byPoint: list(tuple(tuple(u8, u8), list(utf8))),
    call: bytes,
    later: bytes,
    limited: field(list(tuple(u8, u8)), { lengthBound: { max: 2 } }),
  });
  assert.equal(toHex(frame), toHex(Lists.encode({
    owner: 7n, head: [513, 'x'], counts: [['a', 1], ['b', 2]], ids: [2, 513], byPoint: [[[0, 9], []], [[1, 2], ['p']]],
    call: Args.encode(args), later: Args.encode(args), limited: [[1, 1], [2, 2]],
  })));
  assert.equal(Book.width, Lists.width);
  // A member length bound counts entries.
  rejects(() => Book.encode({ ...value, limited: new Map([[1, 1], [2, 2], [3, 3]]) }), 'value', /bound/);
  // A deferred nested value is not looked at by the enclosing reader.
  const odd = Book.encode({ ...value, later: fromHex('010203') });
  assert.deepEqual(Book.validate(odd), { ok: true, hasUnknown: false });
  assert.equal(Args.validate(Book.decode(odd).later).ok, false);
  const bad = Lists.encode({
    owner: 7n, head: [513, 'x'], counts: [], ids: [], byPoint: [], call: fromHex('010203'), later: new Uint8Array(0), limited: [],
  });
  assert.equal(Book.validate(bad).ok, false);
  // `nestedBytes: 'opaque'` makes every nested value its bytes.
  const opaque = Book.type({ nestedBytes: 'opaque' });
  assert.deepEqual(opaque.validate(bad), { ok: true, hasUnknown: false });

  // Views: a pair is a tuple; a map is the list of its entries and a set the
  // list of its elements, in wire order; a nested value is its inner view; a
  // transparent type is its member's.
  const view = Book.view(frame);
  assert.equal(view.get('owner'), 7n);
  const head = view.get('head');
  assert.ok(head instanceof TupleView);
  assert.equal(head.at(0), 513);
  assert.equal(head.at(1), 'x');
  const counts = view.get('counts');
  assert.ok(counts instanceof ListView);
  assert.equal(counts.length, 2);
  assert.equal(counts.at(1).at(0), 'b');
  assert.equal(counts.at(1).at(1), 2);
  assert.deepEqual([...counts.toValue()], [['a', 1], ['b', 2]]);
  const ids = view.get('ids');
  assert.deepEqual([...ids], [2, 513]);
  assert.ok(ids.toValue() instanceof Set);
  const call = view.get('call');
  assert.ok(call instanceof RecordView);
  assert.equal(call.get('to'), 'bob');
  assert.equal(call.get('amount'), 5n);
  assert.equal(toHex(call.bytes), toHex(Args.encode(args)));
  assert.equal(toHex(view.get('later')), toHex(Args.encode(args)));

  // Text and keys.
  assert.equal(Book.valueToJson(value, 'json'),
    '{"owner":7,"head":[513,"x"],"counts":{"a":1,"b":2},"ids":[2,513],"byPoint":[[[0,9],[]],[[1,2],["p"]]],' +
    '"call":{"to":"bob","amount":5,"memo":null},"later":{"to":"bob","amount":5,"memo":null},"limited":[[1,1],[2,2]]}');
  assert.equal(toHex(Book.jsonToPssz(Book.valueToJson(value, 'ijson'), 'ijson')), toHex(frame));
  assert.equal(toHex(map(utf8, u32).encodeKey(value.counts)), toHex(list(tuple(utf8, u32)).encodeKey([['a', 1], ['b', 2]])));
  assert.equal(toHex(set(u16).encodeKey(value.ids)), toHex(list(u16).encodeKey([2, 513])));
  assert.equal(toHex(nested(u32).encodeKey(256)), toHex(bytes.encodeKey(fromHex('00010000'))));
  assert.equal(toHex(Id.encodeKey(7n)), toHex(u64.encodeKey(7n)));
  // Raw string keys are bytes: found by iterating, not by identity.
  const byRaw = map(rawString, u8);
  const raw = byRaw.decode(byRaw.encode(new Map([[fromHex('62'), 2], [fromHex('61'), 1]])));
  assert.deepEqual([...raw.keys()].map(toHex), ['61', '62']);
  assert.equal(byRaw.valueToJson(raw), '{"a":1,"b":2}');
});

test('typed layer: the Schema IR of pair, map, set, nested, transparent and custom types', () => {
  const Args = record('Args', { to: utf8 });
  const Id = transparent('Id', u64);
  const Key = custom('Key', transparent('Key', bytes), { check: (b) => (b.length ? undefined : 'Key: empty') });
  const Level = custom('Level', enumeration(u8, { low: 0, high: 1 }), { check: (v) => (v <= 1 ? undefined : 'Level: not listed') });
  const Either = custom('Either', variant({ 0: u64, 1: utf8 }));
  const schema = generateSchema({
    Holder: record('Holder', {
      id: Id, key: Key, level: Level, either: Either,
      a: tuple(u16, utf8), b: pair(u16, utf8),
      m: map(utf8, u32), um: map(utf8, u32, { unordered: true }),
      s: set(u16), us: set(u16, { unordered: true }),
      n: nested(Args), dn: nested(Args, { deferred: true }),
      bm: field(map(u8, u8), { lengthBound: { max: 2 } }),
      bn: field(nested(Args), { lengthBound: { max: 64 } }),
    }),
  });
  const spell = (name: string): string => {
    const show = (t: { value: { type: string; value: any } }): string => {
      const v = t.value;
      switch (v.type) {
        case 'Type': return v.value.type;
        case 'Int': return `${v.value.isSigned ? 'i' : 'u'}${v.value.bits}`;
        case 'List': case 'Nested': return `${v.type}<${show(v.value.type)}>`;
        case 'BoundedList': return `BoundedList<${show(v.value.type)},${v.value.maxCount}>`;
        case 'Tuple': return `Tuple[${v.value.members.map(show).join(',')}]`;
        case 'Custom': return `Custom{${show(v.value.type)},${v.value.id}}`;
        case 'Enum': return `Enum{${show(v.value.type)}}`;
        case 'Variant': return `Variant{${v.value.members.map((m: any) => show(m.type)).join(',')}}`;
        default: return `${v.type}{${v.value.members.map((m: any) => `${m.name}:${show(m.type)}${m.attributes.map((a: any) => `[${a.name}=${a.value}]`).join('')}`).join(',')}}`;
      }
    };
    return show(schema.ir.types.find((m) => m.name === name)!.type as never);
  };
  const names = schema.ir.types.map((m) => m.name);
  // A transparent type is a named alias of its member's type; with a handler
  // it is the Custom over the member's type, under its own name.
  assert.match(spell('Id'), /^@\d+$/);
  assert.match(spell('Key'), /^Custom\{@\d+,Key\}$/);
  const holder = spell('Holder');
  assert.match(holder, /^Object\{id:Id,key:Key,level:@\d+,either:@\d+,/);
  // A custom enum / variant is interned with its wire type inline.
  const member = (name: string) => new RegExp(`[{,]${name}:([^,\\[}]+)`).exec(holder)![1];
  assert.equal(spell(member('level')), 'Custom{Enum{u8},Level}');
  assert.match(spell(member('either')), /^Custom\{Variant\{@\d+,@\d+\},Either\}$/);
  // A pair is the tuple's node under its own identity; so is an unordered
  // container, and a deferred nested value.
  for (const [x, y] of [['a', 'b'], ['m', 'um'], ['s', 'us'], ['n', 'dn']]) {
    assert.notEqual(member(x), member(y), `${x} / ${y}`);
    assert.equal(spell(member(x)), spell(member(y)), `${x} / ${y}`);
  }
  assert.match(spell(member('m')), /^Custom\{List<@\d+>,map\}$/);
  assert.match(spell(member('s')), /^Custom\{List<@\d+>,set\}$/);
  assert.equal(spell(member('n')), 'Nested<Args>');
  // The entry of a map is the tuple (key, value), interned once for both maps.
  const entry = /List<(@\d+)>/.exec(spell(member('m')))![1];
  assert.match(spell(entry), /^Tuple\[@\d+,@\d+\]$/);
  assert.equal(/List<(@\d+)>/.exec(spell(member('um')))![1], entry);
  // A `max` bound on a map member is the bounded list in the member; a
  // bound on a nested member is always the attribute.
  assert.match(holder, /bm:Custom\{BoundedList<@\d+,2>,map\}/);
  assert.match(holder, /bn:@\d+\[lengthBound=max=64\]/);
  assert.ok(names.includes('Args') && names.includes('Id') && names.includes('Key'));
  // The schema carries the handlers and the deferred policy of the declarations.
  const type = schema.type('Holder');
  const value = {
    id: 1n, key: fromHex('30'), level: 1, either: { type: '0', value: 1n },
    a: [1, 'a'], b: [1, 'a'], m: new Map(), um: new Map(), s: new Set(), us: new Set(),
    n: { to: 'x' }, dn: fromHex('ff'), bm: new Map(), bn: { to: 'x' },
  };
  const frame = type.encode(value);
  assert.deepEqual(type.validate(frame), { ok: true, hasUnknown: false });
  rejects(() => type.encode({ ...value, key: new Uint8Array(0) }), 'value', 'Key: empty');
  rejects(() => type.encode({ ...value, level: 2 }), 'value', 'Level: not listed');
  // Fetched as bytes, the schema has neither: the node is its wire type.
  const fetched = PsszSchema.decode(schema.encode()).type('Holder');
  rejects(() => fetched.decode(frame), 'bytes', /./);
  assert.equal(toHex(fetched.encode({ ...value, dn: { to: 'y' }, key: new Uint8Array(0), level: 2 })).length > 0, true);

  // Declarations that are refused.
  rejects(() => transparent('', u64), 'schema', /needs a name/);
  rejects(() => transparent('Maybe', optional(u8)), 'schema', /not an optional/);
  rejects(() => generateSchema({ a: transparent('X', u8), b: transparent('X', u16) }), 'schema', /two different types are named "X"/);
  rejects(() => custom('map', list(u8)), 'schema', /built-in id/);
  rejects(() => custom('K', u8, { check: 1 as never }), 'schema', /check .* must be a function/);
  rejects(() => custom('D', nested(Args, { deferred: true })).schema, 'schema', /deferred nested value cannot be the wire type/);
  rejects(() => record('R', { m: field(Id, { lengthBound: { max: 1 } }) }).schema, 'schema', /lengthBound does not apply/);
  rejects(() => map(bitlist(8), u8).encode(new Map()), 'schema', /has no key encoding/);
});
