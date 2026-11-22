// The typed layer produces the Schema IR of C++ `psio::SchemaBuilder`: for
// each shared fixture, every type the fixture's IR records is declared with
// the typed definitions (`support/typed-from-ir.ts`), and `generateSchema`
// of those declarations, inserted in the emitter's order, must encode to the
// fixture's `schema_ir` byte for byte.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { PsszSchema } from '../index.js';
import { fixtureHandlers } from './support/custom-handlers.js';
import { TypedFixture } from './support/typed-from-ir.js';

const fixture = (name: string) =>
  JSON.parse(readFileSync(new URL(`../../../conformance/${name}`, import.meta.url), 'utf8'));
const fromHex = (hex: string) => Uint8Array.from(Buffer.from(hex, 'hex'));
const toHex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');

for (const [file, irTypes] of [['pssz-vectors.json', 591], ['json-vectors.json', 223], ['time-key-vectors.json', 140]] as const) {
  test(`${file}: generateSchema of the typed declarations is the C++ Schema IR`, () => {
    const fx: { schema_ir: string; types: { id: string }[] } = fixture(file);
    const schema = PsszSchema.decode(fromHex(fx.schema_ir));
    const typed = new TypedFixture(schema.ir, fixtureHandlers);
    const names = typed.insertionOrder();
    // Every type of the fixture is declared in the typed layer.
    assert.deepEqual([...typed.unsupported], []);
    for (const row of fx.types) assert.ok(typed.tryDef(row.id), `${row.id}: ${typed.unsupported.get(row.id)}`);
    const generated = typed.generate(names);
    assert.deepEqual(generated.ir, schema.ir);
    assert.equal(toHex(generated.encode()), fx.schema_ir);
    console.log(`${file} typed IR:`, JSON.stringify({
      irTypes: schema.ir.types.length, inserted: names.length, fixtureTypeIds: fx.types.length, notDeclarable: 0,
      identicalToCpp: true,
    }));
    assert.equal(schema.ir.types.length, irTypes);
  });
}
