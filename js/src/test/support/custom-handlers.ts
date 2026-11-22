// The custom text forms of the shared JSON fixture, as JavaScript handlers:
// `VersionText` and `AccountText` of cpp/tests/json_conformance_types.hpp.
// A handler works on the decoded value of the wire type.

import type { PsszCustomHandler } from '../../index.js';

const PART = /^(0|[1-9][0-9]*)$/;

/** Custom id "Version": record { major: u16, minor: u16 } as "major.minor". */
export const versionHandler: PsszCustomHandler = {
  toText(value) {
    const v = value as { major: number; minor: number };
    return `${v.major}.${v.minor}`;
  },
  fromText(text) {
    const parts = text.split('.');
    if (parts.length !== 2) throw new Error('version: expected major.minor');
    const [major, minor] = parts.map((part) => {
      if (!PART.test(part) || part.length > 5 || Number(part) > 65535) throw new Error('version: bad part');
      return Number(part);
    });
    return { major, minor };
  },
};

const ALPHABET = '.12345abcdefghijklmnopqrstuvwxyz';

/**
 * Custom id "AccountName": a u64 as up to 12 characters of ALPHABET, 5 bits
 * each, most significant first, right-padded with '.'; no trailing dot.
 */
export const accountHandler: PsszCustomHandler = {
  toText(value) {
    const v = value as bigint;
    let text = '';
    for (let i = 0; i < 12; ++i) text += ALPHABET[Number((v >> BigInt(5 * (11 - i))) & 31n)];
    return text.replace(/\.+$/, '');
  },
  fromText(text) {
    if (text.length > 12 || text.endsWith('.')) throw new Error('account: too long or trailing dot');
    let v = 0n;
    for (let i = 0; i < 12; ++i) {
      let digit = 0;
      if (i < text.length) {
        digit = ALPHABET.indexOf(text[i]);
        if (digit < 0) throw new Error('account: bad character');
      }
      v = (v << 5n) | BigInt(digit);
    }
    return v;
  },
};

// ── conformance/custom-types.md ─────────────────────────────────────────────
//
// The text forms and invariants of the custom types the three fixtures
// share (cpp/tests/custom_conformance_types.hpp), written by hand from the
// rules of that file: a schema cannot carry them.

const DECIMAL = /^(0|[1-9][0-9]*)$/;

// A canonical decimal (`0`, or a non-zero digit followed by digits) at most `max`.
function decimal(text: string, max: bigint): bigint {
  if (!DECIMAL.test(text) || text.length > 20) throw new Error('not a canonical decimal');
  const value = BigInt(text);
  if (value > max) throw new Error('decimal out of range');
  return value;
}

const BASE64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

/** Custom id "Blob": a byte list as base64 (RFC 4648 §4, padded, exact). */
export const blobHandler: PsszCustomHandler = {
  toText(value) {
    const b = value as Uint8Array;
    let text = '';
    for (let i = 0; i < b.length; i += 3) {
      const n = (b[i] << 16) | ((b[i + 1] ?? 0) << 8) | (b[i + 2] ?? 0);
      text += BASE64[n >> 18] + BASE64[(n >> 12) & 63];
      text += i + 1 < b.length ? BASE64[(n >> 6) & 63] : '=';
      text += i + 2 < b.length ? BASE64[n & 63] : '=';
    }
    return text;
  },
  fromText(text) {
    if (text.length % 4) throw new Error('base64: length is not a multiple of 4');
    const out: number[] = [];
    for (let i = 0; i < text.length; i += 4) {
      const last = i + 4 === text.length;
      const pad = last && text[i + 3] === '=' ? (text[i + 2] === '=' ? 2 : 1) : 0;
      let n = 0;
      for (let k = 0; k < 4; ++k) {
        const digit = k >= 4 - pad ? 0 : BASE64.indexOf(text[i + k]);
        if (digit < 0) throw new Error('base64: bad character');
        n = (n << 6) | digit;
      }
      // The bits a padded group does not use are zero.
      if ((pad === 2 && n & 0xffff) || (pad === 1 && n & 0xff)) throw new Error('base64: non-zero padding bits');
      out.push(n >> 16);
      if (pad < 2) out.push((n >> 8) & 255);
      if (pad < 1) out.push(n & 255);
    }
    return Uint8Array.from(out);
  },
};

interface EndpointValue {
  type: string;
  value: { address: Uint8Array; port: number };
}

/** Custom id "Endpoint": variant<V4, V6> as `a.b.c.d:port` / `[g:..:g]:port`. */
export const endpointHandler: PsszCustomHandler = {
  toText(value) {
    const v = value as EndpointValue;
    const a = v.value.address;
    if (v.type === '0') return `${a[0]}.${a[1]}.${a[2]}.${a[3]}:${v.value.port}`;
    const groups: string[] = [];
    for (let i = 0; i < 8; ++i) groups.push(((a[2 * i] << 8) | a[2 * i + 1]).toString(16).padStart(4, '0'));
    return `[${groups.join(':')}]:${v.value.port}`;
  },
  fromText(text) {
    if (text.startsWith('[')) {
      const match = /^\[([0-9a-f]{4}(?::[0-9a-f]{4}){7})\]:(.*)$/s.exec(text);
      if (!match) throw new Error('endpoint: bad V6');
      const address = new Uint8Array(16);
      match[1].split(':').forEach((group, i) => {
        address[2 * i] = parseInt(group.slice(0, 2), 16);
        address[2 * i + 1] = parseInt(group.slice(2), 16);
      });
      return { type: '1', value: { address, port: Number(decimal(match[2], 65535n)) } };
    }
    const colon = text.lastIndexOf(':');
    if (colon < 0) throw new Error('endpoint: no port');
    const octets = text.slice(0, colon).split('.');
    if (octets.length !== 4) throw new Error('endpoint: bad V4');
    return {
      type: '0',
      value: {
        address: Uint8Array.from(octets.map((octet) => Number(decimal(octet, 255n)))),
        port: Number(decimal(text.slice(colon + 1), 65535n)),
      },
    };
  },
};

/** Custom id "Ratio": tuple<i16, u16> as `n/d`. */
export const ratioHandler: PsszCustomHandler = {
  toText(value) {
    const [n, d] = value as [number, number];
    return `${n}/${d}`;
  },
  fromText(text) {
    const slash = text.indexOf('/');
    if (slash < 0) throw new Error('ratio: no slash');
    const n = text.slice(0, slash);
    // `0 | -?[1-9][0-9]*`: "-0" is not a numerator.
    const negative = n.startsWith('-');
    const magnitude = decimal(negative ? n.slice(1) : n, negative ? 32768n : 32767n);
    if (negative && magnitude === 0n) throw new Error('ratio: -0');
    return [Number(negative ? -magnitude : magnitude), Number(decimal(text.slice(slash + 1), 65535n))];
  },
};

interface RefValue {
  type: string;
  value: bigint | Uint8Array;
}

/**
 * Custom id "Ref": variant<u64, raw_string> as `#n` / `@name`; a name is not
 * empty (the invariant: `"@"` is well-formed text of a value it refuses).
 */
export const refHandler: PsszCustomHandler = {
  toText(value) {
    const v = value as RefValue;
    if (v.type === '0') return `#${v.value}`;
    return '@' + new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(v.value as Uint8Array);
  },
  fromText(text) {
    if (text.startsWith('#')) return { type: '0', value: decimal(text.slice(1), (1n << 64n) - 1n) };
    if (text.startsWith('@')) return { type: '1', value: new TextEncoder().encode(text.slice(1)) };
    throw new Error('ref: neither #n nor @name');
  },
  check(value) {
    const v = value as RefValue;
    return v.type === '1' && (v.value as Uint8Array).length === 0 ? 'Ref: a name is not empty' : undefined;
  },
};

/** Custom id "Name": record { value: u64 }, `value < 2^60`. */
export const nameHandler: PsszCustomHandler = {
  check: (value) => ((value as { value: bigint }).value >> 60n === 0n ? undefined : 'Name: value is not a canonical name'),
};
/** Custom id "DerKey": a byte list, not empty and starting with 0x30. */
export const derKeyHandler: PsszCustomHandler = {
  check(value) {
    const b = value as Uint8Array;
    // Thrown rather than returned: both spellings are the invariant's message.
    if (b.length === 0 || b[0] !== 0x30) throw new Error('DerKey: not a DER sequence');
  },
};
/** Custom id "Range": record { lo: u32, hi: u32 }, `lo <= hi`. */
export const rangeHandler: PsszCustomHandler = {
  check: (value) => {
    const r = value as { lo: number; hi: number };
    return r.lo <= r.hi ? null : 'Range: lo is greater than hi';
  },
};
/** Custom id "Level": an enum over u8, one of the four listed values. */
export const levelHandler: PsszCustomHandler = {
  check: (value) => ((value as number) <= 3 ? undefined : 'Level: not a listed level'),
};

/** The handlers of conformance/custom-types.md, by `Custom` id. */
export const customTypeHandlers = {
  Blob: blobHandler, Endpoint: endpointHandler, Ratio: ratioHandler, Ref: refHandler,
  Name: nameHandler, DerKey: derKeyHandler, Range: rangeHandler, Level: levelHandler,
};

/** The fixtures' registry: every custom id their Schema IRs use. */
export const fixtureHandlers = { Version: versionHandler, AccountName: accountHandler, ...customTypeHandlers };
