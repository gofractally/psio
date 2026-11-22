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
  IR_VERSION, PsszError, reservedCustomId, type BitsValue, type IrAnyType, type IrAttribute, type IrMember, type IrSchema,
  type PsszCustomHandler, type Some,
} from './pssz-core.js';
import type { JsonFormat } from './pssz-json.js';
import {
  PsszSchema, type PsszDecodeOptions, type PsszKeyValidation, type PsszType, type PsszValidation, type PsszWidth,
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

interface FieldShape {
  name: string;
  type: AnyDef;
  lengthBound?: LengthBoundOption;
  jsonInt?: IntText;
  ijsonInt?: IntText;
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
  | { k: 'tuple'; items: AnyDef[] }
  | { k: 'variant'; alts: AltShape[] }
  | { k: 'record'; name: string; fields: FieldShape[]; options: RecordOptions }
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

/** Options of a record member (`field`). */
export interface FieldOptions {
  /**
   * Bounds the member's length (bytes of a string, elements of a list),
   * through `optional`. `{ max }` alone on a raw string, bytes or list makes
   * the member a bounded type, which narrows the record's offset width.
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
  /** A cap on the number of fields the record may ever grow to. */
  maxFields?: number;
}

/** Options of `def.type(...)` and `generateSchema`. */
export interface TypeDefOptions {
  /** Offset-width profile; default `auto`. */
  width?: PsszWidth;
  /** A custom type declared without a handler is an error in text. */
  requireHandlers?: boolean;
}

// ── Schema IR generation (cpp/include/psio/schema_builder.hpp) ──────────────

const ir = (type: string, value: unknown): IrAnyType => ({ value: { type, value } } as IrAnyType);
const intIr = (bits: number, isSigned: boolean) => ir('Int', { bits, isSigned });
const attribute = (name: string, value: string): IrAttribute => ({ name, value });

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
    case 'tuple': out = `T(${s.items.map(k).join(',')})`; break;
    case 'variant':
      out = `V(${s.alts.map((a) => `${JSON.stringify([a.name, a.ijsonName ?? null])}${k(a.type)}`).join(',')})`;
      break;
    case 'record': out = `R(${JSON.stringify(s.name)})`; break;
    case 'enum': out = `E(${k(s.under)},${JSON.stringify(s.members.map(([n, v]) => [n, String(v)]))})`; break;
    case 'time': out = `M(${s.id},${k(s.tick)})`; break;
    case 'custom': out = `C(${JSON.stringify(s.id)},${k(s.inner)})`; break;
    case 'box': out = `X(${k(s.inner)})`; break;
    default: return schemaError('typed schema: unresolved definition');
  }
  def.key = out;
  return out;
}

class Builder {
  readonly types: IrMember[] = [];
  readonly handlers = new Map<string, PsszCustomHandler>();
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
      if (def.shape.k === 'record' && this.records.get(known) !== def)
        schemaError(`typed schema: two different records are named "${known}"`);
      return ir('Type', { type: known });
    }
    // A record is its name; so is a custom type declared over a record.
    let named: AnyDef = def;
    while (named.shape.k === 'custom') named = resolve(named.shape.inner);
    const name = named.shape.k === 'record' ? named.shape.name : `@${this.names.size}`;
    if (this.slots.has(name)) schemaError(`typed schema: the name "${name}" is used twice`);
    this.names.set(key, name);
    if (def.shape.k === 'record') this.records.set(name, def);
    // The slot is taken before the children are visited (declaration order).
    this.put(name, ir('Type', { type: name }));
    if (++this.depth > 256) schemaError('typed schema: type nesting too deep');
    this.put(name, this.build(def));
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

  // The type itself, not interned; its children are.
  private build(def: AnyDef): IrAnyType {
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
      case 'variant':
        return ir('Variant', {
          members: s.alts.map((alt): IrMember => ({
            name: alt.name,
            type: this.insert(alt.type),
            attributes: alt.ijsonName === undefined ? [] : [attribute('ijsonName', alt.ijsonName)],
          })),
          attributes: [],
        });
      case 'record': {
        const members = s.fields.map((field) => this.member(field));
        const attributes: IrAttribute[] = [];
        if (s.options.fixed) attributes.push(attribute('definitionWillNotChange', ''));
        if (s.options.maxFields !== undefined) attributes.push(attribute('maxFields', String(s.options.maxFields)));
        if (s.options.maxDynamicData !== undefined)
          attributes.push(attribute('maxDynamicData', String(s.options.maxDynamicData)));
        return ir('Object', { members, attributes });
      }
      case 'enum':
        return ir('Enum', {
          type: this.build(resolve(s.under)),
          members: s.members.map(([name, value]) => ({ name, value: BigInt.asUintN(64, value) })),
        });
      case 'time': return ir('Custom', { type: this.build(resolve(s.tick)), id: s.id });
      case 'custom':
        this.bind(s.id, s.handler);
        return ir('Custom', { type: this.build(resolve(s.inner)), id: s.id });
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

  // `bounded_member_type`: the member type a `max` length bound turns into.
  private boundedMember(def: AnyDef, max: bigint): IrAnyType | null {
    const s = resolve(def).shape;
    if (s.k === 'option') {
      const inner = this.boundedMember(s.inner, max);
      return inner && ir('Option', { type: inner });
    }
    if (s.k === 'bytes' && s.bound === 'list' && s.id !== 'utf8')
      return ir('Custom', { type: this.byteContainer('max', max), id: s.id });
    if (s.k === 'list') return ir('BoundedList', { type: this.insert(s.inner), maxCount: max });
    return null;
  }

  // `build_record_member`.
  private member(field: FieldShape): IrMember {
    const member: IrMember = { name: field.name, type: this.insert(field.type), attributes: [] };
    const direct = resolve(field.type).shape;
    // A bounded UTF-8 string states its own bound.
    const bound = field.lengthBound
      ?? (direct.k === 'bytes' && direct.id === 'utf8' && direct.bound === 'max' ? { max: Number(direct.n) } : undefined);
    if (bound) {
      let target = direct;
      while (target.k === 'option') target = resolve(target.inner).shape;
      const applies = target.k === 'list' || (target.k === 'bytes' && target.bound !== 'exact' &&
        (target.bound === 'list' || target.id === 'utf8'));
      if (!applies) schemaError(`typed schema: lengthBound does not apply to member "${field.name}"`);
      const onlyMax = bound.max !== undefined && bound.exact === undefined && bound.min === undefined;
      const bounded = onlyMax ? this.boundedMember(field.type, BigInt(bound.max!)) : null;
      if (bounded) member.type = bounded;
      else {
        const items: string[] = [];
        for (const key of ['exact', 'min', 'max'] as const)
          if (bound[key] !== undefined) items.push(`${key}=${bound[key]}`);
        if (items.length) member.attributes.push(attribute('lengthBound', items.join(',')));
      }
    }
    if (field.jsonInt) member.attributes.push(attribute('jsonInt', field.jsonInt));
    if (field.ijsonInt) member.attributes.push(attribute('ijsonInt', field.ijsonInt));
    return member;
  }

  schema(requireHandlers?: boolean): PsszSchema {
    const schema: IrSchema = {
      package: { name: '', version: '', attributes: [] },
      interfaces: [], worlds: [], uses: [],
      types: this.types,
      ir_version: IR_VERSION,
    };
    return new PsszSchema(schema, { handlers: this.handlers, requireHandlers });
  }
}

/**
 * The Schema IR of named definitions: what C++ `psio::SchemaBuilder` builds
 * from `insert<T>(name)` of the equivalent types in the same order (records
 * under their own names, every other type interned as `@N`). The result
 * carries the handlers of the custom types it reaches; `schema.ir` is the
 * IR, `schema.encode()` its PSSZ bytes, `schema.type(name)` a codec.
 */
export function generateSchema(types: { [name: string]: TypeDef<any, any> }, options?: { requireHandlers?: boolean }): PsszSchema {
  return declared(() => {
    const builder = new Builder();
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
    if (!options || (options.width === undefined && options.requireHandlers === undefined))
      return (this.auto ??= this.schema.type<T, V>(this.name));
    return this.schema.type<T, V>(this.name, options);
  }

  /** The offset width of this type's own header / slots / offset table. */
  get width(): number {
    return this.type().width;
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

type ValuesOf<D extends readonly AnyDef[]> = { -readonly [I in keyof D]: Infer<D[I]> };
type ViewsOf<D extends readonly AnyDef[]> = { -readonly [I in keyof D]: InferView<D[I]> };

/** Positional elements of different types. Trailing optionals may be absent on the wire. */
export function tuple<D extends readonly AnyDef[]>(...elements: D): TypeDef<ValuesOf<D>, TupleView<ValuesOf<D>, ViewsOf<D>>> {
  return new TypeDef({ k: 'tuple', items: elements.map((e) => isDef(e, 'a tuple element')) });
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
/** A record member with a length bound or an integer text form. */
export function field<T, V>(type: TypeDef<T, V>, options: FieldOptions): Field<T, V> {
  return new Field(isDef(type, 'a field type'), options ?? {});
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
  const shapes = memberNames(fields, 'field name').map((key): FieldShape => {
    const f = fields[key] as MemberInput;
    if (f instanceof Field) {
      const o = f.options;
      const b = o.lengthBound;
      if (b !== undefined)
        for (const k of ['max', 'min', 'exact'] as const)
          if (b[k] !== undefined && !(Number.isInteger(b[k]) && b[k]! >= 0 && b[k]! <= 0xffffffff))
            schemaError(`typed schema: lengthBound.${k} of "${key}" must be a 32-bit unsigned integer`);
      return {
        name: key, type: f.type, lengthBound: b,
        jsonInt: intText(o.jsonInt, 'jsonInt'), ijsonInt: intText(o.ijsonInt, 'ijsonInt'),
      };
    }
    return { name: key, type: isDef(f, `field "${key}"`) };
  });
  const { fixed, maxDynamicData, maxFields } = options;
  if (maxDynamicData !== undefined) count(maxDynamicData, 'maxDynamicData');
  if (maxFields !== undefined) count(maxFields, 'maxFields');
  return new TypeDef({ k: 'record', name, fields: shapes, options: { fixed: !!fixed, maxDynamicData, maxFields } });
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

/** The text form of a custom type, typed by the value of its wire type. */
export interface CustomHandler<T> {
  toText(value: T): string;
  fromText(text: string): T;
}

/**
 * A type with its own text form: on the wire and as a JavaScript value
 * exactly `wire`; in JSON and I-JSON one string, written and read by
 * `handler`. Without a handler its text is the wire type's.
 */
export function custom<T, V>(id: string, wire: TypeDef<T, V>, handler?: CustomHandler<T>): TypeDef<T, V> {
  if (typeof id !== 'string' || id === '') schemaError('typed schema: a custom type needs an id');
  if (reservedCustomId(id)) schemaError(`typed schema: "${id}" is a built-in id, not a custom type`);
  if (handler !== undefined && (typeof handler?.toText !== 'function' || typeof handler?.fromText !== 'function'))
    schemaError(`typed schema: the handler of custom id "${id}" needs toText and fromText`);
  return new TypeDef({ k: 'custom', id, inner: isDef(wire, 'a wire type'), handler: handler as PsszCustomHandler | undefined });
}
