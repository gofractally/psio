// types.ts — typed definitions: declare a type in TypeScript, get its Schema IR
// and a statically typed codec.
//
//   const Transfer = record('Transfer', {
//     from: AccountName, to: AccountName, amount: u64, memo: optional(utf8),
//   });
//   type Transfer = Infer<typeof Transfer>;   // { from: bigint; to: bigint; amount: bigint; memo: string | null }
//   const bytes = Transfer.encode({ from: 1n, to: 2n, amount: 5n, memo: null });
//   Transfer.view(bytes).get('amount');      // 5n, nothing else decoded
//
// A definition holds no codec of its own. It produces the Schema IR that C++
// `psio::SchemaBuilder` produces for the equivalent C++ type (`generateSchema`,
// `def.schema`), and every operation runs through the schema-driven codec of
// `pssz.ts` on that IR: one implementation of PSSZ, key, JSON and views.

import {
  IR_VERSION, PsszError, reservedCustomId, type BitsValue, type IrAnyType, type IrAttribute, type IrFunc, type IrInterface,
  type IrMember, type IrSchema, type OrderTree, type PsszCustomHandler, type Some,
} from './pssz-core.js';
import type { JsonFormat } from './pssz-json.js';
import {
  PsszSchema, type PsszDecodeOptions, type PsszInterface, type PsszKeyValidation, type PsszNestedBytes, type PsszType,
  type PsszValidation, type PsszWidth,
} from './pssz.js';
import type { ListView, RecordView, TupleView, VariantView } from './pssz-view.js';
import { timeSchemaId, timeUnitPeriod, type TimeUnit } from './time.js';

function schemaError(message: string): never {
  throw new PsszError('schema', message);
}
// A declaration fails with a PsszError of kind `schema`, whatever went wrong
// inside it (a `lazy` callback that throws, unbounded structural recursion).
function declared<R>(run: () => R): R {
  try {
    return run();
  } catch (e) {
    if (e instanceof PsszError) throw e;
    throw new PsszError('schema', `typed schema: ${e instanceof Error ? `${e.name}: ${e.message}` : String(e)}`);
  }
}

// ── Shapes ──────────────────────────────────────────────────────────────────

type AnyDef = TypeDef<any, any>;

// The bounds of one position, as written (`PositionOptions`), checked and
// with the positions that say nothing dropped.
interface Position {
  max?: number;
  utf8?: boolean;
  element?: Position;
  keys?: Position;
  values?: Position;
  at?: Map<string, Position>;
}
interface FieldShape {
  name: string;
  type: AnyDef;
  lengthBound?: LengthBoundOption;
  /** What the member says below itself, and `utf8` (never `max`: that is `lengthBound`). */
  position?: Position;
  jsonInt?: IntText;
  ijsonInt?: IntText;
  /** `sorted` / `unique` / `uniqueKeys` of the member and below it (typed-only). */
  order?: OrderTree;
}
interface AltShape {
  name: string;
  type: AnyDef;
  ijsonName?: string;
}
type Shape =
  | { k: 'int'; bits: number; signed: boolean }
  | { k: 'float'; exp: number; mantissa: number }
  | { k: 'bool' }
  | { k: 'char' }
  // A byte container under an adapter: list, bounded list (`n` max) or array (`n` exact).
  | { k: 'bytes'; id: 'utf8' | 'string' | 'hex'; bound: 'list' | 'max' | 'exact'; n: bigint }
  | { k: 'bits'; id: 'bitvector' | 'bitlist'; n: bigint }
  | { k: 'option'; inner: AnyDef }
  | { k: 'list'; inner: AnyDef }
  | { k: 'bounded'; inner: AnyDef; n: bigint }
  | { k: 'array'; inner: AnyDef; n: bigint }
  // `pair`: a C++ std::pair, the same node as the 2-tuple under an identity of its own.
  | { k: 'tuple'; items: AnyDef[]; pair: boolean }
  // `entry`: the tuple (key, value) of a map, the element of a set. `unordered`:
  // a C++ unordered container, the same node under an identity of its own.
  | { k: 'assoc'; id: 'map' | 'set'; entry: AnyDef; unordered: boolean; max: bigint | null }
  | { k: 'nested'; inner: AnyDef; deferred: boolean }
  // `position`: the bounds the type gives its member (it is then that bounded type).
  | { k: 'transparent'; name: string; inner: AnyDef; position?: Position }
  | { k: 'variant'; alts: AltShape[] }
  | { k: 'record'; name: string; fields: FieldShape[]; options: RecordOptions }
  // A method's argument tuple; `uid`: its identity (C++: one type per method).
  // `named`: every parameter has a name (the fields are named "0", "1", ... otherwise).
  | { k: 'args'; uid: number; fields: FieldShape[]; named: boolean; maxDynamicData?: number | bigint; maxFields?: number }
  | { k: 'enum'; under: AnyDef; members: [string, bigint][] }
  | { k: 'time'; id: string; tick: AnyDef }
  | { k: 'custom'; id: string; inner: AnyDef; handler?: PsszCustomHandler }
  | { k: 'box'; inner: AnyDef }
  | { k: 'lazy'; get: () => AnyDef };

/** How an integer member is written in text: a bare number or a string. */
export type IntText = 'number' | 'string';

/** A bound on the length of a string, bytes or list member. */
export interface LengthBoundOption {
  max?: number;
  min?: number;
  exact?: number;
}

/**
 * The bounds of one position of a member's type (`docs/pssz-spec.md` §3.5):
 * the member itself, or a position below it — the elements of a list, the
 * keys of a map, element 2 of a tuple — to any depth. An `optional` is
 * looked through: what is written for it applies to its payload.
 *
 *     field(list(rawString), { max: 8, element: { utf8: true, max: 16 } })
 *
 * The member is exactly the type the bounds describe (here a bounded list of
 * bounded UTF-8 strings): its size bound and offset widths, its bytes, its
 * Schema IR and its checks are that type's. C++ writes the same as member
 * annotations: `attr(tags, max<8> | element(utf8 | max<16>))`.
 */
export interface PositionOptions {
  /**
   * At most `max` bytes of a `rawString` or `bytes`, elements of a `list`,
   * entries of a `map` or `set`.
   */
  max?: number;
  /**
   * A `rawString` here is UTF-8 text: validated, and a `string` in
   * JavaScript instead of a `Uint8Array`.
   */
  utf8?: boolean;
  /** Every element of a `list`, a `boundedList`, an `array` or a `set`. */
  element?: PositionOptions;
  /** Every key of a `map`. */
  keys?: PositionOptions;
  /** Every mapped value of a `map`. */
  values?: PositionOptions;
  /**
   * One element of a `tuple` / `pair`, by index, or one alternative of a
   * `variant`, by name (or index): `{ 0: { max: 16 } }`.
   */
  at?: { readonly [index: string]: PositionOptions };
  /**
   * The elements of a `list` / `boundedList` / `array` here are in order:
   * non-descending (`true`) or non-ascending (`'descending'`). Order is that
   * of maps and sets: bytewise order of the elements' key encoding
   * (`encodeKey`), the same in every language.
   */
  sorted?: boolean | 'descending';
  /**
   * The elements are in strictly ascending order (sorted, no two equal);
   * with `sorted: 'descending'`, strictly descending.
   */
  unique?: boolean;
  /**
   * No two elements have the same key: the element itself, or element 0 of
   * a `pair` / `tuple` element (a list of entries that is a map by
   * content). No order is required.
   */
  uniqueKeys?: boolean;
}

/** Options of a record member or a method parameter (`field`). */
export interface FieldOptions extends PositionOptions {
  /**
   * Bounds the member's own length with `min` / `exact` as well as `max`
   * (bytes of a string, elements of a list, bytes of a nested value),
   * through `optional`. `{ lengthBound: { max } }` is `{ max }`: alone on a
   * raw string, bytes, list, map or set it makes the member a bounded type,
   * which narrows the record's offset width.
   */
  lengthBound?: LengthBoundOption;
  /** The form of the member's integers in strict JSON. */
  jsonInt?: IntText;
  /** The form of the member's integers in I-JSON. */
  ijsonInt?: IntText;
}

/** Options of a variant alternative (`alt`). */
export interface AltOptions {
  /** The alternative's key in I-JSON (default: its name). */
  ijsonName?: string;
}

/** Options of `record`. */
export interface RecordOptions {
  /**
   * The definition will not change: no header, no evolution (C++
   * `definition_will_not_change`).
   */
  fixed?: boolean;
  /** An explicit cap on the encoded size; narrows the offset width. */
  maxDynamicData?: number | bigint;
  /**
   * A cap on the number of fields: the record has, and will ever have, at
   * most this many (`docs/pssz-spec.md` §16). At most 255; a record with
   * more fields than it is an error.
   */
  maxFields?: number;
}

/** Options of `def.type(...)` and `generateSchema`. */
export interface TypeDefOptions {
  /** Offset-width profile; default `auto`. */
  width?: PsszWidth;
  /** A custom type declared without a handler is an error (binary and text). */
  requireHandlers?: boolean;
  /**
   * `opaque`: every nested value is its bytes (a `Uint8Array`), not looked
   * at — whatever the declaration says. Default `typed`.
   */
  nestedBytes?: PsszNestedBytes;
}

// ── Schema IR generation (cpp/include/psio/schema_builder.hpp) ──────────────

const ir = (type: string, value: unknown): IrAnyType => ({ value: { type, value } } as IrAnyType);
const intIr = (bits: number, isSigned: boolean) => ir('Int', { bits, isSigned });
const attribute = (name: string, value: string): IrAttribute => ({ name, value });
const sameRef = (a: IrAnyType, b: IrAnyType) =>
  a.value.type === 'Type' && b.value.type === 'Type' && a.value.value.type === b.value.value.type;

function resolve(def: AnyDef): AnyDef {
  for (let hops = 0; def.shape.k === 'lazy'; ++hops) {
    if (hops > 64) schemaError('typed schema: a lazy definition never resolves');
    const next = def.shape.get();
    if (!(next instanceof TypeDef)) schemaError('typed schema: lazy() must return a type definition');
    def = next;
  }
  return def;
}

// The identity of a type, as C++ type identity: structural for containers
// and scalars, by name for records.
function keyOf(def: AnyDef, depth = 0): string {
  if (depth > 256) schemaError('typed schema: type nesting too deep (use a record for recursion)');
  def = resolve(def);
  if (def.key !== undefined) return def.key;
  const s = def.shape;
  const k = (d: AnyDef) => keyOf(d, depth + 1);
  let out: string;
  switch (s.k) {
    case 'int': out = `${s.signed ? 'i' : 'u'}${s.bits}`; break;
    case 'float': out = `f${s.exp}`; break;
    case 'bool': out = 'bool'; break;
    case 'char': out = 'char'; break;
    case 'bytes': out = `${s.id}:${s.bound}:${s.n}`; break;
    case 'bits': out = `${s.id}:${s.n}`; break;
    case 'option': out = `O(${k(s.inner)})`; break;
    case 'list': out = `L(${k(s.inner)})`; break;
    case 'bounded': out = `B(${k(s.inner)},${s.n})`; break;
    case 'array': out = `A(${k(s.inner)},${s.n})`; break;
    case 'tuple': out = `${s.pair ? 'P' : 'T'}(${s.items.map(k).join(',')})`; break;
    case 'assoc': out = `${s.unordered ? 'U' : ''}${s.id}(${k(s.entry)}${s.max === null ? '' : `,${s.max}`})`; break;
    case 'nested': out = `N${s.deferred ? 'D' : ''}(${k(s.inner)})`; break;
    case 'transparent': out = `W(${JSON.stringify(s.name)})`; break;
    case 'variant':
      out = `V(${s.alts.map((a) => `${JSON.stringify([a.name, a.ijsonName ?? null])}${k(a.type)}`).join(',')})`;
      break;
    case 'record': out = `R(${JSON.stringify(s.name)})`; break;
    case 'args': out = `G(${s.uid})`; break;
    case 'enum': out = `E(${k(s.under)},${JSON.stringify(s.members.map(([n, v]) => [n, String(v)]))})`; break;
    case 'time': out = `M(${s.id},${k(s.tick)})`; break;
    case 'custom': out = `C(${JSON.stringify(s.id)},${k(s.inner)})`; break;
    case 'box': out = `X(${k(s.inner)})`; break;
    default: return schemaError('typed schema: unresolved definition');
  }
  def.key = out;
  return out;
}

// The key of `at` that speaks of alternative `index`: its name, or its index.
function variantKey(alts: AltShape[], at: Map<string, Position>, index: number): string {
  if (at.has(alts[index].name)) return alts[index].name;
  // (an index that is another alternative's name speaks of that one)
  return alts.some((alt) => alt.name === String(index)) ? '\0' : String(index);
}

// `ann::tree_ok`: the bounds fit the type at every depth, with one message
// per misuse. `top`: the position is the member itself, whose own length
// bound `member()` checks (it may be a nested value, a UTF-8 string, ...).
function checkPosition(input: AnyDef, a: Position, top: boolean, where: string): void {
  let def = resolve(input);
  while (def.shape.k === 'option') def = resolve(def.shape.inner);
  const s = def.shape;
  const fail = (what: string): never => schemaError(`typed schema: ${where}: ${what}`);
  const rawBytes = s.k === 'bytes' && s.bound === 'list' && s.id !== 'utf8';
  if (a.max !== undefined && !top && !(rawBytes || s.k === 'list' || (s.k === 'assoc' && s.max === null)))
    fail('max bounds a rawString or bytes (its bytes) or a list, map or set (its elements); this position is none of those');
  if (a.utf8 && !(s.k === 'bytes' && (s.id === 'utf8' || (s.id === 'string' && s.bound === 'list'))))
    fail('utf8 applies to a rawString; this position is not one');
  if (a.element) {
    if (!(s.k === 'list' || s.k === 'bounded' || s.k === 'array' || (s.k === 'assoc' && s.id === 'set')))
      fail('element bounds the elements of a list, an array or a set; this position is none of those ' +
        '(a map takes keys / values, a tuple, pair or variant takes at)');
    checkPosition((s as { inner?: AnyDef; entry?: AnyDef }).inner ?? (s as { entry: AnyDef }).entry, a.element, false, `${where}, element`);
  }
  if (a.keys || a.values) {
    if (!(s.k === 'assoc' && s.id === 'map'))
      fail('keys / values bound the keys / mapped values of a map; this position is not a map');
    const [key, value] = (resolve((s as { entry: AnyDef }).entry).shape as { items: AnyDef[] }).items;
    if (a.keys) checkPosition(key, a.keys, false, `${where}, keys`);
    if (a.values) checkPosition(value, a.values, false, `${where}, values`);
  }
  if (a.at) {
    if (s.k === 'tuple') {
      for (const [key, child] of a.at) {
        if (!/^(0|[1-9][0-9]*)$/.test(key) || Number(key) >= s.items.length) fail(`at ${key}: not an element of this tuple / pair`);
        checkPosition(s.items[Number(key)], child, false, `${where}, at ${key}`);
      }
    } else if (s.k === 'variant') {
      const seen = new Set<number>();
      for (const [key, child] of a.at) {
        let index = s.alts.findIndex((alt) => alt.name === key);
        if (index < 0 && /^(0|[1-9][0-9]*)$/.test(key) && Number(key) < s.alts.length) index = Number(key);
        if (index < 0) fail(`at ${key}: not an alternative of this variant`);
        if (seen.has(index)) fail(`at ${key}: the alternative is named twice`);
        seen.add(index);
        checkPosition(s.alts[index].type, child, false, `${where}, at ${key}`);
      }
    } else
      fail('at bounds an element of a tuple / pair or an alternative of a variant; this position is none of those ' +
        '(a list, array or set takes element)');
  }
}

class Builder {
  readonly types: IrMember[] = [];
  readonly handlers = new Map<string, PsszCustomHandler>();
  /** The names of the deferred nested types: their bytes are opaque. */
  readonly deferred = new Set<string>();
  /** Typed-only order constraints: named type -> member -> constraints. */
  readonly orders = new Map<string, Map<string, OrderTree>>();
  readonly interfaces: IrInterface[] = [];
  /** Interface -> method -> the order constraints of its result. */
  readonly resultOrders = new Map<string, Map<string, OrderTree>>();
  private packageName = '';
  private packageVersion = '';
  private readonly names = new Map<string, string>();
  private readonly slots = new Map<string, IrMember>();
  private readonly records = new Map<string, AnyDef>();
  private depth = 0;

  private put(name: string, type: IrAnyType): void {
    const slot = this.slots.get(name);
    if (slot) slot.type = type;
    else {
      const member: IrMember = { name, type, attributes: [] };
      this.slots.set(name, member);
      this.types.push(member);
    }
  }

  /** `SchemaBuilder::insert<T>(name)`. */
  insertNamed(name: string, def: AnyDef): void {
    const ref = this.insert(def);
    if (ref.value.type === 'Type' && ref.value.value.type === name) return;
    const taken = this.slots.get(name);
    if (taken) schemaError(`typed schema: the name "${name}" is used twice`);
    this.put(name, ref);
  }

  /** `SchemaBuilder::insert<T>()`: the reference to the interned type. */
  insert(input: AnyDef): IrAnyType {
    const def = resolve(input);
    const key = keyOf(def);
    // An equal custom type declared again still states its handler.
    if (def.shape.k === 'custom') this.bind(def.shape.id, def.shape.handler);
    const known = this.names.get(key);
    if (known !== undefined) {
      if ((def.shape.k === 'record' || def.shape.k === 'transparent') && this.records.get(known) !== def)
        schemaError(`typed schema: two different ${def.shape.k === 'record' ? 'records' : 'types'} are named "${known}"`);
      return ir('Type', { type: known });
    }
    // A record and a transparent type are their names; so is a custom type
    // declared over one.
    let named: AnyDef = def;
    while (named.shape.k === 'custom') named = resolve(named.shape.inner);
    const name = named.shape.k === 'record' || named.shape.k === 'transparent' ? named.shape.name : `@${this.names.size}`;
    if (this.slots.has(name)) schemaError(`typed schema: the name "${name}" is used twice`);
    this.names.set(key, name);
    if (def.shape.k === 'record' || def.shape.k === 'transparent') this.records.set(name, def);
    // The slot is taken before the children are visited (declaration order).
    this.put(name, ir('Type', { type: name }));
    if (++this.depth > 256) schemaError('typed schema: type nesting too deep');
    this.put(name, this.build(def, name));
    --this.depth;
    return ir('Type', { type: name });
  }

  // One handler per custom id, whichever declaration brings it.
  private bind(id: string, handler: PsszCustomHandler | undefined): void {
    if (!handler) return;
    const bound = this.handlers.get(id);
    if (bound && bound !== handler)
      schemaError(`typed schema: custom id "${id}" is declared with two different handlers`);
    this.handlers.set(id, handler);
  }

  // The type itself, not interned; its children are. `name`: the name it is
  // interned under (absent: it is written inline in another type).
  // `owner`: the named type this node is (part of) the definition of.
  private build(def: AnyDef, name?: string, owner = name): IrAnyType {
    const s = def.shape;
    switch (s.k) {
      case 'int': return intIr(s.bits, s.signed);
      case 'float': return ir('Float', { exp: s.exp, mantissa: s.mantissa });
      case 'bool': return ir('Custom', { type: intIr(1, false), id: 'bool' });
      case 'char': return ir('Custom', { type: intIr(8, false), id: 'char' });
      case 'bytes': return ir('Custom', { type: this.byteContainer(s.bound, s.n), id: s.id });
      case 'bits':
        return ir('Custom', {
          type: s.id === 'bitvector'
            ? ir('Array', { type: intIr(1, false), len: s.n })
            : ir('BoundedList', { type: intIr(1, false), maxCount: s.n }),
          id: s.id,
        });
      case 'option': return ir('Option', { type: this.insert(s.inner) });
      case 'list': return ir('List', { type: this.insert(s.inner) });
      case 'bounded': return ir('BoundedList', { type: this.insert(s.inner), maxCount: s.n });
      case 'array': return ir('Array', { type: this.insert(s.inner), len: s.n });
      case 'tuple': return ir('Tuple', { members: s.items.map((item) => this.insert(item)) });
      case 'variant': return this.variant(s.alts, (alt) => this.insert(alt.type));
      case 'record': {
        const members = s.fields.map((field) => this.member(field));
        this.keepOrders(s.fields, owner);
        const attributes: IrAttribute[] = [];
        if (s.options.fixed) attributes.push(attribute('definitionWillNotChange', ''));
        if (s.options.maxFields !== undefined) attributes.push(attribute('maxFields', String(s.options.maxFields)));
        if (s.options.maxDynamicData !== undefined)
          attributes.push(attribute('maxDynamicData', String(s.options.maxDynamicData)));
        return ir('Object', { members, attributes });
      }
      case 'args': {
        // `SchemaBuilder`, psio_annotated_tuple: a Tuple node has no
        // attributes, so the tuple is written as the extensible record it is
        // on the wire, which can carry the caps. `tuple` says it is an
        // argument list and which text form it has (docs/json.md).
        const members = s.fields.map((f) => this.parameter(f));
        this.keepOrders(s.fields, owner);
        const attributes = [attribute('tuple', s.named ? 'named' : 'positional')];
        if (s.maxFields !== undefined) attributes.push(attribute('maxFields', String(s.maxFields)));
        if (s.maxDynamicData !== undefined) attributes.push(attribute('maxDynamicData', String(s.maxDynamicData)));
        return ir('Object', { members, attributes });
      }
      case 'enum':
        return ir('Enum', {
          type: this.build(resolve(s.under)),
          members: s.members.map(([name, value]) => ({ name, value: BigInt.asUintN(64, value) })),
        });
      case 'time': return ir('Custom', { type: this.build(resolve(s.tick)), id: s.id });
      case 'assoc': {
        const type = this.insert(s.entry);
        return ir('Custom', {
          type: s.max === null ? ir('List', { type }) : ir('BoundedList', { type, maxCount: s.max }),
          id: s.id,
        });
      }
      case 'nested':
        // The validation policy is not part of the schema: the codec is
        // told which named nodes are opaque.
        if (s.deferred) {
          if (name === undefined)
            schemaError('typed schema: a deferred nested value cannot be the wire type of a custom type; wrap it in a transparent type');
          this.deferred.add(name!);
        }
        return ir('Nested', { type: this.insert(s.inner) });
      case 'transparent': {
        // A named alias of the member's type ...
        if (!s.position) return this.insert(s.inner);
        // ... or of the bounded type its bounds make of the member: an
        // anonymous type of its own, as a wrapper type is.
        const key = `TM(${JSON.stringify(s.name)})`;
        const known = this.names.get(key);
        if (known !== undefined) return ir('Type', { type: known });
        const anonymous = `@${this.names.size}`;
        this.names.set(key, anonymous);
        this.put(anonymous, ir('Type', { type: anonymous }));
        this.put(anonymous, this.annotated(s.inner, s.position));
        return ir('Type', { type: anonymous });
      }
      case 'custom': {
        this.bind(s.id, s.handler);
        // Over a transparent type: the Custom wraps the member's type.
        const wire = resolve(s.inner);
        return ir('Custom', {
          type: wire.shape.k === 'transparent' ? this.build(wire) : this.build(wire, undefined, owner),
          id: s.id,
        });
      }
      case 'box': return ir('Custom', { type: this.insert(s.inner), id: 'box' });
      default: return schemaError('typed schema: unresolved definition');
    }
  }

  private byteContainer(bound: 'list' | 'max' | 'exact', n: bigint): IrAnyType {
    const type = this.insert(u8);
    return bound === 'list' ? ir('List', { type })
      : bound === 'max' ? ir('BoundedList', { type, maxCount: n })
      : ir('Array', { type, len: n });
  }

  // `annotated_type`: the type of `input` at a position that carries `a` — a
  // `max` is the bounded form of a string / list / map / set, `utf8` the
  // UTF-8 string: the nodes the wrapper types produce. A position with
  // nothing to say is the interned type. `useBound` false: the position's
  // own bound is carried elsewhere (the member attribute lengthBound).
  private annotated(input: AnyDef, a: Position | undefined, useBound = true): IrAnyType {
    const def = resolve(input);
    if (!a) return this.insert(def);
    const s = def.shape;
    if (s.k === 'option') {
      // Looked through: the bounds are the payload's.
      const inner = this.annotated(s.inner, a, useBound);
      return sameRef(inner, this.insert(s.inner)) ? this.insert(def) : ir('Option', { type: inner });
    }
    const max = useBound && a.max !== undefined ? BigInt(a.max) : null;
    const listOf = (type: IrAnyType, bound = max) =>
      bound === null ? ir('List', { type }) : ir('BoundedList', { type, maxCount: bound });
    switch (s.k) {
      case 'bytes':
        if (s.bound !== 'list' || s.id === 'utf8') return this.insert(def);
        if (s.id === 'string') {
          if (max === null && !a.utf8) return this.insert(def);
          return ir('Custom', { type: listOf(this.insert(u8)), id: a.utf8 ? 'utf8' : 'string' });
        }
        return max === null ? this.insert(def) : ir('Custom', { type: listOf(this.insert(u8)), id: 'hex' });
      case 'assoc': {
        const below = s.id === 'map' ? !!(a.keys || a.values) : !!a.element;
        if (max === null && !below) return this.insert(def);
        let entry: IrAnyType;
        if (s.id === 'set') entry = this.annotated(s.entry, a.element);
        else if (below) {
          // (in this order: the key's types are registered first)
          const [key, value] = (resolve(s.entry).shape as { items: AnyDef[] }).items;
          const keyType = this.annotated(key, a.keys);
          entry = ir('Tuple', { members: [keyType, this.annotated(value, a.values)] });
        } else entry = this.insert(s.entry);
        return ir('Custom', { type: listOf(entry, max ?? s.max), id: s.id });
      }
      case 'list':
        if (max === null && !a.element) return this.insert(def);
        return listOf(this.annotated(s.inner, a.element));
      case 'bounded':
        // The wrapper's bound / the array's length is in its type.
        if (!a.element) return this.insert(def);
        return ir('BoundedList', { type: this.annotated(s.inner, a.element), maxCount: s.n });
      case 'array':
        if (!a.element) return this.insert(def);
        return ir('Array', { type: this.annotated(s.inner, a.element), len: s.n });
      case 'tuple':
        if (!a.at) return this.insert(def);
        return ir('Tuple', { members: s.items.map((item, i) => this.annotated(item, a.at!.get(String(i)))) });
      case 'variant':
        if (!a.at) return this.insert(def);
        return this.variant(s.alts, (alt, i) => this.annotated(alt.type, a.at!.get(variantKey(s.alts, a.at!, i))));
      default:
        return this.insert(def);
    }
  }

  private variant(alts: AltShape[], type: (alt: AltShape, index: number) => IrAnyType): IrAnyType {
    return ir('Variant', {
      members: alts.map((alt, i): IrMember => ({
        name: alt.name,
        type: type(alt, i),
        attributes: alt.ijsonName === undefined ? [] : [attribute('ijsonName', alt.ijsonName)],
      })),
      attributes: [],
    });
  }

  // The order constraints of a record's / argument list's members, kept
  // beside the schema: they are not in the IR.
  private keepOrders(fields: FieldShape[], owner: string | undefined): void {
    const table = new Map<string, OrderTree>();
    for (const field of fields) if (field.order) table.set(field.name, field.order);
    if (!table.size) return;
    if (owner === undefined) schemaError('typed schema: sorted / unique / uniqueKeys need a named record');
    this.orders.set(owner!, table);
  }

  // `build_record_member` / `apply_member_annotations`. `inherent`: a
  // bounded UTF-8 string states its own bound (a record member; not a
  // method parameter or result).
  private member(field: FieldShape, inherent = true): IrMember {
    const member: IrMember = { name: field.name, type: this.insert(field.type), attributes: [] };
    const direct = resolve(field.type).shape;
    let target = direct;
    while (target.k === 'option') target = resolve(target.inner).shape;
    const bound = field.lengthBound
      ?? (inherent && direct.k === 'bytes' && direct.id === 'utf8' && direct.bound === 'max' ? { max: Number(direct.n) } : undefined);
    let position = field.position;
    let asAttribute = false;
    if (bound) {
      // Elements of a list, entries of a map / set, bytes of a string or of
      // a nested value's encoding.
      const applies = target.k === 'list' || target.k === 'assoc' || target.k === 'nested' ||
        (target.k === 'bytes' && target.bound !== 'exact' && (target.bound === 'list' || target.id === 'utf8'));
      if (!applies) schemaError(`typed schema: lengthBound does not apply to member "${field.name}"`);
      const onlyMax = bound.max !== undefined && bound.exact === undefined && bound.min === undefined;
      // `has_bounded_form`: the types a `max` turns into a bounded type.
      const boundedForm = target.k === 'list' || (target.k === 'assoc' && target.max === null) ||
        (target.k === 'bytes' && target.bound === 'list' && target.id !== 'utf8');
      if (onlyMax && boundedForm) position = { ...position, max: bound.max };
      else {
        asAttribute = true;
        const items: string[] = [];
        for (const key of ['exact', 'min', 'max'] as const)
          if (bound[key] !== undefined) items.push(`${key}=${bound[key]}`);
        if (items.length) member.attributes.push(attribute('lengthBound', items.join(',')));
      }
    }
    if (position) {
      checkPosition(field.type, position, true, `member "${field.name}"`);
      member.type = this.annotated(field.type, position, !asAttribute);
    }
    if (field.jsonInt) member.attributes.push(attribute('jsonInt', field.jsonInt));
    if (field.ijsonInt) member.attributes.push(attribute('ijsonInt', field.ijsonInt));
    return member;
  }

  // A parameter of an argument tuple: its annotated type (its plain type is
  // not registered first, and a Tuple element has no attributes).
  private parameter(field: FieldShape): IrMember {
    const where = `parameter "${field.name}"`;
    const bound = field.lengthBound;
    let position = field.position;
    if (bound) {
      if (bound.exact !== undefined || bound.min !== undefined)
        schemaError(`typed schema: ${where}: a parameter's length bound is max (the Schema IR has no place for exact / min)`);
      position = { ...position, max: bound.max };
    }
    if (field.jsonInt || field.ijsonInt) schemaError(`typed schema: ${where}: jsonInt / ijsonInt belong to record members`);
    if (position) checkPosition(field.type, position, false, where);
    return { name: field.name, type: this.annotated(field.type, position), attributes: [] };
  }

  /**
   * `SchemaBuilder::insert_interface<T>()`: the `Interface` of the
   * declaration, its parameter and result types interned. `argumentLists`:
   * every argument list is also the named type `<interface>.<method>`, so
   * that a reader of the schema alone has a root for it.
   */
  insertInterface(def: InterfaceDef<any>): void {
    const { name, methods, options } = def.shape;
    if (this.interfaces.some((entry) => entry.name === name)) schemaError(`typed schema: two interfaces are named "${name}"`);
    // (the last package named is the schema's)
    if (options.package) {
      this.packageName = options.package;
      this.packageVersion = options.version ?? '';
    }
    const results = new Map<string, OrderTree>();
    const funcs = methods.map((m): IrFunc => {
      // `build_func`: the plain types of the parameters and of the result ...
      const params = m.fields.map((f): IrMember => ({ name: m.named ? f.name : '', type: this.insert(f.type), attributes: [] }));
      let result = m.result ? this.insert(m.result.type) : null;
      // ... then what the declaration says of them.
      m.fields.forEach((f, i) => {
        const member = this.member(f, false);
        params[i].type = member.type;
        params[i].attributes = member.attributes;
      });
      const attributes: IrAttribute[] = [];
      if (m.result) {
        const member = this.member(m.result, false);
        result = member.type;
        for (const a of member.attributes) attributes.push({ name: `result.${a.name}`, value: a.value });
        if (m.result.order) results.set(m.name, m.result.order);
      }
      if (m.maxFields !== undefined) attributes.push(attribute('maxFields', String(m.maxFields)));
      if (m.maxDynamicData !== undefined) attributes.push(attribute('maxDynamicData', String(m.maxDynamicData)));
      return { name: m.name, params, result, attributes };
    });
    this.interfaces.push({ name, type_names: [], funcs, attributes: [] });
    if (results.size) this.resultOrders.set(name, results);
    if (options.argumentLists !== false) for (const m of methods) this.insertNamed(`${name}.${m.name}`, m.args);
  }

  schema(requireHandlers?: boolean): PsszSchema {
    const schema: IrSchema = {
      package: { name: this.packageName, version: this.packageVersion, attributes: [] },
      interfaces: this.interfaces, worlds: [], uses: [],
      types: this.types,
      ir_version: IR_VERSION,
    };
    return new PsszSchema(schema, {
      handlers: this.handlers, requireHandlers, deferredNested: this.deferred, orders: this.orders,
    });
  }
}

/**
 * The Schema IR of named definitions: what C++ `psio::SchemaBuilder` builds
 * from `insert<T>(name)` of the equivalent types in the same order (records
 * under their own names, every other type interned as `@N`). The result
 * carries the handlers of the custom types it reaches; `schema.ir` is the
 * IR, `schema.encode()` its PSSZ bytes, `schema.type(name)` a codec.
 */
export function generateSchema(
  types: { [name: string]: TypeDef<any, any> },
  options?: {
    requireHandlers?: boolean;
    /**
     * Interfaces of the schema (`defineInterface`), inserted before the
     * named types, as C++ `insert_interface<T>()` calls are.
     */
    interfaces?: readonly InterfaceDef<any>[];
  },
): PsszSchema {
  return declared(() => {
    const builder = new Builder();
    for (const entry of options?.interfaces ?? []) {
      if (!(entry instanceof InterfaceDef)) schemaError('typed schema: interfaces must be interface definitions');
      builder.insertInterface(entry);
    }
    for (const name of Object.keys(types)) {
      if (!(types[name] instanceof TypeDef)) schemaError(`typed schema: "${name}" is not a type definition`);
      builder.insertNamed(name, types[name]);
    }
    return builder.schema(options?.requireHandlers);
  });
}

// ── TypeDef ─────────────────────────────────────────────────────────────────

/**
 * A type declared in TypeScript. `T` is the type of its decoded value and
 * `V` what a view returns for it. It has the operations of `PsszType`
 * (automatic width profile); `type(options)` gives another profile.
 */
export class TypeDef<T, V = T> {
  /** @internal */
  key?: string;
  private root?: IrAnyType;
  private own?: PsszSchema;
  private auto?: PsszType<T, V>;

  /** @internal */
  constructor(/** @internal */ readonly shape: Shape) {}

  /** The Schema IR of this type and everything it reaches. */
  get schema(): PsszSchema {
    return (this.own ??= declared(() => {
      const builder = new Builder();
      this.root = builder.insert(this);
      return builder.schema();
    }));
  }

  /** The name this type has in `schema` (a record's own name, or `@N`). */
  get name(): string {
    void this.schema;
    return (this.root!.value.value as { type: string }).type;
  }

  /** The codec of this type under a width profile / handler policy. */
  type(options?: TypeDefOptions): PsszType<T, V> {
    if (!options || (options.width === undefined && options.requireHandlers === undefined && options.nestedBytes === undefined))
      return (this.auto ??= this.schema.type<T, V>(this.name));
    return this.schema.type<T, V>(this.name, options);
  }

  /** The offset width of this type's own header / slots / offset table. */
  get width(): number {
    return this.type().width;
  }
  /** The inferred bound (`inferred_bound`), what widths are chosen from; `null` when unbounded. See `PsszType.inferredBound`. */
  get inferredBound(): bigint | null {
    return this.type().inferredBound;
  }
  /** The largest encoding (`max_encoded_size`); `null` when there is no finite maximum. See `PsszType.maxEncodedSize`. */
  get maxEncodedSize(): bigint | null {
    return this.type().maxEncodedSize;
  }
  /**
   * Whether this capped record holds a record wider than its cap's width
   * class (C++ `psio::pssz_child_wider_than_cap_v<T>`): a query, not an
   * error. See `PsszType.childWiderThanCap`.
   */
  get childWiderThanCap(): boolean {
    return this.type().childWiderThanCap;
  }
  /** The canonical PSSZ bytes of `value`. */
  encode(value: T): Uint8Array {
    return this.type().encode(value);
  }
  /** Decodes fully validated PSSZ bytes. */
  decode(bytes: Uint8Array, options?: PsszDecodeOptions): T {
    return this.type().decode(bytes, options);
  }
  /** Checks PSSZ bytes against the type. Never throws. */
  validate(bytes: Uint8Array): PsszValidation {
    try {
      return this.type().validate(bytes);
    } catch (e) {
      return { ok: false, error: e as PsszError };
    }
  }
  /** Reads the value in place (see `PsszType.view`). */
  view(bytes: Uint8Array): V {
    return this.type().view(bytes);
  }
  /** The `key` bytes of `value`: bytewise order is value order. */
  encodeKey(value: T): Uint8Array {
    return this.type().encodeKey(value);
  }
  /** Decodes key bytes (checked). */
  decodeKey(bytes: Uint8Array): T {
    return this.type().decodeKey(bytes);
  }
  /** Checks key bytes against the type. Never throws. */
  validateKey(bytes: Uint8Array): PsszKeyValidation {
    try {
      return this.type().validateKey(bytes);
    } catch (e) {
      return { ok: false, error: e as PsszError };
    }
  }
  /** The key of the first `elements.length` elements of a tuple / fields of a record. */
  encodeKeyPrefix(elements: unknown[]): Uint8Array {
    return this.type().encodeKeyPrefix(elements);
  }
  /** JSON / I-JSON text -> value. */
  jsonToValue(text: string | Uint8Array, format: JsonFormat = 'ijson'): T {
    return this.type().jsonToValue(text, format);
  }
  /** Value -> JSON / I-JSON text. */
  valueToJson(value: T, format: JsonFormat = 'ijson'): string {
    return this.type().valueToJson(value, format);
  }
  /** JSON / I-JSON text -> canonical PSSZ bytes. */
  jsonToPssz(text: string | Uint8Array, format: JsonFormat = 'ijson'): Uint8Array {
    return this.type().jsonToPssz(text, format);
  }
  /** PSSZ bytes (fully validated) -> JSON / I-JSON text. */
  psszToJson(bytes: Uint8Array, format: JsonFormat = 'ijson', options?: PsszDecodeOptions): string {
    return this.type().psszToJson(bytes, format, options);
  }
}

/** The decoded value type of a definition. */
export type Infer<D> = D extends TypeDef<infer T, any> ? T : never;
/** What a view returns for a definition. */
export type InferView<D> = D extends TypeDef<any, infer V> ? V : never;

// ── Scalars ─────────────────────────────────────────────────────────────────

const int = <T>(bits: number, signed: boolean) => new TypeDef<T>({ k: 'int', bits, signed });

export const bool = new TypeDef<boolean>({ k: 'bool' });
/** One byte of text (C++ `char`): a number 0..255. */
export const char = new TypeDef<number>({ k: 'char' });
export const u8 = int<number>(8, false);
export const u16 = int<number>(16, false);
export const u32 = int<number>(32, false);
export const u64 = int<bigint>(64, false);
export const u128 = int<bigint>(128, false);
export const u256 = int<bigint>(256, false);
export const i8 = int<number>(8, true);
export const i16 = int<number>(16, true);
export const i32 = int<number>(32, true);
export const i64 = int<bigint>(64, true);
export const i128 = int<bigint>(128, true);
export const f32 = new TypeDef<number>({ k: 'float', exp: 8, mantissa: 24 });
export const f64 = new TypeDef<number>({ k: 'float', exp: 11, mantissa: 53 });

// ── Text and bytes ──────────────────────────────────────────────────────────

function count(n: number | bigint, what: string): bigint {
  const ok = typeof n === 'bigint' ? n >= 0n && n < 1n << 64n : Number.isSafeInteger(n) && n >= 0;
  if (!ok) schemaError(`typed schema: ${what} must be an unsigned integer`);
  return BigInt(n);
}
const byteDef = <T>(id: 'utf8' | 'string' | 'hex', bound: 'list' | 'max' | 'exact', n: bigint) =>
  new TypeDef<T>({ k: 'bytes', id, bound, n });

/** Text: UTF-8 on the wire (validated), a `string` in JavaScript. C++ `utf8_string<>`, Rust `String`. */
export const utf8 = byteDef<string>('utf8', 'list', 0n);
/** Text of at most `max` UTF-8 bytes (C++ `utf8_string<N>`). */
export const boundedUtf8 = (max: number) => byteDef<string>('utf8', 'max', count(max, 'a bound'));
/**
 * A raw string (C++ `std::string`, Rust `RawString`): any bytes, a
 * `Uint8Array` in JavaScript. Text holds its UTF-8.
 */
export const rawString = byteDef<Uint8Array>('string', 'list', 0n);
/** Bytes: a `Uint8Array` (hex in I-JSON). */
export const bytes = byteDef<Uint8Array>('hex', 'list', 0n);
/** At most `max` bytes. */
export const boundedBytes = (max: number) => byteDef<Uint8Array>('hex', 'max', count(max, 'a bound'));
/** Exactly `length` bytes (a hash, a public key). */
export const fixedBytes = (length: number) => byteDef<Uint8Array>('hex', 'exact', count(length, 'a length'));
/** Exactly `bits` bits, packed. */
export const bitvector = (bits: number) => new TypeDef<BitsValue>({ k: 'bits', id: 'bitvector', n: count(bits, 'a bit count') });
/** At most `maxBits` bits, packed. */
export const bitlist = (maxBits: number) => new TypeDef<BitsValue>({ k: 'bits', id: 'bitlist', n: count(maxBits, 'a bit count') });

// ── Containers ──────────────────────────────────────────────────────────────

function isDef(x: unknown, what: string): AnyDef {
  if (!(x instanceof TypeDef)) schemaError(`typed schema: ${what} must be a type definition`);
  return x as AnyDef;
}

/**
 * `null` or a value. An optional of an optional holds `null` or
 * `{ some: inner }`.
 */
export function optional<T, V>(
  inner: TypeDef<T, V>,
): null extends T ? TypeDef<Some<T> | null, Some<V> | null> : TypeDef<T | null, V | null> {
  return new TypeDef({ k: 'option', inner: isDef(inner, 'an optional payload') }) as never;
}

/** A list of any length. */
export function list<T, V>(element: TypeDef<T, V>): TypeDef<T[], ListView<T[], V>> {
  return new TypeDef({ k: 'list', inner: isDef(element, 'a list element') });
}

/** A list of at most `max` elements; the bound narrows offset widths. */
export function boundedList<T, V>(element: TypeDef<T, V>, max: number | bigint): TypeDef<T[], ListView<T[], V>> {
  return new TypeDef({ k: 'bounded', inner: isDef(element, 'a list element'), n: count(max, 'a bound') });
}

/** Exactly `length` elements (no count on the wire). */
export function array<T, V>(element: TypeDef<T, V>, length: number | bigint): TypeDef<T[], ListView<T[], V>> {
  return new TypeDef({ k: 'array', inner: isDef(element, 'an array element'), n: count(length, 'a length') });
}

/** C++ `psio::max_tuple_elements`: a pair is 2, a method's argument list is a tuple. */
const MAX_TUPLE_ELEMENTS = 255;

type ValuesOf<D extends readonly AnyDef[]> = { -readonly [I in keyof D]: Infer<D[I]> };
type ViewsOf<D extends readonly AnyDef[]> = { -readonly [I in keyof D]: InferView<D[I]> };

/**
 * Positional elements of different types. Trailing optionals may be absent
 * on the wire. A tuple has at most 255 elements (`docs/pssz-spec.md` §8.4).
 */
export function tuple<D extends readonly AnyDef[]>(...elements: D): TypeDef<ValuesOf<D>, TupleView<ValuesOf<D>, ViewsOf<D>>> {
  if (elements.length > MAX_TUPLE_ELEMENTS) schemaError('typed schema: a tuple has at most 255 elements');
  return new TypeDef({ k: 'tuple', items: elements.map((e) => isDef(e, 'a tuple element')), pair: false });
}

/**
 * A pair (C++ `std::pair`): the 2-tuple `[first, second]`, with the tuple's
 * bytes, key, text and view. It is the tuple's Schema IR node; declared with
 * `pair` it is a type of its own in a generated schema, as in C++.
 */
export function pair<A, VA, B, VB>(
  first: TypeDef<A, VA>, second: TypeDef<B, VB>,
): TypeDef<[A, B], TupleView<[A, B], [VA, VB]>> {
  return new TypeDef({ k: 'tuple', items: [isDef(first, 'a pair element'), isDef(second, 'a pair element')], pair: true });
}

/** Options of `map` and `set`. */
export interface AssocOptions {
  /**
   * The Schema IR identity of a C++ unordered container
   * (`std::unordered_map` / `std::unordered_set`): the same node, bytes and
   * values, but a type of its own in a generated schema, as in C++. Only
   * for reproducing a C++ schema byte for byte.
   */
  unordered?: boolean;
  /**
   * At most `max` entries / elements: the map / set over a bounded list
   * (the node a `field(..., { lengthBound: { max } })` gives a member).
   */
  max?: number | bigint;
}

function assocMax(options: AssocOptions | undefined): bigint | null {
  return options?.max === undefined ? null : count(options.max, 'a bound');
}

/**
 * A map (`docs/pssz-spec.md` §5.9): on the wire the list of its
 * `[key, value]` entries in strictly ascending order of the key bytes of the
 * key (`encodeKey`), with no repeated key. Decodes to a `Map` whose
 * iteration order is that order; encodes a `Map` — or, at run time, any
 * iterable of `[key, value]` entries — in any order. Keys that are not
 * primitives (tuples, records, raw strings) are distinct objects: a `Map`
 * finds them by identity, so look such an entry up by iterating.
 *
 * In text a map is an object when its key is a string by type (`utf8`,
 * `rawString`, a custom type with a text form) and an array of `[key, value]`
 * pairs otherwise. A view of a map is the `ListView` of its entries.
 */
export function map<K, VK, T, VT>(
  key: TypeDef<K, VK>, value: TypeDef<T, VT>, options: AssocOptions = {},
): TypeDef<Map<K, T>, ListView<Map<K, T>, TupleView<[K, T], [VK, VT]>>> {
  const entry = tuple(isDef(key, 'a map key'), isDef(value, 'a mapped type'));
  return new TypeDef({ k: 'assoc', id: 'map', entry, unordered: !!options?.unordered, max: assocMax(options) });
}

/**
 * A set (`docs/pssz-spec.md` §5.9): on the wire the list of its elements in
 * strictly ascending order of their key bytes, with no repeated element.
 * Decodes to a `Set` in that order; encodes a `Set` — or, at run time, any
 * iterable — in any order. In text it is an array. A view of a set is the
 * `ListView` of its elements.
 */
export function set<K, VK>(element: TypeDef<K, VK>, options: AssocOptions = {}): TypeDef<Set<K>, ListView<Set<K>, VK>> {
  return new TypeDef({
    k: 'assoc', id: 'set', entry: isDef(element, 'a set element'), unordered: !!options?.unordered, max: assocMax(options),
  });
}

/**
 * A nested value (`docs/pssz-spec.md` §5.8): on the wire a byte list holding
 * the PSSZ encoding of an `inner` value (always in the automatic profile),
 * so a record with a `nested(T)` member and the same record with a `bytes`
 * member are byte-identical.
 *
 * `nested(T)`: the value is the inner value, and every reader validates the
 * bytes as a `T`; a view of it is the inner value's view. Encoding writes
 * the canonical encoding of the value: fields a newer writer appended to an
 * inner record are not carried through a decode / encode round trip.
 *
 * `nested(T, { deferred: true })` (C++ `nested<T, nested_validation::deferred>`):
 * the value is the bytes (a `Uint8Array`), which an enclosing reader does
 * not look at and a writer writes as they are; decode them with `T` where
 * the value is needed. Same wire, key and Schema IR node.
 *
 * Its text is the inner value's own text in both cases; its key is the key
 * of the byte list (nested values order by their inner bytes).
 */
export function nested<T, V>(inner: TypeDef<T, V>, options?: { deferred?: false }): TypeDef<T, V>;
export function nested<T, V>(inner: TypeDef<T, V>, options: { deferred: true }): TypeDef<Uint8Array, Uint8Array>;
export function nested(inner: AnyDef, options?: { deferred?: boolean }): AnyDef {
  return new TypeDef({ k: 'nested', inner: isDef(inner, 'a nested type'), deferred: !!options?.deferred });
}

/**
 * A transparent type (`docs/pssz-spec.md` §5.10): a named type that IS its
 * one member on the wire, in keys, in text and as a JavaScript value — no
 * header, no slot. What it keeps is its name: in the Schema IR it is the
 * named alias `name` of the member's type. Give it a text form or an
 * invariant with `custom(id, transparent(name, member), handler)`.
 */
export function transparent<T, V>(name: string, member: TypeDef<T, V> | Field<T, V>): TypeDef<T, V> {
  if (typeof name !== 'string' || name === '' || name.startsWith('@'))
    schemaError('typed schema: a transparent type needs a name (not starting with "@")');
  let position: Position | undefined;
  if (member instanceof Field) {
    // The bounds that make a type a bounded type; anything else a member
    // can say has no place in a type (docs/pssz-spec.md 5.10).
    const where = `the member of transparent type "${name}"`;
    for (const key of ['lengthBound', 'jsonInt', 'ijsonInt'] as const)
      if (member.options[key] !== undefined)
        schemaError(`typed schema: ${where}: ${key} is a record member's (use max / utf8 / element / keys / values / at, or an invariant)`);
    if (orderOf(member.options, where)) schemaError(`typed schema: ${where}: sorted / unique / uniqueKeys are a record member's (use an invariant)`);
    position = positionOf(member.options, where);
    if (position) checkPosition(member.type, position, false, where);
  }
  const inner = isDef(member instanceof Field ? member.type : member, 'a transparent member');
  // Presence belongs to the record that holds the value.
  if (inner.shape.k === 'option') schemaError('typed schema: the member of a transparent type is not an optional');
  return new TypeDef({ k: 'transparent', name, inner, position });
}

/** A value that does not count toward its container's size bound (C++ / Rust `Box<T>`). */
export function box<T, V>(inner: TypeDef<T, V>): TypeDef<T, V> {
  return new TypeDef({ k: 'box', inner: isDef(inner, 'a boxed type') });
}

/**
 * A definition resolved when first used: for a recursive type, whose own
 * constant cannot be named inside its initializer.
 *
 *     interface Tree { children: Tree[] }
 *     const Tree: TypeDef<Tree, RecordView<Tree>> = record('Tree', { children: list(lazy(() => Tree)) });
 */
export function lazy<T, V = T>(get: () => TypeDef<T, V>): TypeDef<T, V> {
  if (typeof get !== 'function') schemaError('typed schema: lazy() takes a function');
  return new TypeDef({ k: 'lazy', get });
}

// ── Records ─────────────────────────────────────────────────────────────────

/** A record member with options (`field`). */
export class Field<T, V = T> {
  /** @internal */
  constructor(readonly type: TypeDef<T, V>, readonly options: FieldOptions) {}
}

// What bounds change in the JavaScript value: a `rawString` made UTF-8 is a
// `string`. Everything else keeps its type.
type Sub<O, K extends string> = O extends { readonly [P in K]: infer S } ? S : {};
type At<O, I> = O extends { readonly at: infer A }
  ? I extends keyof A ? A[I]
    : I extends `${infer N extends number}` ? (N extends keyof A ? A[N] : {})
    : {}
  : {};
type EntryOf<O> = { at: { 0: Sub<O, 'keys'>; 1: Sub<O, 'values'> } };

/**
 * The decoded value of a member of value type `T` under the bounds `O`
 * (`field(type, O)`): `T`, with a `Uint8Array` at every position that says
 * `utf8: true` replaced by `string`.
 */
export type Bounded<T, O> =
  T extends null ? null
  : T extends Uint8Array ? (O extends { readonly utf8: true } ? string : T)
  : T extends Map<infer K, infer W> ? Map<Bounded<K, Sub<O, 'keys'>>, Bounded<W, Sub<O, 'values'>>>
  : T extends Set<infer E> ? Set<Bounded<E, Sub<O, 'element'>>>
  : T extends readonly unknown[]
    ? (number extends T['length'] ? Bounded<T[number], Sub<O, 'element'>>[] : { -readonly [I in keyof T]: Bounded<T[I], At<O, I>> })
  : O extends { readonly at: object }
    ? (T extends { type: infer N; value: infer X } ? { type: N; value: Bounded<X, At<O, N>> } : T)
  : T;

/** What a view returns for a member of view type `V` under the bounds `O`. */
export type BoundedView<V, O> =
  V extends RecordView<any, any> ? V
  : V extends ListView<infer T, infer E>
    ? ListView<Bounded<T, O>, BoundedView<E, T extends Map<any, any> ? EntryOf<O> : Sub<O, 'element'>>>
  : V extends TupleView<infer T, infer E extends readonly unknown[]>
    ? TupleView<Bounded<T, O>, { -readonly [I in keyof E]: BoundedView<E[I], At<O, I>> }>
  : V extends VariantView<infer T, infer K, infer X> ? VariantView<Bounded<T, O>, K, BoundedView<X, At<O, K>>>
  : Bounded<V, O>;

/**
 * A record member (or a method parameter) with options: bounds at any depth
 * of its type (`max`, `utf8`, `element`, `keys`, `values`, `at`), a length
 * bound with `min` / `exact`, an integer text form.
 *
 *     record('Post', {
 *       title: field(rawString, { utf8: true, max: 80 }),                    // string
 *       tags:  field(list(rawString), { max: 8, element: { utf8: true, max: 16 } }),  // string[]
 *       seen:  field(map(rawString, list(u32)), { max: 4, keys: { max: 16 }, values: { max: 2 } }),
 *     });
 */
export function field<T, V, const O extends FieldOptions>(
  type: TypeDef<T, V>, options: O,
): Field<Bounded<T, O>, BoundedView<V, O>> {
  return new Field(isDef(type, 'a field type') as never, options ?? {});
}

/** A variant alternative with options (`alt`). */
export class Alt<T, V = T> {
  /** @internal */
  constructor(readonly type: TypeDef<T, V>, readonly options: AltOptions) {}
}
/** A variant alternative with an I-JSON name. */
export function alt<T, V>(type: TypeDef<T, V>, options: AltOptions): Alt<T, V> {
  return new Alt(isDef(type, 'an alternative type'), options ?? {});
}

type MemberInput = TypeDef<any, any> | Field<any, any> | Alt<any, any>;
type MemberValue<M> = M extends TypeDef<infer T, any> ? T : M extends { type: TypeDef<infer T, any> } ? T : never;
type MemberView<M> = M extends TypeDef<any, infer V> ? V : M extends { type: TypeDef<any, infer V> } ? V : never;
type Name<K> = K extends string | number ? `${K}` : never;

// JavaScript lists integer-like keys first, in numeric order: such a name
// keeps its declared position only when it is that position.
function memberNames(members: object, what: string): string[] {
  const names = Object.keys(members);
  names.forEach((name, i) => {
    if (/^(0|[1-9][0-9]*)$/.test(name) && name !== String(i))
      schemaError(`typed schema: ${what} "${name}" is an integer that is not its position; JavaScript would reorder it`);
  });
  return names;
}

function intText(x: unknown, what: string): IntText | undefined {
  if (x !== undefined && x !== 'number' && x !== 'string') schemaError(`typed schema: ${what} is "number" or "string"`);
  return x as IntText | undefined;
}

const bound32 = (x: unknown) => typeof x === 'number' && Number.isInteger(x) && x >= 0 && x <= 0xffffffff;

// The bounds written for one position; undefined when they say nothing.
// `top`: the member itself, whose `max` is its length bound.
function positionOf(o: PositionOptions, where: string, top = false, depth = 0): Position | undefined {
  if (typeof o !== 'object' || o === null) schemaError(`typed schema: ${where}: bounds must be an object`);
  if (depth > 64) schemaError(`typed schema: ${where}: bounds nested too deep`);
  const allowed = top
    ? ['max', 'utf8', 'element', 'keys', 'values', 'at', 'sorted', 'unique', 'uniqueKeys', 'lengthBound', 'jsonInt', 'ijsonInt']
    : ['max', 'utf8', 'element', 'keys', 'values', 'at', 'sorted', 'unique', 'uniqueKeys'];
  for (const key of Object.keys(o))
    if (!allowed.includes(key))
      schemaError(`typed schema: ${where}: unknown option "${key}"` +
        (key === 'lengthBound' ? ' (below the member itself a length bound is max)' : ''));
  const out: Position = {};
  if (o.max !== undefined) {
    if (!bound32(o.max)) schemaError(`typed schema: ${where}: max must be a 32-bit unsigned integer`);
    if (!top) out.max = o.max;
  }
  if (o.utf8 !== undefined) {
    if (typeof o.utf8 !== 'boolean') schemaError(`typed schema: ${where}: utf8 is true or false`);
    if (o.utf8) out.utf8 = true;
  }
  for (const key of ['element', 'keys', 'values'] as const) {
    if (o[key] === undefined) continue;
    const child = positionOf(o[key]!, `${where}, ${key}`, false, depth + 1);
    if (child) out[key] = child;
  }
  if (o.at !== undefined) {
    if (typeof o.at !== 'object' || o.at === null) schemaError(`typed schema: ${where}: at is an object of bounds by index or name`);
    const at = new Map<string, Position>();
    for (const key of Object.keys(o.at)) {
      const child = positionOf(o.at[key], `${where}, at ${key}`, false, depth + 1);
      if (child) at.set(key, child);
    }
    if (at.size) out.at = at;
  }
  return Object.keys(out).length ? out : undefined;
}

// The order constraints written for one position and below it; undefined
// when there are none. (`positionOf` checks the options' shape.)
function orderOf(o: PositionOptions, where: string, depth = 0): OrderTree | undefined {
  if (typeof o !== 'object' || o === null || depth > 64) return undefined;
  const out: { -readonly [K in keyof OrderTree]: OrderTree[K] } = {};
  if (o.sorted !== undefined && o.sorted !== true && o.sorted !== false && o.sorted !== 'descending')
    schemaError(`typed schema: ${where}: sorted is true, false or "descending"`);
  for (const key of ['unique', 'uniqueKeys'] as const)
    if (o[key] !== undefined && typeof o[key] !== 'boolean') schemaError(`typed schema: ${where}: ${key} is true or false`);
  if (o.sorted || o.unique) out.sorted = { direction: o.sorted === 'descending' ? -1 : 1, strict: !!o.unique };
  if (o.uniqueKeys) out.uniqueKeys = true;
  for (const key of ['element', 'keys', 'values'] as const) {
    const child = o[key] === undefined ? undefined : orderOf(o[key]!, `${where}, ${key}`, depth + 1);
    if (child) out[key] = child;
  }
  if (typeof o.at === 'object' && o.at !== null) {
    const at = new Map<string, OrderTree>();
    for (const key of Object.keys(o.at)) {
      const child = orderOf(o.at[key], `${where}, at ${key}`, depth + 1);
      if (child) at.set(key, child);
    }
    if (at.size) out.at = at;
  }
  return Object.keys(out).length ? out : undefined;
}

// The order constraints fit the type at every depth (the codec checks that
// the elements they compare have a key encoding).
function checkOrder(input: AnyDef, tree: OrderTree, where: string): void {
  let def = resolve(input);
  while (def.shape.k === 'option') def = resolve(def.shape.inner);
  const s = def.shape;
  const fail = (what: string): never => schemaError(`typed schema: ${where}: ${what}`);
  const sequence = s.k === 'list' || s.k === 'bounded' || s.k === 'array';
  if ((tree.sorted || tree.uniqueKeys) && !sequence)
    fail('sorted / unique / uniqueKeys constrain the elements of a list or an array (a map or set is ordered by type)');
  if (tree.element) {
    if (!(sequence || (s.k === 'assoc' && s.id === 'set'))) fail('element constrains the elements of a list, an array or a set');
    checkOrder((s as { inner?: AnyDef; entry?: AnyDef }).inner ?? (s as { entry: AnyDef }).entry, tree.element, `${where}, element`);
  }
  if (tree.keys || tree.values) {
    if (!(s.k === 'assoc' && s.id === 'map')) fail('keys / values constrain the keys / mapped values of a map');
    const [key, value] = (resolve((s as { entry: AnyDef }).entry).shape as { items: AnyDef[] }).items;
    if (tree.keys) checkOrder(key, tree.keys, `${where}, keys`);
    if (tree.values) checkOrder(value, tree.values, `${where}, values`);
  }
  if (tree.at) {
    for (const [key, child] of tree.at) {
      let target: AnyDef | undefined;
      if (s.k === 'tuple') target = /^(0|[1-9][0-9]*)$/.test(key) ? s.items[Number(key)] : undefined;
      else if (s.k === 'variant')
        target = (s.alts.find((alt) => alt.name === key) ?? (/^(0|[1-9][0-9]*)$/.test(key) ? s.alts[Number(key)] : undefined))?.type;
      else fail('at constrains an element of a tuple / pair or an alternative of a variant');
      if (!target) fail(`at ${key}: no such element or alternative`);
      checkOrder(target!, child, `${where}, at ${key}`);
    }
  }
}

function fieldShapes(fields: { [name: string]: MemberInput }, what: string): FieldShape[] {
  return memberNames(fields, `${what} name`).map((key): FieldShape => {
    const f = fields[key];
    if (f instanceof Field) {
      const o = f.options;
      const where = `${what} "${key}"`;
      const position = positionOf(o, where, true);
      const order = orderOf(o, where);
      if (order) checkOrder(f.type, order, where);
      let b = o.lengthBound;
      if (b !== undefined) {
        if (typeof b !== 'object' || b === null) schemaError(`typed schema: lengthBound of "${key}" must be an object`);
        for (const k of ['max', 'min', 'exact'] as const)
          if (b[k] !== undefined && !bound32(b[k]))
            schemaError(`typed schema: lengthBound.${k} of "${key}" must be a 32-bit unsigned integer`);
        if (o.max !== undefined && b.max !== undefined && o.max !== b.max)
          schemaError(`typed schema: ${where} states max twice`);
      }
      if (o.max !== undefined) b = { ...b, max: o.max };
      return {
        name: key, type: f.type, lengthBound: b, position, order,
        jsonInt: intText(o.jsonInt, 'jsonInt'), ijsonInt: intText(o.ijsonInt, 'ijsonInt'),
      };
    }
    return { name: key, type: isDef(f, `${what} "${key}"`) };
  });
}

/**
 * A record: named fields in declaration order, an object in JavaScript. It
 * can grow by appending optional fields: a reader of the older definition
 * skips them, a reader of the newer one sees `null` for fields an older
 * writer did not store. The name is the record's name in the Schema IR.
 */
export function record<F extends { [name: string]: TypeDef<any, any> | Field<any, any> }>(
  name: string, fields: F, options: RecordOptions = {},
): TypeDef<{ [K in keyof F]: MemberValue<F[K]> }, RecordView<{ [K in keyof F]: MemberValue<F[K]> }, { [K in keyof F]: MemberView<F[K]> }>> {
  if (typeof name !== 'string' || name === '' || name.startsWith('@'))
    schemaError('typed schema: a record needs a name (not starting with "@")');
  if (typeof fields !== 'object' || fields === null) schemaError('typed schema: record fields must be an object');
  const shapes = fieldShapes(fields, 'field');
  const { fixed, maxDynamicData, maxFields } = options;
  if (maxDynamicData !== undefined) count(maxDynamicData, 'maxDynamicData');
  fieldCount(shapes.length, maxFields, `record "${name}"`, 'fields');
  return new TypeDef({ k: 'record', name, fields: shapes, options: { fixed: !!fixed, maxDynamicData, maxFields } });
}

// docs/pssz-spec.md 8.4, 16: at most 255 fields / parameters, and no more
// than a `maxFields` of at most 255.
function fieldCount(n: number, maxFields: unknown, where: string, what: 'fields' | 'parameters'): void {
  if (n > MAX_TUPLE_ELEMENTS) schemaError(`typed schema: ${where} has more than 255 ${what}`);
  if (maxFields === undefined) return;
  if (typeof maxFields !== 'number' || !Number.isInteger(maxFields) || maxFields < 0 || maxFields > 255)
    schemaError(`typed schema: maxFields of ${where} must be an integer from 0 to 255`);
  if (n > (maxFields as number)) schemaError(`typed schema: ${where} has more ${what} than its maxFields (${maxFields})`);
}

/** Options of `argumentTuple`. */
export interface ArgumentTupleOptions {
  /**
   * The cap on the encoded arguments (`docs/pssz-spec.md` §16). It fixes the
   * tuple's offset width for the method's lifetime: declare it from the
   * start on a method that may gain optional trailing arguments.
   */
  maxDynamicData?: number | bigint;
  /**
   * A cap on the number of parameters the method may ever have (at most
   * 255; `docs/pssz-spec.md` §16).
   */
  maxFields?: number;
}

let argumentTuples = 0;

type Parameter = TypeDef<any, any> | Field<any, any>;
/** The parameters of a method: named (an object) or positional (an array). */
export type MethodParameters = { [name: string]: Parameter } | [Parameter, ...Parameter[]] | [];
/**
 * The JavaScript value of an argument list: an object keyed by parameter
 * name, or — parameters without names — the array of the arguments.
 */
export type ArgumentsOf<P> = P extends readonly unknown[]
  ? { -readonly [I in keyof P]: MemberValue<P[I]> }
  : { -readonly [K in keyof P]: MemberValue<P[K]> };
/** What a view of an argument list returns for each parameter. */
export type ArgumentViewsOf<P> = P extends readonly unknown[]
  ? { -readonly [I in Extract<keyof P, `${number}`>]: MemberView<P[I]> }
  : { -readonly [K in keyof P]: MemberView<P[K]> };

function parameterShapes(parameters: MethodParameters, maxFields: unknown): { fields: FieldShape[]; named: boolean } {
  if (typeof parameters !== 'object' || parameters === null)
    schemaError('typed schema: parameters must be an object (named) or an array (positional)');
  const fields = fieldShapes(parameters as { [name: string]: MemberInput }, 'parameter');
  if (fields.length > MAX_TUPLE_ELEMENTS) schemaError('typed schema: a method has at most 255 parameters');
  fieldCount(fields.length, maxFields, 'the method', 'parameters');
  for (const f of fields) {
    const where = `parameter "${f.name}"`;
    if (f.lengthBound && (f.lengthBound.exact !== undefined || f.lengthBound.min !== undefined))
      schemaError(`typed schema: ${where}: a parameter's length bound is max (the Schema IR has no place for exact / min)`);
    if (f.jsonInt || f.ijsonInt) schemaError(`typed schema: ${where}: jsonInt / ijsonInt belong to record members`);
  }
  return { fields, named: !Array.isArray(parameters) };
}

/**
 * The argument tuple of a method (`docs/pssz-spec.md` §16; C++
 * `psio::reflect<T>::method_arg_tuple<I>`): the tuple of the parameters,
 * each bounded by what `field(type, { max, utf8, element, ... })` says of
 * it, the whole capped by `maxDynamicData`. Its bytes are those of the
 * `tuple` of the same bounded types; an explicit cap rules its offset
 * width, and without one the width follows from the parameters' bounds.
 *
 * MethodParameters given as an object have names: the arguments are an object
 * keyed by parameter name, in JavaScript and in text (`{"to":"bob",
 * "amount":5}`), and a text reader also takes the positional array
 * (`["bob",5]`). MethodParameters given as an array have none: the arguments are
 * the array, in JavaScript and in text (`docs/json.md` "Method arguments").
 * Binary is the positional tuple either way.
 *
 * In the Schema IR the tuple is the `Object` of the parameters carrying
 * `tuple` = `"named"` / `"positional"` (members `"0"`, `"1"`, ...), then
 * `maxFields` and `maxDynamicData` (a `Tuple` node has no attributes), as
 * C++ writes it. Every `argumentTuple(...)` is a type of its own (`@N` in a
 * generated schema), as every C++ method's is. A method may gain optional
 * trailing parameters, as a record gains optional fields.
 */
export function argumentTuple<P extends MethodParameters>(
  parameters: P, options: ArgumentTupleOptions = {},
): TypeDef<ArgumentsOf<P>, RecordView<ArgumentsOf<P>, ArgumentViewsOf<P>>> {
  const { fields, named } = parameterShapes(parameters, options?.maxFields);
  const maxDynamicData = options?.maxDynamicData;
  if (maxDynamicData !== undefined) count(maxDynamicData, 'maxDynamicData');
  return new TypeDef({ k: 'args', uid: ++argumentTuples, fields, named, maxDynamicData, maxFields: options?.maxFields });
}

// ── Interfaces ──────────────────────────────────────────────────────────────

/** Options of `method`. */
export interface MethodOptions<R extends Parameter | undefined = Parameter | undefined> extends ArgumentTupleOptions {
  /**
   * The result: a type, or `field(type, { max, utf8, element, ... })` for a
   * result with bounds (C++ `returns(...)`). The result is then the bounded
   * type: `field(list(rawString), { max: 4, element: { utf8: true, max: 16 } })`
   * is a bounded list of bounded strings, with that type's bytes (a 1-byte
   * offset table) and checks. Absent: the method returns nothing.
   */
  returns?: R;
}

interface MethodShape {
  name: string;
  fields: FieldShape[];
  named: boolean;
  result?: FieldShape;
  maxDynamicData?: number | bigint;
  maxFields?: number;
  args: AnyDef;
}

/**
 * A method declaration (`method`), to be named by `defineInterface`. `A` /
 * `AV`: the value and the view of its arguments; `R` / `RV`: of its result
 * (`undefined`: it returns nothing).
 */
export class MethodDef<A = any, AV = any, R = any, RV = any> {
  /** Type-only: the result's value and view types. */
  declare readonly returns?: [R, RV];
  /** @internal */
  constructor(
    /** The argument list of the method: an `argumentTuple`. */
    readonly args: TypeDef<A, AV>,
    /** @internal */ readonly result: FieldShape | undefined,
    /** @internal */ readonly options: ArgumentTupleOptions,
  ) {}
}

/**
 * A method of an interface: its parameters — named (an object) or
 * positional (an array), each a type or `field(type, bounds)` — and its
 * options: `returns`, the cap on the encoded arguments (`maxDynamicData`)
 * and on the number of parameters (`maxFields`).
 *
 *     method({ to: field(rawString, { utf8: true, max: 16 }), amount: u64 }, { returns: Receipt, maxDynamicData: 256 })
 *     method([u32, u32], { returns: u64 })        // no parameter names
 */
export function method<P extends MethodParameters, R extends Parameter | undefined = undefined>(
  parameters: P, options: MethodOptions<R> = {},
): MethodDef<ArgumentsOf<P>, RecordView<ArgumentsOf<P>, ArgumentViewsOf<P>>, ResultValue<R>, ResultView<R>> {
  const { returns, maxDynamicData, maxFields } = options ?? {};
  let result: FieldShape | undefined;
  if (returns !== undefined) {
    result = { ...fieldShapes({ result: returns as MemberInput }, 'method')[0], name: '' };
    if (result.jsonInt || result.ijsonInt) schemaError('typed schema: a method result: jsonInt / ijsonInt belong to record members');
  }
  return new MethodDef(argumentTuple(parameters, { maxDynamicData, maxFields }) as never, result, { maxDynamicData, maxFields });
}

type ResultValue<R> = [R] extends [undefined] ? undefined : MemberValue<R>;
type ResultView<R> = [R] extends [undefined] ? undefined : MemberView<R>;
/** The typed method a declaration gives (`InterfaceDef.methods`). */
export type MethodOf<D> = D extends MethodDef<infer A, infer AV, infer R, infer RV> ? Method<A, AV, R, RV> : never;

/**
 * One method of a declared interface: its argument list and its result as
 * codecs. `A`: the JavaScript value of the arguments; `R`: of the result
 * (`undefined`: none).
 */
export class Method<A, AV, R, RV> {
  /** @internal */
  constructor(
    private readonly owner: InterfaceDef<any>,
    readonly name: string,
    /** The parameter names, in order (`""` for a parameter without one). */
    readonly params: readonly string[],
    /** Every parameter has a name: the arguments are an object. */
    readonly named: boolean,
    /** The cap on the encoded arguments, or null. */
    readonly maxDynamicData: bigint | null,
    /** The cap on the number of parameters, or null. */
    readonly maxFields: number | null,
    /**
     * The argument list (an `argumentTuple`): `encode`, `decode`,
     * `validate`, `view`, `valueToJson`, `jsonToValue`, ...
     */
    readonly args: TypeDef<A, AV>,
  ) {}

  /**
   * The codec of the result, as the type `returns` makes of it (C++
   * `psio::encode_method_result` / `validate_method_result` /
   * `decode_method_result`); `null` for a method that returns nothing.
   */
  get result(): [R] extends [undefined] ? null : PsszType<R, RV> {
    return this.owner.lookup.method(this.name).result as never;
  }
}

/** Options of `defineInterface`. */
export interface InterfaceOptions {
  /** The package of the schema (`Schema.package.name`). */
  package?: string;
  /** Its version. */
  version?: string;
  /**
   * Every argument list is also a named type `<interface>.<method>` of the
   * schema, so that a reader of the schema alone has a root for it (C++:
   * `builder.insert<method_arg_tuple<I>>("<interface>.<method>")` after
   * `insert_interface<T>()`). Default `true`.
   */
  argumentLists?: boolean;
}

/** An interface declared in TypeScript (`defineInterface`). */
export class InterfaceDef<M extends { [name: string]: MethodDef } = { [name: string]: MethodDef }> {
  /** @internal */
  readonly shape: { name: string; methods: MethodShape[]; options: InterfaceOptions };
  /** The methods, by name. */
  readonly methods: { readonly [K in keyof M]: MethodOf<M[K]> };
  private own?: PsszSchema;
  private found?: PsszInterface;
  private results?: ReadonlyMap<string, OrderTree>;

  /** @internal */
  constructor(readonly name: string, declarations: M, options: InterfaceOptions) {
    const methods = memberNames(declarations, 'method name').map((key): MethodShape => {
      const d = declarations[key];
      if (!(d instanceof MethodDef)) schemaError(`typed schema: method "${key}" must be declared with method(...)`);
      const args = resolve(d.args).shape as Extract<Shape, { k: 'args' }>;
      return { name: key, fields: args.fields, named: args.named, result: d.result, ...d.options, args: d.args };
    });
    this.shape = { name, methods, options };
    const typed: { [name: string]: Method<any, any, any, any> } = {};
    for (const m of methods)
      setOwn(typed, m.name, new Method(
        this, m.name, m.fields.map((f) => (m.named ? f.name : '')), m.named,
        m.maxDynamicData === undefined ? null : BigInt(m.maxDynamicData), m.maxFields ?? null, m.args,
      ));
    this.methods = typed as never;
  }

  /**
   * The Schema IR of the interface: what C++ `psio::SchemaBuilder` builds
   * from `insert_interface<T>()` — `interfaces[0]` with a `Func` per method
   * (parameter names and bounded types, the result as `returns` types it,
   * `result.lengthBound`, `maxFields`, `maxDynamicData`) — followed by the
   * argument lists as named types `<interface>.<method>` (unless
   * `argumentLists: false`).
   */
  get schema(): PsszSchema {
    return (this.own ??= declared(() => {
      const builder = new Builder();
      builder.insertInterface(this);
      this.results = builder.resultOrders.get(this.name);
      return builder.schema();
    }));
  }

  /** @internal The schema-driven lookup of this interface, with the typed-only result constraints. */
  get lookup(): PsszInterface {
    return (this.found ??= this.schema.interface(this.name, this.results));
  }

  /** The method `name`. */
  method<K extends keyof M & string>(name: K): MethodOf<M[K]> {
    if (!Object.prototype.hasOwnProperty.call(this.methods, name)) schemaError(`typed schema: interface "${this.name}" has no method "${name}"`);
    return this.methods[name];
  }
}

function setOwn(target: { [name: string]: unknown }, name: string, value: unknown): void {
  Object.defineProperty(target, name, { value, writable: false, enumerable: true, configurable: false });
}

/**
 * An interface: named methods, in declaration order. It produces the
 * `Interface` / `Func` Schema IR that C++ `PSIO_INTERFACE` +
 * `SchemaBuilder::insert_interface<T>()` produce for the equivalent
 * declaration (`conformance/interface-vectors.json`), and gives each method
 * an argument codec and a result codec.
 *
 *     const Token = defineInterface('token', {
 *       transfer: method(
 *         { to: field(rawString, { utf8: true, max: 16 }), amount: u64, memo: field(optional(rawString), { utf8: true, max: 80 }) },
 *         { returns: Receipt, maxDynamicData: 256, maxFields: 8 }),
 *       add: method([u32, u32], { returns: u64 }),
 *       ping: method({}),
 *     });
 *     Token.methods.transfer.args.encode({ to: 'bob', amount: 5n, memo: null });
 *     Token.methods.add.result.decode(bytes);      // bigint
 *     Token.schema.encode();                       // the Schema IR, as bytes
 */
export function defineInterface<M extends { [name: string]: MethodDef }>(
  name: string, methods: M, options: InterfaceOptions = {},
): InterfaceDef<M> {
  if (typeof name !== 'string' || name === '') schemaError('typed schema: an interface needs a name');
  if (typeof methods !== 'object' || methods === null) schemaError('typed schema: methods must be an object');
  for (const key of ['package', 'version'] as const)
    if (options?.[key] !== undefined && typeof options[key] !== 'string') schemaError(`typed schema: ${key} must be a string`);
  return new InterfaceDef(name, methods, { ...options });
}

/**
 * A tagged union: `{ type: 'Name', value }` in JavaScript. An alternative is
 * a definition, or `alt(definition, { ijsonName })`. For a C++
 * `std::variant` without labels the names are the indices: `{ 0: u32, 1: utf8 }`.
 */
export function variant<C extends { [name: string]: TypeDef<any, any> | Alt<any, any> }>(
  alternatives: C,
): TypeDef<
  { [K in keyof C]: { type: Name<K>; value: MemberValue<C[K]> } }[keyof C],
  { [K in keyof C]: VariantView<{ type: Name<K>; value: MemberValue<C[K]> }, Name<K>, MemberView<C[K]>> }[keyof C]
> {
  if (typeof alternatives !== 'object' || alternatives === null)
    schemaError('typed schema: variant alternatives must be an object');
  const alts = memberNames(alternatives, 'alternative name').map((name): AltShape => {
    const a = alternatives[name] as MemberInput;
    if (a instanceof Alt) {
      const ijsonName = a.options.ijsonName;
      if (ijsonName !== undefined && typeof ijsonName !== 'string') schemaError('typed schema: ijsonName must be a string');
      return { name, type: a.type, ijsonName };
    }
    return { name, type: isDef(a, `alternative "${name}"`) };
  });
  if (alts.length === 0 || alts.length > 256) schemaError('typed schema: a variant has 1 to 256 alternatives');
  return new TypeDef({ k: 'variant', alts });
}

// ── Enums, time, custom types ───────────────────────────────────────────────

/**
 * An enum over an integer type: on the wire and in JavaScript the integer
 * (any in-range value is valid); in text the enumerator's name.
 */
export function enumeration<T extends number | bigint>(
  underlying: TypeDef<T, T>, members: { [name: string]: number | bigint },
): TypeDef<T, T> {
  const under = resolve(isDef(underlying, 'an enum underlying type'));
  if (under.shape.k !== 'int' || under.shape.bits > 64 || under.shape.bits < 8)
    schemaError('typed schema: an enum needs an underlying integer of 8 to 64 bits');
  const { bits, signed } = under.shape;
  const entries = memberNames(members, 'enumerator name').map((name): [string, bigint] => {
    const raw = members[name];
    const value = typeof raw === 'bigint' ? raw : Number.isSafeInteger(raw) ? BigInt(raw) : schemaError('typed schema: an enumerator value is an integer');
    const low = signed ? -(1n << BigInt(bits - 1)) : 0n;
    const high = signed ? (1n << BigInt(bits - 1)) - 1n : (1n << BigInt(bits)) - 1n;
    if (value < low || value > high) schemaError(`typed schema: enumerator "${name}" does not fit the underlying integer`);
    return [name, value];
  });
  return new TypeDef({ k: 'enum', under, members: entries });
}

function timeDef(kind: 'point' | 'duration' | 'monotonic', unit: TimeUnit, tick: AnyDef): TypeDef<bigint> {
  const under = resolve(isDef(tick, 'a tick type'));
  if (under.shape.k !== 'int' || under.shape.bits > 64 || under.shape.bits < 8)
    schemaError('typed schema: a time value needs a tick integer of 8 to 64 bits');
  let id: string;
  try {
    const period = timeUnitPeriod(unit);
    id = timeSchemaId(kind === 'monotonic' ? { point: false, monotonic: true, ...period } : { point: kind === 'point', ...period });
  } catch (e) {
    return schemaError(e instanceof Error ? e.message : String(e));
  }
  return new TypeDef({ k: 'time', id, tick: under });
}

/**
 * A point in time: ticks of `unit` since 1970-01-01T00:00:00Z, a `bigint`
 * at every tick width; RFC 3339 in text (`docs/time.md`).
 */
export const timePoint = (unit: TimeUnit, tick: TypeDef<any, any> = i64) => timeDef('point', unit, tick);
/** A span of time in ticks of `unit`: a `bigint`; its tick count in text. */
export const duration = (unit: TimeUnit, tick: TypeDef<any, any> = i64) => timeDef('duration', unit, tick);
/**
 * A point in time of a clock other than the system clock (a steady /
 * boot-relative clock, a simulation clock): ticks of `unit` since that
 * clock's own epoch, a `bigint`. It has no date: its text is the tick
 * count, exactly like a duration's, for every unit, and the RFC 3339 /
 * `Date` helpers do not apply (`docs/time.md`).
 */
export const monotonic = (unit: TimeUnit, tick: TypeDef<any, any> = i64) => timeDef('monotonic', unit, tick);

/**
 * What a custom type adds to its wire type, typed by the value of the wire
 * type. Every member is optional.
 */
export interface CustomHandler<T> {
  /** The text form: one JSON string in both text formats. */
  toText?(value: T): string;
  fromText?(text: string): T;
  /**
   * The invariant: returns a message (or throws an `Error` with one) when a
   * structurally valid value of the wire type is not a value of the type.
   * It runs on every read and write, binary and text; the message is the
   * error's.
   */
  check?(value: T): string | null | undefined | void;
}

/**
 * A custom type: a type with an identity of its own over its wire type, of
 * any shape (a record, a transparent type, an enum, a variant, a tuple, a
 * list). On the wire and as a JavaScript value it is exactly `wire`; the
 * handler gives it a text form (one JSON string, `toText` / `fromText`), an
 * invariant (`check`), or both. Without a handler it is its wire type.
 */
export function custom<T, V>(id: string, wire: TypeDef<T, V>, handler?: CustomHandler<T>): TypeDef<T, V> {
  if (typeof id !== 'string' || id === '') schemaError('typed schema: a custom type needs an id');
  if (reservedCustomId(id)) schemaError(`typed schema: "${id}" is a built-in id, not a custom type`);
  if (handler !== undefined) {
    if (typeof handler !== 'object' || handler === null)
      schemaError(`typed schema: the handler of custom id "${id}" must be an object`);
    for (const member of ['toText', 'fromText', 'check'] as const)
      if (handler[member] !== undefined && typeof handler[member] !== 'function')
        schemaError(`typed schema: ${member} of the handler of custom id "${id}" must be a function`);
  }
  return new TypeDef({ k: 'custom', id, inner: isDef(wire, 'a wire type'), handler: handler as PsszCustomHandler | undefined });
}
