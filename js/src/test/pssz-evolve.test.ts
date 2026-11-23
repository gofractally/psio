// The schema-upgrade check `psszEvolvesFrom`: the cases of C++
// `pssz_evolves_from` in cpp/tests/consistency_tests.cpp ("member
// annotations and caps"), declared here in the typed layer and — for what
// the Schema IR carries — replayed on the schemas as they travel.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  PsszError, PsszSchema, argumentTuple, array, boundedList, boundedUtf8, defineInterface, field, generateSchema, lazy, list, map,
  fixedBytes, i32, method, nested, optional, psszEvolvesFrom, rawString, record, set, transparent, tuple, u16, u32, u64, u8, utf8, variant,
  type FieldOptions, type PsszCompatibility, type RecordOptions, type TypeDef,
} from '../index.js';

// CONS_EVO(NAME, OWNER, TAGS, ...): { id: u64, owner: string, tags: list<string> }.
const evo = (owner: FieldOptions, tags: FieldOptions, options: RecordOptions = { maxDynamicData: 1024 }) =>
  record('Evo', { id: u64, owner: field(rawString, owner), tags: field(list(rawString), tags) }, options);
const TAGS = { max: 8, element: { max: 16 } };
const V1 = evo({ utf8: true, max: 32 }, TAGS);
const Same = evo({ utf8: true, max: 32 }, TAGS);
const Wider = evo({ utf8: true, max: 64 }, TAGS);
const Tighter = evo({ utf8: true, max: 16 }, TAGS);
const Raw = evo({ max: 32 }, TAGS);
const ElemWider = evo({ utf8: true, max: 32 }, { max: 8, element: { max: 20 } });
const ElemTighter = evo({ utf8: true, max: 32 }, { max: 8, element: { max: 8 } });
const ElemUtf8 = evo({ utf8: true, max: 32 }, { max: 8, element: { utf8: true, max: 16 } });
const CountWider = evo({ utf8: true, max: 32 }, { max: 12, element: { max: 16 } });
const WithMin = evo({ utf8: true, lengthBound: { max: 32, min: 1 } }, TAGS);
const Unbounded = evo({ utf8: true }, TAGS);
const Sorted = evo({ utf8: true, max: 32 }, { ...TAGS, sorted: true });
const CapWider = evo({ utf8: true, max: 32 }, TAGS, { maxDynamicData: 4096 });
const CapTighter = evo({ utf8: true, max: 32 }, TAGS, { maxDynamicData: 512 });
const CapOtherWidth = evo({ utf8: true, max: 32 }, TAGS, { maxDynamicData: 100000 });
const Open = evo({ utf8: true, max: 300 }, TAGS, {});
const OpenWideList = evo({ utf8: true, max: 300 }, { max: 8, element: { max: 40 } }, {});
const OpenWider = evo({ utf8: true, max: 400 }, TAGS, {});
const OpenCapped = evo({ utf8: true, max: 300 }, TAGS, { maxDynamicData: 600 });
const OpenCappedLow = evo({ utf8: true, max: 300 }, TAGS, { maxDynamicData: 400 });

const fields3 = (n: number, options: RecordOptions) =>
  record('Fields3', Object.fromEntries([['a', u32], ...['b', 'c', 'd'].slice(0, n - 1).map((k) => [k, optional(u32)])]), options);
const Fields3 = fields3(2, { maxFields: 3, maxDynamicData: 64 });
const Fields3Grown = fields3(3, { maxFields: 3, maxDynamicData: 64 });
const Fields3Raised = fields3(4, { maxFields: 8, maxDynamicData: 64 });
const Fields3Dropped = fields3(3, { maxDynamicData: 64 });
const Fields3Lowered = fields3(2, { maxFields: 2, maxDynamicData: 64 });

const yes = (a: TypeDef<any, any>, b: TypeDef<any, any>) => assert.deepEqual(psszEvolvesFrom(a, b), { ok: true });
function no(a: TypeDef<any, any>, b: TypeDef<any, any>, reason: RegExp, path?: string): void {
  const result: PsszCompatibility = psszEvolvesFrom(a, b);
  assert.ok(!result.ok, 'compatible');
  assert.match(result.reason, reason);
  if (path !== undefined) assert.equal(result.path, path);
}
// The same two types as a reader that fetched the two schemas has them.
const fetched = (def: TypeDef<any, any>) => PsszSchema.decode(generateSchema({ Root: def }).encode()).type('Root');

test('psszEvolvesFrom: the C++ pssz_evolves_from cases (member annotations and caps)', () => {
  yes(V1, Same);
  // A bound may only widen, and only while every width stays the same.
  yes(V1, Wider);
  no(V1, Tighter, /length bound of the string was tightened \(\[0, 32\] -> \[0, 16\]\)/, '.owner');
  no(Wider, V1, /tightened/, '.owner');
  yes(V1, ElemWider);
  no(V1, ElemTighter, /tightened/, '.tags[]');
  yes(V1, CountWider);
  no(CountWider, V1, /length bound of the list was tightened/, '.tags');
  yes(V1, Unbounded);            // under the cap the widths are the cap's
  no(Unbounded, V1, /tightened \(\[0, unbounded\] -> \[0, 32\]\)/, '.owner');
  no(V1, WithMin, /tightened \(\[0, 32\] -> \[1, 32\]\)/, '.owner');   // a new lower bound refuses old values
  yes(WithMin, V1);
  // utf8 / raw and sorted are the member's type.
  no(V1, Raw, /utf8 changed/, '.owner');
  no(Raw, V1, /utf8 changed/, '.owner');
  no(V1, ElemUtf8, /utf8 changed/, '.tags[]');
  no(V1, Sorted, /sorted \/ unique \/ uniqueKeys changed/, '.tags');
  no(Sorted, V1, /sorted \/ unique \/ uniqueKeys changed/, '.tags');
  // The cap may grow within its width class and may not shrink.
  yes(V1, CapWider);
  no(V1, CapTighter, /maxDynamicData was lowered \(1024 -> 512\)/, '(root)');
  no(V1, CapOtherWidth, /offset width of the record changed \(2 -> 4\)/, '(root)');
  // Adding a cap: only one the old type's inferred bound fits.
  assert.ok(Open.inferredBound! <= 600n && Open.inferredBound! > 400n);
  yes(Open, OpenCapped);
  no(Open, OpenCappedLow, /below the bound inferred for the old record/, '(root)');
  yes(OpenCapped, Open);         // dropping it: nothing old is refused
  // Without a cap, a widened bound that moves a nested list's offset table
  // into another width class changes the bytes.
  assert.equal(Open.width, OpenWideList.width);
  no(Open, OpenWideList, /offset width of the list changed \(1 -> 2\)/, '.tags');
  yes(Open, OpenWider);

  // What the check promises, on bytes: every record of V1 is a record of
  // Wider, and a V1 record at its bound is not a record of Tighter.
  const frame = V1.encode({ id: 0n, owner: 'a'.repeat(32), tags: [new Uint8Array(16).fill(0x62)] } as never);
  assert.ok(Wider.validate(frame).ok);
  assert.ok(CapWider.validate(frame).ok);
  assert.ok(!Tighter.validate(frame).ok);
  assert.ok(!ElemTighter.validate(frame).ok);

  // maxFields: a revision has no more fields than the original's cap and
  // does not raise or drop it.
  yes(Fields3, Fields3Grown);
  no(Fields3, Fields3Raised, /more fields \(4\) than the original's maxFields \(3\)/);
  no(Fields3, Fields3Dropped, /maxFields \(3\) was dropped/);
  yes(Fields3, Fields3Lowered);
  no(Fields3Lowered, Fields3Grown, /more fields \(3\) than the original's maxFields \(2\)/);
  no(fields3(2, { maxFields: 3, maxDynamicData: 64 }), fields3(2, { maxFields: 4, maxDynamicData: 64 }), /maxFields was raised \(3 -> 4\)/);
});

test('psszEvolvesFrom: two fetched schemas give the same answers (what the Schema IR carries)', () => {
  const pairs: [TypeDef<any, any>, TypeDef<any, any>, boolean][] = [
    [V1, Same, true], [V1, Wider, true], [V1, Tighter, false], [Wider, V1, false], [V1, ElemWider, true],
    [V1, ElemTighter, false], [V1, CountWider, true], [CountWider, V1, false], [V1, Unbounded, true],
    [Unbounded, V1, false], [V1, WithMin, false], [WithMin, V1, true], [V1, Raw, false], [Raw, V1, false],
    [V1, ElemUtf8, false], [V1, CapWider, true], [V1, CapTighter, false], [V1, CapOtherWidth, false],
    [Open, OpenCapped, true], [Open, OpenCappedLow, false], [OpenCapped, Open, true], [Open, OpenWideList, false],
    [Open, OpenWider, true], [Fields3, Fields3Grown, true], [Fields3, Fields3Raised, false],
    [Fields3, Fields3Dropped, false], [Fields3, Fields3Lowered, true], [Fields3Lowered, Fields3Grown, false],
  ];
  for (const [a, b, ok] of pairs) {
    assert.equal(psszEvolvesFrom(fetched(a), fetched(b)).ok, ok);
    // (a typed definition against a fetched schema, too)
    assert.equal(psszEvolvesFrom(a, fetched(b)).ok, ok);
  }
  // sorted / unique are typed-only: the Schema IR of the two is the same.
  assert.equal(Buffer.from(generateSchema({ Root: V1 }).encode()).toString('hex'), Buffer.from(generateSchema({ Root: Sorted }).encode()).toString('hex'));
  assert.equal(psszEvolvesFrom(fetched(V1), fetched(Sorted)).ok, true);
});

test('psszEvolvesFrom: the rules of docs/pssz-spec.md 7.5 / 7.6', () => {
  const A = record('A', { id: u32, name: utf8 });
  yes(A, record('A', { id: u32, name: utf8, memo: optional(utf8) }));
  no(A, record('A', { id: u32, name: utf8, count: u32 }), /appended field "count" is not optional/);
  no(A, record('A', { id: u32 }), /fields were removed/);
  no(A, record('A', { name: utf8, id: u32 }), /renamed or reordered/);
  no(A, record('A', { id: u32, title: utf8 }), /field 1 was renamed or reordered \("name" -> "title"\)/);
  no(A, record('A', { id: u64, name: utf8 }), /integer type changed/, '.id');
  no(A, record('A', { id: optional(u32), name: utf8 }), /type changed \(int -> option\)/, '.id');
  no(A, record('A', { id: u32, name: utf8 }, { fixed: true }), /definitionWillNotChange was added or removed/);
  const F = record('F', { a: utf8 }, { fixed: true });
  yes(F, record('F', { a: utf8 }, { fixed: true }));
  no(F, record('F', { a: utf8, b: optional(u32) }, { fixed: true }), /definitionWillNotChange record gained fields/);
  // An appended field that moves the inferred bound into the next width class changes every byte.
  const Small = record('S', { a: boundedUtf8(200) });
  assert.equal(Small.width, 1);
  no(Small, record('S', { a: boundedUtf8(200), b: optional(boundedUtf8(200)) }), /offset width of the record changed \(1 -> 2\)/);
  yes(record('S', { a: boundedUtf8(200) }, { maxDynamicData: 1000 }),
    record('S', { a: boundedUtf8(200), b: optional(boundedUtf8(200)) }, { maxDynamicData: 1000 }));
  no(record('S', { a: utf8 }), record('S', { a: utf8 }, { maxDynamicData: 1 << 20 }), /added to a record of unbounded size/);
  // A variant gains alternatives at the end.
  const V = variant({ A: u32, B: utf8 });
  yes(V, variant({ A: u32, B: utf8, C: u8 }));
  no(V, variant({ B: utf8, A: u32 }), /alternative 0 was renamed or reordered/);
  no(V, variant({ A: u32 }), /alternatives were removed/);
  no(V, variant({ A: u32, B: rawString }), /utf8 changed/, '<B>');
  // The inner type of a nested value, an optional, a list, a map evolves by the same rules.
  const grown = record('A', { id: u32, name: utf8, memo: optional(utf8) });
  yes(nested(A), nested(grown));
  no(nested(grown), nested(A), /fields were removed/, '(nested)');
  yes(list(A), list(grown));
  yes(optional(A), optional(grown));
  yes(map(u32, A), map(u32, grown));
  no(map(u32, A), map(u64, A), /integer type changed/, '[].0');
  no(map(u32, A), list(tuple(u32, A)), /a list, a map and a set are different types/);
  no(set(u32), list(u32), /a list, a map and a set are different types/);
  no(list(u32), array(u32, 4), /type changed \(list -> array\)/);
  no(array(u32, 4), array(u32, 5), /tightened/);
  yes(boundedList(u32, 4), list(u32));
  no(list(u32), boundedList(u32, 4), /tightened/);
  // A tuple gains optional trailing elements while its width stays.
  yes(tuple(u32, boundedUtf8(8)), tuple(u32, boundedUtf8(8), optional(u8)));
  no(tuple(u32, boundedUtf8(8)), tuple(u32, boundedUtf8(8), u8), /appended element 2 is not optional/);
  // A transparent type is its member; a recursive type terminates.
  yes(transparent('Memo', boundedUtf8(80)), transparent('Memo', field(rawString, { utf8: true, max: 120 })));
  no(transparent('Memo', field(rawString, { utf8: true, max: 80 })), transparent('Memo', field(rawString, { max: 80 })), /utf8 changed/);
  const tree = (extra: boolean): TypeDef<any, any> => {
    const self: TypeDef<any, any> = record('Tree', {
      children: list(lazy(() => self)), ...(extra ? { note: optional(utf8) } : {}),
    });
    return self;
  };
  yes(tree(false), tree(true));
  no(tree(true), tree(false), /fields were removed/);
  // Argument lists and results.
  const args = (extra: boolean, cap?: number) => argumentTuple(
    { to: field(rawString, { utf8: true, max: 16 }), amount: u64, ...(extra ? { memo: optional(boundedUtf8(250)) } : {}) },
    cap === undefined ? {} : { maxDynamicData: cap });
  yes(args(false, 512), args(true, 512));
  no(args(false), args(true), /offset width of the record changed \(1 -> 2\)/);   // no cap: the added parameter moves the width
  const api = (max: number) => defineInterface('api', { names: method({}, { returns: field(list(rawString), { max, element: { utf8: true, max: 16 } }) }) });
  assert.deepEqual(psszEvolvesFrom(api(4).methods.names.result, api(8).methods.names.result), { ok: true });
  assert.equal(psszEvolvesFrom(api(8).methods.names.result, api(4).methods.names.result).ok, false);
  const widened = psszEvolvesFrom(api(4).methods.names.result, api(64).methods.names.result);
  assert.ok(!widened.ok && /offset width of the list changed \(1 -> 2\)/.test(widened.reason));
  // Not a type; not the automatic profile.
  assert.throws(() => psszEvolvesFrom({} as never, A), (e: unknown) => e instanceof PsszError && e.kind === 'schema');
  assert.throws(() => psszEvolvesFrom(A.type({ width: 2 }), A), (e: unknown) => e instanceof PsszError && e.kind === 'schema');
});

// The variant cases of cpp/tests/pssz_evolution_matrix_tests.cpp
// (docs/pssz-spec.md 7.5): alternatives may be appended; a larger new
// alternative is refused exactly when it moves an enclosing width. Each
// pair is checked as declared and as two fetched schemas.
test('psszEvolvesFrom: a variant may gain alternatives at the end, unless an enclosing width moves', () => {
  const both = (a: TypeDef<any, any>, b: TypeDef<any, any>, reason?: RegExp, path?: string) => {
    for (const [x, y] of [[a, b], [fetched(a), fetched(b)]] as const) {
      const result = psszEvolvesFrom(x, y);
      if (reason === undefined) assert.deepEqual(result, { ok: true });
      else {
        assert.ok(!result.ok, 'compatible');
        assert.match(result.reason, reason);
        if (path !== undefined) assert.equal(result.path, path);
      }
    }
  };
  const Alt0 = record('Alt', { a: u32 }, { maxDynamicData: 64 });
  const Alt1 = record('Alt', { a: u32, b: optional(u32) }, { maxDynamicData: 64 });
  const AltOther = record('Alt', { z: u64 }, { maxDynamicData: 64 });
  const Var1 = variant({ A: u32, B: utf8 });
  const Var2 = variant({ A: u32, B: utf8, C: u64 });
  const Var3 = variant({ A: u32, B: utf8, C: u64, D: Alt0 });
  // Appended, once and twice; the relation is transitive.
  both(Var1, Var2);
  both(Var2, Var3);
  both(Var1, Var3);
  both(Var1, Var1);
  // Removed, reordered, inserted, retyped: not compatible.
  both(Var2, Var1, /alternatives were removed/);
  both(Var1, variant({ B: utf8, A: u32, C: u64 }), /alternative 0 was renamed or reordered/);
  both(Var1, variant({ A: u32, C: u64, B: utf8 }), /alternative 1 was renamed or reordered/);
  both(Var1, variant({ C: u64, A: u32, B: utf8 }), /alternative 0 was renamed or reordered/);
  both(Var1, variant({ A: u64, B: utf8, C: u32 }), /the integer type changed/, '<A>');
  both(Var1, variant({ A: i32, B: utf8, C: u64 }), /the integer type changed/, '<A>');
  // The same alternatives in the same places under other names, with other
  // types: an inserted alternative is seen by its type too.
  both(Var1, variant({ A: u32, B: u64, C: utf8 }), /the type changed/, '<B>');
  // An existing alternative evolves by the rules of its own type, with or
  // without new alternatives after it.
  both(variant({ A: u32, B: Alt0 }), variant({ A: u32, B: Alt1 }));
  both(variant({ A: u32, B: Alt0 }), variant({ A: u32, B: Alt1, C: u64 }));
  both(variant({ A: u32, B: Alt1 }), variant({ A: u32, B: Alt0, C: u64 }), /fields were removed/, '<B>');
  both(variant({ A: u32, B: Alt0 }), variant({ A: u32, B: AltOther, C: u64 }), /field 0 was renamed or reordered/, '<B>');
  // Wherever a variant is: an optional, a list, a map, a variant, a record.
  both(optional(Var1), optional(Var2));
  both(list(Var1), list(Var2));
  both(map(u32, Var1), map(u32, Var2));
  both(variant({ X: u8, Y: Var1 }), variant({ X: u8, Y: Var2, Z: u16 }));
  both(list(Var2), list(Var1), /alternatives were removed/, '[]');

  // A larger new alternative. The variant alone writes no offsets:
  // appending is compatible whatever the size of the new alternative.
  const Small = variant({ A: u8, B: u16 });
  const Same = variant({ A: u8, B: u16, C: u8 });      // no larger than the others
  const Larger = variant({ A: u8, B: u16, C: u32 });   // raises the bound by 2
  both(Small, Same);
  both(Small, Larger);
  both(variant({ A: u8 }), variant({ A: u8, B: fixedBytes(300) }));
  // A record with no cap: header 1 + slot 1 + (1 + 2) + 249 = 254, one byte
  // wide. Refused exactly when its width moves, as for an appended field.
  const tight = (v: TypeDef<any, any>) => record('Tight', { v, pad: fixedBytes(249) });
  const capped = (v: TypeDef<any, any>) => record('Capped', { v, pad: fixedBytes(249) }, { maxDynamicData: 1000 });
  assert.deepEqual([tight(Small).inferredBound, tight(Small).width], [254n, 1]);
  assert.deepEqual([tight(Same).inferredBound, tight(Same).width], [254n, 1]);
  assert.deepEqual([tight(Larger).inferredBound, tight(Larger).width], [258n, 2]);
  assert.deepEqual([capped(Small).width, capped(Larger).width], [2, 2]);
  both(tight(Small), tight(Same));
  both(tight(Small), tight(Larger), /offset width of the record changed \(1 -> 2\)/, '(root)');
  // A cap reserves the room: the same change is compatible.
  both(capped(Small), capped(Larger));
  both(capped(Small), capped(Same));
  // A tuple and an array take their width from their own bound.
  both(tuple(Small, fixedBytes(249)), tuple(Same, fixedBytes(249)));
  both(tuple(Small, fixedBytes(249)), tuple(Larger, fixedBytes(249)), /offset width of the tuple changed \(1 -> 2\)/);
  both(array(Small, 60), array(Same, 60));
  both(array(Small, 60), array(Larger, 60), /offset width of the list changed \(1 -> 2\)/);
  both(boundedList(Small, 60), boundedList(Larger, 60), /offset width of the list changed \(1 -> 2\)/);
  both(map(u8, Small, { max: 30 }), map(u8, Same, { max: 30 }));
  both(map(u8, Small, { max: 30 }), map(u8, Larger, { max: 30 }), /offset width of the map changed \(1 -> 2\)/);
  // A bounded list inside a capped record: the record's width stays, the
  // list's table does not (60 x (1 + 3) = 240; 60 x (2 + 5) = 420).
  const listed = (v: TypeDef<any, any>) => record('Listed', { items: field(list(v), { max: 60 }) }, { maxDynamicData: 60000 });
  assert.deepEqual([listed(Small).width, listed(Larger).width], [2, 2]);
  assert.deepEqual([boundedList(Small, 60).width, boundedList(Larger, 60).width], [1, 2]);
  both(listed(Small), listed(Same));
  both(listed(Small), listed(Larger), /offset width of the list changed \(1 -> 2\)/, '.items');
  // ... and one level further down: a tuple holding the variant, in a list.
  const rows = (v: TypeDef<any, any>) => record('Rows', { rows: field(list(tuple(v, fixedBytes(249))), { max: 2 }) }, { maxDynamicData: 60000 });
  both(rows(Small), rows(Same));
  both(rows(Small), rows(Larger), /offset width of the (list|tuple) changed/);

  // Bounds on the alternatives (C++ at<I>(...)).
  const ann = (alternatives: { [name: string]: TypeDef<any, any> }, at: { [name: string]: FieldOptions }) =>
    record('Ann', { v: field(variant(alternatives), { at }) }, { maxDynamicData: 200 });
  const Ann0 = ann({ A: u32, B: rawString }, { B: { max: 8 } });
  const Ann1 = ann({ A: u32, B: rawString, C: rawString }, { B: { max: 8 }, C: { max: 4 } });
  const AnnTighter = ann({ A: u32, B: rawString, C: rawString }, { B: { max: 4 }, C: { max: 4 } });
  const AnnWider = ann({ A: u32, B: rawString, C: rawString }, { B: { max: 16 }, C: { max: 4 } });
  both(Ann0, Ann1);
  both(Ann0, AnnWider);
  both(Ann0, AnnTighter, /tightened/, '.v<B>');
  both(Ann1, Ann0, /alternatives were removed/, '.v');

  // The bytes agree with the check: New reads everything Old wrote, to the
  // same value and the same bytes; Old rejects the alternative it does not know.
  for (const value of [{ type: 'A', value: 7 }, { type: 'B', value: 'hello' }, { type: 'B', value: '' }]) {
    const bytes = Var1.encode(value as never);
    assert.deepEqual(Var2.validate(bytes), { ok: true, hasUnknown: false });
    const asNew = Var2.decode(bytes);
    assert.deepEqual(asNew, value);
    assert.deepEqual(Var2.encode(asNew), bytes);
    assert.equal(Var1.validate(Var2.encode(asNew)).ok, true);
  }
  const added = Var2.encode({ type: 'C', value: 9n });
  assert.equal(Var2.validate(added).ok, true);
  assert.equal(Var1.validate(added).ok, false);
  assert.throws(() => Var1.decode(added), PsszError);
  // Inside a record, with the alternative itself evolved.
  const Old = capped(variant({ A: u8, B: Alt0 }));
  const New = capped(variant({ A: u8, B: Alt1, C: u64 }));
  both(Old, New);
  const pad = new Uint8Array(249);
  const recordBytes = Old.encode({ v: { type: 'B', value: { a: 5 } }, pad });
  assert.deepEqual(New.validate(recordBytes), { ok: true, hasUnknown: false });
  const newRecord = New.decode(recordBytes);
  assert.deepEqual(newRecord.v, { type: 'B', value: { a: 5, b: null } });
  assert.deepEqual(New.encode(newRecord), recordBytes);
  assert.equal(Old.validate(New.encode({ v: { type: 'C', value: 1n }, pad })).ok, false);
});
