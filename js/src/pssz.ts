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
// bits -> bigint; floats -> number; string -> string; bytes (hex adapter) ->
// Uint8Array; enum -> the number / bigint of its underlying integer (the
// enumerator name appears only in text); record -> object; list / array /
// tuple -> array; variant -> { type, value }; optional -> null or the value (an optional of an optional
// holds null or { some: inner }); bit types -> { bytes, bitCount }. A NaN
// with a payload decodes to a PsszNaN so that it re-encodes bit for bit.

import {
  Codec, Node, PsszError, normalizeSchema, normalizeType,
  type DecodeContext, type IrAnyType, type IrSchema, type PsszSchemaInput, type PsszTypeInput,
} from './pssz-core.js';
import { jsonToValue, valueToJson, type JsonFormat } from './pssz-json.js';

export {
  PsszError, PsszNaN,
  type PsszErrorKind, type IrSchema, type IrAnyType, type IrMember, type IrAttribute, type IrTypeValue,
  type IrEnum, type IrEnumerator, type CompactEnumerator,
  type CompactType, type PsszSchemaInput, type PsszTypeInput, type Some, type VariantValue, type BitsValue,
} from './pssz-core.js';
export { type JsonFormat } from './pssz-json.js';

/** `auto`: type-derived offset widths (`psio::pssz`). 1 / 2 / 4: one inherited width (`psio::pssz_<W>`). */
export type PsszWidth = 'auto' | 1 | 2 | 4;

export interface PsszTypeOptions {
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

/** One type of a schema under one width profile. */
export class PsszType {
  /** @internal */
  constructor(private readonly codec: Codec, private readonly node: Node) {}

  /** The offset width of this type's own header / slots / offset table. */
  get width(): number {
    return guarded(() => this.codec.widthOf(this.node));
  }

  /** The canonical PSSZ bytes of `value`. */
  encode(value: unknown): Uint8Array {
    return guarded(() => this.codec.encodeAt(this.node, value, 0));
  }

  /** Decodes fully validated PSSZ bytes. */
  decode(bytes: Uint8Array, options?: PsszDecodeOptions): unknown {
    return guarded(() => {
      const input = asBytes(bytes);
      const ctx: DecodeContext = { allowUnknown: !options?.rejectUnknown, hasUnknown: false };
      return this.codec.decodeAt(this.node, input, 0, input.length, 0, ctx);
    });
  }

  /** Checks PSSZ bytes against the type. Never throws. */
  validate(bytes: Uint8Array): PsszValidation {
    try {
      const input = asBytes(bytes);
      const ctx: DecodeContext = { allowUnknown: true, hasUnknown: false };
      guarded(() => this.codec.decodeAt(this.node, input, 0, input.length, 0, ctx));
      return { ok: true, hasUnknown: ctx.hasUnknown };
    } catch (e) {
      return { ok: false, error: e as PsszError };
    }
  }

  /** JSON / I-JSON text -> value. */
  jsonToValue(text: string | Uint8Array, format: JsonFormat = 'ijson'): unknown {
    return guarded(() => jsonToValue(this.node, format, text));
  }

  /** Value -> JSON / I-JSON text. */
  valueToJson(value: unknown, format: JsonFormat = 'ijson'): string {
    return guarded(() => valueToJson(this.node, format, value));
  }

  /** JSON / I-JSON text -> canonical PSSZ bytes. */
  jsonToPssz(text: string | Uint8Array, format: JsonFormat = 'ijson'): Uint8Array {
    return guarded(() => this.codec.encodeAt(this.node, jsonToValue(this.node, format, text), 0));
  }

  /** PSSZ bytes (fully validated) -> JSON / I-JSON text. */
  psszToJson(bytes: Uint8Array, format: JsonFormat = 'ijson', options?: PsszDecodeOptions): string {
    return guarded(() => valueToJson(this.node, format, this.decode(bytes, options)));
  }
}

let metaSchema: PsszSchema | undefined;

/** A Schema IR, ready to hand out codecs for its types. */
export class PsszSchema {
  /** The canonical IR (`psio::Schema` as this library decodes it). */
  readonly ir: IrSchema;
  private readonly codecs = new Map<number, Codec>();

  /**
   * `input` is a `psio::Schema` value (as `PsszSchema.decode` produces), or
   * a `{ name: type }` map in the compact spelling, e.g.
   * `{ Point: { Object: { x: { Int: { bits: 32, isSigned: true } }, y: 'Coord' } } }`.
   */
  constructor(input: PsszSchemaInput) {
    this.ir = guarded(() => normalizeSchema(input));
  }

  /** From the PSSZ encoding of `psio::Schema` (how a schema travels). */
  static decode(bytes: Uint8Array): PsszSchema {
    return new PsszSchema(PsszSchema.meta().type('Schema').decode(bytes) as IrSchema);
  }

  /** From the JSON / I-JSON text of `psio::Schema`. */
  static fromJson(text: string | Uint8Array, format: JsonFormat = 'ijson'): PsszSchema {
    return new PsszSchema(PsszSchema.meta().type('Schema').jsonToValue(text, format) as IrSchema);
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
   * when the type, or anything it reaches, is not expressible in PSSZ.
   */
  type(root: string | PsszTypeInput, options?: PsszTypeOptions): PsszType {
    return guarded(() => {
      const width = options?.width ?? 'auto';
      if (width !== 'auto' && width !== 1 && width !== 2 && width !== 4)
        throw new PsszError('schema', 'pssz: width must be "auto", 1, 2 or 4');
      const w = width === 'auto' ? 0 : width;
      let codec = this.codecs.get(w);
      if (!codec) this.codecs.set(w, (codec = new Codec(this.ir, w)));
      const ir: IrAnyType = normalizeType(root);
      return new PsszType(codec, codec.node(ir));
    });
  }
}

// ── Functional spellings (the C++ / Rust names) ─────────────────────────────

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
const STRING: PsszTypeInput = { Custom: { type: { List: U8 }, id: 'string' } };
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
        FracPack: BOXED,
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
  }),
};
