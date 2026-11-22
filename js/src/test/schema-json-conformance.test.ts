// Cross-language conformance for the Schema IR as text: every row of
// conformance/schema-json-vectors.json. A schema has three serialized forms
// (PSSZ bytes, `json` text, `ijson` text) and all three languages agree on
// each. The C++ and Rust counterparts are
// cpp/tests/schema_json_conformance_tests.cpp and
// rust/psio/tests/schema_json_conformance.rs.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { PsszError, PsszSchema, type JsonFormat } from '../index.js';

interface Fixture {
  version: number;
  schemas: { name: string; ir_hex: string; json: string; ijson: string }[];
  accepts: { name: string; format: JsonFormat; text: string; ir_hex: string }[];
  rejects: { name: string; format: JsonFormat; text: string }[];
}

const fixture: Fixture = JSON.parse(
  readFileSync(new URL('../../../conformance/schema-json-vectors.json', import.meta.url), 'utf8'),
);
const fromHex = (hex: string) => Uint8Array.from(Buffer.from(hex, 'hex'));
const toHex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');
const FORMATS: JsonFormat[] = ['json', 'ijson'];
const utf8 = new TextEncoder();

test('schema json conformance: bytes and both texts of every schema', () => {
  assert.equal(fixture.version, 1);
  assert.ok(fixture.schemas.length >= 7);
  const names = new Set<string>();
  for (const row of fixture.schemas) {
    assert.ok(!names.has(row.name), `duplicate row ${row.name}`);
    names.add(row.name);
    const schema = PsszSchema.decode(fromHex(row.ir_hex));
    assert.equal(toHex(schema.encode()), row.ir_hex, `${row.name}: PSSZ round trip`);
    for (const format of FORMATS) {
      const text = row[format];
      assert.equal(schema.toJson(format), text, `${row.name}: ${format} text`);
      // Text as a string and as its UTF-8 bytes.
      for (const input of [text, utf8.encode(text)]) {
        const back = PsszSchema.fromJson(input, format);
        assert.equal(toHex(back.encode()), row.ir_hex, `${row.name}: bytes from ${format} text`);
        assert.deepEqual(back.ir, schema.ir, `${row.name}: IR from ${format} text`);
      }
    }
  }
  for (const name of ['pssz-vectors', 'json-vectors', 'time-key-vectors', 'schema-of-schema', 'every-alternative'])
    assert.ok(names.has(name), `missing row ${name}`);
});

test('schema json conformance: the hand-built schema holds every IR alternative', () => {
  const row = fixture.schemas.find((r) => r.name === 'every-alternative');
  assert.ok(row);
  const schema = PsszSchema.fromJson(row.ijson);
  // The alternatives the IR has are the ones the schema of psio::Schema lists.
  const anyType = PsszSchema.meta().ir.types.find((m) => m.name === 'AnyType')!.type.value;
  assert.equal(anyType.type, 'Object');
  const variant = (anyType.value as { members: { type: { value: { type: string; value: { members: unknown[] } } } }[] })
    .members[0].type.value;
  assert.equal(variant.type, 'Variant');
  assert.equal(new Set(schema.ir.types.map((m) => m.type.value.type)).size, variant.value.members.length);
  assert.equal(variant.value.members.length, 15);
  assert.equal(schema.ir.ir_version, null);
  assert.ok(schema.ir.interfaces.length > 0 && schema.ir.worlds.length > 0 && schema.ir.uses.length > 0);
  assert.ok(schema.ir.package.attributes.length > 0);
  // 64-bit integers are exact on both sides of 2^53 - 1.
  const len = (name: string) => {
    const type = schema.ir.types.find((m) => m.name === name)!.type.value;
    assert.equal(type.type, 'Array');
    return (type.value as { len: bigint }).len;
  };
  assert.equal(len('array-safe'), 9007199254740991n);
  assert.equal(len('array-beyond-safe'), 9007199254740992n);
  assert.equal(len('array-max'), 18446744073709551615n);
  assert.ok(row.json.includes('"len":18446744073709551615'));
  assert.ok(row.ijson.includes('"len":"18446744073709551615"'));
  assert.ok(row.ijson.includes('"len":9007199254740991'));
  assert.ok(row.ijson.includes('"len":"9007199254740992"'));
});

test('schema json conformance: accepted texts read as the recorded schema', () => {
  assert.ok(fixture.accepts.length >= 18);
  for (const row of fixture.accepts)
    assert.equal(toHex(PsszSchema.fromJson(row.text, row.format).encode()), row.ir_hex, row.name);
});

test('schema json conformance: rejected texts are rejected', () => {
  assert.ok(fixture.rejects.length >= 40);
  for (const row of fixture.rejects) {
    for (const input of [row.text, utf8.encode(row.text)]) {
      try {
        PsszSchema.fromJson(input, row.format);
      } catch (e) {
        assert.ok(e instanceof PsszError, `${row.name}: threw ${String(e)}`);
        assert.equal(e.kind, 'json', `${row.name}: ${e.message}`);
        continue;
      }
      assert.fail(`${row.name}: accepted`);
    }
  }
});
