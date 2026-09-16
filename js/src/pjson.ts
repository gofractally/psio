// Wire contract: docs/wire-contract.md; audited tags, current public C++ semantics.
export const PJSON_WIRE_REVISION = 2;
import { createXXHash3 } from 'hash-wasm';

export class PjsonError extends Error {}
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

/** Initialize XXH3 once. The returned codec has synchronous operations. */
export async function createPjson() {
  const hash = await createXXHash3();
  function hash8(bytes: Uint8Array): number {
    const dot = bytes.lastIndexOf(46);
    hash.init().update(dot < 0 ? bytes : bytes.subarray(0, dot));
    return parseInt(hash.digest('hex').slice(-2), 16);
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
    if (tag === 0) { require(len === 1, 'Invalid null'); return null; }
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
  return { encode: (v: PjsonValue) => encode(v), decode: (data: Uint8Array) => decode(data),
    validate: (data: Uint8Array): boolean => { try { decode(data); return true; } catch { return false; } } };
}
