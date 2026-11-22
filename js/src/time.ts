// time.ts — time values (docs/time.md): schema ids and the RFC 3339 text of
// a time point, exact in bigint ticks. The counterpart of C++ psio/time.hpp.
//
// A time value is a tick count: a duration, a time point counted from
// 1970-01-01T00:00:00Z (UTC, no leap seconds), or a monotonic time point
// counted from the epoch of some other clock (no date: its text is the tick
// count, like a duration's). The schema-driven codecs decode all three to
// `bigint` ticks and never convert implicitly — a `Date` holds only
// milliseconds. The helpers here are the explicit conversions; they apply
// to time points only.

import { PsszError } from './pssz-core.js';

/**
 * The kind of a time value and its tick period `num/den` seconds in lowest
 * terms. `point`: a time point of the Unix epoch (RFC 3339 text).
 * `monotonic`: a time point of some other clock (tick text); never set
 * together with `point`. Neither: a duration.
 */
export interface TimeKind {
  point: boolean;
  monotonic?: true;
  num: bigint;
  den: bigint;
}

/** A tick period: a named unit, or `num/den` seconds. */
export type TimeUnit =
  | 'seconds' | 'millis' | 'micros' | 'nanos'
  | { num: number | bigint; den: number | bigint };

/** The integer type holding the tick count. */
export interface TickType {
  bits: 8 | 16 | 32 | 64;
  signed: boolean;
}

const NAMED: [string, bigint][] = [
  ['seconds', 1n], ['millis', 1000n], ['micros', 1000000n], ['nanos', 1000000000n],
];

function gcd(a: bigint, b: bigint): bigint {
  while (b) [a, b] = [b, a % b];
  return a;
}

function fail(message: string): never {
  throw new PsszError('value', message);
}

/** `unit` as a period in lowest terms. */
export function timeUnitPeriod(unit: TimeUnit): { num: bigint; den: bigint } {
  if (typeof unit === 'string') {
    const named = NAMED.find(([name]) => name === unit);
    if (!named) return fail(`time: unknown unit "${String(unit)}"`);
    return { num: 1n, den: named[1] };
  }
  if (typeof unit !== 'object' || unit === null) return fail('time: a unit is a name or { num, den }');
  const exact = (x: unknown) =>
    typeof x === 'bigint' ? x : typeof x === 'number' && Number.isSafeInteger(x) ? BigInt(x) : 0n;
  const num = exact(unit.num);
  const den = exact(unit.den);
  if (num <= 0n || den <= 0n) return fail('time: a unit period needs positive integers num and den');
  const g = gcd(num, den);
  return { num: num / g, den: den / g };
}

/**
 * Fraction digits of a period of 1/10^k seconds (k = 0..9); -1 for any
 * other period, which has no RFC 3339 form.
 */
export function timeFractionDigits(num: bigint, den: bigint): number {
  if (num !== 1n) return -1;
  let k = 0;
  let p = 1n;
  while (p < den && k < 9) {
    p *= 10n;
    ++k;
  }
  return p === den ? k : -1;
}

/** The schema id of a time kind: `time-point-micros`, `duration-60/1`, `monotonic-nanos`, ... */
export function timeSchemaId(kind: TimeKind): string {
  const g = gcd(kind.num, kind.den);
  if (kind.num <= 0n || kind.den <= 0n) fail('time: a period needs positive num and den');
  if (kind.monotonic !== undefined && kind.monotonic !== true) fail('time: monotonic is true or absent');
  if (kind.point && kind.monotonic) fail('time: a time point is of the Unix epoch or monotonic, not both');
  const num = kind.num / g;
  const den = kind.den / g;
  const named = num === 1n ? NAMED.find(([, d]) => d === den) : undefined;
  const prefix = kind.point ? 'time-point-' : kind.monotonic ? 'monotonic-' : 'duration-';
  return prefix + (named ? named[0] : `${num}/${den}`);
}

/**
 * The ids the time presentations reserve: `time-point` / `duration` /
 * `monotonic`, alone or followed by `-`. A reserved id that `parseTimeSchemaId` does not accept
 * is a malformed schema, never some other custom type.
 */
export function reservedTimeId(id: string): boolean {
  return /^(time-point|duration|monotonic)(-|$)/.test(id);
}

/**
 * The inverse of `timeSchemaId`; null for any other id, including a
 * non-canonical spelling (`1/1000` for millis, `2/4`, leading zeros).
 */
export function parseTimeSchemaId(id: string): TimeKind | null {
  let point = false;
  let monotonic = false;
  let rest: string;
  if (id.startsWith('time-point-')) {
    point = true;
    rest = id.slice(11);
  } else if (id.startsWith('duration-')) rest = id.slice(9);
  else if (id.startsWith('monotonic-')) {
    monotonic = true;
    rest = id.slice(10);
  } else return null;
  const kind = (num: bigint, den: bigint): TimeKind => (monotonic ? { point, monotonic, num, den } : { point, num, den });
  const named = NAMED.find(([name]) => name === rest);
  if (named) return kind(1n, named[1]);
  // Positive decimals without leading zeros, at most 18 digits each.
  const m = /^([1-9][0-9]{0,17})\/([1-9][0-9]{0,17})$/.exec(rest);
  if (!m) return null;
  const num = BigInt(m[1]);
  const den = BigInt(m[2]);
  if (gcd(num, den) !== 1n) return null;
  if (num === 1n && NAMED.some(([, d]) => d === den)) return null;
  return kind(num, den);
}

// ── RFC 3339 ────────────────────────────────────────────────────────────────

/** 0000-01-01T00:00:00Z and 9999-12-31T23:59:59Z as Unix seconds. */
export const MIN_TEXT_SECONDS = -62167219200n;
export const MAX_TEXT_SECONDS = 253402300799n;

// Proleptic Gregorian day arithmetic (days since 1970-01-01); exact in
// doubles for the four-digit year range.
function daysFromCivil(y: number, m: number, d: number): number {
  if (m <= 2) y -= 1;
  const era = Math.floor(y / 400);
  const yoe = y - era * 400;
  const doy = Math.floor((153 * (m > 2 ? m - 3 : m + 9) + 2) / 5) + d - 1;
  const doe = yoe * 365 + Math.floor(yoe / 4) - Math.floor(yoe / 100) + doy;
  return era * 146097 + doe - 719468;
}

function daysInMonth(y: number, m: number): number {
  if (m === 2 && y % 4 === 0 && (y % 100 !== 0 || y % 400 === 0)) return 29;
  return [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][m - 1];
}

const pad = (v: number, n: number) => String(v).padStart(n, '0');

function floorDiv(a: bigint, b: bigint): bigint {
  const q = a / b;
  return a % b !== 0n && a < 0n !== b < 0n ? q - 1n : q;
}

/**
 * The text of `secs` Unix seconds plus `sub` units of 10^-digits seconds.
 * `secs` must lie within the text range.
 */
function writeCivil(secs: bigint, sub: bigint, digits: number): string {
  const s = Number(secs); // |secs| < 2^38: exact
  const days = Math.floor(s / 86400);
  const rem = s - days * 86400;
  const z = days + 719468;
  const era = Math.floor(z / 146097);
  const doe = z - era * 146097;
  const yoe = Math.floor((doe - Math.floor(doe / 1460) + Math.floor(doe / 36524) - Math.floor(doe / 146096)) / 365);
  const doy = doe - (365 * yoe + Math.floor(yoe / 4) - Math.floor(yoe / 100));
  const mp = Math.floor((5 * doy + 2) / 153);
  const d = doy - Math.floor((153 * mp + 2) / 5) + 1;
  const m = mp < 10 ? mp + 3 : mp - 9;
  const y = yoe + era * 400 + (m <= 2 ? 1 : 0);
  return (
    `${pad(y, 4)}-${pad(m, 2)}-${pad(d, 2)}T` +
    `${pad(Math.floor(rem / 3600), 2)}:${pad(Math.floor(rem / 60) % 60, 2)}:${pad(rem % 60, 2)}` +
    (digits > 0 ? '.' + sub.toString().padStart(digits, '0') : '') +
    'Z'
  );
}

/**
 * Parses an RFC 3339 date-time into UTC Unix seconds plus `sub` units of
 * 10^-digits seconds, or returns why the text is rejected.
 */
function parseCivil(s: string, digits: number): { secs: bigint; sub: bigint } | string {
  const grammar = 'time: not an RFC 3339 date-time';
  if (s.length < 20) return grammar;
  const num = (at: number, n: number): number => {
    let v = 0;
    for (let i = at; i < at + n; ++i) {
      const c = s.charCodeAt(i);
      if (!(c >= 48 && c <= 57)) return -1;
      v = v * 10 + (c - 48);
    }
    return v;
  };
  const y = num(0, 4), mo = num(5, 2), d = num(8, 2), h = num(11, 2), mi = num(14, 2), sec = num(17, 2);
  if (
    y < 0 || s[4] !== '-' || mo < 0 || s[7] !== '-' || d < 0 || (s[10] !== 'T' && s[10] !== 't') ||
    h < 0 || s[13] !== ':' || mi < 0 || s[16] !== ':' || sec < 0
  )
    return grammar;
  if (mo < 1 || mo > 12 || d < 1 || d > daysInMonth(y, mo)) return 'time: no such calendar date';
  if (sec === 60) return 'time: leap seconds are not representable';
  if (h > 23 || mi > 59 || sec > 59) return 'time: no such time of day';

  let at = 19;
  let frac = 0;
  if (s[at] === '.') {
    ++at;
    let n = 0;
    for (; at < s.length; ++at, ++n) {
      const c = s.charCodeAt(at);
      if (!(c >= 48 && c <= 57)) break;
      if (n < digits) frac = frac * 10 + (c - 48);
      else if (c !== 48) return 'time: fraction is finer than the unit';
    }
    if (n === 0) return grammar;
    for (; n < digits; ++n) frac *= 10;
  }
  if (at >= s.length) return 'time: missing UTC offset';
  let offset = 0;
  if (s[at] === 'Z' || s[at] === 'z') ++at;
  else if (s[at] === '+' || s[at] === '-') {
    const oh = s.length - at < 6 ? -1 : num(at + 1, 2);
    const om = s.length - at < 6 ? -1 : num(at + 4, 2);
    if (oh < 0 || om < 0 || s[at + 3] !== ':' || oh > 23 || om > 59) return 'time: malformed UTC offset';
    offset = oh * 3600 + om * 60;
    if (s[at] === '-') offset = -offset;
    at += 6;
  } else return 'time: missing UTC offset';
  if (at !== s.length) return grammar;

  const utc = BigInt(daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec - offset);
  if (utc < MIN_TEXT_SECONDS || utc > MAX_TEXT_SECONDS) return 'time: outside years 0000..9999';
  return { secs: utc, sub: BigInt(frac) };
}

/** Whether `ticks` is a value of the tick type. */
export function tickTypeAdmits(type: TickType, ticks: bigint): boolean {
  const bits = BigInt(type.bits);
  return type.signed
    ? ticks >= -(1n << (bits - 1n)) && ticks < 1n << (bits - 1n)
    : ticks >= 0n && ticks < 1n << bits;
}

/**
 * RFC 3339 text of a time point with `digits` fraction digits, or a message
 * when the instant lies outside years 0000..9999 and so has no text.
 * @internal
 */
export function timeTextOf(ticks: bigint, digits: number): { text: string } | string {
  const scale = 10n ** BigInt(digits);
  const secs = floorDiv(ticks, scale);
  if (secs < MIN_TEXT_SECONDS || secs > MAX_TEXT_SECONDS)
    return 'time: time point outside years 0000..9999 has no text';
  return { text: writeCivil(secs, ticks - secs * scale, digits) };
}

/**
 * The tick count of RFC 3339 `text`, or a message saying why it is rejected.
 * @internal
 */
export function timeTicksOf(text: string, digits: number, type: TickType): { ticks: bigint } | string {
  const parsed = parseCivil(text, digits);
  if (typeof parsed === 'string') return parsed;
  const ticks = parsed.secs * 10n ** BigInt(digits) + parsed.sub;
  if (!tickTypeAdmits(type, ticks)) return 'time: outside the range of the tick type';
  return { ticks };
}

function textDigits(unit: TimeUnit): number {
  const { num, den } = timeUnitPeriod(unit);
  const digits = timeFractionDigits(num, den);
  if (digits < 0) fail('time: a time point has RFC 3339 text only for a unit of 1/10^k seconds, k = 0..9');
  return digits;
}

function asTicks(ticks: unknown): bigint {
  if (typeof ticks !== 'bigint') return fail('time: ticks must be a bigint');
  return ticks;
}

/**
 * The RFC 3339 UTC text of a time point, with exactly the unit's fraction
 * digits: `2026-11-22T10:15:30.123456Z` for micros. Throws when the instant
 * lies outside years 0000..9999.
 */
export function timePointToIso(ticks: bigint, unit: TimeUnit): string {
  const out = timeTextOf(asTicks(ticks), textDigits(unit));
  return typeof out === 'string' ? fail(out) : out.text;
}

/**
 * The tick count of an RFC 3339 date-time (any offset, any fraction the unit
 * holds exactly). Throws for anything else, and for an instant the tick type
 * (default: 64-bit signed) cannot hold.
 */
export function timePointFromIso(
  text: string, unit: TimeUnit, tickType: TickType = { bits: 64, signed: true },
): bigint {
  if (typeof text !== 'string') return fail('time: text must be a string');
  const out = timeTicksOf(text, textDigits(unit), tickType);
  return typeof out === 'string' ? fail(out) : out.ticks;
}

/**
 * The `Date` of a time point. Throws unless the instant is a whole number
 * of milliseconds within `Date`'s range: precision is never lost silently.
 */
export function timePointToDate(ticks: bigint, unit: TimeUnit): Date {
  const { num, den } = timeUnitPeriod(unit);
  const scaled = asTicks(ticks) * num * 1000n;
  if (scaled % den !== 0n) return fail('time: time point is not a whole number of milliseconds');
  const ms = scaled / den;
  if (ms < -8640000000000000n || ms > 8640000000000000n) return fail('time: time point outside the range of Date');
  return new Date(Number(ms));
}

/**
 * The tick count of a `Date`, exactly. Throws for an invalid Date and when
 * the unit cannot represent the Date's milliseconds (a coarser unit).
 */
export function timePointFromDate(date: Date, unit: TimeUnit): bigint {
  if (!(date instanceof Date) || Number.isNaN(date.getTime())) return fail('time: invalid Date');
  const { num, den } = timeUnitPeriod(unit);
  const scaled = BigInt(date.getTime()) * den;
  const per = num * 1000n;
  if (scaled % per !== 0n) return fail('time: the unit cannot represent this Date exactly');
  return scaled / per;
}
