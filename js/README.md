# psio — PSIO serialization for JavaScript and TypeScript

The PSIO formats for browser and Node clients:

- **PSSZ**, the schema-driven binary format (`docs/pssz-spec.md`): encode,
  decode, validate, and read in place through views;
- **key**, the order-preserving binary format for database keys
  (`docs/key-spec.md`);
- **JSON and I-JSON**, the text forms of the same values (`docs/json.md`),
  exact for 64-bit and wider integers;
- **pjson**, the self-describing binary JSON (`docs/pjson-spec.md`);
- time values (`docs/time.md`), custom text forms, WIT text of a schema.

Types come from a **Schema IR fetched at run time** (no generated code) or
from **typed definitions written in TypeScript**, which produce the same
Schema IR the C++ and Rust schema builders produce. Both run one codec. The
bytes, the text and the accept / reject decisions are those of the C++ and
Rust implementations: the test suite replays every vector of
`conformance/pssz-vectors.json`, `json-vectors.json` and
`time-key-vectors.json` through both paths.

```bash
npm install psio
```

**Environments.** ES modules only, no runtime dependencies, no Node-specific
APIs, `"sideEffects": false` (tree-shakeable), type declarations included.
It needs ES2022 with `BigInt`, `TextEncoder` / `TextDecoder` and the
`DataView` 64-bit accessors: Node 20 or later, and current Chrome, Edge,
Firefox and Safari (15.4 or later). TypeScript 5 for the typed layer.

## Contents

- [From a schema fetched at run time](#from-a-schema-fetched-at-run-time)
- [Typed definitions](#typed-definitions)
- [Views](#views)
- [Keys](#keys)
- [JSON and I-JSON](#json-and-i-json)
- [Custom types](#custom-types)
- [What a blockchain client needs](#what-a-blockchain-client-needs)
- [Errors and untrusted input](#errors-and-untrusted-input)
- [Reference](#reference): values, text rules, widths, IR version, hand-written schemas
- [WIT, pjson, names, hex](#wit-pjson-names-hex)
- [Exports](#exports)
- [Performance](#performance)

## From a schema fetched at run time

A client that has a Schema IR (`psio::Schema`, normally fetched as its PSSZ
bytes) converts between JavaScript values, PSSZ bytes, JSON and I-JSON with no
other knowledge of the types.

```typescript
import { PsszSchema } from 'psio';

const schema = PsszSchema.decode(schemaBytes);        // or PsszSchema.fromJson(text), new PsszSchema({...})
const Transfer = schema.type('Transfer');

const bytes = Transfer.jsonToPssz('{"from":"alice","amount":"18446744073709551615"}');
Transfer.validate(bytes);                             // { ok: true, hasUnknown: false }
Transfer.decode(bytes);                               // { from: 'alice', amount: 18446744073709551615n }
Transfer.psszToJson(bytes);                           // I-JSON: 64-bit beyond 2^53-1 as a string
Transfer.psszToJson(bytes, 'json');                   // strict JSON: {"from":"alice","amount":18446744073709551615}
Transfer.encode({ from: 'alice', amount: 5n });       // Uint8Array
Transfer.view(bytes).get('amount');                   // 18446744073709551615n, nothing else decoded
```

| | |
|---|---|
| `new PsszSchema(ir, options)` | From a `psio::Schema` value, or a compact `{ Name: type }` map ([below](#schemas-written-by-hand)). `options`: `{ handlers, requireHandlers }` ([custom types](#custom-types)). |
| `PsszSchema.decode(bytes, options)` / `.fromJson(text, format, options)` | From the PSSZ / I-JSON encoding of `psio::Schema`. `schema.encode()` writes it back; `schema.ir` is the IR. |
| `schema.withHandlers(handlers, { requireHandlers })` | The same schema with these custom text handlers added. |
| `schema.type(root, { width, handlers, requireHandlers })` | A `PsszType` for a type name or an inline type. `width`: `'auto'` (default; type-derived offset widths, the `psio::pssz` format) or `1` / `2` / `4` (one inherited width, `psio::pssz_<W>`; see [explicit widths](#explicit-widths)). Throws when the type is not expressible. |
| `type.encode(value)` | Canonical bytes. Trailing absent optionals are pruned; length bounds and caps are enforced. |
| `type.decode(bytes, { rejectUnknown })` | The value. The bytes are fully validated first. Unknown appended optional fields are dropped unless `rejectUnknown`. |
| `type.validate(bytes)` | `{ ok: true, hasUnknown }` or `{ ok: false, error }`. Never throws. |
| `type.view(bytes)` | The value read in place ([views](#views)). |
| `type.jsonToPssz(text, format)` / `type.psszToJson(bytes, format)` | Text to bytes and back; `format` is `'ijson'` (default) or `'json'`. |
| `type.jsonToValue(text, format)` / `type.valueToJson(value, format)` | Text to value and back. |
| `type.encodeKey(value)` / `type.decodeKey(bytes)` / `type.validateKey(bytes)` | The `key` format ([keys](#keys)). |
| `type.encodeKeyPrefix(elements)` / `keyPrefixUpperBound(bytes)` | A prefix key and the exclusive end of its range. |
| `type.width` | The offset width of the type's own header / slots / offset table. |
| `jsonToPssz(schema, root, format, text)`, `psszToJson(schema, root, format, bytes)`, `validatePssz(schema, root, bytes)` | The text conversions and validation under the C++ / Rust names. |

## Typed definitions

Application code declares its types once, in TypeScript. The decoded value is
statically typed, and the definition has every operation of `PsszType`.

```typescript
import { record, optional, list, u32, u64, utf8, generateSchema, type Infer } from 'psio';

const Transfer = record('Transfer', {
  from: utf8,
  to: utf8,
  amount: u64,
  memo: optional(utf8),
  tags: list(utf8),
});
type Transfer = Infer<typeof Transfer>;
// { from: string; to: string; amount: bigint; memo: string | null; tags: string[] }

const bytes = Transfer.encode({ from: 'alice', to: 'bob', amount: 5n, memo: null, tags: [] });
const value = Transfer.decode(bytes);       // typed: value.amount is a bigint
Transfer.validate(bytes);                   // { ok: true, hasUnknown: false }
Transfer.psszToJson(bytes);                 // '{"from":"alice","to":"bob","amount":5,"memo":null,"tags":[]}'
Transfer.jsonToValue('{"from":"a","to":"b","amount":"7","memo":"hi","tags":[]}');
Transfer.view(bytes).get('amount');         // 5n

const schema = generateSchema({ Transfer }); // a PsszSchema: schema.ir, schema.encode()
```

A definition holds no codec of its own: it produces the Schema IR and runs
the schema-driven codec on it. `generateSchema({ name: definition, ... })`
gives the IR that C++ `psio::SchemaBuilder` builds from `insert<T>(name)` of
the equivalent types in the same order — records under their own names,
every other type interned as `@N` — so a schema generated here is byte for
byte the one a C++ service publishes for the same types (the test suite
checks this against the three shared fixtures). `definition.schema` is the
schema of one definition and what it reaches.

| Definition | Value | C++ |
|---|---|---|
| `bool` | `boolean` | `bool` |
| `u8` `u16` `u32` `i8` `i16` `i32` | `number` | fixed-width integers |
| `u64` `i64` `u128` `i128` `u256` | `bigint` | `std::uint64_t`, ..., `psio::uint256` |
| `f32` `f64` | `number` | `float`, `double` |
| `char` | `number` (one byte) | `char` |
| `utf8`, `boundedUtf8(max)` | `string` (validated UTF-8) | `psio::utf8_string<>`, `utf8_string<N>` |
| `rawString` | `Uint8Array` (any bytes) | `std::string` |
| `bytes`, `boundedBytes(max)`, `fixedBytes(n)` | `Uint8Array` | `std::vector<std::uint8_t>`, `psio::bounded<std::uint8_t, N>`, `std::array<std::uint8_t, N>` |
| `bitvector(n)`, `bitlist(max)` | `{ bytes, bitCount }` | `psio::bitvector<N>`, `psio::bitlist<N>` |
| `optional(T)` | `T \| null` (of an optional: `{ some: T } \| null`) | `std::optional<T>` |
| `list(T)`, `boundedList(T, max)`, `array(T, n)` | `T[]` | `std::vector<T>`, `psio::bounded<T, N>`, `std::array<T, N>` |
| `tuple(A, B, ...)` | `[A, B, ...]` | `std::tuple<A, B, ...>` |
| `variant({ name: T, ... })` | `{ type: 'name', value: T }` (a discriminated union) | `std::variant<...>` |
| `record(name, { field: T, ... }, options)` | `{ field: T, ... }` | a `PSIO_REFLECT`ed struct |
| `enumeration(u8, { red: 0, green: 1 })` | the integer (`number` / `bigint`); the name in text | a reflected `enum class` |
| `timePoint(unit, tick = i64)`, `duration(unit, tick = i64)` | `bigint` ticks | `std::chrono` time points and durations (`docs/time.md`) |
| `custom(id, T, handler)` | `T`; its own text ([custom types](#custom-types)) | a type with `schema_custom` |
| `box(T)` | `T` | `psio::Box<T>` |
| `lazy(() => T)` | `T` | (a recursive reference) |

**Records** grow by appending optional fields. A reader of the older
definition skips them (`validate` reports `hasUnknown`); a reader of the
newer one sees `null` for fields an older writer did not store. Trailing
absent optionals are never written.

```typescript
const V1 = record('Person', { name: utf8, age: u32 });
const V2 = record('Person', { name: utf8, age: u32, email: optional(utf8) });
V2.decode(V1.encode({ name: 'Alice', age: 30 }));     // { name: 'Alice', age: 30, email: null }
V1.decode(V2.encode({ name: 'Alice', age: 30, email: 'a@b' }));  // { name: 'Alice', age: 30 }
```

Record options: `{ fixed: true }` (the definition will not change: no header,
no evolution), `{ maxDynamicData: n }` (a cap on the encoded size) and
`{ maxFields: n }`. Member options are given with `field(type, options)`:
`lengthBound: { max, min, exact }` for a string, bytes or list member
(through `optional`), and `jsonInt` / `ijsonInt: 'number' | 'string'` for the
text form of its integers. A variant alternative's I-JSON key is its name, or
`alt(type, { ijsonName })`; a C++ `std::variant` without labels is
`variant({ 0: alt(u32, { ijsonName: 'uint32' }), 1: ... })`.

**Widths, caps and bounds.** Offset widths are derived from the type: a
record whose size is bounded (fixed-size members, bounded strings and lists,
or an explicit `maxDynamicData`) uses 1- or 2-byte offsets, otherwise 4.
Bounds and caps are enforced on encode and on decode.

```typescript
const Small = record('Small', { tag: boundedUtf8(8), ids: boundedList(u16, 4), key: fixedBytes(4) });
Small.width;                                    // 1
record('Note', { text: utf8 }, { maxDynamicData: 200 }).width;   // 1
record('Open', { text: utf8 }).width;           // 4
Small.type({ width: 2 });                       // the explicit psio::pssz_<2> profile, a PsszType
```

**Recursive types** name themselves through `lazy`, with the type written out:

```typescript
interface Tree { label: string; children: Tree[] }
const Tree: TypeDef<Tree, RecordView<Tree>> = record('Tree', { label: utf8, children: list(lazy(() => Tree)) });
```

## Views

`type.view(bytes)` reads a value in place. A record view finds one field
from the record's header and slots, a list view one element from its offset
table; nothing else is decoded or allocated.

```typescript
const Line = record('Line', { sku: utf8, qty: u32, note: optional(utf8) });
const Order = record('Order', { id: u64, lines: list(Line), blob: bytes });

const view = Order.view(orderBytes);        // RecordView, statically typed from the definition
view.get('id');                             // bigint
const lines = view.get('lines');            // ListView
lines.length;                               // from the frame; no element is read
lines.at(1).get('sku');                     // string: one element, one field
for (const line of lines) total += line.get('qty');
view.get('blob');                           // Uint8Array: a subarray of orderBytes, not a copy
lines.at(0).toValue();                      // the element decoded in full: { sku, qty, note }
```

| | |
|---|---|
| `RecordView` | `get(name)`, `fields`, `hasUnknown` |
| `TupleView` | `at(index)`, `length` |
| `ListView` (lists and arrays) | `length`, `at(index)`, iteration |
| `VariantView` | `type`, `index`, `value` |
| all of them | `toValue({ rejectUnknown })` (full, checked decode of that value), `bytes` (its encoding, a subarray) |

A scalar, text, optional (`null`) or bit value is returned as its value;
bytes and raw strings alias the input. From a fetched schema the views are
the same objects, untyped (`schema.type('Order').view(bytes) as RecordView`).

**What a view checks.** Every frame on the path to what is read (header,
slot, offset, selector, span, cap) and the value read, with the errors
`decode` gives: corrupt bytes are a `PsszError` when they are reached, never
a wrong value from outside the frame. A view does not look at what it is not
asked for, so the whole-value rules — trailing optionals pruned, member
length bounds, text validity of unread fields — are those of `validate` /
`decode`. For bytes from an untrusted source whose canonical form matters
(anything that is hashed or signed), call `validate` first.

**No in-place mutation.** A PSSZ encoding is canonical: offset widths follow
from the type, trailing absent optionals are pruned, and a value has exactly
one encoding. Patching bytes cannot keep that in general (changing an
optional or a string moves every later offset and may change the pruning),
so this package does not offer it. To change a value: decode, modify, encode.

```typescript
const order = Order.decode(orderBytes);
order.lines[0].qty += 1;
const updated = Order.encode(order);
```

## Keys

The `key` encoding of a value is a byte string whose bytewise order is the
value's order (`docs/key-spec.md`): database keys, range scans, prefix scans.

```typescript
import { tuple, u32, utf8, optional, i64, keyPrefixUpperBound } from 'psio';

const Key = tuple(u32, utf8, optional(i64));
const key = Key.encodeKey([7, 'alice', -1n]);
Key.decodeKey(key);                          // [7, 'alice', -1n]  (checked: one canonical key)
Key.validateKey(key);                        // { ok: true }

const from = Key.encodeKeyPrefix([7]);       // every key of shard 7 ...
const to = keyPrefixUpperBound(from);        // ... lies in [from, to); null: no upper bound
```

A record is a key as well, its fields in order. `schema.type(...)` of a
fetched schema has the same methods.

## JSON and I-JSON

Every type has two text forms. **`'json'`** is strict JSON: every integer is
a bare number, of any size. **`'ijson'`** (the default) is what a browser
exchanges safely: a 64-bit integer is a bare number while its magnitude is at
most 2^53 − 1 and a decimal string beyond; 128- and 256-bit integers are
always strings; bytes are hex; a variant is `{"Name": value}`.

```typescript
const Balance = record('Balance', { owner: utf8, amount: u64 });

Balance.valueToJson({ owner: 'alice', amount: 9007199254740991n });
// '{"owner":"alice","amount":9007199254740991}'
Balance.valueToJson({ owner: 'alice', amount: 9007199254740992n });
// '{"owner":"alice","amount":"9007199254740992"}'          beyond 2^53 - 1: a string
Balance.valueToJson({ owner: 'alice', amount: 9007199254740992n }, 'json');
// '{"owner":"alice","amount":9007199254740992}'            strict JSON: always a number

Balance.jsonToValue('{"owner":"alice","amount":"7"}');      // { owner: 'alice', amount: 7n }
Balance.jsonToValue('{"owner":"alice","amount":7}');        // the same: both forms are read
```

In JavaScript a 64-bit (or wider) integer is always a `bigint`, whatever its
size, so arithmetic never loses precision; `encode` also accepts an exact
`number`. Text is read by an exact parser, not `JSON.parse` (which would
round a large bare number), and written by the codec, not `JSON.stringify`
(which cannot write a `bigint`): pass the text itself to and from `fetch`.
To hand a value to code that expects plain JSON data, use
`JSON.parse(type.valueToJson(value))` — under `'ijson'` every number in it is
safe. The member options `jsonInt` / `ijsonInt` force a number or a string
for one field. The full rules are under [Text](#text).

## Custom types

A type with its own text form — an account name over a `u64`, `major.minor`
over a record — is `custom(id, wire, handler)`: on the wire and as a
JavaScript value it is exactly its wire type; in JSON and I-JSON it is one
string, written and read by the handler. In the Schema IR it is
`Custom { type: <wire type>, id }`.

```typescript
import { custom, record, u64, nameToNumber, numberToName } from 'psio';

// An account name: a u64 in the bytes and in JavaScript, its name in text.
const AccountNumber = custom('AccountNumber', u64, { toText: numberToName, fromText: nameToNumber });
const Balance = record('Balance', { owner: AccountNumber, amount: u64 });

const bytes = Balance.encode({ owner: nameToNumber('alice'), amount: 5n });
Balance.decode(bytes);            // { owner: <the u64 of "alice">, amount: 5n }
Balance.psszToJson(bytes);        // '{"owner":"alice","amount":5}'
Balance.jsonToValue('{"owner":"alice","amount":5}');
```

- A handler works on the **decoded value of the wire type**: `toText(value)`
  returns the text, `fromText(text)` returns a value, which is checked
  against the wire type. Either may throw; that is an ordinary `PsszError`
  (`'json'` on read, `'value'` on write).
- With a handler the presentation is exactly its text: the plain form is not
  also accepted. Without one (`custom(id, wire)`) the type reads and writes
  as its wire type; `type({ requireHandlers: true })` makes that an error in
  text. Binary encode / decode / validate never depend on handlers.
- One handler per id: declaring the same id with two different handlers in
  one schema is a `'schema'` error.

### Handlers for a fetched schema

A schema fetched at run time names its custom types by id only; the handlers
are registered by id:

```typescript
import { PsszSchema, type PsszCustomHandler } from 'psio';

// Custom { type: Object { major: u16, minor: u16 }, id: "Version" } as "1.2"
const version: PsszCustomHandler = {
  toText: (v) => `${(v as { major: number }).major}.${(v as { minor: number }).minor}`,
  fromText(text) {
    const m = /^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$/.exec(text);
    if (!m) throw new Error('not a version');
    return { major: Number(m[1]), minor: Number(m[2]) };   // range-checked against u16 by the codec
  },
};

const schema = PsszSchema.decode(schemaBytes, { handlers: { Version: version } });
const Release = schema.type('Release');
Release.psszToJson(bytes);                       // {"version":"1.2","name":"first"}
Release.jsonToPssz('{"version":"1.2","name":"first"}');
Release.decode(bytes);                           // { version: { major: 1, minor: 2 }, name: 'first' }
```

- A handler works on the **decoded value of the wire type** (what `decode`
  returns for it), not on its bytes: `toText(value)` returns the text,
  `fromText(text)` returns a value. The value `fromText` returns is checked
  against the wire type (and normalized as `decode` would give it); either
  function may throw, which is an ordinary `PsszError` (`'json'` on read,
  `'value'` on write).
- The text is carried as one JSON string in both formats. With a handler
  registered the presentation is exactly its text: the plain form is not also
  accepted.
- Handlers are given once, as `{ id: handler }` or a `Map`: to `new
  PsszSchema` / `PsszSchema.decode` / `PsszSchema.fromJson`, with
  `schema.withHandlers(...)`, or for one type with `schema.type(root, {
  handlers })`. A handler is bound to the type when it is compiled
  (`schema.type`), not looked up per value.
- **Fallback.** An id with no handler reads and writes as its wire type (the
  account as its number, the version as `{"major":1,"minor":2}`); the plain
  text reads back to the same bytes. `requireHandlers: true` makes a missing
  handler an error when text is read or written ("no handler registered for a
  custom type"); binary encode / decode / validate are unaffected.
- The ids of the binary form (`bool`, `char`, `string`, `utf8`, `hex`,
  `bitvector`, `bitlist`, `box`, `unknown`) take no handler.


## What a blockchain client needs

| Need | Where |
|---|---|
| Pack an action's arguments from a schema the service publishes | `PsszSchema.decode(schemaBytes).type(name).jsonToPssz(text)` or `.encode(value)` |
| Build the bytes of a transaction | a typed definition (`record('Transaction', ...)`) or the node's schema; `encode` gives the one canonical encoding |
| Sign it | the bytes are canonical, so hash and sign them with WebCrypto or a wallet (`crypto.subtle.digest('SHA-256', bytes)`); this package holds no keys and no signature code. `toHex` / `fromHex` for transport |
| Decode query results and events | `type.decode(bytes)` / `type.psszToJson(bytes)`; `type.view(bytes)` to read a few fields of large results |
| Check untrusted bytes | `type.validate(bytes)`; every reader fails with a typed error |
| Compute database keys and scan ranges | `encodeKey`, `encodeKeyPrefix`, `keyPrefixUpperBound` |
| Present 64-bit values safely | `bigint` values; I-JSON text (numbers up to 2^53 − 1, strings beyond) |
| Account names | `nameToNumber` / `numberToName`, as the handler of a `custom` type |
| Timestamps | `timePoint` / `duration` types; `timePointToIso`, `timePointFromIso`, `timePointToDate`, `timePointFromDate` |

## Errors and untrusted input

Every failure of the PSSZ, key, JSON and view entry points is a `PsszError`
with a `kind`:

| kind | meaning |
|---|---|
| `'schema'` | the Schema IR or a typed declaration is malformed, or uses something PSSZ cannot carry |
| `'value'` | a JavaScript value does not fit the type, or has no text |
| `'bytes'` | PSSZ or key bytes were rejected |
| `'json'` | JSON / I-JSON text was rejected (with a byte `offset`) |
| `'internal'` | a defect in this library; never expected |

Untrusted bytes and text never raise anything else, never loop on a length
they claim, and no claimed length is trusted before it is checked against
the input. Values and JSON nest at most 64 levels. `validate` /
`validateKey` / `validatePssz` / `pjsonValidate` return a result and never
throw. pjson failures are a `PjsonError`.

## Reference

### Values

| Schema | JavaScript |
|---|---|
| `bool` | `boolean` |
| integers of 8, 16, 32 bits; `char` | `number` |
| integers of 64, 128, 256 bits | `bigint` (an exact `number` is accepted on encode). A 256-bit integer is 32 little-endian bytes on the wire and one `bigint` here |
| enum (`Enum`) | the value of its underlying integer: `number` for 8 to 32 bits, `bigint` for 64. Never the name; names exist only in text |
| duration / time point (`Custom` id `duration-…` / `time-point-…` over the tick `Int`) | `bigint` ticks at every width. JSON: a duration is its tick integer; a time point is an RFC 3339 UTC string with exactly the unit's fraction digits. Built in: no handler is needed or accepted, and `requireHandlers` does not apply |
| floats | `number`; a NaN carrying a payload decodes to a `PsszNaN` (its `bits` re-encode exactly) |
| UTF-8 string (`utf8` adapter: C++ `utf8_string`, Rust `String`) | `string` (validated UTF-8 on the wire; an unpaired surrogate is an error) |
| raw string (`string` adapter: C++ `std::string`, Rust `RawString`) | `Uint8Array`: any bytes, not validated. `encode` and `valueToJson` also accept a JavaScript `string` (its UTF-8) |
| boxed value (`box`) | the value of the boxed type |
| custom type (any other `Custom` id) | the value of its wire type; a handler changes only its text |
| bytes (`hex` adapter) | `Uint8Array` |
| record | object, members in declaration order; a missing optional member is `null` |
| list, bounded list, array, tuple | array |
| variant | `{ type: 'Name', value }` |
| optional | `null` or the value; an optional of an optional is `null` or `{ some: inner }` |
| `bitvector` / `bitlist` | `{ bytes, bitCount }` (no text form) |

A view (`type.view(bytes)`) returns the same values for scalars and text, a
subarray of the input for bytes and raw strings, and `RecordView` /
`TupleView` / `ListView` / `VariantView` for the composites.

### Text

`'json'` writes every integer as a bare number, bytes as an array of numbers
and a variant as `[index, value]`. `'ijson'` writes a 64-bit integer as a bare
number while its magnitude is at most 2^53 - 1 and as a decimal string beyond
(both are read), bytes as uppercase hex (either case is read) and a variant as
`{"Name": value}`. Member attributes `jsonInt` / `ijsonInt` and the variant
attribute `ijsonName` are honoured. A repeated key is read again and the last
one wins; unknown keys are skipped.

**128- and 256-bit integers** are decimal: `'json'` writes a bare number,
`'ijson'` a decimal string for every value, small ones included (`"7"`). Both
formats read a decimal string, a bare number (any in-range value under
`'json'`; a magnitude of at most 2^53 - 1 under `'ijson'`, a larger bare number
is an error) and the older `"0x..."` string (`"0xff"`, `"-0x2a"` for a signed
128-bit integer; hex digits of either case, at least one, in range). The hex
form is input only. Decimal text is canonical (`0 | -?[1-9][0-9]*`, no `-0`).
`jsonInt` / `ijsonInt` force a number or a string as for any other integer.

**Enums.** On the wire and in JavaScript an enum is its underlying integer,
and any in-range value is valid. In text a value equal to a listed enumerator
is written as its name (`"active"`; the first listed when two share a value),
any other as its number by the integer rule of its width and format. Both
formats read all of:

| text | meaning |
|---|---|
| `"active"` | the name |
| `2` | the number |
| `"2"` | the number as a string (a string starting with a digit or `-`) |
| `"active:2"` | both; they must agree |

An unknown name, a disagreement, an empty name or number part, a
non-canonical (`"02"`, `"-0"`, `"+1"`) or out-of-range number and any other
JSON kind are errors; a number that names no enumerator is accepted.

```typescript
const Status = schema.type('Status');     // Enum over i32: pending = -1, active = 2
Status.decode(Status.jsonToPssz('"active"'));   // 2
Status.valueToJson(2);                           // '"active"'
Status.valueToJson(5);                           // '5'
Status.jsonToValue('"pending:-1"');              // -1
```

**Non-finite floats.** NaN, `Infinity` and `-Infinity` have no text in either
format: `valueToJson` / `psszToJson` throw a `PsszError` of kind `'value'`
(a number too large for a 32-bit float counts as infinite there), and no
spelling of one is read. They remain valid PSSZ values.

**Raw strings.** A raw `string` may hold bytes that are not UTF-8, which is
why its value is a `Uint8Array` and not a JavaScript string (decoding to a
string would lose them, or make the value's type depend on the data). Such a
value is valid PSSZ: it validates, decodes and re-encodes unchanged. In text a
raw string is a JSON string of the UTF-8 it holds; when its bytes are not
UTF-8 it has no text and `psszToJson` / `valueToJson` throw a `'value'` error
("string is not valid UTF-8"). `jsonToValue` returns the bytes of the text.
Use `new TextDecoder().decode(value)` to show one. A `utf8` string is checked
on every read and write and is a JavaScript string throughout.

Text is read by an exact parser, not `JSON.parse`: integers are converted from
their token text with `BigInt`, so strict `json` integers beyond 2^53 are
lossless in both directions, and the rules `JSON.parse` cannot see (`-0`,
`1.0`, `1e2`, leading zeros, an escaped digit in an integer string, repeated
keys in a variant object, more than 64 nested containers) are enforced. Floats
are written as C++ `std::to_chars` writes them, byte for byte: the shortest
decimal that reads back as the same 64-bit (or 32-bit) float, fixed or
scientific, whichever is shorter (`0.1`, `1e+21`, `1e-07`,
`5.960464477539063e-08`, `999999999999999868928`). A 32-bit float is rounded
once, from the decimal text.

### Explicit widths

`width: 1 | 2 | 4` is the typed `psio::pssz_<W>` format: one inherited offset
width for every record, list and array. A record's `maxDynamicData` is
accepted there and does not change the width; the cap of the **top-level**
type bounds the whole encoding (over it: an error from `encode`, `decode` and
`validate`), and a nested record's cap is not looked up. The `maxFields`
record attribute is an unsupported constraint under an explicit width (a
`'schema'` error from `schema.type`), as in C++. The automatic profile
derives widths from caps and enforces every record's.

A boxed value (`Custom { type, id: "box" }`: C++ / Rust `Box<T>`) is its inner
type on the wire and in text, with no inferred size bound: under `'auto'` a
record holding one uses 4-byte offsets unless it carries an explicit cap, and
a box as the root type reports width 4.

### IR version

A schema carries `ir_version` (`schema.ir.ir_version`), the version of the
IR's meaning it is written in; this library reads and writes `IR_VERSION`
(1). A schema encoded before the marker existed still decodes — the field
was appended — with `ir_version: null`, and `schema.type(...)` then throws a
`'schema'` error ("no ir_version marker"): such a schema predates the `box`
and `utf8` markers and would silently give other widths and other values. A
caller that knows what it means states so:
`new PsszSchema({ ...old.ir, ir_version: IR_VERSION })`. A map of named
types written in JavaScript is always in the current version; the full form
(`{ types: [...], ir_version }`) says for itself.

### Schemas written by hand

A schema can be written as a map of named types in the compact spelling:

```typescript
const U8 = { Int: { bits: 8, isSigned: false } };
const schema = new PsszSchema({
  Name: { Custom: { type: { BoundedList: { type: U8, maxCount: 32 } }, id: 'utf8' } },
  Point: { Struct: { x: { Float: { exp: 11, mantissa: 52 } }, y: { Float: { exp: 11, mantissa: 52 } } } },
  Color: { Enum: { type: U8, members: { red: 0, green: 1, blue: 7 } } },
  Shape: {
    Object: {
      members: [
        { name: 'name', type: 'Name' },
        { name: 'outline', type: { List: 'Point' } },
        { name: 'id', type: { Int: { bits: 64, isSigned: false } }, attributes: [{ name: 'ijsonInt', value: 'string' }] },
        { name: 'note', type: { Option: 'Name' } },
      ],
      attributes: [{ name: 'maxDynamicData', value: '60000' }],
    },
  },
});
```

`Struct` is a fixed-layout record (no header); `Object` is extensible unless it
carries the `definitionWillNotChange` attribute. Adapters (`Custom.id`):
`bool`, `char`, `utf8` (a UTF-8 string), `string` (a raw string: any bytes),
`hex`, `bitvector`, `bitlist`, `box`. Any other id is a custom text form over
its inner type ([custom types](#custom-types)).

`Enum` is `{ type, members }`: `type` is the underlying integer (8 to 64 bits
for a text form) and `members` a `{ name: value }` map or a list of
`{ name, value }`. In the IR (`schema.ir`, and the PSSZ / JSON encoding of a
schema) an enumerator's `value` is a `bigint`: the value sign-extended to 64
bits and read as unsigned, so `-1` is `18446744073709551615n`; the compact
spelling accepts the negative number and converts it. `Enum` is the last
alternative of the IR's `AnyType` (index 14); a schema encoded before it
existed decodes unchanged.

## WIT, pjson, names, hex

**WIT.** `emitWit(schema)` is the WIT text of a schema — package, interfaces,
worlds and named types — a port of C++ `psio::emit_wit`, for a fetched schema
or one from `generateSchema`:

```typescript
import { emitWit, generateSchema, record, i32 } from 'psio';
emitWit(generateSchema({ Point: record('Point', { x: i32, y: i32 }) }));
// record point {
//   x: s32,
//   y: s32
// }
```

**pjson** (wire revision 2, `docs/wire-contract.md`): self-describing binary
JSON. `pjsonEncode(value)`, `pjsonDecode(bytes)`, `pjsonValidate(bytes)`.
Integers are `bigint`; a decoded object is a `PjsonObject` (ordered entries,
duplicate names kept); `PjsonDecimal`, `PjsonFloat`, `PjsonString`,
`PjsonTypedArray` and `PjsonRows` carry what plain JavaScript values cannot.

```typescript
import { pjsonEncode, pjsonDecode, pjsonValidate } from 'psio';
const bytes = pjsonEncode({ id: 42n, name: 'Alice', tags: ['a', 'b'] });
pjsonValidate(bytes);    // true
pjsonDecode(bytes);      // PjsonObject { entries: [['id', 42n], ['name', 'Alice'], ['tags', ['a', 'b']]] }
```

**Names.** `nameToNumber(name)` / `numberToName(number)` compress a short
name into a 64-bit number (C++ `psio::compress_name`); `isCompressedName` /
`isHashName` tell a compressed name from a hashed one.

**Hex.** `toHex(bytes)` (lowercase) and `fromHex(text)` (either case).

## Exports

| | |
|---|---|
| Schema-driven | `PsszSchema`, `PsszType`, `PsszError`, `PsszNaN`, `IR_VERSION`, `jsonToPssz`, `psszToJson`, `validatePssz`, `keyPrefixUpperBound` |
| Views | `PsszView`, `RecordView`, `TupleView`, `ListView`, `VariantView` |
| Typed definitions | `TypeDef`, `generateSchema`, `record`, `field`, `variant`, `alt`, `optional`, `list`, `boundedList`, `array`, `tuple`, `enumeration`, `timePoint`, `duration`, `custom`, `box`, `lazy`, `bool`, `char`, `u8` `u16` `u32` `u64` `u128` `u256`, `i8` `i16` `i32` `i64` `i128`, `f32` `f64`, `utf8`, `boundedUtf8`, `rawString`, `bytes`, `boundedBytes`, `fixedBytes`, `bitvector`, `bitlist`, `Field`, `Alt` |
| Time | `timePointToIso`, `timePointFromIso`, `timePointToDate`, `timePointFromDate`, `timeSchemaId`, `parseTimeSchemaId`, `timeFractionDigits` |
| Other | `emitWit`, `toHex`, `fromHex`, `nameToNumber`, `numberToName`, `hashName`, `isHashName`, `isCompressedName` |
| pjson | `pjsonEncode`, `pjsonDecode`, `pjsonValidate`, `PJSON_WIRE_REVISION`, `PjsonError`, `PjsonDecimal`, `PjsonFloat`, `PjsonString`, `PjsonTypedArray`, `PjsonRows`, `PjsonObject` |
| Types | `Infer`, `InferView`, `CustomHandler`, `FieldOptions`, `AltOptions`, `RecordOptions`, `LengthBoundOption`, `IntText`, `TypeDefOptions`, `JsonFormat`, `PsszWidth`, `PsszErrorKind`, `PsszValidation`, `PsszKeyValidation`, `PsszDecodeOptions`, `PsszTypeOptions`, `PsszSchemaOptions`, `PsszCustomHandler`, `PsszCustomHandlers`, `PsszSchemaInput`, `PsszTypeInput`, `CompactType`, `CompactEnumerator`, `Some`, `VariantValue`, `BitsValue`, `TimeKind`, `TimeUnit`, `TickType`, `PjsonValue`, and the IR: `IrSchema`, `IrAnyType`, `IrTypeValue`, `IrMember`, `IrAttribute`, `IrEnum`, `IrEnumerator`, `IrFunc`, `IrResource`, `IrInterface`, `IrWorld`, `IrUse` |

## Performance

`npm test` prints a small benchmark (`src/test/bench.test.ts`; reported, it
never fails on timing): encode, decode, validate and view of a typical record
and of a list of 1,000 records, through the typed layer and through a fetched
schema. The two paths run the same codec and measure the same. On an Apple
M-series laptop under Node 22, for an 87-byte record: encode about 2 µs,
decode 1.3 µs, one field through a view 0.5 µs; for the list of 1,000
(88 KB): decode 1.5 ms, one field of one element through a view under 1 µs,
one field of every element 0.6 ms.
