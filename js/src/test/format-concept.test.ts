// One generic function, every format (gofractally/psio#18; C++
// cpp/tests/format_concept_tests.cpp, Rust tests/format_concept.rs).
//
// `exercise(format, type, value)` is written once against the `Format`
// interface and run for every format value of the package, for the typed
// declaration and for the schema-driven type, over the shared record of
// conformance/cpp-golden.json. What a format lacks is stated in EXPECTED.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { bool, formats, generateSchema, list, rawString, record, u32, type Format, type FormatType } from '../index.js';

const fixtures: { format: string; id: string; wire_hex: string }[] =
  JSON.parse(readFileSync(new URL('../../src/test/cpp-golden.json', import.meta.url), 'utf8'));

const Record = record('Record', { id: u32, name: rawString, values: list(u32), active: bool });
const text = new TextEncoder();
const values: { [id: string]: unknown } = {
  record: { id: 42, name: text.encode('Alice'), values: [1, 256, 65536], active: true },
  empty_record: { id: 0, name: new Uint8Array(0), values: [], active: false },
};
const hex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');

// What each format has today. Gaps against the concept (psio#18, B7): no
// trusted read, size function or buffer type for any format; a view for
// pssz only.
const EXPECTED: { [name: string]: { text: boolean; view: boolean; golden: boolean } } = {
  pssz: { text: false, view: true, golden: true },
  key: { text: false, view: false, golden: true },
  json: { text: true, view: false, golden: true },
  ijson: { text: true, view: false, golden: false },   // no ijson rows in cpp-golden.json
};

function exercise(format: Format, type: FormatType, value: unknown, golden?: string): void {
  const bytes = format.encode(type, value);
  if (golden !== undefined) assert.equal(hex(bytes), golden, `${format.name}: the C++ bytes`);
  assert.equal(hex(format.encodeChecked(type, value)), hex(bytes), format.name);
  assert.equal(format.validate(type, bytes).ok, true, format.name);
  assert.deepEqual(format.decode(type, bytes), value, format.name);
  if (format.text) new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  if (format.view) assert.ok(format.view(type, bytes) !== undefined, format.name);
  // Bytes that are not a frame.
  if (bytes.length > 8) {
    const cut = bytes.subarray(0, 3);
    assert.equal(format.validate(type, cut).ok, false, format.name);
    assert.throws(() => format.decode(type, cut), format.name);
  }
}

test('format concept: the format values and their capabilities', () => {
  assert.deepEqual(Object.keys(formats).sort(), Object.keys(EXPECTED).sort());
  for (const format of Object.values(formats)) {
    const expected = EXPECTED[format.name];
    assert.equal(format.text, expected.text, format.name);
    assert.equal(format.view !== undefined, expected.view, format.name);
  }
});

for (const format of Object.values(formats)) {
  test(`format concept: ${format.name}`, () => {
    const viaSchema = generateSchema({ Record }).type('Record');
    let goldenRows = 0;
    for (const type of [Record, viaSchema] as FormatType[]) {
      for (const [id, value] of Object.entries(values)) {
        const row = fixtures.find(f => f.format === format.name && f.id === id);
        if (row) ++goldenRows;
        exercise(format, type, value, row?.wire_hex);
      }
    }
    assert.equal(goldenRows > 0, EXPECTED[format.name].golden, format.name);
  });
}
