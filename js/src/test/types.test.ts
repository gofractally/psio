// The typed layer: declarations written by hand (as an application would),
// their static types, the Schema IR they generate against the C++ fixtures,
// record evolution, bounds, caps, widths, custom handlers and views.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  ListView, PsszError, PsszSchema, RecordView, TupleView, TypeDef, VariantView, alt, array, bitlist, bool,
  boundedBytes, boundedList, boundedUtf8, box, bytes, custom, duration, enumeration, f32, f64, field, fixedBytes,
  generateSchema, i128, i16, i32, i64, i8, keyPrefixUpperBound, lazy, list, nameToNumber, numberToName, optional,
  rawString, record, timePoint, tuple, u128, u16, u256, u32, u64, u8, utf8, variant,
  type CustomHandler, type Infer, type IrAnyType, type IrSchema, type Some,
} from '../index.js';
import { accountHandler, versionHandler } from './support/custom-handlers.js';

const fixture = (name: string) =>
  JSON.parse(readFileSync(new URL(`../../../conformance/${name}`, import.meta.url), 'utf8'));
const fromHex = (hex: string) => Uint8Array.from(Buffer.from(hex, 'hex'));
const toHex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');

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

// ── The types of cpp/tests/json_conformance_types.hpp, written by hand ──────

const Fixed = record('Fixed', { x: i32, y: i32, on: bool }, { fixed: true });
const Color = enumeration(u8, { red: 0, green: 1, blue: 7 });
const Status = enumeration(i32, { pending: -1, active: 2, closed: 7 });
const Bounds = record('Bounds', {
  name: boundedUtf8(8),
  tags: field(list(u32), { lengthBound: { max: 3 } }),
  memo: field(rawString, { lengthBound: { max: 4 } }),
  blob: field(bytes, { lengthBound: { max: 2 } }),
  note: field(optional(rawString), { lengthBound: { max: 3 } }),
});
const Capped = record('Capped', { id: u16, memo: rawString, at: optional(Fixed), path: list(Fixed) }, { maxDynamicData: 300 });
const Shapes = record('Shapes', {
  color: Color, corners: array(Fixed, 2), scale: f32, delta: i16, label: optional(utf8), small: optional(u8),
  capped: Capped, bounds: optional(Bounds),
});
const Wide = record('Wide', {
  amount: u128, delta: i128, hash: u256, maybe: optional(u128), list: list(i128), status: Status,
});
const Version = custom('Version', record('Version', { major: u16, minor: u16 }), versionHandler as CustomHandler<{ major: number; minor: number }>);
const Account = custom('AccountName', u64, accountHandler as CustomHandler<bigint>);
const Transfer = record('Transfer', {
  from: Account, to: Account, amount: u64, payer: optional(Account), notify: list(Account), since: optional(Version),
});
const Overrides = record('Overrides', {
  a: field(u64, { jsonInt: 'string' }),
  b: field(u64, { ijsonInt: 'number' }),
  c: field(u32, { jsonInt: 'string', ijsonInt: 'string' }),
  d: field(list(u64), { ijsonInt: 'number' }),
  e: field(optional(i64), { jsonInt: 'string' }),
  f: i64,
});
const Inner = record('Inner', { id: u64, label: utf8, flag: bool });
const Choice = variant({
  0: alt(u32, { ijsonName: 'uint32' }), 1: alt(u64, { ijsonName: 'uint64' }), 2: alt(rawString, { ijsonName: 'string' }),
  3: alt(Inner, { ijsonName: 'Inner' }), 4: alt(bytes, { ijsonName: 'uint8[]' }),
});
const Holder = record('Holder', { choice: Choice, choices: list(Choice) });
// cpp/tests/time_key_conformance_types.hpp
const Event = record('Event', {
  name: rawString, at: timePoint('millis'), precise: optional(timePoint('nanos')), history: list(timePoint('seconds')),
  elapsed: duration('nanos'), timeout: duration({ num: 60, den: 1 }, i32),
});

// A type with every `@N` reference replaced by what it names: the IR up to
// the numbering, which depends on what else a schema holds. References to
// records stay references; `records` collects their names.
function expanded(schema: IrSchema, type: IrAnyType, records: Set<string>, top = false): unknown {
  const byName = new Map(schema.types.map((m) => [m.name, m.type]));
  const walk = (t: IrAnyType, own = false): unknown => {
    const v = t.value;
    switch (v.type) {
      case 'Type': {
        const name = v.value.type;
        const target = byName.get(name);
        assert.ok(target, `no type ${name}`);
        const kind = target.value.type;
        if (!own && !name.startsWith('@') && (kind === 'Object' || kind === 'Custom')) {
          records.add(name);
          return { Type: name };
        }
        return walk(target, own && kind === 'Type');
      }
      case 'Int':
      case 'Float':
        return v;
      case 'List':
      case 'Option':
        return { [v.type]: walk(v.value.type) };
      case 'Array':
        return { Array: walk(v.value.type), len: String(v.value.len) };
      case 'BoundedList':
        return { BoundedList: walk(v.value.type), maxCount: String(v.value.maxCount) };
      case 'Tuple':
        return { Tuple: v.value.members.map((m) => walk(m)) };
      case 'Variant':
      case 'Object':
        return {
          [v.type]: v.value.members.map((m) => [m.name, walk(m.type), m.attributes]),
          attributes: v.value.attributes,
        };
      case 'Custom':
        return { Custom: v.value.id, type: walk(v.value.type) };
      case 'Enum':
        return { Enum: walk(v.value.type), members: v.value.members.map((m) => [m.name, String(m.value)]) };
      default:
        return assert.fail(`unexpected ${v.type}`);
    }
  };
  return walk(type, top);
}
// `root` and every record it reaches, each expanded.
function closure(schema: IrSchema, root: string): Map<string, unknown> {
  const out = new Map<string, unknown>();
  const todo = [root];
  while (todo.length) {
    const name = todo.pop()!;
    if (out.has(name)) continue;
    const found = new Set<string>();
    out.set(name, expanded(schema, { value: { type: 'Type', value: { type: name } } }, found, true));
    todo.push(...found);
  }
  return out;
}

test('typed layer: hand-written declarations generate the IR C++ records for the same types', () => {
  const json = PsszSchema.decode(fromHex(fixture('json-vectors.json').schema_ir)).ir;
  const timeKey = PsszSchema.decode(fromHex(fixture('time-key-vectors.json').schema_ir)).ir;
  const cases: [string, TypeDef<any, any>, IrSchema][] = [
    ['Fixed', Fixed, json], ['Bounds', Bounds, json], ['Capped', Capped, json], ['Shapes', Shapes, json],
    ['Wide', Wide, json], ['Transfer', Transfer, json], ['Overrides', Overrides, json], ['Holder', Holder, json],
    ['Color', Color, json], ['Status', Status, json], ['Choice', Choice, json], ['Event', Event, timeKey],
  ];
  for (const [name, def, cpp] of cases) {
    const mine = generateSchema({ [name]: def }).ir;
    assert.deepEqual(closure(mine, name), closure(cpp, name), name);
  }
});

test('typed layer: the JSON fixture replayed through the hand-written declarations', () => {
  const fx = fixture('json-vectors.json');
  const defs: { [id: string]: TypeDef<any, any> } = {
    Fixed, Bounds, Capped, Shapes, Wide, Transfer, Overrides, Holder, Color, Status, Choice, Inner, Version, Account,
  };
  const count = { value: 0, accept: 0, reject: 0 };
  for (const row of fx.vectors as { name: string; type: string; kind: string; json: string; ijson: string; pssz?: string; text?: string; text_hex?: string; format: 'json' | 'ijson'; canonical: string }[]) {
    const def = defs[row.type];
    if (!def) continue;
    if (row.kind === 'value') {
      const bytes = def.jsonToPssz(row.json, 'json');
      if (row.pssz !== undefined) assert.equal(toHex(bytes), row.pssz, row.name);
      assert.equal(def.psszToJson(bytes, 'json'), row.json, row.name);
      assert.equal(def.valueToJson(def.decode(bytes), 'ijson'), row.ijson, row.name);
      ++count.value;
    } else {
      const input = row.text !== undefined ? row.text : fromHex(row.text_hex!);
      if (row.kind === 'accept') {
        assert.equal(def.psszToJson(def.jsonToPssz(input, row.format), row.format), row.canonical, row.name);
        ++count.accept;
      } else {
        try {
          def.jsonToPssz(input, row.format);
          assert.fail(`${row.name}: accepted`);
        } catch (e) {
          assert.ok(e instanceof PsszError && (e.kind === 'json' || e.kind === 'value'), `${row.name}: ${String(e)}`);
        }
        ++count.reject;
      }
    }
  }
  console.log('json-vectors.json (hand-written typed declarations):', JSON.stringify(count));
  assert.ok(count.value > 40 && count.accept > 30 && count.reject > 100);
});

// ── Static types ────────────────────────────────────────────────────────────

test('typed layer: the decoded value is statically typed', () => {
  sameType<Infer<typeof Transfer>, {
    from: bigint; to: bigint; amount: bigint; payer: bigint | null; notify: bigint[];
    since: { major: number; minor: number } | null;
  }>(true);
  sameType<Infer<typeof Choice>,
    | { type: '0'; value: number } | { type: '1'; value: bigint } | { type: '2'; value: Uint8Array }
    | { type: '3'; value: { id: bigint; label: string; flag: boolean } } | { type: '4'; value: Uint8Array }>(true);
  const Pair = tuple(u32, utf8, optional(i64));
  sameType<Infer<typeof Pair>, [number, string, bigint | null]>(true);
  const Nested = optional(optional(u8));
  sameType<Infer<typeof Nested>, Some<number | null> | null>(true);
  sameType<Infer<typeof Event>['at'], bigint>(true);
  sameType<Infer<typeof Color>, number>(true);

  const value: Infer<typeof Transfer> = { from: 1n, to: 2n, amount: 5n, payer: null, notify: [3n], since: { major: 1, minor: 2 } };
  const decoded = Transfer.decode(Transfer.encode(value));
  assert.deepEqual(decoded, value);
  const amount: bigint = decoded.amount;
  assert.equal(amount, 5n);
  // @ts-expect-error a string is not a u64
  assert.throws(() => Transfer.encode({ ...value, amount: 'five' }), PsszError);
  // @ts-expect-error a field is missing
  assert.throws(() => Transfer.encode({ from: 1n }), PsszError);

  assert.deepEqual(Pair.decode(Pair.encode([7, 'x', null])), [7, 'x', null]);
  assert.deepEqual(Nested.decode(Nested.encode({ some: null })), { some: null });
  assert.equal(Nested.decode(Nested.encode(null)), null);
  const choice = Choice.decode(Choice.encode({ type: '3', value: { id: 9n, label: 'l', flag: true } }));
  if (choice.type === '3') assert.equal(choice.value.label, 'l');
  else assert.fail();
});

// ── Evolution, bounds, caps, widths ─────────────────────────────────────────

test('typed layer: a record grows by trailing optionals', () => {
  const V1 = record('Person', { name: utf8, age: u32 });
  const V2 = record('Person', { name: utf8, age: u32, email: optional(utf8), tags: optional(list(utf8)) });
  const old = V1.encode({ name: 'Alice', age: 30 });
  // A newer reader sees null for what the older writer did not store.
  assert.deepEqual(V2.decode(old), { name: 'Alice', age: 30, email: null, tags: null });
  // Trailing absent optionals are not written: the bytes are the old ones.
  assert.equal(toHex(V2.encode({ name: 'Alice', age: 30, email: null, tags: null })), toHex(old));
  // An older reader skips appended fields and says so.
  const grown = V2.encode({ name: 'Alice', age: 30, email: 'a@b', tags: null });
  assert.deepEqual(V1.validate(grown), { ok: true, hasUnknown: true });
  assert.deepEqual(V1.decode(grown), { name: 'Alice', age: 30 });
  throwsKind(() => V1.decode(grown, { rejectUnknown: true }), 'bytes');
  // Views: the same, without decoding the record.
  assert.equal(V2.view(old).get('email'), null);
  assert.equal(V2.view(grown).get('email'), 'a@b');
  assert.equal(V1.view(grown).hasUnknown, true);
  assert.equal(V1.view(grown).get('age'), 30);
  // A fixed record has no header and cannot grow.
  assert.equal(Fixed.encode({ x: 1, y: -1, on: true }).length, 9);
  // Both definitions in one schema: a record is its name.
  throwsKind(() => generateSchema({ a: V1, b: V2 }), 'schema', /two different records are named "Person"/);
});

test('typed layer: bounds and caps are enforced and narrow the offset width', () => {
  const Small = record('Small', { tag: boundedUtf8(8), ids: boundedList(u16, 4), key: fixedBytes(4), blob: boundedBytes(2) });
  assert.equal(Small.width, 1);
  assert.equal(record('Open', { tag: utf8 }).width, 4);
  assert.equal(Capped.width, 2);
  assert.equal(record('Tiny', { memo: utf8 }, { maxDynamicData: 200 }).width, 1);
  const ok = { tag: 'abcdefgh', ids: [1, 2, 3, 4], key: new Uint8Array(4), blob: new Uint8Array(2) };
  assert.deepEqual(Small.decode(Small.encode(ok)), ok);
  throwsKind(() => Small.encode({ ...ok, tag: 'abcdefghi' }), 'value');
  throwsKind(() => Small.encode({ ...ok, ids: [1, 2, 3, 4, 5] }), 'value');
  throwsKind(() => Small.encode({ ...ok, key: new Uint8Array(3) }), 'value');
  throwsKind(() => Small.encode({ ...ok, blob: new Uint8Array(3) }), 'value');
  // A member length bound, through an optional; min / exact as an attribute.
  const b = { name: 'n', tags: [1, 2, 3], memo: new Uint8Array(4), blob: new Uint8Array(2), note: new Uint8Array(3) };
  assert.deepEqual(Bounds.decode(Bounds.encode(b)), b);
  throwsKind(() => Bounds.encode({ ...b, note: new Uint8Array(4) }), 'value');
  throwsKind(() => Bounds.encode({ ...b, tags: [1, 2, 3, 4] }), 'value');
  const Exact = record('Exact', { code: field(utf8, { lengthBound: { exact: 3 } }), list: field(list(u8), { lengthBound: { min: 1, max: 2 } }) });
  assert.deepEqual(Exact.decode(Exact.encode({ code: 'abc', list: [1] })), { code: 'abc', list: [1] });
  throwsKind(() => Exact.encode({ code: 'ab', list: [1] }), 'value');
  throwsKind(() => Exact.encode({ code: 'abc', list: [] }), 'value');
  // Bytes over the cap are refused on both sides.
  const Tiny = record('Tiny', { memo: utf8 }, { maxDynamicData: 8 });
  throwsKind(() => Tiny.encode({ memo: 'x'.repeat(20) }), 'value', /maxDynamicData/);
  const wide = record('Tiny', { memo: utf8 }).type({ width: 1 }).encode({ memo: 'x'.repeat(20) });
  assert.equal(Tiny.validate(wide).ok, false);
  // An explicit width profile.
  const P = record('P', { a: u32, s: utf8 });
  assert.equal(P.type({ width: 2 }).width, 2);
  assert.equal(toHex(P.type({ width: 2 }).encode({ a: 1, s: 'hi' })), '06000100000006006869');
  assert.deepEqual(P.type({ width: 2 }).decode(fromHex('06000100000006006869')), { a: 1, s: 'hi' });
});

test('typed layer: malformed declarations are schema errors', () => {
  throwsKind(() => record('', { a: u8 }), 'schema');
  throwsKind(() => record('@0', { a: u8 }), 'schema');
  throwsKind(() => record('R', { 5: u8 }), 'schema', /JavaScript would reorder/);
  throwsKind(() => record('R', { a: 7 as never }), 'schema');
  throwsKind(() => variant({}), 'schema');
  throwsKind(() => variant({ 1: u8, x: u8 }), 'schema', /JavaScript would reorder/);
  throwsKind(() => enumeration(u8, { big: 256 }), 'schema');
  throwsKind(() => enumeration(u128 as never, { a: 0 }), 'schema');
  throwsKind(() => custom('utf8', u8), 'schema', /built-in id/);
  throwsKind(() => custom('time-point-seconds', i64), 'schema');
  throwsKind(() => timePoint('weeks' as never), 'schema');
  throwsKind(() => timePoint('seconds', f64), 'schema');
  throwsKind(() => boundedUtf8(-1), 'schema');
  throwsKind(() => array(u8, 1.5), 'schema');
  throwsKind(() => record('R', { a: field(u32, { lengthBound: { max: 3 } }) }).schema, 'schema', /does not apply/);
  throwsKind(() => record('R', { a: field(utf8, { lengthBound: { max: -1 } }) }), 'schema');
  throwsKind(() => record('R', { a: field(u32, { jsonInt: 'text' as never }) }), 'schema');
  const other = { toText: String, fromText: BigInt };
  throwsKind(() => record('R', { a: Account, b: custom('AccountName', u64, other) }).schema, 'schema', /two different handlers/);
  throwsKind(() => generateSchema({ a: 5 as never }), 'schema');
  throwsKind(() => generateSchema({ Fixed: u8, x: Fixed }), 'schema', /used twice/);
  const loop: TypeDef<unknown> = lazy(() => loop);
  throwsKind(() => loop.schema, 'schema');
  // A definition that cannot be compiled reports through validate, too.
  const bad = record('R', { a: field(u32, { lengthBound: { max: 3 } }) });
  const result = bad.validate(new Uint8Array(4));
  assert.ok(!result.ok && result.error.kind === 'schema');
});

// ── generateSchema ──────────────────────────────────────────────────────────

test('generateSchema: names, interning and the schema as it travels', () => {
  const schema = generateSchema({ Transfer, amounts: list(u64), Fixed });
  // Records under their own names, everything else interned once as @N in
  // first-visit order; an alias for each name that is not the type's own.
  assert.deepEqual(schema.ir.types.map((m) => m.name), [
    'Transfer', '@1', '@2', '@3', '@4', '@5', 'Version', '@7', '@8', 'amounts', 'Fixed', '@10', '@11',
  ]);
  assert.equal(schema.ir.ir_version, 1);
  // It travels as PSSZ and reads back to the same IR; the codecs agree.
  const fetched = PsszSchema.decode(schema.encode());
  assert.deepEqual(fetched.ir, schema.ir);
  const value = { from: 1n, to: 2n, amount: 3n, payer: null, notify: [], since: { major: 1, minor: 2 } };
  const bytes = Transfer.encode(value);
  assert.equal(toHex(fetched.type('Transfer').encode(value)), toHex(bytes));
  assert.deepEqual(fetched.type('Transfer').decode(bytes), value);
  // The generated schema carries the handlers of the declarations; a
  // fetched one has none until they are registered.
  assert.equal(schema.type('Transfer').psszToJson(bytes), '{"from":"...........1","to":"...........2","amount":3,"payer":null,"notify":[],"since":"1.2"}');
  assert.equal(fetched.type('Transfer').psszToJson(bytes), '{"from":1,"to":2,"amount":3,"payer":null,"notify":[],"since":{"major":1,"minor":2}}');
  assert.equal(Transfer.psszToJson(bytes), schema.type('Transfer').psszToJson(bytes));
  throwsKind(() => generateSchema({ Transfer: custom('Version', record('Transfer', { major: u16, minor: u16 })) }, { requireHandlers: true })
    .type('Transfer').valueToJson({ major: 1, minor: 2 }), 'value', /no handler/);
  // A definition's own schema and name.
  assert.equal(Transfer.name, 'Transfer');
  assert.equal(list(u64).name, '@0');
  assert.deepEqual(list(u64).schema.ir.types.map((m) => m.name), ['@0', '@1']);
});

test('typed layer: recursion through lazy; depth is capped at 64', () => {
  interface Tree { label: string; children: Tree[] }
  const Tree: TypeDef<Tree, RecordView<Tree, { label: string; children: ListView<Tree[], RecordView<Tree>> }>> =
    record('Tree', { label: utf8, children: list(lazy(() => Tree)) }) as never;
  const leaf = (label: string, children: Tree[] = []): Tree => ({ label, children });
  const value = leaf('root', [leaf('a', [leaf('a1')]), leaf('b')]);
  const bytes = Tree.encode(value);
  assert.deepEqual(Tree.decode(bytes), value);
  assert.equal(Tree.view(bytes).get('children').at(0).get('label'), 'a');
  assert.deepEqual(generateSchema({ Tree }).ir.types.map((m) => m.name), ['Tree', '@1', '@2', '@3']);
  // 64 nodes below the root, not more: writing and reading.
  let deep = leaf('x');
  for (let i = 0; i < 31; ++i) deep = leaf('x', [deep]);
  Tree.decode(Tree.encode(deep));
  let tooDeep = deep;
  for (let i = 0; i < 2; ++i) tooDeep = leaf('x', [tooDeep]);
  throwsKind(() => Tree.encode(tooDeep), 'value', /depth/);
  // Bytes nested deeper than any writer produces (a boxed self-reference).
  interface Chain { next: Chain | null }
  const Chain: TypeDef<Chain, RecordView<Chain, { next: RecordView<Chain> | null }>> =
    record('Chain', { next: optional(box(lazy(() => Chain))) }) as never;
  let chain: Chain = { next: null };
  for (let i = 0; i < 31; ++i) chain = { next: chain };
  const ok = Chain.encode(chain);
  assert.deepEqual(Chain.decode(ok), chain);
  // Wrap the frame by hand 40 more times: header (4), slot offset (4), payload.
  let frame = ok;
  for (let i = 0; i < 40; ++i) {
    const next = new Uint8Array(frame.length + 8);
    next.set([4, 0, 0, 0, 4, 0, 0, 0]);
    next.set(frame, 8);
    frame = next;
  }
  throwsKind(() => Chain.decode(frame), 'bytes', /depth/);
  assert.equal(Chain.validate(frame).ok, false);
  // A view descends lazily and stops at the same depth.
  let view: RecordView<Chain, { next: RecordView<Chain> | null }> | null = Chain.view(frame);
  let levels = 0;
  const e = throwsKind(() => {
    while (view) {
      view = view.get('next') as typeof view;
      ++levels;
    }
  }, 'bytes', /depth/);
  assert.ok(e && levels >= 30 && levels <= 64);
});

// ── Custom handlers, keys, I-JSON ───────────────────────────────────────────

test('typed layer: a custom type is its wire type with its own text', () => {
  // An account name: a u64 on the wire and in JavaScript, its name in text.
  const AccountNumber = custom('AccountNumber', u64, { toText: numberToName, fromText: nameToNumber });
  const Balance = record('Balance', { owner: AccountNumber, amount: u64 });
  const alice = nameToNumber('alice');
  const bytes = Balance.encode({ owner: alice, amount: 18446744073709551615n });
  assert.equal(bytes.length, 1 + 16); // a one-byte header: both fields are fixed-size
  assert.deepEqual(Balance.decode(bytes), { owner: alice, amount: 18446744073709551615n });
  // I-JSON: a 64-bit value beyond 2^53 - 1 is a string, a small one a number.
  assert.equal(Balance.psszToJson(bytes), '{"owner":"alice","amount":"18446744073709551615"}');
  assert.equal(Balance.psszToJson(bytes, 'json'), '{"owner":"alice","amount":18446744073709551615}');
  assert.equal(Balance.valueToJson({ owner: alice, amount: 9007199254740991n }), '{"owner":"alice","amount":9007199254740991}');
  assert.equal(Balance.valueToJson({ owner: alice, amount: 9007199254740992n }), '{"owner":"alice","amount":"9007199254740992"}');
  assert.deepEqual(Balance.jsonToValue('{"owner":"alice","amount":"7"}'), { owner: alice, amount: 7n });
  assert.deepEqual(Balance.jsonToValue('{"owner":"alice","amount":7}'), { owner: alice, amount: 7n });
  // With a handler the text is the handler's: the plain number is not read.
  throwsKind(() => Balance.jsonToValue(`{"owner":${alice},"amount":7}`), 'json');
  // No handler: the wire type's text; required: an error, in text only.
  const Plain = custom('AccountNumber', u64);
  assert.equal(Plain.valueToJson(5n), '5');
  throwsKind(() => Plain.type({ requireHandlers: true }).valueToJson(5n), 'value', /no handler/);
  assert.deepEqual(Plain.type({ requireHandlers: true }).decode(Plain.encode(5n)), 5n);
  // A handler that throws, or returns the wrong value, is a typed error.
  const Bad = custom('Bad', u8, { toText: () => { throw new Error('no'); }, fromText: () => 999 });
  throwsKind(() => Bad.valueToJson(1), 'value');
  throwsKind(() => Bad.jsonToValue('"x"'), 'json');
});

test('typed layer: keys', () => {
  const Key = tuple(u32, utf8, optional(i64));
  const key = Key.encodeKey([7, 'alice', -1n]);
  assert.deepEqual(Key.decodeKey(key), [7, 'alice', -1n]);
  assert.deepEqual(Key.validateKey(key), { ok: true });
  assert.equal(Key.validateKey(key.subarray(0, key.length - 1)).ok, false);
  const prefix = Key.encodeKeyPrefix([7]);
  assert.equal(toHex(prefix), '00000007');
  assert.equal(toHex(keyPrefixUpperBound(prefix)!), '00000008');
  assert.ok(Buffer.compare(key.subarray(0, 4), prefix) === 0);
  // Order: bytewise order of keys is value order.
  const keys = [[1, 'b', null], [1, 'a', 5n], [0, 'z', null], [1, 'a', -5n], [1, 'a', null]] as Infer<typeof Key>[];
  const sorted = keys.map((k) => Buffer.from(Key.encodeKey(k))).sort(Buffer.compare).map((k) => Key.decodeKey(k));
  assert.deepEqual(sorted, [[0, 'z', null], [1, 'a', null], [1, 'a', -5n], [1, 'a', 5n], [1, 'b', null]]);
  // A record is a key as well, its fields in order.
  const Row = record('Row', { shard: u32, owner: utf8 });
  assert.equal(toHex(Row.encodeKeyPrefix([7])), '00000007');
  assert.deepEqual(Row.decodeKey(Row.encodeKey({ shard: 7, owner: 'x' })), { shard: 7, owner: 'x' });
});

// ── Views ───────────────────────────────────────────────────────────────────

test('views: fields, elements and alternatives are read in place', () => {
  const Line = record('Line', { sku: utf8, qty: u32, note: optional(utf8) });
  const Order = record('Order', {
    id: u64, lines: list(Line), paid: variant({ no: tuple(), card: fixedBytes(4), account: Account }),
    blob: bytes, where: tuple(f64, f64, optional(utf8)), flags: bitlist(8), maybe: optional(optional(u8)),
    corners: array(Fixed, 2), raw: rawString,
  });
  const value: Infer<typeof Order> = {
    id: 99n,
    lines: [{ sku: 'a', qty: 1, note: null }, { sku: 'bb', qty: 2, note: 'gift' }, { sku: 'ccc', qty: 3, note: null }],
    paid: { type: 'card', value: Uint8Array.of(1, 2, 3, 4) },
    blob: Uint8Array.of(9, 8, 7),
    where: [1.5, -2.5, null],
    flags: { bytes: Uint8Array.of(5), bitCount: 3 },
    maybe: { some: null },
    corners: [{ x: 1, y: 2, on: true }, { x: 3, y: 4, on: false }],
    raw: Uint8Array.of(0xff, 0xfe),
  };
  const data = Order.encode(value);
  const view = Order.view(data);
  assert.ok(view instanceof RecordView);
  assert.deepEqual(view.fields, Object.keys(value));
  assert.equal(view.hasUnknown, false);
  assert.equal(view.get('id'), 99n);
  const lines = view.get('lines');
  assert.ok(lines instanceof ListView);
  assert.equal(lines.length, 3);
  assert.equal(lines.at(1).get('sku'), 'bb');
  assert.equal(lines.at(1).get('note'), 'gift');
  assert.equal(lines.at(2).get('note'), null);
  assert.deepEqual([...lines].map((line) => line.get('qty')), [1, 2, 3]);
  assert.deepEqual(lines.at(0).toValue(), value.lines[0]);
  assert.deepEqual(lines.toValue(), value.lines);
  throwsKind(() => lines.at(3), 'value', /out of range/);
  throwsKind(() => lines.at(-1), 'value');
  throwsKind(() => view.get('nope' as never), 'value', /no field/);
  const paid = view.get('paid');
  assert.ok(paid instanceof VariantView);
  assert.equal(paid.type, 'card');
  assert.equal(paid.index, 1);
  if (paid.type === 'card') assert.deepEqual(paid.value, Uint8Array.of(1, 2, 3, 4));
  assert.deepEqual(paid.toValue(), value.paid);
  // Bytes and raw strings are subarrays of the input, not copies.
  const blob = view.get('blob');
  assert.deepEqual(blob, value.blob);
  assert.equal(blob.buffer, data.buffer);
  assert.equal(view.get('raw').buffer, data.buffer);
  assert.equal(view.bytes.buffer, data.buffer);
  assert.equal(view.bytes.length, data.length);
  const where = view.get('where');
  assert.ok(where instanceof TupleView);
  assert.equal(where.length, 3);
  assert.equal(where.at(1), -2.5);
  assert.equal(where.at(2), null);
  throwsKind(() => where.at(3 as never), 'value');
  assert.deepEqual(view.get('flags'), value.flags);
  assert.deepEqual(view.get('maybe'), { some: null });
  assert.equal(view.get('corners').at(1).get('x'), 3);
  assert.equal(view.get('corners').length, 2);
  assert.deepEqual(view.toValue(), value);
  // The static type of a view follows the declaration.
  const qty: number = lines.at(0).get('qty');
  const note: string | null = lines.at(0).get('note');
  const sku = lines.at(0).get('sku');
  sameType<typeof sku, string>(true);
  assert.deepEqual([qty, note, sku], [1, null, 'a']);
  // @ts-expect-error not a field of Line
  throwsKind(() => lines.at(0).get('price'), 'value');

  // The schema-driven path returns the same views, untyped.
  const fetched = PsszSchema.decode(Order.schema.encode()).type('Order');
  const untyped = fetched.view(data) as RecordView;
  assert.equal(untyped.get('id'), 99n);
  assert.equal(((untyped.get('lines') as ListView).at(1) as RecordView).get('sku'), 'bb');
  // A top-level scalar or list.
  assert.equal(u32.view(u32.encode(7)), 7);
  assert.equal(list(utf8).view(list(utf8).encode(['a', 'b'])).at(1), 'b');
  assert.equal(optional(utf8).view(optional(utf8).encode(null)), null);
  // A Node Buffer is accepted.
  assert.equal(Order.view(Buffer.from(data)).get('id'), 99n);
});

test('views: what is read is checked, with typed errors', () => {
  const Line = record('Line', { sku: utf8, qty: u32 });
  const Lines = list(Line);
  const data = Lines.encode([{ sku: 'a', qty: 1 }, { sku: 'b', qty: 2 }]);
  // A corrupt element is an error when that element is read, not before.
  const corrupt = data.slice();
  corrupt[corrupt.length - 1] = 0xff; // not UTF-8
  const view = Lines.view(corrupt);
  assert.equal(view.length, 2);
  assert.equal(view.at(0).get('sku'), 'a');
  assert.equal(view.at(1).get('qty'), 2);
  throwsKind(() => view.at(1).get('sku'), 'bytes', /UTF-8/);
  throwsKind(() => view.toValue(), 'bytes');
  assert.equal(Lines.validate(corrupt).ok, false);
  // Offsets are checked against the frame before anything is read.
  const table = data.slice();
  table[4] = 0xf0;
  throwsKind(() => Lines.view(table).at(0), 'bytes');
  throwsKind(() => Lines.view(table).at(1), 'bytes');
  const first = data.slice();
  first[0] = 0xff;
  first[1] = 0xff;
  throwsKind(() => Lines.view(first).length, 'bytes');
  throwsKind(() => Lines.view(data.subarray(0, 3)).length, 'bytes');
  throwsKind(() => Line.view(new Uint8Array(2)).get('qty'), 'bytes');
  throwsKind(() => Line.view('nope' as never), 'bytes');
  throwsKind(() => variant({ a: u8 }).view(Uint8Array.of(9)).type, 'bytes');
  throwsKind(() => variant({ a: u8 }).view(new Uint8Array(0)).value, 'bytes');
  throwsKind(() => optional(list(u8)).view(Uint8Array.of(2)), 'bytes');
  throwsKind(() => fixedBytes(4).view(new Uint8Array(3)), 'bytes');
  throwsKind(() => array(u16, 3).view(new Uint8Array(5)).length, 'bytes');
  // A cap is checked for the frame the view is on.
  const Tiny = record('Tiny', { memo: utf8 }, { maxDynamicData: 8 });
  const over = record('Tiny', { memo: utf8 }).type({ width: 1 }).encode({ memo: 'x'.repeat(20) });
  throwsKind(() => Tiny.view(over).get('memo'), 'bytes', /maxDynamicData/);
});

test('scalars and text: every combinator round-trips', () => {
  const All = record('All', {
    a: bool, b: u8, c: u16, d: u32, e: u64, f: u128, g: u256, h: i8, i: i16, j: i32, k: i64, l: i128, m: f32, n: f64,
    s: utf8, r: rawString, y: bytes,
  });
  const value: Infer<typeof All> = {
    a: true, b: 255, c: 65535, d: 4294967295, e: (1n << 64n) - 1n, f: (1n << 128n) - 1n, g: (1n << 256n) - 1n,
    h: -128, i: -32768, j: -2147483648, k: -(1n << 63n), l: -(1n << 127n), m: 0.5, n: Math.PI,
    s: 'héllo', r: Uint8Array.of(0xff), y: Uint8Array.of(1, 2),
  };
  assert.deepEqual(All.decode(All.encode(value)), value);
  // (The raw string 0xff is not UTF-8: it has no text.)
  throwsKind(() => All.valueToJson(value), 'value', /UTF-8/);
  assert.deepEqual(All.jsonToValue(All.valueToJson({ ...value, r: Uint8Array.of(0x41) })), { ...value, r: Uint8Array.of(0x41) });
  assert.deepEqual(All.jsonToValue(All.valueToJson({ ...value, r: Uint8Array.of(0x41) }, 'json'), 'json'), { ...value, r: Uint8Array.of(0x41) });
  assert.deepEqual(All.decodeKey(All.encodeKey(value)), value);
  for (const name of All.view(All.encode(value)).fields)
    assert.deepEqual(All.view(All.encode(value)).get(name as 'a'), value[name as 'a'], name);
  throwsKind(() => u8.encode(256), 'value');
  throwsKind(() => i8.encode(-129), 'value');
  throwsKind(() => utf8.encode('\ud800'), 'value');
});
