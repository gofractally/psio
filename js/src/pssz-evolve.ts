// pssz-evolve.ts — the schema-upgrade check: may `newer` replace `older`?
//
// The port of C++ `psio::pssz_evolves_from<Old, New>` (psio/pssz.hpp) to
// two types of two schemas, by the rules of docs/pssz-spec.md §7.5 / §7.6
// and §16. The wire carries no schema, so nothing but this check stands
// between a published type and a revision that silently changes its bytes.

import {
  PsszError, boundCount, type Codec, type LengthBound, type MemberInfo, type Node, type OrderTree,
} from './pssz-core.js';
import type { PsszType } from './pssz.js';

/** The outcome of `psszEvolvesFrom`: compatible, or the first rule broken and where. */
export type PsszCompatibility =
  | { ok: true }
  | {
    ok: false;
    /** What changed that a compatible revision may not change. */
    reason: string;
    /** Where, from the root: `.field`, `[]` (an element or a map entry), `.2` (a tuple element), `<Alt>` (an alternative), `?` (an optional's payload). */
    path: string;
  };

/** Anything with a codec: a `PsszType` (`schema.type(...)`) or a typed definition. */
export type PsszTypeLike = PsszType<any, any> | { type(): PsszType<any, any> };

interface Position {
  /** The position cap: the explicit cap of the nearest enclosing record. */
  cap: bigint | null;
  /** The length bound of the member that holds this position (through `optional`). */
  bound: LengthBound | null;
}
const NOWHERE: Position = { cap: null, bound: null };

class Incompatible extends Error {
  constructor(readonly reason: string, readonly path: string) {
    super(reason);
  }
}

// The lengths a position admits: [min, max], `null` for no maximum.
function range(container: Node | null, bound: LengthBound | null): { min: bigint; max: bigint | null } {
  let min = 0n;
  let max: bigint | null = null;
  if (container?.bounded) {
    max = container.count;
    if (container.exact) min = container.count;
  }
  if (bound) {
    const lower = bound.exact ?? bound.min;
    const upper = bound.exact ?? bound.max;
    if (lower !== null && lower > min) min = lower;
    if (upper !== null && (max === null || upper < max)) max = upper;
  }
  return { min, max };
}

function orderText(tree: OrderTree | null | undefined): string {
  if (!tree) return '';
  const at = tree.at ? [...tree.at].sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0)).map(([k, v]) => [k, orderText(v)]) : [];
  return JSON.stringify([
    tree.sorted ? [tree.sorted.direction, tree.sorted.strict] : null, !!tree.uniqueKeys,
    orderText(tree.element), orderText(tree.keys), orderText(tree.values), at,
  ]);
}

class Walk {
  private readonly ids = new Map<Node, number>();
  private readonly seen = new Set<string>();

  constructor(private readonly older: Codec, private readonly newer: Codec) {}

  private id(node: Node): number {
    let id = this.ids.get(node);
    if (id === undefined) this.ids.set(node, (id = this.ids.size));
    return id;
  }

  // Everything the old bound admits, the new one admits (§7.5: a bound may
  // only widen).
  private widened(
    a: Node | null, ab: LengthBound | null, b: Node | null, bb: LengthBound | null, what: string, path: string,
  ): void {
    const was = range(a, ab);
    const is = range(b, bb);
    if (is.min > was.min || (is.max !== null && (was.max === null || is.max < was.max)))
      throw new Incompatible(
        `the length bound of ${what} was tightened ([${was.min}, ${was.max ?? 'unbounded'}] -> [${is.min}, ${is.max ?? 'unbounded'}])`,
        path,
      );
  }

  private order(a: OrderTree | null, b: OrderTree | null, path: string): void {
    if (orderText(a) !== orderText(b)) throw new Incompatible('sorted / unique / uniqueKeys changed', path);
  }

  member(a: MemberInfo, b: MemberInfo, capA: bigint | null, capB: bigint | null, path: string): void {
    this.order(a.order, b.order, path);
    this.type(a.text, b.text, { cap: capA, bound: a.bound }, { cap: capB, bound: b.bound }, path);
  }

  type(aRef: Node, bRef: Node, pa: Position, pb: Position, path: string): void {
    const fail = (reason: string): never => {
      throw new Incompatible(reason, path === '' ? '(root)' : path);
    };
    const wa = this.older.wireOf(aRef);
    const wb = this.newer.wireOf(bRef);
    if (wa.boxed !== wb.boxed) fail('a boxed value on one side only');
    const a = wa.node;
    const b = wb.node;
    const key = [this.id(a), this.id(b), pa.cap, pb.cap, JSON.stringify(pa.bound, text), JSON.stringify(pb.bound, text)].join('/');
    // (a recursive type: the pair is being checked further up)
    if (this.seen.has(key)) return;
    this.seen.add(key);
    if (a.kind !== b.kind) fail(`the type changed (${a.kind} -> ${b.kind})`);
    if (this.older.layout(a).fixed !== this.newer.layout(b).fixed) fail('a fixed-size type on one side, a variable-size type on the other');
    const beneath = (cap: bigint | null): Position => ({ cap, bound: null });
    switch (a.kind) {
      case 'int':
        if (a.bits !== b.bits || a.signed !== b.signed) fail('the integer type changed');
        return;
      case 'float':
        if (a.size !== b.size) fail('the float type changed');
        return;
      case 'bool':
      case 'char':
        return;
      case 'bitvector':
        if (a.size !== b.size) fail('the bit count changed');
        return;
      case 'bitlist':
        if (b.size < a.size) fail('the bit bound was tightened');
        return;
      case 'string':
        // A raw string is any bytes and a UTF-8 string is text.
        if (a.raw !== b.raw) fail('utf8 changed (a raw string and a UTF-8 string are different types)');
        this.widened(a.child, pa.bound, b.child, pb.bound, 'the string', path);
        return;
      case 'hex':
        this.widened(a.child, pa.bound, b.child, pb.bound, 'the bytes', path);
        return;
      case 'nested':
        this.widened(null, pa.bound, null, pb.bound, 'the nested value', path);
        // The inner type evolves by the same rules, as a root of its own.
        new Walk(this.older.auto, this.newer.auto).type(a.textChild, b.textChild, NOWHERE, NOWHERE, `${path}(nested)`);
        return;
      case 'option':
        if (this.older.canBeEmpty(a.child) !== this.newer.canBeEmpty(b.child)) fail('the optional form (selector or not) changed');
        this.type(a.textChild, b.textChild, pa, pb, `${path}?`);
        return;
      case 'list':
      case 'array': {
        if (a.assoc !== b.assoc) fail('a list, a map and a set are different types');
        this.widened(a, pa.bound, b, pb.bound, a.assoc ? `the ${a.assoc}` : 'the list', path);
        if (!this.older.layout(a.child).fixed) {
          const was = this.older.widthOf(a, pa.cap, boundCount(pa.bound));
          const is = this.newer.widthOf(b, pb.cap, boundCount(pb.bound));
          if (was !== is) fail(`the offset width of the ${a.assoc ?? 'list'} changed (${was} -> ${is})`);
        }
        // (a map's entry is the [key, value] tuple: its path is `[].0` / `[].1`)
        this.type(a.textChild, b.textChild, beneath(pa.cap), beneath(pb.cap), `${path}[]`);
        return;
      }
      case 'variant':
        // Alternatives may be appended (§7.5), never removed or reordered.
        if (b.members.length < a.members.length) fail('variant alternatives were removed');
        a.members.forEach((m, i) => {
          const other = b.members[i];
          if (m.name !== other.name) fail(`variant alternative ${i} was renamed or reordered ("${m.name}" -> "${other.name}")`);
          this.type(m.text, other.text, beneath(pa.cap), beneath(pb.cap), `${path}<${m.name}>`);
        });
        return;
      case 'tuple':
      case 'record': {
        const record = a.kind === 'record';
        const what = record ? 'record' : 'tuple';
        if (a.stable !== b.stable) fail('definitionWillNotChange was added or removed');
        if (b.members.length < a.members.length) fail(`${record ? 'fields' : 'elements'} were removed`);
        if (a.stable && b.members.length !== a.members.length) fail('a definitionWillNotChange record gained fields');
        const was = this.older.widthOf(a, pa.cap);
        const is = this.newer.widthOf(b, pb.cap);
        if (was !== is) fail(`the offset width of the ${what} changed (${was} -> ${is})`);
        const capA = this.older.capBeneath(a, pa.cap);
        const capB = this.newer.capBeneath(b, pb.cap);
        a.members.forEach((m, i) => {
          const other = b.members[i];
          if (record && m.name !== other.name) fail(`field ${i} was renamed or reordered ("${m.name}" -> "${other.name}")`);
          this.member(m, other, capA, capB, record ? `${path}.${m.name}` : `${path}.${i}`);
        });
        // An appended field must be able to be absent.
        for (let i = a.members.length; i < b.members.length; ++i)
          if (!this.newer.layout(b.members[i].node).optional)
            fail(`the appended ${record ? `field "${b.members[i].name}"` : `element ${i}`} is not optional`);
        if (!record) return;
        // Caps (§7.5, §16): New's cap admits everything Old's writer can
        // produce; Old's maxFields is a promise about every revision.
        if (b.cap !== null) {
          const limit = a.cap ?? this.older.inferredBound(a);
          if (limit === null) fail(`maxDynamicData (${b.cap}) was added to a record of unbounded size`);
          else if (limit > b.cap)
            fail(a.cap !== null
              ? `maxDynamicData was lowered (${a.cap} -> ${b.cap})`
              : `maxDynamicData (${b.cap}) is below the bound inferred for the old record (${limit})`);
        }
        if (a.maxFields !== null) {
          if (b.members.length > a.maxFields) fail(`more fields (${b.members.length}) than the original's maxFields (${a.maxFields})`);
          if (b.maxFields === null) fail(`maxFields (${a.maxFields}) was dropped`);
          else if (b.maxFields > a.maxFields) fail(`maxFields was raised (${a.maxFields} -> ${b.maxFields})`);
        }
        return;
      }
      default:
        return;
    }
  }
}

function text(_key: string, value: unknown): unknown {
  return typeof value === 'bigint' ? String(value) : value;
}

function internals(type: PsszTypeLike, which: string): { codec: Codec; ref: Node; root: MemberInfo | null } {
  const resolved = typeof (type as { internals?: unknown }).internals === 'object'
    ? (type as PsszType) : (type as { type(): PsszType }).type?.();
  const out = resolved?.internals;
  if (!out) throw new PsszError('schema', `psszEvolvesFrom: ${which} must be a PsszType or a type definition`);
  if (out.codec.W !== 0) throw new PsszError('schema', 'psszEvolvesFrom: the check is of the automatic width profile');
  return out;
}

/**
 * Whether `newer` is a compatible revision of `older` (C++
 * `psio::pssz_evolves_from<Old, New>`; `docs/pssz-spec.md` §7.5, §7.6, §16):
 * a reader of `newer` reads every value a writer of `older` can produce,
 * and a reader of `older` reads every value of `newer` that is also one of
 * its own (it reports appended fields as unknown). Run it before publishing
 * a changed type — the wire carries no schema, so nothing else will notice.
 *
 * Compatible:
 * - appending **optional** fields to a record (not to one that is
 *   `fixed`), optional trailing elements to a tuple or parameters to an
 *   argument list, and alternatives to the end of a variant;
 * - **widening** a length bound — a higher or dropped `max`, a lower or
 *   dropped `min` — at a member or below it, while no offset width changes
 *   anywhere;
 * - raising `maxDynamicData` within its width class, adding one that is at
 *   least the bound inferred for the old record, or dropping it when no
 *   width changes; lowering `maxFields` to no less than the field count;
 * - the same changes inside a nested value, an optional, a list, a map.
 *
 * Not compatible (the result says which, and where): removing, reordering,
 * renaming or retyping a field; a tightened bound; `utf8` added or removed;
 * `sorted` / `unique` / `uniqueKeys` changed; a changed offset width at any
 * depth; `fixed` added or removed; a lowered `maxDynamicData`; more fields
 * than the original's `maxFields`, or that cap raised or dropped.
 *
 * Both sides are taken in the automatic width profile. `sorted` / `unique`
 * / `uniqueKeys` are compared between typed definitions only: they are not
 * in the Schema IR, so two fetched schemas cannot differ in them. The names
 * of the types themselves are not compared (the Schema IR does not say
 * which named alias is a transparent type).
 */
export function psszEvolvesFrom(older: PsszTypeLike, newer: PsszTypeLike): PsszCompatibility {
  const a = internals(older, 'the older type');
  const b = internals(newer, 'the newer type');
  try {
    const walk = new Walk(a.codec, b.codec);
    if (orderText(a.root?.order) !== orderText(b.root?.order)) throw new Incompatible('sorted / unique / uniqueKeys changed', '(root)');
    walk.type(a.ref, b.ref, { cap: null, bound: a.root?.bound ?? null }, { cap: null, bound: b.root?.bound ?? null }, '');
    return { ok: true };
  } catch (e) {
    if (e instanceof Incompatible) return { ok: false, reason: e.reason, path: e.path };
    if (e instanceof PsszError) throw e;
    throw new PsszError('internal', e instanceof Error ? `${e.name}: ${e.message}` : String(e));
  }
}
