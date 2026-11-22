// hex.ts — bytes <-> hex text.

import { PsszError } from './pssz-core.js';

const DIGITS = '0123456789abcdef';

/** Lowercase hex of `bytes`. */
export function toHex(bytes: Uint8Array): string {
  let out = '';
  for (let i = 0; i < bytes.length; ++i) out += DIGITS[bytes[i] >> 4] + DIGITS[bytes[i] & 15];
  return out;
}

function digit(c: number): number {
  if (c >= 48 && c <= 57) return c - 48;
  if (c >= 97 && c <= 102) return c - 87;
  if (c >= 65 && c <= 70) return c - 55;
  return -1;
}

/** The bytes of hex text (either case). Throws a `PsszError` of kind `value`. */
export function fromHex(hex: string): Uint8Array {
  if (typeof hex !== 'string' || hex.length % 2 !== 0) throw new PsszError('value', 'hex: an even number of hex digits is required');
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; ++i) {
    const high = digit(hex.charCodeAt(2 * i));
    const low = digit(hex.charCodeAt(2 * i + 1));
    if (high < 0 || low < 0) throw new PsszError('value', 'hex: invalid hex digit');
    out[i] = (high << 4) | low;
  }
  return out;
}
