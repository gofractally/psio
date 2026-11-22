// Cross-language conformance for time values and the `key` format: every row
// of conformance/time-key-vectors.json (conformance/time-key-vectors.md),
// through nothing but the Schema IR recorded in the file. The C++ and Rust
// counterparts are cpp/tests/time_key_conformance_tests.cpp and
// rust/psio/tests/time_key_conformance.rs.
//
// Every row is visited and counted; the totals must add up to the file's
// row count, so nothing is skipped silently.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { PsszError, PsszSchema, keyPrefixUpperBound, type JsonFormat, type PsszType } from '../index.js';

interface Row {
  name: string;
  kind: 'value' | 'text-accept' | 'text-reject' | 'key-reject' | 'key-order' | 'key-order-bits' | 'key-prefix' | 'bound';
  type: string;
  pssz?: string;
  key?: string;
  json?: string;
  ijson?: string;
  text?: string;
  prefix?: string;
  bound?: string | null;
  count?: number;
  values?: string[];
}
interface Fixture {
  version: number;
  schema_ir: string;
  types: { id: string; composite: boolean }[];
  vectors: Row[];
}

const fixture: Fixture = JSON.parse(
  readFileSync(new URL('../../../conformance/time-key-vectors.json', import.meta.url), 'utf8'),
);
const fromHex = (hex: string) => Uint8Array.from(Buffer.from(hex, 'hex'));
const toHex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');
const FORMATS: JsonFormat[] = ['json', 'ijson'];

function rejects(run: () => unknown, name: string): void {
  try {
    run();
  } catch (e) {
    assert.ok(e instanceof PsszError, `${name}: threw ${String(e)}`);
    assert.notEqual(e.kind, 'internal', `${name}: ${e.message}`);
    return;
  }
  assert.fail(`${name}: accepted`);
}

test('time/key conformance: every vector holds', () => {
  assert.equal(fixture.version, 1);
  const schema = PsszSchema.decode(fromHex(fixture.schema_ir));
  const types = new Map<string, PsszType>();
  for (const row of fixture.types) types.set(row.id, schema.type(row.id));
  const composite = new Map(fixture.types.map((t) => [t.id, t.composite]));

  const counts: Record<string, number> = {};
  const seenTypes = new Set<string>();
  const names = new Set<string>();
  for (const row of fixture.vectors) {
    const name = row.name;
    assert.ok(!names.has(name), `${name}: duplicate`);
    names.add(name);
    counts[row.kind] = (counts[row.kind] ?? 0) + 1;

    if (row.kind === 'bound') {
      assert.equal(row.type, 'bytes', name);
      const bound = keyPrefixUpperBound(fromHex(row.prefix!));
      assert.equal(bound === null ? null : toHex(bound), row.bound, name);
      continue;
    }
    const type = types.get(row.type);
    assert.ok(type, `${name}: unknown type`);
    seenTypes.add(row.type);

    switch (row.kind) {
      case 'value': {
        const value = type.decode(fromHex(row.pssz!));
        assert.equal(toHex(type.encode(value)), row.pssz, `${name}: pssz`);
        const key = type.encodeKey(value);
        assert.equal(toHex(key), row.key, `${name}: key`);
        assert.deepEqual(type.validateKey(key), { ok: true }, `${name}: validateKey`);
        const back = type.decodeKey(key);
        assert.deepEqual(back, value, `${name}: decodeKey`);
        assert.equal(toHex(type.encode(back)), row.pssz, `${name}: decodeKey -> pssz`);
        assert.equal(row.json === undefined, row.ijson === undefined, name);
        for (const format of FORMATS) {
          const want = row[format];
          if (want === undefined) {
            rejects(() => type.valueToJson(value, format), `${name}: ${format} text of a value without text`);
            rejects(() => type.psszToJson(fromHex(row.pssz!), format), `${name}: ${format} psszToJson`);
            continue;
          }
          assert.equal(type.valueToJson(value, format), want, `${name}: ${format}`);
          assert.equal(toHex(type.jsonToPssz(want, format)), row.pssz, `${name}: ${format} read`);
          assert.deepEqual(type.jsonToValue(want, format), value, `${name}: ${format} value`);
        }
        break;
      }
      case 'text-accept': {
        for (const format of FORMATS) {
          assert.equal(toHex(type.jsonToPssz(row.text!, format)), row.pssz, `${name}: ${format}`);
          assert.equal(toHex(type.jsonToPssz(Buffer.from(row.text!, 'utf8'), format)), row.pssz, `${name}: ${format} bytes`);
        }
        assert.equal(type.psszToJson(fromHex(row.pssz!), 'json'), row.json, `${name}: canonical`);
        break;
      }
      case 'text-reject':
        for (const format of FORMATS) rejects(() => type.jsonToValue(row.text!, format), `${name}: ${format}`);
        break;
      case 'key-reject': {
        const bytes = fromHex(row.key!);
        rejects(() => type.decodeKey(bytes), name);
        const v = type.validateKey(bytes);
        assert.equal(v.ok, false, name);
        break;
      }
      case 'key-order': {
        assert.ok(row.values!.length > 0, name);
        let last: Buffer | null = null;
        for (const hex of row.values!) {
          const value = type.decode(fromHex(hex));
          const key: Buffer = Buffer.from(type.encodeKey(value));
          // Bytewise (memcmp) order, strictly ascending.
          if (last) assert.ok(Buffer.compare(last, key) < 0, `${name}: keys not ascending at ${hex}`);
          assert.equal(toHex(type.encodeKey(type.decodeKey(key))), toHex(key), `${name}: round trip`);
          last = key;
        }
        break;
      }
      case 'key-order-bits': {
        // Key order where `<` has no answer (NaNs): the keys ascend and
        // each key gives the exact bit pattern back.
        assert.ok(row.values!.length > 0, name);
        let last: Buffer | null = null;
        for (const hex of row.values!) {
          const value = type.decode(fromHex(hex));
          assert.equal(toHex(type.encode(value)), hex, `${name}: pssz bits`);
          const key: Buffer = Buffer.from(type.encodeKey(value));
          if (last) assert.ok(Buffer.compare(last, key) < 0, `${name}: keys not ascending at ${hex}`);
          assert.deepEqual(type.validateKey(key), { ok: true }, `${name}: validateKey`);
          assert.equal(toHex(type.encode(type.decodeKey(key))), hex, `${name}: key gives the bits back`);
          last = key;
        }
        break;
      }
      case 'key-prefix': {
        assert.equal(composite.get(row.type), true, `${name}: composite`);
        const value = type.decode(fromHex(row.pssz!));
        const elements = (Array.isArray(value) ? value : Object.values(value as object)).slice(0, row.count!);
        assert.equal(elements.length, row.count, name);
        const prefix = type.encodeKeyPrefix(elements);
        assert.equal(toHex(prefix), row.prefix, `${name}: prefix`);
        const bound = keyPrefixUpperBound(prefix);
        assert.equal(bound === null ? null : toHex(bound), row.bound, `${name}: bound`);
        const full = Buffer.from(type.encodeKey(value));
        assert.ok(full.subarray(0, prefix.length).equals(prefix), `${name}: byte-prefix of the full key`);
        if (bound) assert.ok(Buffer.compare(full, bound) < 0, `${name}: below the bound`);
        break;
      }
      default:
        assert.fail(`${name}: unknown kind ${String(row.kind)}`);
    }
  }

  const kinds = ['value', 'text-accept', 'text-reject', 'key-reject', 'key-order', 'key-order-bits', 'key-prefix', 'bound'];
  assert.deepEqual(Object.keys(counts).sort(), [...kinds].sort());
  let total = 0;
  for (const kind of kinds) {
    assert.ok(counts[kind] > 0, kind);
    total += counts[kind];
  }
  assert.equal(total, fixture.vectors.length);
  assert.ok(total >= 500);
  // Every declared type has rows.
  for (const row of fixture.types) assert.ok(seenTypes.has(row.id), `no rows for ${row.id}`);
});

test('time/key conformance: the schema marks time values with their ids', () => {
  const schema = PsszSchema.decode(fromHex(fixture.schema_ir));
  const byName = new Map(schema.ir.types.map((m) => [m.name, m.type]));
  const ids = new Map<string, string>();
  for (const member of schema.ir.types) {
    let t = member.type.value;
    for (let hops = 0; t.type === 'Type' && hops < 8; ++hops) t = byName.get(t.value.type)!.value;
    if (t.type === 'Custom') ids.set(member.name, t.value.id);
  }
  assert.equal(ids.get('time_point<seconds>'), 'time-point-seconds');
  assert.equal(ids.get('time_point<millis>'), 'time-point-millis');
  assert.equal(ids.get('time_point<micros>'), 'time-point-micros');
  assert.equal(ids.get('time_point<nanos>'), 'time-point-nanos');
  assert.equal(ids.get('time_point<seconds,u32>'), 'time-point-seconds');
  assert.equal(ids.get('duration<seconds>'), 'duration-seconds');
  assert.equal(ids.get('duration<nanos>'), 'duration-nanos');
  assert.equal(ids.get('duration<60/1,i32>'), 'duration-60/1');
  // Ticks are bigint at every width.
  assert.equal(schema.type('time_point<seconds,u32>').decode(fromHex('01000000')), 1n);
  assert.equal(schema.type('duration<60/1,i32>').decode(fromHex('a6ffffff')), -90n);
});
