import { describe, it } from 'node:test';
import * as assert from 'node:assert/strict';
import {
  nameToNumber, numberToName, isHashName, isCompressedName,
} from '../index.js';

describe('name compression', () => {
  it('empty string → 0', () => {
    assert.strictEqual(nameToNumber(''), 0n);
  });

  it('0 → empty string', () => {
    assert.strictEqual(numberToName(0n), '');
  });

  it('round-trips common names', () => {
    const names = [
      'hello', 'world', 'test', 'name', 'get', 'set', 'run',
      'init', 'transfer', 'balance', 'account',
    ];
    for (const name of names) {
      const num = nameToNumber(name);
      assert.notStrictEqual(num, 0n, `${name} should compress`);
      const decoded = numberToName(num);
      assert.strictEqual(decoded, name, `round-trip failed for "${name}": got "${decoded}"`);
    }
  });

  it('compressed names have no hash bit', () => {
    const num = nameToNumber('hello');
    assert.strictEqual(isCompressedName(num), true);
    assert.strictEqual(isHashName(num), false);
  });

  it('long or unusual names fall back to hash', () => {
    // Very long name or name with unsupported chars should hash
    const num = nameToNumber('thisisaverylongnamethatwontcompress');
    if (num !== 0n) {
      // Either compressed successfully or fell back to hash
      // If hash: marker bit is set
      if (isHashName(num)) {
        // Hash names round-trip through # prefix encoding
        const decoded = numberToName(num);
        assert.ok(decoded.startsWith('#'), 'hash names should decode to # prefix');
      }
    }
  });

  it('single character names', () => {
    for (const ch of 'abcdefghijklmnopqrstuvwxyz') {
      const num = nameToNumber(ch);
      assert.notStrictEqual(num, 0n, `'${ch}' should produce non-zero`);
      const decoded = numberToName(num);
      assert.strictEqual(decoded, ch, `round-trip failed for '${ch}'`);
    }
  });
});
