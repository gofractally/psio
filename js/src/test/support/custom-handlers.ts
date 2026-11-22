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

/** The fixture's registry: the ids its Schema IR uses. */
export const fixtureHandlers = { Version: versionHandler, AccountName: accountHandler };
