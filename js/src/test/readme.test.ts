// The examples of README.md, executed: what the README says a call returns
// is what it returns.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  bool, defineInterface, f64, isCompressedName, isHashName, jsonValue, method, methodToNumber, numberToMethod, psszEquivalent, psszEvolvesFrom, PsszJsonText,
  PsszSchema, RecordView, SchemaBuilder, functionTypeToJson, argumentTuple, field, rawString, boundedList, boundedUtf8, bytes, custom, emitWit, fixedBytes, fromHex, generateSchema, i32,
  i64, keyPrefixUpperBound, lazy, list, nameToNumber, numberToName, optional, pjsonDecode, pjsonEncode, pjsonValidate,
  PjsonObject, PsszError, map, nested, pair, record, set, toHex, transparent, tuple, u16, u32, u64, u8, utf8, timePoint, variant,
  type Infer, type ListView, type PsszCustomHandler, type TypeDef,
} from '../index.js';

test('README: from a schema fetched at run time', () => {
  // What a node would serve: here, generated and sent as bytes.
  const schemaBytes = generateSchema({ Transfer: record('Transfer', { from: utf8, amount: u64 }) }).encode();

  const schema = PsszSchema.decode(schemaBytes);
  const Transfer = schema.type('Transfer');
  const data = Transfer.jsonToPssz('{"from":"alice","amount":"18446744073709551615"}');
  assert.deepEqual(Transfer.validate(data), { ok: true, hasUnknown: false });
  assert.deepEqual(Transfer.decode(data), { from: 'alice', amount: 18446744073709551615n });
  assert.equal(Transfer.psszToJson(data), '{"from":"alice","amount":"18446744073709551615"}');
  assert.equal(Transfer.psszToJson(data, 'json'), '{"from":"alice","amount":18446744073709551615}');
  assert.ok(Transfer.encode({ from: 'alice', amount: 5n }) instanceof Uint8Array);
  assert.equal((Transfer.view(data) as RecordView).get('amount'), 18446744073709551615n);
});

test('README: typed definitions', () => {
  const Transfer = record('Transfer', { from: utf8, to: utf8, amount: u64, memo: optional(utf8), tags: list(utf8) });
  type Transfer = Infer<typeof Transfer>;
  const sample: Transfer = { from: 'alice', to: 'bob', amount: 5n, memo: null, tags: [] };
  const data = Transfer.encode(sample);
  const value = Transfer.decode(data);
  const amount: bigint = value.amount;
  assert.equal(amount, 5n);
  assert.deepEqual(Transfer.validate(data), { ok: true, hasUnknown: false });
  assert.equal(Transfer.psszToJson(data), '{"from":"alice","to":"bob","amount":5,"memo":null,"tags":[]}');
  assert.deepEqual(Transfer.jsonToValue('{"from":"a","to":"b","amount":"7","memo":"hi","tags":[]}'),
    { from: 'a', to: 'b', amount: 7n, memo: 'hi', tags: [] });
  assert.equal(Transfer.view(data).get('amount'), 5n);
  const schema = generateSchema({ Transfer });
  assert.deepEqual(PsszSchema.decode(schema.encode()).type('Transfer').decode(data), sample);

  const V1 = record('Person', { name: utf8, age: u32 });
  const V2 = record('Person', { name: utf8, age: u32, email: optional(utf8) });
  assert.deepEqual(V2.decode(V1.encode({ name: 'Alice', age: 30 })), { name: 'Alice', age: 30, email: null });
  assert.deepEqual(V1.decode(V2.encode({ name: 'Alice', age: 30, email: 'a@b' })), { name: 'Alice', age: 30 });

  const Small = record('Small', { tag: boundedUtf8(8), ids: boundedList(u16, 4), key: fixedBytes(4) });
  assert.equal(Small.width, 1);
  assert.equal(record('Note', { text: utf8 }, { maxDynamicData: 200 }).width, 1);
  assert.equal(record('Open', { text: utf8 }).width, 4);
  assert.equal(tuple(u64, boundedUtf8(32), u8).width, 1);
  assert.equal(tuple(u64, boundedUtf8(32), u8).inferredBound, 43n);
  assert.equal(tuple(u64, boundedUtf8(32), u8).maxEncodedSize, 43n);
  assert.equal(record('Note', { text: utf8 }, { maxDynamicData: 200 }).inferredBound, null);
  assert.equal(record('Note', { text: utf8 }, { maxDynamicData: 200 }).maxEncodedSize, 200n);
  assert.equal(record('Row', { id: u64, memo: boundedUtf8(80) }, { maxDynamicData: 1024 }).inferredBound, 90n);
  assert.equal(record('Row', { id: u64, memo: boundedUtf8(80) }, { maxDynamicData: 1024 }).maxEncodedSize, 92n);
  assert.equal(record('Open', { text: utf8 }).inferredBound, null);
  assert.equal(boundedList(boundedUtf8(16), 15).width, 1);
  assert.equal(boundedList(boundedUtf8(17), 15).width, 2);
  assert.equal(list(boundedUtf8(16)).width, 4);
  assert.equal(record('Event', { at: u64, what: variant({ tick: u8, point: tuple(u32, u32) }) }).width, 1);
  assert.equal(tuple(u32, utf8).width, 4);
  assert.equal(Small.type({ width: 2 }).width, 2);

  // A record's cap bounds what lies beneath it.
  const Memo = record('Memo', { token: u32, memo: utf8, tags: list(utf8), rows: list(list(utf8)) }, { maxDynamicData: 255 });
  assert.equal(toHex(Memo.encode({ token: 1, memo: 'hi', tags: ['ab', 'c'], rows: [['x']] })),
    '07' + '01000000' + '07' + '09' + '0e' + '6869' + '0204' + '6162' + '63' + '01' + '01' + '78');
  assert.deepEqual(Memo.decode(Memo.encode({ token: 1, memo: 'hi', tags: ['ab', 'c'], rows: [['x']] })),
    { token: 1, memo: 'hi', tags: ['ab', 'c'], rows: [['x']] });
  assert.equal(Memo.childWiderThanCap, false);
  const Envelope = record('Envelope', { body: record('Body', { text: utf8 }) }, { maxDynamicData: 255 });
  assert.equal(Envelope.childWiderThanCap, true);

  interface Tree { label: string; children: Tree[] }
  const Tree: TypeDef<Tree, RecordView<Tree>> = record('Tree', { label: utf8, children: list(lazy(() => Tree)) });
  const tree: Tree = { label: 'root', children: [{ label: 'leaf', children: [] }] };
  assert.deepEqual(Tree.decode(Tree.encode(tree)), tree);
});

test('README: views', () => {
  const Line = record('Line', { sku: utf8, qty: u32, note: optional(utf8) });
  const Order = record('Order', { id: u64, lines: list(Line), blob: bytes });
  const orderBytes = Order.encode({
    id: 1n, blob: Uint8Array.of(1, 2),
    lines: [{ sku: 'a', qty: 2, note: null }, { sku: 'b', qty: 3, note: 'x' }],
  });
  const view = Order.view(orderBytes);
  assert.equal(view.get('id'), 1n);
  const lines = view.get('lines');
  assert.equal(lines.length, 2);
  assert.equal(lines.at(1).get('sku'), 'b');
  let total = 0;
  for (const line of lines) total += line.get('qty');
  assert.equal(total, 5);
  assert.equal(view.get('blob').buffer, orderBytes.buffer);
  assert.deepEqual(lines.at(0).toValue(), { sku: 'a', qty: 2, note: null });
  // From a fetched schema: the same views, untyped.
  const untyped = PsszSchema.decode(Order.schema.encode()).type('Order').view(orderBytes) as RecordView;
  assert.equal((untyped.get('lines') as ListView).length, 2);
  // Decode, modify, encode.
  const order = Order.decode(orderBytes);
  order.lines[0].qty += 1;
  const updated = Order.encode(order);
  assert.equal(Order.view(updated).get('lines').at(0).get('qty'), 3);
});

test('README: keys', () => {
  const Key = tuple(u32, utf8, optional(i64));
  const key = Key.encodeKey([7, 'alice', -1n]);
  assert.deepEqual(Key.decodeKey(key), [7, 'alice', -1n]);
  assert.deepEqual(Key.validateKey(key), { ok: true });
  const from = Key.encodeKeyPrefix([7]);
  const to = keyPrefixUpperBound(from)!;
  assert.ok(Buffer.compare(from, key) < 0 && Buffer.compare(key, to) < 0);
});

test('README: JSON and I-JSON', () => {
  const Balance = record('Balance', { owner: utf8, amount: u64 });
  assert.equal(Balance.valueToJson({ owner: 'alice', amount: 9007199254740991n }), '{"owner":"alice","amount":9007199254740991}');
  assert.equal(Balance.valueToJson({ owner: 'alice', amount: 9007199254740992n }), '{"owner":"alice","amount":"9007199254740992"}');
  assert.equal(Balance.valueToJson({ owner: 'alice', amount: 9007199254740992n }, 'json'), '{"owner":"alice","amount":9007199254740992}');
  assert.deepEqual(Balance.jsonToValue('{"owner":"alice","amount":"7"}'), { owner: 'alice', amount: 7n });
  assert.deepEqual(Balance.jsonToValue('{"owner":"alice","amount":7}'), { owner: 'alice', amount: 7n });
  // Plain JSON data for code that expects it: every number is safe.
  assert.deepEqual(JSON.parse(Balance.valueToJson({ owner: 'alice', amount: 9007199254740992n })),
    { owner: 'alice', amount: '9007199254740992' });
});

test('README: the schema as JSON', () => {
  const schema = new PsszSchema({ Point: { Object: { x: { Int: { bits: 32, isSigned: true } } } } });
  assert.equal(schema.toJson(),
    '{"package":{"name":"","version":"","attributes":[]},"interfaces":[],"worlds":[],"uses":[],' +
    '"types":[{"name":"Point","type":{"value":{"Object":{"members":[{"name":"x","type":{"value":' +
    '{"Int":{"bits":32,"isSigned":true}}},"attributes":[]}],"attributes":[]}}},"attributes":[]}],"ir_version":1}');
  assert.equal(schema.toJson('json'),
    '{"package":{"name":"","version":"","attributes":[]},"interfaces":[],"worlds":[],"uses":[],' +
    '"types":[{"name":"Point","type":{"value":[1,{"members":[{"name":"x","type":{"value":' +
    '[8,{"bits":32,"isSigned":true}]},"attributes":[]}],"attributes":[]}]},"attributes":[]}],"ir_version":1}');
  assert.deepEqual(PsszSchema.fromJson(schema.toJson()).ir, schema.ir);
  assert.deepEqual(PsszSchema.fromJson(schema.toJson('json'), 'json').ir, schema.ir);
});

test('README: custom types', () => {
  const Identifier = custom('Identifier', u64, { toText: numberToName, fromText: nameToNumber });
  const Balance = record('Balance', { owner: Identifier, amount: u64 });
  const data = Balance.encode({ owner: nameToNumber('alice'), amount: 5n });
  assert.deepEqual(Balance.decode(data), { owner: nameToNumber('alice'), amount: 5n });
  assert.equal(Balance.psszToJson(data), '{"owner":"alice","amount":5}');
  assert.deepEqual(Balance.jsonToValue('{"owner":"alice","amount":5}'), { owner: nameToNumber('alice'), amount: 5n });

  // Handlers for a fetched schema, by id.
  const Version = custom('Version', record('Version', { major: u16, minor: u16 }));
  const schemaBytes = generateSchema({ Release: record('Release', { version: Version, name: utf8 }) }).encode();
  const version: PsszCustomHandler = {
    toText: (v) => `${(v as { major: number }).major}.${(v as { minor: number }).minor}`,
    fromText(text) {
      const m = /^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$/.exec(text);
      if (!m) throw new Error('not a version');
      return { major: Number(m[1]), minor: Number(m[2]) };
    },
  };
  const schema = PsszSchema.decode(schemaBytes, { handlers: { Version: version } });
  const Release = schema.type('Release');
  const bytesOf = Release.jsonToPssz('{"version":"1.2","name":"first"}');
  assert.equal(Release.psszToJson(bytesOf), '{"version":"1.2","name":"first"}');
  assert.deepEqual(Release.decode(bytesOf), { version: { major: 1, minor: 2 }, name: 'first' });
  assert.equal(PsszSchema.decode(schemaBytes).type('Release').psszToJson(bytesOf), '{"version":{"major":1,"minor":2},"name":"first"}');
});

test('README: what a blockchain client needs', async () => {
  // The service publishes its schema; the client packs an action's
  // arguments from it, builds a transaction and hashes its canonical bytes.
  const Identifier = custom('Identifier', u64, { toText: numberToName, fromText: nameToNumber });
  const serviceSchema = PsszSchema.decode(
    generateSchema({ transfer: record('transfer', { to: Identifier, amount: u64, memo: utf8 }) }).encode(),
    { handlers: { Identifier: { toText: (v) => numberToName(v as bigint), fromText: nameToNumber } } },
  );
  const args = serviceSchema.type('transfer').jsonToPssz('{"to":"bob","amount":"2500000000000000000","memo":"rent"}');
  const Action = record('Action', { sender: Identifier, service: Identifier, method: utf8, rawData: bytes });
  const Transaction = record('Transaction', { expiration: timePoint('seconds', u32), actions: list(Action) });
  const tx = Transaction.encode({
    expiration: 1767225600n,
    actions: [{ sender: nameToNumber('alice'), service: nameToNumber('tokens'), method: 'transfer', rawData: args }],
  });
  assert.deepEqual(Transaction.validate(tx), { ok: true, hasUnknown: false });
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', tx));
  assert.equal(digest.length, 32);
  assert.deepEqual(fromHex(toHex(tx)), tx);
  // Reading it back: one action's arguments through views, then as text.
  const raw = Transaction.view(tx).get('actions').at(0).get('rawData');
  assert.equal(serviceSchema.type('transfer').psszToJson(raw), '{"to":"bob","amount":"2500000000000000000","memo":"rent"}');
  assert.equal(Transaction.psszToJson(tx).startsWith('{"expiration":"2026-01-01T00:00:00Z","actions":[{"sender":"alice","service":"tokens"'), true);
});

test('README: WIT, pjson, hex', () => {
  assert.equal(emitWit(generateSchema({ Point: record('Point', { x: i32, y: i32 }) })), 'record point {\n  x: s32,\n  y: s32\n}\n');
  const data = pjsonEncode({ id: 42n, name: 'Alice', tags: ['a', 'b'] });
  assert.equal(pjsonValidate(data), true);
  const decoded = pjsonDecode(data);
  assert.ok(decoded instanceof PjsonObject);
  assert.deepEqual(decoded.entries, [['id', 42n], ['name', 'Alice'], ['tags', ['a', 'b']]]);
  assert.equal(toHex(Uint8Array.of(0xde, 0xad)), 'dead');
  assert.deepEqual(fromHex('DEad'), Uint8Array.of(0xde, 0xad));
  assert.throws(() => fromHex('xyz'));
});

test('README: pairs, maps, sets, nested values, transparent types', () => {
  const Args = record('Args', { to: utf8, amount: u64 });
  const Id = transparent('Id', u64);
  const Book = record('Book', {
    owner: Id,
    head: pair(u16, utf8),
    counts: map(utf8, u32),
    ids: set(u16),
    byPoint: map(tuple(u8, u8), utf8),
    call: nested(Args),
  });
  type Book = Infer<typeof Book>;
  const value: Book = {
    owner: 7n, head: [513, 'x'],
    counts: new Map([['b', 2], ['a', 1]]),
    ids: new Set([513, 2]),
    byPoint: new Map([[[1, 2], 'p']]),
    call: { to: 'bob', amount: 5n },
  };
  const encoded = Book.encode(value);
  const book = Book.decode(encoded);
  assert.deepEqual([...book.counts], [['a', 1], ['b', 2]]);
  assert.deepEqual([...book.ids], [2, 513]);
  assert.equal(Book.psszToJson(encoded),
    '{"owner":7,"head":[513,"x"],"counts":{"a":1,"b":2},"ids":[2,513],"byPoint":[[[1,2],"p"]],"call":{"to":"bob","amount":5}}');
  // A Map finds a composite key by identity: iterate.
  assert.equal(book.byPoint.get([1, 2]), undefined);
  assert.deepEqual([...book.byPoint], [[[1, 2], 'p']]);
  // The same record with a bytes member is byte-identical.
  const Routed = record('Book', {
    owner: u64, head: tuple(u16, utf8), counts: list(tuple(utf8, u32)), ids: list(u16),
    byPoint: list(tuple(tuple(u8, u8), utf8)), call: bytes,
  });
  assert.equal(toHex(Routed.encode({
    owner: 7n, head: [513, 'x'], counts: [['a', 1], ['b', 2]], ids: [2, 513], byPoint: [[[1, 2], 'p']],
    call: Args.encode({ to: 'bob', amount: 5n }),
  })), toHex(encoded));
});

test('README: invariants', () => {
  const DerKey = custom('DerKey', transparent('DerKey', bytes), {
    check: (b) => (b.length > 0 && b[0] === 0x30 ? undefined : 'DerKey: not a DER sequence'),
  });
  const Range = custom('Range', record('Range', { lo: u32, hi: u32 }), {
    check: (r) => (r.lo <= r.hi ? undefined : 'Range: lo is greater than hi'),
  });
  assert.equal(toHex(Range.encode({ lo: 1, hi: 5 })), '080100000005000000');
  const reversedBytes = fromHex('080500000001000000');
  const failure = (run: () => unknown): PsszError => {
    try {
      run();
    } catch (e) {
      assert.ok(e instanceof PsszError);
      return e;
    }
    return assert.fail('accepted');
  };
  const written = failure(() => Range.encodeChecked({ lo: 5, hi: 1 }));
  assert.deepEqual([written.kind, written.message, written.invariant], ['value', 'Range: lo is greater than hi', true]);
  const validated = Range.validate(reversedBytes);
  assert.ok(!validated.ok && validated.error.kind === 'bytes' && validated.error.message === 'Range: lo is greater than hi');
  const read = failure(() => Range.jsonToValue('{"lo":5,"hi":1}'));
  assert.deepEqual([read.kind, read.message], ['json', 'Range: lo is greater than hi']);
  // A view reads in place and runs no check; decoding through it does.
  assert.equal(Range.view(reversedBytes).get('lo'), 5);
  assert.equal(failure(() => Range.view(reversedBytes).toValue()).message, 'Range: lo is greater than hi');
  assert.equal(failure(() => DerKey.encodeChecked(new Uint8Array(0))).message, 'DerKey: not a DER sequence');
  assert.equal(toHex(DerKey.encode(fromHex('3000'))), '3000');
  assert.equal(failure(() => DerKey.encodeKeyChecked(fromHex('31'))).message, 'DerKey: not a DER sequence');
});

test('README: bounds at any depth, method arguments', () => {
  const Post = record('Post', {
    title: field(rawString, { utf8: true, max: 80 }),
    tags: field(list(rawString), { max: 8, element: { utf8: true, max: 16 } }),
    votes: field(map(rawString, list(u32)), { max: 4, keys: { utf8: true, max: 16 }, values: { max: 2 } }),
    origin: field(tuple(rawString, u32, bytes), { at: { 0: { utf8: true, max: 16 }, 2: { max: 5 } } }),
    body: field(variant({ Text: rawString, Parts: list(rawString) }),
      { at: { Text: { utf8: true, max: 280 }, Parts: { max: 4, element: { utf8: true, max: 280 } } } }),
    reply: field(optional(list(rawString)), { max: 2, element: { max: 32 } }),
  });
  type Post = Infer<typeof Post>;
  const post: Post = {
    title: 'Hello', tags: ['a', 'b'], votes: new Map([['up', [1, 2]]]), origin: ['web', 7, Uint8Array.of(1)],
    body: { type: 'Text', value: 'hi' }, reply: null,
  };
  const typed: {
    title: string; tags: string[]; votes: Map<string, number[]>; origin: [string, number, Uint8Array];
    body: { type: 'Text'; value: string } | { type: 'Parts'; value: string[] }; reply: Uint8Array[] | null;
  } = post;
  assert.deepEqual(Post.decode(Post.encode(typed)), post);
  assert.equal(Post.width, 2);
  // Each type counts its own header / slots / table at its own width:
  // title 80; tags 8 * (1 + 16) = 136; votes 4 * (1 + 27) = 112, an entry
  // being 1 + (1 + 16) + (1 + 8) = 27; origin 1 + (1 + 16) + 4 + (1 + 5) =
  // 28; body 1 + 4 * (2 + 280) = 1129; reply 1 + 2 * (1 + 32) = 67; and the
  // record's own header and six slots at 2 bytes.
  assert.equal(2 * 7 + 80 + 136 + 112 + 28 + 1129 + 67, 1566);
  assert.equal(Post.inferredBound, 1566n);
  // Some at-the-bound values fit; the widest (the four parts) does too.
  const full: Post = { ...post, body: { type: 'Parts', value: Array(4).fill('x'.repeat(280)) }, tags: Array(8).fill('t'.repeat(16)) };
  assert.ok(Post.encode(full).length <= 1566);

  const Tags = record('Tags', { tags: field(list(rawString), { max: 8, element: { utf8: true, max: 16 } }) });
  const Wrapped = record('Tags', { tags: boundedList(boundedUtf8(16), 8) });
  assert.equal(toHex(Tags.encode({ tags: ['ab', 'c'] })), '01010204616263');
  assert.equal(toHex(Wrapped.encode({ tags: ['ab', 'c'] })), '01010204616263');
  assert.deepEqual(Wrapped.decode(Tags.encode({ tags: ['ab', 'c'] })), { tags: ['ab', 'c'] });
  // The plain type registered and the bounded type in the member; a registered type of its own.
  const inPlace = generateSchema({ Tags }).ir.types;
  assert.equal((inPlace[0].type.value.value as { members: { type: { value: { type: string } } }[] }).members[0].type.value.type, 'BoundedList');
  const registered = generateSchema({ Tags: Wrapped }).ir.types;
  assert.equal((registered[0].type.value.value as { members: { type: { value: { type: string } } }[] }).members[0].type.value.type, 'Type');

  // Every writer and reader checks every bound.
  const refused = (run: () => unknown, kind: string, message: string) =>
    assert.throws(run, (e: unknown) => e instanceof PsszError && e.kind === kind && e.message === message);
  const long = { ...post, tags: ['a'.repeat(17)] };
  refused(() => Post.encodeChecked(long), 'value', 'pssz IR: byte count bound exceeded');
  refused(() => Post.encodeKeyChecked(long), 'value', 'key: string exceeds its bound');
  refused(() => Post.valueToJsonChecked(long), 'value', 'json: string exceeds its bound');
  const Loose = record('Post', {
    title: utf8, tags: list(utf8), votes: map(utf8, list(u32)), origin: tuple(utf8, u32, bytes),
    body: variant({ Text: utf8, Parts: list(utf8) }), reply: optional(list(rawString)),
  });
  const text = Loose.valueToJson(long);
  refused(() => Post.jsonToValue(text), 'json', 'json: string exceeds its bound');
  refused(() => Post.jsonToPssz(text), 'json', 'json: string exceeds its bound');
  refused(() => Post.decodeKey(Loose.encodeKeyChecked(long)), 'bytes', 'key: string exceeds its bound');
  for (const width of [1, 2, 4] as const) {
    const frame = Loose.type({ width }).encode(long);
    const reader = Post.type({ width });
    assert.equal(reader.validate(frame).ok, false);
    refused(() => reader.decode(frame), 'bytes', 'pssz IR: byte count bound exceeded');
    refused(() => reader.psszToJson(frame), 'bytes', 'pssz IR: byte count bound exceeded');
    refused(() => (reader.view(frame) as RecordView).toValue(), 'bytes', 'pssz IR: byte count bound exceeded');
  }
  assert.throws(
    () => record('R', { a: field(list(u32), { element: { max: 3 } }) }).schema,
    (e: unknown) => e instanceof PsszError && e.kind === 'schema' && /member "a", element: max bounds/.test(e.message),
  );

  // Method arguments.
  const parameters = {
    to: field(rawString, { utf8: true, max: 16 }), amount: u64, memo: field(rawString, { utf8: true, max: 80 }),
  };
  const Transfer = argumentTuple(parameters, { maxDynamicData: 256 });
  const Transfer2 = argumentTuple({ ...parameters, fee: optional(u32) }, { maxDynamicData: 256 });
  assert.equal(Transfer.width, 2);
  assert.equal(argumentTuple(parameters).width, 1);
  assert.equal(argumentTuple(parameters).inferredBound, 107n);
  // The largest argument list: under the cap the header and the two slots
  // are 2 bytes wide.
  assert.equal(Transfer.inferredBound, 107n);
  assert.equal(Transfer.maxEncodedSize, 110n);
  const args = Transfer.encode({ to: 'al', amount: 5n, memo: 'hi' });
  assert.equal(toHex(args), '0c000c0005000000000000000e00616c6869');
  assert.deepEqual(Transfer2.decode(args), { to: 'al', amount: 5n, memo: 'hi', fee: null });
  assert.deepEqual(Transfer.validate(Transfer2.encode({ to: 'al', amount: 5n, memo: 'hi', fee: 3 })), { ok: true, hasUnknown: true });
  const asTuple = tuple(boundedUtf8(16), u64, boundedUtf8(80));
  assert.equal(toHex(asTuple.type({ width: 2 }).encode(['al', 5n, 'hi'])), toHex(args));
  assert.equal(toHex(asTuple.encode(['al', 5n, 'hi'])), toHex(argumentTuple(parameters).encode({ to: 'al', amount: 5n, memo: 'hi' })));
  assert.equal(toHex(Transfer.encodeKey({ to: 'al', amount: 5n, memo: 'hi' })), toHex(asTuple.encodeKey(['al', 5n, 'hi'])));
  refused(() => Transfer.encodeChecked({ to: 'a'.repeat(17), amount: 5n, memo: '' }), 'value', 'pssz IR: byte count bound exceeded');
  const Note = argumentTuple({ text: rawString }, { maxDynamicData: 64 });
  refused(() => Note.encodeChecked({ text: new Uint8Array(63) }), 'value', 'pssz IR: encoded size exceeds maxDynamicData cap');
  assert.equal(Transfer.valueToJson({ to: 'al', amount: 5n, memo: 'hi' }), '{"to":"al","amount":5,"memo":"hi"}');
  const ir = generateSchema({ transfer: Transfer }).ir.types;
  assert.deepEqual(ir.map((m) => m.name), ['@0', '@1', '@2', 'transfer']);
  assert.equal(ir[0].type.value.type, 'Object');
  assert.deepEqual((ir[0].type.value.value as { attributes: unknown[] }).attributes,
    [{ name: 'tuple', value: 'named' }, { name: 'maxDynamicData', value: '256' }]);
  // In text: the object, or the positional array.
  assert.deepEqual(Transfer.jsonToValue('{"memo":"hi","to":"al","amount":5}'), { to: 'al', amount: 5n, memo: 'hi' });
  assert.deepEqual(Transfer.jsonToValue('["al",5,"hi"]'), { to: 'al', amount: 5n, memo: 'hi' });
  // Parameters without names: the array.
  const Add = argumentTuple([u32, u32]);
  assert.equal(toHex(Add.encode([1, 2])), '080100000002000000');
  assert.equal(Add.valueToJson([1, 2]), '[1,2]');
  const fetchedAdd = PsszSchema.decode(generateSchema({ add: Add }).encode()).type('add');
  assert.deepEqual(fetchedAdd.decode(Add.encode([1, 2])), [1, 2]);
  assert.equal(fetchedAdd.valueToJson([1, 2]), '[1,2]');
});

test('README: a transparent type with a bounded member', () => {
  const Memo = transparent('Memo', field(rawString, { utf8: true, max: 80 }));
  assert.equal(Memo.inferredBound, 80n);
  assert.equal(toHex(Memo.encode('hi')), '6869');
  assert.throws(() => Memo.encodeChecked('x'.repeat(81)), (e: unknown) => e instanceof PsszError && e.message === 'pssz IR: byte count bound exceeded');
  const Wrapped = transparent('Memo', boundedUtf8(80));
  assert.equal(toHex(Wrapped.encode('hi')), '6869');
  assert.equal(Wrapped.inferredBound, 80n);
  assert.deepEqual(generateSchema({ Memo }).ir, generateSchema({ Memo: Wrapped }).ir);
});

test('README: order constraints', () => {
  const Ranked = record('Ranked', {
    scores: field(list(u32), { sorted: true, max: 8 }),
    names: field(list(utf8), { unique: true }),
    entries: field(list(pair(utf8, u32)), { uniqueKeys: true }),
    rows: field(list(list(u8)), { element: { sorted: true } }),
  });
  const refused = (run: () => unknown, message: string) =>
    assert.throws(run, (e: unknown) => e instanceof PsszError && e.kind === 'value' && e.message === message);
  refused(() => Ranked.encodeChecked({ scores: [2, 1], names: [], entries: [], rows: [] }), 'sorted: elements are not in order');
  refused(() => Ranked.encodeChecked({ scores: [], names: ['a', 'a'], entries: [], rows: [] }), 'sorted: elements are not in strictly ascending order');
  refused(() => Ranked.encodeChecked({ scores: [], names: [], entries: [['a', 1], ['b', 2], ['a', 3]], rows: [] }), 'unique_keys: two elements have the same key');
  const ok = { scores: [1, 1, 2, 300], names: ['a', 'ab', 'b'], entries: [['x', 1], ['a', 1]] as [string, number][], rows: [[1, 2, 2], [], [9]] };
  assert.deepEqual(Ranked.decode(Ranked.encode(ok)), ok);
  // Not in the Schema IR: a fetched schema does not know them.
  const Plain = record('Ranked', {
    scores: field(list(u32), { max: 8 }), names: list(utf8), entries: list(pair(utf8, u32)), rows: list(list(u8)),
  });
  assert.equal(toHex(generateSchema({ Ranked }).encode()), toHex(generateSchema({ Ranked: Plain }).encode()));
  const unsorted = Plain.encode({ ...ok, scores: [2, 1] });
  assert.equal(Ranked.validate(unsorted).ok, false);
  assert.equal(PsszSchema.decode(generateSchema({ Ranked }).encode()).type('Ranked').validate(unsorted).ok, true);
  assert.equal(psszEvolvesFrom(Plain, Ranked).ok, false);
});

test('README: maxFields', () => {
  const Account = record('Account', { id: u32, name: optional(utf8) }, { maxFields: 3 });
  const schemaError = (run: () => unknown, message: string) =>
    assert.throws(run, (e: unknown) => e instanceof PsszError && e.kind === 'schema' && e.message === `typed schema: ${message}`);
  schemaError(() => record('Account', { a: u32, b: u32, c: u32 }, { maxFields: 2 }), 'record "Account" has more fields than its maxFields (2)');
  schemaError(() => argumentTuple({ id: u32 }, { maxFields: 256 }), 'maxFields of the method must be an integer from 0 to 255');
  const grown = record('Account', { id: u32, name: optional(utf8), email: optional(utf8) }, { maxFields: 3 });
  assert.deepEqual(psszEvolvesFrom(Account, grown), { ok: true });
  const raised = record('Account', { id: u32, name: optional(utf8) }, { maxFields: 8 });
  assert.deepEqual(psszEvolvesFrom(Account, raised), { ok: false, code: 'max-fields', reason: 'maxFields was raised (3 -> 8)', path: '(root)' });
  // The schema: more members than its maxFields is malformed.
  const ir = JSON.parse(JSON.stringify(generateSchema({ Account }).ir));
  ir.types[0].type.value.value.attributes[0].value = '1';
  assert.throws(() => new PsszSchema(ir).type('Account'), (e: unknown) =>
    e instanceof PsszError && e.kind === 'schema' && e.message === 'pssz IR: a record has more fields than its maxFields');
  assert.equal(new PsszSchema(ir).validate().ok, false);
});

test('README: interfaces', () => {
  const Receipt = record('Receipt', { id: u64, note: field(rawString, { utf8: true, max: 32 }) });
  const Token = defineInterface('token', {
    transfer: method(
      { to: field(rawString, { utf8: true, max: 16 }), amount: u64, memo: field(optional(rawString), { utf8: true, max: 80 }) },
      { returns: Receipt, maxDynamicData: 256, maxFields: 8 }),
    names: method({ limit: u32 }, { returns: field(list(rawString), { max: 4, element: { utf8: true, max: 16 } }) }),
    note: method({ text: field(rawString, { max: 62 }) }, { maxDynamicData: 64 }),
    add: method([u32, u32], { returns: u64 }),
    ping: method({}),
    batch: method({ names: field(list(rawString), { max: 4, element: { max: 8 } }), tip: u8 }, { returns: bool }),
  });
  const transfer = Token.methods.transfer;
  assert.equal(toHex(transfer.args.encode({ to: 'bob', amount: 5n, memo: 'hi' })), '0c000c0005000000000000000f00626f62016869');
  assert.equal(transfer.args.valueToJson({ to: 'bob', amount: 5n, memo: 'hi' }), '{"to":"bob","amount":5,"memo":"hi"}');
  assert.deepEqual(transfer.args.jsonToValue('["bob",5,null]'), { to: 'bob', amount: 5n, memo: null });
  assert.equal(toHex(Token.methods.add.args.encode([1, 2])), '080100000002000000');
  assert.equal(toHex(Token.methods.names.result.encode(['alice', 'bob'])), '0207616c696365626f62');
  assert.deepEqual(Token.methods.names.result.decode(new Uint8Array(0)), []);
  assert.equal(Token.methods.add.result.decode(new Uint8Array(8)), 0n);
  assert.equal(Token.methods.transfer.result.psszToJson(Token.methods.transfer.result.encode({ id: 7n, note: 'ok' })), '{"id":7,"note":"ok"}');
  assert.equal(Token.methods.ping.result, null);
  // The result in one call each way.
  const twoNames = Uint8Array.of(2, 7, 0x61, 0x6c, 0x69, 0x63, 0x65, 0x62, 0x6f, 0x62);
  assert.equal(toHex(Token.methods.names.resultToPssz(['alice', 'bob'])), '0207616c696365626f62');
  assert.deepEqual(Token.methods.names.resultFromPssz(twoNames), ['alice', 'bob']);
  assert.equal(Token.methods.names.validateResult(new Uint8Array(1)).ok, false);
  assert.equal(Token.methods.names.resultView(twoNames).at(1), 'bob');
  assert.equal(Token.methods.ping.resultFromPssz(new Uint8Array(0)), undefined);
  // A method without the Interface envelope.
  const functions = new SchemaBuilder();
  const namesType = functions.insertMethod(Token.methods.names);
  assert.equal(namesType.result!.value.type, 'BoundedList');
  assert.deepEqual(Token.methods.names.functionType(new SchemaBuilder()), namesType);
  assert.equal(functions.insertMethod(Token.methods.ping).result, null);
  assert.equal(functionTypeToJson(functions.insertMethod(Token.methods.ping), 'json').endsWith('"result":null}'), true);
  assert.deepEqual(functions.build().validate(), { ok: true });
  // One label per alternative; pairs for an integer label that is not first.
  const labelled = variant([['uint32', u32], ['1', utf8], ['2', utf8]]);
  assert.equal(labelled.valueToJson({ type: '2', value: 'b' }), '{"2":"b"}');
  assert.equal(variant({ uint32: u32, string: utf8, 'string[]': list(utf8) }).valueToJson({ type: 'string[]', value: ['a'] }), '{"string[]":["a"]}');
  // The static types.
  const decoded: { to: string; amount: bigint; memo: string | null } = transfer.args.decode(transfer.args.encode({ to: 'bob', amount: 5n, memo: null }));
  assert.deepEqual(decoded, { to: 'bob', amount: 5n, memo: null });
  const strings: string[] = Token.methods.names.result.decode(fromHex('0207616c696365626f62'));
  assert.deepEqual(strings, ['alice', 'bob']);
  // A result outside returns(...).
  const refused = (run: () => unknown) => assert.throws(run, (e: unknown) => e instanceof PsszError);
  refused(() => Token.methods.names.result.encodeChecked(['a', 'b', 'c', 'd', 'e']));
  refused(() => Token.methods.names.result.encodeChecked(['a'.repeat(17)]));
  assert.equal(Token.methods.names.result.validate(Uint8Array.of(1, 0xff)).ok, false);
  // The schema.
  const funcs = Token.schema.ir.interfaces[0].funcs;
  assert.deepEqual(funcs.map((f) => f.name), ['transfer', 'names', 'note', 'add', 'ping', 'batch']);
  assert.deepEqual(funcs[0].attributes, [{ name: 'maxFields', value: '8' }, { name: 'maxDynamicData', value: '256' }]);
  assert.deepEqual(funcs[3].params.map((p) => p.name), ['0', '1']);
  assert.ok(Token.schema.ir.types.some((m) => m.name === 'token.transfer'));
  const bare = defineInterface('token', { ping: method({}) }, { argumentLists: false, package: 'pkg', version: '1.0' });
  assert.deepEqual(bare.schema.ir.types, []);
  assert.deepEqual(bare.schema.ir.package, { name: 'pkg', version: '1.0', attributes: [] });
  const both = generateSchema({ Receipt }, { interfaces: [Token] });
  assert.equal(both.ir.interfaces.length, 1);
  assert.equal(toHex(both.encode()), toHex(Token.schema.encode()));

  // From a fetched schema.
  const schema = PsszSchema.decode(Token.schema.encode());
  assert.deepEqual(schema.validate(), { ok: true });
  const names = schema.interface('token').method('names');
  assert.deepEqual(names.params, ['limit']);
  assert.equal(names.named, true);
  assert.equal(toHex(names.args.jsonToPssz('{"limit":10}')), '040a000000');
  assert.deepEqual(names.args.decode(names.args.jsonToPssz('[10]')), { limit: 10 });
  assert.deepEqual(names.result!.decode(Uint8Array.of(2, 7, 0x61, 0x6c, 0x69, 0x63, 0x65, 0x62, 0x6f, 0x62)), ['alice', 'bob']);
  assert.deepEqual(schema.interface('token').method('add').args.decode(Uint8Array.of(8, 1, 0, 0, 0, 2, 0, 0, 0)), [1, 2]);
  assert.equal(toHex(schema.type('token.transfer').encode({ to: 'bob', amount: 5n, memo: 'hi' })), '0c000c0005000000000000000f00626f62016869');
  assert.equal(schema.interface().name, 'token');
  assert.throws(() => schema.interface('nope'), (e: unknown) => e instanceof PsszError && e.kind === 'schema');
  assert.throws(() => schema.interface('token').method('nope'), (e: unknown) => e instanceof PsszError && e.kind === 'schema');
  assert.equal(names.argsIr.value.type, 'Object');
});

test('README: compatibility checking', () => {
  const post = (owner: number, tag: number, extra = {}, cap = 1024) => record('Post', {
    id: u64,
    owner: field(rawString, { utf8: true, max: owner }),
    tags: field(list(rawString), { max: 8, element: { max: tag } }),
    ...extra,
  }, { maxDynamicData: cap });
  const V1 = post(32, 16);
  assert.deepEqual(psszEvolvesFrom(V1, post(64, 16)), { ok: true });
  assert.deepEqual(psszEvolvesFrom(V1, post(32, 16, { note: optional(rawString) })), { ok: true });
  assert.deepEqual(psszEvolvesFrom(V1, post(32, 16, {}, 4096)), { ok: true });
  assert.deepEqual(psszEvolvesFrom(V1, post(16, 16)),
    { ok: false, code: 'bound-tightened', reason: 'the length bound of the string was tightened ([0, 32] -> [0, 16])', path: '.owner' });
  const tags = psszEvolvesFrom(V1, post(32, 8));
  assert.ok(!tags.ok && tags.path === '.tags[]');
  assert.deepEqual(psszEvolvesFrom(V1, post(32, 16, { count: u64 })),
    { ok: false, code: 'appended-field-not-optional', reason: 'the appended field "count" is not optional (old data has no value for it)', path: '(root)' });
  assert.deepEqual(psszEvolvesFrom(V1, post(32, 16, {}, 100000)),
    { ok: false, code: 'width-changed', reason: 'the offset width of the record differs (2 / 4)', path: '(root)' });
  const fetched = (def: typeof V1) => PsszSchema.decode(generateSchema({ Post: def }).encode()).type('Post');
  assert.equal(psszEvolvesFrom(fetched(V1), fetched(post(64, 16))).ok, true);
  assert.equal(psszEvolvesFrom(fetched(V1), fetched(post(16, 16))).ok, false);
  assert.equal(psszEvolvesFrom(V1, fetched(post(64, 16))).ok, true);

  // Wire or contract; psszEquivalent.
  const A = record('P', { id: u64, name: optional(rawString) });
  const renamed = record('P', { id: u64, title: optional(rawString) });
  assert.deepEqual(psszEvolvesFrom(A, renamed),
    { ok: false, code: 'field-renamed', reason: 'field 1 was renamed or reordered ("name" -> "title")', path: '(root)' });
  assert.deepEqual(psszEvolvesFrom(A, renamed, { compare: 'wire' }), { ok: true });
  assert.deepEqual(psszEquivalent(V1, post(32, 16)), { ok: true });
  const wider = psszEquivalent(V1, post(64, 16));
  assert.ok(!wider.ok && wider.code === 'bound-changed' && wider.path === '.owner');
  assert.deepEqual(psszEquivalent(A, renamed, { compare: 'wire' }), { ok: true });
});

test('README: untagged variants and any JSON value', () => {
  const Extra = variant({ text: utf8, items: list(utf8) }, { untagged: true });
  assert.equal(Extra.valueToJson({ type: 'items', value: ['a'] }), '["a"]');
  assert.equal(variant({ text: utf8, items: list(utf8) }).valueToJson({ type: 'items', value: ['a'] }), '{"items":["a"]}');
  assert.equal(Extra.valueToJson({ type: 'text', value: 's' }, 'json'), '"s"');
  assert.deepEqual(Extra.jsonToValue('["a","b"]'), { type: 'items', value: ['a', 'b'] });
  assert.throws(() => Extra.jsonToValue('7'),
    (e: unknown) => e instanceof PsszError && e.message === "json: the value is none of the untagged variant's alternatives");
  assert.throws(() => variant({ a: u32, b: f64 }, { untagged: true }).jsonToValue('1'),
    (e: unknown) => e instanceof PsszError &&
      e.message === 'json: the alternatives of an untagged variant are not told apart by the kind of JSON value');

  const Action = record('Action', { service: utf8, data: jsonValue, more: optional(jsonValue) });
  const bytes = Action.jsonToPssz('{"service":"tokens","data":{"amount":"5","memo":[1.50,null]}}');
  assert.equal(Action.psszToJson(bytes), '{"service":"tokens","data":{"amount":"5","memo":[1.50,null]},"more":null}');
  assert.deepEqual(Action.decode(bytes).data, { amount: '5', memo: [1.5, null] });
  const Twin = record('Action', { service: utf8, data: utf8, more: optional(utf8) });
  assert.deepEqual(Action.encode({ service: 's', data: [true, { k: 1 }], more: null }),
    Twin.encode({ service: 's', data: '[true,{"k":1}]', more: null }));
  assert.deepEqual(Action.encode({ service: 's', data: new PsszJsonText('{ "n": 1.50 }'), more: null }),
    Twin.encode({ service: 's', data: '{"n":1.50}', more: null }));
});

test('README: names', () => {
  assert.equal(numberToName(nameToNumber('alice')), 'alice');
  assert.equal(nameToNumber('x--b'), 0n);
  assert.equal(numberToMethod(methodToNumber('transfer')), 'transfer');
  assert.equal(isHashName(methodToNumber('getSymbolType')), true);
  assert.equal(numberToMethod(methodToNumber('getSymbolType')), '#hmlnslheseoypnnp');
  assert.equal(isCompressedName(methodToNumber('transfer')), true);
});
