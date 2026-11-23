// pssz-evolve.ts — comparing two types of two schemas: may `newer` replace
// `older` (`psszEvolvesFrom`), and are the two the same type
// (`psszEquivalent`)?
//
// The port of C++ `psio::pssz_ir::evolves_from` / `equivalent`
// (psio/pssz_ir_compare.hpp; the run-time twin of
// `psio::pssz_evolves_from<Old, New>`), by the rules of docs/pssz-spec.md
// §7.5 / §7.6 and §16. The wire carries no schema, so nothing but this
// check stands between a published type and a revision that silently
// changes its bytes. The walk, the codes and the paths are pinned by
// conformance/evolution-vectors.json.

import {
  PsszError, boundCount, type Codec, type LengthBound, type MemberInfo, type Node, type OrderTree,
} from './pssz-core.js';
import { PsszInterface, PsszMethod, PsszSchema, type PsszType } from './pssz.js';

/**
 * What a comparison looks at. `wire`: the bytes, and which bytes are
 * values. `contract` (the default, as in C++): also what a consumer of the
 * text forms and of the type's identity sees — the names of record members
 * and variant alternatives, custom ids, enums, the text shape of a record
 * (an object, a named or a positional argument list), `jsonInt` /
 * `ijsonInt`, an alternative's `ijsonName`, and whether a variant is
 * `untagged`.
 */
export type PsszCompare = 'wire' | 'contract';

/** Options of `psszEvolvesFrom` / `psszEquivalent`. */
export interface PsszCompareOptions {
  /** Default `contract`. */
  compare?: PsszCompare;
}

/**
 * The rule a comparison found broken: a stable identifier, shared with the
 * other languages (`conformance/evolution-vectors.md`). `order-changed` is
 * this package's own: `sorted` / `unique` / `uniqueKeys` differ between two
 * typed definitions (they are not in the Schema IR).
 */
export type PsszCompatibilityCode =
  | 'malformed-schema' | 'type-changed' | 'boxed-changed' | 'custom-id-changed' | 'enum-changed'
  | 'integer-changed' | 'float-changed' | 'bit-count-changed' | 'array-length-changed' | 'fixed-size-changed'
  | 'utf8-changed' | 'optional-form-changed' | 'bound-tightened' | 'bound-changed' | 'width-changed'
  | 'key-type-changed' | 'alternatives-removed' | 'alternative-count-changed' | 'alternative-renamed'
  | 'dwnc-changed' | 'fields-removed' | 'field-count-changed' | 'dwnc-gained-fields' | 'field-renamed'
  | 'appended-field-not-optional' | 'max-dynamic-data' | 'max-fields' | 'text-form-changed' | 'result-changed'
  | 'function-removed' | 'function-added' | 'type-removed' | 'type-added' | 'interface-removed' | 'interface-added'
  | 'order-changed';

/** The outcome of `psszEvolvesFrom` / `psszEquivalent`: compatible, or the first rule broken and where. */
export type PsszCompatibility =
  | { ok: true }
  | {
    ok: false;
    /** Which rule, as a stable identifier. */
    code: PsszCompatibilityCode;
    /** What changed, as a sentence for a person. */
    reason: string;
    /**
     * Where, from the root: `.field`, `[]` (an element; a map entry's key
     * and value are `[].0` / `[].1`), `.2` (a tuple element), `<Alt>` (an
     * alternative), `?` (an optional's payload), `(nested)`; for a method
     * `(args)` / `(result)` after its name, for an interface's listed type
     * `type:Name`, for a schema the interface name and `/`. The root
     * itself is `(root)`.
     */
    path: string;
  };

/** Anything with a codec: a `PsszType` (`schema.type(...)`) or a typed definition. */
export type PsszTypeLike = PsszType<any, any> | { type(): PsszType<any, any> };
/**
 * What can be compared, with another of the same sort: two types, two
 * methods (`PsszMethod`: the argument list and the result), two interfaces
 * (`PsszInterface`, or a typed `defineInterface(...)`: every method and
 * every listed type, by name), two schemas (`PsszSchema`: every interface,
 * by name).
 */
export type PsszComparable = PsszTypeLike | PsszMethod<any, any> | PsszInterface | PsszSchema | { readonly lookup: PsszInterface };

interface Position {
  /** The position cap: the explicit cap of the nearest enclosing record. */
  cap: bigint | null;
  /** The length bound of the member that holds this position (through `optional`). */
  bound: LengthBound | null;
}
const NOWHERE: Position = { cap: null, bound: null };

class Incompatible extends Error {
  constructor(readonly code: PsszCompatibilityCode, readonly reason: string, readonly path: string) {
    super(reason);
  }
}

interface Range { min: bigint; max: bigint | null }
// The lengths a position admits: [min, max], `null` for no maximum.
function range(container: Node | null, bound: LengthBound | null): Range {
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


type Kind =
  | 'integer' | 'float' | 'bool' | 'string' | 'bytes' | 'bit vector' | 'bit list' | 'map' | 'set' | 'optional'
  | 'list' | 'array' | 'variant' | 'record' | 'nested value';

// A type with the nodes that add nothing to its bytes taken off.
interface Peeled {
  node: Node;
  boxed: boolean;
  /** Custom ids that are not part of the binary form, outermost first. */
  ids: string[];
  kind: Kind;
}

function kindOf(node: Node): Kind {
  switch (node.kind) {
    case 'int': case 'char': return 'integer';
    case 'float': return 'float';
    case 'bool': return 'bool';
    case 'string': return 'string';
    case 'hex': return 'bytes';
    case 'bitvector': return 'bit vector';
    case 'bitlist': return 'bit list';
    case 'option': return 'optional';
    case 'list': return node.assoc ?? 'list';
    case 'array': return 'array';
    case 'variant': return 'variant';
    case 'tuple': case 'record': return 'record';
    case 'nested': return 'nested value';
    default: throw new PsszError('schema', 'pssz IR: a type the codec does not support');
  }
}

function peel(ref: Node): Peeled {
  let boxed = false;
  const ids: string[] = [];
  for (let hops = 0; ref.kind === 'custom'; ++hops) {
    if (!ref.inner || hops > 4096) throw new PsszError('schema', 'pssz IR: cyclic named aliases');
    if (ref.boxed) boxed = true;
    else ids.push(ref.id);
    ref = ref.inner;
  }
  // A time presentation and "json" are ids over their wire type too.
  if (ref.time || ref.json) ids.push(ref.id);
  return { node: ref, boxed, ids, kind: kindOf(ref) };
}

const rangeText = (r: Range) => `[${r.min}, ${r.max ?? 'unbounded'}]`;
const number = (n: bigint | number | null) => (n === null ? 'unbounded' : String(n));

type TextShape = 'object' | 'named' | 'positional';
const textShape = (node: Node): TextShape => (node.kind === 'tuple' ? 'positional' : node.tuple ?? 'object');

class Walk {
  private readonly ids = new Map<Node, number>();
  private readonly seen = new Set<string>();
  private path = '';

  constructor(private readonly older: Codec, private readonly newer: Codec, private readonly names: boolean) {}

  private id(node: Node): number {
    let id = this.ids.get(node);
    if (id === undefined) this.ids.set(node, (id = this.ids.size));
    return id;
  }

  /** The path of a failure found now. */
  get where(): string {
    return this.path === '' ? '(root)' : this.path;
  }
  fail(code: PsszCompatibilityCode, reason: string): never {
    throw new Incompatible(code, reason, this.where);
  }
  /** Runs `run` one step further down the path. */
  at<R>(step: string, run: () => R): R {
    const outer = this.path;
    this.path = outer + step;
    const out = run();
    this.path = outer;
    return out;
  }

  private ranges(was: Range, is: Range, exact: boolean, what: string): void {
    if (exact) {
      if (was.min !== is.min || was.max !== is.max)
        this.fail('bound-changed', `the length bound of ${what} differs (${rangeText(was)} / ${rangeText(is)})`);
      return;
    }
    // (§7.5: a bound may only widen)
    if (is.min > was.min || (is.max !== null && (was.max === null || is.max < was.max)))
      this.fail('bound-tightened', `the length bound of ${what} was tightened (${rangeText(was)} -> ${rangeText(is)})`);
  }

  private widths(a: Node, b: Node, pa: Position, pb: Position, what: string): void {
    const was = this.older.widthOf(a, pa.cap, boundCount(pa.bound));
    const is = this.newer.widthOf(b, pb.cap, boundCount(pb.bound));
    if (was !== is) this.fail('width-changed', `the offset width of ${what} differs (${was} / ${is})`);
  }

  private enums(a: Node, b: Node, exact: boolean): void {
    if (!a.enumerators !== !b.enumerators) this.fail('enum-changed', 'an enum on one side only');
    if (!a.enumerators || !b.enumerators) return;
    const was = a.enumerators;
    const is = b.enumerators;
    // (the first enumerator of a name is the one a name is looked up as)
    const has = (all: typeof was, name: string, value: bigint) => all.find((e) => e.name === name)?.value === value;
    for (const e of was)
      if (!has(is, e.name, e.value)) this.fail('enum-changed', `the enumerator "${e.name}" was removed or renumbered`);
    if (exact)
      for (const e of is)
        if (!has(was, e.name, e.value)) this.fail('enum-changed', `the enumerator "${e.name}" is on one side only`);
  }

  private order(a: OrderTree | null | undefined, b: OrderTree | null | undefined): void {
    if (orderText(a) !== orderText(b)) this.fail('order-changed', 'sorted / unique / uniqueKeys changed');
  }

  private record(a: Node, b: Node, pa: Position, pb: Position, exact: boolean): void {
    if (a.stable !== b.stable) this.fail('dwnc-changed', 'definitionWillNotChange on one side only');
    const ta = textShape(a);
    const tb = textShape(b);
    if (this.names && ta !== tb)
      this.fail('text-form-changed', 'the text shape of the record differs (an object, a named argument list or positional)');
    const count = a.members.length;
    if (exact && b.members.length !== count)
      this.fail('field-count-changed', `the number of fields differs (${count} / ${b.members.length})`);
    if (b.members.length < count) this.fail('fields-removed', `fields were removed (${count} -> ${b.members.length})`);
    if (a.stable && b.members.length !== count)
      this.fail('dwnc-gained-fields', 'a definitionWillNotChange record gained fields');
    // A record's width is its own wherever it appears; a tuple's is also
    // bounded by the cap around it.
    if (!a.stable) this.widths(a, b, pa, pb, 'the record');
    // The members lie beneath the record's own cap, or beneath none; a
    // Tuple node has none of its own and passes on the one around it.
    const capA = a.kind === 'tuple' ? pa.cap : a.cap;
    const capB = b.kind === 'tuple' ? pb.cap : b.cap;
    const both = a.kind === 'record' && b.kind === 'record';
    for (let i = 0; i < count; ++i) {
      const m = a.members[i];
      const other = b.members[i];
      if (this.names && both && ta !== 'positional' && m.name !== other.name)
        // (reported at the record: the member has two names)
        this.fail('field-renamed', `field ${i} was renamed or reordered ("${m.name}" -> "${other.name}")`);
      this.at(a.kind === 'record' && ta !== 'positional' ? `.${m.name}` : `.${i}`, () => {
        if (this.names && both) {
          if (m.jsonInt !== other.jsonInt) this.fail('text-form-changed', 'jsonInt differs (an integer as a number or a string)');
          if (m.ijsonInt !== other.ijsonInt) this.fail('text-form-changed', 'ijsonInt differs (an integer as a number or a string)');
        }
        this.order(m.order, other.order);
        this.type(m.text, other.text, { cap: capA, bound: m.bound }, { cap: capB, bound: other.bound }, exact);
      });
    }
    // An appended member must be able to be absent.
    for (let i = count; i < b.members.length; ++i)
      if (!this.newer.layout(b.members[i].node).optional)
        this.fail('appended-field-not-optional',
          `the appended field ${b.kind === 'tuple' || tb === 'positional' ? i : `"${b.members[i].name}"`} is not optional (old data has no value for it)`);
    // Caps (§7.5, §16): New's cap admits everything Old's writer can
    // produce; Old's maxFields is a promise about every revision.
    const fieldsA = a.kind === 'tuple' ? null : a.maxFields;
    const fieldsB = b.kind === 'tuple' ? null : b.maxFields;
    const ownA = a.kind === 'tuple' ? null : a.cap;
    const ownB = b.kind === 'tuple' ? null : b.cap;
    if (exact) {
      if (ownA !== ownB)
        this.fail('max-dynamic-data', `maxDynamicData differs (${number(ownA)} / ${number(ownB)}; unbounded: none)`);
      if (fieldsA !== fieldsB)
        this.fail('max-fields', `maxFields differs (${number(fieldsA)} / ${number(fieldsB)}; unbounded: none)`);
      return;
    }
    if (ownB !== null) {
      const limit = ownA ?? this.older.inferredBound(a);
      if (limit === null) this.fail('max-dynamic-data', `maxDynamicData (${ownB}) was added to a record of unbounded size`);
      else if (limit > ownB)
        this.fail('max-dynamic-data', ownA !== null
          ? `maxDynamicData was lowered (${ownA} -> ${ownB})`
          : `maxDynamicData (${ownB}) is below the bound inferred for the old record (${limit})`);
    }
    if (fieldsA !== null) {
      if (b.members.length > fieldsA)
        this.fail('max-fields', `more fields (${b.members.length}) than the original's maxFields (${fieldsA})`);
      if (fieldsB === null) this.fail('max-fields', `maxFields (${fieldsA}) was dropped`);
      else if (fieldsB > fieldsA) this.fail('max-fields', `maxFields was raised (${fieldsA} -> ${fieldsB})`);
    }
  }

  /** The walk of `pssz_ir_compare.hpp`: `exact` for `equivalent`. */
  type(aRef: Node, bRef: Node, pa: Position, pb: Position, exact: boolean): void {
    const pa_ = peel(aRef);
    const pb_ = peel(bRef);
    if (pa_.boxed !== pb_.boxed) this.fail('boxed-changed', 'a boxed value on one side only');
    const a = pa_.node;
    const b = pb_.node;
    if (this.names) {
      if (pa_.ids.length !== pb_.ids.length || pa_.ids.some((id, i) => id !== pb_.ids[i]))
        this.fail('custom-id-changed',
          `a custom type on one side only, or under another id ("${pa_.ids[0] ?? ''}" / "${pb_.ids[0] ?? ''}")`);
      this.enums(a, b, exact);
    }
    // (a recursive type: the pair is being, or has been, compared)
    const key = [
      this.id(a), this.id(b), exact, pa.cap, pb.cap, JSON.stringify(pa.bound, text), JSON.stringify(pb.bound, text),
    ].join('/');
    if (this.seen.has(key)) return;
    this.seen.add(key);
    if (pa_.kind !== pb_.kind) this.fail('type-changed', `the type differs (${pa_.kind} / ${pb_.kind})`);
    const la = this.older.layout(a);
    const lb = this.newer.layout(b);
    const fixedSize = () => {
      if (la.fixed !== lb.fixed || (la.fixed && la.size !== lb.size))
        this.fail('fixed-size-changed',
          'a fixed-size type on one side and a variable-size type on the other, or another fixed size');
    };
    const belowA: Position = { cap: pa.cap, bound: null };
    const belowB: Position = { cap: pb.cap, bound: null };
    switch (pa_.kind) {
      case 'integer': {
        // (a lone `char` is the 8-bit unsigned integer)
        const bits = (n: Node) => (n.kind === 'char' ? 8 : n.bits);
        const signed = (n: Node) => n.kind !== 'char' && n.signed;
        if (bits(a) !== bits(b) || signed(a) !== signed(b))
          this.fail('integer-changed', 'the integer type differs (bits or signedness)');
        return;
      }
      case 'float':
        if (a.size !== b.size) this.fail('float-changed', 'the float type differs');
        return;
      case 'bool':
        return;
      case 'bit vector':
        if (a.size !== b.size) this.fail('bit-count-changed', `the bit count differs (${a.size} / ${b.size})`);
        return;
      case 'bit list':
        return this.ranges({ min: 0n, max: BigInt(a.size) }, { min: 0n, max: BigInt(b.size) }, exact, 'the bit list');
      case 'string':
      case 'bytes':
        // A raw string is any bytes and a UTF-8 string is text. (The
        // element type of the byte container is not compared.)
        if (a.raw !== b.raw)
          this.fail('utf8-changed', 'utf8 on one side only (a raw string and a UTF-8 string are different types)');
        fixedSize();
        return this.ranges(range(a.child, pa.bound), range(b.child, pb.bound), exact,
          pa_.kind === 'string' ? 'the string' : 'the bytes');
      case 'nested value':
        this.ranges(range(null, pa.bound), range(null, pb.bound), exact, 'the nested value');
        // The inner value is a root of its own: no cap or bound reaches it.
        return this.at('(nested)', () => this.type(a.textChild, b.textChild, NOWHERE, NOWHERE, exact));
      case 'optional':
        if (this.older.canBeEmpty(a.child) !== this.newer.canBeEmpty(b.child))
          this.fail('optional-form-changed', "the optional's presence form differs (a selector byte or not)");
        // The member's bound and the cap reach the payload.
        return this.at('?', () => this.type(a.textChild, b.textChild, pa, pb, exact));
      case 'list':
      case 'array':
        if (pa_.kind === 'array') {
          if (a.count !== b.count) this.fail('array-length-changed', `the array length differs (${a.count} / ${b.count})`);
        } else this.ranges(range(a, pa.bound), range(b, pb.bound), exact, 'the list');
        if (!(this.older.layout(a.child).fixed && this.newer.layout(b.child).fixed))
          this.widths(a, b, pa, pb, pa_.kind === 'array' ? 'the array' : 'the list');
        return this.at('[]', () => this.type(a.textChild, b.textChild, belowA, belowB, exact));
      case 'map':
      case 'set': {
        const what = `the ${pa_.kind}`;
        this.ranges(range(a, pa.bound), range(b, pb.bound), exact, what);
        if (!(this.older.layout(a.child).fixed && this.newer.layout(b.child).fixed)) this.widths(a, b, pa, pb, what);
        // Another key type is another order (docs/pssz-spec.md 5.9.4): the
        // keys are the same type, in both relations.
        try {
          this.at(pa_.kind === 'map' ? '[].0' : '[]', () => this.type(a.keyRef, b.keyRef, belowA, belowB, true));
        } catch (e) {
          if (!(e instanceof Incompatible)) throw e;
          throw new Incompatible('key-type-changed', `the key type of ${what} differs: ${e.reason}`, e.path);
        }
        if (pa_.kind === 'set') return;
        // The entry is a tuple (key, value): its width, then the value.
        this.at('[]', () => this.widths(a.child, b.child, belowA, belowB, 'the map entry'));
        return this.at('[].1', () => this.type(a.valueRef, b.valueRef, belowA, belowB, exact));
      }
      case 'variant': {
        if (exact && a.members.length !== b.members.length)
          this.fail('alternative-count-changed',
            `the number of alternatives differs (${a.members.length} / ${b.members.length})`);
        // Alternatives may be appended (§7.5), never removed or reordered.
        if (b.members.length < a.members.length)
          this.fail('alternatives-removed', `variant alternatives were removed (${a.members.length} -> ${b.members.length})`);
        if (this.names && a.untagged !== b.untagged) this.fail('text-form-changed', 'an untagged variant on one side only');
        a.members.forEach((m, i) => {
          const other = b.members[i];
          if (this.names && m.name !== other.name)
            this.fail('alternative-renamed', `alternative ${i} was renamed or reordered ("${m.name}" -> "${other.name}")`);
          this.at(`<${m.name}>`, () => {
            this.type(m.text, other.text, belowA, belowB, exact);
            if (this.names && (m.ijsonNameSet !== other.ijsonNameSet || m.ijsonName !== other.ijsonName))
              this.fail('text-form-changed', 'the I-JSON name of the alternative differs');
          });
        });
        return;
      }
      case 'record':
        fixedSize();
        return this.record(a, b, pa, pb, exact);
    }
  }

  // ── Functions, interfaces, whole schemas ──────────────────────────────────

  method(a: PsszMethod, b: PsszMethod, exact: boolean): void {
    this.at('(args)', () => this.root(a.args, b.args, exact));
    this.at('(result)', () => {
      if (!a.result !== !b.result) this.fail('result-changed', 'a result on one side only');
      if (a.result && b.result) this.root(a.result, b.result, exact);
    });
  }

  interface(a: PsszInterface, b: PsszInterface, exact: boolean): void {
    const find = (in_: PsszInterface, name: string) => in_.methods.find((m) => m.name === name);
    for (const m of a.methods)
      this.at(m.name, () => {
        const other = find(b, m.name);
        if (!other) this.fail('function-removed', 'the function was removed');
        this.method(m, other!, exact);
      });
    if (exact)
      for (const m of b.methods)
        if (!find(a, m.name)) this.at(m.name, () => this.fail('function-added', 'the function is on one side only'));
    for (const name of a.ir.type_names)
      this.at(`type:${name}`, () => {
        if (!b.ir.type_names.includes(name)) this.fail('type-removed', 'the type was removed from the interface');
        this.root(a.schema.type(name), b.schema.type(name), exact);
      });
    if (exact)
      for (const name of b.ir.type_names)
        if (!a.ir.type_names.includes(name))
          this.at(`type:${name}`, () => this.fail('type-added', 'the type is on one side only'));
  }

  schema(a: PsszSchema, b: PsszSchema, exact: boolean): void {
    const namesA = a.interfaceNames;
    const namesB = b.interfaceNames;
    for (const name of namesA)
      this.at(`${name}/`, () => {
        if (!namesB.includes(name)) this.fail('interface-removed', 'the interface was removed');
        this.interface(a.interface(name), b.interface(name), exact);
      });
    if (exact)
      for (const name of namesB)
        if (!namesA.includes(name)) this.at(`${name}/`, () => this.fail('interface-added', 'the interface is on one side only'));
  }

  /** Two types as roots (with what a root carries: a method result's bound and order). */
  root(older: PsszTypeLike, newer: PsszTypeLike, exact: boolean): void {
    const a = internals(older);
    const b = internals(newer);
    this.order(a.root?.order, b.root?.order);
    this.type(a.ref, b.ref, { cap: null, bound: a.root?.bound ?? null }, { cap: null, bound: b.root?.bound ?? null }, exact);
  }
}

function text(_key: string, value: unknown): unknown {
  return typeof value === 'bigint' ? String(value) : value;
}

function internals(type: PsszTypeLike): { codec: Codec; ref: Node; root: MemberInfo | null } {
  const resolved = typeof (type as { internals?: unknown }).internals === 'object'
    ? (type as PsszType) : (type as { type(): PsszType }).type();
  const out = resolved.internals;
  if (out.codec.W !== 0) throw new PsszError('schema', 'pssz: the comparison is of the automatic width profile');
  return out;
}

type Sort = 'type' | 'method' | 'interface' | 'schema';
function sortOf(x: unknown, which: string, name: string): { sort: Sort; value: unknown } {
  if (x instanceof PsszSchema) return { sort: 'schema', value: x };
  if (x instanceof PsszInterface) return { sort: 'interface', value: x };
  if (x instanceof PsszMethod) return { sort: 'method', value: x };
  if (typeof x === 'object' && x !== null) {
    if ((x as { lookup?: unknown }).lookup instanceof PsszInterface) return { sort: 'interface', value: (x as { lookup: PsszInterface }).lookup };
    if (typeof (x as { internals?: unknown }).internals === 'object' || typeof (x as { type?: unknown }).type === 'function')
      return { sort: 'type', value: x };
  }
  throw new PsszError('schema', `${name}: ${which} must be a PsszType or a type definition, a PsszMethod, a PsszInterface or a PsszSchema`);
}

function compare(name: string, older: unknown, newer: unknown, options: PsszCompareOptions | undefined, exact: boolean): PsszCompatibility {
  const mode = options?.compare ?? 'contract';
  if (mode !== 'wire' && mode !== 'contract') throw new PsszError('schema', `${name}: compare must be "wire" or "contract"`);
  // A typed interface compiles its schema here: a declaration error is the caller's.
  const a = sortOf(older, exact ? 'the first argument' : 'the older side', name);
  const b = sortOf(newer, exact ? 'the second argument' : 'the newer side', name);
  if (a.sort !== b.sort) throw new PsszError('schema', `${name}: a ${a.sort} cannot be compared with a ${b.sort}`);
  // (not a type, or not the automatic profile: the caller's error, not a verdict)
  const codecOf = (x: { sort: Sort; value: unknown }): Codec =>
    x.sort === 'type' ? internals(x.value as PsszTypeLike).codec
      : x.sort === 'method' ? internals((x.value as PsszMethod).args).codec
      : x.sort === 'interface' ? (x.value as PsszInterface).codec
      : (x.value as PsszSchema).automatic;
  const walk = new Walk(codecOf(a), codecOf(b), mode === 'contract');
  try {
    switch (a.sort) {
      case 'type': walk.root(a.value as PsszTypeLike, b.value as PsszTypeLike, exact); break;
      case 'method': walk.method(a.value as PsszMethod, b.value as PsszMethod, exact); break;
      case 'interface': walk.interface(a.value as PsszInterface, b.value as PsszInterface, exact); break;
      case 'schema': walk.schema(a.value as PsszSchema, b.value as PsszSchema, exact); break;
    }
    return { ok: true };
  } catch (e) {
    if (e instanceof Incompatible) return { ok: false, code: e.code, reason: e.reason, path: e.path };
    // A schema that is not well-formed, or names a type it does not have.
    if (e instanceof PsszError && e.kind === 'schema')
      return { ok: false, code: 'malformed-schema', reason: e.message, path: walk.where };
    if (e instanceof PsszError) throw e;
    throw new PsszError('internal', e instanceof Error ? `${e.name}: ${e.message}` : String(e));
  }
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
 * `options.compare`: `contract` (the default) also refuses what a consumer
 * of the text forms and of the type's identity would see — a renamed field
 * or alternative, another custom id, an enumerator removed or renumbered,
 * another text shape; `wire` compares the bytes only (a renamed field is
 * compatible). The result's `code` is the rule broken
 * (`conformance/evolution-vectors.md`), `path` where.
 *
 * Besides two types: two `PsszMethod`s (the argument list and the result
 * both evolve), two interfaces (every method and listed type of the older
 * one exists in the newer and evolves; methods are matched by name) and two
 * `PsszSchema`s (every interface, by name).
 *
 * Both sides are taken in the automatic width profile. `sorted` / `unique`
 * / `uniqueKeys` are compared between typed definitions only (code
 * `order-changed`): they are not in the Schema IR, so two fetched schemas
 * cannot differ in them. The names of the types themselves are not compared
 * (the Schema IR does not say which named alias is a transparent type).
 */
export function psszEvolvesFrom(older: PsszComparable, newer: PsszComparable, options?: PsszCompareOptions): PsszCompatibility {
  return compare('psszEvolvesFrom', older, newer, options, false);
}

/**
 * Whether the two are the same type (C++ `psio::pssz_ir::equivalent`): the
 * same values, the same bytes, the same permitted revisions — and, under
 * `compare: 'contract'` (the default), the same text and identity. The
 * symmetric relation of `psszEvolvesFrom`: a different bound is
 * `bound-changed`, a different field count `field-count-changed`, a
 * different cap `max-dynamic-data` / `max-fields`, an interface with
 * another function `function-added`.
 */
export function psszEquivalent(a: PsszComparable, b: PsszComparable, options?: PsszCompareOptions): PsszCompatibility {
  return compare('psszEquivalent', a, b, options, true);
}
