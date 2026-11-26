// pssz.ts — public API of the schema-driven PSSZ / JSON / I-JSON codec.
//
// A consumer holds a Schema IR (`psio::Schema`, usually fetched as its PSSZ
// bytes) and no generated types:
//
//   const schema = PsszSchema.decode(schemaBytes);
//   const Transfer = schema.type('Transfer');
//   const bytes = Transfer.jsonToPssz('{"from":"alice","amount":"18446744073709551615"}');
//   const value = Transfer.decode(bytes);        // { from: 'alice', amount: 18446744073709551615n }
//   const text  = Transfer.psszToJson(bytes);    // I-JSON by default
//
// Values: bool -> boolean; integers up to 32 bits -> number; 64 / 128 / 256
// bits -> bigint; floats -> number; UTF-8 string ("utf8") -> string; raw
// string ("string": any bytes) and bytes (hex adapter) -> Uint8Array; enum -> the number / bigint of its underlying integer (the
// enumerator name appears only in text); record -> object; list / array /
// tuple / pair -> array; map -> Map and set -> Set, in canonical (key) order
// (any iterable of entries / elements is encoded); nested value -> the inner
// value (its bytes, a Uint8Array, when nested bytes are opaque);
// any JSON value (Custom "json" over a UTF-8 string) -> the plain JSON value
// (PsszJsonText: its exact text);
// variant -> { type, value }; optional -> null or the value
// (an optional of an optional holds null or { some: inner }); bit types -> { bytes, bitCount }. A NaN
// with a payload decodes to a PsszNaN so that it re-encodes bit for bit.
// A time value (duration, time point or monotonic time point, docs/time.md)
// -> bigint ticks at every width; see time.ts for the Date / RFC 3339
// conversions of a time point.
//
// The same values have a `key` encoding (docs/key-spec.md): bytes whose
// bytewise order is the value's order, for database keys.
//
//   const Key = schema.type({ Tuple: [U32, STRING] });
//   const from = Key.encodeKeyPrefix([7]);          // every key of shard 7 ...
//   const to   = keyPrefixUpperBound(from);         // ... lies in [from, to)

import {
  Codec, IR_VERSION, MemberInfo, Node, PsszError, boundCount, checkAttributes, checkIrVersion, findAttribute,
  fastWrite, memberBoundAdmits, nestedMember, normalizeMember, normalizeSchema, normalizeType, parseCap, parseLengthBound,
  parseMaxFields, reservedCustomId, settleMember,
  type DecodeContext, type IrAnyType, type IrAttribute, type IrFunc, type IrFunctionType, type IrInterface, type IrSchema,
  type OrderTable, type OrderTree, type PsszCustomHandler, type PsszErrorKind, type PsszSchemaInput,
  type PsszTypeInput,
} from './pssz-core.js';
import { jsonToValue, valueToJson, type JsonFormat } from './pssz-json.js';
import { reservedTimeId } from './time.js';
import { decodeKey, encodeKey, encodeKeyPrefix, keyPrefixUpperBound as upperBound } from './pssz-key.js';
import { viewAt } from './pssz-view.js';

export {
  PsszError, PsszNaN, PsszNegativeZero, PsszJsonText, IR_VERSION,
  type PsszErrorKind, type IrSchema, type IrAnyType, type IrMember, type IrAttribute, type IrTypeValue,
  type IrEnum, type IrEnumerator, type IrFunc, type IrFunctionType, type IrResource, type IrInterface, type IrWorld, type IrUse,
  type CompactEnumerator, type PsszCustomHandler,
  type CompactType, type PsszSchemaInput, type PsszTypeInput, type Some, type VariantValue, type BitsValue,
} from './pssz-core.js';
export { type JsonFormat } from './pssz-json.js';
export { PsszView, RecordView, TupleView, ListView, VariantView } from './pssz-view.js';

/** `auto`: type-derived offset widths (`psio::pssz`). 1 / 2 / 4: one inherited width (`psio::pssz_<W>`). */
export type PsszWidth = 'auto' | 1 | 2 | 4;

/** Custom id -> its handler: the text form and / or the invariant of the type. */
export type PsszCustomHandlers = { [id: string]: PsszCustomHandler } | ReadonlyMap<string, PsszCustomHandler>;

/**
 * What a codec does with the bytes of a nested value (`Nested{type}`).
 * `typed`: they are validated as the inner type by every reader, and the
 * value is the inner value. `opaque`: they are not looked at, and the value
 * is the bytes (a `Uint8Array`) — the behaviour of a C++
 * `nested<T, nested_validation::deferred>` member, and of a reader that must
 * keep foreign bytes exactly.
 */
export type PsszNestedBytes = 'typed' | 'opaque';

export interface PsszSchemaOptions {
  /**
   * Handlers of custom types (`Custom` ids that are not part of the binary
   * form), by id: the type's text form (`toText` / `fromText`) and / or its
   * invariant (`check`). A handler is bound when a type is compiled
   * (`schema.type`), not looked up per value. `check` runs on every entry
   * point, binary and text.
   */
  handlers?: PsszCustomHandlers;
  /**
   * A strict registry: a custom id with no handler is an error on every
   * entry point, binary and text (default: such a type is its wire type —
   * the wire type's text, its invariant not checked).
   */
  requireHandlers?: boolean;
  /** Nested bytes; default `typed`. */
  nestedBytes?: PsszNestedBytes;
  /** @internal Named types whose Nested node is opaque (typed deferred members). */
  deferredNested?: Iterable<string>;
  /** @internal Typed-only order constraints (sorted / unique / uniqueKeys), by named type and member. */
  orders?: OrderTable;
}
export interface PsszTypeOptions extends PsszSchemaOptions {
  /** Offset-width profile; default `auto`. */
  width?: PsszWidth;
}
export interface PsszDecodeOptions {
  /**
   * Reject a record that carries appended optional fields this schema does
   * not know (default: accept and drop them, as `validate` reports).
   */
  rejectUnknown?: boolean;
}
export type PsszKeyValidation = { ok: true } | { ok: false; error: PsszError };
/** The outcome of `PsszSchema.validate()`. */
export type PsszSchemaValidation = { ok: true } | { ok: false; error: PsszError };
/**
 * @internal What a root type carries that a record member would: the bound
 * and order constraints `returns(...)` gives a method result.
 */
export interface PsszRootOptions {
  /** The value of a `lengthBound` attribute. */
  lengthBound?: string | null;
  order?: OrderTree | null;
}
export type PsszValidation =
  | { ok: true; /** Unknown appended optional fields were present. */ hasUnknown: boolean }
  | { ok: false; error: PsszError };

// Every public entry point throws PsszError and nothing else.
function guarded<T>(run: () => T): T {
  try {
    return run();
  } catch (e) {
    if (e instanceof PsszError) throw e;
    throw new PsszError('internal', e instanceof Error ? `${e.name}: ${e.message}` : String(e));
  }
}

function asBytes(bytes: unknown): Uint8Array {
  if (!(bytes instanceof Uint8Array)) throw new PsszError('bytes', 'pssz: input must be a Uint8Array');
  // A subclass (a Node Buffer) slices into views; decoded bytes must be copies.
  return bytes.constructor === Uint8Array ? bytes : new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.length);
}

/**
 * One type of a schema under one width profile. From a schema fetched at
 * run time the value type `T` and the view type `V` are `unknown`; the typed
 * definitions of `types.ts` state them.
 */
export class PsszType<T = unknown, V = unknown> {
  // The type on the wire: "box" and custom text wrappers looked through.
  private readonly node: Node;
  private readonly boxed: boolean;

  // The count a root length bound gives the list it holds (its offset width).
  private readonly count: bigint | null;

  /**
   * @internal `ref` is the type as written (the text walk starts there).
   * `root`: the bound and order constraints of a root that has them (a
   * method result), held as a member's are.
   */
  constructor(private readonly codec: Codec, private readonly ref: Node, private readonly root: MemberInfo | null = null) {
    ({ node: this.node, boxed: this.boxed } = codec.wireOf(ref));
    this.count = boundCount(root?.bound ?? null);
  }

  /** @internal The codec and the type as written (`psszEvolvesFrom`). */
  get internals(): { codec: Codec; ref: Node; root: MemberInfo | null } {
    return { codec: this.codec, ref: this.ref, root: this.root };
  }

  // A root with constraints: the value against them, as a member's is.
  private admit(value: unknown, kind: PsszErrorKind): unknown {
    const root = this.root;
    if (!root) return value;
    if (root.bound) {
      value = settleMember(root.node, value);
      if (!memberBoundAdmits(root.bound, root.node, value)) throw new PsszError(kind, 'pssz IR: member length bound violated');
    }
    if (root.order)
      this.codec.checkOrder(root, value, (message) => {
        throw new PsszError(kind, message);
      });
    return value;
  }
  // ... and a nested root's bound, which counts its encoded bytes.
  private admitBytes(bytes: Uint8Array, kind: PsszErrorKind): void {
    const root = this.root;
    if (root?.bound && nestedMember(root.node) && !this.codec.nestedBoundAdmits(root.bound, root.node, bytes, 0, bytes.length))
      throw new PsszError(kind, 'pssz IR: member length bound violated');
  }

  /**
   * The offset width of this type's own header / slots / offset table. A
   * boxed value has no inferred bound: 4 under the automatic profile.
   */
  get width(): number {
    return guarded(() => (this.boxed && this.codec.W === 0 ? 4 : this.codec.widthOf(this.node, null, this.count)));
  }

  /**
   * The inferred bound of the type (`inferred_bound`, `docs/pssz-spec.md`
   * §3.2): the number offset widths are chosen from, each header, slot and
   * offset table counted at the width the bound itself selects; `null`
   * when unbounded (a plain string or list, a boxed value, anything holding
   * one). The type's own `maxDynamicData` is not looked at — it rules the
   * width whatever this is — while a capped record inside contributes its
   * cap. It is not the largest encoding of a capped type: that is
   * `maxEncodedSize`.
   */
  get inferredBound(): bigint | null {
    return guarded(() => (this.boxed ? null : this.codec.inferredBound(this.node, this.count)));
  }

  /**
   * The maximum encoded size of the type (`max_encoded_size`,
   * `docs/pssz-spec.md` §3.7): no automatic-profile encoding of a value of
   * it is longer; `null` when there is no finite maximum. Every header,
   * slot and offset table is counted at the width that is written (the
   * class of the cap for a capped record, of min(bound, cap) for a list or
   * tuple beneath one), what the type holds at its own maximum, and a
   * capped record is the smaller of its cap and that sum — the cap alone
   * when something inside is unbounded. Conservative by one selector byte
   * per optional and by ceil(N / 8) + 1 for a bitlist. No width is chosen
   * from it.
   */
  get maxEncodedSize(): bigint | null {
    return guarded(() => (this.boxed ? null : this.codec.maxEncodedSize(this.node, this.count)));
  }

  /**
   * Whether this record has a `maxDynamicData` cap and holds — with no
   * other record in between — a record whose offsets are wider than the
   * cap's width class (C++ `psio::pssz_child_wider_than_cap_v<T>`). The cap
   * narrows the lists, maps, sets and tuples beneath it, but a record keeps
   * its own width wherever it appears, so such a child spends offset bytes
   * on a capacity it cannot use there: give it a cap or member bounds of
   * its own. A query, not an error; `false` under an explicit width.
   */
  get childWiderThanCap(): boolean {
    return guarded(() => !this.boxed && this.codec.childWiderThanCap(this.node));
  }

  /**
   * The PSSZ bytes of `value`: the FAST write. The value is not held against
   * what its type declares (member bounds, order, handler checks); it
   * throws only for a value the format cannot represent (an offset that
   * does not fit its width, a `maxDynamicData` cap, a wrong JS type).
   * `encodeChecked` is the validated write (C++ `to_pssz` /
   * `to_pssz_checked`).
   */
  encode(value: T): Uint8Array {
    return fastWrite(() => this.encodeChecked(value));
  }

  /**
   * The validated write: the canonical PSSZ bytes of `value`, or a
   * PsszError for a value `validate` would refuse the bytes of.
   */
  encodeChecked(value: T): Uint8Array {
    return guarded(() => {
      const out = this.codec.encodeTop(this.ref, this.admit(value, 'value'), this.count);
      this.admitBytes(out, 'value');
      return out;
    });
  }

  /** Decodes fully validated PSSZ bytes. */
  decode(bytes: Uint8Array, options?: PsszDecodeOptions): T {
    return guarded(() => {
      const input = asBytes(bytes);
      const ctx: DecodeContext = { allowUnknown: !options?.rejectUnknown, hasUnknown: false };
      return this.read(input, ctx) as T;
    });
  }

  private read(input: Uint8Array, ctx: DecodeContext): unknown {
    const value = this.codec.decodeTop(this.ref, input, ctx, this.count);
    if (this.root) {
      this.admit(value, 'bytes');
      this.admitBytes(input, 'bytes');
    }
    return value;
  }

  /**
   * Reads the value in place: a scalar or text is returned as its value,
   * bytes as a subarray of `bytes`, a record / tuple / list / variant as a
   * view (`RecordView`, `TupleView`, `ListView`, `VariantView`) that reads
   * one field or element at a time. A view checks the frames and values it
   * reads, not the rest: use `validate` first when the whole encoding must
   * be known to be canonical.
   */
  view(bytes: Uint8Array): V {
    return guarded(() => {
      const input = asBytes(bytes);
      this.codec.checkTopCap(this.node, input.length, (m) => {
        throw new PsszError('bytes', m);
      });
      return viewAt(this.codec, this.ref, input, 0, input.length, 0, null, this.count) as V;
    });
  }

  /** Checks PSSZ bytes against the type. Never throws. */
  validate(bytes: Uint8Array): PsszValidation {
    try {
      const input = asBytes(bytes);
      const ctx: DecodeContext = { allowUnknown: true, hasUnknown: false };
      guarded(() => this.read(input, ctx));
      return { ok: true, hasUnknown: ctx.hasUnknown };
    } catch (e) {
      return { ok: false, error: e as PsszError };
    }
  }

  /**
   * The `key` bytes of `value`: bytewise order is value order. The entries
   * of a map / set are written in ascending key order; a value that
   * violates an invariant of a registered handler has no key.
   */
  encodeKey(value: T): Uint8Array {
    return fastWrite(() => this.encodeKeyChecked(value));
  }

  /**
   * The validated key write: the key of `value`, or a PsszError for a value
   * `validateKey` would refuse the key of (a bound, an order constraint, a
   * handler check). `encodeKey` is the fast write: every value has a key.
   */
  encodeKeyChecked(value: T): Uint8Array {
    return guarded(() => {
      const admitted = this.admit(value, 'value');
      this.codec.checkValue(this.ref, admitted, 'value');
      return encodeKey(this.codec, this.node, admitted);
    });
  }

  /**
   * Decodes key bytes. Checked: exactly one canonical key spanning the
   * whole input, or a PsszError.
   */
  decodeKey(bytes: Uint8Array): T {
    return guarded(() => this.admit(decodeKey(this.codec, this.ref, asBytes(bytes)), 'bytes')) as T;
  }

  /** Checks key bytes against the type. Never throws. */
  validateKey(bytes: Uint8Array): PsszKeyValidation {
    try {
      this.decodeKey(bytes);
      return { ok: true };
    } catch (e) {
      return { ok: false, error: e as PsszError };
    }
  }

  /**
   * The key of the first `elements.length` elements of a tuple type (fields
   * of a record type), each encoded as the type's element at its position:
   * the prefix every full key with those leading elements starts with.
   */
  encodeKeyPrefix(elements: unknown[]): Uint8Array {
    // A fast write of the elements, as `encodeKey` (C++ `key_prefix`).
    return fastWrite(() => guarded(() => encodeKeyPrefix(this.codec, this.ref, elements)));
  }

  /** JSON / I-JSON text -> value. */
  jsonToValue(text: string | Uint8Array, format: JsonFormat = 'ijson'): T {
    return guarded(() => this.admit(jsonToValue(this.codec, this.ref, format, text), 'json')) as T;
  }

  /**
   * Value -> JSON / I-JSON text: the fast write (the value is not held
   * against bounds, order or handler checks; text that would not be JSON
   * still throws). `valueToJsonChecked` is the validated write.
   */
  valueToJson(value: T, format: JsonFormat = 'ijson'): string {
    return fastWrite(() => this.valueToJsonChecked(value, format));
  }

  /** The validated JSON write: the text, or a PsszError for a value `jsonToValue` would refuse. */
  valueToJsonChecked(value: T, format: JsonFormat = 'ijson'): string {
    return guarded(() => valueToJson(this.codec, this.ref, format, this.admit(value, 'value')));
  }

  /** JSON / I-JSON text -> canonical PSSZ bytes. */
  jsonToPssz(text: string | Uint8Array, format: JsonFormat = 'ijson'): Uint8Array {
    // (a json value is carried as its text: every number as it was written)
    // Text from outside: the validated write.
    return this.encodeChecked(guarded(() => this.admit(jsonToValue(this.codec, this.ref, format, text, true), 'json')) as T);
  }

  /** PSSZ bytes (fully validated) -> JSON / I-JSON text. */
  psszToJson(bytes: Uint8Array, format: JsonFormat = 'ijson', options?: PsszDecodeOptions): string {
    // The decode runs the handlers' checks; the writer need not repeat them.
    // A json value is carried as its text: every number as it was written.
    return guarded(() => {
      const ctx: DecodeContext = { allowUnknown: !options?.rejectUnknown, hasUnknown: false, jsonText: true };
      return valueToJson(this.codec, this.ref, format, this.read(asBytes(bytes), ctx), false);
    });
  }
}

function handlerMap(input: PsszCustomHandlers | undefined): Map<string, PsszCustomHandler> {
  const out = new Map<string, PsszCustomHandler>();
  if (input === undefined) return out;
  const entries: Iterable<[string, PsszCustomHandler]> =
    input instanceof Map ? input : Object.entries(input as { [id: string]: PsszCustomHandler });
  for (const [id, handler] of entries) {
    if (reservedTimeId(id))
      throw new PsszError('schema', `pssz: custom id "${id}" is a built-in time presentation and takes no handler`);
    if (reservedCustomId(id))
      throw new PsszError('schema', id === 'json'
        ? 'pssz: custom id "json" is the built-in JSON value and takes no handler'
        : `pssz: custom id "${id}" is part of the binary form and takes no handler`);
    if (typeof handler !== 'object' || handler === null)
      throw new PsszError('schema', `pssz: the handler of custom id "${id}" must be an object`);
    for (const member of ['toText', 'fromText', 'check'] as const)
      if (handler[member] !== undefined && typeof handler[member] !== 'function')
        throw new PsszError('schema', `pssz: ${member} of the handler of custom id "${id}" must be a function`);
    out.set(id, handler);
  }
  return out;
}

let metaSchema: PsszSchema | undefined;

/** A Schema IR, ready to hand out codecs for its types. */
export class PsszSchema {
  /** The canonical IR (`psio::Schema` as this library decodes it). */
  readonly ir: IrSchema;
  private readonly codecs = new Map<number, Codec>();
  private readonly handlers: ReadonlyMap<string, PsszCustomHandler>;
  private readonly requireHandlers: boolean;
  private readonly nestedBytes: PsszNestedBytes;
  private readonly deferred: ReadonlySet<string>;
  private readonly orders: OrderTable;

  /**
   * `input` is a `psio::Schema` value (as `PsszSchema.decode` produces), or
   * a `{ name: type }` map in the compact spelling, e.g.
   * `{ Point: { Object: { x: { Int: { bits: 32, isSigned: true } }, y: 'Coord' } } }`.
   */
  constructor(input: PsszSchemaInput, options?: PsszSchemaOptions) {
    this.ir = guarded(() => normalizeSchema(input));
    this.handlers = guarded(() => handlerMap(options?.handlers));
    this.requireHandlers = options?.requireHandlers ?? false;
    const nested = options?.nestedBytes ?? 'typed';
    if (nested !== 'typed' && nested !== 'opaque')
      throw new PsszError('schema', 'pssz: nestedBytes must be "typed" or "opaque"');
    this.nestedBytes = nested;
    this.deferred = new Set(options?.deferredNested ?? []);
    this.orders = options?.orders ?? new Map();
  }

  // The same schema under other options (absent: this schema's).
  private derive(options: PsszSchemaOptions): PsszSchema {
    return guarded(() => {
      const merged = new Map(this.handlers);
      for (const [id, handler] of handlerMap(options.handlers)) merged.set(id, handler);
      return new PsszSchema(this.ir, {
        handlers: merged,
        requireHandlers: options.requireHandlers ?? this.requireHandlers,
        nestedBytes: options.nestedBytes ?? this.nestedBytes,
        deferredNested: this.deferred,
        orders: this.orders,
      });
    });
  }

  private codec(w: 0 | 1 | 2 | 4): Codec {
    let codec = this.codecs.get(w);
    if (!codec) {
      const options = {
        opaqueNested: this.nestedBytes === 'opaque',
        deferred: this.deferred,
        orders: this.orders,
        // The bytes inside a nested value are always the automatic profile.
        auto: w === 0 ? undefined : this.codec(0),
      };
      this.codecs.set(w, (codec = new Codec(this.ir, w, this.handlers, this.requireHandlers, options)));
    }
    return codec;
  }

  /** @internal The codec of the automatic profile (`psszEvolvesFrom`). */
  get automatic(): Codec {
    return this.codec(0);
  }

  /** From the PSSZ encoding of `psio::Schema` (how a schema travels). */
  static decode(bytes: Uint8Array, options?: PsszSchemaOptions): PsszSchema {
    return new PsszSchema(PsszSchema.meta().type<IrSchema>('Schema').decode(bytes), options);
  }

  /**
   * From the JSON / I-JSON text of `psio::Schema`: the text C++
   * `psio::encode(psio::ijson{}, schema)` and Rust
   * `json::to_string::<IJson, _>(&schema)` write (docs/json.md, "The Schema
   * IR as JSON"). Read by this package's own schema-driven text codec over
   * the schema of `psio::Schema` (`PsszSchema.meta()`): 64-bit integers
   * (`Array.len`, `BoundedList.maxCount`, `Enumerator.value`) are exact.
   */
  static fromJson(text: string | Uint8Array, format: JsonFormat = 'ijson', options?: PsszSchemaOptions): PsszSchema {
    return new PsszSchema(PsszSchema.meta().type<IrSchema>('Schema').jsonToValue(text, format), options);
  }

  /** The JSON / I-JSON text of this schema, byte for byte the C++ and Rust text. */
  toJson(format: JsonFormat = 'ijson'): string {
    return PsszSchema.meta().type<IrSchema>('Schema').valueToJson(this.ir, format);
  }

  /**
   * The same schema with these custom handlers (added to, and replacing by
   * id, the ones it has), this `requireHandlers` and this `nestedBytes`.
   */
  withHandlers(
    handlers: PsszCustomHandlers, options?: { requireHandlers?: boolean; nestedBytes?: PsszNestedBytes },
  ): PsszSchema {
    return this.derive({ handlers, requireHandlers: options?.requireHandlers, nestedBytes: options?.nestedBytes });
  }

  /** The PSSZ encoding of this schema. */
  encode(): Uint8Array {
    return PsszSchema.meta().type('Schema').encode(this.ir);
  }

  /** The Schema IR of `psio::Schema` itself. */
  static meta(): PsszSchema {
    return (metaSchema ??= new PsszSchema(META_SCHEMA));
  }

  /**
   * The codec for a named type or an inline type. Throws a `schema` error
   * when the type, or anything it reaches, is not expressible in PSSZ, and
   * when the schema carries no `ir_version` (or one newer than `IR_VERSION`).
   */
  type<T = unknown, V = unknown>(root: string | PsszTypeInput, options?: PsszTypeOptions): PsszType<T, V> {
    return this.rootType<T, V>(root, options);
  }

  /** @internal `type`, of a root that carries a member's constraints (a method result). */
  rootType<T = unknown, V = unknown>(
    root: string | PsszTypeInput, options?: PsszTypeOptions, constraints?: PsszRootOptions,
  ): PsszType<T, V> {
    return guarded(() => {
      // A schema written before the IR was versioned would give other
      // widths and other values: refused, not guessed at.
      checkIrVersion(this.ir);
      const width = options?.width ?? 'auto';
      if (width !== 'auto' && width !== 1 && width !== 2 && width !== 4)
        throw new PsszError('schema', 'pssz: width must be "auto", 1, 2 or 4');
      const w = width === 'auto' ? 0 : width;
      // Handlers / options given for this type alone: its own compilation.
      if (
        options?.handlers !== undefined ||
        (options?.requireHandlers !== undefined && options.requireHandlers !== this.requireHandlers) ||
        (options?.nestedBytes !== undefined && options.nestedBytes !== this.nestedBytes)
      )
        return this.derive(options).rootType<T, V>(root, { width }, constraints);
      const codec = this.codec(w);
      const ir: IrAnyType = normalizeType(root);
      const ref = codec.node(ir);
      const bound = constraints?.lengthBound == null
        ? null : parseLengthBound([{ name: 'lengthBound', value: constraints.lengthBound }]);
      const order = constraints?.order ?? null;
      if (order) codec.orderFits(ref, order);
      const member = bound || order ? new MemberInfo('', ref, bound, 'natural', 'natural', order) : null;
      return new PsszType<T, V>(codec, ref, member);
    });
  }

  /**
   * Checks the whole schema, not only what one root reaches (C++
   * `psio::pssz_ir::validate_schema`): every named type is a PSSZ type, and
   * every function of every interface and resource has at most 255
   * parameters and no more than its `maxFields`, well-formed parameter and
   * result types, and caps that parse. Call it once on a schema taken from
   * outside before dispatching through it. Never throws.
   */
  validate(): PsszSchemaValidation {
    try {
      guarded(() => {
        checkIrVersion(this.ir);
        const codec = this.codec(0);
        for (const named of this.ir.types) {
          if (named.type.value.type === 'Resource') {
            const methods = (named.type.value.value as { methods?: unknown }).methods;
            if (!Array.isArray(methods)) throw new PsszError('schema', 'pssz IR: malformed resource');
            for (const f of methods) checkFunc(codec, normalizeFunc(f));
          } else codec.node(named.type);
        }
        for (const entry of this.ir.interfaces) for (const f of normalizeInterface(entry).funcs) checkFunc(codec, f);
      });
      return { ok: true };
    } catch (e) {
      return { ok: false, error: e as PsszError };
    }
  }

  /** The names of the schema's interfaces, in order. */
  get interfaceNames(): string[] {
    return guarded(() => this.ir.interfaces.map((entry) => normalizeInterface(entry).name));
  }

  /**
   * An interface of the schema, by name (the only one when no name is
   * given): its methods, each with the codec of its argument list and of
   * its result. Throws a `schema` error when there is none, or when a
   * function is malformed (more than 255 parameters, more than its
   * `maxFields`, a cap that does not parse, a type that is not PSSZ).
   */
  interface(name?: string, /** @internal */ results?: ReadonlyMap<string, OrderTree>): PsszInterface {
    return guarded(() => {
      checkIrVersion(this.ir);
      const all = this.ir.interfaces.map(normalizeInterface);
      let found: IrInterface | undefined;
      if (name === undefined) {
        if (all.length !== 1) throw new PsszError('schema', `pssz: the schema has ${all.length} interfaces; name one`);
        found = all[0];
      } else found = all.find((entry) => entry.name === name);
      if (!found) throw new PsszError('schema', `pssz: the schema has no interface "${name}"`);
      return new PsszInterface(this, found, this.codec(0), results);
    });
  }
}

function isRecord(x: unknown): x is { [key: string]: unknown } {
  return typeof x === 'object' && x !== null && !Array.isArray(x);
}
function normalizeAttributeList(x: unknown): IrAttribute[] {
  return normalizeMember({ name: '', type: { Tuple: [] }, attributes: x }).attributes;
}
function normalizeFunc(x: unknown): IrFunc {
  if (!isRecord(x) || typeof x.name !== 'string' || !Array.isArray(x.params))
    throw new PsszError('schema', 'pssz IR: malformed function');
  return {
    name: x.name,
    params: x.params.map((p) => normalizeMember(p)),
    result: x.result === undefined || x.result === null ? null : normalizeType(x.result),
    attributes: normalizeAttributeList(x.attributes),
  };
}
function normalizeInterface(x: unknown): IrInterface {
  if (!isRecord(x) || typeof x.name !== 'string' || !Array.isArray(x.funcs))
    throw new PsszError('schema', 'pssz IR: malformed interface');
  const names = Array.isArray(x.type_names) ? x.type_names : [];
  if (names.some((n) => typeof n !== 'string')) throw new PsszError('schema', 'pssz IR: malformed interface');
  return { name: x.name, type_names: names as string[], funcs: x.funcs.map(normalizeFunc), attributes: normalizeAttributeList(x.attributes) };
}

// A function's parameter list is a tuple (docs/pssz-spec.md 8.4, 16).
function checkFunc(codec: Codec, f: IrFunc): { maxFields: number | null; cap: bigint | null } {
  if (f.params.length > 255) throw new PsszError('schema', 'pssz IR: a function has more than 255 parameters');
  const maxFields = parseMaxFields(f.attributes);
  if (maxFields !== null && f.params.length > maxFields)
    throw new PsszError('schema', 'pssz IR: a function has more parameters than its maxFields');
  const cap = parseCap(f.attributes);
  for (const param of f.params) {
    checkAttributes(param.attributes, 'record_member', true);
    codec.node(param.type);
  }
  if (f.result) codec.node(f.result);
  return { maxFields, cap };
}

/**
 * One function of an interface, from the Schema IR (`Func`): what it
 * declares, the codec of its argument list and the codec of its result.
 *
 * The arguments are, in binary, the tuple of the parameters. As a
 * JavaScript value and in text they are an object keyed by parameter name
 * when every parameter has a name (`named`; a reader takes the positional
 * array too), and the array of the arguments otherwise (`docs/json.md`
 * "Method arguments").
 *
 * A result is encoded as the type `returns(...)` makes of it: `result` is
 * the codec of `Func.result` under the function's `result.lengthBound`.
 */
export class PsszMethod<A = unknown, R = unknown> {
  readonly name: string;
  /**
   * The parameter names, in order, as the Schema IR has them: a function
   * that does not name every parameter is positional and its parameters are
   * named `"0"`, `"1"`, ... (a schema written before positional names
   * existed has `""` there and is read the same).
   */
  readonly params: readonly string[];
  /** Every parameter has a name of its own: the arguments are an object in text. */
  readonly named: boolean;
  /** The cap on the encoded arguments, or null. */
  readonly maxDynamicData: bigint | null;
  /** The cap on the number of parameters, or null. */
  readonly maxFields: number | null;
  /** The argument list as a type: the `Object` C++ gives `method_arg_tuple`. */
  readonly argsIr: IrAnyType;
  /** The codec of the argument list. */
  readonly args: PsszType<A>;
  /** The codec of the result; null: the function returns nothing. */
  readonly result: PsszType<R> | null;

  /** @internal */
  constructor(schema: PsszSchema, readonly func: IrFunc, codec: Codec, resultOrder: OrderTree | null = null) {
    const { maxFields, cap } = checkFunc(codec, func);
    this.name = func.name;
    this.params = func.params.map((p) => p.name);
    // (C++ `argument_list`: positional names are "0", "1", ...; no parameter is named so)
    this.named = func.params.every((p, i) => p.name !== '' && p.name !== String(i));
    this.maxDynamicData = cap;
    this.maxFields = maxFields;
    const attributes: IrAttribute[] = [{ name: 'tuple', value: this.named ? 'named' : 'positional' }];
    for (const name of ['maxFields', 'maxDynamicData']) {
      const attribute = findAttribute(func.attributes, name);
      if (attribute) attributes.push(attribute);
    }
    this.argsIr = {
      value: {
        type: 'Object',
        value: {
          members: func.params.map((p, i) => ({ name: this.named ? p.name : String(i), type: p.type, attributes: p.attributes })),
          attributes,
        },
      },
    };
    this.args = schema.type<A>(this.argsIr);
    const bound = findAttribute(func.attributes, 'result.lengthBound');
    this.result = func.result
      ? schema.rootType<R>(func.result, undefined, { lengthBound: bound ? bound.value ?? '' : null, order: resultOrder })
      : null;
  }

  // ── The result in one call (C++ `psio::method_result_to_pssz`, ...) ──────
  //
  // Each writes or reads the result AS `returns(...)` TYPES IT: its bounds,
  // `utf8` and element rules applied, and the offset widths of the bounded
  // type. A method that returns nothing has no result bytes.
  //
  // An empty optional result is ZERO BYTES when its payload cannot be empty
  // (a number, a record, a variant: docs/pssz-spec.md 5.5.1) — "no bytes"
  // and `null` are then the same frame. An optional whose payload can be
  // empty (a string, a list: 5.5.2) has a selector byte: `null` is `00`, a
  // present value `01` and its encoding, and no bytes are not its result.

  /** The PSSZ bytes of a result (the callee's side); no bytes for a method that returns nothing. */
  resultToPssz(value: R): Uint8Array {
    return fastWrite(() => this.resultToPsszChecked(value));
  }

  /** The validated write of a result: its bytes, or a PsszError for a value outside what `returns` declares. */
  resultToPsszChecked(value: R): Uint8Array {
    if (this.result) return this.result.encodeChecked(value);
    if (value !== undefined && value !== null)
      throw new PsszError('value', `pssz: method "${this.name}" returns nothing`);
    return new Uint8Array(0);
  }

  /** Validates and decodes a result (the caller's side); `undefined` for a method that returns nothing, which accepts only no bytes. */
  resultFromPssz(bytes: Uint8Array, options?: PsszDecodeOptions): R {
    if (this.result) return this.result.decode(bytes, options);
    const checked = this.validateResult(bytes);
    if (!checked.ok) throw checked.error;
    return undefined as R;
  }

  /** Whether `bytes` are a result of this method. Never throws. */
  validateResult(bytes: Uint8Array): PsszValidation {
    if (this.result) return this.result.validate(bytes);
    try {
      if (asBytes(bytes).length !== 0) throw new PsszError('bytes', 'pssz: a method with no result returned bytes');
      return { ok: true, hasUnknown: false };
    } catch (e) {
      return { ok: false, error: e as PsszError };
    }
  }

  /**
   * Validates a result, then views it in place (C++
   * `psio::validated_method_result_view`): the view reads the offsets of the
   * type `returns(...)` makes of the result. `undefined` for a method that
   * returns nothing.
   */
  resultView<V = unknown>(bytes: Uint8Array): V {
    const checked = this.validateResult(bytes);
    if (!checked.ok) throw checked.error;
    return (this.result ? this.result.view(bytes) : undefined) as V;
  }
}

/**
 * The JSON / I-JSON text of a `FunctionType`, byte for byte the C++ and
 * Rust text (`psio::encode(psio::json{}, functionType)`).
 */
export function functionTypeToJson(type: IrFunctionType, format: JsonFormat = 'ijson'): string {
  return PsszSchema.meta()
    .type<IrFunctionType>({ Object: { params: 'AnyType', result: { Option: 'AnyType' } } })
    .valueToJson(type, format);
}

/** One interface of a schema (`PsszSchema.interface`). */
export class PsszInterface {
  readonly name: string;
  /** The methods, in declaration order. */
  readonly methods: readonly PsszMethod[];
  private readonly byName: Map<string, PsszMethod>;

  /** @internal */
  constructor(
    /** @internal */ readonly schema: PsszSchema, readonly ir: IrInterface, /** @internal */ readonly codec: Codec,
    results?: ReadonlyMap<string, OrderTree>,
  ) {
    this.name = ir.name;
    this.methods = ir.funcs.map((f) => new PsszMethod(schema, f, codec, results?.get(f.name) ?? null));
    this.byName = new Map();
    for (const method of this.methods) {
      if (this.byName.has(method.name)) throw new PsszError('schema', `pssz IR: interface "${ir.name}" has two functions "${method.name}"`);
      this.byName.set(method.name, method);
    }
  }

  /** The method `name`; a `schema` error when the interface has none. */
  method<A = unknown, R = unknown>(name: string): PsszMethod<A, R> {
    const found = this.byName.get(name);
    if (!found) throw new PsszError('schema', `pssz: interface "${this.name}" has no method "${name}"`);
    return found as PsszMethod<A, R>;
  }
}

// ── Functional spellings (the C++ / Rust names) ─────────────────────────────

/**
 * The exclusive upper bound of a prefix scan over keys: the smallest byte
 * string greater than every byte string starting with `prefix` (trailing FF
 * bytes dropped, last byte incremented). `null`: no bound — the prefix is
 * empty or all FF, and the scan runs to the end of the key space.
 */
export function keyPrefixUpperBound(prefix: Uint8Array): Uint8Array | null {
  return guarded(() => upperBound(prefix));
}

/** JSON / I-JSON text of a `root` value -> its canonical PSSZ bytes. */
export function jsonToPssz(
  schema: PsszSchema, root: string | PsszTypeInput, format: JsonFormat, text: string | Uint8Array,
): Uint8Array {
  return schema.type(root).jsonToPssz(text, format);
}
/** PSSZ bytes of a `root` value -> its JSON / I-JSON text. */
export function psszToJson(
  schema: PsszSchema, root: string | PsszTypeInput, format: JsonFormat, bytes: Uint8Array,
): string {
  return schema.type(root).psszToJson(bytes, format);
}
/** Validates PSSZ bytes against `root`. Never throws for bad bytes. */
export function validatePssz(schema: PsszSchema, root: string | PsszTypeInput, bytes: Uint8Array): PsszValidation {
  let type: PsszType;
  try {
    type = schema.type(root);
  } catch (e) {
    return { ok: false, error: e as PsszError };
  }
  return type.validate(bytes);
}

// ── The schema of psio::Schema (cpp/include/psio/schema_ir.hpp) ─────────────

const U8: PsszTypeInput = { Int: { bits: 8, isSigned: false } };
const U32: PsszTypeInput = { Int: { bits: 32, isSigned: false } };
const U64: PsszTypeInput = { Int: { bits: 64, isSigned: false } };
const BOOL: PsszTypeInput = { Custom: { type: { Int: { bits: 1, isSigned: false } }, id: 'bool' } };
// The names of a schema are text: a JavaScript string.
const STRING: PsszTypeInput = { Custom: { type: { List: U8 }, id: 'utf8' } };
const list = (type: PsszTypeInput): PsszTypeInput => ({ List: type });
const option = (type: PsszTypeInput): PsszTypeInput => ({ Option: type });
const object = (members: { [name: string]: PsszTypeInput }): PsszTypeInput => ({ Object: members });
const ATTRIBUTES = list('Attribute');
const RECORD = object({ members: list('Member'), attributes: ATTRIBUTES });
const BOXED = object({ type: 'AnyType' });

const META_SCHEMA: PsszSchemaInput = {
  Attribute: object({ name: STRING, value: option(STRING) }),
  Member: object({ name: STRING, type: 'AnyType', attributes: ATTRIBUTES }),
  Func: object({ name: STRING, params: list('Member'), result: option('AnyType'), attributes: ATTRIBUTES }),
  Resource: object({ name: STRING, methods: list('Func'), attributes: ATTRIBUTES }),
  AnyType: object({
    value: {
      Variant: {
        Struct: RECORD,
        Object: RECORD,
        Array: object({ type: 'AnyType', len: U64 }),
        List: BOXED,
        BoundedList: object({ type: 'AnyType', maxCount: U64 }),
        Option: BOXED,
        Variant: RECORD,
        Tuple: object({ members: list('AnyType') }),
        Int: object({ bits: U32, isSigned: BOOL }),
        Float: object({ exp: U32, mantissa: U32 }),
        Nested: BOXED,
        Custom: object({ type: 'AnyType', id: STRING }),
        Resource: 'Resource',
        Type: object({ type: STRING }),
        // Added after the original 14 alternatives: a schema written before
        // it existed decodes unchanged.
        Enum: object({ type: 'AnyType', members: list('Enumerator') }),
      },
    },
  }),
  Enumerator: object({ name: STRING, value: U64 }),
  Package: object({ name: STRING, version: STRING, attributes: ATTRIBUTES }),
  UseItem: object({ name: STRING, alias: option(STRING) }),
  Use: object({ package: STRING, interface_name: STRING, version: STRING, items: list('UseItem') }),
  UseRef: object({ package: STRING, interface_name: STRING }),
  Interface: object({ name: STRING, type_names: list(STRING), funcs: list('Func'), attributes: ATTRIBUTES }),
  World: object({ name: STRING, imports: list('UseRef'), exports: list(STRING), attributes: ATTRIBUTES }),
  Schema: object({
    package: 'Package',
    interfaces: list('Interface'),
    worlds: list('World'),
    uses: list('Use'),
    types: list('Member'),
    // Appended to the record: a schema written before it has none.
    ir_version: option(U32),
  }),
};
