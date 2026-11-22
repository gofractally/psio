// pssz-key.ts — the `key` format (docs/key-spec.md), schema-driven: byte
// strings whose bytewise order is the value's order. The counterpart of C++
// `psio::key` (psio/key.hpp), over the same nodes and the same JavaScript
// values as the PSSZ codec.
//
// This file is internal. The public API is `pssz.ts`.

import {
  MAX_DEPTH, Node, PsszError, PsszNaN, memberBoundAdmits, setField, unwrapOption, utf8Decode, utf8Encode,
  utf8Valid, wellFormed, wrapSome,
  type BitsValue, type VariantValue,
} from './pssz-core.js';

function valueError(message: string): never {
  throw new PsszError('value', message);
}
function bytesError(message: string): never {
  throw new PsszError('bytes', message);
}
function schemaError(message: string): never {
  throw new PsszError('schema', message);
}

const QUIET_NAN_32 = 0x7fc00000n;
const QUIET_NAN_64 = 0x7ff8000000000000n;

// A list whose elements are single key bytes shares the string framing.
function octetList(element: Node): boolean {
  return element.kind === 'char' || (element.kind === 'int' && element.bits === 8 && !element.time);
}

// The byte container under a string / hex adapter admits `n` bytes.
function containerAdmits(container: Node, n: number): boolean {
  if (container.kind === 'array') return n === container.countNum;
  return !container.bounded || n <= container.countNum;
}

// ── Writing ─────────────────────────────────────────────────────────────────

class Sink {
  private buf = new Uint8Array(64);
  private len = 0;

  private grow(n: number): void {
    if (this.len + n <= this.buf.length) return;
    const next = new Uint8Array(Math.max(this.buf.length * 2, this.len + n));
    next.set(this.buf.subarray(0, this.len));
    this.buf = next;
  }
  byte(b: number): void {
    this.grow(1);
    this.buf[this.len++] = b;
  }
  bytes(b: Uint8Array): void {
    this.grow(b.length);
    this.buf.set(b, this.len);
    this.len += b.length;
  }
  /** `v` (already reduced to `size` bytes, unsigned) big-endian. */
  bigEndian(v: bigint, size: number): void {
    this.grow(size);
    for (let i = size - 1; i >= 0; --i) {
      this.buf[this.len + i] = Number(v & 0xffn);
      v >>= 8n;
    }
    this.len += size;
  }
  /** Bytes with 00 → 00 01, then the terminator 00 00. */
  escaped(b: Uint8Array, flip = 0): void {
    this.grow(b.length + 2);
    for (let i = 0; i < b.length; ++i) {
      const k = b[i] ^ flip;
      this.byte(k);
      if (k === 0) this.byte(1);
    }
    this.byte(0);
    this.byte(0);
  }
  finish(): Uint8Array {
    return this.buf.slice(0, this.len);
  }
}

function writeKey(node: Node, value: unknown, out: Sink, n: number): void {
  // A decoded value is at most MAX_DEPTH nodes deep; far beyond is a cycle.
  if (n > 4 * MAX_DEPTH) valueError('key: node depth limit exceeded');
  switch (node.kind) {
    case 'int': {
      if (node.bits === 1) return schemaError('key: a one-bit integer has no key encoding');
      let v: bigint;
      if (typeof value === 'bigint') v = value;
      else if (typeof value === 'number' && Number.isSafeInteger(value)) v = BigInt(value);
      else return valueError('key: integer value required');
      const bits = BigInt(node.bits);
      if (node.signed) {
        const half = 1n << (bits - 1n);
        if (v < -half || v >= half) valueError('key: signed integer overflow');
        v += half; // the sign bit flipped
      } else if (v < 0n || v >= 1n << bits) valueError('key: unsigned integer overflow');
      return out.bigEndian(v, node.size);
    }
    case 'float': {
      const size = node.size;
      const scratch = new DataView(new ArrayBuffer(8));
      let bits: bigint;
      if (value instanceof PsszNaN) {
        if (value.size !== size) return valueError('key: NaN bits of the wrong float width');
        bits = value.bits;
      } else if (typeof value !== 'number') return valueError('key: float value required');
      else if (Number.isNaN(value)) bits = size === 4 ? QUIET_NAN_32 : QUIET_NAN_64;
      else if (size === 4) {
        scratch.setFloat32(0, value);
        bits = BigInt(scratch.getUint32(0));
      } else {
        scratch.setFloat64(0, value);
        bits = scratch.getBigUint64(0);
      }
      const width = BigInt(size * 8);
      const sign = 1n << (width - 1n);
      if (bits === sign) bits = 0n; // -0 is +0
      bits = bits & sign ? BigInt.asUintN(size * 8, ~bits) : bits ^ sign;
      return out.bigEndian(bits, size);
    }
    case 'bool':
      if (typeof value !== 'boolean') return valueError('key: bool value required');
      return out.byte(value ? 1 : 0);
    case 'char':
      if (typeof value !== 'number' || !Number.isInteger(value) || value < 0 || value > 255)
        return valueError('key: char byte value required');
      return out.byte(value);
    case 'string': {
      if (typeof value !== 'string') return valueError('key: string value required');
      if (!wellFormed(value)) return valueError('key: string is not valid UTF-8');
      const bytes = utf8Encode(value);
      if (!containerAdmits(node.child, bytes.length)) valueError('key: string exceeds its bound');
      return node.child.kind === 'array' ? out.bytes(bytes) : out.escaped(bytes);
    }
    case 'hex':
      if (!(value instanceof Uint8Array)) return valueError('key: bytes value required');
      if (!containerAdmits(node.child, value.length)) valueError('key: bytes exceed their bound');
      return node.child.kind === 'array' ? out.bytes(value) : out.escaped(value);
    case 'bitvector': {
      const bits = value as BitsValue;
      if (
        typeof value !== 'object' || value === null || !(bits.bytes instanceof Uint8Array) ||
        bits.bitCount !== node.size || bits.bytes.length !== Math.ceil(node.size / 8)
      )
        return valueError('key: packed Bits value of the declared size required');
      if (node.size % 8 && bits.bytes[bits.bytes.length - 1] >> node.size % 8)
        valueError('key: packed bits above logical count');
      return out.bytes(bits.bytes);
    }
    case 'bitlist':
      return schemaError('key: bitlist has no key encoding');
    case 'option': {
      const [some, inner] = unwrapOption(node, value, valueError);
      if (!some) return out.byte(0);
      out.byte(1);
      return writeKey(node.child, inner, out, n + 1);
    }
    case 'list': {
      if (!Array.isArray(value)) return valueError('key: sequence value required');
      if (node.bounded && value.length > node.countNum) valueError('key: list exceeds its bound');
      const element = node.child;
      if (octetList(element)) {
        const signed = element.kind === 'int' && element.signed;
        const bytes = new Uint8Array(value.length);
        for (let i = 0; i < value.length; ++i) {
          const v: unknown = value[i];
          if (typeof v !== 'number' || !Number.isInteger(v) || (signed ? v < -128 || v > 127 : v < 0 || v > 255))
            return valueError('key: byte value out of range');
          bytes[i] = v & 0xff;
        }
        return out.escaped(bytes, signed ? 0x80 : 0);
      }
      for (const item of value) {
        out.byte(1);
        writeKey(element, item, out, n + 1);
      }
      return out.byte(0);
    }
    case 'array':
      if (!Array.isArray(value)) return valueError('key: sequence value required');
      if (value.length !== node.countNum) valueError('key: array length does not match the schema');
      for (const item of value) writeKey(node.child, item, out, n + 1);
      return;
    case 'tuple':
      if (!Array.isArray(value) || value.length !== node.members.length)
        return valueError('key: tuple value of the declared arity required');
      for (let i = 0; i < node.members.length; ++i) writeKey(node.members[i].node, value[i], out, n + 1);
      return;
    case 'record': {
      if (typeof value !== 'object' || value === null || Array.isArray(value))
        return valueError('key: record value required');
      const record = value as Record<string, unknown>;
      for (const member of node.members) writeMember(node, member.name, record[member.name], out, n);
      return;
    }
    case 'variant': {
      const v = value as VariantValue;
      if (typeof value !== 'object' || value === null || typeof v.type !== 'string')
        return valueError('key: variant value { type, value } required');
      const index = node.byName!.get(v.type);
      if (index === undefined) return valueError(`key: unknown variant alternative "${v.type}"`);
      out.byte(index);
      return writeKey(node.members[index].node, v.value, out, n + 1);
    }
  }
}

function writeMember(node: Node, name: string, value: unknown, out: Sink, n: number): void {
  const member = node.members[node.byName!.get(name)!];
  // An absent optional member may be omitted from the object.
  const v = value === undefined && member.node.kind === 'option' ? null : value;
  if (member.bound && !memberBoundAdmits(member.bound, member.node, v))
    valueError('key: member length bound violated');
  writeKey(member.node, v, out, n + 1);
}

/** The key bytes of `value`. */
export function encodeKey(node: Node, value: unknown): Uint8Array {
  const out = new Sink();
  writeKey(node, value, out, 0);
  return out.finish();
}

/** The key of the first `elements.length` elements / fields of a composite type. */
export function encodeKeyPrefix(node: Node, elements: unknown): Uint8Array {
  if (node.kind !== 'tuple' && node.kind !== 'record')
    schemaError('key: a prefix key needs a tuple or record type');
  if (!Array.isArray(elements)) return valueError('key: prefix elements must be an array');
  if (elements.length > node.members.length) valueError('key: more prefix elements than the key type has');
  const out = new Sink();
  for (let i = 0; i < elements.length; ++i) {
    if (node.kind === 'record') writeMember(node, node.members[i].name, elements[i], out, 0);
    else writeKey(node.members[i].node, elements[i], out, 1);
  }
  return out.finish();
}

/**
 * The exclusive upper bound of a prefix scan: the smallest byte string
 * greater than every byte string that starts with `prefix`; null when there
 * is none (an empty or all-FF prefix).
 */
export function keyPrefixUpperBound(prefix: Uint8Array): Uint8Array | null {
  if (!(prefix instanceof Uint8Array)) throw new PsszError('bytes', 'key: input must be a Uint8Array');
  let n = prefix.length;
  while (n > 0 && prefix[n - 1] === 0xff) --n;
  if (n === 0) return null;
  const out = new Uint8Array(prefix.subarray(0, n));
  ++out[n - 1];
  return out;
}

// ── Reading ─────────────────────────────────────────────────────────────────

class Source {
  p = 0;
  constructor(readonly b: Uint8Array) {}

  need(n: number): void {
    if (this.b.length - this.p < n) bytesError('key: truncated');
  }
  byte(): number {
    this.need(1);
    return this.b[this.p++];
  }
  bigEndian(size: number): bigint {
    this.need(size);
    let v = 0n;
    for (let i = 0; i < size; ++i) v = (v << 8n) | BigInt(this.b[this.p++]);
    return v;
  }
  /** Unescaped bytes up to the 00 00 terminator, which is consumed. */
  escaped(flip = 0): Uint8Array {
    const b = this.b;
    // First pass: find the terminator and the unescaped length.
    let q = this.p;
    let count = 0;
    for (;;) {
      if (q >= b.length) bytesError('key: unterminated string');
      if (b[q] !== 0) {
        ++q;
        ++count;
        continue;
      }
      if (q + 1 >= b.length) bytesError('key: unterminated string');
      if (b[q + 1] === 0) break;
      if (b[q + 1] !== 1) bytesError('key: invalid escape');
      q += 2;
      ++count;
    }
    const out = new Uint8Array(count);
    let at = 0;
    for (let i = this.p; i < q; ++i) {
      out[at++] = b[i] ^ flip;
      if (b[i] === 0) ++i;
    }
    this.p = q + 2;
    return out;
  }
}

function readKey(node: Node, src: Source, n: number): unknown {
  if (n > MAX_DEPTH) bytesError('key: node depth limit exceeded');
  switch (node.kind) {
    case 'int': {
      if (node.bits === 1) return schemaError('key: a one-bit integer has no key encoding');
      let v = src.bigEndian(node.size);
      if (node.signed) v -= 1n << BigInt(node.bits - 1);
      return node.size <= 4 && !node.time ? Number(v) : v;
    }
    case 'float': {
      const size = node.size;
      const sign = 1n << BigInt(size * 8 - 1);
      let bits = src.bigEndian(size);
      bits = bits & sign ? bits ^ sign : BigInt.asUintN(size * 8, ~bits);
      if (bits === sign) bytesError('key: negative zero is not canonical');
      const view = new DataView(new ArrayBuffer(8));
      let value: number;
      if (size === 4) {
        view.setUint32(0, Number(bits));
        value = view.getFloat32(0);
      } else {
        view.setBigUint64(0, bits);
        value = view.getFloat64(0);
      }
      if (Number.isNaN(value) && bits !== (size === 4 ? QUIET_NAN_32 : QUIET_NAN_64))
        return new PsszNaN(bits, size as 4 | 8);
      return value;
    }
    case 'bool': {
      const b = src.byte();
      if (b > 1) bytesError('key: invalid bool');
      return b === 1;
    }
    case 'char':
      return src.byte();
    case 'string': {
      let bytes: Uint8Array;
      if (node.child.kind === 'array') {
        src.need(node.child.countNum);
        bytes = src.b.slice(src.p, src.p + node.child.countNum);
        src.p += bytes.length;
      } else {
        bytes = src.escaped();
        if (!containerAdmits(node.child, bytes.length)) bytesError('key: string exceeds its bound');
      }
      if (!utf8Valid(bytes, 0, bytes.length)) bytesError('key: string is not valid UTF-8');
      return utf8Decode(bytes, 0, bytes.length);
    }
    case 'hex': {
      if (node.child.kind === 'array') {
        src.need(node.child.countNum);
        const bytes = new Uint8Array(src.b.subarray(src.p, src.p + node.child.countNum));
        src.p += bytes.length;
        return bytes;
      }
      const bytes = src.escaped();
      if (!containerAdmits(node.child, bytes.length)) bytesError('key: bytes exceed their bound');
      return bytes;
    }
    case 'bitvector': {
      const count = Math.ceil(node.size / 8);
      src.need(count);
      const bytes = new Uint8Array(src.b.subarray(src.p, src.p + count));
      src.p += count;
      if (node.size % 8 && bytes[count - 1] >> node.size % 8) bytesError('key: bitvector padding bits are set');
      const value: BitsValue = { bytes, bitCount: node.size };
      return value;
    }
    case 'bitlist':
      return schemaError('key: bitlist has no key encoding');
    case 'option': {
      const tag = src.byte();
      if (tag === 0) return null;
      if (tag !== 1) bytesError('key: invalid optional marker');
      return wrapSome(node, readKey(node.child, src, n + 1));
    }
    case 'list': {
      const element = node.child;
      if (octetList(element)) {
        const signed = element.kind === 'int' && element.signed;
        const bytes = src.escaped(signed ? 0x80 : 0);
        if (node.bounded && bytes.length > node.countNum) bytesError('key: list exceeds its bound');
        const out: number[] = new Array(bytes.length);
        for (let i = 0; i < bytes.length; ++i) out[i] = signed ? (bytes[i] << 24) >> 24 : bytes[i];
        return out;
      }
      const out: unknown[] = [];
      for (;;) {
        const tag = src.byte();
        if (tag === 0) return out;
        if (tag !== 1) bytesError('key: invalid list element marker');
        if (node.bounded && out.length >= node.countNum) bytesError('key: list exceeds its bound');
        out.push(readKey(element, src, n + 1));
      }
    }
    case 'array': {
      // Every element of a key type takes at least one byte unless the
      // element itself is empty; bound the work by the bytes available.
      const out: unknown[] = [];
      for (let i = 0; i < node.countNum; ++i) {
        const before = src.p;
        out.push(readKey(node.child, src, n + 1));
        if (src.p === before && node.countNum - i > 1 << 20)
          bytesError('key: zero-size array count exceeds the supported limit');
      }
      return out;
    }
    case 'tuple':
      return node.members.map((member) => readKey(member.node, src, n + 1));
    case 'record': {
      const out: Record<string, unknown> = {};
      for (const member of node.members) {
        const value = readKey(member.node, src, n + 1);
        if (member.bound && !memberBoundAdmits(member.bound, member.node, value))
          bytesError('key: member length bound violated');
        setField(out, member.name, value);
      }
      return out;
    }
    case 'variant': {
      const index = src.byte();
      if (index >= node.members.length) bytesError('key: variant index out of range');
      const member = node.members[index];
      const value: VariantValue = { type: member.name, value: readKey(member.node, src, n + 1) };
      return value;
    }
  }
}

/** Reads exactly one value spanning the whole buffer; canonical bytes only. */
export function decodeKey(node: Node, bytes: Uint8Array): unknown {
  const src = new Source(bytes);
  const value = readKey(node, src, 0);
  if (src.p !== bytes.length) bytesError('key: trailing bytes');
  return value;
}
