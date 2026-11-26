// The reserved custom id `json` (any JSON value: C++ `psio::json_value`),
// the variant attribute `untagged`, and errors that name the member — the
// cases of cpp/tests/json_api_tests.cpp ("json_value: ...", "json: an
// untagged variant ...", "json: untagged variants in a record, in PSSZ and
// through the schema", "errors that name the member"), over hand-written
// IR and over the typed layer.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  PsszError, PsszJsonText, PsszSchema, bool, bytes, custom, emitWit, f64, generateSchema, i32, jsonValue, list, map, optional,
  psszEquivalent, psszEvolvesFrom, record, u32, u64, u8, utf8, variant, defineInterface, method,
  type Infer, type JsonFormat, type PsszErrorKind,
} from '../index.js';

const FORMATS: JsonFormat[] = ['json', 'ijson'];
const toHex = (b: Uint8Array) => Buffer.from(b).toString('hex');
function rejects(run: () => unknown, kind: PsszErrorKind, message?: string | RegExp): void {
  assert.throws(run, (e: unknown) => {
    assert.ok(e instanceof PsszError, String(e));
    assert.equal(e.kind, kind, e.message);
    if (typeof message === 'string') assert.equal(e.message, message);
    else if (message) assert.match(e.message, message);
    return true;
  });
}

// ── Hand-written IR ─────────────────────────────────────────────────────────

const U8 = { Int: { bits: 8, isSigned: false } } as const;
const UTF8 = { Custom: { type: { List: U8 }, id: 'utf8' } } as const;
const JSON_VALUE = { Custom: { type: UTF8, id: 'json' } } as const;
// struct Pretty { string service; json_value data; optional<json_value> more; vector<json_value> list; }
const pretty = new PsszSchema({
  Pretty: { Object: { service: UTF8, data: JSON_VALUE, more: { Option: JSON_VALUE }, list: { List: JSON_VALUE } } },
  // The same bytes: the JSON texts as UTF-8 strings.
  PrettyText: { Object: { service: UTF8, data: UTF8, more: { Option: UTF8 }, list: { List: UTF8 } } },
  Any: JSON_VALUE,
  Text: UTF8,
});
const Pretty = pretty.type('Pretty');
const PrettyText = pretty.type('PrettyText');
const Any = pretty.type('Any');
const Text = pretty.type('Text');

test('json value: a document read and written again is the same text', () => {
  // Numbers keep their text: nothing is rounded, renormalised or widened.
  const text = '{"a":1.50,"b":"x\\n","c":[1,2,{"d":null}],"big":18446744073709551616,"e":1e3,"neg":-0,"z":true,"o":{}}';
  for (const format of FORMATS) {
    const packed = Any.jsonToPssz(text, format);
    // In PSSZ: the UTF-8 string holding the compact text.
    assert.deepEqual(packed, Text.encode(text));
    assert.equal(Any.psszToJson(packed, format), text);
    // As a JavaScript value: plain data.
    const value = Any.jsonToValue(text, format) as Record<string, unknown>;
    assert.deepEqual(Object.keys(value), ['a', 'b', 'c', 'big', 'e', 'neg', 'z', 'o']);
    assert.equal(value.a, 1.5);
    assert.equal(value.b, 'x\n');
    assert.deepEqual(value.c, [1, 2, { d: null }]);
    assert.equal(value.big, 18446744073709551616n);
    assert.equal(value.e, 1000);
    assert.ok(Object.is(value.neg, -0));
    assert.equal(value.z, true);
    assert.deepEqual(value.o, {});
    assert.deepEqual(Any.decode(packed), value);
    // ... which is the same value under another text.
    assert.equal(Any.valueToJson(value, format),
      '{"a":1.5,"b":"x\\n","c":[1,2,{"d":null}],"big":18446744073709551616,"e":1000,"neg":-0,"z":true,"o":{}}');
    // The exact text, where it matters.
    assert.deepEqual(Any.encode(new PsszJsonText(text)), packed);
    assert.equal(Any.valueToJson(new PsszJsonText(text), format), text);
  }
  assert.equal(Any.psszToJson(Any.jsonToPssz(' [ 1 , "a" ] ')), '[1,"a"]');
  assert.deepEqual(Any.jsonToValue(' [ 1 , "a" ] '), [1, 'a']);
  // Scalars at the root.
  assert.equal(Any.psszToJson(Any.jsonToPssz('null')), 'null');
  assert.equal(Any.jsonToValue('null'), null);
  assert.equal(Any.psszToJson(Any.jsonToPssz('-12.5e-3')), '-12.5e-3');
  assert.equal(Any.jsonToValue('-12.5e-3'), -0.0125);
  // A number beyond a double keeps its token.
  const huge = Any.jsonToValue('1e999');
  assert.ok(huge instanceof PsszJsonText && huge.text === '1e999');
  assert.equal(Any.valueToJson(huge), '1e999');

  // Not JSON: an error, on every reading entry point.
  for (const bad of ['{"a":1,}', '[1 2]', '', '01', '{"a":1}x', 'nul'])
    for (const format of FORMATS) {
      rejects(() => Any.jsonToValue(bad, format), 'json');
      rejects(() => Any.jsonToPssz(bad, format), 'json');
    }
  // Nesting is bounded like every reader's.
  rejects(() => Any.jsonToValue('['.repeat(65)), 'json');
  rejects(() => Any.jsonToPssz('['.repeat(65) + ']'.repeat(65)), 'json', 'json: max depth exceeded');
  assert.equal(Any.psszToJson(Any.jsonToPssz('['.repeat(64) + ']'.repeat(64))), '['.repeat(64) + ']'.repeat(64));
});

test('json value: repeated keys, values built in code, values with no JSON text', () => {
  // The text walk copies the members as written; the JavaScript value keeps
  // the last value of a repeated key, at the position of the first.
  const repeated = '{"a":1,"b":2,"a":{"x":[]},"b":3}';
  assert.equal(Any.psszToJson(Any.jsonToPssz(repeated)), repeated);
  assert.equal(Any.valueToJson(Any.jsonToValue(repeated)), '{"a":{"x":[]},"b":3}');
  assert.equal(Any.valueToJson(Any.decode(Any.jsonToPssz(repeated))), '{"a":{"x":[]},"b":3}');

  const built = { service: 'tokens', amount: 5, ratio: 0.25, ok: true, none: null, list: [1, -2, 'three'] };
  assert.equal(Any.valueToJson(built), '{"service":"tokens","amount":5,"ratio":0.25,"ok":true,"none":null,"list":[1,-2,"three"]}');
  assert.deepEqual(Any.decode(Any.encode(built)), built);
  // A number is its text; a json value does not know its width, so ijson
  // writes it as it is.
  assert.equal(Any.valueToJson(18446744073709551615n, 'ijson'), '18446744073709551615');
  assert.equal(Any.decode(Any.encode(18446744073709551615n)), 18446744073709551615n);
  assert.equal(Any.valueToJson({ __proto__: null, k: [new PsszJsonText(' 1.50 '), 'x'] }), '{"k":[1.50,"x"]}');
  const own = Any.jsonToValue('{"__proto__":1,"k":2}') as object;
  assert.deepEqual(Object.keys(own), ['__proto__', 'k']);
  assert.equal(Object.getPrototypeOf(own), Object.prototype);

  // A value with no JSON text is an error, not output.
  for (const run of [Any.encode.bind(Any), Any.valueToJson.bind(Any), Any.encodeKey.bind(Any)] as ((v: unknown) => unknown)[]) {
    rejects(() => run(new PsszJsonText('0x10')), 'value', 'json: the text of a json value is not JSON');
    rejects(() => run(Infinity), 'value', 'json: a json_value number is not a JSON number');
    rejects(() => run(NaN), 'value');
    rejects(() => run('\ud800'), 'value');
    rejects(() => run(undefined), 'value');
    rejects(() => run({ a: undefined }), 'value');
    rejects(() => run(new Map()), 'value');
    rejects(() => run(new Uint8Array(1)), 'value');
    rejects(() => run(new PsszJsonText('')), 'value');
    rejects(() => run(new PsszJsonText('1 2')), 'value');
    let deep: unknown = 1;
    for (let i = 0; i < 65; ++i) deep = [deep];
    rejects(() => run(deep), 'value', 'json: max depth exceeded');
    rejects(() => run([new PsszJsonText('['.repeat(64) + ']'.repeat(64))]), 'value');
  }
  // The I-JSON string rule is the only difference between the formats.
  const nonchar = '￿';
  assert.equal(Any.valueToJson(nonchar, 'json'), '"￿"');
  rejects(() => Any.valueToJsonChecked(nonchar, 'ijson'), 'value');
  rejects(() => Any.jsonToValue('"￿"', 'ijson'), 'json');
  rejects(() => Any.jsonToPssz('{"k":"\\uffff"}', 'ijson'), 'json');
  assert.deepEqual(Any.jsonToValue('"￿"', 'json'), nonchar);
  // (the bytes hold it either way)
  assert.equal(Any.decode(Any.encode(nonchar)), nonchar);
  rejects(() => Any.psszToJson(Any.encodeChecked(nonchar), 'ijson'), 'value');
  // The key of a json value is the key of its text.
  assert.deepEqual(Any.encodeKey({ a: [1] }), Text.encodeKey('{"a":[1]}'));
  assert.deepEqual(Any.decodeKey(Any.encodeKey({ a: [1] })), { a: [1] });
});

test('json value: a member of a record, in text and in PSSZ (hand-written IR)', () => {
  const text = '{"service":"tokens","data":{"amount":"5","memo":[1.0,null]},"more":7,"list":[true,"x",{}]}';
  const twin = { service: 'tokens', data: '{"amount":"5","memo":[1.0,null]}', more: '7', list: ['true', '"x"', '{}'] };
  // PSSZ: each json value is a UTF-8 string holding its compact text.
  const packed = PrettyText.encode(twin);
  for (const format of FORMATS) {
    assert.deepEqual(Pretty.jsonToPssz(text, format), packed);
    assert.equal(Pretty.psszToJson(packed, format), text);
    assert.deepEqual(Pretty.jsonToValue(text, format),
      { service: 'tokens', data: { amount: '5', memo: [1, null] }, more: 7, list: [true, 'x', {}] });
  }
  assert.deepEqual(Pretty.decode(packed), { service: 'tokens', data: { amount: '5', memo: [1, null] }, more: 7, list: [true, 'x', {}] });
  assert.deepEqual(Pretty.validate(packed), { ok: true, hasUnknown: false });
  const view = Pretty.view(packed) as { get(name: string): unknown };
  assert.deepEqual(view.get('data'), { amount: '5', memo: [1, null] });
  assert.equal(view.get('more'), 7);
  // A missing optional member is none; a missing json value is an error
  // (null is a value: it has to be written).
  const q = Pretty.jsonToValue('{"service":"s","data":null,"list":[]}') as { data: unknown; more: unknown };
  assert.equal(q.data, null);
  assert.equal(q.more, null);
  rejects(() => Pretty.jsonToValue('{"service":"s","list":[]}'), 'json', 'json: missing required member "data"');
  rejects(() => Pretty.encodeChecked({ service: 's', list: [] }), 'value');
  // An optional json value reads `null` as empty.
  assert.deepEqual(Pretty.jsonToPssz('{"service":"s","data":null,"more":null,"list":[]}'),
    PrettyText.encode({ service: 's', data: 'null', more: null, list: [] }));

  // Bytes that are not JSON text are not a value; text that is JSON but not
  // compact is a value with another, canonical, encoding.
  const bad = PrettyText.encode({ ...twin, data: '{not json' });
  const refused = Pretty.validate(bad);
  assert.ok(!refused.ok && refused.error.kind === 'bytes' && refused.error.message === 'pssz IR: the text of a json value is not JSON');
  rejects(() => Pretty.decode(bad), 'bytes');
  rejects(() => Pretty.psszToJson(bad), 'bytes');
  for (const not of ['', ' ', '1 2', '﻿1', '"\\ud800"'])
    assert.equal(Any.validate(Text.encode(not)).ok, false, JSON.stringify(not));
  const loose = PrettyText.encode({ ...twin, data: '{ "amount" : "5" }' });
  assert.equal(Pretty.validate(loose).ok, true);
  // Loose text in the bytes is presented compact.
  assert.equal(Pretty.psszToJson(loose), '{"service":"tokens","data":{"amount":"5"},"more":7,"list":[true,"x",{}]}');
  assert.notDeepEqual(Pretty.encode(Pretty.decode(loose)), loose);
  assert.deepEqual(Pretty.encode(Pretty.decode(loose)), PrettyText.encode({ ...twin, data: '{"amount":"5"}' }));
});

test('json value: the id is reserved, sits on a UTF-8 string, and is a custom id to a comparison', () => {
  // No handler may claim the id.
  rejects(() => new PsszSchema({ T: U8 }, { handlers: { json: {} } }), 'schema', /"json"/);
  rejects(() => pretty.withHandlers({ json: { check: () => null } }), 'schema');
  rejects(() => custom('json', utf8), 'schema', /built-in id/);
  // A json value sits directly on a UTF-8 string.
  for (const inner of [{ Custom: { type: { List: U8 }, id: 'string' } }, { List: U8 }, U8, { Custom: { type: UTF8, id: 'box' } }])
    rejects(() => new PsszSchema({ T: { Custom: { type: inner, id: 'json' } } } as never).type('T'), 'schema',
      'pssz IR: a json value must sit directly on a UTF-8 string');
  // (a named alias of one is one; so is a bounded one)
  const aliased = new PsszSchema({ S: UTF8, T: { Custom: { type: 'S', id: 'json' } }, B: { Custom: { type: { Custom: { type: { BoundedList: { type: U8, maxCount: 8 } }, id: 'utf8' } }, id: 'json' } } });
  assert.equal(aliased.type('T').psszToJson(aliased.type('T').jsonToPssz('[1]')), '[1]');
  assert.equal(aliased.type('B').valueToJson([1, 2, 3]), '[1,2,3]');
  rejects(() => aliased.type('B').jsonToPssz('[1,2,3,4]'), 'json', 'json: string exceeds its bound');
  rejects(() => aliased.type('B').encodeChecked([1, 2, 3, 4]), 'value');
  // The same bytes as a UTF-8 string; another type to a consumer of its text.
  assert.deepEqual(psszEquivalent(Any, Text, { compare: 'wire' }), { ok: true });
  const contract = psszEquivalent(Any, Text);
  assert.ok(!contract.ok && contract.code === 'custom-id-changed' && contract.path === '(root)');
  assert.deepEqual(psszEvolvesFrom(Pretty, PrettyText, { compare: 'wire' }), { ok: true });
  assert.equal((psszEvolvesFrom(Pretty, PrettyText) as { code: string; path: string }).path, '.data');
});

test('json value: the typed definition is the C++ schema node', () => {
  const Typed = record('Pretty', { service: utf8, data: jsonValue, more: optional(jsonValue), list: list(jsonValue) });
  const schema = generateSchema({ Pretty: Typed });
  const members = (schema.ir.types.find((m) => m.name === 'Pretty')!.type.value as { value: { members: { type: { value: { value: { type: string } } } }[] } }).value.members;
  const node = schema.ir.types.find((m) => m.name === members[1].type.value.value.type)!.type.value;
  // Custom{ Custom{ List<u8>, "utf8" }, "json" }
  assert.equal(node.type, 'Custom');
  const outer = node.value as { id: string; type: { value: { type: string; value: { id: string; type: { value: { type: string } } } } } };
  assert.equal(outer.id, 'json');
  assert.equal(outer.type.value.type, 'Custom');
  assert.equal(outer.type.value.value.id, 'utf8');
  assert.equal(outer.type.value.value.type.value.type, 'List');
  assert.deepEqual(schema.validate(), { ok: true });
  const text = '{"service":"tokens","data":{"amount":"5","memo":[1.0,null]},"more":7,"list":[true,"x",{}]}';
  const packed = Typed.jsonToPssz(text);
  assert.deepEqual(packed, Pretty.jsonToPssz(text));
  assert.equal(Typed.psszToJson(packed, 'json'), text);
  const value = Typed.decode(packed);
  assert.deepEqual(value.data, { amount: '5', memo: [1, null] });
  assert.deepEqual(Typed.encode({ service: 'tokens', data: { amount: '5', memo: [new PsszJsonText('1.0'), null] }, more: 7, list: [true, 'x', {}] }), packed);
  // A fetched copy of the typed schema reads the same.
  const fetched = PsszSchema.decode(schema.encode()).type('Pretty');
  assert.equal(fetched.psszToJson(packed), text);
  assert.deepEqual(psszEquivalent(Typed, Pretty), { ok: true });
  // A map keyed by text with JSON values; a json value is not a string key.
  const M = map(utf8, jsonValue);
  assert.equal(M.valueToJson(new Map([['b', [1]], ['a', { k: null }]])), '{"a":{"k":null},"b":[1]}');
  assert.equal(map(jsonValue, u8).valueToJson(new Map([['a', 1]])), '[["a",1]]');
});

// ── Untagged variants ───────────────────────────────────────────────────────

const Point = record('Point', { x: i32, y: i32 });
// using Extra = std::variant<std::string, std::vector<std::string>>;
const Extra = variant({ 0: utf8, 1: list(utf8) }, { untagged: true });
const TaggedExtra = variant({ 0: utf8, 1: list(utf8) });
// using Mixed = std::variant<bool, int, std::string, std::vector<int>, Point>;
const Mixed = variant({ 0: bool, 1: i32, 2: utf8, 3: list(i32), 4: Point }, { untagged: true });
const NONE = "json: the value is none of the untagged variant's alternatives";
const AMBIGUOUS = 'json: the alternatives of an untagged variant are not told apart by the kind of JSON value';

test('untagged variant: the bare value of its alternative', () => {
  for (const format of FORMATS) {
    assert.equal(Extra.valueToJson({ type: '1', value: ['a'] }, format), '["a"]');
    assert.equal(Extra.valueToJson({ type: '0', value: 's' }, format), '"s"');
    assert.equal(Extra.valueToJson({ type: '1', value: ['a', 'b'] }, format), '["a","b"]');
    assert.deepEqual(Extra.jsonToValue('["a"]', format), { type: '1', value: ['a'] });
    assert.deepEqual(Extra.jsonToValue(' "x" ', format), { type: '0', value: 'x' });
    assert.deepEqual(Extra.jsonToValue('[]', format), { type: '1', value: [] });
    // A value of no alternative's kind; a value of the right kind that is
    // not the alternative.
    rejects(() => Extra.jsonToValue('7', format), 'json', NONE);
    rejects(() => Extra.jsonToValue('[1]', format), 'json');
    rejects(() => Extra.jsonToValue('{"string":"x"}', format), 'json', NONE);
    rejects(() => Extra.jsonToValue('{"0":"x"}', format), 'json', NONE);
    rejects(() => Extra.jsonToValue('[0,"x"]', format), 'json');

    // Five kinds, five alternatives.
    const all: Infer<typeof Mixed>[] = [
      { type: '0', value: true }, { type: '1', value: 7 }, { type: '2', value: 's' }, { type: '3', value: [1, 2] },
      { type: '4', value: { x: 3, y: 4 } },
    ];
    const text = '[true,7,"s",[1,2],{"x":3,"y":4}]';
    assert.equal(list(Mixed).valueToJson(all, format), text);
    assert.deepEqual(list(Mixed).jsonToValue(text, format), all);
    rejects(() => Mixed.jsonToValue('null', format), 'json', NONE);
    rejects(() => Mixed.jsonToValue('1.5', format), 'json');   // a number: the i32
  }
  // Binary forms are untouched: the selector byte, then the alternative.
  const one = Extra.encode({ type: '1', value: ['a'] });
  assert.equal(one[0], 1);
  assert.deepEqual(one, TaggedExtra.encode({ type: '1', value: ['a'] }));
  assert.equal(TaggedExtra.valueToJson({ type: '1', value: ['a'] }, 'ijson'), '{"1":["a"]}');
  assert.equal(TaggedExtra.valueToJson({ type: '1', value: ['a'] }, 'json'), '[1,["a"]]');
});

test('untagged variants in a record, in PSSZ and through the schema', () => {
  // struct Config { map<string, Extra> extra; optional<Extra> one; vector<Mixed> mixed; }
  const Config = record('Config', { extra: map(utf8, Extra), one: optional(Extra), mixed: list(Mixed) });
  const value = {
    extra: new Map([['hosts', { type: '1', value: ['a', 'b'] }], ['name', { type: '0', value: 'n' }]]),
    one: { type: '0', value: 'one' },
    mixed: [{ type: '4', value: { x: 1, y: 2 } }, { type: '0', value: false }],
  } as never;
  const text = '{"extra":{"hosts":["a","b"],"name":"n"},"one":"one","mixed":[{"x":1,"y":2},false]}';
  const packed = Config.encode(value);
  for (const format of FORMATS) {
    assert.equal(Config.valueToJson(value, format), text);
    assert.deepEqual(Config.jsonToValue(text, format), value);
    assert.deepEqual(Config.jsonToPssz(text, format), packed);
    assert.equal(Config.psszToJson(packed, format), text);
  }
  // An optional untagged variant: null is none.
  assert.equal(Config.jsonToValue('{"extra":{},"one":null,"mixed":[]}').one, null);

  // The schema says `untagged`, and the schema-driven codecs follow.
  const built = generateSchema({ Config });
  assert.deepEqual(built.validate(), { ok: true });
  const untagged = built.ir.types.filter((m) =>
    m.type.value.type === 'Variant' && m.type.value.value.attributes.some((a) => a.name === 'untagged' && a.value === '')).length;
  assert.equal(untagged, 2);
  const fetched = PsszSchema.decode(built.encode()).type('Config');
  for (const format of FORMATS) {
    assert.equal(fetched.psszToJson(packed, format), text);
    assert.deepEqual(fetched.jsonToPssz(text, format), packed);
  }
  rejects(() => fetched.jsonToPssz('{"extra":{"k":7},"one":null,"mixed":[]}'), 'json', NONE);
  // The schema text carries the attribute, too.
  assert.equal(PsszSchema.fromJson(built.toJson()).type('Config').psszToJson(packed), text);

  // A schema whose alternatives cannot be told apart is refused where a
  // value is read (hand-written IR, as the C++ case) — and where one is
  // written.
  const ambiguous = new PsszSchema({
    V: { Variant: { members: [
      { name: '0', type: { Int: { bits: 32, isSigned: false } }, attributes: [] },
      { name: '1', type: { Float: { exp: 11, mantissa: 53 } }, attributes: [] },
    ], attributes: [{ name: 'untagged', value: '' }] } },
  } as never);
  assert.deepEqual(ambiguous.validate(), { ok: true });
  const V = ambiguous.type('V');
  for (const format of FORMATS) {
    rejects(() => V.jsonToPssz('1', format), 'json', AMBIGUOUS);
    rejects(() => V.valueToJsonChecked({ type: '0', value: 1 }, format), 'value', AMBIGUOUS);
    // (a kind neither has is none of them)
    rejects(() => V.jsonToPssz('"x"', format), 'json', NONE);
  }
  assert.deepEqual(V.decode(V.encode({ type: '1', value: 1.5 })), { type: '1', value: 1.5 });
  // The pairs docs/json.md names: a 64-bit integer may be a string, bytes
  // may be a string; an optional is also null; a json value is every kind.
  for (const [a, b] of [[u64, utf8], [utf8, bytes], [optional(u8), optional(utf8)], [jsonValue, bool], [map(utf8, u8), Point]] as const)
    rejects(() => variant({ a, b }, { untagged: true }).valueToJsonChecked({ type: 'a', value: null as never }), 'value', AMBIGUOUS);
  // An untagged variant inside an untagged variant has its alternatives' kinds.
  const Outer = variant({ inner: Extra, n: u32 }, { untagged: true });
  assert.deepEqual(Outer.jsonToValue('["a"]'), { type: 'inner', value: { type: '1', value: ['a'] } });
  assert.deepEqual(Outer.jsonToValue('5'), { type: 'n', value: 5 });
  assert.equal(Outer.valueToJson({ type: 'inner', value: { type: '0', value: 's' } }), '"s"');
  // A custom type counts as a string when a text handler is registered for it.
  const Id = custom('Id', u32, { toText: (v) => `#${v}`, fromText: (t) => Number(t.slice(1)) });
  const WithId = variant({ id: Id, n: u32 }, { untagged: true });
  assert.equal(WithId.valueToJson({ type: 'id', value: 7 }), '"#7"');
  assert.deepEqual(WithId.jsonToValue('"#7"'), { type: 'id', value: 7 });
  assert.deepEqual(WithId.jsonToValue('7'), { type: 'n', value: 7 });

  // Whether a variant is untagged is part of its text contract.
  assert.deepEqual(psszEquivalent(TaggedExtra, Extra, { compare: 'wire' }), { ok: true });
  const contract = psszEquivalent(TaggedExtra, Extra);
  assert.ok(!contract.ok && contract.code === 'text-form-changed' && contract.path === '(root)');
  const evolved = psszEvolvesFrom(Extra, TaggedExtra);
  assert.ok(!evolved.ok && evolved.code === 'text-form-changed');
  assert.deepEqual(psszEvolvesFrom(Extra, TaggedExtra, { compare: 'wire' }), { ok: true });
  // Options are checked.
  rejects(() => variant({ a: u8 }, { untagged: 1 as never }), 'schema');
  // An unknown variant attribute is still refused.
  rejects(() => new PsszSchema({ V: { Variant: { members: [{ name: 'a', type: U8, attributes: [] }], attributes: [{ name: 'tagged', value: '' }] } } } as never).type('V'), 'schema');
});

// ── Errors that name the member; positional parameters ──────────────────────

test('a missing required member is named', () => {
  // struct Ev { u32 id; string event; optional<u32> n; }
  const Ev = record('Ev', { id: u32, event: utf8, n: optional(u32) });
  const fetched = PsszSchema.decode(generateSchema({ Ev }).encode()).type('Ev');
  for (const format of FORMATS)
    for (const type of [Ev, fetched]) {
      rejects(() => type.jsonToValue('{"id":3}', format), 'json', 'json: missing required member "event"');
      rejects(() => type.jsonToPssz('{"event":"x"}', format), 'json', 'json: missing required member "id"');
      rejects(() => type.jsonToValue('{}', format), 'json', 'json: missing required member "id"');
      assert.deepEqual(type.jsonToValue('{"id":3,"event":"x"}', format), { id: 3, event: 'x', n: null });
    }
  // ... and so does a named argument list.
  const api = defineInterface('api', { post: method({ id: u32, event: utf8 }), add: method([u32, u32], { returns: u64 }) });
  rejects(() => api.methods.post.args.jsonToValue('{"id":1}'), 'json', 'json: missing required member "event"');
  rejects(() => api.schema.interface('api').method('post').args.jsonToPssz('{"event":"x"}'), 'json', 'json: missing required member "id"');
});

test('positional parameters are named "0", "1", ... and printed arg0, arg1 in WIT', () => {
  const api = defineInterface('api', { post: method({ id: u32, event: utf8 }), add: method([u32, u32], { returns: u64 }), ping: method([]) });
  const funcs = api.schema.ir.interfaces[0].funcs;
  assert.deepEqual(funcs[0].params.map((p) => p.name), ['id', 'event']);
  assert.deepEqual(funcs[1].params.map((p) => p.name), ['0', '1']);
  assert.deepEqual(api.methods.add.params, ['0', '1']);
  assert.equal(api.methods.add.named, false);
  const lookup = PsszSchema.decode(api.schema.encode()).interface('api');
  assert.deepEqual(lookup.method('add').params, ['0', '1']);
  assert.equal(lookup.method('add').named, false);
  assert.equal(lookup.method('post').named, true);
  assert.equal(lookup.method('add').args.valueToJson([1, 2]), '[1,2]');
  assert.equal(toHex(lookup.method('add').args.jsonToPssz('[1,2]')), '080100000002000000');
  rejects(() => lookup.method('add').args.jsonToValue('{"0":1,"1":2}'), 'json');
  // C++ `emit_wit` (`schema_types::parameter_identifier`).
  const wit = emitWit(api.schema);
  assert.ok(wit.includes('  post: func(id: u32, event: string);\n'), wit);
  assert.ok(wit.includes('  add: func(arg0: u32, arg1: u32) -> u64;\n'), wit);
  assert.ok(wit.includes('  ping: func();\n'), wit);
  // A schema written before positional names existed ("" for each) reads the same.
  const old = structuredClone(api.schema.ir);
  for (const p of old.interfaces[0].funcs[1].params) p.name = '';
  const legacy = new PsszSchema(old).interface('api').method('add');
  assert.equal(legacy.named, false);
  assert.equal(legacy.args.valueToJson([1, 2]), '[1,2]');
  assert.ok(emitWit(new PsszSchema(old)).includes('  add: func(arg: u32, arg: u32) -> u64;\n'));
});

// ── Spec text: §4.1, §7.1 ───────────────────────────────────────────────────

test('a bool is exactly 0 or 1; only optional fields may be appended', () => {
  // docs/pssz-spec.md 4.1.
  assert.deepEqual(bool.encode(true), Uint8Array.of(1));
  assert.deepEqual(bool.encode(false), Uint8Array.of(0));
  assert.equal(bool.decode(Uint8Array.of(1)), true);
  for (const byte of [2, 3, 0x80, 0xff]) {
    rejects(() => bool.decode(Uint8Array.of(byte)), 'bytes', 'pssz IR: invalid bool');
    assert.equal(bool.validate(Uint8Array.of(byte)).ok, false);
    assert.equal(list(bool).validate(Uint8Array.of(1, byte)).ok, false);
    assert.equal(record('B', { b: bool }, { fixed: true }).validate(Uint8Array.of(byte)).ok, false);
    assert.equal(optional(bool).validate(Uint8Array.of(byte)).ok, false);
  }
  rejects(() => bool.encodeChecked(1 as never), 'value');
  // docs/pssz-spec.md 7.1 / 7.5: an appended field is optional.
  // (a record that will grow reserves room: without the cap the appended
  // field would move the record's offset width, which is another rule)
  const V1 = record('R', { id: u32 }, { maxDynamicData: 1024 });
  const optionalField = record('R', { id: u32, note: optional(utf8) }, { maxDynamicData: 1024 });
  const required = record('R', { id: u32, count: u32 }, { maxDynamicData: 1024 });
  const moved = psszEvolvesFrom(record('R', { id: u32 }), record('R', { id: u32, note: optional(utf8) }));
  assert.ok(!moved.ok && moved.code === 'width-changed');
  for (const compare of ['wire', 'contract'] as const) {
    assert.deepEqual(psszEvolvesFrom(V1, optionalField, { compare }), { ok: true });
    const refused = psszEvolvesFrom(V1, required, { compare });
    assert.ok(!refused.ok && refused.code === 'appended-field-not-optional' && refused.path === '(root)');
    const variable = psszEvolvesFrom(V1, record('R', { id: u32, tags: list(u8) }, { maxDynamicData: 1024 }), { compare });
    assert.ok(!variable.ok && variable.code === 'appended-field-not-optional');
  }
  // An old writer's value is a new reader's value with the field absent;
  // a new writer's value with it present is, to the old reader, unknown.
  assert.deepEqual(optionalField.decode(V1.encode({ id: 7 })), { id: 7, note: null });
  const grown = optionalField.encode({ id: 7, note: 'n' });
  assert.deepEqual(V1.validate(grown), { ok: true, hasUnknown: true });
  assert.deepEqual(V1.decode(grown), { id: 7 });
  rejects(() => V1.decode(grown, { rejectUnknown: true }), 'bytes');
  // The required field's reader refuses the old bytes.
  assert.equal(required.validate(V1.encode({ id: 7 })).ok, false);
});
