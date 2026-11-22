// pssz-json.ts — JSON (RFC 8259) and I-JSON (RFC 7493) text for a value known
// only by its Schema IR. A port of `cpp/include/psio/pssz_ir_json.hpp` over a
// port of `json_text_cursor` (docs/json.md).
//
// The text is read by the exact parser below, never by `JSON.parse`: integers
// are taken from their token text (so a 64-bit integer is exact), and the
// rules JSON.parse cannot see are enforced (`-0`, `1.0`, `1e2` and leading
// zeros are not integers; an integer or hex string must be written without
// escapes; a repeated key is read again and the last one wins; an I-JSON
// variant object has exactly one member; nesting stops at 64).
//
// A 128 / 256-bit integer is decimal (a string under ijson, a bare number
// under json; the legacy "0x" string is read). An Enum is its enumerator's
// name when the value is listed and its number otherwise. A non-finite float
// has no text. A raw string (Custom "string") holding bytes that are not
// UTF-8 has no text. A Custom id that is not part of the binary form is its
// handler's text as one JSON string, or its inner type's text without one.
//
// This file is internal. The public API is `pssz.ts`.

import {
  Codec, MAX_DEPTH, Node, PsszError, PsszNaN, isNoncharacter, memberBoundAdmits, setField, unwrapOption, utf8CodePoint, utf8Valid,
  utf8Decode, utf8Encode, utf8Length, utf8Multibyte, wellFormed, wrapSome,
  type IntForm, type IrEnumerator, type VariantValue,
} from './pssz-core.js';

import { tickTypeAdmits, timeTextOf, timeTicksOf } from './time.js';

export type JsonFormat = 'json' | 'ijson';

const SAFE_INTEGER = (1n << 53n) - 1n;

// ── Floats ──────────────────────────────────────────────────────────────────

const f64 = new Float64Array(1);
const u64 = new BigUint64Array(f64.buffer);
const f32 = new Float32Array(1);
const u32 = new Uint32Array(f32.buffer);

interface Decimal {
  /** Significant digits, no leading or trailing zero. */
  digits: string;
  /** Decimal exponent of the first digit (d.ddd x 10^exponent). */
  exponent: number;
}

/** The exact decimal expansion of a positive finite double. */
function exactDecimal(a: number): Decimal {
  f64[0] = a;
  const bits = u64[0];
  const biased = Number((bits >> 52n) & 0x7ffn);
  let mantissa = bits & ((1n << 52n) - 1n);
  let e = -1074;
  if (biased !== 0) {
    mantissa |= 1n << 52n;
    e = biased - 1075;
  }
  let text: string;
  let exponent: number;
  if (e >= 0) {
    text = (mantissa << BigInt(e)).toString();
    exponent = text.length - 1;
  } else {
    text = (mantissa * 5n ** BigInt(-e)).toString();
    exponent = text.length - 1 + e;
  }
  return { digits: text.replace(/0+$/, ''), exponent };
}

// "d.ddde[+-]x" -> digits and exponent.
function splitExponential(text: string): Decimal {
  const at = text.indexOf('e');
  return { digits: text.slice(0, at).replace('.', ''), exponent: Number(text.slice(at + 1)) };
}

// Shortest decimal that reads back as the same binary32, closest to it.
function shortestSingle(a: number): Decimal {
  for (let precision = 1; precision <= 9; ++precision) {
    const closest = splitExponential(a.toExponential(precision - 1));
    const reads = (digits: bigint) =>
      Math.fround(Number(`${digits}e${closest.exponent - (precision - 1)}`)) === a;
    const middle = BigInt(closest.digits);
    let best: bigint | null = null;
    if (reads(middle)) best = middle;
    else {
      // At a power of two the rounding interval is lopsided: the closest
      // decimal can fall outside it while a neighbour is inside.
      for (const candidate of [middle - 1n, middle + 1n]) {
        if (candidate <= 0n || !reads(candidate)) continue;
        const distance = (c: bigint) => Math.abs(Number(`${c}e${closest.exponent - (precision - 1)}`) - a);
        if (best === null || distance(candidate) < distance(best)) best = candidate;
      }
    }
    if (best !== null) {
      const text = best.toString();
      return {
        digits: text.replace(/0+$/, '') || '0',
        exponent: closest.exponent + (text.length - precision),
      };
    }
  }
  return splitExponential(a.toExponential(8));
}

/**
 * Shortest round-trip decimal in the layout of C++ `std::to_chars`: fixed or
 * scientific, whichever is shorter (fixed on a tie), with a signed exponent
 * of at least two digits. `x` is finite.
 */
export function formatFloat(x: number, single: boolean): string {
  if (x === 0) return Object.is(x, -0) ? '-0' : '0';
  const sign = x < 0 ? '-' : '';
  const a = Math.abs(x);
  let { digits, exponent } = single ? shortestSingle(a) : splitExponential(a.toExponential());
  // When the value lies exactly halfway between two shortest candidates
  // std::to_chars keeps the even last digit. The engine's choice there is
  // unspecified, so decide it from the exact expansion.
  if (digits.length <= 17) {
    const probe = splitExponential(a.toExponential(digits.length));
    if (probe.exponent === exponent && probe.digits.length === digits.length + 1 && probe.digits.endsWith('5')) {
      const exact = exactDecimal(a);
      if (exact.exponent === exponent && exact.digits === probe.digits) {
        // Both neighbours are equally close; a candidate must still read
        // back as the value (at a power of two the lower one may not).
        const reads = (candidate: string) => {
          const value = Number(`${candidate}e${exponent - (candidate.length - 1)}`);
          return (single ? Math.fround(value) : value) === a;
        };
        const down = exact.digits.slice(0, digits.length);
        const up = (BigInt(down) + 1n).toString();
        const even = (down.charCodeAt(down.length - 1) - 48) % 2 === 0;
        if (reads(down) && (even || up.length !== down.length || !reads(up))) digits = down;
        else if (up.length === down.length && reads(up)) digits = up;
      }
    }
  }
  const n = digits.length;
  const magnitude = Math.abs(exponent);
  const scientificLength = n + (n > 1 ? 1 : 0) + 2 + (magnitude >= 100 ? 3 : 2);
  const fixedLength = exponent < 0 ? 1 - exponent + n : exponent >= n - 1 ? exponent + 1 : n + 1;
  if (fixedLength <= scientificLength) {
    if (exponent < 0) return `${sign}0.${'0'.repeat(-exponent - 1)}${digits}`;
    // An integer: every digit of its exact value.
    if (exponent >= n - 1) return sign + BigInt(a).toString();
    return `${sign}${digits.slice(0, exponent + 1)}.${digits.slice(exponent + 1)}`;
  }
  return `${sign}${digits[0]}${n > 1 ? '.' + digits.slice(1) : ''}e${exponent < 0 ? '-' : '+'}${
    magnitude < 10 ? '0' : ''
  }${magnitude}`;
}

// Compares the unsigned RFC 8259 number `token` with the positive double `d`.
function compareTokenToDouble(token: string, d: number): number {
  const match = /^([0-9]+)(?:\.([0-9]+))?(?:[eE]([+-]?[0-9]+))?$/.exec(token)!;
  const integer = match[1];
  const all = integer + (match[2] ?? '');
  const first = all.search(/[1-9]/);
  if (first < 0) return -1; // zero < d
  const exponent = integer.length - 1 - first + Number(match[3] ?? '0');
  const digits = all.slice(first).replace(/0+$/, '');
  const exact = exactDecimal(d);
  if (exponent !== exact.exponent) return exponent < exact.exponent ? -1 : 1;
  return digits < exact.digits ? -1 : digits > exact.digits ? 1 : 0;
}

/**
 * A JSON number token as a correctly rounded binary32, or null when it is
 * not finite there. `Math.fround(Number(token))` rounds twice and is wrong
 * when the double lands exactly halfway between two floats; that case is
 * decided from the token's exact value.
 */
export function parseSingle(token: string): number | null {
  const d = Number(token);
  if (!Number.isFinite(d)) return null;
  const negative = token.charCodeAt(0) === 45;
  const a = Math.abs(d);
  let f = Math.fround(a);
  if (f !== a) {
    // The floats either side of `a`; 2^128 stands in for infinity.
    let lo: number;
    let hi: number;
    f32[0] = f;
    if (f > a) {
      hi = Number.isFinite(f) ? f : 2 ** 128;
      if (Number.isFinite(f)) {
        u32[0] -= 1;
        lo = f32[0];
      } else lo = 3.4028234663852886e38;
    } else {
      lo = f;
      u32[0] += 1;
      hi = Number.isFinite(f32[0]) ? f32[0] : 2 ** 128;
    }
    if (a - lo === hi - a) {
      const order = compareTokenToDouble(negative ? token.slice(1) : token, a);
      if (order < 0) f = lo;
      else if (order > 0) f = hi === 2 ** 128 ? Infinity : hi;
      // order == 0: an exact tie, which Math.fround already sent to even.
    }
  }
  if (!Number.isFinite(f)) return null;
  return negative ? -f : f;
}

// ── Syntax: the cursor ──────────────────────────────────────────────────────

export type JsonKind = 'object' | 'array' | 'string' | 'number' | 'boolean' | 'null';

interface StringEvent {
  bytes: Uint8Array;
  start: number;
  end: number;
  /** The text between the quotes differed from the value. */
  escaped: boolean;
}

const QUOTE = 34, BACKSLASH = 92, COMMA = 44, COLON = 58, MINUS = 45;
const LBRACE = 123, RBRACE = 125, LBRACKET = 91, RBRACKET = 93;

function hexDigit(c: number): number {
  if (c >= 48 && c <= 57) return c - 48;
  if (c >= 97 && c <= 102) return c - 87;
  if (c >= 65 && c <= 70) return c - 55;
  return -1;
}

/**
 * Forward-only, single-pass JSON text cursor over UTF-8 bytes. It owns
 * whitespace, punctuation, string escapes and UTF-8, the RFC 8259 number
 * grammar, the nesting limit and skipping a value. `nonchar` applies the
 * I-JSON string rule (no Unicode noncharacters), also inside skipped values.
 */
export class JsonCursor {
  p = 0;
  private depth = 0;
  private readonly fresh: boolean[] = [];
  private readonly end: number;

  constructor(private readonly b: Uint8Array, private readonly nonchar: boolean) {
    this.end = b.length;
    if (b.length === 0) this.fail('json: empty input');
  }

  fail(what: string): never {
    throw new PsszError('json', what, this.p);
  }

  private ws(): void {
    const b = this.b;
    while (this.p < this.end) {
      const c = b[this.p];
      if (c !== 32 && c !== 9 && c !== 10 && c !== 13) break;
      ++this.p;
    }
  }
  private take(c: number): boolean {
    this.ws();
    if (this.p < this.end && this.b[this.p] === c) {
      ++this.p;
      return true;
    }
    return false;
  }
  private keyword(word: string): boolean {
    if (this.end - this.p < word.length) return false;
    for (let i = 0; i < word.length; ++i) if (this.b[this.p + i] !== word.charCodeAt(i)) return false;
    this.p += word.length;
    return true;
  }

  kind(): JsonKind {
    this.ws();
    if (this.p >= this.end) this.fail('json: unexpected end of input');
    const c = this.b[this.p];
    switch (c) {
      case LBRACE: return 'object';
      case LBRACKET: return 'array';
      case QUOTE: return 'string';
      case 116: case 102: return 'boolean';
      case 110: return 'null';
      default:
        if (c === MINUS || (c >= 48 && c <= 57)) return 'number';
        return this.fail('json: unexpected character at value start');
    }
  }

  private open(c: number, expected: string): void {
    if (!this.take(c)) this.fail(expected);
    if (this.depth >= MAX_DEPTH) this.fail('json: max depth exceeded');
    this.fresh[this.depth++] = true;
  }
  // Advances past the separator before the next item, or closes the
  // container. False once it is closed.
  private advance(close: number, expected: string): boolean {
    if (this.depth === 0) this.fail('json: cursor is not inside a container');
    const first = this.fresh[this.depth - 1];
    this.fresh[this.depth - 1] = false;
    if (this.take(close)) {
      --this.depth;
      return false;
    }
    if (!first && !this.take(COMMA)) this.fail(expected);
    return true;
  }

  beginObject(): void { this.open(LBRACE, "json: expected '{'"); }
  beginArray(): void { this.open(LBRACKET, "json: expected '['"); }

  /** The next member's key (unescaped), or null once the object is closed. */
  nextField(): string | null {
    if (!this.advance(RBRACE, "json: expected ',' or '}' in object")) return null;
    const key = this.string();
    if (!this.take(COLON)) this.fail("json: expected ':' after object key");
    return utf8Decode(key.bytes, key.start, key.end);
  }
  /** Positions at the next element; false once the array is closed. */
  nextElement(): boolean {
    return this.advance(RBRACKET, "json: expected ',' or ']' in array");
  }

  private hex4(): number {
    if (this.end - this.p < 4) this.fail('json: truncated \\uXXXX escape');
    let v = 0;
    for (let k = 0; k < 4; ++k) {
      const d = hexDigit(this.b[this.p + k]);
      if (d < 0) this.fail('json: bad hex digit in \\uXXXX');
      v = (v << 4) | d;
    }
    this.p += 4;
    return v;
  }

  // Reads a JSON string; `p` must be at its opening quote. Appends the
  // unescaped UTF-8 to `out` when given. Returns whether it had escapes.
  private readString(out: number[] | null): boolean {
    const b = this.b;
    if (this.p >= this.end || b[this.p] !== QUOTE) this.fail('json: expected a string');
    ++this.p;
    let escaped = false;
    while (true) {
      if (this.p >= this.end) this.fail('json: unterminated string');
      const c = b[this.p];
      if (c === QUOTE) {
        ++this.p;
        return escaped;
      }
      if (c === BACKSLASH) {
        escaped = true;
        if (this.end - this.p < 2) this.fail('json: trailing backslash in string');
        const e = b[this.p + 1];
        this.p += 2;
        let cp: number;
        switch (e) {
          case QUOTE: cp = QUOTE; break;
          case BACKSLASH: cp = BACKSLASH; break;
          case 47: cp = 47; break;
          case 98: cp = 8; break;
          case 102: cp = 12; break;
          case 110: cp = 10; break;
          case 114: cp = 13; break;
          case 116: cp = 9; break;
          case 117: {
            cp = this.hex4();
            if (cp >= 0xdc00 && cp <= 0xdfff) this.fail('json: unpaired surrogate escape');
            if (cp >= 0xd800 && cp <= 0xdbff) {
              if (this.end - this.p < 2 || b[this.p] !== BACKSLASH || b[this.p + 1] !== 117)
                this.fail('json: unpaired surrogate escape');
              this.p += 2;
              const lo = this.hex4();
              if (lo < 0xdc00 || lo > 0xdfff) this.fail('json: unpaired surrogate escape');
              cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
            }
            if (this.nonchar && isNoncharacter(cp)) this.fail('ijson: Unicode noncharacter in string');
            break;
          }
          default:
            this.p -= 1;
            return this.fail('json: invalid escape character in string');
        }
        if (out) {
          if (cp < 0x80) out.push(cp);
          else if (cp < 0x800) out.push(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f));
          else if (cp < 0x10000) out.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
          else
            out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
        }
        continue;
      }
      if (c < 0x20) this.fail('json: unescaped control character in string');
      if (c < 0x80) {
        if (out) out.push(c);
        ++this.p;
        continue;
      }
      const n = utf8Multibyte(b, this.p, this.end);
      if (n === 0) this.fail('json: string is not valid UTF-8');
      if (this.nonchar && isNoncharacter(utf8CodePoint)) this.fail('ijson: Unicode noncharacter in string');
      if (out) for (let k = 0; k < n; ++k) out.push(b[this.p + k]);
      this.p += n;
    }
  }

  /** A string value, unescaped. */
  string(): StringEvent {
    this.ws();
    const start = this.p;
    if (!this.readString(null)) return { bytes: this.b, start: start + 1, end: this.p - 1, escaped: false };
    this.p = start;
    const out: number[] = [];
    this.readString(out);
    return { bytes: Uint8Array.from(out), start: 0, end: out.length, escaped: true };
  }

  // RFC 8259 number grammar; `p` at its first character.
  private skipNumber(): void {
    const b = this.b;
    const end = this.end;
    let p = this.p;
    const digit = () => p < end && b[p] >= 48 && b[p] <= 57;
    if (p < end && b[p] === MINUS) ++p;
    if (p >= end) this.fail('json: truncated number');
    if (b[p] === 48) ++p;
    else if (b[p] >= 49 && b[p] <= 57) {
      ++p;
      while (digit()) ++p;
    } else this.fail('json: number missing integer part');
    if (p < end && b[p] === 46) {
      ++p;
      const fraction = p;
      while (digit()) ++p;
      if (p === fraction) this.fail('json: number missing fractional digits');
    }
    if (p < end && (b[p] === 101 || b[p] === 69)) {
      ++p;
      if (p < end && (b[p] === 43 || b[p] === MINUS)) ++p;
      const exponent = p;
      while (digit()) ++p;
      if (p === exponent) this.fail('json: number missing exponent digits');
    }
    this.p = p;
  }

  /** A number as its RFC 8259 token text. */
  number(): string {
    this.ws();
    const begin = this.p;
    if (begin >= this.end || !(this.b[begin] === MINUS || (this.b[begin] >= 48 && this.b[begin] <= 57)))
      this.fail('json: expected a number');
    this.skipNumber();
    let token = '';
    for (let i = begin; i < this.p; ++i) token += String.fromCharCode(this.b[i]);
    return token;
  }

  boolean(): boolean {
    this.ws();
    if (this.keyword('true')) return true;
    if (this.keyword('false')) return false;
    return this.fail('json: expected true or false');
  }

  null(): void {
    this.ws();
    if (!this.keyword('null')) this.fail('json: malformed keyword');
  }

  /** Skips one well-formed value of any shape (the value of an unknown key). */
  skip(): void {
    this.skipValue(this.depth);
  }
  private skipValue(depth: number): void {
    this.ws();
    if (this.p >= this.end) this.fail('json: unexpected end of input');
    const c = this.b[this.p];
    switch (c) {
      case LBRACE:
        if (depth >= MAX_DEPTH) this.fail('json: max depth exceeded');
        ++this.p;
        if (this.take(RBRACE)) return;
        while (true) {
          this.ws();
          this.readString(null);
          if (!this.take(COLON)) this.fail("json: expected ':' after object key");
          this.skipValue(depth + 1);
          if (this.take(RBRACE)) return;
          if (!this.take(COMMA)) this.fail("json: expected ',' or '}' in object");
        }
      case LBRACKET:
        if (depth >= MAX_DEPTH) this.fail('json: max depth exceeded');
        ++this.p;
        if (this.take(RBRACKET)) return;
        while (true) {
          this.skipValue(depth + 1);
          if (this.take(RBRACKET)) return;
          if (!this.take(COMMA)) this.fail("json: expected ',' or ']' in array");
        }
      case QUOTE:
        this.readString(null);
        return;
      case 116:
        if (!this.keyword('true')) this.fail('json: malformed keyword');
        return;
      case 102:
        if (!this.keyword('false')) this.fail('json: malformed keyword');
        return;
      case 110:
        if (!this.keyword('null')) this.fail('json: malformed keyword');
        return;
      default:
        if (c === MINUS || (c >= 48 && c <= 57)) return this.skipNumber();
        this.fail('json: unexpected character at value start');
    }
  }

  /** Nothing but whitespace remains. */
  finish(): void {
    this.ws();
    if (this.p !== this.end) this.fail('json: trailing bytes after value');
  }
}

// ── Types: reading ──────────────────────────────────────────────────────────

// `0 | -?[1-9][0-9]*` within the range of an integer of `bits` bits.
function parseInteger(text: string, signed: boolean, bits: number): bigint | null {
  if (text.length > MAX_INTEGER_TEXT || !/^(0|-?[1-9][0-9]*)$/.test(text)) return null;
  return inRange(BigInt(text), signed, bits);
}
// Longest integer text: a 256-bit decimal has 78 digits.
const MAX_INTEGER_TEXT = 80;

function inRange(value: bigint, signed: boolean, bits: number): bigint | null {
  if (!signed) return value >= 0n && value < 1n << BigInt(bits) ? value : null;
  const half = 1n << BigInt(bits - 1);
  return value >= -half && value < half ? value : null;
}

// The text of a 128 / 256-bit integer: canonical decimal, or the legacy
// `[-]0x<hex>` form (input only; at most bits / 4 digits of either case).
function parseWide(text: string, signed: boolean, bits: number): bigint | null {
  if (text.length > MAX_INTEGER_TEXT) return null;
  const hex = /^(-?)0[xX]([0-9a-fA-F]+)$/.exec(text);
  if (!hex) return parseInteger(text, signed, bits);
  if (hex[2].length > bits / 4 || (hex[1] && !signed)) return null;
  const magnitude = BigInt('0x' + hex[2]);
  return inRange(hex[1] ? -magnitude : magnitude, signed, bits);
}

// The characters of an unescaped string event (integer and enum text).
function asciiText(s: StringEvent): string {
  return String.fromCharCode(...s.bytes.subarray(s.start, s.end));
}

// The byte container under a string / hex adapter admits `n` bytes.
function bytesAdmit(container: Node, n: number): boolean {
  if (container.kind === 'array') return n === container.countNum;
  return !container.bounded || n <= container.countNum;
}

// The value a custom handler deals in: checked against the wire type and
// returned as `decode` would give it. Throws PsszError when it does not fit.
function canonical(codec: Codec, node: Node, value: unknown): unknown {
  const bytes = codec.encodeAt(node, value, 0);
  return codec.decodeAt(node, bytes, 0, bytes.length, 0, { allowUnknown: false, hasUnknown: false });
}

class Reader {
  constructor(
    private readonly c: JsonCursor,
    private readonly ijson: boolean,
    private readonly codec: Codec,
  ) {}

  // A custom presentation: one JSON string, turned by its handler into a
  // value of the inner type.
  private presented(node: Node): unknown {
    const c = this.c;
    if (c.kind() !== 'string') c.fail('json: expected a string (custom text form)');
    const s = c.string();
    let value: unknown;
    try {
      value = node.handler!.fromText(utf8Decode(s.bytes, s.start, s.end));
    } catch {
      return c.fail('json: text is not a value of the custom type');
    }
    try {
      return canonical(this.codec, this.codec.wireOf(node).node, value);
    } catch {
      return c.fail('json: custom handler produced a value that is not the inner type');
    }
  }

  // A 128 / 256-bit integer: a decimal string (or the legacy "0x" string),
  // or a bare decimal number: any in-range value under json, at most
  // 2^53 - 1 in magnitude under ijson. A forced form accepts that form only.
  private wideInteger(bits: number, signed: boolean, form: IntForm): bigint {
    const c = this.c;
    const quoted = c.kind() === 'string';
    let value: bigint | null;
    if (quoted) {
      if (form === 'number') c.fail('json: expected a number, found a string');
      const s = c.string();
      value = s.escaped || s.end - s.start > MAX_INTEGER_TEXT ? null : parseWide(asciiText(s), signed, bits);
      if (value === null) return c.fail('json: integer string is not a canonical in-range decimal');
    } else {
      if (form === 'string') c.fail('json: expected an integer string');
      if (c.kind() !== 'number') c.fail('json: expected an integer');
      value = parseWide(c.number(), signed, bits);
      if (value === null) return c.fail('json: integer is not a canonical in-range decimal');
      if (this.ijson && form === 'natural' && (value > SAFE_INTEGER || value < -SAFE_INTEGER))
        c.fail('ijson: bare integer outside the safe range must be a string');
    }
    return value;
  }

  // An Enum: the enumerator's name ("active"), the number (2 or the numeric
  // string "2"), or both ("active:2", which must agree). An unknown name is
  // an error; a number that names no enumerator is accepted.
  private enumeration(node: Node, form: IntForm): number | bigint {
    const c = this.c;
    const { bits, signed } = node;
    if (bits > 64 || bits < 8) c.fail('pssz IR: an enum needs an underlying integer of 8 to 64 bits');
    if (c.kind() !== 'string') return this.integer(bits, signed, form);
    const s = c.string();
    const first = s.end > s.start ? s.bytes[s.start] : 0;
    let value: bigint | null;
    if (first === MINUS || (first >= 48 && first <= 57)) {
      value = s.end - s.start > MAX_INTEGER_TEXT ? null : parseInteger(asciiText(s), signed, bits);
      if (value === null) return c.fail('json: enum number is not a canonical in-range decimal');
    } else {
      const text = utf8Decode(s.bytes, s.start, s.end);
      const colon = text.indexOf(':');
      const name = colon < 0 ? text : text.slice(0, colon);
      const found = node.enumerators!.find((m) => m.name === name);
      if (!found) return c.fail('json: unknown enumerator name');
      if (colon >= 0) {
        value = parseInteger(text.slice(colon + 1), signed, bits);
        if (value === null) return c.fail('json: enum number is not a canonical in-range decimal');
        if (BigInt.asUintN(64, value) !== found.value) c.fail('json: enumerator name and number disagree');
      } else {
        value = inRange(signed ? BigInt.asIntN(64, found.value) : found.value, signed, bits);
        if (value === null) return c.fail('json: enumerator value is outside its underlying integer');
      }
    }
    return bits <= 32 ? Number(value) : value;
  }

  private integer(bits: number, signed: boolean, form: IntForm): number | bigint {
    const c = this.c;
    if (bits > 64) return this.wideInteger(bits, signed, form);
    const wide = this.ijson && bits === 64 && form === 'natural';
    const kind = c.kind();
    let value: bigint | null;
    if (kind === 'string') {
      if (form !== 'string' && !wide) c.fail('json: expected a number, found a string');
      const s = c.string();
      value = s.escaped || s.end - s.start > 21 ? null : parseInteger(asciiText(s), signed, bits);
      if (value === null) return c.fail('json: integer string is not a canonical in-range decimal');
    } else {
      if (form === 'string') c.fail('json: expected an integer string');
      if (kind !== 'number') c.fail('json: expected an integer');
      const token = c.number();
      value = token.length > 21 ? null : parseInteger(token, signed, bits);
      if (value === null) return c.fail('json: integer is not a canonical in-range decimal');
      // Natural ijson: a bare number only where JavaScript is exact.
      if (wide && (value > SAFE_INTEGER || value < -SAFE_INTEGER))
        c.fail('ijson: bare integer outside the safe range must be a string');
    }
    return bits <= 32 ? Number(value) : value;
  }

  // A time value: bigint ticks. A duration is its tick integer; a time
  // point is an RFC 3339 string (the JSON string's value, however escaped).
  private time(node: Node, form: IntForm): bigint {
    const c = this.c;
    if (!node.time!.point) return BigInt(this.integer(node.bits, node.signed, form));
    if (node.timeDigits < 0) c.fail('json: this time point unit has no RFC 3339 text form');
    if (c.kind() !== 'string') c.fail('json: expected an RFC 3339 date-time string');
    const s = c.string();
    // A date-time is ASCII; anything else fails the grammar.
    let text = '';
    if (s.end - s.start <= 256) for (let i = s.start; i < s.end; ++i) text += String.fromCharCode(s.bytes[i]);
    const out = timeTicksOf(text, node.timeDigits, { bits: node.bits as 8 | 16 | 32 | 64, signed: node.signed });
    return typeof out === 'string' ? c.fail(out) : out.ticks;
  }

  private arrayItems(limit: number | null, exact: boolean, item: () => void): void {
    const c = this.c;
    c.beginArray();
    let count = 0;
    while (c.nextElement()) {
      if (limit !== null && count >= limit)
        c.fail(exact ? 'json: array has too many elements' : 'json: array exceeds its bound');
      ++count;
      item();
    }
    if (exact && limit !== null && count !== limit) c.fail('json: array has too few elements');
  }

  private sequence(node: Node, form: IntForm): unknown[] {
    const items: unknown[] = [];
    this.arrayItems(node.bounded ? node.countNum : null, node.exact, () => {
      items.push(this.read(node.textChild, form, 0));
    });
    return items;
  }

  private record(node: Node): Record<string, unknown> {
    const c = this.c;
    c.beginObject();
    // One slot per member, filled as keys arrive; a repeated key overwrites
    // its slot (last one wins).
    const slots = new Array<unknown>(node.members.length).fill(undefined);
    while (true) {
      const key = c.nextField();
      if (key === null) break;
      const index = node.byName!.get(key);
      if (index === undefined) {
        c.skip();
        continue;
      }
      const member = node.members[index];
      const value = this.read(member.text, this.ijson ? member.ijsonInt : member.jsonInt, 0);
      if (member.bound && !memberBoundAdmits(member.bound, member.node, value))
        c.fail('json: member length_bound violated');
      slots[index] = value;
    }
    const out: Record<string, unknown> = {};
    for (let i = 0; i < slots.length; ++i) {
      if (slots[i] === undefined) {
        // A missing Option member is empty; any other is an error.
        if (node.members[i].node.kind !== 'option') c.fail('json: missing required member');
        slots[i] = null;
      }
      setField(out, node.members[i].name, slots[i]);
    }
    return out;
  }

  private variant(node: Node): VariantValue {
    const c = this.c;
    let index: number;
    let payload: unknown;
    if (this.ijson) {
      if (c.kind() !== 'object') c.fail('ijson: expected a variant object');
      c.beginObject();
      const key = c.nextField();
      if (key === null) return c.fail('ijson: variant object must have one key');
      index = node.members.findIndex((m) => m.ijsonName === key);
      if (index < 0) c.fail('ijson: unknown variant alternative');
      payload = this.read(node.members[index].text, 'natural', 0);
      if (c.nextField() !== null) c.fail('ijson: variant object must have one key');
    } else {
      if (c.kind() !== 'array') c.fail('json: expected a [index, value] variant');
      c.beginArray();
      if (!c.nextElement()) c.fail('json: expected a [index, value] variant');
      index = this.integer(32, false, 'natural') as number;
      if (index >= node.members.length) c.fail('json: variant index out of range');
      if (!c.nextElement()) c.fail("json: expected ',' in variant");
      payload = this.read(node.members[index].text, 'natural', 0);
      if (c.nextElement()) c.fail("json: expected ']' after variant value");
    }
    return { type: node.members[index].name, value: payload };
  }

  read(node: Node, form: IntForm, n: number): unknown {
    // `n` counts schema steps that consume no text (nested Options); it
    // restarts inside every array / object, whose nesting the cursor limits.
    const c = this.c;
    if (n > MAX_DEPTH) c.fail('json: max depth exceeded');
    switch (node.kind) {
      case 'int':
        if (node.time) return this.time(node, form);
        if (node.enumerators) return this.enumeration(node, form);
        return this.integer(node.bits, node.signed, form);
      case 'float': {
        const token = c.number();
        const value = node.size === 4 ? parseSingle(token) : Number(token);
        if (value === null || !Number.isFinite(value)) return c.fail('json: number out of range');
        return value;
      }
      case 'bool':
        return c.boolean();
      case 'char':
        return this.integer(8, false, form);
      case 'bitvector':
      case 'bitlist':
        return c.fail('json: bit types have no JSON form');
      case 'string': {
        if (c.kind() !== 'string') c.fail('json: expected a string');
        const s = c.string();
        if (!bytesAdmit(node.child, s.end - s.start)) c.fail('json: string exceeds its bound');
        // A raw string is its bytes (here: the UTF-8 of the text).
        if (node.raw) return s.bytes.slice(s.start, s.end);
        return utf8Decode(s.bytes, s.start, s.end);
      }
      case 'hex': {
        const container = node.child;
        if (!this.ijson) {
          const bytes: number[] = [];
          this.arrayItems(container.bounded ? container.countNum : null, container.exact, () => {
            bytes.push(this.integer(8, false, form) as number);
          });
          return Uint8Array.from(bytes);
        }
        if (c.kind() !== 'string') c.fail('ijson: expected a hex string');
        const s = c.string();
        if (s.escaped) c.fail('ijson: non-hex character in hex string');
        const length = s.end - s.start;
        for (let i = s.start; i < s.end; ++i)
          if (hexDigit(s.bytes[i]) < 0) c.fail('ijson: non-hex character in hex string');
        if (length % 2) c.fail('ijson: odd-length hex string');
        if (!bytesAdmit(container, length / 2))
          c.fail(container.exact ? 'ijson: hex string has the wrong length' : 'json: array exceeds its bound');
        const bytes = new Uint8Array(length / 2);
        for (let i = 0; i < bytes.length; ++i)
          bytes[i] = (hexDigit(s.bytes[s.start + 2 * i]) << 4) | hexDigit(s.bytes[s.start + 2 * i + 1]);
        return bytes;
      }
      case 'option':
        if (c.kind() === 'null') {
          c.null();
          return null;
        }
        return wrapSome(node, this.read(node.textChild, form, n + 1));
      case 'list':
      case 'array':
        return this.sequence(node, form);
      case 'tuple': {
        const items: unknown[] = [];
        this.arrayItems(node.members.length, true, () => {
          items.push(this.read(node.members[items.length].text, 'natural', 0));
        });
        return items;
      }
      case 'record':
        return this.record(node);
      case 'variant':
        return this.variant(node);
      case 'custom':
        // "box", or a text presentation with no handler: the inner type.
        if (!node.boxed) {
          if (node.handler) return this.presented(node);
          if (this.codec.requireHandlers) c.fail('pssz IR: no handler registered for a custom type');
        }
        return this.read(node.inner!, form, n + 1);
    }
  }
}

/** Reads exactly one value of `node` from JSON / I-JSON text. */
export function jsonToValue(codec: Codec, node: Node, format: JsonFormat, text: string | Uint8Array): unknown {
  let bytes: Uint8Array;
  if (typeof text === 'string') {
    if (!wellFormed(text)) throw new PsszError('json', 'json: text holds an unpaired surrogate', 0);
    bytes = utf8Encode(text);
  } else bytes = text;
  const ijson = format === 'ijson';
  const cursor = new JsonCursor(bytes, ijson);
  const value = new Reader(cursor, ijson, codec).read(node, 'natural', 0);
  cursor.finish();
  return value;
}

// ── Types: writing ──────────────────────────────────────────────────────────

const HEX_UPPER = '0123456789ABCDEF';
const HEX_LOWER = '0123456789abcdef';

function fail(message: string): never {
  throw new PsszError('value', message);
}

// Quotes and escapes `s`. `nonchar` also refuses Unicode noncharacters.
function writeString(out: string[], s: string, nonchar: boolean): void {
  if (!wellFormed(s)) fail('json: string is not valid UTF-8');
  let text = '"';
  let run = 0;
  for (let i = 0; i < s.length; ++i) {
    const c = s.charCodeAt(i);
    if (c >= 0x80) {
      if (nonchar) {
        const cp = s.codePointAt(i)!;
        if (isNoncharacter(cp)) fail('ijson: string holds a Unicode noncharacter');
        if (cp > 0xffff) ++i;
      }
      continue;
    }
    if (c >= 0x20 && c !== QUOTE && c !== BACKSLASH) continue;
    text += s.slice(run, i);
    run = i + 1;
    switch (c) {
      case QUOTE: text += '\\"'; break;
      case BACKSLASH: text += '\\\\'; break;
      case 10: text += '\\n'; break;
      case 13: text += '\\r'; break;
      case 9: text += '\\t'; break;
      default: text += '\\u00' + HEX_LOWER[c >> 4] + HEX_LOWER[c & 15];
    }
  }
  out.push(text + s.slice(run) + '"');
}

class Writer {
  readonly out: string[] = [];
  constructor(private readonly ijson: boolean, private readonly codec: Codec) {}

  private integer(
    bits: number, signed: boolean, form: IntForm, value: unknown, enumerators?: IrEnumerator[],
  ): void {
    let v: bigint;
    if (typeof value === 'bigint') v = value;
    else if (typeof value === 'number' && Number.isSafeInteger(value)) v = BigInt(value);
    else return fail('pssz IR: integer value does not match the schema');
    const half = 1n << BigInt(bits - 1);
    if (signed ? v < -half || v >= half : v < 0n || v >= 1n << BigInt(bits))
      fail('pssz IR: integer value does not match the schema');
    if (enumerators) {
      if (bits > 64 || bits < 8) fail('pssz IR: an enum needs an underlying integer of 8 to 64 bits');
      // A listed value is its (first) name; any other its number.
      const raw = BigInt.asUintN(64, v);
      const named = enumerators.find((m) => m.value === raw);
      if (named) return writeString(this.out, named.name, this.ijson);
    }
    // 64 bits: a string beyond 2^53 - 1 under ijson. Wider: always a string
    // under ijson, a bare number under json.
    const quoted =
      form === 'string' ||
      (form === 'natural' && this.ijson &&
        (bits > 64 || (bits === 64 && (v > SAFE_INTEGER || v < -SAFE_INTEGER))));
    this.out.push(quoted ? `"${v}"` : v.toString());
  }

  private sequence(node: Node, value: unknown, form: IntForm, n: number): void {
    if (!Array.isArray(value)) return fail('pssz IR: sequence value required');
    if (node.bounded && (node.exact ? value.length !== node.countNum : value.length > node.countNum))
      fail('json: array exceeds its bound');
    this.out.push('[');
    for (let i = 0; i < value.length; ++i) {
      if (i) this.out.push(',');
      this.write(node.textChild, value[i], form, n + 1);
    }
    this.out.push(']');
  }

  write(node: Node, value: unknown, form: IntForm, n: number): void {
    // A decoded value is at most MAX_DEPTH nodes deep and text nests at most
    // MAX_DEPTH containers; anything far beyond is a cyclic object.
    if (n > 4 * MAX_DEPTH) fail('pssz IR: node depth limit exceeded');
    const out = this.out;
    switch (node.kind) {
      case 'int':
        if (node.time?.point) {
          if (typeof value !== 'bigint' && !(typeof value === 'number' && Number.isSafeInteger(value)))
            return fail('pssz IR: integer value does not match the schema');
          const ticks = BigInt(value);
          if (!tickTypeAdmits({ bits: node.bits as 8 | 16 | 32 | 64, signed: node.signed }, ticks))
            return fail('pssz IR: integer value does not match the schema');
          if (node.timeDigits < 0) return fail('json: this time point unit has no RFC 3339 text form');
          const text = timeTextOf(ticks, node.timeDigits);
          if (typeof text === 'string') return fail(text);
          out.push('"' + text.text + '"');
          return;
        }
        return this.integer(node.bits, node.signed, form, value, node.enumerators);
      case 'float':
        if (value instanceof PsszNaN) return fail(`${this.ijson ? 'ijson' : 'json'}: non-finite float has no JSON text`);
        if (typeof value !== 'number') return fail('pssz IR: float value does not match the schema');
      {
        // The value as the float holds it: a number beyond binary32 is infinite there.
        const held = node.size === 4 ? Math.fround(value) : value;
        if (!Number.isFinite(held)) return fail(`${this.ijson ? 'ijson' : 'json'}: non-finite float has no JSON text`);
        out.push(formatFloat(held, node.size === 4));
        return;
      }
      case 'bool':
        if (typeof value !== 'boolean') return fail('pssz IR: bool value required');
        out.push(value ? 'true' : 'false');
        return;
      case 'char':
        return this.integer(8, false, form, value);
      case 'bitvector':
      case 'bitlist':
        return fail('json: bit types have no JSON form');
      case 'string':
        if (node.raw && value instanceof Uint8Array) {
          // A raw string holding bytes that are not UTF-8 has no JSON text.
          if (!bytesAdmit(node.child, value.length)) fail('json: string exceeds its bound');
          if (!utf8Valid(value, 0, value.length)) fail('json: string is not valid UTF-8');
          return writeString(out, utf8Decode(value, 0, value.length), this.ijson);
        }
        if (typeof value !== 'string') return fail('pssz IR: string value required');
        if (!wellFormed(value)) return fail('json: string is not valid UTF-8');
        if (!bytesAdmit(node.child, utf8Length(value))) fail('json: string exceeds its bound');
        return writeString(out, value, this.ijson);
      case 'hex': {
        if (!(value instanceof Uint8Array)) return fail('pssz IR: bytes value required');
        if (!bytesAdmit(node.child, value.length)) fail('json: array exceeds its bound');
        if (this.ijson) {
          let text = '"';
          for (const byte of value) text += HEX_UPPER[byte >> 4] + HEX_UPPER[byte & 15];
          out.push(text + '"');
          return;
        }
        out.push('[');
        for (let i = 0; i < value.length; ++i) {
          if (i) out.push(',');
          this.integer(8, false, form, value[i]);
        }
        out.push(']');
        return;
      }
      case 'option': {
        const [some, payload] = unwrapOption(node, value, fail);
        if (!some) out.push('null');
        else this.write(node.textChild, payload, form, n + 1);
        return;
      }
      case 'list':
      case 'array':
        return this.sequence(node, value, form, n);
      case 'tuple':
        if (!Array.isArray(value) || value.length !== node.members.length)
          return fail('pssz IR: tuple value count mismatch');
        out.push('[');
        for (let i = 0; i < value.length; ++i) {
          if (i) out.push(',');
          this.write(node.members[i].text, value[i], 'natural', n + 1);
        }
        out.push(']');
        return;
      case 'record': {
        if (typeof value !== 'object' || value === null || Array.isArray(value) || value instanceof Uint8Array)
          return fail('pssz IR: record value required');
        const fields = value as Record<string, unknown>;
        out.push('{');
        let first = true;
        for (const member of node.members) {
          const own = Object.prototype.hasOwnProperty.call(fields, member.name) ? fields[member.name] : undefined;
          if (own === undefined && member.node.kind !== 'option')
            fail(`pssz IR: missing required record value "${member.name}"`);
          const field = own === undefined ? null : own;
          if (member.bound && !memberBoundAdmits(member.bound, member.node, field))
            fail('json: member length_bound violated');
          if (!first) out.push(',');
          first = false;
          writeString(out, member.name, false);
          out.push(':');
          this.write(member.text, field, this.ijson ? member.ijsonInt : member.jsonInt, n + 1);
        }
        out.push('}');
        return;
      }
      case 'variant': {
        const v = value as VariantValue | null;
        if (typeof v !== 'object' || v === null || typeof v.type !== 'string')
          return fail('pssz IR: a variant value is { type, value }');
        const index = node.byName!.get(v.type);
        if (index === undefined) return fail(`pssz IR: unknown variant alternative "${v.type}"`);
        const member = node.members[index];
        if (this.ijson) {
          out.push('{');
          writeString(out, member.ijsonName, false);
          out.push(':');
          this.write(member.text, v.value, 'natural', n + 1);
          out.push('}');
        } else {
          out.push(`[${index},`);
          this.write(member.text, v.value, 'natural', n + 1);
          out.push(']');
        }
        return;
      }
      case 'custom': {
        if (!node.boxed) {
          if (node.handler) {
            // The inner value as `decode` gives it, then the handler's text
            // as one JSON string.
            let inner: unknown;
            try {
              inner = canonical(this.codec, this.codec.wireOf(node).node, value);
            } catch (e) {
              if (!(e instanceof PsszError)) throw e;
              return fail('pssz IR: custom value does not match its inner type');
            }
            let text: unknown;
            try {
              text = node.handler.toText(inner);
            } catch {
              return fail('json: custom handler has no text for the value');
            }
            if (typeof text !== 'string') return fail('json: custom handler has no text for the value');
            return writeString(out, text, this.ijson);
          }
          if (this.codec.requireHandlers) fail('pssz IR: no handler registered for a custom type');
        }
        // "box", or a text presentation with no handler: the inner type.
        return this.write(node.inner!, value, form, n + 1);
      }
    }
  }
}

/** The JSON / I-JSON text of `value`. */
export function valueToJson(codec: Codec, node: Node, format: JsonFormat, value: unknown): string {
  const writer = new Writer(format === 'ijson', codec);
  writer.write(node, value, 'natural', 0);
  return writer.out.join('');
}
