// A lazy view on bytes that are not a frame of its type (gofractally/psio#15;
// docs/annotations.md 7.2 "On bytes that are not a frame"): a member that
// cannot be placed is a PsszError of kind 'bytes' when it is read — never an
// empty or zero member, never a read outside the bytes. These are the cases
// of the C++ test (cpp/tests/trusted_structure_tests.cpp).
import test from 'node:test';
import assert from 'node:assert/strict';
import {
  PsszError, bool, list, nested, optional, record, u32, u64, utf8, type ListView, type RecordView,
} from '../index.js';

const Wide = record('VsWide', { id: u32, a: utf8, b: list(u64), c: optional(utf8) });
const Old = record('VsOld', { id: u32, a: utf8 });
const Fixed = record('VsFixed', { x: u64, y: u64, z: u32 });
const Rows = record('VsRows', { id: u32, rows: list(Wide), names: list(utf8) });

const walk = (v: unknown): number => {
  const view = v as RecordView;
  const b = view.get('b') as ListView;
  let sum = Number(view.get('id')) + (view.get('a') as string).length + b.length;
  for (const x of b) sum += Number(x);
  const c = view.get('c');
  if (typeof c === 'string') sum += c.length;
  return sum;
};
const fails = (run: () => unknown, name: string): void => {
  assert.throws(run, (e: unknown) => e instanceof PsszError && e.kind === 'bytes', name);
};
const withBytes = (bytes: Uint8Array, edits: [number, number][]): Uint8Array => {
  const out = bytes.slice();
  for (const [at, value] of edits) out[at] = value;
  return out;
};

test('view: a member that cannot be placed is an error, not an empty member', () => {
  const wide = Wide.encode({ id: 7, a: 'hello', b: [1n, 2n, 3n], c: 'opt' });
  assert.equal(walk(Wide.view(wide)), 7 + 5 + 3 + 6 + 3);
  const cases: [string, Uint8Array][] = [
    ['cut to 10 bytes', wide.slice(0, 10)],
    ['the first offset 0xffff', withBytes(wide, [[8, 0xff], [9, 0xff]])],
    ['no bytes', new Uint8Array(0)],
    ['a header larger than the buffer', withBytes(wide, [[0, 0xff]])],
    ['offsets out of order', withBytes(wide, [[12, 5]])],
    ['the payload cut', wide.slice(0, wide.length - 9)],
    ['a list span that is not a multiple of its element', withBytes(wide, [[16, 0x2a]])],
    ['a row of an older type: a required member is missing', Old.encode({ id: 7, a: 'hello' })],
  ];
  for (const [name, bytes] of cases) {
    assert.equal(Wide.validate(bytes).ok, false, name);
    fails(() => walk(Wide.view(bytes)), name);
    fails(() => Wide.decode(bytes), name);
  }
  const fixed = Fixed.encode({ x: 1n, y: 2n, z: 3 });
  fails(() => (Fixed.view(fixed.slice(0, 10)) as RecordView).get('y'), 'a fixed record cut');
});

test('view: every truncation and single-byte corruption reads inside the bytes or fails', () => {
  const good = Rows.encode({
    id: 1,
    rows: [{ id: 7, a: 'hello', b: [1n, 2n, 3n], c: 'opt' }, { id: 8, a: '', b: [], c: null }],
    names: ['one', '', 'three'],
  });
  const walkRows = (bytes: Uint8Array): void => {
    const view = Rows.view(bytes) as RecordView;
    void view.get('id');
    const rows = view.get('rows') as ListView<unknown, RecordView>;
    // A list never announces more elements than its bytes can hold.
    assert.ok(rows.length <= bytes.length);
    for (const row of rows) walk(row);
    const names = view.get('names') as ListView<unknown, string>;
    assert.ok(names.length <= bytes.length);
    for (const name of names) void name.length;
  };
  walkRows(good);
  let failed = 0;
  let read = 0;
  const probe = (bytes: Uint8Array): void => {
    // In an ArrayBuffer of its own: a read outside is `undefined` or a
    // RangeError from the DataView, which is not a PsszError.
    const exact = new Uint8Array(bytes.length);
    exact.set(bytes);
    try {
      walkRows(exact);
      ++read;
    } catch (e) {
      assert.ok(e instanceof PsszError && e.kind === 'bytes', `${e}`);
      ++failed;
      // What a view refuses, validation refuses.
      assert.equal(Rows.validate(exact).ok, false);
    }
  };
  for (let n = 0; n < good.length; ++n) probe(good.slice(0, n));
  for (let i = 0; i < good.length; ++i)
    for (const value of [0, 0xff, (good[i] + 1) & 0xff, (good[i] - 1) & 0xff]) probe(withBytes(good, [[i, value]]));
  for (let i = 0; i + 1 < good.length; ++i) probe(withBytes(good, [[i, 0xff], [i + 1, 0xff]]));
  assert.ok(failed > 100, `failed ${failed}`);
  assert.ok(read > 0, `read ${read}`);
});

test('validating an outer value validates a nested member as its type', () => {
  // What a node relies on when it reads a message off the network: the
  // validating read of the outer value validates a `nested(T)` member as a
  // T. `nested(T, { deferred: true })` is the documented exception.
  const Header = record('VsHeader', { num: u32, producer: utf8, xs: list(u64) });
  const Block = record('VsBlock', { header: Header, trx: list(utf8), sig: optional(utf8), final: bool });
  const block = (n: number) => ({ header: { num: n, producer: 'prod', xs: [1n, 2n, 3n] }, trx: ['a', 'bb'], sig: 'sig', final: true });
  const inner = Block.encode(block(7));
  const find = (outer: Uint8Array): number => {
    for (let at = 0; at + inner.length <= outer.length; ++at)
      if (inner.every((byte, i) => outer[at + i] === byte)) return at;
    throw new Error('the member bytes are not in the outer frame');
  };
  const mutations = (Outer: { validate(b: Uint8Array): { ok: boolean }; decode(b: Uint8Array): unknown }, good: Uint8Array, validated: boolean) => {
    const pos = find(good);
    assert.ok(Outer.validate(good).ok);
    let tried = 0;
    let accepted = 0;
    let memberRefused = 0;
    let seed = pos * 1000003 + inner.length;
    const random = (): number => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) >> 16 & 0xff;
    for (let i = 0; i < inner.length; ++i)
      for (const v of [0xff, 0x00, 0x7f, random(), random(), random()]) {
        if (good[pos + i] === v) continue;
        const bad = good.slice();
        bad[pos + i] = v;
        ++tried;
        const innerValid = Block.validate(bad.slice(pos, pos + inner.length)).ok;
        const outerValid = Outer.validate(bad).ok;
        let decoded = true;
        try {
          Outer.decode(bad);
        } catch (e) {
          assert.ok(e instanceof PsszError, `${e}`);
          decoded = false;
        }
        if (validated) assert.equal(outerValid, innerValid, `byte ${i} set to ${v}`);
        else assert.ok(outerValid, `deferred: byte ${i} set to ${v}`);
        assert.equal(decoded, outerValid, `byte ${i} set to ${v}`);
        if (outerValid) ++accepted;
        if (!innerValid) ++memberRefused;
      }
    return { tried, accepted, memberRefused };
  };
  const InNested = record('VsInNested', { kind: u32, packed: nested(Block), after: utf8 });
  const InOptional = record('VsInOptional', { kind: u32, packed: optional(nested(Block)), after: utf8 });
  const InList = record('VsInList', { kind: u32, packed: list(nested(Block)), after: utf8 });
  for (const t of [
    mutations(InNested, InNested.encode({ kind: 1, packed: block(7), after: 'z' }), true),
    mutations(InOptional, InOptional.encode({ kind: 1, packed: block(7), after: 'z' }), true),
    mutations(InList, InList.encode({ kind: 1, packed: [block(1), block(7), block(3)], after: 'z' }), true),
  ]) {
    assert.ok(t.tried > 150, `${t.tried}`);
    assert.ok(t.accepted > 0 && t.memberRefused > 0 && t.accepted + t.memberRefused === t.tried, JSON.stringify(t));
  }
  // The exception: a deferred member's bytes are not looked at.
  const InDeferred = record('VsInDeferred', { kind: u32, packed: nested(Block, { deferred: true }), after: utf8 });
  const d = mutations(InDeferred, InDeferred.encode({ kind: 1, packed: inner, after: 'z' }), false);
  assert.equal(d.accepted, d.tried);
  assert.ok(d.memberRefused > 0);
});
