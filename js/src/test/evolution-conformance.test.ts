// conformance/evolution-vectors.json: pairs of types and of interfaces of
// two Schema IRs, with the verdict of the run-time schema comparison —
// `psszEvolvesFrom` and `psszEquivalent`, each under `wire` and `contract`
// — as { ok, code, path }: the first rule broken, in the order the C++
// reference walks (conformance/evolution-vectors.md).
//
// Every row is run, in every one of its four verdicts; the counts at the
// end are the fixture's. Nothing is skipped.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  PsszSchema, psszEquivalent, psszEvolvesFrom,
  type PsszComparable, type PsszCompare, type PsszCompatibility,
} from '../index.js';

interface Verdict { ok: boolean; code: string; path: string }
interface Verdicts { wire: Verdict; contract: Verdict }
interface TypeRow {
  name: string; old_schema: string; old_type: string; new_schema: string; new_type: string;
  evolves: Verdicts; equivalent: Verdicts;
}
interface InterfaceRow {
  name: string; old_schema: string; old_interface: string; new_schema: string; new_interface: string;
  evolves: Verdicts; equivalent: Verdicts;
}
interface Fixture {
  version: number; schemas: { id: string; ir_hex: string }[]; types: TypeRow[]; interfaces: InterfaceRow[];
}
const fixture: Fixture = JSON.parse(readFileSync(new URL('../../../conformance/evolution-vectors.json', import.meta.url), 'utf8'));
const fromHex = (hex: string) => Uint8Array.from(Buffer.from(hex, 'hex'));

const schemas = new Map<string, PsszSchema>();
for (const s of fixture.schemas) {
  assert.ok(!schemas.has(s.id), `schema id "${s.id}" twice`);
  schemas.set(s.id, PsszSchema.decode(fromHex(s.ir_hex)));
}
const schema = (id: string): PsszSchema => {
  const found = schemas.get(id);
  assert.ok(found, `no schema "${id}"`);
  return found;
};
const verdict = (r: PsszCompatibility): Verdict => (r.ok ? { ok: true, code: '', path: '' } : { ok: false, code: r.code, path: r.path });
const MODES: PsszCompare[] = ['wire', 'contract'];

let verdicts = 0;
function run(name: string, row: { evolves: Verdicts; equivalent: Verdicts }, older: () => PsszComparable, newer: () => PsszComparable): void {
  for (const mode of MODES) {
    const evolves = psszEvolvesFrom(older(), newer(), { compare: mode });
    assert.deepEqual(verdict(evolves), row.evolves[mode], `${name}: evolves (${mode})${evolves.ok ? '' : `: ${evolves.reason}`}`);
    const equivalent = psszEquivalent(older(), newer(), { compare: mode });
    assert.deepEqual(verdict(equivalent), row.equivalent[mode], `${name}: equivalent (${mode})${equivalent.ok ? '' : `: ${equivalent.reason}`}`);
    // A failure carries a sentence for a person beside the code.
    for (const r of [evolves, equivalent]) if (!r.ok) assert.ok(r.reason.length > 0, name);
    verdicts += 2;
  }
}

test('evolution-vectors.json: every type pair, evolves and equivalent, wire and contract', () => {
  assert.equal(fixture.version, 1);
  for (const row of fixture.types)
    run(row.name, row, () => schema(row.old_schema).type(row.old_type), () => schema(row.new_schema).type(row.new_type));
  // The file as checked in: 50 pairs, each in both directions.
  assert.equal(fixture.types.length, 100);
  assert.equal(fixture.types.filter((row) => row.name.endsWith('~reverse')).length, 50);
  assert.equal(verdicts, 400);
});

test('evolution-vectors.json: every interface pair, evolves and equivalent, wire and contract', () => {
  const before = verdicts;
  for (const row of fixture.interfaces)
    run(row.name, row,
      () => schema(row.old_schema).interface(row.old_interface), () => schema(row.new_schema).interface(row.new_interface));
  // 6 pairs, each in both directions.
  assert.equal(fixture.interfaces.length, 12);
  assert.equal(verdicts - before, 48);
});

test('evolution-vectors.json: whole schemas agree with their interfaces; equivalence is symmetric', () => {
  let whole = 0;
  for (const row of fixture.interfaces) {
    const a = schema(row.old_schema);
    const b = schema(row.new_schema);
    if (a.interfaceNames.length !== 1 || b.interfaceNames.length !== 1 || row.old_interface !== row.new_interface) continue;
    for (const mode of MODES) {
      // The same verdict, the path under the interface's name.
      const expected = row.evolves[mode];
      const got = verdict(psszEvolvesFrom(a, b, { compare: mode }));
      assert.deepEqual(got, expected.ok ? expected : { ...expected, path: `${row.old_interface}/${expected.path}` }, row.name);
      ++whole;
    }
  }
  assert.equal(whole, 24);
  for (const row of fixture.types)
    for (const mode of MODES) {
      const forward = psszEquivalent(schema(row.old_schema).type(row.old_type), schema(row.new_schema).type(row.new_type), { compare: mode });
      const back = psszEquivalent(schema(row.new_schema).type(row.new_type), schema(row.old_schema).type(row.old_type), { compare: mode });
      assert.equal(forward.ok, back.ok, `${row.name} (${mode})`);
      // Equivalent types evolve in both directions.
      if (forward.ok) assert.equal(row.evolves[mode].ok, true, row.name);
    }
});

test('a schema that names a type it does not have is malformed-schema, with the place', () => {
  const ir = structuredClone(schema(fixture.interfaces[0].old_schema).ir);
  ir.interfaces[0].type_names = ['Missing'];
  const broken = new PsszSchema(ir);
  const name = fixture.interfaces[0].old_interface;
  for (const compare of MODES) {
    const r = psszEvolvesFrom(broken.interface(name), broken.interface(name), { compare });
    assert.ok(!r.ok && r.code === 'malformed-schema' && r.path === 'type:Missing', JSON.stringify(r));
    const whole = psszEquivalent(broken, broken, { compare });
    assert.ok(!whole.ok && whole.code === 'malformed-schema' && whole.path === `${name}/type:Missing`);
  }
  // A listed type that exists is compared.
  ir.interfaces[0].type_names = [ir.types[0].name];
  const listed = new PsszSchema(ir);
  assert.deepEqual(psszEquivalent(listed.interface(name), listed.interface(name)), { ok: true });
  const removed = psszEvolvesFrom(listed.interface(name), schema(fixture.interfaces[0].old_schema).interface(name));
  assert.ok(!removed.ok && removed.code === 'type-removed' && removed.path === `type:${ir.types[0].name}`);
  const added = psszEquivalent(schema(fixture.interfaces[0].old_schema).interface(name), listed.interface(name));
  assert.ok(!added.ok && added.code === 'type-added' && added.path === `type:${ir.types[0].name}`);
});
