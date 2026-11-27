// pjson: the key hash against a reference implementation, the shared corpus
// of conformance/fixtures and conformance/fixtures-reject, and mutation
// fuzzing (untrusted bytes fail only with a PjsonError).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { createXXHash3 } from 'hash-wasm';
import { PjsonError, pjsonCanonicalize, pjsonDecode, pjsonEncode, pjsonLazy, pjsonValidate } from '../index.js';
import { xxh3 } from '../xxh3.js';

function prng(seed: number): () => number {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

test('xxh3: equal to the reference XXH3-64 at every length class', async () => {
  const reference = await createXXHash3();
  const random = prng(0x1234);
  const lengths = [...Array(300).keys(), 512, 1023, 1024, 1025, 2047, 2048, 2049, 4096, 5000];
  for (const length of lengths) {
    for (let round = 0; round < 3; ++round) {
      const input = Uint8Array.from({ length }, () => Math.floor(random() * 256));
      reference.init().update(input);
      assert.equal(xxh3(input).toString(16).padStart(16, '0'), reference.digest('hex'), `length ${length}`);
    }
  }
});

const corpus = new URL('../../../conformance/', import.meta.url);
function files(dir: URL): URL[] {
  const out: URL[] = [];
  for (const entry of readdirSync(dir, { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : 1))) {
    if (entry.isDirectory()) out.push(...files(new URL(`${entry.name}/`, dir)));
    else if (entry.name.endsWith('.json')) out.push(new URL(entry.name, dir));
  }
  return out;
}
interface Fixture {
  id: string;
  wire_hex: string;
  must_round_trip?: boolean;
  must_validate?: boolean;
  must_reject?: boolean;
}
const hex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');

// conformance/fixtures and fixtures-reject are the corpus of the historical
// pjson draft (docs/wire-contract.md: "preserved for provenance ... not
// release acceptance evidence"). Revision 2, which C++, Rust and this
// package implement, shares its type numbers but not every draft feature.
// The corpus is replayed in full; a fixture whose outcome differs under
// revision 2 is listed here with the reason, and the lists are exact.
const DRAFT_ONLY: { [id: string]: string } = {
  canonical_nan_f128: 'binary128',
  ieee_float_f128_minus_one: 'binary128',
  ieee_float_f128_one: 'binary128',
  ieee_float_f128_pos_inf: 'binary128',
  bytes_base58_deadbeef: 'bytes presentation flag',
  bytes_base64url_deadbeef: 'bytes presentation flag',
  bytes_hex_deadbeef: 'bytes presentation flag',
  extension_inside_object: 'extension payload',
  extension_subtype_0_empty: 'extension payload',
  extension_subtype_15_max: 'extension payload',
  extension_subtype_5_small: 'extension payload',
  json_ingress_extension_envelope: 'extension payload',
  numeric_string_snowflake_class: 'numeric-string wrapper',
  numeric_string_wrapping_decimal_3_14: 'numeric-string wrapper',
  numeric_string_wrapping_nint_inline_minus_1: 'numeric-string wrapper',
  numeric_string_wrapping_uint_42: 'numeric-string wrapper',
  numeric_string_wrapping_uint_inline_5: 'numeric-string wrapper',
  emitter_numeric_string_unaffected_by_mode: 'numeric-string wrapper',
  json_ingress_lift_canonical_decimal: 'numeric-string wrapper',
  json_ingress_lift_canonical_int: 'numeric-string wrapper',
  row_array_empty: 'a row array with no keys (C++ rejects K == 0)',
};
const DRAFT_ONLY_REJECT: { [id: string]: string } = {
  ieee_float_bit3_set_reserved: 'IEEE bit 3 is the notation hint in revision 2',
};

test('pjson corpus: conformance/fixtures and conformance/fixtures-reject', () => {
  const count = { fixtures: 0, decoded: 0, reencodedSameBytes: 0, draftOnly: 0, noCanonicalForm: 0, rejects: 0, rejected: 0, draftOnlyReject: 0 };
  const draftOnly: string[] = [];
  for (const file of files(new URL('fixtures/', corpus))) {
    const fx: Fixture = JSON.parse(readFileSync(file, 'utf8'));
    const bytes = Uint8Array.from(Buffer.from(fx.wire_hex, 'hex'));
    ++count.fixtures;
    if (fx.id in DRAFT_ONLY) {
      // Unsupported in revision 2: refused with a typed error.
      assert.equal(pjsonLazy.validate(bytes), false, fx.id);
      assert.throws(() => pjsonLazy.decode(bytes), PjsonError, fx.id);
      draftOnly.push(fx.id);
      ++count.draftOnly;
      continue;
    }
    assert.equal(pjsonLazy.validate(bytes), true, fx.id);
    const value = pjsonLazy.decode(bytes);
    ++count.decoded;
    // What decodes re-encodes to bytes that decode again (the draft's
    // canonical-form rules are not revision 2's: equal bytes are counted).
    // (A draft string that is not text has no canonical form.)
    let again: Uint8Array | undefined;
    try { again = pjsonEncode(value); } catch (e) { assert.ok(e instanceof PjsonError, fx.id); ++count.noCanonicalForm; }
    if (again) {
      assert.equal(pjsonValidate(again), true, fx.id);
      if (hex(again) === fx.wire_hex) ++count.reencodedSameBytes;
    }
  }
  const draftOnlyReject: string[] = [];
  for (const file of files(new URL('fixtures-reject/', corpus))) {
    const fx: Fixture = JSON.parse(readFileSync(file, 'utf8'));
    const bytes = Uint8Array.from(Buffer.from(fx.wire_hex, 'hex'));
    ++count.rejects;
    if (fx.id in DRAFT_ONLY_REJECT) {
      assert.equal(pjsonLazy.validate(bytes), true, fx.id);
      draftOnlyReject.push(fx.id);
      ++count.draftOnlyReject;
      continue;
    }
    assert.equal(pjsonLazy.validate(bytes), false, fx.id);
    assert.throws(() => pjsonLazy.decode(bytes), PjsonError, fx.id);
    ++count.rejected;
  }
  console.log('pjson draft corpus:', JSON.stringify({ ...count, draftOnlyReasons: DRAFT_ONLY, draftOnlyRejectReasons: DRAFT_ONLY_REJECT }));
  assert.deepEqual(draftOnly.sort(), Object.keys(DRAFT_ONLY).sort());
  assert.deepEqual(draftOnlyReject.sort(), Object.keys(DRAFT_ONLY_REJECT).sort());
  assert.equal(count.decoded + count.draftOnly, count.fixtures);
  assert.equal(count.rejected + count.draftOnlyReject, count.rejects);
  assert.equal(count.fixtures, 105);
  assert.equal(count.rejects, 21);
});

test('pjson: truncated and mutated bytes fail only with a PjsonError', () => {
  const random = prng(0xbead);
  const sources: Uint8Array[] = [];
  for (const file of files(new URL('fixtures/', corpus)))
    sources.push(Uint8Array.from(Buffer.from((JSON.parse(readFileSync(file, 'utf8')) as Fixture).wire_hex, 'hex')));
  sources.push(pjsonEncode({ id: 42n, name: 'Alice', tags: ['a', 'b'], nested: { 'a.b': [1.5, null, true] } }));
  const stats = { frames: 0, accepted: 0, rejected: 0, canonical: 0 };
  const check = (bytes: Uint8Array, what: string) => {
    ++stats.frames;
    // `valid`: a well-formed frame (the lazy tag). `canonical`: the one
    // encoding of its value (pjson).
    const valid = pjsonLazy.validate(bytes), canonical = pjsonValidate(bytes);
    assert.ok(!canonical || valid, `${what}: canonical and not well-formed`);
    let decodes = true;
    try { pjsonDecode(bytes); } catch (e) { decodes = false; assert.ok(e instanceof PjsonError, `${what}: threw ${String(e)}`); }
    assert.equal(decodes, canonical, `${what}: validate and decode disagree`);
    try {
      pjsonLazy.decode(bytes);
      assert.ok(valid, `${what}: validate rejects, decode accepts`);
      ++stats.accepted;
    } catch (e) {
      assert.ok(e instanceof PjsonError, `${what}: threw ${String(e)}`);
      assert.ok(!valid, `${what}: validate accepts, decode rejects`);
      ++stats.rejected;
      return;
    }
    // The canonical form of a well-formed frame: it exists unless the frame
    // holds text that is not text; it is canonical and its own canonical
    // form; and it is the frame itself exactly when the frame is canonical.
    let made: Uint8Array;
    try { made = pjsonCanonicalize(bytes); } catch (e) {
      assert.ok(e instanceof PjsonError, `${what}: threw ${String(e)}`);
      assert.ok(!canonical, `${what}: a canonical frame with no canonical form`);
      return;
    }
    assert.ok(pjsonValidate(made), `${what}: the canonical form is not canonical`);
    assert.equal(hex(pjsonCanonicalize(made)), hex(made), what);
    assert.equal(hex(made) === hex(bytes), canonical, what);
    stats.canonical += canonical ? 1 : 0;
  };
  sources.forEach((good, n) => {
    check(good, `source ${n}`);
    for (let cut = 0; cut < good.length; ++cut) check(good.subarray(0, cut), `source ${n} cut ${cut}`);
    for (let i = 0; i < Math.min(good.length * 4, 256); ++i) {
      const mutated = good.slice();
      const at = Math.floor(random() * good.length);
      mutated[at] = i % 3 === 0 ? mutated[at] ^ (1 << Math.floor(random() * 8)) : i % 3 === 1 ? Math.floor(random() * 256) : 0xff;
      check(mutated, `source ${n} byte ${at} = ${mutated[at]}`);
    }
  });
  assert.throws(() => pjsonDecode('00' as never), PjsonError);
  assert.equal(pjsonValidate(null as never), false);
  console.log('pjson fuzz:', JSON.stringify(stats));
  assert.ok(stats.accepted > 500 && stats.rejected > 2000);
});
