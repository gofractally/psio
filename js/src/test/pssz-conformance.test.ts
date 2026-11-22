// Cross-language conformance: the schema-driven PSSZ / JSON / I-JSON codec
// against the two shared fixtures, using nothing but the Schema IR recorded
// in them. The C++ and Rust counterparts are
// cpp/tests/schema_conformance_tests.cpp and
// rust/psio/tests/schema_conformance.rs.
//
// Every vector of both fixtures is visited and accounted for: the totals at
// the end must add up to the fixture sizes, so nothing is skipped silently.
//
// Each fixture is replayed twice: through the schema-driven path (the IR as
// fetched) and through the typed layer (every fixture type declared with
// the typed definitions, `support/typed-from-ir.ts`; `typed-ir.test.ts`
// checks that those declarations generate the fixture's IR).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  IR_VERSION, PsszError, PsszSchema, PsszView, validatePssz, type JsonFormat, type PsszNestedBytes, type PsszType,
  type PsszWidth,
} from '../index.js';
import { fixtureHandlers } from './support/custom-handlers.js';
import { TypedFixture, type TypeSource } from './support/typed-from-ir.js';

const PATHS = ['schema-driven', 'typed'] as const;

const fixture = (name: string) =>
  JSON.parse(readFileSync(new URL(`../../../conformance/${name}`, import.meta.url), 'utf8'));
const fromHex = (hex: string) => Uint8Array.from(Buffer.from(hex, 'hex'));
const toHex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');

interface PsszTypeRow {
  id: string;
  ir: boolean;
  auto_width: number;
  /** The id of the type's `Custom` node: its handler is registered under it. */
  custom_id?: string;
  /** An invariant constrains values of the type (its own, or one it holds). */
  invariant?: boolean;
}
interface PsszRow {
  name: string;
  class: string;
  type: string;
  format: 'auto' | 'pssz8' | 'pssz16' | 'pssz32';
  hex: string;
  expect: 'canonical' | 'unknown' | 'reject';
  /** For `reject`: the C++ error (class `invariant`: the invariant's message). */
  reason: string;
  reencoded: string;
  /** Id of the logical value written, or empty for a hand-made frame. */
  value: string;
  writer: string;
}
interface JsonTypeRow {
  id: string;
  ir: boolean;
  pssz: boolean;
  /** The typed codec's automatic width (present when `pssz`). */
  auto_width?: number;
  /** The type holds a custom text form: its value rows carry `plain_*`. */
  custom?: boolean;
  /** An invariant constrains values of the type: it has `invariant` rows. */
  invariant?: boolean;
}
interface JsonRow {
  name: string;
  type: string;
  kind: 'value' | 'accept' | 'reject' | 'invariant';
  format?: JsonFormat;
  json?: string;
  ijson?: string;
  text?: string;
  text_hex?: string;
  canonical?: string;
  pssz?: string;
  /** The text with no custom handler registered (the wire type's own). */
  plain_json?: string;
  plain_ijson?: string;
  /** Kind `invariant`: the invariant's message. */
  reason?: string;
}

function rejects(run: () => unknown, name: string): PsszError {
  try {
    run();
  } catch (e) {
    assert.ok(e instanceof PsszError, `${name}: threw ${String(e)}`);
    assert.notEqual(e.kind, 'internal', `${name}: ${e.message}`);
    return e;
  }
  return assert.fail(`${name}: accepted`);
}

// JSON has one null: Some(None) of a nested optional reads back as None (in
// the typed text codecs too), so such a value does not round-trip via text.
function hasSomeNone(value: unknown): boolean {
  if (typeof value !== 'object' || value === null || value instanceof Uint8Array) return false;
  if (Array.isArray(value)) return value.some(hasSomeNone);
  if (value instanceof Set) return [...value].some(hasSomeNone);
  if (value instanceof Map) return [...value].some(hasSomeNone);
  const keys = Object.keys(value);
  const record = value as Record<string, unknown>;
  if (keys.length === 1 && keys[0] === 'some' && record.some === null) return true;
  return keys.some((key) => hasSomeNone(record[key]));
}

// The classes of pssz-vectors.json and their sizes, as conformance/pssz-vectors.md
// lists them: every one is replayed in full, and a fixture that changes must
// be looked at.
const PSSZ_CLASSES: { [cls: string]: number } = {
  corpus: 956, 'evolution-forward': 660, 'evolution-backward': 1380, pruning: 212, malformed: 419,
  'optional-presence': 400, width: 309, 'cap-overflow-explicit-nested': 15, tuple: 60, variant: 48, enum: 76,
  'enum-unknown-value': 36, string: 308, 'string-raw-non-utf8': 44, 'length-bound-overflow': 32, box: 100,
  bytes: 88, 'zero-fixed': 100, depth: 28, nested: 252, 'nested-unknown': 20, 'nested-deferred': 24,
  'map-set': 266, 'map-set-order': 352, transparent: 160, custom: 116, invariant: 128,
};
const NO_HANDLER = 'pssz IR: no handler registered for a custom type';

for (const path of PATHS) test(`PSSZ fixture: every type and vector, ${path}`, () => {
  const fx: { schema_ir: string; types: PsszTypeRow[]; vectors: PsszRow[] } = fixture('pssz-vectors.json');
  const schema = PsszSchema.decode(fromHex(fx.schema_ir));
  // Three registries (conformance/custom-types.md): none, the handlers of
  // the fixture's custom types, and a strict one that lacks them.
  const typedPlain = new TypedFixture(schema.ir);
  const source: TypeSource = path === 'typed' ? typedPlain : schema;
  const handled: TypeSource = path === 'typed' ? new TypedFixture(schema.ir, fixtureHandlers) : schema.withHandlers(fixtureHandlers);
  const strict: TypeSource = path === 'typed'
    ? { type: (id, options) => typedPlain.type(id, { ...options, requireHandlers: true }) }
    : PsszSchema.decode(fromHex(fx.schema_ir), { requireHandlers: true });
  // The IR travels as its own PSSZ encoding and re-encodes to the same bytes.
  assert.equal(toHex(schema.encode()), fx.schema_ir);

  // Width fidelity: for EVERY type the schema-derived automatic width is the
  // typed codec's (recorded by the emitter as `auto_width`).
  const notExpressible = fx.types.filter((t) => !t.ir).map((t) => t.id);
  assert.deepEqual(notExpressible, []);
  let widthsChecked = 0;
  const byName = new Map(schema.ir.types.map((m) => [m.name, m.type]));
  const manifest = new Map(fx.types.map((t) => [t.id, t]));
  let customIds = 0;
  for (const row of fx.types) {
    assert.ok(row.auto_width === 1 || row.auto_width === 2 || row.auto_width === 4, row.id);
    assert.equal(source.type(row.id).width, row.auto_width, `auto width of ${row.id}`);
    assert.equal(handled.type(row.id).width, row.auto_width, `auto width of ${row.id}, with handlers`);
    ++widthsChecked;
    // Manifest key `custom_id`: the id of the type's Schema IR `Custom`
    // node, under which its handler is registered.
    let node = byName.get(row.id)!.value;
    for (let hops = 0; node.type === 'Type' && hops < 64; ++hops) node = byName.get(node.value.type)!.value;
    const isCustom = node.type === 'Custom' && Object.hasOwn(fixtureHandlers, node.value.id);
    assert.equal(isCustom, row.custom_id !== undefined, `custom_id of ${row.id}`);
    if (row.custom_id !== undefined) {
      assert.equal(node.type === 'Custom' && node.value.id, row.custom_id, `custom_id of ${row.id}`);
      ++customIds;
    }
    assert.ok(row.invariant === undefined || row.invariant === true, row.id);
  }
  assert.equal(widthsChecked, fx.types.length);
  assert.equal(customIds, 8);
  // A box gives its container no inferred bound; an explicit cap still does.
  const widthOf = (id: string) => source.type(id).width;
  assert.deepEqual(
    ['BDirect', 'BoxRec', 'BoxCapRec', 'BoxVecCapRec', 'box<BLeaf>', 'optional<BoxCapRec>'].map(widthOf),
    [1, 4, 1, 2, 4, 1],
  );

  const types = new Map<string, PsszType | PsszError>();
  const typeOf = (from: TypeSource, tag: string, id: string, width: PsszWidth, nestedBytes: PsszNestedBytes = 'typed'): PsszType => {
    const key = `${tag}/${nestedBytes}/${width}/${id}`;
    let type = types.get(key);
    if (!type) {
      try {
        type = from.type(id, nestedBytes === 'typed' ? { width } : { width, nestedBytes });
      } catch (e) {
        assert.ok(e instanceof PsszError);
        type = e;
      }
      types.set(key, type);
    }
    // Every fixture type builds under every profile. (The explicit profiles
    // enforce maxDynamicData on the top-level type only, as the typed
    // psio::pssz_<W> codecs that produced the fixture do.)
    if (type instanceof PsszError) assert.fail(`${id} (${tag}, ${width}): ${type.message}`);
    return type;
  };

  const count = {
    auto: {
      vectors: 0, validateAccept: 0, validateReject: 0, canonicalReencode: 0, unknownReencode: 0,
      jsonRoundTrip: 0, ijsonRoundTrip: 0, someNoneLossy: 0, noJsonForm: 0,
      ijsonNoncharacter: 0, rawNonUtf8NoText: 0, handlerTextRoundTrip: 0,
      invariant: 0, nestedUnknown: 0, nestedDeferred: 0,
    },
    explicit: {
      vectors: 0, validateAccept: 0, validateReject: 0, canonicalReencode: 0,
      unknownReencode: 0, invariant: 0, nestedUnknown: 0, nestedDeferred: 0,
    },
  };
  const classes = new Map<string, number>();
  const rowByName = new Map(fx.vectors.map((v) => [v.name, v]));
  assert.equal(rowByName.size, fx.vectors.length, 'vector names are unique');

  for (const row of fx.vectors) {
    const bytes = fromHex(row.hex);
    const cls = row.name.slice(0, row.name.indexOf('/'));
    assert.equal(row.class, cls, row.name);
    const automatic = row.format === 'auto';
    const width: PsszWidth = automatic ? 'auto' : row.format === 'pssz8' ? 1 : row.format === 'pssz16' ? 2 : 4;
    const c = automatic ? count.auto : count.explicit;
    ++c.vectors;
    classes.set(cls, (classes.get(cls) ?? 0) + 1);
    assert.equal(row.reason !== '', row.expect === 'reject', `${row.name}: reason`);

    // ── Class `invariant`: a structurally valid frame of a value that
    // violates an invariant. `reject` is the typed outcome, and that of a
    // schema-driven codec given the handlers, under every width profile;
    // with no handler the frame is its wire type; a strict registry that
    // lacks the handler refuses the type.
    if (cls === 'invariant') {
      assert.equal(row.expect, 'reject', row.name);
      assert.equal(manifest.get(row.type)?.invariant, true, `${row.name}: manifest invariant`);
      const plain = typeOf(source, 'plain', row.type, width);
      assert.deepEqual(plain.validate(bytes), { ok: true, hasUnknown: false }, `${row.name}: no handler`);
      const value = plain.decode(bytes);
      assert.equal(toHex(plain.encode(value)), row.hex, `${row.name}: no handler, re-encode`);
      const checked = typeOf(handled, 'handled', row.type, width);
      const refused = checked.validate(bytes);
      assert.ok(!refused.ok, `${row.name}: accepted with the handlers`);
      assert.equal(refused.error.message, row.reason, row.name);
      assert.equal(refused.error.kind, 'bytes', row.name);
      assert.equal(refused.error.invariant, true, row.name);
      assert.equal(rejects(() => checked.decode(bytes), row.name).message, row.reason, row.name);
      // A view does not check; decoding through it does.
      const view = checked.view(bytes);
      if (view instanceof PsszView)
        assert.equal(rejects(() => view.toValue(), row.name).message, row.reason, `${row.name}: view.toValue`);
      // No writer writes the value back: binary, key and text.
      const written = rejects(() => checked.encode(value), `${row.name}: encode`);
      assert.equal(written.message, row.reason, row.name);
      assert.equal(written.kind, 'value', row.name);
      assert.equal(written.invariant, true, row.name);
      if (automatic) {
        for (const format of ['json', 'ijson'] as const) {
          assert.equal(rejects(() => checked.psszToJson(bytes, format), row.name).message, row.reason, `${row.name}: ${format}`);
          assert.equal(rejects(() => checked.valueToJson(value, format), row.name).message, row.reason, `${row.name}: ${format} value`);
          // The wire type's text reads back to the frame.
          const text = plain.psszToJson(bytes, format);
          assert.equal(toHex(plain.jsonToPssz(text, format)), row.hex, `${row.name}: plain ${format}`);
        }
      }
      const required = typeOf(strict, 'strict', row.type, width).validate(bytes);
      assert.ok(!required.ok, `${row.name}: accepted by a strict registry`);
      assert.equal(required.error.message, NO_HANDLER, row.name);
      assert.equal(required.error.invariant, false, row.name);
      ++c.invariant;
      continue;
    }

    // A typed nested<T,deferred> does not look inside its bytes. The schema
    // does not record that policy, so the schema-driven reader gives the
    // recorded outcome when told the nested bytes are opaque; the typed
    // declarations say `deferred` themselves.
    const deferred = cls === 'nested-deferred';
    const mode: PsszNestedBytes = deferred && path === 'schema-driven' ? 'opaque' : 'typed';
    const type = typeOf(source, 'plain', row.type, width, mode);
    const result = type.validate(bytes);
    if (!result.ok) assert.notEqual(result.error.kind, 'internal', `${row.name}: ${result.error.message}`);
    // The handlers change nothing for a value that satisfies its
    // invariants, or for a type that has none.
    const withHandlers = typeOf(handled, 'handled', row.type, width, mode);
    assert.equal(withHandlers.validate(bytes).ok, result.ok, `${row.name}: with handlers`);

    if (deferred) {
      // Any inner bytes (valid, garbage, empty, unknown fields) are
      // canonical, and kept as they are.
      assert.equal(row.expect, 'canonical', row.name);
      assert.deepEqual(result, { ok: true, hasUnknown: false }, row.name);
      assert.equal(toHex(type.encode(type.decode(bytes))), row.hex, row.name);
      assert.deepEqual(type.decode(bytes, { rejectUnknown: true }), type.decode(bytes), row.name);
      // The same through the option alone, on both paths.
      const opaque = typeOf(source, 'plain', row.type, width, 'opaque');
      assert.deepEqual(opaque.validate(bytes), { ok: true, hasUnknown: false }, row.name);
      assert.equal(toHex(opaque.encode(opaque.decode(bytes))), row.hex, row.name);
      ++c.nestedDeferred;
      ++c.validateAccept;
      ++c.canonicalReencode;
      continue;
    }

    if (cls === 'nested-unknown' && row.expect === 'unknown') {
      // Unknown fields inside nested bytes. The value of a nested node is
      // the inner value, so the default re-encoding is canonical: the frame
      // of the `nested` vector holding the same logical value for this
      // reader. With the bytes opaque the frame is kept, which is what the
      // typed C++ reader's re-encoding (`reencoded`) is.
      assert.deepEqual(result, { ok: true, hasUnknown: true }, row.name);
      rejects(() => type.decode(bytes, { rejectUnknown: true }), row.name);
      const canonicalAgain = type.encode(type.decode(bytes));
      assert.notEqual(toHex(canonicalAgain), row.hex, row.name);
      assert.deepEqual(type.validate(canonicalAgain), { ok: true, hasUnknown: false }, row.name);
      assert.equal(toHex(type.encode(type.decode(canonicalAgain, { rejectUnknown: true }))), toHex(canonicalAgain), row.name);
      if (row.value !== '') {
        const same = rowByName.get(`nested/${row.value}/as/${row.type}/${row.format}`);
        assert.ok(same && same.expect === 'canonical', `${row.name}: no nested vector of the same value`);
        assert.equal(toHex(canonicalAgain), same.hex, row.name);
      }
      const opaque = typeOf(source, 'plain', row.type, width, 'opaque');
      assert.deepEqual(opaque.validate(bytes), { ok: true, hasUnknown: false }, row.name);
      const kept = toHex(opaque.encode(opaque.decode(bytes)));
      assert.equal(kept, row.reencoded, row.name);
      assert.equal(kept, row.hex, row.name);
      ++c.nestedUnknown;
      ++c.validateAccept;
      ++c.unknownReencode;
      continue;
    }

    // Non-UTF-8 bytes in a raw string (Custom "string") are valid PSSZ, as
    // for the typed codecs: recorded canonical, replayed like any other row.
    if (cls === 'string-raw-non-utf8') assert.equal(row.expect, 'canonical', row.name);
    if (row.expect === 'reject') {
      assert.equal(result.ok, false, `${row.name}: accepted`);
      rejects(() => type.decode(bytes), row.name);
      // A map / set out of order is refused for its order.
      if (cls === 'map-set-order' && !result.ok)
        assert.equal(result.error.message.replace(/^pssz IR: /, 'pssz: '), row.reason, row.name);
      ++c.validateReject;
      continue;
    }
    assert.ok(result.ok, `${row.name}: ${!result.ok && result.error.message}`);
    assert.equal(result.hasUnknown, row.expect === 'unknown', row.name);
    ++c.validateAccept;
    const value = type.decode(bytes);
    const again = toHex(type.encode(value));
    if (row.expect === 'unknown') {
      assert.equal(again, row.reencoded, row.name);
      rejects(() => type.decode(bytes, { rejectUnknown: true }), row.name);
      ++c.unknownReencode;
      continue;
    }
    assert.equal(again, row.hex, row.name);
    assert.deepEqual(type.decode(bytes, { rejectUnknown: true }), value, row.name);
    assert.equal(toHex(withHandlers.encode(withHandlers.decode(bytes))), row.hex, `${row.name}: with handlers`);
    ++c.canonicalReencode;
    if (!automatic) continue;

    // Bytes -> text -> bytes, in both text formats.
    const lossy = hasSomeNone(value);
    let noText = false;
    for (const format of ['ijson', 'json'] as const) {
      let text: string;
      try {
        text = type.psszToJson(bytes, format);
      } catch (e) {
        assert.ok(e instanceof PsszError && e.kind === 'value', `${row.name}: ${String(e)}`);
        if (e.message.includes('no JSON')) {
          // Only bit types and non-finite floats have no text (128 / 256-bit
          // integers and enums do).
          assert.ok(
            e.message === 'json: bit types have no JSON form' ||
              e.message === `${format}: non-finite float has no JSON text`,
            `${row.name}: ${e.message}`,
          );
          noText = true;
          break;
        }
        // A raw string holding bytes that are not UTF-8 has no text.
        if (e.message === 'json: string is not valid UTF-8') {
          noText = true;
          if (format === 'ijson') ++count.auto.rawNonUtf8NoText;
          break;
        }
        // A string holding a Unicode noncharacter is JSON but not I-JSON.
        assert.equal(format, 'ijson', row.name);
        assert.equal(e.message, 'ijson: string holds a Unicode noncharacter', row.name);
        ++count.auto.ijsonNoncharacter;
        continue;
      }
      assert.equal(toHex(type.jsonToPssz(text, format)) === row.hex, !lossy, `${row.name} via ${format}: ${text}`);
      if (!lossy) ++count.auto[format === 'json' ? 'jsonRoundTrip' : 'ijsonRoundTrip'];
      // The same through the handlers' text forms.
      if (!lossy) {
        const typedText = withHandlers.psszToJson(bytes, format);
        assert.equal(toHex(withHandlers.jsonToPssz(typedText, format)), row.hex, `${row.name} via ${format}, handlers: ${typedText}`);
        ++count.auto.handlerTextRoundTrip;
      }
    }
    if (noText) ++count.auto.noJsonForm;
    else if (lossy) ++count.auto.someNoneLossy;
    // Every raw non-UTF-8 value is refused by the text writers.
    if (cls === 'string-raw-non-utf8') assert.ok(noText, row.name);
  }

  console.log(`pssz-vectors.json (${path}):`, JSON.stringify({ types: fx.types.length, vectors: fx.vectors.length, ...count }));
  const a = count.auto;
  const e = count.explicit;
  // Every vector was replayed: nothing is skipped.
  assert.equal(a.vectors + e.vectors, fx.vectors.length);
  assert.equal(a.validateAccept + a.validateReject + a.invariant, a.vectors);
  assert.equal(a.canonicalReencode + a.unknownReencode, a.validateAccept);
  assert.equal(a.jsonRoundTrip + a.someNoneLossy + a.noJsonForm, a.canonicalReencode - a.nestedDeferred);
  assert.equal(e.validateAccept + e.validateReject + e.invariant, e.vectors);
  assert.equal(e.canonicalReencode + e.unknownReencode, e.validateAccept);
  // The fixture as checked in (a change to it must be looked at): every
  // class of conformance/pssz-vectors.md, at its recorded size.
  assert.deepEqual(Object.fromEntries([...classes].sort()), Object.fromEntries(Object.entries(PSSZ_CLASSES).sort()));
  assert.equal(a.invariant + e.invariant, PSSZ_CLASSES.invariant);
  assert.equal(a.nestedDeferred + e.nestedDeferred, PSSZ_CLASSES['nested-deferred']);
  assert.equal(a.nestedUnknown + e.nestedUnknown, 16);
  assert.ok(a.rawNonUtf8NoText >= 11);
  // Top-level caps under an explicit width are rejections that were replayed.
  const explicitCap = fx.vectors.filter(
    (v) => v.format !== 'auto' && v.name.startsWith('width/') && v.name.includes('cap') && v.expect === 'reject',
  );
  assert.equal(explicitCap.length, 12);
  assert.equal(fx.types.length, 308);
  assert.equal(fx.vectors.length, 6589);
});

for (const path of PATHS) test(`JSON fixture: every type and vector, ${path}`, () => {
  const fx: { schema_ir: string; types: JsonTypeRow[]; vectors: JsonRow[] } = fixture('json-vectors.json');
  const decoded = PsszSchema.decode(fromHex(fx.schema_ir));
  assert.equal(toHex(decoded.encode()), fx.schema_ir);
  const rows = new Map(fx.types.map((t) => [t.id, t]));
  // The fixture's custom text forms, registered once (schema-driven: for
  // the schema; typed: declared with the custom types); the same types with
  // no handler (the fallback) and with handlers required.
  const typedPlain = new TypedFixture(decoded.ir);
  const schema: TypeSource = path === 'typed' ? typedPlain : decoded;
  const withHandlers: TypeSource =
    path === 'typed' ? new TypedFixture(decoded.ir, fixtureHandlers) : decoded.withHandlers(fixtureHandlers);
  const strict: TypeSource = path === 'typed'
    ? { type: (id) => typedPlain.type(id, { requireHandlers: true }) }
    : PsszSchema.decode(fromHex(fx.schema_ir), { requireHandlers: true });
  const types = new Map<string, PsszType>();
  for (const row of fx.types) if (row.ir) types.set(row.id, withHandlers.type(row.id));
  // Width fidelity: every type the typed PSSZ codec encodes records its
  // automatic width, and the schema derives the same (handlers or not).
  let widthsChecked = 0;
  for (const row of fx.types) {
    assert.equal(row.auto_width !== undefined, row.pssz, row.id);
    if (row.auto_width === undefined) continue;
    assert.equal(types.get(row.id)!.width, row.auto_width, `auto width of ${row.id}`);
    assert.equal(schema.type(row.id).width, row.auto_width, `auto width of ${row.id}`);
    ++widthsChecked;
  }
  assert.equal(widthsChecked, fx.types.filter((t) => t.pssz).length);

  const count = {
    value: 0, valueTextToBytes: 0, valueBytesMatchFixture: 0, valueBytesToText: 0, accept: 0, reject: 0,
    invariant: 0, notExpressibleInIr: 0, plainValue: 0, strictRefused: 0, strictBinaryRefused: 0,
  };
  const notExpressible = new Set<string>();
  const covered = new Set<string>();
  const inputOf = (row: JsonRow) => (row.text !== undefined ? row.text : fromHex(row.text_hex!));

  for (const row of fx.vectors) {
    const info = rows.get(row.type);
    assert.ok(info, row.name);
    if (!info.ir) {
      ++count.notExpressibleInIr;
      notExpressible.add(row.type);
      continue;
    }
    covered.add(row.type);
    const type = types.get(row.type)!;
    assert.equal(row.pssz !== undefined, info.pssz && row.kind !== 'reject', row.name);
    assert.equal(row.reason !== undefined, row.kind === 'invariant', `${row.name}: reason`);
    if (row.kind === 'value') {
      ++count.value;
      const bytes = type.jsonToPssz(row.json!, 'json');
      const again = type.jsonToPssz(row.ijson!, 'ijson');
      assert.equal(toHex(bytes), toHex(again), row.name);
      count.valueTextToBytes += 2;
      if (row.pssz !== undefined) {
        assert.equal(toHex(bytes), row.pssz, row.name);
        ++count.valueBytesMatchFixture;
      }
      const validation = type.validate(bytes);
      assert.ok(validation.ok && !validation.hasUnknown, row.name);
      assert.equal(type.psszToJson(bytes, 'json'), row.json, row.name);
      assert.equal(type.psszToJson(bytes, 'ijson'), row.ijson, row.name);
      count.valueBytesToText += 2;
      // The plain value <-> text functions agree with the byte path.
      assert.deepEqual(type.jsonToValue(row.json!, 'json'), type.decode(bytes), row.name);
      assert.equal(type.valueToJson(type.decode(bytes), 'ijson'), row.ijson, row.name);
      // Text given as UTF-8 bytes reads the same.
      assert.equal(toHex(type.jsonToPssz(Buffer.from(row.ijson!, 'utf8'), 'ijson')), toHex(bytes), row.name);

      // A type holding a custom text form. With NO handler the schema path
      // writes the wire type's own text (the row's plain_*) and reads it
      // back to the same bytes; with handlers required it refuses.
      assert.equal(row.plain_json !== undefined, info.custom === true, row.name);
      assert.equal(row.plain_ijson !== undefined, info.custom === true, row.name);
      if (info.custom) {
        const plain = schema.type(row.type);
        assert.equal(plain.psszToJson(bytes, 'json'), row.plain_json, row.name);
        assert.equal(plain.psszToJson(bytes, 'ijson'), row.plain_ijson, row.name);
        assert.equal(toHex(plain.jsonToPssz(row.plain_json!, 'json')), toHex(bytes), row.name);
        assert.equal(toHex(plain.jsonToPssz(row.plain_ijson!, 'ijson')), toHex(bytes), row.name);
        // The binary side does not depend on handlers.
        assert.equal(toHex(plain.encode(type.decode(bytes))), toHex(bytes), row.name);
        ++count.plainValue;
        const required = strict.type(row.type);
        for (const format of ['json', 'ijson'] as const) {
          // (The bytes are refused first; then the value's text.)
          const bytesToText = rejects(() => required.psszToJson(bytes, format), row.name);
          assert.equal(bytesToText.message, 'pssz IR: no handler registered for a custom type', row.name);
          const write = rejects(() => required.valueToJson(plain.decode(bytes), format), row.name);
          assert.equal(write.kind, 'value', row.name);
          assert.equal(write.message, 'pssz IR: no handler registered for a custom type', row.name);
          const read = rejects(() => required.jsonToPssz(row[format]!, format), row.name);
          assert.equal(read.kind, 'json', row.name);
          assert.equal(read.message, 'pssz IR: no handler registered for a custom type', row.name);
        }
        // A strict registry applies on the binary entry points as well:
        // wherever a value of a custom type occurs (an empty map keyed by
        // one holds none).
        const refused = required.validate(bytes);
        if (refused.ok) {
          assert.equal(toHex(required.encode(required.decode(bytes))), toHex(bytes), row.name);
        } else {
          assert.equal(refused.error.message, 'pssz IR: no handler registered for a custom type', row.name);
          assert.equal(refused.error.kind, 'bytes', row.name);
          assert.equal(rejects(() => required.decode(bytes), row.name).kind, 'bytes', row.name);
          const unwritten = rejects(() => required.encode(plain.decode(bytes)), row.name);
          assert.equal(unwritten.kind, 'value', row.name);
          assert.equal(unwritten.message, 'pssz IR: no handler registered for a custom type', row.name);
          ++count.strictBinaryRefused;
        }
        ++count.strictRefused;
      }
    } else if (row.kind === 'accept') {
      ++count.accept;
      const bytes = type.jsonToPssz(inputOf(row), row.format!);
      if (row.pssz !== undefined) assert.equal(toHex(bytes), row.pssz, row.name);
      assert.equal(type.psszToJson(bytes, row.format!), row.canonical, row.name);
    } else if (row.kind === 'invariant') {
      // Well-formed text of the type (the typed text: a custom text form
      // where the type has one) whose value violates an invariant. With
      // the handlers every entry point, text and binary, reports the
      // invariant's own message; with none the value is its wire type; a
      // strict registry that lacks the handler refuses the type.
      ++count.invariant;
      assert.equal(info.invariant, true, row.name);
      const format = row.format!;
      const bytes = fromHex(row.pssz!);
      const read = rejects(() => type.jsonToPssz(row.text!, format), `${row.name}: text`);
      assert.equal(read.message, row.reason, row.name);
      assert.equal(read.kind, 'json', row.name);
      assert.equal(read.invariant, true, row.name);
      assert.equal(rejects(() => type.jsonToValue(row.text!, format), row.name).message, row.reason, row.name);
      for (const f of ['json', 'ijson'] as const)
        assert.equal(rejects(() => type.psszToJson(bytes, f), `${row.name}: bytes to ${f}`).message, row.reason, row.name);
      const refused = type.validate(bytes);
      assert.ok(!refused.ok, row.name);
      assert.equal(refused.error.message, row.reason, row.name);
      assert.equal(refused.error.invariant, true, row.name);
      assert.equal(rejects(() => type.decode(bytes), row.name).message, row.reason, row.name);
      // No handler: valid bytes, the wire type's texts, each reading back.
      const plain = schema.type(row.type);
      assert.deepEqual(plain.validate(bytes), { ok: true, hasUnknown: false }, row.name);
      const value = plain.decode(bytes);
      assert.equal(plain.psszToJson(bytes, 'json'), row.plain_json, row.name);
      assert.equal(plain.psszToJson(bytes, 'ijson'), row.plain_ijson, row.name);
      assert.equal(toHex(plain.jsonToPssz(row.plain_json!, 'json')), row.pssz, row.name);
      assert.equal(toHex(plain.jsonToPssz(row.plain_ijson!, 'ijson')), row.pssz, row.name);
      // Writers refuse the value too: binary and text.
      const written = rejects(() => type.encode(value), `${row.name}: encode`);
      assert.equal(written.message, row.reason, row.name);
      assert.equal(written.kind, 'value', row.name);
      for (const f of ['json', 'ijson'] as const) {
        const text = rejects(() => type.valueToJson(value, f), `${row.name}: value to ${f}`);
        assert.equal(text.message, row.reason, row.name);
        assert.equal(text.kind, 'value', row.name);
      }
      const required = strict.type(row.type).validate(bytes);
      assert.ok(!required.ok, row.name);
      assert.equal(required.error.message, 'pssz IR: no handler registered for a custom type', row.name);
    } else {
      assert.equal(row.kind, 'reject', row.name);
      ++count.reject;
      const error = rejects(() => type.jsonToPssz(inputOf(row), row.format!), row.name);
      assert.ok(error.kind === 'json' || error.kind === 'value', `${row.name}: ${error.kind} ${error.message}`);
    }
  }
  console.log(
    `json-vectors.json (${path}):`,
    JSON.stringify({ types: fx.types.length, vectors: fx.vectors.length, ...count, notExpressible: [...notExpressible] }),
  );
  // Every expressible type is exercised, and every vector is accounted for.
  for (const row of fx.types) if (row.ir) assert.ok(covered.has(row.id), row.id);
  assert.equal(count.value + count.accept + count.reject + count.invariant + count.notExpressibleInIr, fx.vectors.length);
  // Every type an invariant constrains has `invariant` rows, and only those.
  for (const row of fx.types)
    assert.equal(fx.vectors.some((v) => v.type === row.id && v.kind === 'invariant'), row.invariant === true, row.id);
  assert.deepEqual(
    { value: count.value, accept: count.accept, reject: count.reject, invariant: count.invariant },
    { value: 818, accept: 194, reject: 697, invariant: 50 },
  );
  // Enums and 128 / 256-bit integers are replayed, not skipped.
  // The custom types are expressible and were replayed with and without
  // handlers.
  assert.equal(count.notExpressibleInIr, 0);
  for (const id of ['Version', 'Release', 'Account', 'Transfer']) {
    assert.ok(rows.get(id)?.ir && rows.get(id)?.custom && covered.has(id), id);
    assert.ok(fx.vectors.some((v) => v.type === id && v.kind === 'value' && v.plain_json !== undefined), id);
    assert.ok(fx.vectors.some((v) => v.type === id && v.kind === 'reject'), id);
  }
  assert.ok(count.plainValue >= 14 && count.strictRefused === count.plainValue);
  assert.ok(count.strictBinaryRefused >= 30 && count.strictBinaryRefused <= count.strictRefused);
  for (const id of ['Color', 'Status', 'u128', 'i128', 'u256', 'Wide']) {
    assert.ok(rows.get(id)?.ir && covered.has(id), id);
    for (const kind of id === 'Wide' ? ['value'] : ['value', 'accept', 'reject'])
      assert.ok(fx.vectors.some((v) => v.type === id && v.kind === kind), `${id} ${kind}`);
  }
  // The custom types of conformance/custom-types.md, the maps keyed by a
  // text form, and the nested values.
  for (const id of ['Blob', 'Endpoint', 'Ratio', 'Ref', 'Guarded', 'map<Account,u64>', 'map<Version,string>', 'map<Ref,u8>'])
    assert.ok(rows.get(id)?.custom && fx.vectors.some((v) => v.type === id && v.plain_json !== undefined), id);
  for (const id of ['nested<u64>', 'nested<NestedArgs>', 'nested<NestedArgs,deferred>', 'NestedAction', 'set<nested<u64>>',
    'map<nested<u64>,u8>', 'nested<map<string,u32>>', 'map<string,u32>', 'set<string>', 'pair<u32,string>', 'Id', 'Tag'])
    assert.ok(rows.get(id)?.ir && covered.has(id), id);
  assert.equal(fx.types.length, 96);
  assert.equal(fx.vectors.length, 1759);
});

// Malformed schemas: all three languages refuse the same ones, with an
// error, at every schema-driven entry point; their well-formed neighbours
// work. (C++ schema_conformance_tests.cpp, Rust schema_conformance.rs.)
test('JSON fixture: malformed schemas are refused, their neighbours accepted', () => {
  const fx: {
    schema_rejects: { name: string; schema_ir: string; root: string }[];
    schema_accepts: { name: string; schema_ir: string; root: string; pssz: string; json: string; ijson: string }[];
  } = fixture('json-vectors.json');
  assert.ok(fx.schema_rejects.length >= 25);
  assert.ok(fx.schema_accepts.length >= 8);
  const names = new Set<string>();
  for (const row of fx.schema_rejects) {
    assert.ok(!names.has(row.name), `${row.name}: duplicate`);
    names.add(row.name);
    // The IR itself is a valid Schema value (it decodes, and re-encodes to
    // the same bytes): it is its meaning that is refused.
    const schema = PsszSchema.decode(fromHex(row.schema_ir));
    assert.equal(toHex(schema.encode()), row.schema_ir, row.name);
    for (const width of ['auto', 1, 2, 4] as PsszWidth[]) {
      const error = rejects(() => schema.type(row.root, { width }), `${row.name}: type(${width})`);
      assert.equal(error.kind, 'schema', `${row.name}: ${error.message}`);
    }
    for (const bytes of [new Uint8Array(0), new Uint8Array(1), new Uint8Array(8)]) {
      const result = validatePssz(schema, row.root, bytes);
      assert.equal(result.ok, false, row.name);
      if (!result.ok) assert.equal(result.error.kind, 'schema', row.name);
    }
    // Also when the malformed type is reached through a handler-carrying copy.
    rejects(() => schema.withHandlers({}, { requireHandlers: true }).type(row.root), `${row.name}: strict`);
  }
  for (const row of fx.schema_accepts) {
    assert.ok(!names.has(row.name), `${row.name}: duplicate`);
    names.add(row.name);
    const type = PsszSchema.decode(fromHex(row.schema_ir)).type(row.root);
    const bytes = fromHex(row.pssz);
    assert.deepEqual(type.validate(bytes), { ok: true, hasUnknown: false }, row.name);
    assert.equal(type.psszToJson(bytes, 'json'), row.json, `${row.name}: json`);
    assert.equal(type.psszToJson(bytes, 'ijson'), row.ijson, `${row.name}: ijson`);
    assert.equal(toHex(type.jsonToPssz(row.json, 'json')), row.pssz, `${row.name}: json read`);
    assert.equal(toHex(type.jsonToPssz(row.ijson, 'ijson')), row.pssz, `${row.name}: ijson read`);
    assert.equal(toHex(type.encode(type.decode(bytes))), row.pssz, `${row.name}: value`);
  }

  // The accepted schemas through the typed layer, where it can declare the
  // type; the others are listed with the reason. (A refused schema is
  // malformed IR: the typed layer cannot write one, so those rows are
  // schema-driven only.)
  const typedRan: string[] = [];
  const notDeclarable: { [name: string]: string } = {};
  for (const row of fx.schema_accepts) {
    const typed = new TypedFixture(PsszSchema.decode(fromHex(row.schema_ir)).ir);
    const def = typed.tryDef(row.root);
    if (!def) {
      notDeclarable[row.name] = typed.unsupported.get(row.root)!;
      continue;
    }
    const bytes = fromHex(row.pssz);
    assert.deepEqual(def.validate(bytes), { ok: true, hasUnknown: false }, row.name);
    assert.equal(def.psszToJson(bytes, 'json'), row.json, `${row.name}: typed json`);
    assert.equal(def.psszToJson(bytes, 'ijson'), row.ijson, `${row.name}: typed ijson`);
    assert.equal(toHex(def.jsonToPssz(row.json, 'json')), row.pssz, `${row.name}: typed json read`);
    assert.equal(toHex(def.jsonToPssz(row.ijson, 'ijson')), row.pssz, `${row.name}: typed ijson read`);
    assert.equal(toHex(def.encode(def.decode(bytes))), row.pssz, `${row.name}: typed value`);
    typedRan.push(row.name);
  }
  console.log('json-vectors.json schema rows:', JSON.stringify({
    schemaRejects: fx.schema_rejects.length, schemaAccepts: fx.schema_accepts.length,
    schemaAcceptsTyped: typedRan.length, notDeclarable,
  }));
  assert.equal(typedRan.length + Object.keys(notDeclarable).length, fx.schema_accepts.length);
  assert.equal(fx.schema_rejects.length, 77);
  assert.equal(fx.schema_accepts.length, 28);
});

// The IR root says which version of the IR it is written in. The field was
// appended, so a schema without it still decodes; no codec is derived from
// it, with an error that says why.
test('schema IR: the ir_version marker is required and backward compatible', () => {
  const fx: { schema_ir: string; types: { id: string; ir: boolean; pssz?: boolean }[] } = fixture('json-vectors.json');
  const current = PsszSchema.decode(fromHex(fx.schema_ir));
  assert.equal(current.ir.ir_version, IR_VERSION);
  assert.equal(IR_VERSION, 1);
  const id = fx.types.find((t) => t.ir && t.pssz)!.id;
  current.type(id);

  // A map of named types is written here and now: the current version.
  assert.equal(new PsszSchema({ T: { Int: { bits: 8, isSigned: false } } }).ir.ir_version, IR_VERSION);
  // The full form states its version; without one it has none.
  const members = [{ name: 'T', type: { Int: { bits: 8, isSigned: false } } }];
  assert.equal(new PsszSchema({ types: members, ir_version: 1 }).ir.ir_version, 1);
  assert.equal(new PsszSchema({ types: members }).ir.ir_version, null);
  assert.equal(new PsszSchema({ types: members, ir_version: null }).ir.ir_version, null);
  rejects(() => new PsszSchema({ types: members, ir_version: -1 }), 'negative');
  rejects(() => new PsszSchema({ types: members, ir_version: 1.5 }), 'fraction');
  rejects(() => new PsszSchema({ types: members, ir_version: '1' as unknown as number }), 'string');

  // The same schema as it was written before the marker existed.
  const old = new PsszSchema({ ...current.ir, ir_version: null });
  const oldBytes = old.encode();
  assert.notEqual(toHex(oldBytes), fx.schema_ir);
  const decoded = PsszSchema.decode(oldBytes);
  assert.equal(decoded.ir.ir_version, null);
  assert.deepEqual(decoded.ir.types, current.ir.types);
  assert.equal(toHex(decoded.encode()), toHex(oldBytes));
  // Through JSON as well: a document without the member has no version.
  const withNull = PsszSchema.meta().type('Schema').valueToJson(old.ir, 'json');
  assert.ok(withNull.endsWith(',"ir_version":null}'));
  assert.equal(PsszSchema.fromJson(withNull, 'json').ir.ir_version, null);
  const text = withNull.replace(',"ir_version":null}', '}');
  assert.ok(!text.includes('ir_version'));
  assert.equal(PsszSchema.fromJson(text, 'json').ir.ir_version, null);
  assert.equal(PsszSchema.fromJson(PsszSchema.meta().type('Schema').valueToJson(current.ir, 'ijson'), 'ijson').ir.ir_version, IR_VERSION);

  const refused: [PsszSchema, RegExp][] = [
    [decoded, /no ir_version marker/],
    [new PsszSchema({ ...current.ir, ir_version: IR_VERSION + 1 }), /newer than this library/],
    [new PsszSchema({ ...current.ir, ir_version: 0 }), /newer than this library/],
  ];
  for (const [schema, what] of refused) {
    for (const width of ['auto', 1, 2, 4] as PsszWidth[]) {
      const error = rejects(() => schema.type(id, { width }), String(what));
      assert.equal(error.kind, 'schema');
      assert.match(error.message, what);
    }
    const result = validatePssz(schema, id, new Uint8Array(0));
    assert.equal(result.ok, false);
    if (!result.ok) assert.match(result.error.message, what);
    rejects(() => schema.withHandlers({}).type(id), 'withHandlers');
    rejects(() => schema.type({ Int: { bits: 8, isSigned: false } }), 'inline type');
  }
});
