// pssz-core.ts — schema-driven PSSZ codec over the Schema IR.
//
// A port of the checked runtime codec `cpp/include/psio/pssz_ir.hpp` (and its
// Rust twin `psio::pssz::dynamic`): the same width derivation, the same frame
// parser, the same accept / reject set.
//
// This file is internal. The public API is `pssz.ts`.

import { parseTimeSchemaId, reservedTimeId, timeFractionDigits, type TimeKind } from './time.js';
import { encodeKey } from './pssz-key.js';
import { jsonTextValue, jsonValueText } from './pssz-json.js';

// ── Errors ──────────────────────────────────────────────────────────────────

/**
 * - `schema`: the Schema IR is malformed or uses something PSSZ cannot carry.
 * - `value`: a JavaScript value does not fit the schema (encode / text write).
 * - `bytes`: PSSZ input was rejected.
 * - `json`: JSON / I-JSON text was rejected.
 * - `internal`: a defect in this library (never expected).
 */
export type PsszErrorKind = 'schema' | 'value' | 'bytes' | 'json' | 'internal';

/** The only error the PSSZ / JSON entry points throw or return. */
export class PsszError extends Error {
  constructor(
    readonly kind: PsszErrorKind,
    message: string,
    /** Byte offset in the text for `json` errors. */
    readonly offset?: number,
    /**
     * The error is a type's own: the invariant of a custom type (the `check`
     * of its handler) refused a structurally valid value. `message` is the
     * invariant's message, unchanged.
     */
    readonly invariant = false,
  ) {
    super(message);
    this.name = 'PsszError';
  }
}

/** `kMaxValidationDepth`: value nodes below the root, and JSON nesting. */
export const MAX_DEPTH = 64;

// A fixed-count array of zero-size elements carries no bytes, so its count is
// not bounded by the input. C++ materializes any count; this port refuses
// more than this many rather than loop on a schema-supplied number.
const ZERO_SIZE_ARRAY_LIMIT = 1 << 20;

const U64_MAX = (1n << 64n) - 1n;

// ── Schema IR (mirrors cpp/include/psio/schema_ir.hpp) ──────────────────────
//
// This is exactly the value the PSSZ / I-JSON encoding of `psio::Schema`
// decodes to under this library's own value model: a record is an object, a
// variant is `{ type, value }`, an optional is `null` or the value, a 64-bit
// integer is a bigint.

export interface IrAttribute {
  name: string;
  value: string | null;
}
export interface IrMember {
  name: string;
  type: IrAnyType;
  attributes: IrAttribute[];
}
export interface IrRecord {
  members: IrMember[];
  attributes: IrAttribute[];
}
export type IrTypeValue =
  | { type: 'Struct'; value: IrRecord }
  | { type: 'Object'; value: IrRecord }
  | { type: 'Array'; value: { type: IrAnyType; len: bigint } }
  | { type: 'List'; value: { type: IrAnyType } }
  | { type: 'BoundedList'; value: { type: IrAnyType; maxCount: bigint } }
  | { type: 'Option'; value: { type: IrAnyType } }
  | { type: 'Variant'; value: IrRecord }
  | { type: 'Tuple'; value: { members: IrAnyType[] } }
  | { type: 'Int'; value: { bits: number; isSigned: boolean } }
  | { type: 'Float'; value: { exp: number; mantissa: number } }
  | { type: 'Nested'; value: { type: IrAnyType } }
  | { type: 'Custom'; value: { type: IrAnyType; id: string } }
  | { type: 'Resource'; value: IrResource }
  | { type: 'Type'; value: { type: string } }
  | { type: 'Enum'; value: IrEnum };
/** `psio::schema_types::Enumerator`. */
export interface IrEnumerator {
  name: string;
  /** The enumerator's value sign-extended to 64 bits, as an unsigned integer. */
  value: bigint;
}
/** `psio::schema_types::Enum`: on the wire exactly `type`, its underlying integer. */
export interface IrEnum {
  type: IrAnyType;
  members: IrEnumerator[];
}
export interface IrAnyType {
  value: IrTypeValue;
}
/** `psio::schema_types::Func`. */
export interface IrFunc {
  name: string;
  params: IrMember[];
  result: IrAnyType | null;
  attributes: IrAttribute[];
}
/**
 * `psio::schema_types::FunctionType`: a function outside an `Interface` —
 * its argument list and its result as `returns` types it (`null`: none).
 */
export interface IrFunctionType {
  params: IrAnyType;
  result: IrAnyType | null;
}
/** `psio::schema_types::Resource`. */
export interface IrResource {
  name: string;
  methods: IrFunc[];
  attributes: IrAttribute[];
}
/** `psio::schema_types::Interface`. */
export interface IrInterface {
  name: string;
  type_names: string[];
  funcs: IrFunc[];
  attributes: IrAttribute[];
}
/** `psio::schema_types::World`. */
export interface IrWorld {
  name: string;
  imports: { package: string; interface_name: string }[];
  exports: string[];
  attributes: IrAttribute[];
}
/** `psio::schema_types::Use`. */
export interface IrUse {
  package: string;
  interface_name: string;
  version: string;
  items: { name: string; alias: string | null }[];
}
/**
 * `psio::Schema`. Only `types` is read by the codecs; the envelope
 * (package, interfaces, worlds, uses) describes an API and is carried as is.
 */
export interface IrSchema {
  package: { name: string; version: string; attributes: IrAttribute[] };
  interfaces: IrInterface[];
  worlds: IrWorld[];
  uses: IrUse[];
  types: IrMember[];
  /**
   * The version of the IR's meaning the schema was written in
   * (`IR_VERSION`), or `null` for a schema written before the marker
   * existed. The field was appended to the record: older schemas decode.
   */
  ir_version: number | null;
}

/**
 * The version of the IR's meaning that this library writes and reads (C++
 * `psio::schema_ir_version`, Rust `IR_VERSION`).
 *
 * - 1: a boxed value is marked `Custom{.., "box"}`, a UTF-8 string is
 *   `"utf8"` and `"string"` is a raw string.
 *
 * A schema with no `ir_version` predates those markers: read as version 1 it
 * would silently derive other offset widths and return bytes for text, so
 * `PsszSchema.type` refuses it instead of guessing.
 */
export const IR_VERSION = 1;

/** What a schema-driven codec checks before it compiles a type. */
export function checkIrVersion(schema: IrSchema): void {
  if (schema.ir_version === IR_VERSION) return;
  if (schema.ir_version === null)
    schemaError('pssz IR: schema has no ir_version marker (it predates the box / utf8 markers and would derive different widths); regenerate it');
  schemaError('pssz IR: schema ir_version is newer than this library');
}

/**
 * The compact spelling of a type. Accepted wherever a type is expected and
 * normalized to `IrAnyType`.
 *
 * Byte containers: `Custom` id `"utf8"` is text (a JavaScript `string`),
 * `"string"` is a raw string and `"hex"` is bytes (both a `Uint8Array`).
 */
export type CompactType =
  | string
  | { Int: { bits: number; isSigned: boolean } }
  | { Float: { exp: number; mantissa: number } }
  | { List: PsszTypeInput }
  | { Option: PsszTypeInput }
  | { Nested: PsszTypeInput }
  | { Array: { type: PsszTypeInput; len: number | bigint } }
  | { BoundedList: { type: PsszTypeInput; maxCount: number | bigint } }
  | { Tuple: PsszTypeInput[] }
  | { Custom: { type: PsszTypeInput; id: string } }
  | { Enum: { type: PsszTypeInput; members: CompactEnumerator[] | { [name: string]: number | bigint } } }
  | { Struct: CompactRecord }
  | { Object: CompactRecord }
  | { Variant: CompactRecord };
export type CompactRecord =
  | { [member: string]: PsszTypeInput }
  | { members: CompactMember[]; attributes?: CompactAttribute[] };
export interface CompactMember {
  name: string;
  type: PsszTypeInput;
  attributes?: CompactAttribute[];
}
/** `value` may be negative (it is sign-extended to 64 bits). */
export interface CompactEnumerator {
  name: string;
  value: number | bigint | string;
}
export interface CompactAttribute {
  name: string;
  value?: string | null;
}
export type PsszTypeInput = IrAnyType | CompactType;
/**
 * A schema: the full IR (as decoded from its PSSZ / JSON form), the full
 * form written by hand (`types` and the `ir_version` it is written in), or
 * a map of named types, which is always in the current IR version.
 */
export type PsszSchemaInput =
  | IrSchema
  | { types: CompactMember[]; ir_version?: number | null }
  | { [name: string]: PsszTypeInput };

function schemaError(message: string): never {
  throw new PsszError('schema', message);
}

const IR_KINDS = new Set([
  'Struct', 'Object', 'Array', 'List', 'BoundedList', 'Option', 'Variant', 'Tuple', 'Int',
  'Float', 'Nested', 'Custom', 'Resource', 'Type', 'Enum',
]);

function isObject(x: unknown): x is Record<string, unknown> {
  return typeof x === 'object' && x !== null && !Array.isArray(x);
}

function toU64(x: unknown, what: string): bigint {
  let v: bigint;
  if (typeof x === 'bigint') v = x;
  else if (typeof x === 'number' && Number.isSafeInteger(x)) v = BigInt(x);
  else if (typeof x === 'string' && /^(0|[1-9][0-9]{0,19})$/.test(x)) v = BigInt(x);
  else return schemaError(`pssz IR: ${what} must be an unsigned integer`);
  if (v < 0n || v > U64_MAX) schemaError(`pssz IR: ${what} out of range`);
  return v;
}
// An enumerator value: an unsigned 64-bit integer, or a negative one of at
// least -2^63, stored sign-extended to 64 bits.
function toEnumValue(x: unknown): bigint {
  const negative = typeof x === 'bigint' ? x < 0n : typeof x === 'number' && Number.isSafeInteger(x) && x < 0;
  if (!negative) return toU64(x, 'Enumerator.value');
  const v = BigInt(x as number | bigint);
  if (v < -(1n << 63n)) schemaError('pssz IR: Enumerator.value out of range');
  return BigInt.asUintN(64, v);
}
function toU32(x: unknown, what: string): number {
  if (typeof x !== 'number' || !Number.isInteger(x) || x < 0 || x > 0xffffffff)
    schemaError(`pssz IR: ${what} must be a 32-bit unsigned integer`);
  return x as number;
}
function toStr(x: unknown, what: string): string {
  if (typeof x !== 'string') schemaError(`pssz IR: ${what} must be a string`);
  return x as string;
}

function normalizeAttributes(x: unknown): IrAttribute[] {
  if (x === undefined || x === null) return [];
  if (!Array.isArray(x)) schemaError('pssz IR: attributes must be an array');
  return (x as unknown[]).map((a) => {
    if (!isObject(a)) return schemaError('pssz IR: malformed attribute');
    const value = a.value === undefined || a.value === null ? null : toStr(a.value, 'attribute value');
    return { name: toStr(a.name, 'attribute name'), value };
  });
}
export function normalizeMember(x: unknown, depth = 0): IrMember {
  if (!isObject(x)) return schemaError('pssz IR: malformed member');
  return {
    name: toStr(x.name, 'member name'),
    type: normalizeType(x.type, depth),
    attributes: normalizeAttributes(x.attributes),
  };
}
function normalizeRecord(x: unknown, depth: number): IrRecord {
  if (!isObject(x)) return schemaError('pssz IR: malformed record');
  if (Array.isArray(x.members)) {
    return {
      members: x.members.map((m) => normalizeMember(m, depth)),
      attributes: normalizeAttributes(x.attributes),
    };
  }
  // Compact: { name: type, ... } in declaration order.
  return {
    members: Object.keys(x).map((name) => ({
      name,
      type: normalizeType(x[name], depth),
      attributes: [],
    })),
    attributes: [],
  };
}

/** Validates and copies a type in either spelling into the canonical IR. */
export function normalizeType(x: unknown, depth = 0): IrAnyType {
  if (depth > 256) schemaError('pssz IR: schema nesting too deep');
  if (typeof x === 'string') return { value: { type: 'Type', value: { type: x } } };
  if (!isObject(x)) return schemaError('pssz IR: malformed type');
  let kind: string;
  let body: unknown;
  if (isObject(x.value) && typeof x.value.type === 'string' && 'value' in x.value) {
    kind = x.value.type;
    body = x.value.value;
  } else {
    const keys = Object.keys(x);
    if (keys.length !== 1) return schemaError('pssz IR: malformed type');
    kind = keys[0];
    body = x[kind];
    // Compact single-child forms: { List: T }, { Option: T }, { Tuple: [T] }.
    if (kind === 'List' || kind === 'Option' || kind === 'Nested') body = { type: body };
    else if (kind === 'Tuple') body = { members: body };
  }
  if (!IR_KINDS.has(kind)) return schemaError(`pssz IR: unknown type kind "${kind}"`);
  const d = depth + 1;
  switch (kind) {
    case 'Struct':
    case 'Object':
    case 'Variant':
      return { value: { type: kind, value: normalizeRecord(body, d) } };
    case 'Resource':
      return { value: { type: 'Resource', value: body as IrResource } };
    default:
      break;
  }
  if (!isObject(body)) return schemaError('pssz IR: malformed type');
  switch (kind) {
    case 'Array':
      return {
        value: { type: kind, value: { type: normalizeType(body.type, d), len: toU64(body.len, 'Array.len') } },
      };
    case 'BoundedList':
      return {
        value: {
          type: kind,
          value: { type: normalizeType(body.type, d), maxCount: toU64(body.maxCount, 'BoundedList.maxCount') },
        },
      };
    case 'List':
    case 'Option':
    case 'Nested':
      return { value: { type: kind, value: { type: normalizeType(body.type, d) } } };
    case 'Tuple':
      if (!Array.isArray(body.members)) return schemaError('pssz IR: malformed tuple');
      return { value: { type: kind, value: { members: body.members.map((m) => normalizeType(m, d)) } } };
    case 'Int':
      if (typeof body.isSigned !== 'boolean') return schemaError('pssz IR: Int.isSigned must be a boolean');
      return { value: { type: kind, value: { bits: toU32(body.bits, 'Int.bits'), isSigned: body.isSigned } } };
    case 'Float':
      return {
        value: {
          type: kind,
          value: { exp: toU32(body.exp, 'Float.exp'), mantissa: toU32(body.mantissa, 'Float.mantissa') },
        },
      };
    case 'Custom':
      return { value: { type: kind, value: { type: normalizeType(body.type, d), id: toStr(body.id, 'Custom.id') } } };
    case 'Enum': {
      let members: IrEnumerator[];
      if (Array.isArray(body.members)) {
        members = body.members.map((m) => {
          if (!isObject(m)) return schemaError('pssz IR: malformed enumerator');
          return { name: toStr(m.name, 'Enumerator.name'), value: toEnumValue(m.value) };
        });
      } else if (isObject(body.members)) {
        // Compact: { name: value, ... } in declaration order.
        const map = body.members;
        members = Object.keys(map).map((name) => ({ name, value: toEnumValue(map[name]) }));
      } else return schemaError('pssz IR: malformed enum');
      return { value: { type: kind, value: { type: normalizeType(body.type, d), members } } };
    }
    default:
      return { value: { type: 'Type', value: { type: toStr(body.type, 'Type.type') } } };
  }
}

/** Validates and copies a schema in either spelling into the canonical IR. */
export function normalizeSchema(x: unknown): IrSchema {
  if (!isObject(x)) return schemaError('pssz IR: malformed schema');
  let types: IrMember[];
  // A map of named types is written here and now, in the current IR. The
  // full form says for itself which version it is (absent: none).
  let version: number | null = IR_VERSION;
  let pkg = { name: '', version: '', attributes: [] as IrAttribute[] };
  let rest: { interfaces: IrInterface[]; worlds: IrWorld[]; uses: IrUse[] } = {
    interfaces: [], worlds: [], uses: [],
  };
  if (Array.isArray(x.types)) {
    types = x.types.map((m) => normalizeMember(m, 0));
    const v = x.ir_version;
    if (v === undefined || v === null) version = null;
    else if (typeof v === 'number' && Number.isInteger(v) && v >= 0 && v <= 0xffffffff) version = v;
    else if (typeof v === 'bigint' && v >= 0n && v <= 0xffffffffn) version = Number(v);
    else schemaError('pssz IR: ir_version must be a 32-bit unsigned integer');
    if (isObject(x.package)) {
      pkg = {
        name: toStr(x.package.name ?? '', 'package name'),
        version: toStr(x.package.version ?? '', 'package version'),
        attributes: normalizeAttributes(x.package.attributes),
      };
    }
    rest = {
      interfaces: Array.isArray(x.interfaces) ? x.interfaces : [],
      worlds: Array.isArray(x.worlds) ? x.worlds : [],
      uses: Array.isArray(x.uses) ? x.uses : [],
    };
  } else {
    types = Object.keys(x).map((name) => ({ name, type: normalizeType(x[name]), attributes: [] }));
  }
  return { package: pkg, ...rest, types, ir_version: version };
}

// ── UTF-8 ───────────────────────────────────────────────────────────────────

/** Code point of the last sequence `utf8Multibyte` accepted. */
export let utf8CodePoint = 0;

/**
 * Length (2..4) of the well-formed multi-byte sequence at `p`, or 0: rejects
 * overlong forms, surrogates and code points above U+10FFFF.
 */
export function utf8Multibyte(b: Uint8Array, p: number, end: number): number {
  const c = b[p];
  let n: number;
  let cp: number;
  let min: number;
  if (c >= 0xc2 && c <= 0xdf) { n = 2; cp = c & 0x1f; min = 0x80; }
  else if (c >= 0xe0 && c <= 0xef) { n = 3; cp = c & 0x0f; min = 0x800; }
  else if (c >= 0xf0 && c <= 0xf4) { n = 4; cp = c & 0x07; min = 0x10000; }
  else return 0;
  if (end - p < n) return 0;
  for (let k = 1; k < n; ++k) {
    const x = b[p + k];
    if ((x & 0xc0) !== 0x80) return 0;
    cp = (cp << 6) | (x & 0x3f);
  }
  if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
  utf8CodePoint = cp;
  return n;
}

export function isNoncharacter(cp: number): boolean {
  return (cp >= 0xfdd0 && cp <= 0xfdef) || (cp & 0xfffe) === 0xfffe;
}

export function utf8Valid(b: Uint8Array, start: number, end: number): boolean {
  let p = start;
  while (p < end) {
    if (b[p] < 0x80) { ++p; continue; }
    const n = utf8Multibyte(b, p, end);
    if (n === 0) return false;
    p += n;
  }
  return true;
}

/** A JavaScript string is text only when it has no unpaired surrogate. */
export function wellFormed(s: string): boolean {
  for (let i = 0; i < s.length; ++i) {
    const c = s.charCodeAt(i);
    if (c < 0xd800 || c > 0xdfff) continue;
    if (c > 0xdbff || i + 1 >= s.length) return false;
    const d = s.charCodeAt(i + 1);
    if (d < 0xdc00 || d > 0xdfff) return false;
    ++i;
  }
  return true;
}

/** UTF-8 byte length of a well-formed string. */
export function utf8Length(s: string): number {
  let n = 0;
  for (let i = 0; i < s.length; ++i) {
    const c = s.charCodeAt(i);
    if (c < 0x80) n += 1;
    else if (c < 0x800) n += 2;
    else if (c >= 0xd800 && c <= 0xdbff) { n += 4; ++i; }
    else n += 3;
  }
  return n;
}

const textEncoder = new TextEncoder();
// ignoreBOM: a leading U+FEFF is data, not a signature to strip.
const textDecoder = new TextDecoder('utf-8', { ignoreBOM: true });

export function utf8Encode(s: string): Uint8Array {
  return textEncoder.encode(s);
}
/** Decodes bytes already known to be valid UTF-8. */
export function utf8Decode(b: Uint8Array, start: number, end: number): string {
  return textDecoder.decode(b.subarray(start, end));
}

// ── Compiled schema graph ───────────────────────────────────────────────────

export interface LengthBound {
  exact: bigint | null;
  max: bigint | null;
  min: bigint | null;
}
function boundAdmits(bound: LengthBound, n: number): boolean {
  const v = BigInt(n);
  return !(
    (bound.max !== null && v > bound.max) ||
    (bound.exact !== null && v !== bound.exact) ||
    (bound.min !== null && v < bound.min)
  );
}

export type IntForm = 'natural' | 'number' | 'string';

/**
 * What a custom type (a `Custom` id that is not part of the binary form)
 * adds to its wire type. Every member is optional.
 *
 * - `toText` / `fromText`: the text form, ONE JSON string in both text
 *   formats. `toText` gets the decoded value of the wire type, `fromText`
 *   returns one; either may throw for a value / text it does not accept.
 *   Without them the type's text is its wire type's.
 * - `check`: the invariant. It gets the decoded value of the wire type
 *   (structurally valid, as `decode` gives it) and returns a message — or
 *   throws an `Error` with one — when the value is not a value of the type;
 *   anything else accepts it. It runs on every value of the type, on the
 *   binary and the text entry points, values inside it first, and the
 *   message is the error's, unchanged.
 */
export interface PsszCustomHandler {
  toText?(value: unknown): string;
  fromText?(text: string): unknown;
  check?(value: unknown): string | null | undefined | void;
}

// Custom ids that are part of the binary form (pssz_ir.hpp "Custom ids").
// Any other id is "box" or a custom type: its inner type on the wire.
const WIRE_CUSTOM_IDS = new Set([
  'bool', 'char', 'string', 'utf8', 'hex', 'bitvector', 'bitlist', 'map', 'set', 'unknown',
]);
/**
 * Ids a handler cannot be registered for: the binary-form ids, "box",
 * "json" (any JSON value, `jsonValue`) and the built-in presentations (the
 * time ids of docs/time.md).
 */
export function reservedCustomId(id: string): boolean {
  return id === 'box' || id === 'json' || WIRE_CUSTOM_IDS.has(id) || reservedTimeId(id);
}

/**
 * The exact text of a JSON value (C++ `psio::json_value` keeps a number as
 * the token it was written as). A `json` node (`Custom{utf8, "json"}`,
 * typed: `jsonValue`) decodes to a plain JavaScript value — `1.50` becomes
 * `1.5`, an object's integer-like keys move to the front — which is the
 * same JSON value but not always the same text. Where the text itself
 * matters, encode a `PsszJsonText`: its text is checked to be one JSON
 * value and stored compact (no whitespace, strings re-escaped, number
 * tokens and member order unchanged). It may stand anywhere inside the
 * value of a `json` node.
 */
export class PsszJsonText {
  constructor(readonly text: string) {}
  toString(): string {
    return this.text;
  }
}

// The node a reference is on the wire: wrappers ("box", text presentations)
// looked through. `boxed`: a "box" was on the way.
function strip(ref: Node): { node: Node; boxed: boolean } {
  let boxed = false;
  for (let hops = 0; ref.kind === 'custom'; ++hops) {
    if (!ref.inner || hops > 4096) schemaError('pssz IR: cyclic named aliases');
    boxed = boxed || ref.boxed;
    ref = ref.inner;
  }
  return { node: ref, boxed };
}

/** One member of a record / tuple / variant. */
export class MemberInfo {
  private wire?: { node: Node; boxed: boolean };
  constructor(
    readonly name: string,
    /** The member's type as written: wrappers included (the text walk). */
    readonly text: Node,
    readonly bound: LengthBound | null,
    readonly jsonInt: IntForm,
    readonly ijsonInt: IntForm,
    /** Typed-only order constraints of the member and the positions below it. */
    readonly order: OrderTree | null = null,
  ) {}
  /** The member's type on the wire. */
  get node(): Node {
    return (this.wire ??= strip(this.text)).node;
  }
  /** The member is a boxed value: no inferred size bound. */
  get boxed(): boolean {
    return (this.wire ??= strip(this.text)).boxed;
  }
}

/**
 * The order constraints of one position (`docs/pssz-spec.md` §3.5, "Order
 * constraints"): `sorted` / `unique` / `unique_keys` on a list or array, and
 * those of the positions below it. They are typed-only — not in the Schema
 * IR — and are handed to the codec beside the schema.
 */
export interface OrderTree {
  /** The elements are in order: ascending (1) or descending (-1) key bytes; `strict`: no two equal. */
  sorted?: { direction: 1 | -1; strict: boolean };
  /** No two elements have the same key (the element, or element 0 of a pair / tuple). */
  uniqueKeys?: boolean;
  element?: OrderTree;
  keys?: OrderTree;
  values?: OrderTree;
  at?: ReadonlyMap<string, OrderTree>;
}
/** Named type -> member name -> the member's order constraints. */
export type OrderTable = ReadonlyMap<string, ReadonlyMap<string, OrderTree>>;

export type NodeKind =
  | 'int' | 'float' | 'bool' | 'char' | 'string' | 'hex' | 'bitvector' | 'bitlist'
  | 'option' | 'list' | 'array' | 'tuple' | 'record' | 'variant'
  // A byte list holding the automatic-profile encoding of `textChild`.
  | 'nested'
  // A wrapper that adds nothing to the bytes: "box" or a text presentation.
  // Only the text walk and `strip` see it; the binary codec works on `child`
  // / `MemberInfo.node`, which look through it.
  | 'custom';

interface Layout {
  fixed: boolean;
  size: number;
  optional: boolean;
}
interface Memo<T> {
  value?: T;
  error?: string;
  /** Schema recursion the computation needs below its entry. */
  height: number;
}

/** One resolved IR type. */
export class Node {
  kind: NodeKind = 'int';
  // int: bits / signed / size. float: size. bit types: size = bit count.
  bits = 0;
  signed = false;
  size = 0;
  /**
   * option payload; list / array element; string / hex byte container, as
   * written (wrappers included). The binary codec uses `child`.
   */
  textChild!: Node;
  private wireChild?: { node: Node; boxed: boolean };
  /** `textChild` on the wire. */
  get child(): Node {
    return (this.wireChild ??= strip(this.textChild)).node;
  }
  /** `textChild` is a boxed value: no inferred size bound. */
  get childBoxed(): boolean {
    return (this.wireChild ??= strip(this.textChild)).boxed;
  }
  /** string: raw bytes (Custom "string"), not validated as UTF-8 ("utf8"). */
  raw = false;
  /**
   * string: a "json" node — the UTF-8 string holds the compact text of one
   * JSON value, which is the node's JavaScript value and its text form.
   */
  json = false;
  /** variant: the attribute `untagged` — in text the bare value of the alternative. */
  untagged = false;
  /** untagged variant: its alternatives are told apart by JSON kind (memo). */
  untaggedOk?: boolean;
  /**
   * list: a map (`textChild` is the entry tuple [key, value]) or a set: the
   * entries are in strictly ascending order of the key bytes of the key.
   */
  assoc?: 'map' | 'set';
  /** nested: this node's bytes are not looked at (a typed deferred member). */
  opaque = false;
  /** map / set: the key type as written (map: the entry's first member). */
  get keyRef(): Node {
    return this.assoc === 'map' ? strip(this.textChild).node.members[0].text : this.textChild;
  }
  /** map: the mapped type as written. */
  get valueRef(): Node {
    return strip(this.textChild).node.members[1].text;
  }
  /** map / set: a Nested node occurs in the key type. */
  keyNestedMemo?: boolean;
  /** custom wrapper: the wrapped type, the Custom id, "box", the handler. */
  inner?: Node;
  id = '';
  boxed = false;
  handler?: PsszCustomHandler;
  /** array length or bounded-list maximum. */
  count = 0n;
  /** `count` as a number (Infinity beyond 2^53). */
  countNum = 0;
  /** list kinds: `bounded` (array or bounded list), `exact` (array). */
  bounded = false;
  exact = false;
  members: MemberInfo[] = [];
  /** record: no header (Struct, or Object with definitionWillNotChange). */
  stable = false;
  /** record: explicit maxDynamicData. */
  cap: bigint | null = null;
  /**
   * record: an argument list (the `tuple` attribute). `named`: an object in
   * text, and a reader takes the positional array too. `positional`: the
   * members are named "0", "1", ...; the text and the JavaScript value are
   * the array.
   */
  tuple: 'named' | 'positional' | null = null;
  /** record: its `maxFields` cap (automatic profile), or null. */
  maxFields: number | null = null;
  /**
   * int: a time value (Custom time id over the tick integer, docs/time.md).
   * Its value is always a bigint tick count.
   */
  time: TimeKind | null = null;
  /** time point: fraction digits of its RFC 3339 text; -1 for no text form. */
  timeDigits = -1;

  layoutMemo?: Memo<Layout>;
  emptyMemo?: Memo<boolean>;
  /** record: the width of its header and slots (its own wherever it is). */
  width?: number;
  boundMemo: (bigint | null | undefined)[] = [undefined, undefined];
  /** Not a record: its own bound (no position cap), per member count. */
  ownBound?: bigint | null;
  ownBoundDone = false;
  countBound?: Map<bigint, bigint | null>;
  fixedPayloadMemo?: boolean;
  /** `Codec.maxEncodedSize` of the node at the top level, once computed. */
  maxSize?: bigint | null;
  /** Field layouts, by offset width. */
  fields?: FieldLayout[][];
  byName?: Map<string, number>;
  /**
   * An Enum (kind `int`, the underlying integer): the enumerators in
   * declaration order, `value` as the 64 two's-complement bits the IR holds.
   * It only affects text.
   */
  enumerators?: IrEnumerator[];
}

interface FieldLayout {
  fixedSize: number;
  variable: boolean;
  optional: boolean;
}

export function findAttribute(attributes: IrAttribute[], name: string): IrAttribute | undefined {
  for (const a of attributes) if (a.name === name) return a;
  return undefined;
}
function parseU64(text: string): bigint | null {
  if (!/^(0|[1-9][0-9]{0,19})$/.test(text)) return null;
  const v = BigInt(text);
  return v > U64_MAX ? null : v;
}
export function parseLengthBound(attributes: IrAttribute[]): LengthBound | null {
  const attribute = findAttribute(attributes, 'lengthBound');
  if (!attribute) return null;
  if (attribute.value === null || attribute.value === '')
    return schemaError('pssz IR: malformed lengthBound attribute');
  const bound: LengthBound = { exact: null, max: null, min: null };
  for (const item of attribute.value.split(',')) {
    const equals = item.indexOf('=');
    const n = equals < 0 ? null : parseU64(item.slice(equals + 1));
    const key = item.slice(0, equals);
    if (n === null || (key !== 'max' && key !== 'exact' && key !== 'min') || bound[key] !== null)
      return schemaError('pssz IR: malformed lengthBound attribute');
    bound[key] = n;
  }
  return bound;
}
export function parseCap(attributes: IrAttribute[]): bigint | null {
  const attribute = findAttribute(attributes, 'maxDynamicData');
  if (!attribute) return null;
  const n = attribute.value === null ? null : parseU64(attribute.value);
  if (n === null) return schemaError('pssz IR: malformed maxDynamicData attribute');
  return n;
}
/** `maxFields = "N"`: the most fields a record / parameters a function will ever have; N <= 255. */
export function parseMaxFields(attributes: IrAttribute[]): number | null {
  const attribute = findAttribute(attributes, 'maxFields');
  if (!attribute) return null;
  const n = attribute.value === null ? null : parseU64(attribute.value);
  if (n === null || n > 255n) return schemaError('pssz IR: malformed maxFields attribute');
  return Number(n);
}
function intForm(attributes: IrAttribute[], name: string): IntForm {
  const a = findAttribute(attributes, name);
  if (a && a.value !== null) return a.value === 'string' ? 'string' : 'number';
  return 'natural';
}

type Site = 'record' | 'record_member' | 'variant' | 'variant_member';

// `caps`: the automatic profile, which also takes maxFields; an explicit
// profile rejects that constraint, as the typed psio::pssz_<W> codecs do.
export function checkAttributes(values: IrAttribute[], site: Site, caps: boolean): void {
  for (const { name, value } of values) {
    if (name === 'doc' || name === 'description' || name === 'visible') continue;
    if (site === 'record' && name === 'definitionWillNotChange') continue;
    if (site === 'record' && (name === 'maxDynamicData' || (caps && name === 'maxFields'))) continue;
    // An argument list: the same bytes as any record; its text form differs.
    if (site === 'record' && name === 'tuple') {
      if (value !== 'named' && value !== 'positional') schemaError('pssz IR: malformed tuple attribute');
      continue;
    }
    if (site === 'record_member' && (name === 'lengthBound' || name === 'jsonInt' || name === 'ijsonInt')) {
      if (name !== 'lengthBound' && value !== 'number' && value !== 'string')
        schemaError('pssz IR: malformed integer form attribute');
      continue;
    }
    // An untagged variant: the same bytes; in text the bare alternative.
    if (site === 'variant' && name === 'untagged') continue;
    schemaError('pssz IR: unsupported schema constraint or annotation');
  }
}

/** Per-call decode state. */
export interface DecodeContext {
  allowUnknown: boolean;
  hasUnknown: boolean;
  /** A "json" node decodes to its stored text (a `PsszJsonText`), for the text walk. */
  jsonText?: boolean;
}

function bytesError(message: string): never {
  throw new PsszError('bytes', message);
}
function valueError(message: string): never {
  throw new PsszError('value', message);
}

function widthClass(bound: bigint | null): number {
  return bound !== null && bound <= 0xffn ? 1 : bound !== null && bound <= 0xffffn ? 2 : 4;
}
function boundAdd(a: bigint | null, b: bigint | null): bigint | null {
  if (a === null || b === null) return null;
  const sum = a + b;
  return sum > U64_MAX ? null : sum;
}
function boundMultiply(count: bigint, value: bigint | null): bigint | null {
  if (count === 0n) return 0n;
  if (value === null) return null;
  const product = count * value;
  return product > U64_MAX ? null : product;
}

// max_size.hpp fit(): the bound of a type that writes `slots` offset-width
// words (header, slots, table entries) and whose other bytes are bounded by
// `rest`, for the least width in {1, 2, 4} whose total fits it. The width
// class of the result is that width (docs/pssz-spec.md 3.2).
function fit(slots: bigint, rest: bigint | null): bigint | null {
  if (rest === null) return null;
  if (slots + rest <= 0xffn) return slots + rest;
  if (2n * slots + rest <= 0xffffn) return 2n * slots + rest;
  const out = 4n * slots + rest;
  return out > U64_MAX ? null : out;
}

function packedByteCount(count: number): number {
  return Math.floor(count / 8) + (count % 8 !== 0 ? 1 : 0);
}

const QUIET_NAN_32 = 0x7fc00000;
const QUIET_NAN_64 = 0x7ff8000000000000n;

/**
 * A NaN other than the quiet NaN (`0x7fc00000` / `0x7ff8000000000000`).
 * PSSZ keeps NaN payloads bit for bit and a JavaScript number cannot, so a
 * decoded float is a plain number unless it is one of these; `encode`
 * writes the bits back. `Number(x)` is NaN.
 */
export class PsszNaN {
  constructor(
    /** The IEEE 754 bit pattern. */
    readonly bits: bigint,
    /** Byte width of the float: 4 or 8. */
    readonly size: 4 | 8,
  ) {}
  valueOf(): number {
    return NaN;
  }
}

/**
 * The float −0 as an element of a set or a key of a map. A JavaScript `Set`
 * / `Map` cannot hold −0 (it stores +0), and PSSZ keeps the sign bit, so a
 * decoded set element or map key that is exactly the float −0 is
 * `PsszNegativeZero.value`; anywhere else −0 is the plain number. Every
 * encoder accepts it wherever a float is expected. `Number(x)` is −0.
 */
export class PsszNegativeZero {
  static readonly value = new PsszNegativeZero();
  private constructor() {}
  valueOf(): number {
    return -0;
  }
}

/** Some(x) of an optional whose payload is itself optional. */
export interface Some<T = unknown> {
  some: T;
}
export interface VariantValue<T = unknown> {
  type: string;
  value: T;
}
export interface BitsValue {
  /** Packed bits, least significant first; unused high bits are zero. */
  bytes: Uint8Array;
  bitCount: number;
}

/** The value of an Option node holding `inner` (see `Some`). */
export function wrapSome(node: Node, inner: unknown): unknown {
  return node.child.kind === 'option' ? { some: inner } : inner;
}
/** Splits an Option value: `[isSome, payload]`. */
export function unwrapOption(node: Node, value: unknown, fail: (m: string) => never): [boolean, unknown] {
  if (value === null || value === undefined) return [false, undefined];
  if (node.child.kind !== 'option') return [true, value];
  if (!isObject(value) || !('some' in value))
    return fail('pssz IR: a nested optional needs null or { some: value }');
  return [true, value.some];
}

/** The length a member `lengthBound` constrains, looking through Option. */
export function memberBoundAdmits(bound: LengthBound, node: Node, value: unknown): boolean {
  while (node.kind === 'option') {
    if (value === null || value === undefined) return true;
    if (node.child.kind === 'option') {
      if (!isObject(value) || !('some' in value)) return true;
      value = value.some;
    }
    node = node.child;
  }
  // A nested member's bound counts the bytes of its encoding: checked where
  // the bytes exist (`nestedBoundAdmits`), not on the inner value.
  if (node.kind === 'nested') return true;
  // A json value: the bytes of its text.
  if (node.kind === 'string' && node.json) {
    try {
      return boundAdmits(bound, utf8Length(jsonValueText(value, valueError)));
    } catch {
      return true; // (not a JSON value: the encoder says so)
    }
  }
  if (typeof value === 'string') return boundAdmits(bound, utf8Length(value));
  if (value instanceof Uint8Array) return boundAdmits(bound, value.length);
  if (Array.isArray(value)) return boundAdmits(bound, value.length);
  if (value instanceof Map || value instanceof Set) return boundAdmits(bound, value.size);
  return true;
}

/** The count a member `lengthBound` gives the list it holds: exact, else max. */
export function boundCount(bound: LengthBound | null): bigint | null {
  return bound === null ? null : bound.exact ?? bound.max;
}

/** Whether a member type is a nested value, looking through Option. */
export function nestedMember(node: Node): boolean {
  for (let hops = 0; node.kind === 'option' && hops <= MAX_DEPTH; ++hops) node = node.child;
  return node.kind === 'nested';
}

/**
 * The entries of a map / the elements of a set value, as an array: a `Map`,
 * a `Set`, an array or any other iterable (iterated once). Null: not one.
 */
export function assocItems(value: unknown): unknown[] | null {
  if (Array.isArray(value)) return value;
  if (value === null || typeof value !== 'object' || value instanceof Uint8Array) return null;
  if (typeof (value as Iterable<unknown>)[Symbol.iterator] !== 'function') return null;
  return Array.from(value as Iterable<unknown>);
}

/**
 * A member value about to be checked against its length bound and written:
 * a map / set given as an iterable that cannot be counted without being
 * consumed (not an array, `Map` or `Set`) is turned into an array, once.
 */
export function settleMember(node: Node, value: unknown): unknown {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) return value;
  if (value instanceof Map || value instanceof Set || value instanceof Uint8Array) return value;
  if (node.kind === 'option') {
    if (node.child.kind !== 'option') return settleMember(node.child, value);
    if (!('some' in value)) return value;
    return { some: settleMember(node.child, (value as Some).some) };
  }
  if (node.kind === 'list' && node.assoc) return assocItems(value) ?? value;
  return value;
}

/** Bytewise (unsigned, lexicographic) order; a prefix sorts first. */
export function compareBytes(a: Uint8Array, b: Uint8Array): number {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; ++i) if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
  return a.length - b.length;
}

/** What a codec does with the bytes of a Nested node. */
export interface CodecOptions {
  /** Every Nested node is read and written as its bytes, unexamined. */
  opaqueNested?: boolean;
  /** Named types whose Nested node is opaque (typed deferred members). */
  deferred?: ReadonlySet<string>;
  /** The automatic-profile codec of the same schema (explicit profiles). */
  auto?: Codec;
  /** Typed-only order constraints (sorted / unique / uniqueKeys), by named type and member. */
  orders?: OrderTable;
}

/**
 * The member values of a record value: an object keyed by member name, or —
 * a positional argument list — the array of its arguments. Returns the
 * lookup of member `i` (undefined: not given).
 */
export function recordFields(
  node: Node, value: unknown, fail: (m: string) => never, strict = true,
): (member: MemberInfo, i: number) => unknown {
  if (node.tuple === 'positional') {
    if (!Array.isArray(value)) return fail('pssz IR: a positional argument list is an array');
    if (value.length > node.members.length) fail('pssz IR: more arguments than parameters');
    return (_member, i) => value[i];
  }
  if (!isObject(value) || value instanceof Uint8Array || value instanceof Map || value instanceof Set)
    return fail('pssz IR: record value required');
  if (strict)
    for (const key of Object.keys(value))
      if (value[key] !== undefined && !node.byName!.has(key)) fail(`pssz IR: unknown record value "${key}"`);
  return (member) => (Object.prototype.hasOwnProperty.call(value, member.name) ? value[member.name] : undefined);
}

export function setField(target: Record<string, unknown>, name: string, value: unknown): void {
  if (name === '__proto__')
    Object.defineProperty(target, name, { value, writable: true, enumerable: true, configurable: true });
  else target[name] = value;
}

/**
 * The codec for one schema under one width profile. `W` = 1, 2 or 4: the
 * explicit profile (one inherited width). `W` = 0: the automatic profile
 * (every record, array and list uses its own type-derived width).
 */
export class Codec {
  private readonly named = new Map<string, IrMember | null>();
  private readonly nodes = new Map<IrAnyType, Node>();
  /**
   * The codec of the bytes inside a Nested node: always the automatic
   * profile, with the same handlers and options.
   */
  readonly auto: Codec;
  /** Every Nested node is opaque. */
  readonly opaqueNested: boolean;
  private readonly deferred: ReadonlySet<string>;
  private readonly orders: OrderTable;
  /** A handler has a `check`, or every custom id needs a handler. */
  readonly checksValues: boolean;
  // Scoped switches, shared with `auto`: `quiet` > 0 runs no handler (the
  // structure only); `opaque` > 0 treats every Nested node as opaque.
  private readonly state: { quiet: number; opaque: number };

  constructor(
    readonly schema: IrSchema,
    readonly W: 0 | 1 | 2 | 4,
    /** Custom id -> handler, bound to the nodes as they are built. */
    readonly handlers: ReadonlyMap<string, PsszCustomHandler> = new Map(),
    /** A custom id with no handler is an error, not its wire type. */
    readonly requireHandlers = false,
    options: CodecOptions = {},
  ) {
    for (const member of schema.types)
      this.named.set(member.name, this.named.has(member.name) ? null : member);
    this.opaqueNested = options.opaqueNested ?? false;
    this.deferred = options.deferred ?? new Set();
    this.orders = options.orders ?? new Map();
    this.auto = W === 0 ? this : options.auto ?? new Codec(schema, 0, handlers, requireHandlers, options);
    this.state = W === 0 ? { quiet: 0, opaque: 0 } : this.auto.state;
    let checks = requireHandlers;
    for (const handler of handlers.values()) checks = checks || typeof handler.check === 'function';
    this.checksValues = checks;
  }

  /** Runs `run` with no handler consulted: the structure only. */
  quietly<R>(run: () => R): R {
    ++this.state.quiet;
    try {
      return run();
    } finally {
      --this.state.quiet;
    }
  }
  // Runs `run` with no handler consulted and every Nested node opaque.
  private rawly<R>(run: () => R): R {
    ++this.state.quiet;
    ++this.state.opaque;
    try {
      return run();
    } finally {
      --this.state.quiet;
      --this.state.opaque;
    }
  }
  /** Whether the bytes of the Nested node are not looked at. */
  isOpaque(node: Node): boolean {
    return node.opaque || this.opaqueNested || this.state.opaque > 0;
  }

  private get automatic(): boolean {
    return this.W === 0;
  }

  // `input` with named aliases followed, and nothing else. `via`: the last
  // name on the way.
  private resolve(input: IrAnyType, via?: { name?: string }): IrAnyType {
    let out = input;
    for (let hops = 0; hops <= this.schema.types.length; ++hops) {
      if (out.value.type !== 'Type') return out;
      const found = this.named.get(out.value.value.type);
      if (found === null) schemaError('pssz IR: duplicate named type');
      if (found === undefined) return schemaError(`pssz IR: unresolved named type "${out.value.value.type}"`);
      if (via) via.name = found!.name;
      out = found!.type;
    }
    return schemaError('pssz IR: cyclic named aliases');
  }

  // The type an adapter (a Custom id with a binary meaning, a time id) or an
  // Enum sits on: found through named aliases only. A wrapper in that place
  // (an Enum, a "box", a text presentation) is a malformed schema in every
  // language, not something to look through.
  private direct(input: IrAnyType): IrTypeValue {
    const t = this.resolve(input).value;
    if (t.type === 'Enum' || (t.type === 'Custom' && !WIRE_CUSTOM_IDS.has(t.value.id)))
      return schemaError('pssz IR: an adapter or enum must sit directly on its wire type');
    return t;
  }

  /** `ref` on the wire, and whether it is a boxed value. */
  wireOf(ref: Node): { node: Node; boxed: boolean } {
    return strip(ref);
  }

  /**
   * Builds (once) the node for `root` and everything it reaches, applying
   * the checks of `Codec::metadata`. A failure leaves no partial nodes.
   */
  node(root: IrAnyType): Node {
    const added: IrAnyType[] = [];
    try {
      const out = this.build(root, added, 0);
      // The key type of a map / set has a key encoding: checked once the
      // graph is complete (a key type may be recursive).
      for (const key of added) {
        const node = this.nodes.get(key)!;
        if (node.assoc) this.hasKey(node.keyRef, []);
      }
      return out;
    } catch (e) {
      for (const key of added) this.nodes.delete(key);
      if (e instanceof RangeError) schemaError('pssz IR: schema nesting too deep');
      throw e;
    }
  }

  // `owner`: the named type a Custom written in place belongs to.
  private build(input: IrAnyType, added: IrAnyType[], depth: number, owner?: string): Node {
    if (depth > 512) schemaError('pssz IR: schema nesting too deep');
    const via: { name?: string } = {};
    const type = this.resolve(input, via);
    const named = via.name ?? owner;
    const known = this.nodes.get(type);
    if (known) return known;
    const node = new Node();
    this.nodes.set(type, node);
    added.push(type);
    const d = depth + 1;
    const t = type.value;
    switch (t.type) {
      case 'Int': {
        const { bits, isSigned } = t.value;
        node.kind = 'int';
        node.bits = bits;
        node.signed = isSigned;
        if (bits === 1) node.size = 1;
        else if (bits === 8 || bits === 16 || bits === 32 || bits === 64 || bits === 128) node.size = bits / 8;
        else if (bits === 256 && !isSigned) node.size = 32;
        else schemaError('pssz IR: unsupported integer width');
        break;
      }
      case 'Enum': {
        // On the wire an Enum is exactly its underlying integer.
        let under: IrTypeValue | undefined;
        try {
          under = this.direct(t.value.type);
        } catch (e) {
          // An enum over itself, or over a wrapper, has no integer under it.
          if (!(e instanceof PsszError) || e.kind !== 'schema') throw e;
        }
        // Any integer the codec has; text needs 8 to 64 bits.
        if (under?.type !== 'Int') return schemaError('pssz IR: an enum needs an underlying integer');
        const integer = strip(this.build(t.value.type, added, d)).node;
        node.kind = 'int';
        node.bits = integer.bits;
        node.signed = integer.signed;
        node.size = integer.size;
        node.enumerators = t.value.members;
        break;
      }
      case 'Float': {
        const { exp, mantissa } = t.value;
        node.kind = 'float';
        if (exp === 8 && (mantissa === 23 || mantissa === 24)) node.size = 4;
        else if (exp === 11 && (mantissa === 52 || mantissa === 53)) node.size = 8;
        else schemaError('pssz IR: unsupported IEEE float width');
        break;
      }
      case 'Option':
        node.kind = 'option';
        node.textChild = this.build(t.value.type, added, d);
        break;
      case 'List':
        node.kind = 'list';
        node.textChild = this.build(t.value.type, added, d);
        break;
      case 'BoundedList':
      case 'Array':
        node.kind = t.type === 'Array' ? 'array' : 'list';
        node.bounded = true;
        node.exact = t.type === 'Array';
        node.count = t.type === 'Array' ? t.value.len : t.value.maxCount;
        node.countNum = node.count > BigInt(Number.MAX_SAFE_INTEGER) ? Infinity : Number(node.count);
        node.textChild = this.build(t.value.type, added, d);
        break;
      case 'Tuple':
        // docs/pssz-spec.md 8.4: a tuple has at most 255 elements.
        if (t.value.members.length > 255) schemaError('pssz IR: a tuple has more than 255 elements');
        node.kind = 'tuple';
        node.members = t.value.members.map(
          (m, i) => new MemberInfo(String(i), this.build(m, added, d), null, 'natural', 'natural'),
        );
        break;
      case 'Struct':
      case 'Object':
      case 'Variant': {
        const record = t.type !== 'Variant';
        node.kind = record ? 'record' : 'variant';
        checkAttributes(t.value.attributes, record ? 'record' : 'variant', this.automatic);
        const orders = record && named !== undefined ? this.orders.get(named) : undefined;
        if (record) {
          node.stable =
            t.type === 'Struct' || t.value.attributes.some((a) => a.name === 'definitionWillNotChange');
          node.cap = parseCap(t.value.attributes);
          const form = findAttribute(t.value.attributes, 'tuple');
          if (form) node.tuple = form.value as 'named' | 'positional';
          // docs/pssz-spec.md 8.4, 16.
          if (t.value.members.length > 255) schemaError('pssz IR: a record has more than 255 fields');
          const maxFields = parseMaxFields(t.value.attributes);
          node.maxFields = maxFields;
          if (maxFields !== null && t.value.members.length > maxFields)
            schemaError('pssz IR: a record has more fields than its maxFields');
        } else {
          if (t.value.members.length === 0 || t.value.members.length > 256)
            schemaError('pssz IR: invalid variant schema');
          node.untagged = findAttribute(t.value.attributes, 'untagged') !== undefined;
        }
        const seen = new Set<string>();
        for (const member of t.value.members) {
          if (seen.has(member.name)) schemaError('pssz IR: duplicate schema member');
          seen.add(member.name);
          checkAttributes(member.attributes, record ? 'record_member' : 'variant_member', this.automatic);
          const info = new MemberInfo(
            member.name,
            this.build(member.type, added, d),
            record ? parseLengthBound(member.attributes) : null,
            record ? intForm(member.attributes, 'jsonInt') : 'natural',
            record ? intForm(member.attributes, 'ijsonInt') : 'natural',
            orders?.get(member.name) ?? null,
          );
          node.members.push(info);
        }
        if (orders) for (const member of node.members) if (member.order) this.orderFits(member.text, member.order);
        node.byName = new Map(node.members.map((m, i) => [m.name, i]));
        break;
      }
      case 'Custom': {
        const id = t.value.id;
        const time = parseTimeSchemaId(id);
        if (time) {
          // A built-in presentation: a duration / time point / monotonic
          // time point is its tick integer on the wire, bigint ticks as a
          // value, and has the text form of docs/time.md (RFC 3339 for a
          // time point, the tick count for the other two).
          const ticks = this.direct(t.value.type);
          const bits = ticks.type === 'Int' ? ticks.value.bits : 0;
          if (ticks.type !== 'Int' || (bits !== 8 && bits !== 16 && bits !== 32 && bits !== 64))
            return schemaError('pssz IR: time value requires an 8/16/32/64-bit tick integer');
          node.kind = 'int';
          node.bits = bits;
          node.signed = ticks.value.isSigned;
          node.size = bits / 8;
          node.time = time;
          node.id = id;
          node.timeDigits = time.point ? timeFractionDigits(time.num, time.den) : -1;
          break;
        }
        if (reservedTimeId(id)) return schemaError('pssz IR: unsupported custom adapter');
        if (id === 'map' || id === 'set') {
          // A list whose entries are in strictly ascending key order. It
          // sits directly on its list (named aliases only), and a map's
          // element is (an alias of) a tuple of exactly two members.
          const map = id === 'map';
          const shape = map
            ? 'pssz IR: a map must sit directly on a list of two-member tuples'
            : 'pssz IR: a set must sit directly on a list';
          const list = this.direct(t.value.type);
          if (list.type !== 'List' && list.type !== 'BoundedList') return schemaError(shape);
          if (map) {
            const entry = this.resolve(list.value.type).value;
            if (entry.type !== 'Tuple' || entry.value.members.length !== 2) return schemaError(shape);
          }
          node.kind = 'list';
          node.assoc = id;
          if (list.type === 'BoundedList') {
            node.bounded = true;
            node.count = list.value.maxCount;
            node.countNum = node.count > BigInt(Number.MAX_SAFE_INTEGER) ? Infinity : Number(node.count);
          }
          node.textChild = this.build(list.value.type, added, d);
          break;
        }
        if (id === 'json') {
          // Any JSON value: a UTF-8 string holding its compact text. It
          // sits directly on that string (named aliases only).
          const text = this.resolve(t.value.type).value;
          if (text.type !== 'Custom' || text.value.id !== 'utf8')
            return schemaError('pssz IR: a json value must sit directly on a UTF-8 string');
          const string = this.build(t.value.type, added, d);
          node.kind = 'string';
          node.json = true;
          node.id = id;
          node.textChild = string.textChild;
          break;
        }
        if (!WIRE_CUSTOM_IDS.has(id)) {
          // "box" or a custom type: the inner type on the wire.
          node.kind = 'custom';
          node.id = id;
          node.boxed = id === 'box';
          if (!node.boxed) node.handler = this.handlers.get(id);
          node.inner = this.build(t.value.type, added, d, named);
          break;
        }
        const inner = this.direct(t.value.type);
        const unsignedInt = (x: IrTypeValue, bits: number) =>
          x.type === 'Int' && x.value.bits === bits && !x.value.isSigned;
        if (id === 'bool' || id === 'char') {
          if (!unsignedInt(inner, id === 'bool' ? 1 : 8))
            schemaError('pssz IR: primitive adapter has incompatible inner schema');
          node.kind = id;
          node.size = 1;
        } else if (id === 'bitvector' || id === 'bitlist') {
          let element: IrAnyType;
          let bound: bigint;
          if (id === 'bitvector') {
            if (inner.type !== 'Array') return schemaError('pssz IR: bitvector requires array bit metadata');
            element = inner.value.type;
            bound = inner.value.len;
          } else {
            if (inner.type !== 'BoundedList')
              return schemaError('pssz IR: bitlist requires bounded bit metadata');
            element = inner.value.type;
            bound = inner.value.maxCount;
          }
          if (!unsignedInt(this.direct(element), 1))
            schemaError('pssz IR: packed bit element must be unsigned one-bit integer');
          if (bound > BigInt(Number.MAX_SAFE_INTEGER))
            schemaError('pssz IR: bit count exceeds platform capacity');
          node.kind = id;
          node.size = Number(bound);
        } else if (id === 'string' || id === 'utf8' || id === 'hex') {
          if (inner.type !== 'List' && inner.type !== 'BoundedList' && inner.type !== 'Array')
            return schemaError('pssz IR: byte adapter requires byte container');
          const element = this.direct(inner.value.type);
          if (element.type === 'Custom' && element.value.id === 'char') {
            if (!unsignedInt(this.direct(element.value.type), 8))
              schemaError('pssz IR: primitive adapter has incompatible inner schema');
          } else if (!unsignedInt(element, 8)) schemaError('pssz IR: byte adapter requires unsigned bytes');
          // "string" is raw bytes; "utf8" is validated on every read and write.
          node.kind = id === 'hex' ? 'hex' : 'string';
          node.raw = id === 'string';
        } else schemaError('pssz IR: unsupported custom adapter');
        node.textChild = this.build(t.value.type, added, d);
        break;
      }
      case 'Nested':
        // A byte list holding the encoding of the inner type, which is
        // always in the automatic profile: its nodes are that codec's.
        node.kind = 'nested';
        node.opaque = via.name !== undefined && this.deferred.has(via.name);
        node.textChild = this.automatic ? this.build(t.value.type, added, d) : this.auto.node(t.value.type);
        break;
      default:
        return schemaError('pssz IR: unsupported schema type');
    }
    return node;
  }

  // Whether `ref` has a `key` encoding (docs/key-spec.md): the key type of
  // a map / the element type of a set. A schema error otherwise.
  private hasKey(ref: Node, active: Node[]): void {
    const none = 'pssz IR: the key type of a map / set has no key encoding';
    const { node, boxed } = strip(ref);
    if (boxed) schemaError(none);
    if (active.includes(node)) return;
    if (active.length > MAX_DEPTH) schemaError('pssz IR: node depth limit exceeded');
    active.push(node);
    switch (node.kind) {
      case 'int':
        if (node.bits === 1) schemaError(none);
        break;
      case 'bitlist':
        schemaError(none);
        break;
      case 'option':
      case 'array':
        this.hasKey(node.textChild, active);
        break;
      case 'list':
        // A map / set as (part of) a key: its own key type is checked as
        // that node's; here, what it maps to.
        if (!node.assoc) this.hasKey(node.textChild, active);
        else if (node.assoc === 'map') this.hasKey(node.valueRef, active);
        break;
      case 'tuple':
      case 'record':
      case 'variant':
        for (const member of node.members) this.hasKey(member.text, active);
        break;
      default:
        break; // float, bool, char, string, hex, bitvector; nested: the key of a byte list
    }
    active.pop();
  }

  /** Whether a Nested node occurs in (a part of) the key type of a map / set. */
  keyHasNested(node: Node): boolean {
    if (node.keyNestedMemo !== undefined) return node.keyNestedMemo;
    const seen = new Set<Node>();
    const walk = (ref: Node): boolean => {
      const at = strip(ref).node;
      if (seen.has(at)) return false;
      seen.add(at);
      switch (at.kind) {
        case 'nested':
          return true;
        case 'option':
        case 'list':
        case 'array':
          return walk(at.textChild);
        case 'tuple':
        case 'record':
        case 'variant':
          return at.members.some((member) => walk(member.text));
        default:
          return false;
      }
    };
    return (node.keyNestedMemo = walk(node.keyRef));
  }

  // ── Layout ────────────────────────────────────────────────────────────────

  // C++ `layout` / `can_be_empty` recurse over the schema with a depth
  // counter; the result here is memoized together with the recursion it
  // needs, so a schema too deep for C++ fails here as well, and a schema
  // that recurses forever through fixed layouts is an error, not a hang.
  private memoized<T>(
    node: Node,
    slot: 'layoutMemo' | 'emptyMemo',
    compute: () => { value: T; height: number },
  ): T {
    let memo = node[slot] as Memo<T> | undefined;
    if (!memo) {
      // In progress: reaching this node again is unbounded recursion.
      node[slot] = { error: 'pssz IR: node depth limit exceeded', height: 0 } as Memo<never>;
      try {
        const { value, height } = compute();
        memo = height > MAX_DEPTH
          ? { error: 'pssz IR: node depth limit exceeded', height }
          : { value, height };
      } catch (e) {
        if (!(e instanceof PsszError)) throw e;
        memo = { error: e.message, height: 0 };
      }
      (node[slot] as Memo<T>) = memo;
    }
    if (memo.error !== undefined) schemaError(memo.error);
    return memo.value as T;
  }

  layout(node: Node): Layout {
    return this.memoized(node, 'layoutMemo', () => {
      const child = (n: Node) => {
        const value = this.layout(n);
        return { value, height: n.layoutMemo!.height + 1 };
      };
      switch (node.kind) {
        case 'int':
        case 'float':
        case 'bool':
        case 'char':
          return { value: { fixed: true, size: node.size, optional: false }, height: 0 };
        case 'bitvector':
          return { value: { fixed: true, size: packedByteCount(node.size), optional: false }, height: 0 };
        case 'option':
          return { value: { fixed: false, size: 0, optional: true }, height: 0 };
        case 'array': {
          const { value, height } = child(node.child);
          let size = value.size;
          if (value.fixed) {
            size *= node.countNum;
            if (size > Number.MAX_SAFE_INTEGER) schemaError('pssz IR: array size overflow');
            if (Number.isNaN(size)) size = 0; // 0 * Infinity: zero-size elements
          }
          return { value: { fixed: value.fixed, size: value.fixed ? size : 0, optional: false }, height };
        }
        case 'string':
        case 'hex':
          return child(node.child);
        case 'record': {
          if (!node.stable) return { value: { fixed: false, size: 0, optional: false }, height: 0 };
          let fixed = true;
          let size = 0;
          let height = 0;
          for (const member of node.members) {
            const c = child(member.node);
            fixed = fixed && c.value.fixed;
            size += c.value.size;
            height = Math.max(height, c.height);
          }
          if (size > Number.MAX_SAFE_INTEGER) schemaError('pssz IR: size overflow');
          return { value: { fixed, size: fixed ? size : 0, optional: false }, height };
        }
        default:
          return { value: { fixed: false, size: 0, optional: false }, height: 0 };
      }
    });
  }

  /** Whether a value of the type can encode to zero bytes (spec 5.5). */
  canBeEmpty(node: Node): boolean {
    return this.memoized(node, 'emptyMemo', () => {
      switch (node.kind) {
        case 'list':
        case 'nested':
          return { value: true, height: 0 };
        case 'option': {
          const value = !this.canBeEmpty(node.child);
          return { value, height: node.child.emptyMemo!.height + 1 };
        }
        case 'array': {
          if (node.count === 0n) return { value: true, height: 0 };
          const info = this.layout(node.child);
          return { value: info.fixed && info.size === 0, height: node.child.layoutMemo!.height + 1 };
        }
        case 'record': {
          if (!node.stable) return { value: false, height: 0 };
          const info = this.layout(node);
          return { value: info.fixed && info.size === 0, height: node.layoutMemo!.height };
        }
        case 'bitvector':
          return { value: node.size === 0, height: 0 };
        case 'string':
        case 'hex': {
          const value = this.canBeEmpty(node.child);
          return { value, height: node.child.emptyMemo!.height + 1 };
        }
        default:
          return { value: false, height: 0 };
      }
    });
  }

  // ── Width derivation (max_size.hpp bookkeeping) ───────────────────────────

  private fixedPayload(node: Node, active: Node[]): boolean {
    if (active.includes(node)) return false;
    if (node.fixedPayloadMemo !== undefined) return node.fixedPayloadMemo;
    active.push(node);
    let out = false;
    switch (node.kind) {
      case 'int':
      case 'float':
      case 'bool':
      case 'char':
      case 'bitvector': // ceil(N / 8) bytes inline: no offset slot
        out = true;
        break;
      case 'array':
        out = this.fixedPayload(node.child, active);
        break;
      case 'string':
      case 'hex':
        if (node.child.kind === 'array') out = this.fixedPayload(node.child, active);
        break;
      case 'record':
        if (node.stable) {
          out = true;
          for (const member of node.members) out = this.fixedPayload(member.node, active) && out;
        }
        break;
      default:
        break;
    }
    active.pop();
    if (active.length === 0) node.fixedPayloadMemo = out;
    return out;
  }

  private boundOf(node: Node, annotation: LengthBound | null, reserveCap: boolean, active: Node[]): bigint | null {
    if (active.includes(node)) return null;
    if (reserveCap && node.cap !== null) return node.cap;
    const slot = reserveCap ? 1 : 0;
    if (annotation === null && node.boundMemo[slot] !== undefined) return node.boundMemo[slot]!;
    active.push(node);
    // N elements, each its reserved payload, and the offset table (at the
    // container's own width) when they are not inline fixed payloads.
    const elements = (child: Node, boxed: boolean, count: bigint): bigint | null => {
      if (count === 0n) return 0n;
      // A boxed value has no inferred bound (the typed codecs give it none).
      const element = boxed ? null : this.boundOf(child, null, true, active);
      const total = boundMultiply(count, element);
      return this.fixedPayload(child, []) ? total : fit(count, total);
    };
    const sequence = (container: Node): bigint | null => {
      if (container.kind === 'list' && !container.bounded) {
        const count = annotation ? annotation.exact ?? annotation.max : null;
        return count !== null ? elements(container.child, container.childBoxed, count) : null;
      }
      return elements(container.child, container.childBoxed, container.count);
    };
    let out: bigint | null = null;
    switch (node.kind) {
      case 'int':
      case 'float':
      case 'bool':
      case 'char':
        out = BigInt(node.size);
        break;
      case 'list':
      case 'array':
        out = sequence(node);
        break;
      case 'option':
        // One selector byte is reserved for every optional.
        out = node.childBoxed ? null : boundAdd(this.boundOf(node.child, annotation, true, active), 1n);
        break;
      case 'string':
      case 'hex':
        out = node.childBoxed ? null : sequence(node.child);
        break;
      case 'record': {
        // The header (extensible records) and one slot per variable member,
        // at the record's own width.
        let slots = node.stable ? 0n : 1n;
        let total: bigint | null = 0n;
        for (const member of node.members) {
          const payload = member.boxed ? null : this.boundOf(member.node, member.bound, true, active);
          if (!this.fixedPayload(member.node, [])) ++slots;
          total = boundAdd(total, payload);
        }
        out = fit(slots, total);
        break;
      }
      case 'nested':
        // No bound of its own; a member lengthBound bounds its bytes.
        out = annotation ? annotation.exact ?? annotation.max : null;
        break;
      case 'tuple': {
        // The extensible positional record of its elements (an element has
        // no member annotation): the header, then per element its reserved
        // payload and, when it is not an inline fixed payload, a slot.
        let slots = 1n;
        let total: bigint | null = 0n;
        for (const member of node.members) {
          const payload = member.boxed ? null : this.boundOf(member.node, null, true, active);
          if (!this.fixedPayload(member.node, [])) ++slots;
          total = boundAdd(total, payload);
        }
        out = fit(slots, total);
        break;
      }
      case 'variant': {
        // One selector byte, then the widest alternative's payload;
        // unbounded when any alternative is.
        let widest: bigint | null = 0n;
        for (const member of node.members) {
          const payload = member.boxed ? null : this.boundOf(member.node, null, true, active);
          if (payload === null || widest === null) widest = null;
          else if (payload > widest) widest = payload;
        }
        out = boundAdd(widest, 1n);
        break;
      }
      case 'bitvector':
      case 'bitlist':
        // ceil(N / 8) bytes; a bitlist one more for its delimiter bit.
        out = BigInt(packedByteCount(node.size) + (node.kind === 'bitlist' ? 1 : 0));
        break;
      default:
        break;
    }
    active.pop();
    if (annotation === null) node.boundMemo[slot] = out;
    return out;
  }

  /**
   * The maximum encoded size (docs/pssz-spec.md 3.7) of a value of `node`
   * at a position whose cap is `cap`, or null: no finite maximum. The walk
   * of `boundOf` with three differences: a type's header, slots and offset
   * table are counted at the width it really has (`widthOf`); what it holds
   * counts as its own maximum; and a capped record is the smaller of its cap
   * and that sum, the cap when something inside is unbounded. The two
   * conservatisms of the inferred bound stay: one selector byte for every
   * optional, ceil(N / 8) + 1 for a bitlist.
   */
  private maxOf(node: Node, annotation: LengthBound | null, cap: bigint | null, active: Node[]): bigint | null {
    if (active.includes(node)) return null;
    active.push(node);
    const elements = (container: Node, count: bigint, own: bigint | null): bigint | null => {
      if (count === 0n) return 0n;
      const element = container.childBoxed ? null : this.maxOf(container.child, null, cap, active);
      const total = boundMultiply(count, element);
      if (this.fixedPayload(container.child, [])) return total;
      return boundAdd(total, count * BigInt(this.widthOf(container, cap, own)));
    };
    const sequence = (container: Node): bigint | null => {
      if (container.kind === 'list' && !container.bounded) {
        const count = annotation ? annotation.exact ?? annotation.max : null;
        return count !== null ? elements(container, count, count) : null;
      }
      return elements(container, container.count, null);
    };
    // A record's or tuple's header and slots at `width`, and its members.
    const members = (slots: bigint, width: number, beneath: bigint | null, annotated: boolean): bigint | null => {
      let total: bigint | null = 0n;
      for (const member of node.members) {
        const payload = member.boxed ? null : this.maxOf(member.node, annotated ? member.bound : null, beneath, active);
        if (!this.fixedPayload(member.node, [])) ++slots;
        total = boundAdd(total, payload);
      }
      return boundAdd(total, slots * BigInt(width));
    };
    let out: bigint | null = null;
    switch (node.kind) {
      case 'int':
      case 'float':
      case 'bool':
      case 'char':
        out = BigInt(node.size);
        break;
      case 'list':
      case 'array':
        out = sequence(node);
        break;
      case 'option':
        out = node.childBoxed ? null : boundAdd(this.maxOf(node.child, annotation, cap, active), 1n);
        break;
      case 'string':
      case 'hex':
        out = node.childBoxed ? null : sequence(node.child);
        break;
      case 'record':
        // A record is a boundary for the position cap: its members are
        // beneath its own cap, or beneath none.
        out = members(node.stable ? 0n : 1n, this.widthOf(node), this.capBeneath(node, cap), true);
        if (node.cap !== null && (out === null || out > node.cap)) out = node.cap;
        break;
      case 'nested':
        out = annotation ? annotation.exact ?? annotation.max : null;
        break;
      case 'tuple':
        out = members(1n, this.widthOf(node, cap), cap, false);
        break;
      case 'variant': {
        let widest: bigint | null = 0n;
        for (const member of node.members) {
          const payload = member.boxed ? null : this.maxOf(member.node, null, cap, active);
          if (payload === null || widest === null) widest = null;
          else if (payload > widest) widest = payload;
        }
        out = boundAdd(widest, 1n);
        break;
      }
      case 'bitvector':
      case 'bitlist':
        out = BigInt(packedByteCount(node.size) + (node.kind === 'bitlist' ? 1 : 0));
        break;
      default:
        break;
    }
    active.pop();
    return out;
  }

  /**
   * `max_encoded_size` of the type (docs/pssz-spec.md 3.7): no encoding of
   * a value of it in the automatic profile is longer; null when there is no
   * finite maximum. `count` as for `inferredBound`.
   */
  maxEncodedSize(node: Node, count: bigint | null = null): bigint | null {
    if (count !== null && !(node.kind === 'list' && !node.bounded)) count = null;
    if (count === null) {
      if (node.maxSize === undefined) node.maxSize = this.maxOf(node, null, null, []);
      return node.maxSize;
    }
    return this.maxOf(node, { exact: null, max: count, min: null }, null, []);
  }

  /**
   * `inferred_bound` of the type itself (docs/pssz-spec.md 3.2; its explicit
   * cap is not looked at), or null: unbounded. `count`: for a plain list (or
   * the list of a map / set), the count the `lengthBound` of the member
   * holding it gives.
   */
  inferredBound(node: Node, count: bigint | null = null): bigint | null {
    if (count !== null && !(node.kind === 'list' && !node.bounded)) count = null;
    if (count === null) {
      if (!node.ownBoundDone) {
        node.ownBound = this.boundOf(node, null, false, []);
        node.ownBoundDone = true;
      }
      return node.ownBound!;
    }
    const memo = (node.countBound ??= new Map());
    let out = memo.get(count);
    if (out === undefined) {
      out = this.boundOf(node, { exact: null, max: count, min: null }, false, []);
      memo.set(count, out);
    }
    return out;
  }

  /**
   * Width of a record's header and slots, or of a list / array / map / set
   * offset table, or of a tuple's header and slots. Explicit profile: W.
   * Automatic: the type's own — the width class of its explicit cap, else of
   * its inferred bound (docs/pssz-spec.md 3.1).
   *
   * `cap`: the position cap (spec 3.6), the explicit cap of the nearest
   * enclosing record. Anything that is not a record is also bounded by it:
   * its width is the class of the smaller of the two. A record's width is
   * its own wherever it appears. `count`: the count (exact, else max) of the
   * `lengthBound` of the member holding a plain list.
   */
  widthOf(node: Node, cap: bigint | null = null, count: bigint | null = null): number {
    if (!this.automatic) return this.W;
    if (node.kind === 'record') {
      if (node.width === undefined)
        node.width = widthClass(node.cap !== null ? node.cap : this.inferredBound(node));
      return node.width;
    }
    let bound = this.inferredBound(node, count);
    if (cap !== null && (bound === null || bound > cap)) bound = cap;
    return widthClass(bound);
  }

  /**
   * The position cap of the members of `node`: a record's explicit cap (or
   * none); `cap`, unchanged, for anything else. Explicit profiles have none.
   */
  capBeneath(node: Node, cap: bigint | null): bigint | null {
    if (!this.automatic) return null;
    return node.kind === 'record' ? node.cap : cap;
  }

  /**
   * C++ `psio::pssz_child_wider_than_cap_v<T>`: the record has an explicit
   * cap and a record wider than the cap's width class lies beneath it, with
   * no other record in between. Such a child keeps its own width (spec 3.6)
   * and spends offset bytes on a capacity it can never use there; the remedy
   * is a cap or member bounds on the child. A query, not an error.
   */
  childWiderThanCap(node: Node): boolean {
    if (!this.automatic || node.kind !== 'record' || node.cap === null) return false;
    const width = this.widthOf(node);
    const seen = new Set<Node>();
    const holds = (at: Node): boolean => {
      if (seen.has(at)) return false;
      seen.add(at);
      switch (at.kind) {
        case 'record':
          return this.widthOf(at) > width;
        case 'option':
        case 'list':
        case 'array':
          return holds(at.child);
        case 'tuple':
        case 'variant':
          return at.members.some((member) => holds(member.node));
        default:
          return false; // scalars, strings, bit types; nested: a root of its own
      }
    };
    return node.members.some((member) => holds(member.node));
  }

  // A record over its explicit cap is not a value of its type. The automatic
  // profile checks every record where it is read or written.
  overCap(node: Node, size: number): boolean {
    return this.automatic && node.cap !== null && BigInt(size) > node.cap;
  }

  /**
   * Explicit profiles, as the typed `psio::pssz_<W>` codecs: the cap of the
   * top-level type bounds the whole encoding; nested caps are not looked up.
   * `fail` is called when `size` bytes are over it.
   */
  checkTopCap(root: Node, size: number, fail: (m: string) => never): void {
    if (!this.automatic && root.cap !== null && BigInt(size) > root.cap)
      fail('pssz IR: encoded size exceeds maxDynamicData cap');
  }
  /**
   * `encodeAt` of a top-level value, with the explicit profiles' cap check.
   * `root` is the type as written: its custom nodes are checked.
   */
  encodeTop(root: Node, value: unknown, count: bigint | null = null): Uint8Array {
    const out = this.encodeAt(root, value, 0, null, count);
    this.checkTopCap(strip(root).node, out.length, valueError);
    return out;
  }
  /** `decodeAt` of a whole top-level frame, with the explicit profiles' cap check. */
  decodeTop(root: Node, bytes: Uint8Array, ctx: DecodeContext, count: bigint | null = null): unknown {
    this.checkTopCap(strip(root).node, bytes.length, bytesError);
    return this.decodeAt(root, bytes, 0, bytes.length, 0, ctx, null, count);
  }

  // ── Custom types: handlers on the binary entry points ─────────────────────

  /**
   * The handler of a custom node on a value of it: a strict registry needs
   * one, and its `check` (the invariant) runs. `value` is the decoded value
   * of the wire type. `fail` gets the invariant's own message.
   */
  checkCustom(node: Node, value: unknown, fail: (message: string, invariant: boolean) => never): void {
    if (node.boxed || this.state.quiet > 0) return;
    const handler = node.handler;
    if (!handler) {
      if (this.requireHandlers) fail('pssz IR: no handler registered for a custom type', false);
      return;
    }
    if (typeof handler.check !== 'function') return;
    let verdict: unknown;
    try {
      verdict = handler.check(value);
    } catch (e) {
      verdict = e instanceof Error && e.message !== '' ? e.message : 'pssz IR: invariant violated';
    }
    if (typeof verdict === 'string') fail(verdict === '' ? 'pssz IR: invariant violated' : verdict, true);
  }

  /**
   * `value` as `decode` gives it: encoded and read back with no handler
   * consulted. Throws when it is not a value of the type.
   */
  canonical(node: Node, value: unknown): unknown {
    return this.quietly(() => {
      const bytes = this.encodeAt(node, value, 0);
      return this.decodeAt(node, bytes, 0, bytes.length, 0, { allowUnknown: false, hasUnknown: false });
    });
  }

  /**
   * Runs the checks of the custom nodes `value` is a value of (it was read
   * by a walk that does not: a key). An error of kind `kind`.
   */
  checkValue(ref: Node, value: unknown, kind: PsszErrorKind): void {
    if (!this.checksValues) return;
    try {
      this.encodeAt(ref, value, 0);
    } catch (e) {
      if (!(e instanceof PsszError) || e.kind === kind) throw e;
      throw new PsszError(kind, e.message, undefined, e.invariant);
    }
  }

  // ── Order constraints (typed-only: sorted / unique / uniqueKeys) ──────────

  // The constraints fit the type they are written for, and every element
  // they compare has a key encoding.
  orderFits(ref: Node, tree: OrderTree, depth = 0): void {
    if (depth > MAX_DEPTH) schemaError('pssz IR: order constraints nested too deep');
    let node = strip(ref).node;
    while (node.kind === 'option') node = node.child;
    const sequence = (node.kind === 'list' || node.kind === 'array') && !node.assoc;
    if (tree.sorted || tree.uniqueKeys) {
      if (!sequence) schemaError('pssz IR: sorted / unique / uniqueKeys constrain a list or an array');
      this.hasKey(node.textChild, []);
    }
    if (tree.element) {
      if (!sequence && node.assoc !== 'set') schemaError('pssz IR: element order constraints need a list, an array or a set');
      this.orderFits(node.textChild, tree.element, depth + 1);
    }
    if (tree.keys || tree.values) {
      if (node.assoc !== 'map') schemaError('pssz IR: keys / values order constraints need a map');
      if (tree.keys) this.orderFits(node.keyRef, tree.keys, depth + 1);
      if (tree.values) this.orderFits(node.valueRef, tree.values, depth + 1);
    }
    if (tree.at) {
      if (node.kind !== 'tuple' && node.kind !== 'variant')
        schemaError('pssz IR: at order constraints need a tuple or a variant');
      for (const [key, child] of tree.at) {
        const index = node.byName?.get(key) ?? (/^(0|[1-9][0-9]*)$/.test(key) ? Number(key) : -1);
        if (index < 0 || index >= node.members.length) schemaError('pssz IR: at order constraint names no position');
        this.orderFits(node.members[index].text, child, depth + 1);
      }
    }
  }

  /**
   * Checks a member's value against the member's order constraints
   * (`docs/pssz-spec.md` §3.5): order and equality are those of the
   * elements' key bytes. Runs no handler. `fail` gets the C++ message.
   */
  checkOrder(member: MemberInfo, value: unknown, fail: (m: string) => never): void {
    if (member.order) this.quietly(() => this.orderAt(member.text, member.order!, value, fail));
  }

  private orderAt(ref: Node, tree: OrderTree, value: unknown, fail: (m: string) => never): void {
    let node = strip(ref).node;
    while (node.kind === 'option') {
      if (value === null || value === undefined) return;
      if (node.child.kind === 'option') {
        if (!isObject(value) || !('some' in value)) return;
        value = value.some;
      }
      node = node.child;
    }
    // (A value of the wrong shape is the writer's to refuse.)
    if (node.kind === 'list' || node.kind === 'array') {
      const items = assocItems(value);
      if (!items) return;
      if (node.assoc === 'map') {
        for (const entry of items) {
          if (!Array.isArray(entry) || entry.length !== 2) return;
          if (tree.keys) this.orderAt(node.keyRef, tree.keys, entry[0], fail);
          if (tree.values) this.orderAt(node.valueRef, tree.values, entry[1], fail);
        }
        return;
      }
      if (tree.element) for (const item of items) this.orderAt(node.textChild, tree.element, item, fail);
      if (node.assoc || items.length < 2) return;
      const element = node.child;
      if (tree.sorted) {
        const { direction, strict } = tree.sorted;
        let previous = encodeKey(this, element, items[0]);
        for (let i = 1; i < items.length; ++i) {
          const current = encodeKey(this, element, items[i]);
          const c = direction * compareBytes(previous, current);
          if (c > 0 || (c === 0 && strict))
            fail(strict ? 'sorted: elements are not in strictly ascending order' : 'sorted: elements are not in order');
          previous = current;
        }
      }
      if (tree.uniqueKeys) {
        const first = element.kind === 'tuple' && element.members.length > 0 ? element.members[0].node : null;
        const keys = items.map((item) => {
          if (!first) return encodeKey(this, element, item);
          if (!Array.isArray(item)) return valueError('pssz IR: tuple value required');
          return encodeKey(this, first, item[0]);
        });
        keys.sort(compareBytes);
        for (let i = 1; i < keys.length; ++i)
          if (compareBytes(keys[i - 1], keys[i]) === 0) fail('unique_keys: two elements have the same key');
      }
      return;
    }
    if (!tree.at) return;
    if (node.kind === 'tuple') {
      if (!Array.isArray(value)) return;
      for (const [key, child] of tree.at) this.orderAt(node.members[Number(key)].text, child, value[Number(key)], fail);
    } else if (node.kind === 'variant') {
      if (!isObject(value) || typeof value.type !== 'string') return;
      const index = node.byName!.get(value.type);
      if (index === undefined) return;
      const child = tree.at.get(value.type) ?? (node.byName!.has(String(index)) ? undefined : tree.at.get(String(index)));
      if (child) this.orderAt(node.members[index].text, child, value.value, fail);
    }
  }

  // ── Maps and sets ─────────────────────────────────────────────────────────

  /**
   * The canonical order of a map / set value's entries: indices into
   * `items`, ascending by the key bytes of their keys. Null: two entries
   * have equal key bytes. Computing the order runs no handler.
   */
  assocOrder(node: Node, items: unknown[]): number[] | null {
    const keys = this.assocKeys(node, items);
    const order = items.map((_, i) => i);
    order.sort((a, b) => compareBytes(keys[a], keys[b]));
    for (let i = 1; i < order.length; ++i)
      if (compareBytes(keys[order[i - 1]], keys[order[i]]) === 0) return null;
    return order;
  }

  // The key bytes of every entry's key, in `items` order.
  private assocKeys(node: Node, items: unknown[]): Uint8Array[] {
    const key = strip(node.keyRef).node;
    return this.quietly(() => items.map((item) => {
      if (node.assoc !== 'map') return encodeKey(this, key, item);
      if (!Array.isArray(item) || item.length !== 2) return valueError('pssz IR: a map entry is a [key, value] pair');
      return encodeKey(this, key, item[0]);
    }));
  }

  private encodeAssoc(
    node: Node, value: unknown, n: number, cap: bigint | null, count: bigint | null,
  ): Uint8Array {
    let items = assocItems(value);
    if (!items) return valueError('pssz IR: sequence value required');
    const order = this.assocOrder(node, items);
    if (!order)
      return valueError(node.assoc === 'map' ? 'pssz IR: map has duplicate keys' : 'pssz IR: set has duplicate elements');
    // A value already in canonical order (anything a decoder or a text
    // reader produced) is encoded as it is.
    if (order.some((index, i) => index !== i)) {
      const given = items;
      items = order.map((index) => given[index]);
    }
    return this.encodeSequence(node, items, n, cap, count);
  }

  private decodeAssoc(
    node: Node, bytes: Uint8Array, start: number, end: number, n: number, ctx: DecodeContext,
    cap: bigint | null, count: bigint | null,
  ): unknown {
    const items = this.decodeSequence(node, bytes, start, end, n, ctx, cap, count);
    // The order is that of the bytes read. A decoded Nested value stands
    // for its canonical encoding, which is not what a compatible read saw
    // when the inner bytes carry unknown fields: keys holding a Nested node
    // are taken from the bytes as they are.
    let keys: Uint8Array[];
    if (this.keyHasNested(node) && !this.opaqueNested && this.state.opaque === 0) {
      keys = this.rawly(() => {
        const raw: DecodeContext = { allowUnknown: ctx.allowUnknown, hasUnknown: false };
        return this.assocKeys(node, this.decodeSequence(node, bytes, start, end, n, raw, cap, count));
      });
    } else keys = this.assocKeys(node, items);
    for (let i = 1; i < keys.length; ++i)
      if (compareBytes(keys[i - 1], keys[i]) >= 0)
        bytesError(node.assoc === 'map'
          ? 'pssz IR: map entries are not in strictly ascending key order'
          : 'pssz IR: set elements are not in strictly ascending order');
    const out = this.assocValue(node, items);
    // Distinct key bytes are distinct JavaScript keys; never a silent loss.
    if (out.size !== items.length)
      bytesError(node.assoc === 'map' ? 'pssz IR: map container merged distinct keys' : 'pssz IR: set container merged distinct elements');
    return out;
  }

  /**
   * The `Map` / `Set` of a map / set node from its entries / elements in
   * canonical order. A float key that is −0 is held as `PsszNegativeZero`
   * (a `Map` / `Set` would store +0 and lose the sign bit).
   */
  assocValue(node: Node, items: unknown[]): Map<unknown, unknown> | Set<unknown> {
    const float = strip(node.keyRef).node.kind === 'float';
    const key = (k: unknown) => (float && Object.is(k, -0) ? PsszNegativeZero.value : k);
    if (node.assoc === 'map') return new Map((items as [unknown, unknown][]).map(([k, v]) => [key(k), v]));
    return new Set(float ? items.map(key) : items);
  }

  /**
   * A nested member's `lengthBound` (through Option) against its encoded
   * form `[start, end)`: it counts the bytes of the inner encoding. An
   * absent optional has no length.
   */
  nestedBoundAdmits(bound: LengthBound, node: Node, bytes: Uint8Array, start: number, end: number): boolean {
    while (node.kind === 'option') {
      node = node.child;
      const selector = this.canBeEmpty(node);
      if (start === end || (selector && bytes[start] === 0)) return true;
      if (selector) ++start;
    }
    return boundAdmits(bound, end - start);
  }

  // `w`: the width of the node at its position (a tuple's can differ from
  // one position to another, under a position cap).
  fieldLayouts(node: Node, w: number): FieldLayout[] {
    const memo = (node.fields ??= []);
    return (memo[w] ??= node.members.map((member) => {
      const info = this.layout(member.node);
      return { fixedSize: info.fixed ? info.size : w, variable: !info.fixed, optional: info.optional };
    }));
  }

  // ── Encode ────────────────────────────────────────────────────────────────

  private encodeFields(layouts: Layout[], parts: Uint8Array[], stable: boolean, w: number): Uint8Array {
    let fixed = 0;
    let total = 0;
    for (let i = 0; i < parts.length; ++i) {
      if (layouts[i].fixed) {
        if (parts[i].length !== layouts[i].size) valueError('pssz IR: fixed encoder size mismatch');
        fixed += layouts[i].size;
      } else {
        fixed += w;
        total += parts[i].length;
      }
    }
    const header = stable ? 0 : w;
    total += header + fixed;
    const maxOffset = w === 4 ? 0xffffffff : (1 << (w * 8)) - 1;
    const writeOffset = (at: number, value: number) => {
      if (value > maxOffset) valueError('pssz IR: offset width overflow');
      for (let i = 0; i < w; ++i) out[at + i] = Math.floor(value / 2 ** (8 * i)) & 0xff;
    };
    const out = new Uint8Array(total);
    if (!stable) writeOffset(0, fixed);
    let cursor = header;
    let tail = header + fixed;
    for (let i = 0; i < parts.length; ++i) {
      if (layouts[i].fixed) {
        out.set(parts[i], cursor);
        cursor += layouts[i].size;
      } else {
        writeOffset(cursor, tail - header);
        out.set(parts[i], tail);
        tail += parts[i].length;
        cursor += w;
      }
    }
    return out;
  }

  checkDepth(n: number, fail: (m: string) => never): void {
    if (n > MAX_DEPTH) fail('pssz IR: node depth limit exceeded');
  }

  /**
   * `cap`: the position cap of the value (spec 3.6; none at a root).
   * `count`: the `lengthBound` count of the record member the value is; it
   * reaches a list through Option only.
   */
  encodeAt(
    node: Node, value: unknown, n: number, cap: bigint | null = null, count: bigint | null = null,
  ): Uint8Array {
    this.checkDepth(n, valueError);
    switch (node.kind) {
      case 'int': {
        let v: bigint;
        if (typeof value === 'bigint') v = value;
        else if (typeof value === 'number' && Number.isSafeInteger(value)) v = BigInt(value);
        else return valueError('pssz IR: integer value required');
        const bits = BigInt(node.bits);
        if (node.signed) {
          const limit = 1n << (bits - 1n);
          if (v < -limit || v >= limit) valueError('pssz IR: signed integer overflow');
        } else if (v < 0n || v >= 1n << bits) valueError('pssz IR: unsigned integer overflow');
        const out = new Uint8Array(node.size);
        let raw = BigInt.asUintN(node.size * 8, v);
        for (let i = 0; i < node.size; ++i) {
          out[i] = Number(raw & 0xffn);
          raw >>= 8n;
        }
        return out;
      }
      case 'float': {
        const out = new Uint8Array(node.size);
        const view = new DataView(out.buffer);
        if (value instanceof PsszNegativeZero) value = -0;
        if (value instanceof PsszNaN) {
          if (value.size !== node.size) return valueError('pssz IR: NaN bits of the wrong float width');
          if (node.size === 4) view.setUint32(0, Number(value.bits), true);
          else view.setBigUint64(0, value.bits, true);
        } else if (typeof value !== 'number') return valueError('pssz IR: float value required');
        // A plain NaN is the quiet NaN, whatever bits the engine would pick.
        else if (node.size === 4) {
          if (Number.isNaN(value)) view.setUint32(0, QUIET_NAN_32, true);
          else view.setFloat32(0, value, true);
        } else if (Number.isNaN(value)) view.setBigUint64(0, QUIET_NAN_64, true);
        else view.setFloat64(0, value, true);
        return out;
      }
      case 'bool':
        if (typeof value !== 'boolean') return valueError('pssz IR: bool value required');
        return Uint8Array.of(value ? 1 : 0);
      case 'char':
        if (typeof value !== 'number' || !Number.isInteger(value) || value < 0 || value > 255)
          return valueError('pssz IR: char byte value required');
        return Uint8Array.of(value);
      case 'string': {
        // A raw string is bytes; a JavaScript string stands for its UTF-8.
        let bytes: Uint8Array;
        if (node.json) bytes = utf8Encode(jsonValueText(value, valueError));
        else if (node.raw && value instanceof Uint8Array) bytes = new Uint8Array(value);
        else if (typeof value !== 'string')
          return valueError(node.raw ? 'pssz IR: raw string value (bytes or string) required' : 'pssz IR: string value required');
        else if (!wellFormed(value)) return valueError('pssz IR: string is not valid UTF-8');
        else bytes = utf8Encode(value);
        this.boundBytes(node.child, bytes.length, valueError);
        return bytes;
      }
      case 'hex': {
        if (!(value instanceof Uint8Array)) return valueError('pssz IR: bytes value required');
        this.boundBytes(node.child, value.length, valueError);
        if (value.length) this.checkDepth(n + 1, valueError);
        return new Uint8Array(value);
      }
      case 'bitvector':
      case 'bitlist': {
        const list = node.kind === 'bitlist';
        if (!isObject(value) || !(value.bytes instanceof Uint8Array) || typeof value.bitCount !== 'number')
          return valueError('pssz IR: explicit packed Bits value required');
        const count = value.bitCount;
        const data = value.bytes;
        if (!Number.isInteger(count) || count < 0 || (list ? count > node.size : count !== node.size))
          return valueError('pssz IR: packed bit logical count mismatch');
        if (data.length !== packedByteCount(count)) valueError('pssz IR: packed bit byte count mismatch');
        if (count % 8 && data[data.length - 1] & ~((1 << (count % 8)) - 1))
          valueError('pssz IR: packed bits above logical count');
        if (!list) return new Uint8Array(data);
        const out = new Uint8Array(Math.floor(count / 8) + 1);
        out.set(data);
        out[out.length - 1] |= 1 << (count % 8);
        return out;
      }
      case 'option': {
        const [some, payload] = unwrapOption(node, value, valueError);
        const selector = this.canBeEmpty(node.child);
        if (!some) return selector ? Uint8Array.of(0) : new Uint8Array(0);
        const inner = this.encodeAt(node.textChild, payload, n + 1, cap, count);
        if (!selector) return inner;
        const out = new Uint8Array(inner.length + 1);
        out[0] = 1;
        out.set(inner, 1);
        return out;
      }
      case 'record': {
        const fields = recordFields(node, value, valueError);
        const w = this.widthOf(node);
        const values: unknown[] = [];
        const layouts: Layout[] = [];
        let keep = 0;
        for (let i = 0; i < node.members.length; ++i) {
          const member = node.members[i];
          const info = this.layout(member.node);
          const own = fields(member, i);
          if (own === undefined && !info.optional)
            valueError(`pssz IR: missing required record value "${member.name}"`);
          let field: unknown = own === undefined ? null : own;
          if (member.bound) {
            field = settleMember(member.node, field);
            if (!memberBoundAdmits(member.bound, member.node, field))
              valueError('pssz IR: member length bound violated');
          }
          if (member.order) this.checkOrder(member, field, valueError);
          values.push(field);
          layouts.push(info);
          if (node.stable || !info.optional || field !== null) keep = values.length;
        }
        const parts: Uint8Array[] = [];
        // The members lie beneath this record's cap, or beneath none.
        const beneath = this.capBeneath(node, cap);
        for (let i = 0; i < keep; ++i) {
          const member = node.members[i];
          const part = this.encodeAt(member.text, values[i], n + 1, beneath, boundCount(member.bound));
          // A nested member's bound counts its encoded bytes.
          if (
            member.bound && nestedMember(member.node) &&
            !this.nestedBoundAdmits(member.bound, member.node, part, 0, part.length)
          )
            valueError('pssz IR: member length bound violated');
          parts.push(part);
        }
        layouts.length = keep;
        const out = this.encodeFields(layouts, parts, node.stable, w);
        if (this.overCap(node, out.length)) valueError('pssz IR: encoded size exceeds maxDynamicData cap');
        return out;
      }
      case 'tuple': {
        if (!Array.isArray(value) || value.length !== node.members.length)
          return valueError('pssz IR: tuple value count mismatch');
        const layouts: Layout[] = [];
        let keep = 0;
        for (let i = 0; i < value.length; ++i) {
          const info = this.layout(node.members[i].node);
          layouts.push(info);
          if (!info.optional || (value[i] !== null && value[i] !== undefined)) keep = i + 1;
        }
        const parts: Uint8Array[] = [];
        for (let i = 0; i < keep; ++i) parts.push(this.encodeAt(node.members[i].text, value[i], n + 1, cap));
        layouts.length = keep;
        return this.encodeFields(layouts, parts, false, this.widthOf(node, cap));
      }
      case 'list':
      case 'array':
        if (node.assoc) return this.encodeAssoc(node, value, n, cap, count);
        if (!Array.isArray(value)) return valueError('pssz IR: sequence value required');
        return this.encodeSequence(node, value, n, cap, count);
      case 'variant': {
        if (!isObject(value) || typeof value.type !== 'string')
          return valueError('pssz IR: a variant value is { type, value }');
        const selector = node.byName!.get(value.type);
        if (selector === undefined) return valueError(`pssz IR: unknown variant alternative "${value.type}"`);
        const inner = this.encodeAt(node.members[selector].text, value.value, n + 1, cap);
        const out = new Uint8Array(inner.length + 1);
        out[0] = selector;
        out.set(inner, 1);
        return out;
      }
      case 'nested': {
        if (this.isOpaque(node)) {
          if (!(value instanceof Uint8Array))
            return valueError('pssz IR: bytes value required for an opaque nested value');
          if (value.length) this.checkDepth(n + 1, valueError);
          return new Uint8Array(value);
        }
        // The inner value's canonical encoding, always in the automatic
        // profile; its root is one node level below the nested node. It is
        // a root of its own: no position cap reaches into it.
        return this.auto.encodeAt(node.textChild, value, n + 1);
      }
      case 'custom': {
        // A wrapper adds nothing to the bytes. The values inside it were
        // checked as they were written; then this one, as `decode` gives it.
        const out = this.encodeAt(node.inner!, value, n, cap, count);
        if (!node.boxed && this.state.quiet === 0 && (node.handler?.check || (!node.handler && this.requireHandlers))) {
          const inner = node.inner!;
          const held = node.handler?.check
            ? this.quietly(() =>
              this.decodeAt(inner, out, 0, out.length, n, { allowUnknown: false, hasUnknown: false }, cap, count))
            : undefined;
          this.checkCustom(node, held, (message, invariant) => {
            throw new PsszError('value', message, undefined, invariant);
          });
        }
        return out;
      }
    }
  }

  // The elements of a list / array / map / set, in the order given.
  private encodeSequence(
    node: Node, value: unknown[], n: number, cap: bigint | null, count: bigint | null,
  ): Uint8Array {
    if (node.bounded && (node.exact ? value.length !== node.countNum : value.length > node.countNum))
      valueError('pssz IR: sequence bound exceeded');
    const w = this.widthOf(node, cap, count);
    const info = this.layout(node.child);
    if (!node.exact && info.fixed && !info.size && value.length)
      valueError('pssz IR: ambiguous zero-size list');
    const parts: Uint8Array[] = [];
    const layouts: Layout[] = [];
    for (let i = 0; i < value.length; ++i) {
      parts.push(this.encodeAt(node.textChild, value[i], n + 1, cap));
      layouts.push(info);
    }
    return this.encodeFields(layouts, parts, true, w);
  }

  // The byte container under a string / hex adapter admits `count` bytes.
  boundBytes(container: Node, count: number, fail: (m: string) => never): void {
    if (container.kind === 'array') {
      if (count !== container.countNum) fail('pssz IR: byte array count mismatch');
    } else if (container.bounded && count > container.countNum) fail('pssz IR: byte count bound exceeded');
  }

  // ── Decode ────────────────────────────────────────────────────────────────

  readOffset(bytes: Uint8Array, at: number, w: number): number {
    let out = 0;
    for (let i = 0; i < w; ++i) out += bytes[at + i] * 2 ** (8 * i);
    return out;
  }

  /**
   * A port of `pssz_record_spans`. Returns, per field, `[begin, end)` and
   * whether the writer stored it; `unknown` when the writer appended slots
   * this reader does not know.
   */
  recordSpans(
    bytes: Uint8Array, begin: number, end: number, w: number, stable: boolean, fields: FieldLayout[],
  ): { begins: number[]; ends: number[]; present: boolean[]; unknown: boolean } {
    const count = fields.length;
    const begins = new Array<number>(count).fill(0);
    const ends = new Array<number>(count).fill(0);
    const present = new Array<boolean>(count).fill(false);
    let knownSize = 0;
    for (const field of fields) knownSize += field.fixedSize;
    let fixedStart = begin;
    let fixedSize = knownSize;
    if (!stable) {
      if (end - begin < w) bytesError('pssz: record header truncated');
      fixedSize = this.readOffset(bytes, begin, w);
      fixedStart += w;
    }
    if (fixedSize > end - fixedStart) bytesError('pssz: record fixed region truncated');
    if (fixedSize > knownSize && (fixedSize - knownSize) % w !== 0)
      bytesError('pssz: partial unknown optional slot');
    const unknown = fixedSize > knownSize;

    const payloadSize = end - fixedStart;
    let previousField = count;
    let previousOffset = fixedSize;
    let seenVariable = false;
    const acceptOffset = (offset: number, index: number) => {
      if (offset > payloadSize || offset < previousOffset || (!seenVariable && offset !== fixedSize))
        bytesError('pssz: invalid record payload offset');
      if (previousField !== count) ends[previousField] = fixedStart + offset;
      if (index !== count) begins[index] = fixedStart + offset;
      previousField = index;
      previousOffset = offset;
      seenVariable = true;
    };

    let cursor = 0;
    let missing = false;
    for (let i = 0; i < count; ++i) {
      const field = fields[i];
      if (missing || field.fixedSize > fixedSize - cursor) {
        if (cursor !== fixedSize) bytesError('pssz: header cuts a known field');
        if (!field.optional || stable) bytesError('pssz: required record field missing');
        missing = true;
        continue;
      }
      present[i] = true;
      if (field.variable) acceptOffset(this.readOffset(bytes, fixedStart + cursor, w), i);
      else {
        begins[i] = fixedStart + cursor;
        ends[i] = begins[i] + field.fixedSize;
      }
      cursor += field.fixedSize;
    }
    if (unknown)
      for (let offset = knownSize; offset < fixedSize; offset += w)
        acceptOffset(this.readOffset(bytes, fixedStart + offset, w), count);
    if (previousField !== count) ends[previousField] = end;
    if (!seenVariable && payloadSize !== fixedSize) bytesError('pssz: fixed record has trailing payload');
    return { begins, ends, present, unknown };
  }

  private decodeMembers(
    node: Node, bytes: Uint8Array, start: number, end: number, stable: boolean, n: number,
    ctx: DecodeContext, cap: bigint | null,
  ): unknown[] {
    const w = this.widthOf(node, cap);
    // A record's members lie beneath its cap, or beneath none.
    const beneath = this.capBeneath(node, cap);
    const fields = this.fieldLayouts(node, w);
    const { begins, ends, present, unknown } = this.recordSpans(bytes, start, end, w, stable, fields);
    if (unknown && !ctx.allowUnknown) bytesError('pssz IR: unknown fields in exact decode');
    if (unknown) ctx.hasUnknown = true;
    const values: unknown[] = [];
    for (let i = 0; i < fields.length; ++i) {
      const member = node.members[i];
      // A field the writer did not store is an absent optional.
      const value = present[i]
        ? this.decodeAt(member.text, bytes, begins[i], ends[i], n + 1, ctx, beneath, boundCount(member.bound))
        : null;
      // Pruning is mandatory: the last slot a writer emits is never an
      // absent optional. A frame with unknown appended slots is not judged.
      if (
        !stable && !unknown && present[i] && fields[i].optional &&
        !(i + 1 < fields.length && present[i + 1]) && value === null
      )
        bytesError('pssz IR: trailing absent optional field was not pruned');
      if (member.bound && !memberBoundAdmits(member.bound, member.node, value))
        bytesError('pssz IR: member length bound violated');
      // A nested member's bound counts its encoded bytes.
      if (
        member.bound && present[i] && nestedMember(member.node) &&
        !this.nestedBoundAdmits(member.bound, member.node, bytes, begins[i], ends[i])
      )
        bytesError('pssz IR: member length bound violated');
      if (member.order) this.checkOrder(member, value, bytesError);
      values.push(value);
    }
    return values;
  }

  private decodeSequence(
    node: Node, bytes: Uint8Array, start: number, end: number, n: number, ctx: DecodeContext,
    cap: bigint | null, count_: bigint | null,
  ): unknown[] {
    const element = node.child;
    const w = this.widthOf(node, cap, count_);
    const info = this.layout(element);
    const size = end - start;
    let count: number;
    if (node.exact) {
      if (!Number.isFinite(node.countNum)) bytesError('pssz IR: array count overflow');
      count = node.countNum;
    } else if (size === 0) count = 0;
    else if (info.fixed) {
      if (!info.size || size % info.size) bytesError('pssz IR: fixed list span mismatch');
      count = size / info.size;
    } else {
      if (w > size) bytesError('pssz IR: offset underrun');
      const first = this.readOffset(bytes, start, w);
      if (!first || first % w || first > size) bytesError('pssz IR: invalid list offset table');
      count = first / w;
    }
    if (node.bounded && (node.exact ? count !== node.countNum : count > node.countNum))
      bytesError('pssz IR: sequence bound exceeded');
    const out: unknown[] = [];
    if (info.fixed) {
      // The count is checked against the input before anything is built.
      if (count * info.size !== size) bytesError('pssz IR: fixed sequence span mismatch');
      if (!info.size && count > ZERO_SIZE_ARRAY_LIMIT)
        bytesError('pssz IR: zero-size array count exceeds the implementation limit');
      for (let i = 0; i < count; ++i) {
        const at = start + i * info.size;
        out.push(this.decodeAt(node.textChild, bytes, at, at + info.size, n + 1, ctx, cap));
      }
    } else {
      if (count > Math.floor(size / w) || (!count && size)) bytesError('pssz IR: list table span mismatch');
      const table = count * w;
      let previous = table;
      for (let i = 0; i < count; ++i) {
        const begin = this.readOffset(bytes, start + i * w, w);
        const stop = i + 1 < count ? this.readOffset(bytes, start + (i + 1) * w, w) : size;
        if ((!i && begin !== table) || begin < previous || begin > stop || stop > size)
          bytesError('pssz IR: invalid list offsets');
        out.push(this.decodeAt(node.textChild, bytes, start + begin, start + stop, n + 1, ctx, cap));
        previous = begin;
      }
    }
    return out;
  }

  /** `cap`, `count`: as for `encodeAt`. */
  decodeAt(
    node: Node, bytes: Uint8Array, start: number, end: number, n: number, ctx: DecodeContext,
    cap: bigint | null = null, count: bigint | null = null,
  ): unknown {
    this.checkDepth(n, bytesError);
    const size = end - start;
    switch (node.kind) {
      case 'int': {
        if (size !== node.size) bytesError('pssz IR: integer span mismatch');
        if (node.size <= 4) {
          let raw = this.readOffset(bytes, start, node.size);
          if (node.bits === 1 && raw !== 0 && raw !== (node.signed ? 0xff : 1))
            bytesError('pssz IR: one-bit integer overflow');
          if (node.signed && bytes[end - 1] & 0x80) raw -= 2 ** (node.size * 8);
          return node.time ? BigInt(raw) : raw;
        }
        let raw = 0n;
        for (let i = node.size - 1; i >= 0; --i) raw = (raw << 8n) | BigInt(bytes[start + i]);
        return node.signed ? BigInt.asIntN(node.size * 8, raw) : raw;
      }
      case 'float': {
        if (size !== node.size) bytesError('pssz IR: float span mismatch');
        const view = new DataView(bytes.buffer, bytes.byteOffset + start, size);
        const value = size === 4 ? view.getFloat32(0, true) : view.getFloat64(0, true);
        if (Number.isNaN(value)) {
          const bits = size === 4 ? BigInt(view.getUint32(0, true)) : view.getBigUint64(0, true);
          if (bits !== (size === 4 ? BigInt(QUIET_NAN_32) : QUIET_NAN_64)) return new PsszNaN(bits, size as 4 | 8);
        }
        return value;
      }
      case 'bool':
        if (size !== 1 || bytes[start] > 1) bytesError('pssz IR: invalid bool');
        return bytes[start] === 1;
      case 'char':
        if (size !== 1) bytesError('pssz IR: char span mismatch');
        return bytes[start];
      case 'string':
        this.boundBytes(node.child, size, bytesError);
        // "string" is raw bytes; "utf8" is validated.
        if (node.raw) return bytes.slice(start, end);
        if (node.json) return jsonTextValue(bytes.subarray(start, end), ctx.jsonText === true, bytesError);
        if (!utf8Valid(bytes, start, end)) bytesError('pssz IR: string is not valid UTF-8');
        return utf8Decode(bytes, start, end);
      case 'hex':
        this.boundBytes(node.child, size, bytesError);
        if (size) this.checkDepth(n + 1, bytesError);
        return bytes.slice(start, end);
      case 'bitvector':
      case 'bitlist': {
        let count = node.size;
        if (node.kind === 'bitlist') {
          if (size === 0 || bytes[end - 1] === 0)
            bytesError('pssz IR: bitlist delimiter missing or noncanonical');
          const highest = 31 - Math.clz32(bytes[end - 1]);
          count = (size - 1) * 8 + highest;
          if (count > node.size) bytesError('pssz IR: bitlist count exceeds bound');
        } else {
          if (size !== packedByteCount(count)) bytesError('pssz IR: packed bit byte count mismatch');
          if (count % 8 && bytes[end - 1] & ~((1 << (count % 8)) - 1))
            bytesError('pssz IR: packed bits above logical count');
        }
        const data = bytes.slice(start, start + packedByteCount(count));
        if (node.kind === 'bitlist' && count % 8) data[data.length - 1] &= (1 << (count % 8)) - 1;
        const value: BitsValue = { bytes: data, bitCount: count };
        return value;
      }
      case 'option': {
        if (this.canBeEmpty(node.child)) {
          if (size === 0) bytesError('pssz IR: optional selector missing');
          if (bytes[start] === 0) {
            if (size !== 1) bytesError('pssz IR: trailing None bytes');
            return null;
          }
          if (bytes[start] !== 1) bytesError('pssz IR: invalid optional selector');
          ++start;
        } else if (size === 0) return null;
        return wrapSome(node, this.decodeAt(node.textChild, bytes, start, end, n + 1, ctx, cap, count));
      }
      case 'record': {
        if (this.overCap(node, size)) bytesError('pssz IR: encoded size exceeds maxDynamicData cap');
        const values = this.decodeMembers(node, bytes, start, end, node.stable, n, ctx, cap);
        // A positional argument list is the array of its arguments.
        if (node.tuple === 'positional') return values;
        const out: Record<string, unknown> = {};
        for (let i = 0; i < values.length; ++i) setField(out, node.members[i].name, values[i]);
        return out;
      }
      case 'tuple':
        return this.decodeMembers(node, bytes, start, end, false, n, ctx, cap);
      case 'list':
      case 'array':
        if (node.assoc) return this.decodeAssoc(node, bytes, start, end, n, ctx, cap, count);
        return this.decodeSequence(node, bytes, start, end, n, ctx, cap, count);
      case 'variant': {
        if (size === 0) bytesError('pssz IR: variant selector missing');
        const selector = bytes[start];
        if (selector >= node.members.length) bytesError('pssz IR: variant selector out of range');
        const member = node.members[selector];
        const value: VariantValue = {
          type: member.name,
          value: this.decodeAt(member.text, bytes, start + 1, end, n + 1, ctx, cap),
        };
        return value;
      }
      case 'nested':
        if (this.isOpaque(node)) {
          if (size) this.checkDepth(n + 1, bytesError);
          return bytes.slice(start, end);
        }
        // The bytes are read as the inner type, in the automatic profile,
        // under the same unknown-field rule; its root is one level below.
        return this.auto.decodeAt(node.textChild, bytes, start, end, n + 1, ctx);
      case 'custom': {
        // The values inside were checked as they were read; then this one.
        const value = this.decodeAt(node.inner!, bytes, start, end, n, ctx, cap, count);
        this.checkCustom(node, value, (message, invariant) => {
          throw new PsszError('bytes', message, undefined, invariant);
        });
        return value;
      }
    }
  }
}
