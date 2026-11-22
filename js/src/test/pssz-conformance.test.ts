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
import { IR_VERSION, PsszError, PsszSchema, validatePssz, type JsonFormat, type PsszType, type PsszWidth } from '../index.js';
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
  /** The typed codec's automatic width (present when `pssz`). */
  auto_width?: number;
  /** The type holds a custom text form: its value rows carry `plain_*`. */
  custom?: boolean;
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
  /** The text with no custom handler registered (the wire type's own). */
  plain_json?: string;
  plain_ijson?: string;
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

for (const path of PATHS) test(`PSSZ fixture: every type and vector, ${path}`, () => {
  const fx: { schema_ir: string; types: PsszTypeRow[]; vectors: PsszRow[] } = fixture('pssz-vectors.json');
  const schema = PsszSchema.decode(fromHex(fx.schema_ir));
  const source: TypeSource = path === 'typed' ? new TypedFixture(schema.ir) : schema;
  // The IR travels as its own PSSZ encoding and re-encodes to the same bytes.
  assert.equal(toHex(schema.encode()), fx.schema_ir);

  // Width fidelity: for EVERY type the schema-derived automatic width is the
  // typed codec's (recorded by the emitter as `auto_width`).
  const notExpressible = fx.types.filter((t) => !t.ir).map((t) => t.id);
  assert.deepEqual(notExpressible, []);
  let widthsChecked = 0;
  for (const row of fx.types) {
    assert.ok(row.auto_width === 1 || row.auto_width === 2 || row.auto_width === 4, row.id);
    assert.equal(source.type(row.id).width, row.auto_width, `auto width of ${row.id}`);
    ++widthsChecked;
  }
  assert.equal(widthsChecked, fx.types.length);
  // A box gives its container no inferred bound; an explicit cap still does.
  const widthOf = (id: string) => source.type(id).width;
  assert.deepEqual(
    ['BDirect', 'BoxRec', 'BoxCapRec', 'BoxVecCapRec', 'box<BLeaf>', 'optional<BoxCapRec>'].map(widthOf),
    [1, 4, 1, 2, 4, 1],
  );

  const types = new Map<string, PsszType | PsszError>();
  const typeOf = (id: string, width: PsszWidth): PsszType | PsszError => {
    const key = `${width}/${id}`;
    let type = types.get(key);
    if (!type) {
      try {
        type = source.type(id, { width });
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
      jsonRoundTrip: 0, ijsonRoundTrip: 0, someNoneLossy: 0, noJsonForm: 0,
      ijsonNoncharacter: 0, rawNonUtf8NoText: 0,
    },
    explicit: {
      vectors: 0, validateAccept: 0, validateReject: 0, canonicalReencode: 0,
      unknownReencode: 0,
    },
  };
  const classes = new Map<string, number>();

  for (const row of fx.vectors) {
    const bytes = fromHex(row.hex);
    const cls = row.name.slice(0, row.name.indexOf('/'));
    const automatic = row.format === 'auto';
    const width: PsszWidth = automatic ? 'auto' : row.format === 'pssz8' ? 1 : row.format === 'pssz16' ? 2 : 4;
    const c = automatic ? count.auto : count.explicit;
    ++c.vectors;
    classes.set(cls, (classes.get(cls) ?? 0) + 1);
    const type = typeOf(row.type, width);
    // Every fixture type builds under every profile. (The explicit profiles
    // enforce maxDynamicData on the top-level type only, as the typed
    // psio::pssz_<W> codecs that produced the fixture do.)
    if (type instanceof PsszError) assert.fail(`${row.name}: ${type.message}`);
    const result = type.validate(bytes);
    if (!result.ok) assert.notEqual(result.error.kind, 'internal', `${row.name}: ${result.error.message}`);

    // Non-UTF-8 bytes in a raw string (Custom "string") are valid PSSZ, as
    // for the typed codecs: recorded canonical, replayed like any other row.
    if (cls === 'string-raw-non-utf8') assert.equal(row.expect, 'canonical', row.name);
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
    }
    if (noText) ++count.auto.noJsonForm;
    else if (lossy) ++count.auto.someNoneLossy;
    // Every raw non-UTF-8 value is refused by the text writers.
    if (cls === 'string-raw-non-utf8') assert.ok(noText, row.name);
  }

  console.log(`pssz-vectors.json (${path}):`, JSON.stringify({ types: fx.types.length, vectors: fx.vectors.length, ...count }));
  const a = count.auto;
  const e = count.explicit;
  assert.equal(a.vectors + e.vectors, fx.vectors.length);
  assert.equal(a.validateAccept + a.validateReject, a.vectors);
  assert.equal(a.canonicalReencode + a.unknownReencode, a.validateAccept);
  assert.equal(a.jsonRoundTrip + a.someNoneLossy + a.noJsonForm, a.canonicalReencode);
  assert.equal(e.validateAccept + e.validateReject, e.vectors);
  assert.equal(e.canonicalReencode + e.unknownReencode, e.validateAccept);
  // The fixture as checked in (a change to it must be looked at).
  // The classes this change is about are replayed in full, none skipped.
  assert.equal(classes.get('box'), 100);
  assert.equal(classes.get('string-raw-non-utf8'), 44);
  assert.equal(classes.get('cap-overflow-explicit-nested'), 15);
  assert.ok(a.rawNonUtf8NoText >= 11);
  // Top-level caps under an explicit width are rejections that were replayed.
  const explicitCap = fx.vectors.filter(
    (v) => v.format !== 'auto' && v.name.startsWith('width/') && v.name.includes('cap') && v.expect === 'reject',
  );
  assert.equal(explicitCap.length, 12);
  assert.equal(fx.types.length, 220);
  assert.equal(fx.vectors.length, 5267);
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
    notExpressibleInIr: 0, plainValue: 0, strictRefused: 0,
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
          const write = rejects(() => required.psszToJson(bytes, format), row.name);
          assert.equal(write.kind, 'value', row.name);
          assert.equal(write.message, 'pssz IR: no handler registered for a custom type', row.name);
          const read = rejects(() => required.jsonToPssz(row[format]!, format), row.name);
          assert.equal(read.kind, 'json', row.name);
          assert.equal(read.message, 'pssz IR: no handler registered for a custom type', row.name);
        }
        assert.equal(toHex(required.encode(required.decode(bytes))), toHex(bytes), row.name);
        ++count.strictRefused;
      }
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
    `json-vectors.json (${path}):`,
    JSON.stringify({ types: fx.types.length, vectors: fx.vectors.length, ...count, notExpressible: [...notExpressible] }),
  );
  // Every expressible type is exercised, and every vector is accounted for.
  for (const row of fx.types) if (row.ir) assert.ok(covered.has(row.id), row.id);
  assert.equal(count.value + count.accept + count.reject + count.notExpressibleInIr, fx.vectors.length);
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
  for (const id of ['Color', 'Status', 'u128', 'i128', 'u256', 'Wide']) {
    assert.ok(rows.get(id)?.ir && covered.has(id), id);
    for (const kind of id === 'Wide' ? ['value'] : ['value', 'accept', 'reject'])
      assert.ok(fx.vectors.some((v) => v.type === id && v.kind === kind), `${id} ${kind}`);
  }
  assert.equal(fx.types.length, 51);
  assert.equal(fx.vectors.length, 1325);
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
  assert.equal(fx.schema_rejects.length, 52);
  assert.equal(fx.schema_accepts.length, 15);
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
