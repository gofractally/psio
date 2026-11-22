// Cross-language conformance: the schema-driven PSSZ / JSON / I-JSON codec
// against the two shared fixtures, using nothing but the Schema IR recorded
// in them. The C++ and Rust counterparts are
// cpp/tests/schema_conformance_tests.cpp and
// rust/psio/tests/schema_conformance.rs.
//
// Every vector of both fixtures is visited and accounted for: the totals at
// the end must add up to the fixture sizes, so nothing is skipped silently.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { PsszError, PsszSchema, type JsonFormat, type PsszType, type PsszWidth } from '../index.js';

const fixture = (name: string) =>
  JSON.parse(readFileSync(new URL(`../../../conformance/${name}`, import.meta.url), 'utf8'));
const fromHex = (hex: string) => Uint8Array.from(Buffer.from(hex, 'hex'));
const toHex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');

interface PsszTypeRow {
  id: string;
  ir: boolean;
  auto_width: number;
}
interface PsszRow {
  name: string;
  type: string;
  format: 'auto' | 'pssz8' | 'pssz16' | 'pssz32';
  hex: string;
  expect: 'canonical' | 'unknown' | 'reject';
  reencoded: string;
}
interface JsonTypeRow {
  id: string;
  ir: boolean;
  pssz: boolean;
}
interface JsonRow {
  name: string;
  type: string;
  kind: 'value' | 'accept' | 'reject';
  format?: JsonFormat;
  json?: string;
  ijson?: string;
  text?: string;
  text_hex?: string;
  canonical?: string;
  pssz?: string;
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
  const keys = Object.keys(value);
  const record = value as Record<string, unknown>;
  if (keys.length === 1 && keys[0] === 'some' && record.some === null) return true;
  return keys.some((key) => hasSomeNone(record[key]));
}

test('PSSZ fixture: every type and vector through the Schema IR', () => {
  const fx: { schema_ir: string; types: PsszTypeRow[]; vectors: PsszRow[] } = fixture('pssz-vectors.json');
  const schema = PsszSchema.decode(fromHex(fx.schema_ir));
  // The IR travels as its own PSSZ encoding and re-encodes to the same bytes.
  assert.equal(toHex(schema.encode()), fx.schema_ir);

  // Type manifest: the IR-derived automatic width of every type.
  const notExpressible = fx.types.filter((t) => !t.ir).map((t) => t.id);
  assert.deepEqual(notExpressible, []);
  for (const row of fx.types) assert.equal(schema.type(row.id).width, row.auto_width, `auto width of ${row.id}`);

  const types = new Map<string, PsszType | PsszError>();
  const typeOf = (id: string, width: PsszWidth): PsszType | PsszError => {
    const key = `${width}/${id}`;
    let type = types.get(key);
    if (!type) {
      try {
        type = schema.type(id, { width });
      } catch (e) {
        assert.ok(e instanceof PsszError);
        type = e;
      }
      types.set(key, type);
    }
    return type;
  };

  const count = {
    auto: {
      vectors: 0, validateAccept: 0, validateReject: 0, canonicalReencode: 0, unknownReencode: 0,
      rawNonUtf8Rejected: 0, jsonRoundTrip: 0, ijsonRoundTrip: 0, someNoneLossy: 0, noJsonForm: 0,
      ijsonNoncharacter: 0,
    },
    explicit: {
      vectors: 0, validateAccept: 0, validateReject: 0, canonicalReencode: 0,
      unknownReencode: 0, rawNonUtf8Rejected: 0,
    },
  };

  for (const row of fx.vectors) {
    const bytes = fromHex(row.hex);
    const cls = row.name.slice(0, row.name.indexOf('/'));
    const automatic = row.format === 'auto';
    const width: PsszWidth = automatic ? 'auto' : row.format === 'pssz8' ? 1 : row.format === 'pssz16' ? 2 : 4;
    const c = automatic ? count.auto : count.explicit;
    ++c.vectors;
    const type = typeOf(row.type, width);
    // Every fixture type builds under every profile. (The explicit profiles
    // enforce maxDynamicData on the top-level type only, as the typed
    // psio::pssz_<W> codecs that produced the fixture do.)
    if (type instanceof PsszError) assert.fail(`${row.name}: ${type.message}`);
    const result = type.validate(bytes);
    if (!result.ok) assert.notEqual(result.error.kind, 'internal', `${row.name}: ${result.error.message}`);

    if (cls === 'string-raw-non-utf8') {
      // The IR has one "string" for raw and UTF-8 strings and the schema
      // codecs treat it as UTF-8: these frames, valid for the typed raw
      // string, are rejected through the schema (docs/json.md).
      assert.equal(row.expect, 'canonical', row.name);
      assert.equal(result.ok, false, row.name);
      rejects(() => type.decode(bytes), row.name);
      ++c.rawNonUtf8Rejected;
      continue;
    }
    if (row.expect === 'reject') {
      assert.equal(result.ok, false, `${row.name}: accepted`);
      rejects(() => type.decode(bytes), row.name);
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
              e.message === 'json: non-finite float has no JSON text',
            `${row.name}: ${e.message}`,
          );
          noText = true;
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
    }
    if (noText) ++count.auto.noJsonForm;
    else if (lossy) ++count.auto.someNoneLossy;
  }

  console.log('pssz-vectors.json:', JSON.stringify({ types: fx.types.length, vectors: fx.vectors.length, ...count }));
  const a = count.auto;
  const e = count.explicit;
  assert.equal(a.vectors + e.vectors, fx.vectors.length);
  assert.equal(a.validateAccept + a.validateReject + a.rawNonUtf8Rejected, a.vectors);
  assert.equal(a.canonicalReencode + a.unknownReencode, a.validateAccept);
  assert.equal(a.jsonRoundTrip + a.someNoneLossy + a.noJsonForm, a.canonicalReencode);
  assert.equal(e.validateAccept + e.validateReject + e.rawNonUtf8Rejected, e.vectors);
  assert.equal(e.canonicalReencode + e.unknownReencode, e.validateAccept);
  // The fixture as checked in (a change to it must be looked at).
  assert.equal(fx.types.length, 204);
  assert.equal(fx.vectors.length, 5167);
});

test('JSON fixture: every type and vector through the Schema IR', () => {
  const fx: { schema_ir: string; types: JsonTypeRow[]; vectors: JsonRow[] } = fixture('json-vectors.json');
  const schema = PsszSchema.decode(fromHex(fx.schema_ir));
  assert.equal(toHex(schema.encode()), fx.schema_ir);
  const rows = new Map(fx.types.map((t) => [t.id, t]));
  const types = new Map<string, PsszType>();
  for (const row of fx.types) if (row.ir) types.set(row.id, schema.type(row.id));

  const count = {
    value: 0, valueTextToBytes: 0, valueBytesMatchFixture: 0, valueBytesToText: 0, accept: 0, reject: 0,
    notExpressibleInIr: 0,
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
    } else if (row.kind === 'accept') {
      ++count.accept;
      const bytes = type.jsonToPssz(inputOf(row), row.format!);
      if (row.pssz !== undefined) assert.equal(toHex(bytes), row.pssz, row.name);
      assert.equal(type.psszToJson(bytes, row.format!), row.canonical, row.name);
    } else {
      assert.equal(row.kind, 'reject', row.name);
      ++count.reject;
      const error = rejects(() => type.jsonToPssz(inputOf(row), row.format!), row.name);
      assert.ok(error.kind === 'json' || error.kind === 'value', `${row.name}: ${error.kind} ${error.message}`);
    }
  }
  console.log(
    'json-vectors.json:',
    JSON.stringify({ types: fx.types.length, vectors: fx.vectors.length, ...count, notExpressible: [...notExpressible] }),
  );
  // Every expressible type is exercised, and every vector is accounted for.
  for (const row of fx.types) if (row.ir) assert.ok(covered.has(row.id), row.id);
  assert.equal(count.value + count.accept + count.reject + count.notExpressibleInIr, fx.vectors.length);
  // Enums and 128 / 256-bit integers are replayed, not skipped.
  for (const id of ['Color', 'Status', 'u128', 'i128', 'u256', 'Wide']) {
    assert.ok(rows.get(id)?.ir && covered.has(id), id);
    for (const kind of id === 'Wide' ? ['value'] : ['value', 'accept', 'reject'])
      assert.ok(fx.vectors.some((v) => v.type === id && v.kind === kind), `${id} ${kind}`);
  }
  assert.equal(fx.types.length, 49);
  assert.equal(fx.vectors.length, 1291);
});
