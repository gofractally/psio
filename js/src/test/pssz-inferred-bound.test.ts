// The inferred size bound of variants and tuples (docs/pssz-spec.md 3.2):
//   Variant(Ts...)  1 + the widest alternative
//   Tuple(Ts...)    the extensible record of its elements: header, then per
//                   element its payload (+ an offset slot when variable)
//   Pair(A, B)      Tuple(A, B)
// with the header and slots counted at the type's own width: the least of
// 1, 2 and 4 bytes whose worst case fits (`fit`, docs/pssz-spec.md 3.2). A
// bounded tuple / variant, and a record that holds one, gets one- or
// two-byte offsets instead of four. Bit types are bounded too. The widths and golden bytes are those of
// cpp/tests/pssz_inferred_bound_tests.cpp; the typed layer and the
// schema-driven codec (over the schema as it travels) agree on each.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  PsszSchema, array, bitlist, bitvector, boundedList, boundedUtf8, generateSchema, list, map, optional, pair, record,
  set, tuple, u16, u32, u64, u8, utf8, variant, type TypeDef,
} from '../index.js';

const toHex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');
const fromHex = (hex: string) => Uint8Array.from(Buffer.from(hex, 'hex'));

const Name = boundedUtf8(32);
const Point = record('Point', { x: u32, y: u32 }, { fixed: true });
const Capped = record('Capped', { text: utf8 }, { maxDynamicData: 300 });
const Free = record('Free', { text: utf8 });
const Event = record('Event', { at: u64, what: variant({ a: u8, b: Point, c: u64 }) });
const Call = record('Call', { method: u32, args: tuple(u64, Name, u8) });
const Open = record('Open', { id: u32, what: variant({ a: u8, b: list(u8) }) });

/** The automatic width, typed and from the schema as it travels. */
function width(def: TypeDef<any, any>): number {
  const fetched = PsszSchema.decode(generateSchema({ Root: def }).encode()).type('Root');
  assert.equal(fetched.width, def.width);
  // An explicit profile is not affected by the bound.
  for (const w of [1, 2, 4] as const) assert.equal(def.type({ width: w }).width, w);
  return def.width;
}

/** One value on the typed and the schema-driven codec. */
function agree<T>(def: TypeDef<T, any>, value: T, wire: string): void {
  const hex = wire.replace(/ /g, '');
  assert.equal(toHex(def.encode(value)), hex);
  assert.deepEqual(def.validate(fromHex(hex)), { ok: true, hasUnknown: false });
  assert.deepEqual(def.decode(fromHex(hex), { rejectUnknown: true }), value);
  const fetched = PsszSchema.decode(generateSchema({ Root: def }).encode()).type('Root');
  assert.equal(fetched.width, def.width);
  assert.deepEqual(fetched.validate(fromHex(hex)), { ok: true, hasUnknown: false });
  assert.equal(toHex(fetched.encode(fetched.decode(fromHex(hex)))), hex);
}

test('pssz inferred bound: the width a tuple, pair or variant-holding record takes', () => {
  // (A variant has no header or slots of its own; its bound shows in the
  // width of the tuple / record that holds it.) W + (1 + widest) + W <= 255.
  assert.equal(width(tuple(variant({ a: u8 }))), 1);
  assert.equal(width(tuple(variant({ a: u8, b: Name }))), 1);                  // 1 + 33 + 1
  assert.equal(width(tuple(variant({ a: u8, b: variant({ c: u16, d: u64 }) }))), 1);
  assert.equal(width(tuple(variant({ a: u8, b: array(u8, 252) }))), 1);        // 1 + 253 + 1 = 255
  assert.equal(width(tuple(variant({ a: u8, b: array(u8, 253) }))), 2);        // 2 + 254 + 2
  // An alternative with an explicit cap contributes the cap.
  assert.equal(width(tuple(variant({ a: u8, b: Capped }))), 2);                // 2 + 301 + 2
  // Unbounded if any alternative is.
  assert.equal(width(tuple(variant({ a: u8, b: utf8 }))), 4);
  assert.equal(width(tuple(variant({ a: list(u8), b: u8 }))), 4);
  assert.equal(width(tuple(variant({ a: u8, b: Free }))), 4);

  // Tuple: header W, each element its payload, + W when variable.
  assert.equal(width(tuple()), 1);
  assert.equal(width(tuple(u32)), 1);
  assert.equal(width(tuple(u8, u16, u32, u64)), 1);
  assert.equal(width(tuple(u64, Name, u8)), 1);
  assert.equal(width(tuple(optional(u32))), 1);
  assert.equal(width(tuple(tuple(u8), u8)), 1);
  // The thresholds: the header counts at the tuple's own width.
  assert.equal(width(tuple(array(u8, 254))), 1);                               // 1 + 254 = 255
  assert.equal(width(tuple(array(u8, 255))), 2);                               // 2 + 255 = 257
  assert.equal(width(tuple(array(u8, 65533))), 2);                             // 2 + 65533 = 65535
  assert.equal(width(tuple(array(u8, 65534))), 4);                             // 4 + 65534 = 65538
  assert.equal(width(tuple(Capped, u8)), 2);                                   // 2 + 300 + 2 + 1 = 305
  assert.equal(width(tuple(array(u64, 40))), 2);                               // 322
  assert.equal(width(tuple(array(u64, 9000))), 4);                             // 72004
  // An element has no member annotation: a string or list is unbounded.
  assert.equal(width(tuple(u32, utf8)), 4);
  assert.equal(width(tuple(list(u8))), 4);
  assert.equal(width(tuple(Free)), 4);
  // A pair is the 2-tuple.
  assert.equal(width(pair(u32, u8)), 1);
  assert.equal(width(pair(u32, Name)), width(tuple(u32, Name)));
  assert.equal(width(pair(utf8, u8)), 4);

  // A record holding a bounded variant / tuple is bounded itself.
  assert.equal(width(Event), 1);
  assert.equal(width(Call), 1);
  assert.equal(width(Open), 4);
  // An unbounded list has a four-byte offset table whatever its elements.
  assert.equal(width(list(tuple(u32))), 4);
  assert.equal(width(map(u32, u8)), 4);
  assert.equal(width(set(tuple(u8))), 4);

  // Bit types: ceil(N / 8) bytes; a bitlist one more for its delimiter. A
  // bitvector is inline (no slot); a bitlist has one slot.
  assert.equal(width(record('BitFlags', { id: u8, flags: bitvector(12), seen: bitlist(12) })), 1);
  // record{bitlist<N>} is 2 * W + ceil(N / 8) + 1.
  assert.equal(width(record('BitList2016', { seen: bitlist(2016) })), 1);     // 255
  assert.equal(width(record('BitList2017', { seen: bitlist(2017) })), 2);     // 258
  assert.equal(width(boundedList(bitlist(12), 10)), 1);                        // 10 * (1 + 3)
  assert.equal(width(tuple(bitvector(12), bitlist(12))), 1);                   // 1 + 2 + 1 + 3
});

test('pssz inferred bound: tuples take the width their bound allows', () => {
  // One-byte header and no slots: all elements fixed.
  agree(tuple(), [], '00');
  agree(tuple(u32), [9], '04 09000000');
  agree(pair(u32, u8), [9, 7], '05 09000000 07');
  // A variable element: one-byte header, one-byte offset slot.
  agree(tuple(u64, Name, u8), [1n, 'bob', 2], '0a 0100000000000000 0a 02 626f62');
  // Trailing optionals prune as in any record.
  agree(tuple(u8, optional(u32)), [1, null], '01 01');
  agree(tuple(u8, optional(u32)), [1, 5], '02 01 02 05000000');
  // Two-byte offsets.
  agree(tuple(Capped, u8), [{ text: 'hi' }, 3], '0300 0300 03 0200 0200 6869');
  // An unbounded element keeps the tuple at four bytes.
  agree(tuple(u32, utf8), [9, 'hi'], '08000000 09000000 08000000 6869');
});

test('pssz inferred bound: a variant bounds the record that holds it', () => {
  // The variant itself has no offsets: selector, payload.
  agree(variant({ a: u8, b: u32 }), { type: 'b', value: 9 }, '01 09000000');
  agree(variant({ a: u8, b: Point }), { type: 'b', value: { x: 1, y: 2 } }, '01 0100000002000000');
  // Event: one-byte header (9) and slot.
  agree(Event, { at: 7n, what: { type: 'a', value: 5 } }, '09 0700000000000000 09 00 05');
  agree(Event, { at: 7n, what: { type: 'b', value: { x: 1, y: 2 } } }, '09 0700000000000000 09 01 0100000002000000');
  // Call: the argument tuple inside a record, both at one byte.
  agree(Call, { method: 3, args: [1n, 'bob', 2] }, '05 03000000 05 0a 0100000000000000 0a 02 626f62');
});

test('pssz inferred bound: map entries are bounded tuples', () => {
  // An unbounded list's offset table is four bytes wide; each entry is the
  // 2-tuple at its own width (one byte: header 5, u32, u8).
  agree(map(u32, u8), new Map([[1, 2], [3, 4]]), '08000000 0e000000 05 01000000 02 05 03000000 04');
  agree(list(tuple(u32, u8)), [[1, 2]], '04000000 05 01000000 02');
});
