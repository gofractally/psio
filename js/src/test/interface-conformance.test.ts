// conformance/interface-vectors.json: what a method declaration becomes.
// The interface is declared here in the typed layer (the JavaScript
// spelling of cpp/tests/interface_conformance_types.hpp); its Schema IR
// must be the fixture's byte for byte, and every row is replayed through
//
//   typed          the declaration's argument and result codecs,
//   schema root    the fixture's schema, root `token.<method>` (arguments),
//   schema func    the fixture's schema, the interface's `Func`: the
//                  argument list built from its parameters, and its result.
//
// Nothing is skipped: the counts at the end are the fixture's.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  PsszError, PsszSchema, SchemaBuilder, bool, defineInterface, field, functionTypeToJson, generateSchema, list, method,
  optional, rawString, record, u32, u64, u8, type IrFunctionType, type JsonFormat, type PsszType,
} from '../index.js';

interface MethodRow {
  name: string; params: string[]; named: boolean; args: string; result: boolean;
  max_dynamic_data: number | null; max_fields: number | null; width: number;
  /** pssz-spec 3.2: what the width is chosen from; 3.7: the largest argument list. */
  inferred_bound: number | null; max_encoded_size: number | null;
  /** The json text of the method's FunctionType (C++ `SchemaBuilder::insert_method<T, I>()`). */
  function: string;
}
interface Row {
  name: string;
  kind: 'args' | 'args-accept' | 'args-reject' | 'args-pssz-reject' | 'result' | 'result-pssz-reject' | 'result-void';
  method: string; pssz: string | null; json: string | null; ijson: string | null; text: string | null; reason: string | null;
}
interface Fixture {
  version: number; interface: string; declaration: string[]; schema_ir: string; schema_json: string;
  function_schema_ir: string; function_schema_json: string;
  methods: MethodRow[]; vectors: Row[];
}
const fixture: Fixture = JSON.parse(readFileSync(new URL('../../../conformance/interface-vectors.json', import.meta.url), 'utf8'));
const fromHex = (hex: string) => Uint8Array.from(Buffer.from(hex, 'hex'));
const toHex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');
const FORMATS: JsonFormat[] = ['json', 'ijson'];

// record Receipt { id: u64, note: utf8 string of at most 32 bytes }
const Receipt = record('Receipt', { id: u64, note: field(rawString, { utf8: true, max: 32 }) });
// interface token { ... } (conformance/interface-vectors.md "The declaration")
const Token = defineInterface('token', {
  transfer: method(
    { to: field(rawString, { utf8: true, max: 16 }), amount: u64, memo: field(optional(rawString), { utf8: true, max: 80 }) },
    { returns: Receipt, maxDynamicData: 256, maxFields: 8 }),
  names: method({ limit: u32 }, { returns: field(list(rawString), { max: 4, element: { utf8: true, max: 16 } }) }),
  note: method({ text: field(rawString, { max: 62 }) }, { maxDynamicData: 64 }),
  add: method([u32, u32], { returns: u64 }),
  ping: method({}),
  batch: method({ names: field(list(rawString), { max: 4, element: { max: 8 } }), tip: u8 }, { returns: bool }),
  find: method({ id: u64 }, { returns: optional(Receipt) }),
  label: method({ id: u64 }, { returns: field(optional(rawString), { utf8: true, max: 8 }) }),
}, { package: 'iface_conf' });

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

// The C++ typed validator's message, and this codec's for the same refusal.
const REASONS: { [reason: string]: RegExp } = {
  'pssz: member length_bound violated': /^pssz IR: (byte count|sequence) bound exceeded$/,
  'utf8_spec: invalid leading byte': /^pssz IR: string is not valid UTF-8$/,
  'pssz: encoded size exceeds maxDynamicData cap': /^pssz IR: encoded size exceeds maxDynamicData cap$/,
  'pssz: optional<var> missing selector': /^pssz IR: /,
  'pssz: a method with no result returned bytes': /^pssz: a method with no result returned bytes$/,
};

test('interface-vectors.json: the typed declaration is the C++ Interface / Func Schema IR', () => {
  assert.equal(fixture.version, 1);
  assert.equal(fixture.interface, Token.name);
  // Byte for byte, and as text.
  assert.equal(toHex(Token.schema.encode()), fixture.schema_ir);
  assert.equal(Token.schema.toJson('json'), fixture.schema_json);
  assert.equal(toHex(generateSchema({}, { interfaces: [Token] }).encode()), fixture.schema_ir);
  const fetched = PsszSchema.decode(fromHex(fixture.schema_ir));
  assert.deepEqual(fetched.ir, Token.schema.ir);
  assert.equal(toHex(fetched.encode()), fixture.schema_ir);
  assert.equal(toHex(PsszSchema.fromJson(fixture.schema_json, 'json').encode()), fixture.schema_ir);
  assert.deepEqual(fetched.validate(), { ok: true });
  assert.deepEqual(Token.schema.validate(), { ok: true });
  assert.deepEqual(fetched.interfaceNames, ['token']);
  assert.equal(fetched.ir.package.name, 'iface_conf');

  // What the IR holds (conformance/interface-vectors.md).
  const iface = fetched.ir.interfaces[0];
  assert.deepEqual(iface.funcs.map((f) => f.name), fixture.methods.map((m) => m.name));
  const attributes = (name: string) => Object.fromEntries(iface.funcs.find((f) => f.name === name)!.attributes.map((a) => [a.name, a.value]));
  assert.deepEqual(attributes('transfer'), { maxFields: '8', maxDynamicData: '256' });
  assert.deepEqual(attributes('note'), { maxDynamicData: '64' });
  assert.deepEqual(attributes('names'), {});
  assert.deepEqual(iface.funcs.find((f) => f.name === 'add')!.params.map((p) => p.name), ['0', '1']);
  assert.equal(iface.funcs.find((f) => f.name === 'ping')!.result, null);
  // returns(max 4, element(utf8, max 16)) is the bounded type.
  assert.equal(iface.funcs.find((f) => f.name === 'names')!.result!.value.type, 'BoundedList');
  // Without the argument lists: insert_interface<T>() alone.
  const bare = defineInterface('token', { ping: method({}), add: method([u32, u32], { returns: u64 }) }, { argumentLists: false });
  assert.deepEqual(bare.schema.ir.types.map((m) => m.name), ['@0', '@1']);
  assert.equal(bare.schema.ir.interfaces[0].funcs.length, 2);
  assert.equal(toHex(bare.methods.add.args.encode([1, 2])), '080100000002000000');
});

test('interface-vectors.json: the methods as FunctionTypes, without the Interface envelope', () => {
  // What C++ does: a fresh builder, insert_method<token, I>() per method in
  // declaration order.
  const builder = new SchemaBuilder();
  const functions: { [name: string]: IrFunctionType } = {};
  for (const row of fixture.methods) functions[row.name] = builder.insertMethod(Token.method(row.name as 'ping'));
  const schema = builder.build();
  for (const row of fixture.methods) {
    assert.equal(functionTypeToJson(functions[row.name], 'json'), row.function, row.name);
    assert.equal(functions[row.name].result !== null, row.result, row.name);
  }
  // The same schema, byte for byte and as text.
  assert.equal(toHex(schema.encode()), fixture.function_schema_ir);
  assert.equal(schema.toJson('json'), fixture.function_schema_json);
  assert.deepEqual(schema.validate(), { ok: true });
  assert.deepEqual(schema.ir.interfaces, []);

  // The result node carries returns(...): the bounded list of bounded UTF-8
  // strings, where the plain result type is a list of raw strings.
  assert.equal(functions.names.result!.value.type, 'BoundedList');
  assert.equal(functions.label.result!.value.type, 'Option');
  assert.equal(functions.ping.result, null);
  // The spellings: method.functionType(builder), and the parts.
  const again = new SchemaBuilder();
  assert.deepEqual(Token.methods.transfer.functionType(again), functions.transfer);
  const parts = new SchemaBuilder();
  assert.deepEqual(parts.insertMethodArgs(Token.methods.transfer), functions.transfer.params);
  assert.deepEqual(parts.insertMethodResult(Token.methods.transfer), functions.transfer.result);
  assert.equal(parts.insertMethodResult(Token.methods.ping), null);

  // Every result row against the FunctionType's result node.
  let checked = 0;
  for (const row of fixture.vectors) {
    const node = functions[row.method].result;
    if (!node || (row.kind !== 'result' && row.kind !== 'result-pssz-reject')) continue;
    const codec = schema.type(node);
    assert.equal(codec.validate(fromHex(row.pssz!)).ok, row.kind === 'result', row.name);
    if (row.kind === 'result')
      for (const format of FORMATS) assert.equal(codec.psszToJson(fromHex(row.pssz!), format), row[format], `${row.name}: ${format}`);
    ++checked;
  }
  assert.equal(checked, 14 + 6);

  // A returns bound with no type form is carried by a Func only: the bare
  // node is refused instead of saying less than the declaration.
  const api = defineInterface('api', { code: method({}, { returns: field(list(u8), { lengthBound: { min: 1, max: 3 } }) }) });
  for (const run of [() => new SchemaBuilder().insertMethod(api.methods.code), () => new SchemaBuilder().insertMethodResult(api.methods.code)])
    assert.throws(run, (e: unknown) => e instanceof PsszError && e.kind === 'schema' && /lengthBound, which has no type form/.test(e.message));
  assert.equal(new SchemaBuilder().insertMethodArgs(api.methods.code).value.type, 'Type');
  assert.throws(() => new SchemaBuilder().insertMethod({} as never), (e: unknown) => e instanceof PsszError && e.kind === 'schema');
});

test('interface-vectors.json: the methods manifest, typed and schema-driven', () => {
  const fetched = PsszSchema.decode(fromHex(fixture.schema_ir));
  const lookup = fetched.interface('token');
  assert.equal(lookup, lookup);
  assert.equal(fetched.interface().name, 'token');
  assert.equal(lookup.methods.length, fixture.methods.length);
  assert.equal(fixture.methods.length, 8);
  for (const row of fixture.methods) {
    const typed = Token.method(row.name as 'ping');
    const func = lookup.method(row.name);
    const root = fetched.type(row.args);
    assert.equal(row.args, `token.${row.name}`);
    for (const [label, m] of [['typed', typed], ['schema', func]] as const) {
      const name = `${row.name} (${label})`;
      assert.equal(m.name, row.name, name);
      // (`row.params`: the names the declaration lists; a positional method's are "0", "1", ...)
      assert.deepEqual(m.params, row.named ? row.params : m.params.map((_, i) => String(i)), name);
      if (!row.named) assert.deepEqual(row.params, [], name);
      assert.equal(m.named, row.named, name);
      assert.equal(m.maxDynamicData === null ? null : Number(m.maxDynamicData), row.max_dynamic_data, name);
      assert.equal(m.maxFields, row.max_fields, name);
      assert.equal(m.result !== null, row.result, name);
      assert.equal(m.args.width, row.width, name);
      const bound = m.args.inferredBound;
      assert.equal(bound === null ? null : Number(bound), row.inferred_bound, name);
      const largest = m.args.maxEncodedSize;
      assert.equal(largest === null ? null : Number(largest), row.max_encoded_size, name);
    }
    assert.equal(root.width, row.width, row.name);
    assert.equal(root.inferredBound === null ? null : Number(root.inferredBound), row.inferred_bound, row.name);
    assert.equal(root.maxEncodedSize === null ? null : Number(root.maxEncodedSize), row.max_encoded_size, row.name);
    // The argument list a Func describes is the named type C++ emits for it.
    const named = fetched.ir.types.find((m) => m.name === row.args)!.type.value;
    assert.equal(named.type, 'Type');
    const interned = fetched.ir.types.find((m) => m.name === (named.value as { type: string }).type)!.type;
    assert.deepEqual(func.argsIr, interned, row.name);
  }
  assert.throws(() => lookup.method('nope'), (e: unknown) => e instanceof PsszError && e.kind === 'schema');
  assert.throws(() => fetched.interface('nope'), (e: unknown) => e instanceof PsszError && e.kind === 'schema');
  assert.throws(() => Token.method('nope' as 'ping'), (e: unknown) => e instanceof PsszError && e.kind === 'schema');
});

test('interface-vectors.json: every row, typed and schema-driven', () => {
  const fetched = PsszSchema.decode(fromHex(fixture.schema_ir));
  const lookup = fetched.interface('token');
  const counts: { [kind: string]: number } = {};
  let codecsRun = 0;
  const names = new Set<string>();
  for (const row of fixture.vectors) {
    assert.ok(!names.has(row.name), `${row.name}: duplicate`);
    names.add(row.name);
    assert.ok(row.name.startsWith(`${row.kind}/${row.method}/`), row.name);
    counts[row.kind] = (counts[row.kind] ?? 0) + 1;
    const typed = Token.method(row.method as 'ping');
    const func = lookup.method(row.method);
    // A method that returns nothing: its result is no bytes, and only those.
    if (!typed.result) {
      assert.equal(func.result, null, row.name);
      assert.ok(row.kind === 'result-void' || row.kind === 'result-pssz-reject' || !row.kind.startsWith('result'), row.name);
    }
    if (row.kind === 'result-void' || (row.kind === 'result-pssz-reject' && !typed.result)) {
      const bytes = fromHex(row.pssz!);
      for (const [label, m] of [['typed', typed], ['schema func', func]] as const) {
        const name = `${row.name} (${label})`;
        ++codecsRun;
        if (row.kind === 'result-void') {
          assert.equal(row.pssz, '', name);
          assert.deepEqual(m.validateResult(bytes), { ok: true, hasUnknown: false }, name);
          assert.equal(m.resultFromPssz(bytes), undefined, name);
          assert.equal(m.resultView(bytes), undefined, name);
          assert.equal(toHex(m.resultToPssz(undefined as never)), '', name);
          rejects(() => m.resultToPssz(1 as never), `${name}: a value`);
        } else {
          const refused = m.validateResult(bytes);
          assert.ok(!refused.ok && refused.error.kind === 'bytes', `${name}: accepted`);
          assert.match(refused.error.message, REASONS[row.reason!], name);
          rejects(() => m.resultFromPssz(bytes), name);
          rejects(() => m.resultView(bytes), `${name}: view`);
        }
      }
      continue;
    }
    const isResult = row.kind === 'result' || row.kind === 'result-pssz-reject';
    // The one-call result helpers are the result codec.
    if (isResult) {
      const bytes = fromHex(row.pssz!);
      for (const [label, m] of [['typed', typed], ['schema func', func]] as const) {
        const name = `${row.name} (${label} helpers)`;
        if (row.kind === 'result') {
          assert.deepEqual(m.validateResult(bytes), { ok: true, hasUnknown: false }, name);
          const value = m.resultFromPssz(bytes);
          assert.equal(toHex(m.resultToPssz(value as never)), row.pssz, name);
          assert.deepEqual(value, (m.result as unknown as PsszType).decode(bytes), name);
          m.resultView(bytes);
        } else {
          assert.equal(m.validateResult(bytes).ok, false, name);
          rejects(() => m.resultFromPssz(bytes), name);
          rejects(() => m.resultView(bytes), `${name}: view`);
        }
      }
    }
    // The codecs of the row: [label, codec].
    const codecs: [string, PsszType<any, any>][] = isResult
      ? [['typed', typed.result as unknown as PsszType], ['schema func', func.result!]]
      : [['typed', typed.args.type() as PsszType], ['schema root', fetched.type(`token.${row.method}`)], ['schema func', func.args]];
    for (const [label, codec] of codecs) {
      const name = `${row.name} (${label})`;
      assert.ok(codec, name);
      ++codecsRun;
      switch (row.kind) {
        case 'args':
        case 'result': {
          const bytes = fromHex(row.pssz!);
          assert.deepEqual(codec.validate(bytes), { ok: true, hasUnknown: false }, name);
          const value = codec.decode(bytes, { rejectUnknown: true });
          assert.equal(toHex(codec.encode(value)), row.pssz, `${name}: encode`);
          for (const format of FORMATS) {
            assert.equal(codec.valueToJson(value, format), row[format], `${name}: ${format} write`);
            assert.equal(codec.psszToJson(bytes, format), row[format], `${name}: ${format} from bytes`);
            assert.equal(toHex(codec.jsonToPssz(row[format]!, format)), row.pssz, `${name}: ${format} read`);
            assert.deepEqual(codec.jsonToValue(row[format]!, format), value, `${name}: ${format} value`);
          }
          break;
        }
        case 'args-accept': {
          for (const format of FORMATS) {
            assert.equal(toHex(codec.jsonToPssz(row.text!, format)), row.pssz, `${name}: ${format}`);
            assert.equal(toHex(codec.jsonToPssz(Buffer.from(row.text!, 'utf8'), format)), row.pssz, `${name}: ${format} bytes`);
          }
          assert.equal(codec.psszToJson(fromHex(row.pssz!), 'json'), row.json, `${name}: canonical`);
          break;
        }
        case 'args-reject':
          for (const format of FORMATS) {
            assert.equal(rejects(() => codec.jsonToValue(row.text!, format), `${name}: ${format}`).kind, 'json', name);
            rejects(() => codec.jsonToPssz(row.text!, format), `${name}: ${format} to bytes`);
          }
          break;
        case 'args-pssz-reject':
        case 'result-pssz-reject': {
          const bytes = fromHex(row.pssz!);
          const expected = REASONS[row.reason!];
          assert.ok(expected, `${name}: reason ${row.reason}`);
          const refused = codec.validate(bytes);
          assert.ok(!refused.ok && refused.error.kind === 'bytes', `${name}: accepted`);
          assert.match(refused.error.message, expected, name);
          assert.match(rejects(() => codec.decode(bytes), name).message, expected, `${name}: decode`);
          for (const format of FORMATS) assert.match(rejects(() => codec.psszToJson(bytes, format), name).message, expected, `${name}: ${format}`);
          break;
        }
        default:
          assert.fail(`${row.name}: unknown kind ${String(row.kind)}`);
      }
    }
  }
  console.log('interface-vectors.json:', JSON.stringify({ methods: fixture.methods.length, vectors: fixture.vectors.length, codecsRun, ...counts }));
  // The file as checked in (conformance/README.md): 8 methods, 69 rows.
  assert.equal(fixture.vectors.length, 69);
  assert.deepEqual(counts, {
    args: 15, 'args-accept': 11, 'args-reject': 17, 'args-pssz-reject': 4, result: 14, 'result-pssz-reject': 7, 'result-void': 1,
  });
  // Argument rows through three codecs, result rows through two.
  assert.equal(codecsRun, (15 + 11 + 17 + 4) * 3 + (14 + 7 + 1) * 2);
});

test('interface: the JavaScript values of arguments and results are statically typed', () => {
  // Named parameters: an object; a utf8 bound makes a rawString a string.
  const transfer: { to: string; amount: bigint; memo: string | null } = Token.methods.transfer.args.decode(fromHex('0a000a000500000000000000626f62'));
  assert.deepEqual(transfer, { to: 'bob', amount: 5n, memo: null });
  // Positional parameters: the array.
  const add: [number, number] = Token.methods.add.args.decode(fromHex('080100000002000000'));
  assert.deepEqual(add, [1, 2]);
  assert.equal(Token.methods.add.args.valueToJson([1, 2]), '[1,2]');
  assert.equal(toHex(Token.methods.add.args.encodeKey([1, 2])), '0000000100000002');
  assert.deepEqual(Token.methods.add.args.decodeKey(fromHex('0000000100000002')), [1, 2]);
  assert.equal(Token.methods.add.args.view(fromHex('080100000002000000')).get('1'), 2);
  rejects(() => Token.methods.add.args.encode([1, 2, 3] as never), 'three arguments');
  rejects(() => Token.methods.add.args.encode({ 0: 1, 1: 2 } as never), 'an object for a positional list');
  rejects(() => Token.methods.transfer.args.encode(['bob', 5n, null] as never), 'an array for a named list');
  // Results.
  const receipt: { id: bigint; note: string } = Token.methods.transfer.result.decode(fromHex('090700000000000000096f6b'));
  assert.deepEqual(receipt, { id: 7n, note: 'ok' });
  const names: string[] = Token.methods.names.result.decode(fromHex('0207616c696365626f62'));
  assert.deepEqual(names, ['alice', 'bob']);
  const sum: bigint = Token.methods.add.result.decode(fromHex('0300000000000000'));
  assert.equal(sum, 3n);
  const none: null = Token.methods.ping.result;
  assert.equal(none, null);
  assert.equal(Token.methods.note.result, null);
  // The result is the bounded type: a 1-byte offset table, not the 4 of a list of strings.
  assert.equal(Token.methods.names.result.width, 1);
  assert.equal(Token.methods.names.result.inferredBound, 68n);
  assert.equal(toHex(list(rawString).encode([Buffer.from('alice'), Buffer.from('bob')])), '080000000d000000616c696365626f62');
  // The result in one call, as returns types it; the view reads the bounded type's offsets.
  assert.equal(toHex(Token.methods.names.resultToPssz(['alice', 'bob'])), '0207616c696365626f62');
  const read: string[] = Token.methods.names.resultFromPssz(fromHex('0207616c696365626f62'));
  assert.deepEqual(read, ['alice', 'bob']);
  assert.equal(Token.methods.names.validateResult(fromHex('05060708096161616161')).ok, false);
  const view = Token.methods.names.resultView(fromHex('0207616c696365626f62'));
  assert.equal(view.length, 2);
  assert.equal(view.at(1), 'bob');
  rejects(() => Token.methods.names.resultToPssz(['a', 'b', 'c', 'd', 'e']), 'five names');
  // An empty optional result is zero bytes when its payload cannot be empty ...
  assert.equal(toHex(Token.methods.find.resultToPssz(null)), '');
  const found: { id: bigint; note: string } | null = Token.methods.find.resultFromPssz(new Uint8Array(0));
  assert.equal(found, null);
  assert.deepEqual(Token.methods.find.resultFromPssz(fromHex('090700000000000000096f6b')), { id: 7n, note: 'ok' });
  // ... and has a selector byte when it can (a string).
  assert.equal(toHex(Token.methods.label.resultToPssz(null)), '00');
  assert.equal(toHex(Token.methods.label.resultToPssz('')), '01');
  assert.equal(Token.methods.label.validateResult(new Uint8Array(0)).ok, false);
  // A method that returns nothing: no bytes.
  const nothing: undefined = Token.methods.ping.resultFromPssz(new Uint8Array(0));
  assert.equal(nothing, undefined);
  // Every writer refuses a result outside returns(...).
  for (const run of [
    () => Token.methods.names.result.encode(['a', 'b', 'c', 'd', 'e']),
    () => Token.methods.names.result.encode(['a'.repeat(17)]),
    () => Token.methods.names.result.valueToJson(['a'.repeat(17)]),
    () => Token.methods.names.result.jsonToValue('["a","b","c","d","e"]'),
    () => Token.methods.names.result.encodeKey(['a', 'b', 'c', 'd', 'e']),
  ]) rejects(run, 'a result outside its bounds');
});

test('interface: a result bound with no type form is result.lengthBound, enforced on both paths', () => {
  const api = defineInterface('api', {
    code: method({}, { returns: field(list(u8), { lengthBound: { min: 1, max: 3 } }) }),
    tag: method({}, { returns: field(rawString, { utf8: true, lengthBound: { exact: 2 } }) }),
    sorted: method({ ids: field(list(u32), { sorted: true }) }, { returns: field(list(u32), { unique: true }) }),
  });
  const funcs = api.schema.ir.interfaces[0].funcs;
  assert.deepEqual(funcs[0].attributes, [{ name: 'result.lengthBound', value: 'min=1,max=3' }]);
  assert.deepEqual(funcs[1].attributes, [{ name: 'result.lengthBound', value: 'exact=2' }]);
  assert.deepEqual(funcs[2].attributes, []);
  const fetched = PsszSchema.decode(api.schema.encode()).interface('api');
  for (const code of [api.methods.code.result as PsszType<number[]>, fetched.method<unknown, number[]>('code').result!]) {
    assert.equal(toHex(code.encode([1, 2])), '0102');
    assert.deepEqual(code.decode(fromHex('010203')), [1, 2, 3]);
    assert.equal(rejects(() => code.encode([]), 'below min').kind, 'value');
    assert.equal(rejects(() => code.encode([1, 2, 3, 4]), 'above max').kind, 'value');
    assert.equal(code.validate(fromHex('01020304')).ok, false);
    assert.equal(code.validate(new Uint8Array(0)).ok, false);
    assert.equal(rejects(() => code.jsonToValue('[]'), 'text').kind, 'json');
    assert.equal(code.inferredBound, 3n);
  }
  for (const tag of [api.methods.tag.result as PsszType<string>, fetched.method<unknown, string>('tag').result!]) {
    assert.equal(toHex(tag.encode('ab')), '6162');
    rejects(() => tag.encode('abc'), 'not exact');
    assert.equal(tag.validate(fromHex('61')).ok, false);
  }
  // sorted / unique are typed-only: the typed codecs enforce them, a fetched schema does not know them.
  assert.equal(rejects(() => api.methods.sorted.args.encode({ ids: [2, 1] }), 'unsorted arguments').message, 'sorted: elements are not in order');
  assert.equal(rejects(() => api.methods.sorted.result.encode([1, 1]), 'a repeated result').message, 'sorted: elements are not in strictly ascending order');
  assert.equal(api.methods.sorted.result.validate(fromHex('0200000001000000')).ok, false);
  assert.equal(fetched.method('sorted').result!.validate(fromHex('0200000001000000')).ok, true);
  assert.equal(toHex(api.methods.sorted.result.encode([1, 2])), '0100000002000000');

  // Declarations that are refused.
  const schemaError = (run: () => unknown, message: RegExp) =>
    assert.throws(run, (e: unknown) => e instanceof PsszError && e.kind === 'schema' && message.test(e.message));
  schemaError(() => method({ a: u32, b: u32 }, { maxFields: 1 }), /more parameters than its maxFields \(1\)/);
  schemaError(() => method({ a: u32 }, { maxFields: 256 }), /maxFields of the method must be an integer from 0 to 255/);
  schemaError(() => method(Array(256).fill(u8) as []), /at most 255 parameters/);
  schemaError(() => method({ a: field(rawString, { lengthBound: { min: 1 } }) }), /a parameter's length bound is max/);
  schemaError(() => defineInterface('', {}), /an interface needs a name/);
  schemaError(() => defineInterface('x', { m: u32 as never }), /must be declared with method/);
  schemaError(() => generateSchema({}, { interfaces: [api, api] }), /two interfaces are named "api"/);
});
