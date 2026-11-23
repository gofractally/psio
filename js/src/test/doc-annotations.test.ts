// The TypeScript snippets of docs/annotations.md and
// docs/tutorial-annotations.md. Mirrors cpp/tests/doc_annotations_examples.cpp
// (same values, same bytes).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  argumentTuple, boundedList, boundedUtf8, custom, field, list, optional, rawString, record, toHex, transparent, tuple,
  u32, u64, utf8, type Infer,
} from '../index.js';

// ── tut-1 ──
const AccountPlain = record('AccountPlain', { id: u64, owner: rawString, tags: list(rawString), balance: u64 });

// ── tut-2 ──
const accountFields = {
  id: u64,
  owner: field(rawString, { utf8: true, max: 32 }),                             // C++ utf8 | max<32>
  tags: field(list(rawString), { max: 8, element: { utf8: true, max: 16 } }),   // C++ max<8> | element(utf8 | max<16>)
  balance: u64,
};
const Account = record('Account', accountFields);
type Account = Infer<typeof Account>;   // { id: bigint; owner: string; tags: string[]; balance: bigint }

// ── tut-3 ──
const AccountV1 = record('AccountV1', accountFields, { maxDynamicData: 1024 });

// ── tut-4 ──
const AccountV2 = record('AccountV2', {
  ...accountFields,
  email: field(optional(rawString), { utf8: true, max: 64 }),                   // new: optional, last
}, { maxDynamicData: 1024 });

test('doc: widths', () => {
  assert.equal(AccountPlain.width, 4);
  assert.equal(Account.width, 1);
  assert.equal(AccountV1.width, 2);
  assert.equal(AccountV2.width, 2);
  // Without the cap the same field moves the width: not a compatible change.
  assert.equal(record('AccountGrown', { ...accountFields, email: field(optional(rawString), { utf8: true, max: 64 }) }).width, 2);
});

test('doc: a bounded record', () => {
  const alice: Account = { id: 7n, owner: 'alice', tags: ['vip', 'beta'], balance: 100n };
  const data = Account.encode(alice);
  // The bytes C++ and Rust write for the same declaration.
  assert.equal(toHex(data), '12070000000000000012176400000000000000616c696365080000000b00000076697062657461');
  assert.deepEqual(Account.decode(data), alice);
  assert.throws(() => Account.encode({ ...alice, owner: 'x'.repeat(33) }));
  assert.throws(() => Account.encode({ ...alice, tags: Array(9).fill('t') }));
});

test('doc: evolution', () => {
  const v1 = { id: 7n, owner: 'alice', tags: ['vip'], balance: 100n };
  assert.deepEqual(AccountV2.encode({ ...v1, email: null }), AccountV1.encode(v1));
  assert.deepEqual(AccountV2.decode(AccountV1.encode(v1)), { ...v1, email: null });
  const newer = AccountV2.encode({ ...v1, email: 'alice@example.com' });
  assert.deepEqual(AccountV1.decode(newer), v1);
  assert.deepEqual(AccountV1.validate(newer), { ok: true, hasUnknown: true });
  assert.throws(() => AccountV1.decode(newer, { rejectUnknown: true }));
});

test('doc: json forms', () => {
  const a = { id: 7n, owner: 'alice', tags: ['vip'], balance: 9007199254740993n, email: null };
  const data = AccountV2.encode(a);
  assert.equal(AccountV2.psszToJson(data, 'json'),
    '{"id":7,"owner":"alice","tags":["vip"],"balance":9007199254740993,"email":null}');
  assert.equal(AccountV2.psszToJson(data),
    '{"id":7,"owner":"alice","tags":["vip"],"balance":"9007199254740993","email":null}');
  const Totals = record('Totals', {
    supply: field(u64, { jsonInt: 'string', ijsonInt: 'string' }),
    holders: field(u64, { ijsonInt: 'number' }),
    decimals: u32,
  });
  assert.equal(Totals.valueToJson({ supply: 5n, holders: 5n, decimals: 4 }), '{"supply":"5","holders":5,"decimals":4}');
});

test('doc: keys', () => {
  assert.equal(toHex(u64.encodeKey(258n)), '0000000000000102');
  const ByOwner = tuple(utf8, u64);
  assert.equal(toHex(ByOwner.encodeKey(['bob', 7n])), '626f6200000000000000000007');
  assert.equal(toHex(ByOwner.encodeKeyPrefix(['bob'])), '626f620000');
});

test('doc: transparent, invariant, fixed records', () => {
  const AccountId = transparent('AccountId', u64);
  assert.deepEqual(AccountId.encode(42n), u64.encode(42n));
  const Symbol = custom('Symbol', record('Symbol', { code: field(rawString, { max: 8 }) }), {
    check: (s) => (/^[A-Z]*$/.test(new TextDecoder().decode(s.code)) ? undefined : 'symbol is not upper case'),
  });
  assert.throws(() => Symbol.encode({ code: new TextEncoder().encode('usd') }));
  const Point = record('Point', { x: u32, y: u32 }, { fixed: true });
  assert.equal(Point.encode({ x: 1, y: 2 }).length, 8);
});

test('doc: method arguments', () => {
  const parameters = { from: u64, to: u64, amount: u64, memo: field(rawString, { utf8: true, max: 80 }) };
  const Transfer = argumentTuple(parameters, { maxDynamicData: 1024 });
  const TransferV2 = argumentTuple({ ...parameters, fee: optional(u64) }, { maxDynamicData: 1024 });
  assert.equal(Transfer.width, 2);
  const args = Transfer.encode({ from: 1n, to: 2n, amount: 500n, memo: 'rent' });
  assert.equal(args.length, 2 + 8 + 8 + 8 + 2 + 4);
  assert.deepEqual(TransferV2.decode(args), { from: 1n, to: 2n, amount: 500n, memo: 'rent', fee: null });
  assert.throws(() => Transfer.encode({ from: 1n, to: 2n, amount: 500n, memo: 'm'.repeat(81) }));
});

test('doc: wrapper spellings are the same bytes', () => {
  const A = record('Tags', { tags: field(list(rawString), { max: 8, element: { utf8: true, max: 16 } }) });
  const W = record('Tags', { tags: boundedList(boundedUtf8(16), 8) });
  assert.deepEqual(A.encode({ tags: ['ab', 'c'] }), W.encode({ tags: ['ab', 'c'] }));
});
