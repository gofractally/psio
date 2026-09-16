import { describe, it } from 'node:test';
import * as assert from 'node:assert/strict';
import {
  bool, u8, u16, u32, u64, i8, i16, i32, i64, f32, f64,
  str, bytes, optional, vec, array, tuple, variant,
  struct, fixedStruct,
} from '../index.js';

function toHex(data: Uint8Array): string {
  return [...data].map(b => b.toString(16).padStart(2, '0')).join('');
}

function assertHex(type: { pack(v: any): Uint8Array }, value: any, expectedHex: string, label: string): void {
  const packed = type.pack(value);
  const actualHex = toHex(packed);
  assert.strictEqual(actualHex, expectedHex, `${label}: expected ${expectedHex}, got ${actualHex}`);
}

// These hex values come from running the Rust fracpack implementation
// with identical values. See rust/fracpack/tests/cross_lang.rs
describe('cross-language byte compatibility (vs Rust)', () => {
  it('u32: 0x01020304', () => {
    assertHex(u32, 0x01020304, '04030201', 'u32');
  });

  it('string: "hi"', () => {
    assertHex(str, 'hi', '020000006869', 'string');
  });

  it('vec<u32>: [1, 2]', () => {
    assertHex(vec(u32), [1, 2], '080000000100000002000000', 'vec<u32>');
  });

  it('optional<u32>: None', () => {
    assertHex(optional(u32), null, '01000000', 'option_none');
  });

  it('optional<u32>: Some(42)', () => {
    assertHex(optional(u32), 42, '040000002a000000', 'option_some');
  });

  it('fixed struct Point {x:1, y:2}', () => {
    const Point = fixedStruct({ x: u32, y: u32 });
    assertHex(Point, { x: 1, y: 2 }, '0100000002000000', 'fixed_struct');
  });

  it('extensible struct Simple {x:42}', () => {
    const Simple = struct({ x: u32 });
    assertHex(Simple, { x: 42 }, '04002a000000', 'extensible_struct');
  });

  it('person {name:"Alice", age:30, active:true}', () => {
    const Person = struct({
      name: str,
      age: u32,
      active: bool,
    });
    assertHex(
      Person,
      { name: 'Alice', age: 30, active: true },
      '0900090000001e0000000105000000416c696365',
      'person',
    );
  });

  it('tuple (42, "hello")', () => {
    const T = tuple(u32, str);
    assertHex(T, [42, 'hello'], '08002a000000040000000500000068656c6c6f', 'tuple');
  });

  it('person_v2 with email=None (trailing optional elided)', () => {
    const PersonV2 = struct({
      name: str,
      age: u32,
      email: optional(str),
    });
    assertHex(
      PersonV2,
      { name: 'Alice', age: 30, email: null },
      '0800080000001e00000005000000416c696365',
      'person_v2_email_none',
    );
  });
});
