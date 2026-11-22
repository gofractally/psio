// The examples of README.md, executed: what the README says a call returns
// is what it returns.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  PsszSchema, RecordView, boundedList, boundedUtf8, bytes, custom, emitWit, fixedBytes, fromHex, generateSchema, i32,
  i64, keyPrefixUpperBound, lazy, list, nameToNumber, numberToName, optional, pjsonDecode, pjsonEncode, pjsonValidate,
  PjsonObject, PsszError, map, nested, pair, record, set, toHex, transparent, tuple, u16, u32, u64, u8, utf8, timePoint,
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
  assert.equal(Small.type({ width: 2 }).width, 2);

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
  const AccountNumber = custom('AccountNumber', u64, { toText: numberToName, fromText: nameToNumber });
  const Balance = record('Balance', { owner: AccountNumber, amount: u64 });
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
  const AccountNumber = custom('AccountNumber', u64, { toText: numberToName, fromText: nameToNumber });
  const serviceSchema = PsszSchema.decode(
    generateSchema({ transfer: record('transfer', { to: AccountNumber, amount: u64, memo: utf8 }) }).encode(),
    { handlers: { AccountNumber: { toText: (v) => numberToName(v as bigint), fromText: nameToNumber } } },
  );
  const args = serviceSchema.type('transfer').jsonToPssz('{"to":"bob","amount":"2500000000000000000","memo":"rent"}');
  const Action = record('Action', { sender: AccountNumber, service: AccountNumber, method: utf8, rawData: bytes });
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
  const written = failure(() => Range.encode({ lo: 5, hi: 1 }));
  assert.deepEqual([written.kind, written.message, written.invariant], ['value', 'Range: lo is greater than hi', true]);
  const validated = Range.validate(reversedBytes);
  assert.ok(!validated.ok && validated.error.kind === 'bytes' && validated.error.message === 'Range: lo is greater than hi');
  const read = failure(() => Range.jsonToValue('{"lo":5,"hi":1}'));
  assert.deepEqual([read.kind, read.message], ['json', 'Range: lo is greater than hi']);
  // A view reads in place and runs no check; decoding through it does.
  assert.equal(Range.view(reversedBytes).get('lo'), 5);
  assert.equal(failure(() => Range.view(reversedBytes).toValue()).message, 'Range: lo is greater than hi');
  assert.equal(failure(() => DerKey.encode(new Uint8Array(0))).message, 'DerKey: not a DER sequence');
  assert.equal(toHex(DerKey.encode(fromHex('3000'))), '3000');
  assert.equal(failure(() => DerKey.encodeKey(fromHex('31'))).message, 'DerKey: not a DER sequence');
});
