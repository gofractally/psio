// Wire contract: docs/wire-contract.md; audited tags, current public C++ semantics.
export const PJSON_WIRE_REVISION = 2;
import { xxh3 } from './xxh3.js';

/** The only error the pjson functions throw. */
export class PjsonError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'PjsonError';
  }
}
export class PjsonDecimal {
  constructor(readonly mantissa: bigint, readonly scale: number) {}
}
/** Explicit IEEE representation; ordinary JS numbers use C++'s compact encoding. */
export class PjsonFloat {
  constructor(readonly bits: bigint, readonly width: 1 | 2 | 3 = 3,
              readonly scientific = false) {}
  static fromNumber(value: number): PjsonFloat {
    const data = new DataView(new ArrayBuffer(8));
    data.setFloat64(0, value, true);
    return new PjsonFloat(data.getBigUint64(0, true));
  }
}
/** Preserve arbitrary string bytes and the escape-form flag without UTF-8 loss. */
export class PjsonString {
  constructor(readonly bytes: Uint8Array, readonly escaped = false) {}
}
export class PjsonTypedArray {
  constructor(readonly code: number, readonly bytes: Uint8Array) {}
}
/** Ordered entries preserve duplicate names and arbitrary key bytes. */
export class PjsonObject {
  constructor(readonly entries: [string | PjsonString, PjsonValue][]) {}
}
/** Explicit shared-schema row array. The C++ dynamic encoder emits ordinary arrays. */
export class PjsonRows {
  constructor(readonly records: (Map<string, PjsonValue> | PjsonObject)[],
              readonly keys?: (string | PjsonString)[]) {}
}
export type PjsonValue = null | boolean | number | bigint | string | Uint8Array |
  PjsonDecimal | PjsonFloat | PjsonString | PjsonTypedArray | PjsonRows | PjsonObject |
  PjsonValue[] | Map<string, PjsonValue> | { [key: string]: PjsonValue };

const utf8 = new TextEncoder();
const text = new TextDecoder('utf-8', { fatal: true });
const sizes = [1, 2, 4, 8, 1, 2, 4, 8, 4, 8];
const max128 = (1n << 128n) - 1n;
function require(condition: unknown, message: string): asserts condition {
  if (!condition) throw new PjsonError(message);
}
function join(parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0; for (const p of parts) { out.set(p, at); at += p.length; }
  return out;
}
function le(n: bigint | number, width: number): Uint8Array {
  let value = BigInt(n);
  require(value >= 0n && value < (1n << BigInt(width * 8)), 'Integer out of range');
  const out = new Uint8Array(width);
  for (let i = 0; i < width; ++i) { out[i] = Number(value & 255n); value >>= 8n; }
  return out;
}
function read(data: Uint8Array, at: number, width: number): bigint {
  require(at >= 0 && at + width <= data.length, 'Truncated integer');
  let n = 0n;
  for (let i = width - 1; i >= 0; --i) n = (n << 8n) | BigInt(data[at + i]);
  return n;
}
function width(n: number): number {
  require(n <= 0xffffffff, 'Container exceeds 32-bit offsets');
  return n <= 255 ? 1 : n <= 65535 ? 2 : n <= 0xffffff ? 3 : 4;
}
function varuint(n: number): Uint8Array {
  require(Number.isInteger(n) && n >= 0 && n < 2 ** 30, 'Varuint out of range');
  const count = n < 64 ? 1 : n < 16384 ? 2 : n < 4194304 ? 3 : 4;
  return join([Uint8Array.of(((count - 1) << 6) | (n & 63)), le(Math.floor(n / 64), count - 1)]);
}
function readVar(data: Uint8Array, at: number): [number, number] {
  require(at < data.length, 'Truncated varuint');
  const count = (data[at] >> 6) + 1;
  return [(data[at] & 63) + 64 * Number(read(data, at + 1, count - 1)), count];
}
function magnitude(n: bigint, tag: number): Uint8Array {
  require(n >= 0n && n <= max128, 'Integer exceeds 128 bits');
  let count = 1; for (let v = n >> 8n; v; v >>= 8n) ++count;
  return join([Uint8Array.of(tag | (count - 1)), le(n, count)]);
}
function integer(n: bigint): Uint8Array {
  return n >= 0n && n <= 15n ? Uint8Array.of(0x20 | Number(n)) :
    n < 0n && n >= -15n ? Uint8Array.of(0x30 | Number(-n)) :
    magnitude(n < 0n ? -n : n, n < 0n ? 0x50 : 0x40);
}
function decimal(m: bigint, scale: number): Uint8Array {
  require(m >= -(1n << 127n) && m < (1n << 127n), 'Decimal mantissa exceeds i128');
  if (scale === 0) return integer(m);
  const zz = m < 0n ? -m * 2n - 1n : m * 2n;
  return join([magnitude(zz, 0x70), varuint(scale < 0 ? -2 * scale - 1 : 2 * scale)]);
}
function ieee(value: PjsonFloat): Uint8Array {
  require([1, 2, 3].includes(value.width), 'Unsupported IEEE width');
  const masks = [null, [0x7c00n, 0x3ffn, 0x7e00n],
    [0x7f800000n, 0x7fffffn, 0x7fc00000n],
    [0x7ff0000000000000n, 0xfffffffffffffn, 0x7ff8000000000000n]];
  const [exp, frac, nan] = masks[value.width]!;
  const bits = (value.bits & exp) === exp && (value.bits & frac) !== 0n ? nan : value.bits;
  return join([Uint8Array.of(0x60 | value.width | (value.scientific ? 8 : 0)), le(bits, 1 << value.width)]);
}
function number(value: number): Uint8Array {
  if (!Number.isFinite(value)) return ieee(PjsonFloat.fromNumber(value));
  // Minus zero keeps its sign: binary16, the form every codec writes for it.
  if (Object.is(value, -0)) return Uint8Array.of(0x61, 0x00, 0x80);
  // Match to_chars' shortest notation, which counts the exponent sign and
  // its minimum two digits when choosing fixed versus scientific notation.
  const scientific = value.toExponential().replace(/e([+-])(\d)$/, 'e$10$2');
  const fixed = expandDecimal(value.toString());
  const shortest = fixed.length <= scientific.length ? fixed : scientific;
  const [digits, exp = '0'] = shortest.split('e');
  const point = digits.indexOf('.');
  const m = BigInt(digits.replace('.', ''));
  const scale = Number(exp) - (point < 0 ? 0 : digits.length - point - 1);
  const compact = decimal(m, scale);
  return compact.length < 9 ? compact : ieee(PjsonFloat.fromNumber(value));
}
function expandDecimal(s: string): string {
  if (!s.includes('e')) return s;
  const [d, e] = s.split('e'); const negative = d.startsWith('-');
  const digits = d.replace('-', '').replace('.', '');
  const at = 1 + Number(e);
  const fixed = at <= 0 ? '0.' + '0'.repeat(-at) + digits :
    at >= digits.length ? digits + '0'.repeat(at - digits.length) :
    digits.slice(0, at) + '.' + digits.slice(at);
  return (negative ? '-' : '') + fixed;
}

// The hash byte of an object key: XXH3-64 of the key up to its last '.'.
function hash8(bytes: Uint8Array): number {
  const dot = bytes.lastIndexOf(46);
  return Number(xxh3(dot < 0 ? bytes : bytes.subarray(0, dot)) & 0xffn);
}
function encode(value: PjsonValue, depth = 0): Uint8Array {
  require(depth <= 64, 'Maximum nesting depth exceeded');
  if (value === null) return Uint8Array.of(0);
  if (typeof value === 'boolean') return Uint8Array.of(value ? 0x11 : 0x10);
  if (typeof value === 'bigint') return integer(value);
  if (typeof value === 'number') return number(value);
  if (typeof value === 'string') return join([Uint8Array.of(0x90), utf8.encode(value)]);
  if (value instanceof PjsonString) return join([Uint8Array.of(value.escaped ? 0x91 : 0x90), value.bytes]);
  if (value instanceof PjsonDecimal) return decimal(value.mantissa, value.scale);
  if (value instanceof PjsonFloat) return ieee(value);
  if (value instanceof Uint8Array) return join([Uint8Array.of(0xa0), value]);
  if (value instanceof PjsonTypedArray) {
    const size = sizes[value.code];
    require(size && value.bytes.length % size === 0, 'Invalid typed array');
    return join([Uint8Array.of(0xb1 + value.code), value.bytes, le(value.bytes.length / size, 2)]);
  }
  if (value instanceof PjsonRows) return encodeRows(value.records, depth, value.keys);
  const array = Array.isArray(value);
  const entries: [string | PjsonString, PjsonValue][] = array ? value.map(v => ['', v]) :
    value instanceof PjsonObject ? value.entries :
    value instanceof Map ? [...value.entries()] : Object.entries(value);
  require(entries.length <= 65535, 'Container exceeds 65535 entries');
  const parts: Uint8Array[] = [], offsets: number[] = [], keys: Uint8Array[] = [];
  let length = 0;
  for (const [key, child] of entries) {
    offsets.push(length);
    const k = key instanceof PjsonString ? key.bytes : utf8.encode(key); keys.push(k);
    const prefix = array ? new Uint8Array() :
      k.length < 255 ? k : join([varuint(k.length - 255), k]);
    const part = join([prefix, encode(child, depth + 1)]);
    parts.push(part); length += part.length;
  }
  const w = width(length);
  return join([Uint8Array.of(array ? 0xb0 : 0xc0, w - 1), ...parts,
    array ? new Uint8Array() : Uint8Array.from(keys.map(hash8)),
    ...offsets.map((off, i) => array ? le(off, w) : join([le(off, w), Uint8Array.of(Math.min(keys[i].length, 255))])),
    le(entries.length, 2)]);
}
function encodeRows(records: (Map<string, PjsonValue> | PjsonObject)[], depth: number,
                    schema?: (string | PjsonString)[]): Uint8Array {
  require(records.length <= 65535, 'Invalid row count');
  const entries = records.map(r => r instanceof PjsonObject ? r.entries : [...r.entries()]);
  const names = entries.length ? entries[0].map(([k]) => k) : schema ?? [];
  const keys = names.map(k => k instanceof PjsonString ? k.bytes : utf8.encode(k));
  require(keys.length > 0 && keys.every(k => k.length < 255), 'Invalid row schema');
  const rows = entries.map(r => {
    require(r.length === keys.length && r.every(([k], i) => {
      const bytes = k instanceof PjsonString ? k.bytes : utf8.encode(k);
      return bytes.length === keys[i].length && bytes.every((b, j) => b === keys[i][j]);
    }), 'Mismatched row schema');
    return r.map(([, v]) => encode(v, depth + 1));
  });
  const sw = width(rows.reduce((max, row) => Math.max(max, row.reduce((n, v) => n + v.length, 0)), 0));
  const bodies = rows.map(row => {
    let pos = 0; const offsets = row.map(v => { const at = pos; pos += v.length; return le(at, sw); });
    return join([...row, ...offsets]);
  });
  let total = 0; const offsets = bodies.map(b => { const at = total; total += b.length; return at; });
  const rw = width(total); let keyOffset = 0;
  const slots = keys.map(k => { const at = keyOffset; keyOffset += k.length;
    require(keyOffset <= 0xffffff, 'Row keys exceed 24-bit offsets');
    return join([le(at, 3), Uint8Array.of(k.length)]); });
  return join([Uint8Array.of(0xc1, (sw - 1) | ((rw - 1) << 2)), varuint(keys.length),
    ...slots, Uint8Array.from(keys.map(hash8)), ...keys, ...bodies,
    ...offsets.map(o => le(o, rw)), le(records.length, 2)]);
}
function decodeKey(bytes: Uint8Array): string | PjsonString {
  try { return text.decode(bytes); } catch { return new PjsonString(bytes.slice()); }
}
function decode(data: Uint8Array, depth = 0): PjsonValue {
  require(depth <= 64 && data.length > 0, 'Empty value or maximum depth exceeded');
  const low = data[0] & 15, tag = data[0] >> 4, len = data.length;
  if (tag === 0) { require(len === 1 && low === 0, 'Invalid null'); return null; }
  if (tag === 1) { require(len === 1 && low <= 1, 'Invalid bool'); return low === 1; }
  if (tag === 2) { require(len === 1, 'Invalid inline uint'); return BigInt(low); }
  if (tag === 3) { require(len === 1 && low !== 0, 'Invalid inline negative integer'); return -BigInt(low); }
  if (tag === 4 || tag === 5) {
    require(len === low + 2, 'Invalid integer size');
    const n = read(data, 1, low + 1);
    require(tag !== 5 || n !== 0n, 'Negative zero is reserved');
    return tag === 5 ? -n : n;
  }
  if (tag === 7) {
    const zz = read(data, 1, low + 1), [scale, consumed] = readVar(data, low + 2);
    require(len === low + 2 + consumed, 'Trailing decimal bytes');
    return new PjsonDecimal((zz >> 1n) ^ -(zz & 1n), scale % 2 ? -(scale + 1) / 2 : scale / 2);
  }
  if (tag === 6) {
    const w = low & 7; require(w >= 1 && w <= 3 && len === 1 + (1 << w), 'Invalid IEEE value');
    return new PjsonFloat(read(data, 1, 1 << w), w as 1 | 2 | 3, !!(low & 8));
  }
  if (tag === 9) {
    require(low <= 1, 'Reserved string flag');
    // Preserve escape-form and invalid UTF-8 strings losslessly.
    if (low) return new PjsonString(data.slice(1), true);
    try { return text.decode(data.subarray(1)); } catch { return new PjsonString(data.slice(1)); }
  }
  if (tag === 10) { require(low === 0, 'Reserved bytes flag'); return data.slice(1); }
  if (tag === 11 && low !== 0) {
    require(low <= 10 && len >= 3, 'Invalid typed array');
    require(len === 3 + Number(read(data, len - 2, 2)) * sizes[low - 1], 'Invalid typed array size');
    return new PjsonTypedArray(low - 1, data.slice(1, -2));
  }
  if (tag === 12 && low === 1) return decodeRows(data, depth);
  require((tag === 11 || tag === 12) && low === 0 && len >= 4, 'Reserved tag or short container');
  require((data[1] & 0xfc) === 0, 'Reserved container width bits');
  const object = tag === 12, w = data[1] + 1, stride = w + (object ? 1 : 0);
  const count = Number(read(data, len - 2, 2)), table = len - 2 - count * stride;
  const end = table - (object ? count : 0); require(end >= 2, 'Truncated container index');
  const children: PjsonValue[] = [], entries: [string | PjsonString, PjsonValue][] = [];
  for (let i = 0; i < count; ++i) {
    const at = 2 + Number(read(data, table + i * stride, w));
    const next = i + 1 === count ? end : 2 + Number(read(data, table + (i + 1) * stride, w));
    require(at <= next && next <= end, 'Invalid child offset');
    let start = at;
    let key: string | PjsonString = '';
    if (object) {
      let size = data[table + i * stride + w];
      if (size === 255) { const [excess, used] = readVar(data.subarray(0, next), start); size += excess; start += used; }
      require(start + size <= next, 'Truncated object key');
      const bytes = data.subarray(start, start + size);
      require(hash8(bytes) === data[end + i], 'Object key hash mismatch');
      key = decodeKey(bytes); start += size;
    }
    const child = decode(data.subarray(start, next), depth + 1);
    if (object) entries.push([key, child]); else children.push(child);
  }
  return object ? new PjsonObject(entries) : children;
}
function decodeRows(data: Uint8Array, depth: number): PjsonRows {
  const len = data.length;
  require(len >= 5 && (data[1] & 0xf0) === 0, 'Invalid row array header');
  const sw = (data[1] & 3) + 1, rw = ((data[1] >> 2) & 3) + 1;
  const [k, used] = readVar(data, 2), slots = 2 + used, hashes = slots + k * 4, keysAt = hashes + k;
  require(k > 0 && keysAt <= len - 2, 'Invalid row schema');
  const specs: [number, number][] = [];
  for (let j = 0; j < k; ++j) {
    const off = Number(read(data, slots + j * 4, 3)), size = data[slots + j * 4 + 3];
    require(size < 255, 'Unsupported long row key'); specs.push([off, size]);
  }
  const keysSize = specs[k - 1][0] + specs[k - 1][1], start = keysAt + keysSize;
  const count = Number(read(data, len - 2, 2)), table = len - 2 - count * rw;
  require(start <= table, 'Truncated row body');
  const names = specs.map(([off, size], j) => {
    require(off + size <= keysSize, 'Invalid row key offset');
    const bytes = data.subarray(keysAt + off, keysAt + off + size);
    require(hash8(bytes) === data[hashes + j], 'Row key hash mismatch'); return decodeKey(bytes);
  });
  const records: PjsonObject[] = [];
  for (let i = 0; i < count; ++i) {
    const at = start + Number(read(data, table + i * rw, rw));
    const next = i + 1 === count ? table : start + Number(read(data, table + (i + 1) * rw, rw));
    const fields = next - k * sw;
    require(at <= fields && next <= table, 'Invalid row offset');
    const record: [string | PjsonString, PjsonValue][] = [];
    for (let j = 0; j < k; ++j) {
      const off = at + Number(read(data, fields + j * sw, sw));
      const end = j + 1 === k ? fields : at + Number(read(data, fields + (j + 1) * sw, sw));
      require(off <= end && end <= fields, 'Invalid row field offset');
      record.push([names[j], decode(data.subarray(off, end), depth + 1)]);
    }
    records.push(new PjsonObject(record));
  }
  return new PjsonRows(records, names);
}
// ── The canonical form (docs/pjson-spec.md §15; gofractally/psio#17) ────────
//
// One encoding per value, a function of the VALUE alone (C++
// psio/pjson_number_model.hpp and psio/pjson_canonical.hpp):
//
//   number   an integer below 2^128 in magnitude: the integer forms with the
//            fewest bytes. Any other finite number: the smallest of decimal
//            (no trailing zeros), binary32 and binary64 that carries it; a
//            tie goes to the float, the narrower first. -0, NaN, ±Infinity:
//            binary16.
//   string   raw text, valid UTF-8.
//   array    empty: the generic empty array. All integers of at most 64
//            bits: a typed array of the narrowest code. Else all numbers
//            binary32 carries: typed f32; else binary64: typed f64. Else all
//            objects with the same names in the same order: a row array.
//            Else generic.
//   object   generic; order and repeated names kept.

/** What a float stands for: THE switch point (C++ `pjson_detail::float_model`).
 *  DEFAULT CHOSEN, AWAITING OWNER (gofractally/psio#17, comment 7208961356).
 *  `declared-width`: the shortest decimal that reads back to the float at its
 *  own width, so a binary32 holding 0.1f is 0.1. `binary64`: the float is
 *  widened first, so it is 0.10000000149011612. */
export const PJSON_FLOAT_MODEL: 'declared-width' | 'binary64' = 'declared-width';

/** A pjson number: (-1)^neg * mag * 10^scale, or one of the specials. */
interface Num { kind: 'finite' | '-0' | 'nan' | '+inf' | '-inf'; neg: boolean; mag: bigint; scale: number; }
const ZERO: Num = { kind: 'finite', neg: false, mag: 0n, scale: 0 };
function finite(neg: boolean, mag: bigint, scale: number): Num {
  if (mag === 0n) return ZERO;
  while (mag % 10n === 0n) { mag /= 10n; ++scale; }
  return { kind: 'finite', neg, mag, scale };
}
function sameNum(a: Num, b: Num): boolean {
  return a.kind === b.kind && a.neg === b.neg && a.mag === b.mag && a.scale === b.scale;
}
/** mag * 10^scale as an integer below 2^128, when it is one. */
function wholeOf(n: Num): bigint | undefined {
  if (n.kind !== 'finite' || n.scale < 0) return undefined;
  if (n.scale > 38) return n.mag === 0n ? 0n : undefined;
  const v = n.mag * 10n ** BigInt(n.scale);
  return v <= max128 ? v : undefined;
}
/** "d.ddde±x" (toExponential) as a number. */
function parseExponential(s: string): Num {
  const neg = s.startsWith('-');
  const [digits, exp] = (neg ? s.slice(1) : s).split('e');
  const point = digits.indexOf('.');
  return finite(neg, BigInt(digits.replace('.', '')), Number(exp) - (point < 0 ? 0 : digits.length - point - 1));
}
function special(x: number): Num | undefined {
  if (Number.isNaN(x)) return { ...ZERO, kind: 'nan' };
  if (!Number.isFinite(x)) return { ...ZERO, kind: x > 0 ? '+inf' : '-inf' };
  if (x === 0) return Object.is(x, -0) ? { ...ZERO, kind: '-0' } : ZERO;
  return undefined;
}
/** The number a binary64 stands for: its shortest digits. */
function doubleReadsAs(x: number): Num {
  return special(x) ?? parseExponential(x.toExponential());
}
/** The number a binary32 stands for (`x` is exactly a binary32). */
function floatReadsAs(x: number): Num {
  const s = special(x);
  if (s) return s;
  if (PJSON_FLOAT_MODEL === 'binary64') return parseExponential(x.toExponential());
  // The fewest digits that read back to this binary32, and of those the
  // decimal nearest it. (The value rounded to that many digits, or the
  // decimal one step either side of it: below a power of two the interval
  // that reads back is lopsided, and the rounded value can fall outside it
  // while its neighbour does not.)
  for (let digits = 0; digits < 9; ++digits) {
    const [mantissa, exp] = x.toExponential(digits).split('e');
    const m = BigInt(mantissa.replace('.', '')), scale = Number(exp) - digits;
    let best: bigint | undefined, distance = Infinity;
    for (const candidate of [m, m + (m < 0n ? -1n : 1n), m - (m < 0n ? -1n : 1n)]) {
      const value = Number(`${candidate}e${scale}`);
      if (Math.fround(value) === x && Math.abs(value - x) < distance) { best = candidate; distance = Math.abs(value - x); }
    }
    if (best !== undefined) return finite(best < 0n, best < 0n ? -best : best, scale);
  }
  return parseExponential(x.toExponential(8));
}
const scratch = new DataView(new ArrayBuffer(8));
function stepFloat(x: number, up: boolean): number {
  scratch.setFloat32(0, x, true);
  const bits = scratch.getUint32(0, true), positive = x > 0;
  scratch.setUint32(0, positive === up ? bits + 1 : bits - 1, true);
  return scratch.getFloat32(0, true);
}
/** The binary64 that carries the number, if one does. */
function carriedByDouble(n: Num): number | undefined {
  if (n.kind === '-0') return -0;
  if (n.kind === 'nan') return NaN;
  if (n.kind === '+inf') return Infinity;
  if (n.kind === '-inf') return -Infinity;
  if (n.mag === 0n) return 0;
  if (n.mag.toString().length > 17) return undefined;
  const x = Number(`${n.neg ? '-' : ''}${n.mag}e${n.scale}`);
  return Number.isFinite(x) && x !== 0 && sameNum(doubleReadsAs(x), n) ? x : undefined;
}
/** The binary32 that carries the number, if one does. */
function carriedByFloat(n: Num): number | undefined {
  if (n.kind !== 'finite' || n.mag === 0n) { const d = carriedByDouble(n); return d === undefined ? d : Math.fround(d); }
  if (PJSON_FLOAT_MODEL === 'binary64') {
    const wide = carriedByDouble(n);
    return wide !== undefined && Math.fround(wide) === wide ? wide : undefined;
  }
  if (n.mag.toString().length > 9) return undefined;
  // The binary32 nearest the decimal. Rounding the nearest binary64 again
  // can land on a neighbour, so the neighbours are tried too.
  const x = Math.fround(Number(`${n.neg ? '-' : ''}${n.mag}e${n.scale}`));
  if (!Number.isFinite(x) || x === 0) return undefined;
  for (const candidate of [x, stepFloat(x, true), stepFloat(x, false)])
    if (Number.isFinite(candidate) && candidate !== 0 && sameNum(floatReadsAs(candidate), n)) return candidate;
  return undefined;
}
function halfToNumber(bits: number): number {
  const sign = bits & 0x8000 ? -1 : 1, exp = (bits >> 10) & 31, frac = bits & 1023;
  if (exp === 0) return sign * frac * 2 ** -24;
  if (exp === 31) return frac ? NaN : sign * Infinity;
  return sign * (1 + frac / 1024) * 2 ** (exp - 15);
}
function numOfFloat(value: PjsonFloat): Num {
  if (value.width === 3) { scratch.setBigUint64(0, value.bits, true); return doubleReadsAs(scratch.getFloat64(0, true)); }
  if (value.width === 2) { scratch.setUint32(0, Number(value.bits), true); return floatReadsAs(scratch.getFloat32(0, true)); }
  return floatReadsAs(halfToNumber(Number(value.bits)));   // a binary32 holds every binary16
}
/** The number of a numeric value, or undefined when the value is not one. */
function numOf(value: PjsonValue): Num | undefined {
  if (typeof value === 'bigint') {
    require(value >= -max128 && value <= max128, 'Integer exceeds 128 bits');
    return finite(value < 0n, value < 0n ? -value : value, 0);
  }
  if (typeof value === 'number') return doubleReadsAs(value);
  if (value instanceof PjsonDecimal) return finite(value.mantissa < 0n, value.mantissa < 0n ? -value.mantissa : value.mantissa, value.scale);
  if (value instanceof PjsonFloat) return numOfFloat(value);
  return undefined;
}
function floatBytes(x: number, bytes: 4 | 8): Uint8Array {
  const out = new Uint8Array(bytes), view = new DataView(out.buffer);
  if (Number.isNaN(x)) { if (bytes === 4) view.setUint32(0, 0x7fc00000, true); else view.setBigUint64(0, 0x7ff8000000000000n, true); }
  else if (bytes === 4) view.setFloat32(0, x, true); else view.setFloat64(0, x, true);
  return out;
}
/** The one canonical encoding of a number. */
function canonicalNumber(n: Num): Uint8Array {
  if (n.kind === '-0') return Uint8Array.of(0x61, 0x00, 0x80);
  if (n.kind === 'nan') return Uint8Array.of(0x61, 0x00, 0x7e);
  if (n.kind === '+inf') return Uint8Array.of(0x61, 0x00, 0x7c);
  if (n.kind === '-inf') return Uint8Array.of(0x61, 0x00, 0xfc);
  const whole = wholeOf(n);
  if (whole !== undefined) return integer(n.neg ? -whole : whole);
  const asDecimal = n.mag < (1n << 127n) ? decimal(n.neg ? -n.mag : n.mag, n.scale) : undefined;
  const size = asDecimal ? asDecimal.length : Infinity;
  if (size >= 5) { const f = carriedByFloat(n); if (f !== undefined) return join([Uint8Array.of(0x62), floatBytes(f, 4)]); }
  if (size >= 9) { const d = carriedByDouble(n); if (d !== undefined) return join([Uint8Array.of(0x63), floatBytes(d, 8)]); }
  require(asDecimal, 'A number has no canonical form');
  return asDecimal;
}
/** The canonical array of the numbers, or undefined when a typed array does not hold them. */
function numberArray(numbers: Num[]): Uint8Array | undefined {
  let integers = true, anyNegative = false, maxPositive = 0n, maxNegative = 0n;
  const wholes: bigint[] = [];
  for (const n of numbers) {
    const whole = wholeOf(n);
    if (whole === undefined || whole >> 64n || (n.neg && whole > (1n << 63n))) { integers = false; break; }
    wholes.push(n.neg ? -whole : whole);
    if (n.neg && whole) { anyNegative = true; if (whole > maxNegative) maxNegative = whole; }
    else if (whole > maxPositive) maxPositive = whole;
  }
  const count = le(numbers.length, 2);
  if (integers) {
    let code = -1;
    if (!anyNegative) code = maxPositive <= 0xffn ? 4 : maxPositive <= 0xffffn ? 5 : maxPositive <= 0xffffffffn ? 6 : 7;
    else {
      const fits = (bits: bigint) => maxPositive <= (1n << (bits - 1n)) - 1n && maxNegative <= (1n << (bits - 1n));
      code = fits(8n) ? 0 : fits(16n) ? 1 : fits(32n) ? 2 : maxPositive <= (1n << 63n) - 1n ? 3 : -1;
    }
    if (code >= 0) {
      const w = sizes[code];
      return join([Uint8Array.of(0xb1 + code), ...wholes.map(v => le(v < 0n ? v + (1n << BigInt(8 * w)) : v, w)), count]);
    }
  }
  const narrow = numbers.map(carriedByFloat);
  if (narrow.every(f => f !== undefined)) return join([Uint8Array.of(0xb9), ...narrow.map(f => floatBytes(f!, 4)), count]);
  const wide = numbers.map(carriedByDouble);
  if (wide.every(d => d !== undefined)) return join([Uint8Array.of(0xba), ...wide.map(d => floatBytes(d!, 8)), count]);
  return undefined;
}
function typedElements(value: PjsonTypedArray): Num[] {
  const size = sizes[value.code], out: Num[] = [];
  require(size && value.bytes.length % size === 0, 'Invalid typed array');
  const view = new DataView(value.bytes.buffer, value.bytes.byteOffset, value.bytes.length);
  for (let at = 0; at < value.bytes.length; at += size) {
    if (value.code === 8) out.push(floatReadsAs(view.getFloat32(at, true)));
    else if (value.code === 9) out.push(doubleReadsAs(view.getFloat64(at, true)));
    else {
      let v = read(value.bytes, at, size);
      if (value.code < 4 && v >> BigInt(8 * size - 1)) v -= 1n << BigInt(8 * size);
      out.push(finite(v < 0n, v < 0n ? -v : v, 0));
    }
  }
  return out;
}
// JSON escape form -> the text. A lone surrogate, a bad escape, or a quote
// or control character that may not stand unescaped is not text.
function unescapeText(bytes: Uint8Array): Uint8Array {
  const out: number[] = [];
  const hex4 = (at: number) => {
    require(at + 4 <= bytes.length, 'A string is not text: it has no canonical form');
    const digits = String.fromCharCode(...bytes.subarray(at, at + 4));
    require(/^[0-9a-fA-F]{4}$/.test(digits), 'A string is not text: it has no canonical form');
    return parseInt(digits, 16);
  };
  for (let i = 0; i < bytes.length;) {
    const c = bytes[i];
    require(c !== 0x22 && c >= 0x20, 'A string is not text: it has no canonical form');
    if (c !== 0x5c) { out.push(c); ++i; continue; }
    const e = bytes[++i]; ++i;
    const simple: { [k: number]: number } = { 0x22: 0x22, 0x5c: 0x5c, 0x2f: 0x2f, 0x62: 8, 0x66: 12, 0x6e: 10, 0x72: 13, 0x74: 9 };
    if (e in simple) { out.push(simple[e]); continue; }
    require(e === 0x75, 'A string is not text: it has no canonical form');
    let cp = hex4(i); i += 4;
    if (cp >= 0xd800 && cp <= 0xdbff) {
      require(bytes[i] === 0x5c && bytes[i + 1] === 0x75, 'A string is not text: it has no canonical form');
      const low = hex4(i + 2);
      require(low >= 0xdc00 && low <= 0xdfff, 'A string is not text: it has no canonical form');
      i += 6; cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
    } else require(cp < 0xdc00 || cp > 0xdfff, 'A string is not text: it has no canonical form');
    out.push(...utf8.encode(String.fromCodePoint(cp)));
  }
  return textBytes(Uint8Array.from(out));
}
function textBytes(bytes: Uint8Array): Uint8Array {
  try { text.decode(bytes); } catch { throw new PjsonError('A string is not UTF-8: it has no canonical form'); }
  return bytes;
}
function keyBytes(key: string | PjsonString): Uint8Array {
  return key instanceof PjsonString ? textBytes(key.bytes) : utf8.encode(key);
}
type Entries = [string | PjsonString, PjsonValue][];
/** The members of a value that is a single object, else undefined. */
function entriesOf(value: PjsonValue): Entries | undefined {
  if (value === null || typeof value !== 'object' || Array.isArray(value) || value instanceof Uint8Array ||
      value instanceof PjsonString || value instanceof PjsonDecimal || value instanceof PjsonFloat ||
      value instanceof PjsonTypedArray || value instanceof PjsonRows) return undefined;
  return value instanceof PjsonObject ? value.entries : value instanceof Map ? [...value.entries()] : Object.entries(value);
}
function sameBytes(a: Uint8Array, b: Uint8Array): boolean {
  return a.length === b.length && a.every((v, i) => v === b[i]);
}
/** A row array of the records (at least one), each a list of members with the same names. */
function canonicalRows(records: Entries[], depth: number): Uint8Array {
  require(records.length <= 65535, 'Container exceeds 65535 entries');
  const keys = records[0].map(([k]) => keyBytes(k));
  const rows = records.map(r => r.map(([, v]) => canonical(v, depth + 1)));
  const sw = width(rows.reduce((max, row) => Math.max(max, row.reduce((n, v) => n + v.length, 0)), 0));
  const bodies = rows.map(row => {
    let pos = 0; const offsets = row.map(v => { const at = pos; pos += v.length; return le(at, sw); });
    return join([...row, ...offsets]);
  });
  let total = 0; const offsets = bodies.map(b => { const at = total; total += b.length; return at; });
  const rw = width(total); let keyOffset = 0;
  const slots = keys.map(k => { const at = keyOffset; keyOffset += k.length;
    require(keyOffset <= 0xffffff, 'Row keys exceed 24-bit offsets');
    return join([le(at, 3), Uint8Array.of(k.length)]); });
  return join([Uint8Array.of(0xc1, (sw - 1) | ((rw - 1) << 2)), varuint(keys.length),
    ...slots, Uint8Array.from(keys.map(hash8)), ...keys, ...bodies,
    ...offsets.map(o => le(o, rw)), le(records.length, 2)]);
}
const EMPTY_ARRAY = Uint8Array.of(0xb0, 0, 0, 0);
function canonicalArray(elements: PjsonValue[], depth: number): Uint8Array {
  require(elements.length <= 65535, 'Container exceeds 65535 entries');
  if (elements.length === 0) return EMPTY_ARRAY;
  const numbers = elements.map(numOf);
  if (numbers.every(n => n !== undefined)) {
    const typed = numberArray(numbers as Num[]);
    if (typed) return typed;
  }
  const records = elements.map(entriesOf);
  if (records.every(r => r !== undefined)) {
    const first = records[0]!.map(([k]) => keyBytes(k));
    const same = first.length > 0 && first.every(k => k.length < 255) && records.every(r =>
      r!.length === first.length && r!.every(([k], i) => sameBytes(keyBytes(k), first[i])));
    if (same) return canonicalRows(records as Entries[], depth);
  }
  const parts = elements.map(e => canonical(e, depth + 1));
  let length = 0; const offsets = parts.map(p => { const at = length; length += p.length; return at; });
  const w = width(length);
  return join([Uint8Array.of(0xb0, w - 1), ...parts, ...offsets.map(o => le(o, w)), le(elements.length, 2)]);
}
function canonical(value: PjsonValue, depth = 0): Uint8Array {
  require(depth <= 64, 'Maximum nesting depth exceeded');
  if (value === null) return Uint8Array.of(0);
  if (typeof value === 'boolean') return Uint8Array.of(value ? 0x11 : 0x10);
  const n = numOf(value);
  if (n) return canonicalNumber(n);
  if (typeof value === 'string') return join([Uint8Array.of(0x90), utf8.encode(value)]);
  if (value instanceof PjsonString)
    return join([Uint8Array.of(0x90), value.escaped ? unescapeText(value.bytes) : textBytes(value.bytes)]);
  if (value instanceof Uint8Array) return join([Uint8Array.of(0xa0), value]);
  if (value instanceof PjsonTypedArray) {
    const elements = typedElements(value);
    if (elements.length === 0) return EMPTY_ARRAY;
    const typed = numberArray(elements);
    require(typed, 'A typed array element is not a number a typed array holds');
    return typed;
  }
  if (value instanceof PjsonRows) {
    if (value.records.length === 0) return EMPTY_ARRAY;
    return canonicalRows(value.records.map(r => r instanceof PjsonObject ? r.entries : [...r.entries()]), depth);
  }
  if (Array.isArray(value)) return canonicalArray(value, depth);
  const entries = entriesOf(value)!;
  require(entries.length <= 65535, 'Container exceeds 65535 entries');
  const parts: Uint8Array[] = [], offsets: number[] = [], keys: Uint8Array[] = [];
  let length = 0;
  for (const [key, child] of entries) {
    offsets.push(length);
    const k = keyBytes(key); keys.push(k);
    const part = join([k.length < 255 ? k : join([varuint(k.length - 255), k]), canonical(child, depth + 1)]);
    parts.push(part); length += part.length;
  }
  const w = width(length);
  return join([Uint8Array.of(0xc0, w - 1), ...parts, Uint8Array.from(keys.map(hash8)),
    ...offsets.map((off, i) => join([le(off, w), Uint8Array.of(Math.min(keys[i].length, 255))])),
    le(entries.length, 2)]);
}

// Every public entry point throws PjsonError and nothing else.
function guarded<T>(run: () => T): T {
  try {
    return run();
  } catch (e) {
    if (e instanceof PjsonError) throw e;
    throw new PjsonError(e instanceof Error ? `${e.name}: ${e.message}` : String(e));
  }
}
function input(data: unknown): Uint8Array {
  if (!(data instanceof Uint8Array)) throw new PjsonError('pjson: input must be a Uint8Array');
  return data.constructor === Uint8Array ? data : new Uint8Array(data.buffer, data.byteOffset, data.length);
}

/** The pjson bytes of `value`: the canonical form of the value it is. An
 *  array of numbers is a typed array, an escaped `PjsonString` is its text,
 *  and so on, whichever classes the value was built from. */
export function pjsonEncode(value: PjsonValue): Uint8Array {
  return guarded(() => canonical(value));
}
/** Decodes pjson bytes: the canonical frame of a value, nothing else (see
 *  `pjsonValidate`). `pjsonLazy.decode` reads any well-formed frame. */
export function pjsonDecode(data: Uint8Array): PjsonValue {
  return guarded(() => {
    const value = decode(input(data));
    require(sameBytes(canonical(value), input(data)), 'A well-formed frame that is not the canonical encoding of its value');
    return value;
  });
}
/** Whether `data` is pjson: the canonical encoding of its value. Never
 *  throws. A well-formed frame that is not canonical is refused here and
 *  accepted by `pjsonLazy.validate`. */
export function pjsonValidate(data: Uint8Array): boolean {
  try {
    const bytes = input(data);
    return sameBytes(canonical(decode(bytes)), bytes);
  } catch {
    return false;
  }
}
/** The canonical frame of a well-formed one. Throws a `PjsonError` when the
 *  bytes are not a frame, or hold text that is not UTF-8. */
export function pjsonCanonicalize(data: Uint8Array): Uint8Array {
  return guarded(() => canonical(decode(input(data))));
}
/** C++ `psio::pjson_lazy`: any well-formed frame. The one non-canonical
 *  path (what a quick JSON transcode writes); `encode` writes a value in the
 *  forms its classes name. */
export const pjsonLazy = {
  encode(value: PjsonValue): Uint8Array {
    return guarded(() => encode(value));
  },
  decode(data: Uint8Array): PjsonValue {
    return guarded(() => decode(input(data)));
  },
  validate(data: Uint8Array): boolean {
    try {
      decode(input(data));
      return true;
    } catch {
      return false;
    }
  },
};
