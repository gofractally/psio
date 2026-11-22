// A small benchmark, reported and never failing on timing: encode, decode
// and view of a typical record and of a list of 1,000 records, through the
// typed layer and through the schema-driven path (a schema fetched as
// bytes). Both run the same codec; the numbers make a regression visible.
//
//   node --test dist/test/bench.test.js

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  PsszSchema, bool, list, optional, record, u32, u64, utf8, type Infer, type ListView, type PsszType, type RecordView,
} from '../index.js';

const Account = record('Account', {
  id: u64, name: utf8, email: optional(utf8), balance: u64, active: bool, tags: list(utf8), logins: u32,
});
const Accounts = list(Account);
type Account = Infer<typeof Account>;

const one = (i: number): Account => ({
  id: BigInt(1000 + i), name: `account-${i}`, email: i % 3 ? `user${i}@example.com` : null,
  balance: BigInt(i) * 1000003n, active: i % 2 === 0, tags: ['alpha', 'beta', `t${i % 7}`], logins: i * 3,
});
const single = one(7);
const many = Array.from({ length: 1000 }, (_, i) => one(i));

// Median ns per operation over several batches.
function measure(run: () => unknown, iterations: number): number {
  for (let i = 0; i < Math.max(3, iterations >> 3); ++i) run();
  const samples: number[] = [];
  for (let batch = 0; batch < 5; ++batch) {
    const start = process.hrtime.bigint();
    for (let i = 0; i < iterations; ++i) run();
    samples.push(Number(process.hrtime.bigint() - start) / iterations);
  }
  return samples.sort((a, b) => a - b)[2];
}
const fmt = (ns: number) => (ns >= 1e6 ? `${(ns / 1e6).toFixed(2)} ms` : ns >= 1e3 ? `${(ns / 1e3).toFixed(2)} µs` : `${ns.toFixed(0)} ns`);

test('benchmark: encode / decode / view, typed and schema-driven (reported)', () => {
  // The schema-driven path: the same types from a schema that travelled.
  const fetched = PsszSchema.decode(Accounts.schema.encode());
  const paths: [string, PsszType, PsszType][] = [
    ['typed', Account as unknown as PsszType, Accounts as unknown as PsszType],
    ['schema-driven', fetched.type('Account'), fetched.type(Accounts.name)],
  ];
  const oneBytes = Account.encode(single);
  const manyBytes = Accounts.encode(many);
  const rows: string[] = [];
  let sink = 0n;
  for (const [path, rec, recs] of paths) {
    // Both paths give the same bytes and values.
    assert.deepEqual(rec.encode(single), oneBytes);
    assert.deepEqual(recs.decode(manyBytes), many);
    const results: [string, number][] = [
      ['record encode', measure(() => rec.encode(single), 2000)],
      ['record decode', measure(() => rec.decode(oneBytes), 2000)],
      ['record validate', measure(() => rec.validate(oneBytes), 2000)],
      ['record view, 1 field', measure(() => { sink += (rec.view(oneBytes) as RecordView).get('balance') as bigint; }, 2000)],
      ['record to I-JSON', measure(() => rec.psszToJson(oneBytes), 1000)],
      ['list[1000] encode', measure(() => recs.encode(many), 5)],
      ['list[1000] decode', measure(() => recs.decode(manyBytes), 5)],
      ['list[1000] view, 1 field of 1 element', measure(() => {
        sink += ((recs.view(manyBytes) as ListView).at(500) as RecordView).get('balance') as bigint;
      }, 2000)],
      ['list[1000] view, 1 field of every element', measure(() => {
        for (const row of recs.view(manyBytes) as ListView<unknown, RecordView>) sink += row.get('balance') as bigint;
      }, 20)],
    ];
    for (const [name, ns] of results) rows.push(`${path.padEnd(14)} ${name.padEnd(42)} ${fmt(ns).padStart(10)}`);
  }
  assert.ok(sink > 0n);
  console.log(
    `benchmark (record ${oneBytes.length} bytes, list of 1000 = ${manyBytes.length} bytes; median per operation):\n` +
      rows.join('\n'),
  );
});
