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
  Codec, IR_VERSION, Node, PsszError, checkIrVersion, normalizeSchema, normalizeType, reservedCustomId,
  type DecodeContext, type IrAnyType, type IrSchema, type PsszCustomHandler, type PsszSchemaInput,
  type PsszTypeInput,
} from './pssz-core.js';
import { jsonToValue, valueToJson, type JsonFormat } from './pssz-json.js';
import { reservedTimeId } from './time.js';
import { decodeKey, encodeKey, encodeKeyPrefix, keyPrefixUpperBound as upperBound } from './pssz-key.js';
import { viewAt } from './pssz-view.js';

export {
  PsszError, PsszNaN, PsszNegativeZero, IR_VERSION,
  type PsszErrorKind, type IrSchema, type IrAnyType, type IrMember, type IrAttribute, type IrTypeValue,
  type IrEnum, type IrEnumerator, type IrFunc, type IrResource, type IrInterface, type IrWorld, type IrUse,
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

  /** @internal `ref` is the type as written (the text walk starts there). */
  constructor(private readonly codec: Codec, private readonly ref: Node) {
    ({ node: this.node, boxed: this.boxed } = codec.wireOf(ref));
  }

  /**
   * The offset width of this type's own header / slots / offset table. A
   * boxed value has no inferred bound: 4 under the automatic profile.
   */
  get width(): number {
    return guarded(() => (this.boxed && this.codec.W === 0 ? 4 : this.codec.widthOf(this.node)));
  }

  /**
   * The inferred size bound of the type (`max_encoded_size`,
   * `docs/pssz-spec.md` §3.2): the largest encoding in the automatic
   * profile, each header, slot and offset table counted at its own width;
   * `null` when unbounded (a plain string or list, a boxed value, anything
   * holding one). The type's own `maxDynamicData` is not looked at — it
   * rules the width whatever this is — while a capped record inside
   * contributes its cap.
   */
  get maxEncodedSize(): bigint | null {
    return guarded(() => (this.boxed ? null : this.codec.inferredBound(this.node)));
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

  /** The canonical PSSZ bytes of `value`. */
  encode(value: T): Uint8Array {
    return guarded(() => this.codec.encodeTop(this.ref, value));
  }

  /** Decodes fully validated PSSZ bytes. */
  decode(bytes: Uint8Array, options?: PsszDecodeOptions): T {
    return guarded(() => {
      const input = asBytes(bytes);
      const ctx: DecodeContext = { allowUnknown: !options?.rejectUnknown, hasUnknown: false };
      return this.codec.decodeTop(this.ref, input, ctx) as T;
    });
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
      return viewAt(this.codec, this.ref, input, 0, input.length, 0) as V;
    });
  }

  /** Checks PSSZ bytes against the type. Never throws. */
  validate(bytes: Uint8Array): PsszValidation {
    try {
      const input = asBytes(bytes);
      const ctx: DecodeContext = { allowUnknown: true, hasUnknown: false };
      guarded(() => this.codec.decodeTop(this.ref, input, ctx));
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
    return guarded(() => {
      this.codec.checkValue(this.ref, value, 'value');
      return encodeKey(this.codec, this.node, value);
    });
  }

  /**
   * Decodes key bytes. Checked: exactly one canonical key spanning the
   * whole input, or a PsszError.
   */
  decodeKey(bytes: Uint8Array): T {
    return guarded(() => decodeKey(this.codec, this.ref, asBytes(bytes))) as T;
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
    return guarded(() => encodeKeyPrefix(this.codec, this.ref, elements));
  }

  /** JSON / I-JSON text -> value. */
  jsonToValue(text: string | Uint8Array, format: JsonFormat = 'ijson'): T {
    return guarded(() => jsonToValue(this.codec, this.ref, format, text)) as T;
  }

  /** Value -> JSON / I-JSON text. */
  valueToJson(value: T, format: JsonFormat = 'ijson'): string {
    return guarded(() => valueToJson(this.codec, this.ref, format, value));
  }

  /** JSON / I-JSON text -> canonical PSSZ bytes. */
  jsonToPssz(text: string | Uint8Array, format: JsonFormat = 'ijson'): Uint8Array {
    return guarded(() => this.codec.encodeTop(this.ref, jsonToValue(this.codec, this.ref, format, text)));
  }

  /** PSSZ bytes (fully validated) -> JSON / I-JSON text. */
  psszToJson(bytes: Uint8Array, format: JsonFormat = 'ijson', options?: PsszDecodeOptions): string {
    // The decode runs the handlers' checks; the writer need not repeat them.
    return guarded(() => valueToJson(this.codec, this.ref, format, this.decode(bytes, options), false));
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
      throw new PsszError('schema', `pssz: custom id "${id}" is part of the binary form and takes no handler`);
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
      });
    });
  }

  private codec(w: 0 | 1 | 2 | 4): Codec {
    let codec = this.codecs.get(w);
    if (!codec) {
      const options = {
        opaqueNested: this.nestedBytes === 'opaque',
        deferred: this.deferred,
        // The bytes inside a nested value are always the automatic profile.
        auto: w === 0 ? undefined : this.codec(0),
      };
      this.codecs.set(w, (codec = new Codec(this.ir, w, this.handlers, this.requireHandlers, options)));
    }
    return codec;
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
        return this.derive(options).type<T, V>(root, { width });
      const codec = this.codec(w);
      const ir: IrAnyType = normalizeType(root);
      return new PsszType<T, V>(codec, codec.node(ir));
    });
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
