// Schema fidelity and custom text forms through the Schema IR (docs/json.md
// "Width fidelity", "Raw strings and UTF-8 strings", "Explicit-width
// profiles", "Custom types through a schema"): boxes, raw vs UTF-8 strings,
// caps under an explicit width, and the custom handler registry.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { PsszError, PsszSchema, RecordView, type JsonFormat, type PsszCustomHandler, type PsszTypeInput } from '../index.js';
import { accountHandler, versionHandler } from './support/custom-handlers.js';

const hex = (bytes: Uint8Array) => Buffer.from(bytes).toString('hex');
const fromHex = (text: string) => Uint8Array.from(Buffer.from(text, 'hex'));
const bytesOf = (text: string) => Uint8Array.from(Buffer.from(text, 'utf8'));
const FORMATS: JsonFormat[] = ['json', 'ijson'];

const U8 = { Int: { bits: 8, isSigned: false } };
const U16 = { Int: { bits: 16, isSigned: false } };
const U32 = { Int: { bits: 32, isSigned: false } };
const U64 = { Int: { bits: 64, isSigned: false } };
const RAW = { Custom: { type: { List: U8 }, id: 'string' } };
const UTF8 = { Custom: { type: { List: U8 }, id: 'utf8' } };
const box = (type: PsszTypeInput): PsszTypeInput => ({ Custom: { type, id: 'box' } });
const cap = (n: number) => [{ name: 'maxDynamicData', value: String(n) }];

function rejects(run: () => unknown, kind: 'json' | 'value' | 'schema' | 'bytes', pattern: RegExp, what = ''): void {
  assert.throws(run, (e: unknown) => {
    assert.ok(e instanceof PsszError, `${what}: ${String(e)}`);
    assert.equal(e.kind, kind, `${what}: ${e.message}`);
    assert.match(e.message, pattern, what);
    return true;
  }, what);
}

// ── Boxes ───────────────────────────────────────────────────────────────────

const LEAF = { Object: { n: U8, tag: { Custom: { type: { BoundedList: { type: U8, maxCount: 4 } }, id: 'utf8' } } } };
const boxes = new PsszSchema({
  Leaf: LEAF,
  Direct: { Object: { id: U32, leaf: 'Leaf' } },
  Boxed: { Object: { id: U32, leaf: box('Leaf') } },
  BoxedCap200: { Object: { members: [{ name: 'id', type: U32 }, { name: 'leaf', type: box('Leaf') }], attributes: cap(200) } },
  BoxedCap60000: { Object: { members: [{ name: 'id', type: U32 }, { name: 'leaf', type: box('Leaf') }], attributes: cap(60000) } },
  BoxedCap70000: { Object: { members: [{ name: 'id', type: U32 }, { name: 'leaf', type: box('Leaf') }], attributes: cap(70000) } },
  BoxedLeaf: box('Leaf'),
  OptBoxed: { Object: { id: U32, leaf: { Option: box('Leaf') }, tail: U8 } },
  ListBoxed: { Object: { id: U8, leaves: { BoundedList: { type: box('Leaf'), maxCount: 3 } } } },
  ListDirect: { Object: { id: U8, leaves: { BoundedList: { type: 'Leaf', maxCount: 3 } } } },
  FixedBoxed: { Object: { id: U8, word: box(U32) } },
  FixedDirect: { Object: { id: U8, word: U32 } },
  Tree: { Object: { value: U8, children: { List: box('Tree') } } },
  Chain: { Object: { value: U8, next: { Option: box('Chain') } } },
  BoxOfBox: box(box(U16)),
});

test('box: no inferred size bound, so a container of one derives 4-byte offsets', () => {
  const width = (id: string) => boxes.type(id).width;
  // The same record: without the box 1, with it 4, with a cap the cap's width.
  assert.equal(width('Direct'), 1);
  assert.equal(width('Boxed'), 4);
  assert.equal(width('BoxedCap200'), 1);
  assert.equal(width('BoxedCap60000'), 2);
  assert.equal(width('BoxedCap70000'), 4);
  assert.equal(width('OptBoxed'), 4);
  assert.equal(width('ListDirect'), 1);
  assert.equal(width('ListBoxed'), 4);
  assert.equal(width('FixedDirect'), 1);
  assert.equal(width('FixedBoxed'), 4);
  // The boxed type itself keeps its own width; as a root a box reports 4.
  assert.equal(width('Leaf'), 1);
  assert.equal(width('BoxedLeaf'), 4);
  assert.equal(boxes.type({ Option: box('Leaf') }).width, 4);
  assert.equal(boxes.type({ Option: 'Leaf' }).width, 1);
  // An explicit profile has one inherited width, box or not.
  for (const w of [1, 2, 4] as const) {
    assert.equal(boxes.type('Boxed', { width: w }).width, w);
    assert.equal(boxes.type('BoxedLeaf', { width: w }).width, w);
  }
});

test('box: the bytes and the text are the inner type\'s', () => {
  const leaf = { n: 7, tag: 'abcd' };
  // A box has no header of its own.
  assert.equal(hex(boxes.type('BoxedLeaf').encode(leaf)), hex(boxes.type('Leaf').encode(leaf)));
  assert.deepEqual(boxes.type('BoxedLeaf').decode(boxes.type('Leaf').encode(leaf)), leaf);
  assert.equal(hex(boxes.type('BoxOfBox').encode(513)), '0102');
  assert.equal(boxes.type('BoxOfBox').decode(fromHex('0102')), 513);
  // The record differs from the unboxed one only in its offset width.
  const value = { id: 1, leaf };
  assert.equal(hex(boxes.type('Direct').encode(value)), '05' + '01000000' + '05' + '02' + '07' + '02' + '61626364');
  assert.equal(
    hex(boxes.type('Boxed').encode(value)),
    '08000000' + '01000000' + '08000000' + '02' + '07' + '02' + '61626364',
  );
  assert.deepEqual(boxes.type('Boxed').decode(boxes.type('Boxed').encode(value)), value);
  // Under one explicit width the two records are byte-identical.
  for (const w of [1, 2, 4] as const)
    assert.equal(hex(boxes.type('Boxed', { width: w }).encode(value)), hex(boxes.type('Direct', { width: w }).encode(value)));
  for (const format of FORMATS) {
    const text = '{"id":1,"leaf":{"n":7,"tag":"abcd"}}';
    assert.equal(boxes.type('Boxed').valueToJson(value, format), text);
    assert.deepEqual(boxes.type('Boxed').jsonToValue(text, format), value);
    assert.equal(boxes.type('BoxedLeaf').valueToJson(leaf, format), '{"n":7,"tag":"abcd"}');
    assert.equal(boxes.type('BoxOfBox').jsonToValue('513', format), 513);
    // A boxed optional member may be left out of the text.
    assert.deepEqual(boxes.type('OptBoxed').jsonToValue('{"id":1,"tail":2}', format), { id: 1, leaf: null, tail: 2 });
    assert.equal(boxes.type('OptBoxed').valueToJson({ id: 1, leaf, tail: 2 }, format), '{"id":1,"leaf":{"n":7,"tag":"abcd"},"tail":2}');
  }
  // Validation is the inner type's.
  rejects(() => boxes.type('BoxedLeaf').encodeChecked({ n: 7, tag: 'abcde' }), 'value', /bound/);
  assert.equal(boxes.type('BoxOfBox').validate(fromHex('010203')).ok, false);
});

test('box: recursive types through a box', () => {
  const tree = boxes.type('Tree');
  const value = { value: 1, children: [{ value: 2, children: [] }, { value: 3, children: [{ value: 4, children: [] }] }] };
  assert.equal(tree.width, 4);
  assert.deepEqual(tree.decode(tree.encode(value)), value);
  for (const format of FORMATS) assert.deepEqual(tree.jsonToValue(tree.valueToJson(value, format), format), value);
  const chain = boxes.type('Chain');
  const links = { value: 1, next: { value: 2, next: { value: 3, next: null } } };
  assert.equal(chain.width, 4);
  assert.deepEqual(chain.decode(chain.encode(links)), links);
  assert.equal(chain.valueToJson(links), '{"value":1,"next":{"value":2,"next":{"value":3,"next":null}}}');
  // A box around nothing but itself is not a type.
  rejects(() => new PsszSchema({ T: box('T') }).type('T').encodeChecked(1), 'schema', /cyclic/);
});

// ── Raw strings and UTF-8 strings ───────────────────────────────────────────

const strings = new PsszSchema({
  Raw: RAW,
  Utf8: UTF8,
  Raw4: { Custom: { type: { BoundedList: { type: U8, maxCount: 4 } }, id: 'string' } },
  Raw2x: { Custom: { type: { Array: { type: U8, len: 2 } }, id: 'string' } },
  Utf84: { Custom: { type: { BoundedList: { type: U8, maxCount: 4 } }, id: 'utf8' } },
  Message: {
    Object: {
      members: [
        { name: 'raw', type: RAW, attributes: [{ name: 'lengthBound', value: 'max=3' }] },
        { name: 'text', type: UTF8 },
        { name: 'maybe', type: { Option: RAW } },
      ],
    },
  },
});

test('raw string: the value is its bytes; any bytes are valid PSSZ', () => {
  const raw = strings.type('Raw');
  // Decoded: a Uint8Array (a JavaScript string cannot hold every raw string).
  const decoded = raw.decode(bytesOf('héllo'));
  assert.ok(decoded instanceof Uint8Array);
  assert.equal(hex(decoded), hex(bytesOf('héllo')));
  // Encoded from bytes, or from a JavaScript string as its UTF-8.
  assert.equal(hex(raw.encode('héllo')), hex(bytesOf('héllo')));
  assert.equal(hex(raw.encode(bytesOf('héllo'))), hex(bytesOf('héllo')));
  assert.equal(hex(raw.encode('')), '');
  // The decoded value is a copy.
  const input = bytesOf('abc');
  const copy = raw.decode(input) as Uint8Array;
  input[0] = 0;
  assert.equal(hex(copy), '616263');
  // Bytes that are not UTF-8: valid, decoded, re-encoded unchanged.
  for (const bad of ['ff', 'c080', 'eda080', 'f4908080', '61ff62', '80', 'c3']) {
    const bytes = fromHex(bad);
    assert.deepEqual(raw.validate(bytes), { ok: true, hasUnknown: false }, bad);
    const value = raw.decode(bytes);
    assert.ok(value instanceof Uint8Array);
    assert.equal(hex(raw.encode(value)), bad);
    // ... and without text, in both formats.
    for (const format of FORMATS) {
      rejects(() => raw.psszToJson(bytes, format), 'value', /string is not valid UTF-8/, bad);
      rejects(() => raw.valueToJsonChecked(value, format), 'value', /string is not valid UTF-8/, bad);
    }
  }
  rejects(() => raw.encodeChecked(7), 'value', /raw string value/);
  rejects(() => raw.encodeChecked([1, 2]), 'value', /raw string value/);
  rejects(() => raw.encodeChecked('\ud800'), 'value', /not valid UTF-8/);
});

test('raw string: text is the UTF-8 it holds; bounds count bytes', () => {
  const raw = strings.type('Raw');
  for (const format of FORMATS) {
    assert.equal(raw.psszToJson(bytesOf('héllo "q"'), format), '"héllo \\"q\\""');
    assert.equal(raw.valueToJson('héllo', format), '"héllo"');
    assert.equal(raw.valueToJson(bytesOf('héllo'), format), '"héllo"');
    const value = raw.jsonToValue('"h\\u00e9llo"', format);
    assert.ok(value instanceof Uint8Array);
    assert.equal(hex(value), hex(bytesOf('héllo')));
    assert.equal(hex(raw.jsonToPssz('"héllo"', format)), hex(bytesOf('héllo')));
    rejects(() => raw.jsonToValue('7', format), 'json', /expected a string/);
    rejects(() => raw.valueToJsonChecked(7, format), 'value', /string value required/);

    const raw4 = strings.type('Raw4');
    assert.equal(hex(raw4.jsonToPssz('"abcd"', format)), '61626364');
    rejects(() => raw4.jsonToPssz('"abcde"', format), 'json', /exceeds its bound/);
    rejects(() => raw4.jsonToPssz('"ééé"', format), 'json', /exceeds its bound/);
    rejects(() => raw4.valueToJsonChecked(bytesOf('abcde'), format), 'value', /exceeds its bound/);
    rejects(() => raw4.valueToJsonChecked('abcde', format), 'value', /exceeds its bound/);
  }
  // I-JSON refuses a noncharacter; JSON carries it.
  assert.equal(raw.psszToJson(bytesOf('￾'), 'json'), '"￾"');
  rejects(() => raw.psszToJson(bytesOf('￾'), 'ijson'), 'value', /noncharacter/);

  const raw4 = strings.type('Raw4');
  assert.equal(hex(raw4.encode(fromHex('ffffffff'))), 'ffffffff');
  rejects(() => raw4.encodeChecked(fromHex('ffffffffff')), 'value', /bound exceeded/);
  rejects(() => raw4.encodeChecked('ééé'), 'value', /bound exceeded/);
  assert.equal(raw4.validate(fromHex('ffffffffff')).ok, false);
  const raw2 = strings.type('Raw2x');
  assert.equal(hex(raw2.decode(fromHex('ff00')) as Uint8Array), 'ff00');
  rejects(() => raw2.encodeChecked('a'), 'value', /count mismatch/);
});

test('utf8 string: a JavaScript string, validated on every read and write', () => {
  const utf8 = strings.type('Utf8');
  assert.equal(utf8.decode(bytesOf('héllo')), 'héllo');
  assert.equal(hex(utf8.encode('héllo')), hex(bytesOf('héllo')));
  for (const bad of ['ff', 'c080', 'eda080', 'f4908080', '61ff62', '80', 'c3']) {
    const bytes = fromHex(bad);
    const result = utf8.validate(bytes);
    assert.ok(!result.ok && result.error.kind === 'bytes' && /not valid UTF-8/.test(result.error.message), bad);
    rejects(() => utf8.decode(bytes), 'bytes', /not valid UTF-8/, bad);
    for (const format of FORMATS) rejects(() => utf8.psszToJson(bytes, format), 'bytes', /not valid UTF-8/, bad);
  }
  // Bytes are not a UTF-8 string value.
  rejects(() => utf8.encode(bytesOf('abc')), 'value', /string value required/);
  rejects(() => utf8.valueToJson(bytesOf('abc')), 'value', /string value required/);
  rejects(() => utf8.encode('\udc00'), 'value', /not valid UTF-8/);
  rejects(() => strings.type('Utf84').encodeChecked('abcde'), 'value', /bound exceeded/);
  for (const format of FORMATS) assert.equal(utf8.jsonToValue('"héllo"', format), 'héllo');
});

test('raw and utf8 strings as record members', () => {
  const message = strings.type('Message');
  const value = { raw: fromHex('ff00ff'), text: 'ok', maybe: fromHex('c080') };
  const bytes = message.encode(value);
  assert.deepEqual(message.decode(bytes), value);
  assert.equal(message.validate(bytes).ok, true);
  // A string value is accepted for the raw members and decodes to bytes.
  assert.deepEqual(
    message.decode(message.encode({ raw: 'abc', text: 'ok', maybe: 'é' })),
    { raw: bytesOf('abc'), text: 'ok', maybe: bytesOf('é') },
  );
  // The member length bound counts the raw string's bytes.
  rejects(() => message.encodeChecked({ raw: fromHex('ff00ff00'), text: '', maybe: null }), 'value', /length bound/);
  rejects(() => message.encodeChecked({ raw: 'éé', text: '', maybe: null }), 'value', /length bound/);
  for (const format of FORMATS) {
    // One member without text makes the whole value textless.
    rejects(() => message.psszToJson(bytes, format), 'value', /string is not valid UTF-8/);
    const text = '{"raw":"abc","text":"ok","maybe":null}';
    assert.equal(message.valueToJson({ raw: 'abc', text: 'ok', maybe: null }, format), text);
    assert.deepEqual(message.jsonToValue(text, format), { raw: bytesOf('abc'), text: 'ok', maybe: null });
    rejects(() => message.jsonToValue('{"raw":"abcd","text":"ok"}', format), 'json', /length_bound/);
  }
});

// ── Explicit-width profiles and caps ────────────────────────────────────────

const caps = new PsszSchema({
  Inner: { Object: { members: [{ name: 'data', type: { Custom: { type: { List: U8 }, id: 'hex' } } }], attributes: cap(16) } },
  Outer: { Object: { members: [{ name: 'inner', type: 'Inner' }, { name: 'more', type: { List: 'Inner' } }], attributes: cap(64) } },
  Uncapped: { Object: { inner: 'Inner' } },
  Counted: { Object: { members: [{ name: 'a', type: U8 }], attributes: [{ name: 'maxFields', value: '4' }] } },
  HoldsCounted: { Object: { c: 'Counted' } },
});

test('explicit width: the top-level cap bounds the encoding; nested caps are not looked up', () => {
  const small = { data: new Uint8Array(2) };
  const big = { data: new Uint8Array(20) }; // over Inner's cap of 16
  const huge = { data: new Uint8Array(60) };
  for (const w of [1, 2, 4] as const) {
    const inner = caps.type('Inner', { width: w });
    const outer = caps.type('Outer', { width: w });
    // The type is accepted and keeps the one inherited width.
    assert.equal(inner.width, w);
    assert.equal(outer.width, w);
    // Top level: enforced on encode, decode and validate.
    assert.equal(inner.decode(inner.encode(small)) !== null, true);
    rejects(() => inner.encodeChecked(big), 'value', /exceeds maxDynamicData cap/, `w${w}`);
    const overInner = caps.type('Uncapped', { width: w }).encode({ inner: big });
    const innerBytes = overInner.subarray(2 * w); // header, one offset slot, then Inner
    assert.equal(hex(innerBytes), hex(caps.type({ Object: { data: { Custom: { type: { List: U8 }, id: 'hex' } } } }, { width: w }).encode(big)));
    rejects(() => inner.decode(innerBytes), 'bytes', /exceeds maxDynamicData cap/, `w${w}`);
    const validation = inner.validate(innerBytes);
    assert.ok(!validation.ok && /exceeds maxDynamicData cap/.test(validation.error.message));
    rejects(() => inner.psszToJson(innerBytes), 'bytes', /exceeds maxDynamicData cap/);
    rejects(() => inner.jsonToPssz(`{"data":"${'00'.repeat(20)}"}`), 'value', /exceeds maxDynamicData cap/);
    // Nested: Inner over its cap inside Outer is fine while Outer fits its own.
    const nested = { inner: big, more: [] };
    const bytes = outer.encode(nested);
    assert.ok(bytes.length <= 64);
    assert.deepEqual(outer.decode(bytes), nested);
    assert.deepEqual(outer.validate(bytes), { ok: true, hasUnknown: false });
    assert.deepEqual(caps.type('Uncapped', { width: w }).decode(overInner), { inner: big });
    // ... and Outer's own cap still bounds the whole.
    rejects(() => outer.encodeChecked({ inner: huge, more: [] }), 'value', /exceeds maxDynamicData cap/);
    rejects(() => outer.encodeChecked({ inner: small, more: [big, big, big] }), 'value', /exceeds maxDynamicData cap/);
    // A cap is the top-level type's only when the root is that record.
    const optional = caps.type({ Option: 'Inner' }, { width: w });
    assert.deepEqual(optional.decode(optional.encode(big)), big);
    const list = caps.type({ List: 'Inner' }, { width: w });
    assert.deepEqual(list.decode(list.encode([big, big])), [big, big]);
  }
  // The automatic profile derives widths from caps and enforces every record's.
  assert.equal(caps.type('Inner').width, 1);
  assert.equal(caps.type('Outer').width, 1);
  rejects(() => caps.type('Outer').encodeChecked({ inner: big, more: [] }), 'value', /exceeds maxDynamicData cap/);
  rejects(() => caps.type('Uncapped').encodeChecked({ inner: big }), 'value', /exceeds maxDynamicData cap/);
  rejects(() => caps.type({ Option: 'Inner' }).encodeChecked(big), 'value', /exceeds maxDynamicData cap/);
  assert.deepEqual(caps.type('Outer').decode(caps.type('Outer').encode({ inner: small, more: [small] })), { inner: small, more: [small] });
});

test('explicit width: maxFields is an unsupported constraint there, as in the typed codecs', () => {
  // The automatic profile takes the attribute.
  assert.deepEqual(caps.type('Counted').decode(caps.type('Counted').encode({ a: 1 })), { a: 1 });
  assert.deepEqual(caps.type('HoldsCounted').decode(caps.type('HoldsCounted').encode({ c: { a: 1 } })), { c: { a: 1 } });
  for (const w of [1, 2, 4] as const) {
    rejects(() => caps.type('Counted', { width: w }), 'schema', /unsupported schema constraint/, `w${w}`);
    rejects(() => caps.type('HoldsCounted', { width: w }), 'schema', /unsupported schema constraint/, `w${w}`);
    // maxDynamicData is accepted there.
    assert.equal(caps.type('Inner', { width: w }).width, w);
  }
});

// ── Custom text handlers ────────────────────────────────────────────────────

const VERSION = { Custom: { type: { Object: { major: U16, minor: U16 } }, id: 'Version' } };
const ACCOUNT = { Custom: { type: U64, id: 'AccountName' } };
const customIr = {
  Version: VERSION,
  Account: ACCOUNT,
  Plain: { Object: { major: U16, minor: U16 } },
  Transfer: {
    Object: {
      members: [
        { name: 'from', type: 'Account' },
        { name: 'to', type: 'Account', attributes: [{ name: 'ijsonInt', value: 'number' }] },
        { name: 'notify', type: { List: 'Account' } },
        { name: 'since', type: { Option: 'Version' } },
        { name: 'either', type: { Variant: { Who: 'Account', Which: 'Version' } } },
        { name: 'pair', type: { Tuple: ['Account', 'Version'] } },
      ],
    },
  },
  Odd: { Custom: { type: U8, id: 'Odd' } },
  Nested: { Custom: { type: { Custom: { type: U64, id: 'AccountName' } }, id: 'Outer' } },
  BoxedAccount: { Custom: { type: 'Account', id: 'box' } },
};
const handlers = { Version: versionHandler, AccountName: accountHandler };
const plain = new PsszSchema(customIr);
const custom = new PsszSchema(customIr, { handlers });
const strict = new PsszSchema(customIr, { requireHandlers: true });

const ALICE = 235814801429757952n;
const BOB = 274965867874222080n;
const transfer = {
  from: ALICE, to: BOB, notify: [ALICE, 0n], since: { major: 1, minor: 2 },
  either: { type: 'Which', value: { major: 3, minor: 4 } }, pair: [BOB, { major: 0, minor: 0 }],
};

test('custom type: on the wire exactly its inner type, whatever is registered', () => {
  for (const schema of [plain, custom, strict.withHandlers(handlers, { requireHandlers: false }), custom.withHandlers({}, { requireHandlers: true })]) {
    // Width, bytes, validation and the decoded value are the inner type's.
    assert.equal(schema.type('Version').width, plain.type('Plain').width);
    assert.equal(hex(schema.type('Version').encode({ major: 12, minor: 345 })), '040c005901');
    assert.deepEqual(schema.type('Version').decode(fromHex('040c005901')), { major: 12, minor: 345 });
    assert.equal(hex(schema.type('Account').encode(ALICE)), '0000000050c84503');
    assert.equal(schema.type('Account').decode(fromHex('0000000050c84503')), ALICE);
    assert.equal(schema.type('Account').validate(fromHex('00')).ok, false);
    assert.deepEqual(schema.type('Transfer').decode(schema.type('Transfer').encode(transfer)), transfer);
    assert.equal(hex(schema.type('Transfer').encode(transfer)), hex(plain.type('Transfer').encode(transfer)));
    for (const w of [1, 2, 4] as const)
      assert.equal(hex(schema.type('Version', { width: w }).encode({ major: 1, minor: 2 })), hex(plain.type('Plain', { width: w }).encode({ major: 1, minor: 2 })));
  }
  // "unknown" marks a type the schema builder could not describe: no codec.
  rejects(() => new PsszSchema({ T: { Custom: { type: U8, id: 'unknown' } } }).type('T'), 'schema', /unsupported custom adapter/);
});

test('custom type: a registered handler carries the text as one JSON string', () => {
  const version = custom.type('Version');
  const account = custom.type('Account');
  for (const format of FORMATS) {
    assert.equal(version.valueToJson({ major: 12, minor: 345 }, format), '"12.345"');
    assert.deepEqual(version.jsonToValue('"12.345"', format), { major: 12, minor: 345 });
    assert.equal(version.psszToJson(fromHex('040c005901'), format), '"12.345"');
    assert.equal(hex(version.jsonToPssz('"12.345"', format)), '040c005901');
    // The same string in both formats, small or large.
    assert.equal(account.valueToJson(ALICE, format), '"alice"');
    assert.equal(account.valueToJson(0n, format), '""');
    assert.equal(account.jsonToValue('"alice"', format), ALICE);
    assert.equal(account.jsonToValue('"\\u0061lice"', format), ALICE);
    assert.equal(hex(account.jsonToPssz('"bob.token"', format)), '0080a920cde0d003');
    // A number value is taken as the inner type takes it; the handler sees a bigint.
    assert.equal(account.valueToJson(0, format), '""');

    // Through members, lists, optionals, variants and tuples.
    const text =
      '{"from":"alice","to":"bob","notify":["alice",""],"since":"1.2",' +
      (format === 'ijson' ? '"either":{"Which":"3.4"}' : '"either":[1,"3.4"]') +
      ',"pair":["bob","0.0"]}';
    assert.equal(custom.type('Transfer').valueToJson(transfer, format), text);
    assert.deepEqual(custom.type('Transfer').jsonToValue(text, format), transfer);
    assert.equal(hex(custom.type('Transfer').jsonToPssz(text, format)), hex(plain.type('Transfer').encode(transfer)));
    // A box around a custom type is still presented.
    assert.equal(custom.type('BoxedAccount').valueToJson(ALICE, format), '"alice"');
    assert.equal(custom.type('BoxedAccount').jsonToValue('"alice"', format), ALICE);
    // The outermost registered presentation wins; an unregistered one is transparent.
    assert.equal(custom.type('Nested').valueToJson(ALICE, format), '"alice"');
    assert.equal(custom.type('Nested').jsonToValue('"alice"', format), ALICE);

    // With a handler the plain form is NOT also accepted, nor any other kind.
    rejects(() => account.jsonToValue('7', format), 'json', /expected a string \(custom text form\)/);
    rejects(() => account.jsonToValue('"235814801429757952"', format), 'json', /not a value of the custom type/);
    rejects(() => version.jsonToValue('{"major":1,"minor":2}', format), 'json', /expected a string \(custom text form\)/);
    for (const bad of ['null', 'true', '[]', '1.2'])
      rejects(() => version.jsonToValue(bad, format), 'json', /expected a string \(custom text form\)/, bad);
    // The handler refuses text that is not its presentation.
    for (const bad of ['"1"', '"1.65536"', '"1.2.3"', '"01.2"', '"x"', '""', '"1."'])
      rejects(() => version.jsonToValue(bad, format), 'json', /not a value of the custom type/, bad);
    for (const bad of ['"Alice"', '"aaaaaaaaaaaaa"', '"alice."', '"a6"'])
      rejects(() => account.jsonToValue(bad, format), 'json', /not a value of the custom type/, bad);
    rejects(
      () => custom.type('Transfer').jsonToValue(text.replace('"from":"alice"', '"from":1'), format),
      'json', /expected a string \(custom text form\)/,
    );
    // The value must be one of the inner type.
    rejects(() => account.valueToJsonChecked('alice', format), 'value', /does not match its inner type/);
    rejects(() => version.valueToJsonChecked({ major: 70000, minor: 0 }, format), 'value', /does not match its inner type/);
  }
});

test('custom type: with no handler it reads and writes as its wire type', () => {
  for (const format of FORMATS) {
    assert.equal(plain.type('Version').valueToJson({ major: 12, minor: 345 }, format), '{"major":12,"minor":345}');
    assert.deepEqual(plain.type('Version').jsonToValue('{"major":12,"minor":345}', format), { major: 12, minor: 345 });
    // The wire type's own integer rule.
    const big = format === 'ijson' ? `"${ALICE}"` : `${ALICE}`;
    assert.equal(plain.type('Account').valueToJson(ALICE, format), big);
    assert.equal(plain.type('Account').jsonToValue(big, format), ALICE);
    assert.equal(plain.type('Account').valueToJson(7n, format), '7');
    // ... and the member's integer form reaches it.
    const text = plain.type('Transfer').valueToJson(transfer, format);
    assert.ok(text.includes(`"to":${BOB},`), text);
    assert.deepEqual(plain.type('Transfer').jsonToValue(text, format), transfer);
    assert.equal(hex(plain.type('Transfer').jsonToPssz(text, format)), hex(custom.type('Transfer').encode(transfer)));
    // The presentation is not read without its handler.
    rejects(() => plain.type('Version').jsonToValue('"12.345"', format), 'json', /expected '\{'/);
    rejects(() => plain.type('Account').jsonToValue('"alice"', format), 'json', /expected a number|canonical in-range/);
    // An id nobody registered falls back the same way next to registered ones.
    assert.equal(custom.type('Odd').valueToJson(9, format), '9');
    assert.equal(custom.type('Odd').jsonToValue('9', format), 9);
  }
});

test('custom type: requireHandlers makes a missing handler an error on every entry point', () => {
  for (const format of FORMATS) {
    rejects(() => strict.type('Version').valueToJsonChecked({ major: 1, minor: 2 }, format), 'value', /no handler registered for a custom type/);
    // (The bytes are refused before any text is written.)
    rejects(() => strict.type('Version').psszToJson(fromHex('0401000200'), format), 'bytes', /no handler registered for a custom type/);
    rejects(() => strict.type('Version').jsonToValue('"1.2"', format), 'json', /no handler registered for a custom type/);
    rejects(() => strict.type('Version').jsonToPssz('{"major":1,"minor":2}', format), 'json', /no handler registered for a custom type/);
    rejects(() => strict.type('Transfer').valueToJsonChecked(transfer, format), 'value', /no handler registered/);
    // A type with no custom member, and a box, are unaffected.
    assert.equal(strict.type('Plain').valueToJson({ major: 1, minor: 2 }, format), '{"major":1,"minor":2}');
    assert.equal(strict.type({ Custom: { type: U8, id: 'box' } }).valueToJson(7, format), '7');
    // With every id registered, strict is satisfied; one missing is not.
    const all = strict.withHandlers(handlers);
    assert.equal(all.type('Version').valueToJson({ major: 1, minor: 2 }, format), '"1.2"');
    assert.equal(all.type('Account').valueToJson(ALICE, format), '"alice"');
    rejects(() => all.type('Odd').valueToJsonChecked(1, format), 'value', /no handler registered/);
    rejects(() => all.type('Nested').valueToJsonChecked(ALICE, format), 'value', /no handler registered/);
  }
  // A strict registry applies on the binary entry points as well: a caller
  // that must not accept or produce a value it cannot check and present.
  for (const width of ['auto', 1, 2, 4] as const) {
    const version = strict.type('Version', { width });
    const bytes = plain.type('Version', { width }).encode({ major: 1, minor: 2 });
    rejects(() => version.encodeChecked({ major: 1, minor: 2 }), 'value', /no handler registered for a custom type/);
    rejects(() => version.decode(bytes), 'bytes', /no handler registered for a custom type/);
    const validated = version.validate(bytes);
    assert.ok(!validated.ok && validated.error.kind === 'bytes' && !validated.error.invariant);
    rejects(() => (version.view(bytes) as RecordView).toValue(), 'bytes', /no handler registered/);
    // A view reads in place and consults no handler.
    assert.equal((version.view(bytes) as RecordView).get('major'), 1);
  }
  rejects(() => strict.type('Transfer').encodeChecked(transfer), 'value', /no handler registered/);
  rejects(() => strict.type('Account').encodeKeyChecked(ALICE), 'value', /no handler registered/);
  rejects(() => strict.type('Account').decodeKey(plain.type('Account').encodeKeyChecked(ALICE)), 'bytes', /no handler registered/);
  // A type with no custom member, a box and the built-in ids are unaffected.
  assert.equal(hex(strict.type('Plain').encode({ major: 1, minor: 2 })), '0401000200');
  assert.equal(hex(strict.type({ Custom: { type: U8, id: 'box' } }).encode(7)), '07');
  // With every id registered the binary side is exact.
  const all = strict.withHandlers({ ...handlers, Odd: {}, Outer: {} });
  assert.equal(hex(all.type('Version').encode({ major: 1, minor: 2 })), '0401000200');
  assert.deepEqual(all.type('Version').decode(fromHex('0401000200')), { major: 1, minor: 2 });
  assert.equal(hex(all.type('Transfer').encode(transfer)), hex(plain.type('Transfer').encode(transfer)));
});

test('custom handlers: the registry (schema, withHandlers, per type); bound once per type', () => {
  let lookups = 0;
  const counting = new Map<string, PsszCustomHandler>([['Version', versionHandler], ['AccountName', accountHandler]]);
  const registry = { ...handlers };
  const schema = new PsszSchema(customIr, { handlers: registry });
  const version = schema.type('Version');
  // The handler is resolved when the type is compiled: changing the registry
  // object afterwards changes nothing, for this type or a later one.
  registry.Version = { toText: () => { ++lookups; return 'changed'; }, fromText: () => ({ major: 9, minor: 9 }) };
  assert.equal(version.valueToJson({ major: 1, minor: 2 }), '"1.2"');
  assert.equal(schema.type('Version').valueToJson({ major: 1, minor: 2 }), '"1.2"');
  assert.equal(lookups, 0);
  // A Map works as a registry; so do PsszSchema.decode / fromJson options.
  const viaMap = PsszSchema.decode(plain.encode(), { handlers: counting });
  assert.equal(viaMap.type('Account').valueToJson(ALICE), '"alice"');
  const viaJson = PsszSchema.fromJson(PsszSchema.meta().type('Schema').valueToJson(plain.ir), 'ijson', { handlers });
  assert.equal(viaJson.type('Version').valueToJson({ major: 1, minor: 2 }), '"1.2"');
  // The custom ids travel with the schema.
  assert.deepEqual(viaMap.ir, plain.ir);

  // withHandlers: a new schema; the original is unchanged. Ids are replaced one by one.
  const upper: PsszCustomHandler = {
    toText: (v) => `V${(v as { major: number }).major}`,
    fromText: (t) => ({ major: Number(t.slice(1)), minor: 0 }),
  };
  const replaced = custom.withHandlers({ Version: upper });
  assert.equal(replaced.type('Version').valueToJson({ major: 5, minor: 0 }), '"V5"');
  assert.deepEqual(replaced.type('Version').jsonToValue('"V5"'), { major: 5, minor: 0 });
  assert.equal(replaced.type('Account').valueToJson(ALICE), '"alice"');
  assert.equal(custom.type('Version').valueToJson({ major: 5, minor: 0 }), '"5.0"');
  assert.equal(plain.withHandlers(handlers, { requireHandlers: true }).type('Version').valueToJson({ major: 5, minor: 0 }), '"5.0"');

  // Per type: handlers for this compilation only, with any width.
  const one = plain.type('Version', { handlers: { Version: versionHandler } });
  assert.equal(one.valueToJson({ major: 1, minor: 2 }), '"1.2"');
  assert.equal(plain.type('Version').valueToJson({ major: 1, minor: 2 }), '{"major":1,"minor":2}');
  const wide = plain.type('Version', { width: 4, handlers });
  assert.equal(wide.width, 4);
  assert.equal(hex(wide.jsonToPssz('"1.2"')), '0400000001000200');
  assert.equal(wide.psszToJson(fromHex('0400000001000200')), '"1.2"');
  rejects(() => custom.type('Odd', { requireHandlers: true }).valueToJsonChecked(1), 'value', /no handler registered/);
  assert.equal(custom.type('Version', { requireHandlers: true }).valueToJson({ major: 1, minor: 2 }), '"1.2"');

  // Ids that are part of the binary form take no handler.
  for (const id of ['bool', 'char', 'string', 'utf8', 'hex', 'bitvector', 'bitlist', 'box', 'unknown'])
    rejects(() => new PsszSchema(customIr, { handlers: { [id]: versionHandler } }), 'schema', /part of the binary form/, id);
  rejects(() => plain.withHandlers({ box: versionHandler }), 'schema', /part of the binary form/);
  for (const id of ['map', 'set'])
    rejects(() => new PsszSchema(customIr, { handlers: { [id]: versionHandler } }), 'schema', /part of the binary form/, id);
  // Every member of a handler is optional; one that is present is a function.
  for (const member of ['toText', 'fromText', 'check'])
    rejects(() => new PsszSchema(customIr, { handlers: { Version: { [member]: 1 } as unknown as PsszCustomHandler } }),
      'schema', /must be a function/, member);
  rejects(() => new PsszSchema(customIr, { handlers: { Version: null as unknown as PsszCustomHandler } }), 'schema', /must be an object/);
  // A handler with nothing registers the id (a strict registry is satisfied): the type is its wire type.
  const known = new PsszSchema(customIr, { handlers: { Version: {} }, requireHandlers: true });
  assert.equal(known.type('Version').valueToJson({ major: 1, minor: 2 }), '{"major":1,"minor":2}');
  assert.equal(hex(known.type('Version').encode({ major: 1, minor: 2 })), '0401000200');
});

test('custom handlers: a handler that throws or misbehaves is an ordinary error', () => {
  const calls: unknown[] = [];
  const schema = new PsszSchema(customIr, {
    handlers: {
      Version: {
        toText(value) {
          calls.push(value);
          if ((value as { major: number }).major === 13) throw new RangeError('unlucky');
          if ((value as { major: number }).major === 14) return 14 as unknown as string;
          return 'v';
        },
        fromText(text) {
          if (text === 'throw') throw new TypeError('no');
          if (text === 'string') return 'not a record';
          if (text === 'range') return { major: 70000, minor: 0 };
          if (text === 'missing') return { major: 1 };
          if (text === 'extra') return { major: 1, minor: 2, patch: 3 };
          return { major: 1, minor: 2 };
        },
      },
      // The handler gets, and may return, the decoded value of the wire type.
      AccountName: { toText: (v) => typeof v, fromText: () => 7 },
    },
  });
  const version = schema.type('Version');
  for (const format of FORMATS) {
    rejects(() => version.valueToJsonChecked({ major: 13, minor: 0 }, format), 'value', /custom handler has no text for the value/);
    rejects(() => version.valueToJsonChecked({ major: 14, minor: 0 }, format), 'value', /custom handler has no text for the value/);
    rejects(() => version.jsonToValue('"throw"', format), 'json', /not a value of the custom type/);
    for (const bad of ['string', 'range', 'missing', 'extra'])
      rejects(() => version.jsonToValue(`"${bad}"`, format), 'json', /produced a value that is not the inner type/, bad);
    assert.deepEqual(version.jsonToValue('"ok"', format), { major: 1, minor: 2 });
    assert.equal(schema.type('Account').valueToJson(5, format), '"bigint"');
    // A returned number is the inner type's value: a bigint once read.
    assert.equal(schema.type('Account').jsonToValue('"x"', format), 7n);
    assert.equal(hex(schema.type('Account').jsonToPssz('"x"', format)), '0700000000000000');
  }
  // One call per value written.
  calls.length = 0;
  schema.type({ List: 'Version' }).valueToJson([{ major: 1, minor: 0 }, { major: 2, minor: 0 }]);
  assert.deepEqual(calls, [{ major: 1, minor: 0 }, { major: 2, minor: 0 }]);
  // The text a handler writes is escaped, and I-JSON's string rule applies.
  const odd = new PsszSchema(customIr, {
    handlers: { Odd: { toText: (v) => (v === 1 ? 'a"b\\c\n' : '￾'), fromText: (t) => t.length } },
  }).type('Odd');
  assert.equal(odd.valueToJson(1, 'json'), '"a\\"b\\\\c\\n"');
  assert.equal(odd.jsonToValue('"a\\"b\\\\c\\n"', 'json'), 6);
  assert.equal(odd.valueToJson(2, 'json'), '"￾"');
  rejects(() => odd.valueToJsonChecked(2, 'ijson'), 'value', /noncharacter/);
});

test('non-finite float: the error names the format', () => {
  const f64 = new PsszSchema({ F: { Float: { exp: 11, mantissa: 52 } } }).type('F');
  for (const value of [NaN, Infinity, -Infinity]) {
    rejects(() => f64.valueToJsonChecked(value, 'json'), 'value', /^json: non-finite float has no JSON text$/);
    rejects(() => f64.valueToJsonChecked(value, 'ijson'), 'value', /^ijson: non-finite float has no JSON text$/);
  }
  rejects(() => f64.psszToJson(fromHex('000000000000f07f'), 'json'), 'value', /^json: non-finite/);
  rejects(() => f64.psszToJson(fromHex('010000000000f07f'), 'ijson'), 'value', /^ijson: non-finite/);
});

// C++ `std::vector<char>` now builds Custom{List<u8>, "hex"}, the node every
// byte list has (cpp/tests/schema_byte_list_tests.cpp). A published schema
// may still hold the former node, Custom{List<Custom{u8, "char"}>, "hex"}:
// it is read as the same bytes, with the same text, width, bounds and key.
test('byte lists: the former char-element node is still read as bytes', () => {
  const CHAR = { Custom: { type: U8, id: 'char' } };
  const hexOf = (element: PsszTypeInput, shape: 'List' | 'Array' = 'List'): PsszTypeInput =>
    ({ Custom: { type: shape === 'List' ? { List: element } : { Array: { type: element, len: 3 } }, id: 'hex' } });
  const bytes = Uint8Array.of(0, 128, 255);
  for (const shape of ['List', 'Array'] as const) {
    const schema = new PsszSchema({
      Former: hexOf(CHAR, shape), Now: hexOf(U8, shape),
      FormerRec: { Object: { id: U32, data: hexOf(CHAR, shape), tail: { Option: hexOf(CHAR, shape) } } },
      NowRec: { Object: { id: U32, data: hexOf(U8, shape), tail: { Option: hexOf(U8, shape) } } },
    });
    // The schema with the former node survives its own PSSZ encoding.
    const fetched = PsszSchema.decode(schema.encode());
    for (const source of [schema, fetched]) {
      const former = source.type('Former');
      const now = source.type('Now');
      assert.deepEqual(former.validate(bytes), { ok: true, hasUnknown: false });
      assert.deepEqual(former.decode(bytes), bytes);
      assert.deepEqual(former.decode(bytes), now.decode(bytes));
      assert.equal(hex(former.encode(bytes)), hex(now.encode(bytes)));
      for (const format of FORMATS) {
        const text = now.psszToJson(bytes, format);
        assert.equal(text, format === 'ijson' ? '"0080FF"' : '[0,128,255]');
        assert.equal(former.psszToJson(bytes, format), text);
        assert.equal(hex(former.jsonToPssz(text, format)), '0080ff');
      }
      assert.equal(hex(former.encodeKey(bytes)), hex(now.encodeKey(bytes)));
      assert.deepEqual([former.width, former.inferredBound, former.maxEncodedSize], [now.width, now.inferredBound, now.maxEncodedSize]);
      const value = { id: 7, data: bytes, tail: bytes };
      const formerRec = source.type('FormerRec');
      const nowRec = source.type('NowRec');
      const frame = nowRec.encode(value);
      assert.equal(hex(formerRec.encode(value)), hex(frame));
      assert.deepEqual(formerRec.decode(frame), value);
      assert.equal(formerRec.psszToJson(frame), nowRec.psszToJson(frame));
      assert.deepEqual([formerRec.width, formerRec.inferredBound, formerRec.maxEncodedSize], [nowRec.width, nowRec.inferredBound, nowRec.maxEncodedSize]);
    }
  }
});
