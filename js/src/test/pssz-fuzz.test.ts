// Property tests over the shared fixtures: truncations, single-byte
// mutations and one-byte extensions of valid frames and texts. Untrusted
// input must never crash or hang, must fail only with a typed PsszError, and
// must agree with validate: whatever validate accepts decodes, and
// re-encodes to the same bytes (a value has exactly one encoding).
//
// Both tests run twice: on the schema-driven path and on the typed layer
// (the fixture's types declared with the typed definitions). Every frame is
// also read through a view: walking a view fails only with a typed
// PsszError, never reads outside the frame, and on bytes that validate it
// yields exactly the decoded value.

import { test } from 'node:test';
import { fixtureHandlers } from './support/custom-handlers.js';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  ListView, PsszError, PsszNaN, PsszSchema, PsszView, RecordView, TupleView, VariantView,
  type JsonFormat, type PsszType, type PsszWidth,
} from '../index.js';
import { TypedFixture, type TypeSource } from './support/typed-from-ir.js';

const PATHS = ['schema-driven', 'typed'] as const;

const fixture = (name: string) =>
  JSON.parse(readFileSync(new URL(`../../../conformance/${name}`, import.meta.url), 'utf8'));
const fromHex = (hex: string) => Uint8Array.from(Buffer.from(hex, 'hex'));
const toHex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');

// Deterministic PRNG (mulberry32): a failure reproduces.
function prng(seed: number): () => number {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function typed(run: () => unknown, what: string): PsszError | undefined {
  try {
    run();
    return undefined;
  } catch (e) {
    assert.ok(e instanceof PsszError, `${what}: threw ${String(e)}`);
    assert.notEqual(e.kind, 'internal', `${what}: ${e.message}`);
    return e;
  }
}

const fresh = () => ({
  frames: 0, accepted: 0, acceptedUnknown: 0, rejected: 0, texts: 0, textsAccepted: 0,
  viewWalked: 0, viewRejected: 0, viewLazyAccepted: 0,
});
let stats = fresh();

// Reads everything a view can reach, into the shape `decode` gives.
function walk(v: unknown): unknown {
  if (v instanceof RecordView) return v.fields.map((name) => [name, walk(v.get(name))]);
  if (v instanceof TupleView) return Array.from({ length: v.length }, (_, i) => walk(v.at(i)));
  if (v instanceof ListView) {
    // No more elements than the frame can hold (zero-size elements aside).
    const out: unknown[] = [];
    for (const element of v) out.push(walk(element));
    return out;
  }
  if (v instanceof VariantView) return { type: v.type, value: walk(v.value) };
  if (v !== null && typeof v === 'object' && !(v instanceof Uint8Array) && !(v instanceof PsszNaN) && 'some' in v)
    return { some: walk((v as { some: unknown }).some) };
  return v;
}
// One spelling for a decoded value and a walked view.
function show(v: unknown): string {
  return JSON.stringify(v, (_key, x) => {
    if (typeof x === 'bigint') return `${x}n`;
    if (typeof x === 'number' && !Number.isFinite(x)) return `float ${x}`;
    if (typeof x === 'number' && Object.is(x, -0)) return 'float -0';
    if (x instanceof Uint8Array) return `hex ${Buffer.from(x).toString('hex')}`;
    if (x instanceof PsszNaN) return `nan ${x.bits}`;
    if (x !== null && typeof x === 'object' && !Array.isArray(x) && !('type' in x && 'value' in x && Object.keys(x).length === 2)
        && !('some' in x && Object.keys(x).length === 1) && !('bitCount' in x))
      return Object.keys(x).map((name) => [name, (x as Record<string, unknown>)[name]]);
    return x;
  });
}

// The contract of a view over arbitrary bytes.
function checkView(type: PsszType, bytes: Uint8Array, valid: boolean, what: string): void {
  let walked: unknown;
  // A view must stay inside its frame: surround the frame with other bytes.
  const padded = new Uint8Array(bytes.length + 16).fill(0xa5);
  padded.set(bytes, 8);
  const frame = padded.subarray(8, 8 + bytes.length);
  let root: unknown;
  const error = typed(() => {
    root = type.view(frame);
    walked = walk(root);
  }, `${what}: view`);
  if (root instanceof PsszView) {
    // toValue of the root is decode.
    let full: unknown;
    const failed = typed(() => (full = (root as PsszView).toValue()), `${what}: toValue`);
    assert.equal(!failed, valid, `${what}: view.toValue disagrees with validate`);
    if (!failed) assert.equal(show(full), show(type.decode(bytes)), `${what}: toValue`);
  }
  if (error) {
    assert.ok(!valid, `${what}: validate accepts, the view fails: ${error.message}`);
    assert.equal(error.kind, 'bytes', `${what}: ${error.message}`);
    ++stats.viewRejected;
    return;
  }
  if (valid) {
    assert.equal(show(walked), show(type.decode(bytes)), `${what}: view differs from decode`);
    ++stats.viewWalked;
  } else ++stats.viewLazyAccepted; // structurally readable, not canonical
}

// The contract between validate, decode and encode for arbitrary bytes.
function checkFrame(type: PsszType, bytes: Uint8Array, what: string): void {
  ++stats.frames;
  const result = type.validate(bytes);
  checkView(type, bytes, result.ok, what);
  if (!result.ok) {
    assert.ok(result.error instanceof PsszError, what);
    assert.notEqual(result.error.kind, 'internal', `${what}: ${result.error.message}`);
    assert.ok(typed(() => type.decode(bytes), what), `${what}: validate rejects, decode accepts`);
    ++stats.rejected;
    return;
  }
  const value = type.decode(bytes);
  const again = type.encode(value);
  if (!result.hasUnknown) {
    assert.equal(toHex(again), toHex(bytes), `${what}: accepted bytes are not canonical`);
    ++stats.accepted;
  } else {
    // Unknown appended fields are dropped; what remains is canonical.
    const second = type.validate(again);
    assert.ok(second.ok && !second.hasUnknown, what);
    assert.equal(toHex(type.encode(type.decode(again))), toHex(again), what);
    ++stats.acceptedUnknown;
  }
  // Text of an accepted value, when it has one, reads back to valid bytes.
  for (const format of ['ijson', 'json'] as const) {
    let text = '';
    if (typed(() => (text = type.psszToJson(bytes, format)), what)) continue;
    const back = type.jsonToPssz(text, format);
    assert.ok(type.validate(back).ok, `${what}: ${format} ${text}`);
  }
}

for (const path of PATHS) test(`PSSZ frames and views: truncation, mutation and extension agree with validate, ${path}`, () => {
  stats = fresh();
  const fx = fixture('pssz-vectors.json');
  const decoded = PsszSchema.decode(fromHex(fx.schema_ir));
  const schema: TypeSource = path === 'typed' ? new TypedFixture(decoded.ir) : decoded;
  const random = prng(0x5eed);
  const types = new Map<string, PsszType>();
  let sources = 0;
  for (const row of fx.vectors as { name: string; type: string; format: string; hex: string; expect: string }[]) {
    if (row.expect === 'reject') continue;
    const width: PsszWidth = row.format === 'auto' ? 'auto' : row.format === 'pssz8' ? 1 : row.format === 'pssz16' ? 2 : 4;
    const key = `${width}/${row.type}`;
    let type = types.get(key);
    if (!type) types.set(key, (type = schema.type(row.type, { width })));
    const good = fromHex(row.hex);
    ++sources;
    checkFrame(type, good, row.name);
    // Every truncation of a short frame; a spread of cuts of a long one.
    const step = Math.max(1, Math.floor(good.length / 48));
    for (let cut = 0; cut < good.length; cut += step) checkFrame(type, good.subarray(0, cut), `${row.name} cut ${cut}`);
    // Single-byte mutations: a bit flip, a random byte, 0x00 and 0xff.
    const mutations = Math.min(good.length * 2, 64);
    for (let i = 0; i < mutations; ++i) {
      const at = Math.floor(random() * good.length);
      const mutated = good.slice();
      const choice = i % 4;
      mutated[at] =
        choice === 0 ? mutated[at] ^ (1 << Math.floor(random() * 8))
        : choice === 1 ? Math.floor(random() * 256)
        : choice === 2 ? 0
        : 0xff;
      checkFrame(type, mutated, `${row.name} byte ${at} = ${mutated[at]}`);
    }
    // One trailing byte.
    for (const extra of [0, 1, 0xff]) {
      const longer = new Uint8Array(good.length + 1);
      longer.set(good);
      longer[good.length] = extra;
      checkFrame(type, longer, `${row.name} + ${extra}`);
    }
  }
  console.log(`pssz fuzz (${path}):`, JSON.stringify({ sources, ...stats }));
  assert.ok(sources > 4000);
  assert.ok(stats.rejected > 50000 && stats.accepted > 10000);
  assert.ok(stats.viewWalked > 10000 && stats.viewRejected > 10000);
  assert.equal(stats.viewWalked + stats.viewRejected + stats.viewLazyAccepted, stats.frames);
});

for (const path of PATHS) test(`JSON / I-JSON text: truncation and mutation fail typed or round-trip, ${path}`, () => {
  stats = fresh();
  const fx = fixture('json-vectors.json');
  const schema = PsszSchema.decode(fromHex(fx.schema_ir));
  // The fixture's custom text forms are registered, as its texts need.
  const handled: TypeSource =
    path === 'typed' ? new TypedFixture(schema.ir, fixtureHandlers) : schema.withHandlers(fixtureHandlers);
  const random = prng(0xfeed);
  const expressible = new Set((fx.types as { id: string; ir: boolean }[]).filter((t) => t.ir).map((t) => t.id));
  const checkText = (type: PsszType, format: JsonFormat, text: Uint8Array, what: string) => {
    ++stats.texts;
    let bytes: Uint8Array = new Uint8Array(0);
    const error = typed(() => (bytes = type.jsonToPssz(text, format)), what);
    if (error) {
      assert.ok(error.kind === 'json' || error.kind === 'value', `${what}: ${error.kind}`);
      return;
    }
    ++stats.textsAccepted;
    // Accepted text gives valid bytes whose text is a fixed point.
    assert.ok(type.validate(bytes).ok, what);
    const canonical = type.psszToJson(bytes, format);
    assert.equal(toHex(type.jsonToPssz(canonical, format)), toHex(bytes), what);
    assert.equal(type.psszToJson(type.jsonToPssz(canonical, format), format), canonical, what);
  };
  const alphabet = Buffer.from('{}[]",:\\0123456789-+.eEtfnu aA\u0000\u007fÿ', 'latin1');
  for (const row of fx.vectors as { name: string; type: string; kind: string; json?: string; ijson?: string }[]) {
    if (row.kind !== 'value' || !expressible.has(row.type)) continue;
    const type = handled.type(row.type);
    for (const format of ['json', 'ijson'] as const) {
      const good = Buffer.from(row[format]!, 'utf8');
      const step = Math.max(1, Math.floor(good.length / 32));
      for (let cut = 0; cut < good.length; cut += step)
        checkText(type, format, good.subarray(0, cut), `${row.name} ${format} cut ${cut}`);
      for (let i = 0; i < Math.min(good.length * 2, 48); ++i) {
        const at = Math.floor(random() * good.length);
        const mutated = Uint8Array.from(good);
        mutated[at] = alphabet[Math.floor(random() * alphabet.length)];
        checkText(type, format, mutated, `${row.name} ${format} byte ${at} = ${mutated[at]}`);
      }
    }
  }
  console.log(`json fuzz (${path}):`, JSON.stringify({ texts: stats.texts, accepted: stats.textsAccepted }));
  assert.ok(stats.texts > 20000 && stats.textsAccepted > 500);
});
