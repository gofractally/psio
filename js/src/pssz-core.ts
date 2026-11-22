// pssz-core.ts — schema-driven PSSZ codec over the Schema IR.
//
// A port of the checked runtime codec `cpp/include/psio/pssz_ir.hpp` (and its
// Rust twin `psio::pssz::dynamic`): the same width derivation, the same frame
// parser, the same accept / reject set. It holds no reference to fracpack.
//
// This file is internal. The public API is `pssz.ts`.

import { parseTimeSchemaId, reservedTimeId, timeFractionDigits, type TimeKind } from './time.js';

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
  | { type: 'FracPack'; value: { type: IrAnyType } }
  | { type: 'Custom'; value: { type: IrAnyType; id: string } }
  | { type: 'Resource'; value: unknown }
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
/** `psio::Schema`. Only `types` is read by the codec. */
export interface IrSchema {
  package: { name: string; version: string; attributes: IrAttribute[] };
  interfaces: unknown[];
  worlds: unknown[];
  uses: unknown[];
  types: IrMember[];
}

/**
 * The compact spelling of a type (the shape `schema.ts` `AnyType` uses, plus
 * `BoundedList` and attributes). Accepted wherever a type is expected and
 * normalized to `IrAnyType`.
 */
export type CompactType =
  | string
  | { Int: { bits: number; isSigned: boolean } }
  | { Float: { exp: number; mantissa: number } }
  | { List: PsszTypeInput }
  | { Option: PsszTypeInput }
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
export type PsszSchemaInput =
  | IrSchema
  | { types: CompactMember[] }
  | { [name: string]: PsszTypeInput };

function schemaError(message: string): never {
  throw new PsszError('schema', message);
}

const IR_KINDS = new Set([
  'Struct', 'Object', 'Array', 'List', 'BoundedList', 'Option', 'Variant', 'Tuple', 'Int',
  'Float', 'FracPack', 'Custom', 'Resource', 'Type', 'Enum',
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
function normalizeMember(x: unknown, depth: number): IrMember {
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
    if (kind === 'List' || kind === 'Option' || kind === 'FracPack') body = { type: body };
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
      return { value: { type: 'Resource', value: body } };
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
    case 'FracPack':
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
  let pkg = { name: '', version: '', attributes: [] as IrAttribute[] };
  let rest: { interfaces: unknown[]; worlds: unknown[]; uses: unknown[] } = {
    interfaces: [], worlds: [], uses: [],
  };
  if (Array.isArray(x.types)) {
    types = x.types.map((m) => normalizeMember(m, 0));
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
  return { package: pkg, ...rest, types };
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
 * The text form of a custom type (a `Custom` id that is not part of the
 * binary form): `toText` gets the decoded value of the wire type, `fromText`
 * returns one. Either may throw for a value / text it does not accept.
 */
export interface PsszCustomHandler {
  toText(value: unknown): string;
  fromText(text: string): unknown;
}

// Custom ids that are part of the binary form (pssz_ir.hpp "Custom ids").
// Any other id is "box" or a text presentation: its inner type on the wire.
const WIRE_CUSTOM_IDS = new Set(['bool', 'char', 'string', 'utf8', 'hex', 'bitvector', 'bitlist', 'unknown']);
/**
 * Ids a handler cannot be registered for: the binary-form ids, "box", and
 * the built-in presentations (the time ids of docs/time.md).
 */
export function reservedCustomId(id: string): boolean {
  return id === 'box' || WIRE_CUSTOM_IDS.has(id) || reservedTimeId(id);
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
    /** Variant alternative's I-JSON name. */
    readonly ijsonName: string,
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

export type NodeKind =
  | 'int' | 'float' | 'bool' | 'char' | 'string' | 'hex' | 'bitvector' | 'bitlist'
  | 'option' | 'list' | 'array' | 'tuple' | 'record' | 'variant'
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
   * int: a time value (Custom time id over the tick integer, docs/time.md).
   * Its value is always a bigint tick count.
   */
  time: TimeKind | null = null;
  /** time point: fraction digits of its RFC 3339 text; -1 for no text form. */
  timeDigits = -1;

  layoutMemo?: Memo<Layout>;
  emptyMemo?: Memo<boolean>;
  width?: number;
  boundMemo: (bigint | null | undefined)[] = [undefined, undefined];
  fixedPayloadMemo?: boolean;
  fields?: FieldLayout[];
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

function findAttribute(attributes: IrAttribute[], name: string): IrAttribute | undefined {
  for (const a of attributes) if (a.name === name) return a;
  return undefined;
}
function parseU64(text: string): bigint | null {
  if (!/^(0|[1-9][0-9]{0,19})$/.test(text)) return null;
  const v = BigInt(text);
  return v > U64_MAX ? null : v;
}
function parseLengthBound(attributes: IrAttribute[]): LengthBound | null {
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
function parseCap(attributes: IrAttribute[]): bigint | null {
  const attribute = findAttribute(attributes, 'maxDynamicData');
  if (!attribute) return null;
  const n = attribute.value === null ? null : parseU64(attribute.value);
  if (n === null) return schemaError('pssz IR: malformed maxDynamicData attribute');
  return n;
}
function intForm(attributes: IrAttribute[], name: string): IntForm {
  const a = findAttribute(attributes, name);
  if (a && a.value !== null) return a.value === 'string' ? 'string' : 'number';
  return 'natural';
}

type Site = 'record' | 'record_member' | 'variant' | 'variant_member';

// `caps`: the automatic profile, which also takes maxFields; an explicit
// profile rejects that constraint, as the typed psio::pssz_<W> codecs do.
function checkAttributes(values: IrAttribute[], site: Site, caps: boolean): void {
  for (const { name, value } of values) {
    if (name === 'doc' || name === 'description' || name === 'visible') continue;
    if (site === 'record' && name === 'definitionWillNotChange') continue;
    if (site === 'record' && (name === 'maxDynamicData' || (caps && name === 'maxFields'))) continue;
    if (site === 'record_member' && (name === 'lengthBound' || name === 'jsonInt' || name === 'ijsonInt')) {
      if (name !== 'lengthBound' && value !== 'number' && value !== 'string')
        schemaError('pssz IR: malformed integer form attribute');
      continue;
    }
    if (site === 'variant_member' && name === 'ijsonName' && value !== null) continue;
    schemaError('pssz IR: unsupported schema constraint or annotation');
  }
}

/** Per-call decode state. */
export interface DecodeContext {
  allowUnknown: boolean;
  hasUnknown: boolean;
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
  if (typeof value === 'string') return boundAdmits(bound, utf8Length(value));
  if (value instanceof Uint8Array) return boundAdmits(bound, value.length);
  if (Array.isArray(value)) return boundAdmits(bound, value.length);
  return true;
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

  constructor(
    readonly schema: IrSchema,
    readonly W: 0 | 1 | 2 | 4,
    /** Custom id -> text handler, bound to the nodes as they are built. */
    readonly handlers: ReadonlyMap<string, PsszCustomHandler> = new Map(),
    /** A custom id with no handler is an error in text, not the inner type's text. */
    readonly requireHandlers = false,
  ) {
    for (const member of schema.types)
      this.named.set(member.name, this.named.has(member.name) ? null : member);
  }

  private get automatic(): boolean {
    return this.W === 0;
  }

  private resolve(input: IrAnyType): IrAnyType {
    let out = input;
    for (let hops = 0; hops <= this.schema.types.length; ++hops) {
      if (out.value.type !== 'Type') return out;
      const found = this.named.get(out.value.value.type);
      if (found === null) schemaError('pssz IR: duplicate named type');
      if (found === undefined) return schemaError(`pssz IR: unresolved named type "${out.value.value.type}"`);
      out = found!.type;
    }
    return schemaError('pssz IR: cyclic named aliases');
  }

  // The type `input` is on the wire: aliases followed, and the nodes that
  // add nothing to the bytes (Enum, "box", text presentations) looked through.
  private wire(input: IrAnyType): IrTypeValue {
    let out = input;
    for (let hops = 0; hops <= 4 * this.schema.types.length + 64; ++hops) {
      out = this.resolve(out);
      const t = out.value;
      if (t.type === 'Enum') out = t.value.type;
      else if (t.type === 'Custom' && !WIRE_CUSTOM_IDS.has(t.value.id)) out = t.value.type;
      else return t;
    }
    return schemaError('pssz IR: cyclic named aliases');
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
      return this.build(root, added, 0);
    } catch (e) {
      for (const key of added) this.nodes.delete(key);
      if (e instanceof RangeError) schemaError('pssz IR: schema nesting too deep');
      throw e;
    }
  }

  private build(input: IrAnyType, added: IrAnyType[], depth: number): Node {
    if (depth > 512) schemaError('pssz IR: schema nesting too deep');
    const type = this.resolve(input);
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
          under = this.wire(t.value.type);
        } catch (e) {
          // An enum over itself has no integer under it.
          if (!(e instanceof PsszError) || !e.message.includes('cyclic')) throw e;
        }
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
        node.kind = 'tuple';
        node.members = t.value.members.map(
          (m, i) => new MemberInfo(String(i), this.build(m, added, d), null, 'natural', 'natural', ''),
        );
        break;
      case 'Struct':
      case 'Object':
      case 'Variant': {
        const record = t.type !== 'Variant';
        node.kind = record ? 'record' : 'variant';
        checkAttributes(t.value.attributes, record ? 'record' : 'variant', this.automatic);
        if (record) {
          node.stable =
            t.type === 'Struct' || t.value.attributes.some((a) => a.name === 'definitionWillNotChange');
          node.cap = parseCap(t.value.attributes);
        } else if (t.value.members.length === 0 || t.value.members.length > 256)
          schemaError('pssz IR: invalid variant schema');
        const seen = new Set<string>();
        for (const member of t.value.members) {
          if (seen.has(member.name)) schemaError('pssz IR: duplicate schema member');
          seen.add(member.name);
          checkAttributes(member.attributes, record ? 'record_member' : 'variant_member', this.automatic);
          const name = record ? undefined : findAttribute(member.attributes, 'ijsonName');
          node.members.push(new MemberInfo(
            member.name,
            this.build(member.type, added, d),
            record ? parseLengthBound(member.attributes) : null,
            record ? intForm(member.attributes, 'jsonInt') : 'natural',
            record ? intForm(member.attributes, 'ijsonInt') : 'natural',
            name && name.value !== null ? name.value : member.name,
          ));
        }
        node.byName = new Map(node.members.map((m, i) => [m.name, i]));
        break;
      }
      case 'Custom': {
        const id = t.value.id;
        const time = parseTimeSchemaId(id);
        if (time) {
          // A built-in presentation: a duration / time point is its tick
          // integer on the wire, bigint ticks as a value, and has the text
          // form of docs/time.md.
          const ticks = this.wire(t.value.type);
          const bits = ticks.type === 'Int' ? ticks.value.bits : 0;
          if (ticks.type !== 'Int' || (bits !== 8 && bits !== 16 && bits !== 32 && bits !== 64))
            return schemaError('pssz IR: time value requires an 8/16/32/64-bit tick integer');
          node.kind = 'int';
          node.bits = bits;
          node.signed = ticks.value.isSigned;
          node.size = bits / 8;
          node.time = time;
          node.timeDigits = time.point ? timeFractionDigits(time.num, time.den) : -1;
          break;
        }
        if (reservedTimeId(id)) return schemaError('pssz IR: unsupported custom adapter');
        if (!WIRE_CUSTOM_IDS.has(id)) {
          // "box" or a text presentation: the inner type on the wire.
          node.kind = 'custom';
          node.id = id;
          node.boxed = id === 'box';
          if (!node.boxed) node.handler = this.handlers.get(id);
          node.inner = this.build(t.value.type, added, d);
          break;
        }
        const inner = this.wire(t.value.type);
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
          if (!unsignedInt(this.wire(element), 1))
            schemaError('pssz IR: packed bit element must be unsigned one-bit integer');
          if (bound > BigInt(Number.MAX_SAFE_INTEGER))
            schemaError('pssz IR: bit count exceeds platform capacity');
          node.kind = id;
          node.size = Number(bound);
        } else if (id === 'string' || id === 'utf8' || id === 'hex') {
          if (inner.type !== 'List' && inner.type !== 'BoundedList' && inner.type !== 'Array')
            return schemaError('pssz IR: byte adapter requires byte container');
          const element = this.wire(inner.value.type);
          if (element.type === 'Custom' && element.value.id === 'char') {
            if (!unsignedInt(this.wire(element.value.type), 8))
              schemaError('pssz IR: primitive adapter has incompatible inner schema');
          } else if (!unsignedInt(element, 8)) schemaError('pssz IR: byte adapter requires unsigned bytes');
          // "string" is raw bytes; "utf8" is validated on every read and write.
          node.kind = id === 'hex' ? 'hex' : 'string';
          node.raw = id === 'string';
        } else schemaError('pssz IR: unsupported custom adapter');
        node.textChild = this.build(t.value.type, added, d);
        break;
      }
      case 'FracPack':
        return schemaError('pssz IR: legacy FracPack marker is unsupported');
      default:
        return schemaError('pssz IR: unsupported schema type');
    }
    return node;
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
    // N elements, each its reserved payload plus an offset slot when it is
    // not an inline fixed payload.
    const elements = (child: Node, boxed: boolean, count: bigint): bigint | null => {
      if (count === 0n) return 0n;
      // A boxed value has no inferred bound (the typed codecs give it none).
      let element = boxed ? null : this.boundOf(child, null, true, active);
      if (!this.fixedPayload(child, [])) element = boundAdd(element, 4n);
      return boundMultiply(count, element);
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
        let total: bigint | null = node.stable ? 0n : 4n;
        for (const member of node.members) {
          let payload = member.boxed ? null : this.boundOf(member.node, member.bound, true, active);
          if (!this.fixedPayload(member.node, [])) payload = boundAdd(payload, 4n);
          total = boundAdd(total, payload);
        }
        out = total;
        break;
      }
      default:
        break; // tuple, variant, bit types: no inferred bound
    }
    active.pop();
    if (annotation === null) node.boundMemo[slot] = out;
    return out;
  }


  /** Width of a record's header and slots, or of a list / array table. */
  widthOf(node: Node): number {
    if (!this.automatic) return this.W;
    if (node.width !== undefined) return node.width;
    let out = 4;
    // A list's offset table is always 4 wide.
    if (node.kind !== 'list') {
      if (node.cap !== null) out = node.cap <= 0xffn ? 1 : node.cap <= 0xffffn ? 2 : 4;
      else out = widthClass(this.boundOf(node, null, false, []));
    }
    node.width = out;
    return out;
  }

  // A record over its explicit cap is not a value of its type. The automatic
  // profile checks every record where it is read or written.
  private overCap(node: Node, size: number): boolean {
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
  /** `encodeAt` of a top-level value, with the explicit profiles' cap check. */
  encodeTop(root: Node, value: unknown): Uint8Array {
    const out = this.encodeAt(root, value, 0);
    this.checkTopCap(root, out.length, valueError);
    return out;
  }
  /** `decodeAt` of a whole top-level frame, with the explicit profiles' cap check. */
  decodeTop(root: Node, bytes: Uint8Array, ctx: DecodeContext): unknown {
    this.checkTopCap(root, bytes.length, bytesError);
    return this.decodeAt(root, bytes, 0, bytes.length, 0, ctx);
  }

  private fieldLayouts(node: Node, w: number): FieldLayout[] {
    if (!node.fields) {
      node.fields = node.members.map((member) => {
        const info = this.layout(member.node);
        return { fixedSize: info.fixed ? info.size : w, variable: !info.fixed, optional: info.optional };
      });
    }
    return node.fields;
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

  private checkDepth(n: number, fail: (m: string) => never): void {
    if (n > MAX_DEPTH) fail('pssz IR: node depth limit exceeded');
  }

  encodeAt(node: Node, value: unknown, n: number): Uint8Array {
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
        if (node.raw && value instanceof Uint8Array) bytes = new Uint8Array(value);
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
        const inner = this.encodeAt(node.child, payload, n + 1);
        if (!selector) return inner;
        const out = new Uint8Array(inner.length + 1);
        out[0] = 1;
        out.set(inner, 1);
        return out;
      }
      case 'record': {
        if (!isObject(value) || value instanceof Uint8Array)
          return valueError('pssz IR: record value required');
        for (const key of Object.keys(value))
          if (value[key] !== undefined && !node.byName!.has(key))
            valueError(`pssz IR: unknown record value "${key}"`);
        const w = this.widthOf(node);
        const values: unknown[] = [];
        const layouts: Layout[] = [];
        let keep = 0;
        for (const member of node.members) {
          const info = this.layout(member.node);
          const own = Object.prototype.hasOwnProperty.call(value, member.name)
            ? value[member.name]
            : undefined;
          if (own === undefined && !info.optional)
            valueError(`pssz IR: missing required record value "${member.name}"`);
          const field = own === undefined ? null : own;
          if (member.bound && !memberBoundAdmits(member.bound, member.node, field))
            valueError('pssz IR: member length bound violated');
          values.push(field);
          layouts.push(info);
          if (node.stable || !info.optional || field !== null) keep = values.length;
        }
        const parts: Uint8Array[] = [];
        for (let i = 0; i < keep; ++i) parts.push(this.encodeAt(node.members[i].node, values[i], n + 1));
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
        for (let i = 0; i < keep; ++i) parts.push(this.encodeAt(node.members[i].node, value[i], n + 1));
        layouts.length = keep;
        return this.encodeFields(layouts, parts, false, this.widthOf(node));
      }
      case 'list':
      case 'array': {
        if (!Array.isArray(value)) return valueError('pssz IR: sequence value required');
        if (node.bounded && (node.exact ? value.length !== node.countNum : value.length > node.countNum))
          valueError('pssz IR: sequence bound exceeded');
        const w = this.widthOf(node);
        const info = this.layout(node.child);
        if (!node.exact && info.fixed && !info.size && value.length)
          valueError('pssz IR: ambiguous zero-size list');
        const parts: Uint8Array[] = [];
        const layouts: Layout[] = [];
        for (let i = 0; i < value.length; ++i) {
          parts.push(this.encodeAt(node.child, value[i], n + 1));
          layouts.push(info);
        }
        return this.encodeFields(layouts, parts, true, w);
      }
      case 'variant': {
        if (!isObject(value) || typeof value.type !== 'string')
          return valueError('pssz IR: a variant value is { type, value }');
        const selector = node.byName!.get(value.type);
        if (selector === undefined) return valueError(`pssz IR: unknown variant alternative "${value.type}"`);
        const inner = this.encodeAt(node.members[selector].node, value.value, n + 1);
        const out = new Uint8Array(inner.length + 1);
        out[0] = selector;
        out.set(inner, 1);
        return out;
      }
      case 'custom':
        // A wrapper adds nothing to the bytes.
        return this.encodeAt(strip(node).node, value, n);
    }
  }

  // The byte container under a string / hex adapter admits `count` bytes.
  private boundBytes(container: Node, count: number, fail: (m: string) => never): void {
    if (container.kind === 'array') {
      if (count !== container.countNum) fail('pssz IR: byte array count mismatch');
    } else if (container.bounded && count > container.countNum) fail('pssz IR: byte count bound exceeded');
  }

  // ── Decode ────────────────────────────────────────────────────────────────

  private readOffset(bytes: Uint8Array, at: number, w: number): number {
    let out = 0;
    for (let i = 0; i < w; ++i) out += bytes[at + i] * 2 ** (8 * i);
    return out;
  }

  /**
   * A port of `pssz_record_spans`. Returns, per field, `[begin, end)` and
   * whether the writer stored it; `unknown` when the writer appended slots
   * this reader does not know.
   */
  private recordSpans(
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
    ctx: DecodeContext,
  ): unknown[] {
    const w = this.widthOf(node);
    const fields = this.fieldLayouts(node, w);
    const { begins, ends, present, unknown } = this.recordSpans(bytes, start, end, w, stable, fields);
    if (unknown && !ctx.allowUnknown) bytesError('pssz IR: unknown fields in exact decode');
    if (unknown) ctx.hasUnknown = true;
    const values: unknown[] = [];
    for (let i = 0; i < fields.length; ++i) {
      const member = node.members[i];
      // A field the writer did not store is an absent optional.
      const value = present[i] ? this.decodeAt(member.node, bytes, begins[i], ends[i], n + 1, ctx) : null;
      // Pruning is mandatory: the last slot a writer emits is never an
      // absent optional. A frame with unknown appended slots is not judged.
      if (
        !stable && !unknown && present[i] && fields[i].optional &&
        !(i + 1 < fields.length && present[i + 1]) && value === null
      )
        bytesError('pssz IR: trailing absent optional field was not pruned');
      if (member.bound && !memberBoundAdmits(member.bound, member.node, value))
        bytesError('pssz IR: member length bound violated');
      values.push(value);
    }
    return values;
  }

  private decodeSequence(
    node: Node, bytes: Uint8Array, start: number, end: number, n: number, ctx: DecodeContext,
  ): unknown[] {
    const element = node.child;
    const w = this.widthOf(node);
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
        out.push(this.decodeAt(element, bytes, at, at + info.size, n + 1, ctx));
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
        out.push(this.decodeAt(element, bytes, start + begin, start + stop, n + 1, ctx));
        previous = begin;
      }
    }
    return out;
  }

  decodeAt(node: Node, bytes: Uint8Array, start: number, end: number, n: number, ctx: DecodeContext): unknown {
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
        return wrapSome(node, this.decodeAt(node.child, bytes, start, end, n + 1, ctx));
      }
      case 'record': {
        if (this.overCap(node, size)) bytesError('pssz IR: encoded size exceeds maxDynamicData cap');
        const values = this.decodeMembers(node, bytes, start, end, node.stable, n, ctx);
        const out: Record<string, unknown> = {};
        for (let i = 0; i < values.length; ++i) setField(out, node.members[i].name, values[i]);
        return out;
      }
      case 'tuple':
        return this.decodeMembers(node, bytes, start, end, false, n, ctx);
      case 'list':
      case 'array':
        return this.decodeSequence(node, bytes, start, end, n, ctx);
      case 'variant': {
        if (size === 0) bytesError('pssz IR: variant selector missing');
        const selector = bytes[start];
        if (selector >= node.members.length) bytesError('pssz IR: variant selector out of range');
        const member = node.members[selector];
        const value: VariantValue = {
          type: member.name,
          value: this.decodeAt(member.node, bytes, start + 1, end, n + 1, ctx),
        };
        return value;
      }
      case 'custom':
        return this.decodeAt(strip(node).node, bytes, start, end, n, ctx);
    }
  }
}
