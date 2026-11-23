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
- [Pairs, maps, sets, nested values, transparent types](#pairs-maps-sets-nested-values-transparent-types)
- [Bounds at any depth, method arguments](#bounds-at-any-depth-method-arguments)
- [Views](#views)
- [Keys](#keys)
- [JSON and I-JSON](#json-and-i-json)
- [Custom types](#custom-types)
- [What a blockchain client needs](#what-a-blockchain-client-needs)
- [Errors and untrusted input](#errors-and-untrusted-input)
- [Reference](#reference): values, text rules, widths, IR version, the schema as JSON, hand-written schemas
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
| `new PsszSchema(ir, options)` | From a `psio::Schema` value, or a compact `{ Name: type }` map ([below](#schemas-written-by-hand)). `options`: `{ handlers, requireHandlers, nestedBytes }` ([custom types](#custom-types), [nested values](#pairs-maps-sets-nested-values-transparent-types)). |
| `PsszSchema.decode(bytes, options)` / `.fromJson(text, format, options)` | From the PSSZ bytes / the JSON or I-JSON text of `psio::Schema`. `schema.encode()` / `schema.toJson(format)` write them back ([the schema as JSON](#the-schema-as-json)); `schema.ir` is the IR. |
| `schema.withHandlers(handlers, { requireHandlers, nestedBytes })` | The same schema with these custom handlers (text forms, invariants) added. |
| `schema.type(root, { width, handlers, requireHandlers, nestedBytes })` | A `PsszType` for a type name or an inline type. `nestedBytes: 'opaque'` reads and writes every nested value as its bytes, unexamined (default `'typed'`: validated as the inner type). `width`: `'auto'` (default; type-derived offset widths, the `psio::pssz` format) or `1` / `2` / `4` (one inherited width, `psio::pssz_<W>`; see [explicit widths](#explicit-widths)). Throws when the type is not expressible. |
| `type.encode(value)` | Canonical bytes. Trailing absent optionals are pruned; length bounds and caps are enforced. |
| `type.decode(bytes, { rejectUnknown })` | The value. The bytes are fully validated first. Unknown appended optional fields are dropped unless `rejectUnknown`. |
| `type.validate(bytes)` | `{ ok: true, hasUnknown }` or `{ ok: false, error }`. Never throws. |
| `type.view(bytes)` | The value read in place ([views](#views)). |
| `type.jsonToPssz(text, format)` / `type.psszToJson(bytes, format)` | Text to bytes and back; `format` is `'ijson'` (default) or `'json'`. |
| `type.jsonToValue(text, format)` / `type.valueToJson(value, format)` | Text to value and back. |
| `type.encodeKey(value)` / `type.decodeKey(bytes)` / `type.validateKey(bytes)` | The `key` format ([keys](#keys)). |
| `type.encodeKeyPrefix(elements)` / `keyPrefixUpperBound(bytes)` | A prefix key and the exclusive end of its range. |
| `type.width` | The offset width of the type's own header / slots / offset table. |
| `type.maxEncodedSize` | The inferred size bound (`max_encoded_size`) as a `bigint`, or `null` when unbounded. |
| `type.childWiderThanCap` | A capped record holds a record wider than its cap allows ([widths](#typed-definitions)): a query, not an error. |
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
| `pair(A, B)` | `[A, B]` | `std::pair<A, B>` (the 2-tuple) |
| `map(K, V)` | `Map<K, V>`, in key order | `std::map<K, V>`; `std::unordered_map` with `{ unordered: true }` |
| `set(K)` | `Set<K>`, in key order | `std::set<K>`; `std::unordered_set` with `{ unordered: true }` |
| `nested(T)` | `T` (on the wire: a byte list holding the encoding of a `T`) | `psio::nested<T>` |
| `nested(T, { deferred: true })` | `Uint8Array` (the bytes, not looked at) | `psio::nested<T, psio::nested_validation::deferred>` |
| `transparent(name, T)` | `T` | a `transparent()` type: a named type that is its one member |
| `variant({ name: T, ... })` | `{ type: 'name', value: T }` (a discriminated union) | `std::variant<...>` |
| `record(name, { field: T, ... }, options)` | `{ field: T, ... }` | a `PSIO_REFLECT`ed struct |
| `enumeration(u8, { red: 0, green: 1 })` | the integer (`number` / `bigint`); the name in text | a reflected `enum class` |
| `timePoint(unit, tick = i64)`, `duration(unit, tick = i64)`, `monotonic(unit, tick = i64)` | `bigint` ticks | `std::chrono` time points of `system_clock`, durations, and time points of any other clock (`docs/time.md`) |
| `custom(id, T, handler)` | `T`; its own text and / or invariant ([custom types](#custom-types)) | a type with a text adapter and / or an invariant |
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
`max` for a string, bytes or list member, the entries of a map / set member
or the encoded bytes of a nested member (through `optional`), or
`lengthBound: { max, min, exact }` for a bound with `min` / `exact`;
`utf8`, `element`, `keys`, `values` and `at` for
[bounds below the member](#bounds-at-any-depth-method-arguments); and
`jsonInt` / `ijsonInt: 'number' | 'string'` for the text form of its
integers. A variant alternative's I-JSON key is its name, or
`alt(type, { ijsonName })`; a C++ `std::variant` without labels is
`variant({ 0: alt(u32, { ijsonName: 'uint32' }), 1: ... })`.

**Widths, caps and bounds.** Offset widths are derived from the type, and
every type that has offsets takes its own: a record, a tuple or a pair (a
header and one slot per variable member), and a list, an array, a map or a
set of variable-size elements (an offset table of one entry per element). The
width is 1 byte when the type's size bound is at most 255, 2 through 65 535,
otherwise 4. The bound is inferred (`docs/pssz-spec.md` §3.2): fixed-size
members, bounded strings and lists, a member `lengthBound`, bit types
(`bitvector(n)` is ⌈n/8⌉ bytes, `bitlist(n)` one more), a variant (one
selector byte plus its widest alternative), a tuple or pair (the record of
its elements) are bounded when everything they hold is; a plain string or
list without a bound is not, and neither is anything holding one. Each type
counts its own header, slots and table at its own width — the narrowest whose
worst case fits — and what it holds at that type's finished bound. An
explicit `maxDynamicData` always rules its record: the width follows from the
cap alone, whether the inferred bound is smaller, larger or absent. A tuple
has at most 255 elements. Bounds and caps are enforced on encode and on every
decode (`decode`, `validate`, bytes to text, a view's `toValue()`).

```typescript
const Small = record('Small', { tag: boundedUtf8(8), ids: boundedList(u16, 4), key: fixedBytes(4) });
Small.width;                                    // 1
record('Note', { text: utf8 }, { maxDynamicData: 200 }).width;   // 1
record('Open', { text: utf8 }).width;           // 4
record('Open', { text: utf8 }).maxEncodedSize;  // null: unbounded
tuple(u64, boundedUtf8(32), u8).width;          // 1
tuple(u64, boundedUtf8(32), u8).maxEncodedSize; // 43n   (1 + 8 + (1 + 32) + 1)
record('Event', { at: u64, what: variant({ tick: u8, point: tuple(u32, u32) }) }).width;   // 1
tuple(u32, utf8).width;                         // 4   (an unbounded element)
boundedList(boundedUtf8(16), 15).width;         // 1   (15 * (1 + 16) = 255: a 1-byte offset table)
boundedList(boundedUtf8(17), 15).width;         // 2   (15 * (2 + 17) = 285)
list(boundedUtf8(16)).width;                    // 4   (no count bound)
Small.type({ width: 2 });                       // the explicit psio::pssz_<2> profile, a PsszType
```

**A record's cap bounds what lies beneath it** (`docs/pssz-spec.md` §3.6).
Nothing inside a record capped at N bytes is larger than N, so the lists,
arrays, maps, sets and tuples beneath it — through lists, optionals,
variants and tuples, down to the next record or nested value — take the width
class of the cap when their own bound is larger or absent. The member types
stay the plain ones, and nothing else changes: no new check, no new schema.

```typescript
const Memo = record('Memo', { token: u32, memo: utf8, tags: list(utf8), rows: list(list(utf8)) },
                    { maxDynamicData: 255 });
toHex(Memo.encode({ token: 1, memo: 'hi', tags: ['ab', 'c'], rows: [['x']] }));
// '07' '01000000' '07' '09' '0e' '6869' '0204' '6162' '63' '01' '01' '78': every offset is one byte
const Envelope = record('Envelope', { body: record('Body', { text: utf8 }) }, { maxDynamicData: 255 });
Envelope.childWiderThanCap;                     // true: Body keeps its own 4-byte offsets
```

A tighter bound of the member's own still wins, and a looser one is clamped
by the cap. A record keeps its own width wherever it appears (its bytes do
not depend on what holds it), so a child record that should be narrow
declares its own cap or bounds; `childWiderThanCap` (C++
`psio::pssz_child_wider_than_cap_v<T>`) reports a capped record that holds a
wider one. The explicit profiles ignore all of this.

**Recursive types** name themselves through `lazy`, with the type written out:

```typescript
interface Tree { label: string; children: Tree[] }
const Tree: TypeDef<Tree, RecordView<Tree>> = record('Tree', { label: utf8, children: list(lazy(() => Tree)) });
```

## Pairs, maps, sets, nested values, transparent types

```typescript
import { map, set, pair, nested, transparent, record, tuple, u8, u16, u32, u64, utf8, type Infer } from 'psio';

const Args = record('Args', { to: utf8, amount: u64 });
const Id = transparent('Id', u64);              // a named type that IS a u64: bytes, key, text, value
const Book = record('Book', {
  owner: Id,
  head: pair(u16, utf8),                        // [number, string]: the 2-tuple
  counts: map(utf8, u32),                       // Map<string, number>
  ids: set(u16),                                // Set<number>
  byPoint: map(tuple(u8, u8), utf8),            // Map<[number, number], string>
  call: nested(Args),                           // { to, amount }: on the wire, the bytes of an Args
});
type Book = Infer<typeof Book>;

const bytes = Book.encode({
  owner: 7n, head: [513, 'x'],
  counts: new Map([['b', 2], ['a', 1]]),        // any order
  ids: new Set([513, 2]),
  byPoint: new Map([[[1, 2], 'p']]),
  call: { to: 'bob', amount: 5n },
});
const book = Book.decode(bytes);
[...book.counts];                               // [['a', 1], ['b', 2]]   canonical order
[...book.ids];                                  // [2, 513]
Book.psszToJson(bytes);
// {"owner":7,"head":[513,"x"],"counts":{"a":1,"b":2},"ids":[2,513],
//  "byPoint":[[[1,2],"p"]],"call":{"to":"bob","amount":5}}
```

**Maps and sets** (`docs/pssz-spec.md` §5.9) add no wire shape: a map is the
list of its `[key, value]` entries and a set the list of its elements, with
one rule — the entries are in **strictly ascending order of the key bytes of
the key** (`encodeKey`: numeric for integers, bytewise for strings,
lexicographic for tuples and records), and no key occurs twice. That is the
same order in C++, Rust and JavaScript.

- `decode` returns a `Map` / `Set` whose iteration order is that order.
  `encode` takes a `Map` / `Set` — at run time any iterable of entries /
  elements — in any order, writes the canonical order, and refuses two
  entries with equal key bytes (`map has duplicate keys`). Every reader
  (`validate`, `decode`, `view(...).toValue()`, `decodeKey`, `jsonToValue`)
  refuses a frame that is out of order or repeats a key.
- **Composite keys are objects.** A `Map` finds a key by identity, so
  `map.get([1, 2])` finds nothing for a tuple, record or raw-string key:
  look such an entry up by iterating. Primitive keys (numbers, bigints,
  strings, booleans) work as usual.
- **The float −0.** A `Set` / `Map` stores +0 for −0 and PSSZ keeps the sign
  bit, so a set element or map key that is exactly the float −0 decodes to
  `PsszNegativeZero.value` (`Number(x)` is −0); every encoder accepts it
  wherever a float is expected. −0 and +0 are one key: a set holds one of
  them.
- **Text.** A set is an array. A map is an object when its key is a string
  *by type* (`utf8`, `rawString`, a transparent type over one, a custom type
  whose handler has a text form) and an array of `[key, value]` pairs
  otherwise — an integer key is not an object key even where I-JSON writes
  it as a string. Readers take the entries in any order and refuse a
  repeated key (`json: duplicate map key`); writers emit the canonical
  order.
- In the Schema IR they are `Custom{ List{ Tuple{[K, V]} }, "map" }` and
  `Custom{ List{ K }, "set" }`; `map` and `set` are part of the binary form
  and take no handler. A key type must have a key encoding (not `bitlist`,
  a one-bit integer or a boxed type): a schema error otherwise.

**Nested values** (`docs/pssz-spec.md` §5.8). A `nested(T)` member is, on
the wire, a byte list holding the PSSZ encoding of a `T` — always in the
automatic width profile, whatever the profile around it — so a record with a
`nested(T)` member and the same record with a `bytes` member are
byte-identical: a router declares bytes, the service that owns the arguments
declares them typed.

- The value is the inner value. Every reader validates the inner bytes as a
  `T` (unknown appended fields inside are accepted and reported through
  `hasUnknown`, like any others), and `encode` writes the canonical encoding
  of the value: fields a newer writer appended to the inner record do not
  survive decode → encode.
- `nested(T, { deferred: true })`, or the option `nestedBytes: 'opaque'` on
  `schema.type(root, ...)` / `definition.type(...)` for every nested node of
  a type: the value is the bytes (a `Uint8Array`), which the enclosing
  reader does not look at and the writer writes as they are — for a relay
  that must keep foreign bytes exactly. Validate them with `T.validate`
  where they are read.
- Text is the inner value's own text in both formats, never hex. The key of
  a nested value is the key of its byte list: nested values order by their
  inner **bytes** (a `nested(u32)` holding 256 sorts before the one holding
  1), which is also their order as map keys and set elements.
- A member `lengthBound` on a nested member counts the bytes of the inner
  encoding. In the Schema IR the node is `Nested{type}`, whatever the policy.

**Transparent types** (`docs/pssz-spec.md` §5.10). `transparent(name, T)` is
a named type that is exactly its member on the wire, in keys, in text, in
views and as a JavaScript value: no header, no slot. It keeps its name — in
the Schema IR it is the named alias `name` of the member's type — and is
where a text form or an invariant is attached:
`custom('DerKey', transparent('DerKey', bytes), { check })`. Its member is
not an `optional` (hold an `optional(Wrapper)` instead).

**Pairs.** `pair(A, B)` is `tuple(A, B)`: same bytes, key, text and view.

*Reproducing a C++ schema byte for byte.* C++ interns one schema entry per
C++ type, and several C++ types share a node: `std::pair` and the 2-tuple,
`std::map` and `std::unordered_map`, a nested value and its deferred form.
`pair`, `{ unordered: true }` and `{ deferred: true }` are those second
identities, so that `generateSchema` of the equivalent declarations is the
schema `psio::SchemaBuilder` builds. They change nothing on the wire.

## Bounds at any depth, method arguments

A bound does not have to be in the type (`docs/pssz-spec.md` §3.5).
`field(type, bounds)` bounds a member and any position below it — the
elements of a list, the keys of a map, one element of a tuple — with the
member's type written as the plain string / list / map it is:

```typescript
import { record, field, list, map, tuple, variant, optional, rawString, bytes, u32, type Infer } from 'psio';

const Post = record('Post', {
  title:  field(rawString, { utf8: true, max: 80 }),                              // text, at most 80 bytes
  tags:   field(list(rawString), { max: 8, element: { utf8: true, max: 16 } }),   // at most 8, each at most 16
  votes:  field(map(rawString, list(u32)), { max: 4, keys: { utf8: true, max: 16 }, values: { max: 2 } }),
  origin: field(tuple(rawString, u32, bytes), { at: { 0: { utf8: true, max: 16 }, 2: { max: 5 } } }),
  body:   field(variant({ Text: rawString, Parts: list(rawString) }),
                { at: { Text: { utf8: true, max: 280 }, Parts: { max: 4, element: { utf8: true, max: 280 } } } }),
  reply:  field(optional(list(rawString)), { max: 2, element: { max: 32 } }),      // when present
});
type Post = Infer<typeof Post>;
// { title: string; tags: string[]; votes: Map<string, number[]>; origin: [string, number, Uint8Array];
//   body: { type: 'Text'; value: string } | { type: 'Parts'; value: string[] }; reply: Uint8Array[] | null }
Post.width;                                    // 2: every position is bounded
Post.maxEncodedSize;                           // 1566n
```

| Option | Bounds |
|---|---|
| `max: n` | the position: bytes of a `rawString` or `bytes`, elements of a `list`, entries of a `map` or `set` (on the member itself also a `utf8` string and the bytes of a `nested` value) |
| `utf8: true` | a `rawString` here is UTF-8 text: validated, and a `string` in JavaScript instead of a `Uint8Array` |
| `element: { ... }` | every element of a `list`, a `boundedList`, an `array` or a `set` |
| `keys: { ... }`, `values: { ... }` | every key, every mapped value of a `map` |
| `at: { 0: { ... } }` | one element of a `tuple` / `pair`, by index; one alternative of a `variant`, by name (or index) |

The options nest to any depth, and an `optional` is looked through: what is
written for it applies to its payload. Below the member itself a length
bound is `max` (`min` and `exact` are the member's: `lengthBound`). Bounds
written for a position they cannot apply to are a schema error that names
the position. The static type follows: `Infer` and the views give `string`
exactly where `utf8: true` is written.

**The member is the type its bounds describe.** `tags` above is a bounded
list of bounded UTF-8 strings: it has that type's size bound (so the record
narrows its offsets), that type's widths at every depth — a list, a map, a
set, an array of variable elements, a tuple or a pair at or below the member
takes its width from the bounds of its position: the list below has a 1-byte
offset table — the same bytes, and the same checks. The wrapper types say the same thing in
the type, and a reader cannot tell the two apart:

```typescript
const Tags    = record('Tags', { tags: field(list(rawString), { max: 8, element: { utf8: true, max: 16 } }) });
const Wrapped = record('Tags', { tags: boundedList(boundedUtf8(16), 8) });
toHex(Tags.encode({ tags: ['ab', 'c'] }));     // '01010204616263'
toHex(Wrapped.encode({ tags: ['ab', 'c'] }));  // the same bytes
```

They differ only in a generated schema, as in C++: `field(list(rawString),
{ ... })` is the C++ member annotation
`attr(tags, max<8> | element(utf8 | max<16>))` on a
`std::vector<std::string>` (the member's plain type is registered and the
bounded type is written in the member), `boundedList(boundedUtf8(16), 8)`
is `psio::bounded<psio::utf8_string<16>, 8>` (a registered type of its own).
Use the spelling the C++ type uses when `generateSchema` must reproduce a
C++ schema byte for byte; otherwise either.

**Every writer and reader checks every bound**, at every depth: `encode`,
`validate`, `decode`, a view's `toValue()`, bytes to text, text to value and
to bytes, `valueToJson`, `encodeKey` and `decodeKey`. A value outside a bound
is an error of kind `value` (a writer) or `bytes` / `json` (a reader).
(C++ `psio::key` checks these bounds when it reads a key, not when it writes
one; here both do.)

```typescript
Post.encode({ ...post, tags: ['a'.repeat(17)] });   // throws PsszError: pssz IR: byte count bound exceeded
```

**Method arguments** (`docs/pssz-spec.md` §16). The arguments of a method
are the tuple of its parameters. `argumentTuple(parameters, options)`
declares it: each parameter bounded by its `field(type, bounds)`, and
`maxDynamicData` capping the encoded arguments. The cap rules the tuple's
offset width for the method's lifetime, and — the argument tuple being a
record for this purpose — the widths of the lists, maps and tuples among the
arguments; without one the width follows from the parameters' bounds.

```typescript
import { argumentTuple, field, optional, rawString, u32, u64, toHex } from 'psio';

const parameters = {
  to: field(rawString, { utf8: true, max: 16 }), amount: u64, memo: field(rawString, { utf8: true, max: 80 }),
};
const Transfer  = argumentTuple(parameters, { maxDynamicData: 256 });
const Transfer2 = argumentTuple({ ...parameters, fee: optional(u32) }, { maxDynamicData: 256 });

Transfer.width;                                 // 2: the cap (256) rules
argumentTuple(parameters).width;                // 1: no cap, the bounds give 107 bytes at most
const args = Transfer.encode({ to: 'al', amount: 5n, memo: 'hi' });
toHex(args);                                    // '0c000c0005000000000000000e00616c6869'
Transfer2.decode(args);                         // { to: 'al', amount: 5n, memo: 'hi', fee: null }
```

- In JavaScript the arguments are an object keyed by parameter name. On the
  wire they are the tuple: the bytes of
  `tuple(boundedUtf8(16), u64, boundedUtf8(80))` at the tuple's width, and
  the tuple's key.
- A method gains optional trailing parameters as a record gains optional
  fields (`Transfer2` reads `Transfer`'s bytes, and the reverse with
  `hasUnknown`). Declare the cap from the start on a method that may grow:
  an added argument that moves the inferred bound into the next width class
  changes the bytes of every call.
- Arguments over the cap, or outside a parameter's bounds, are refused by
  every writer and reader.
- In the Schema IR a `Tuple` node has no attributes, so the argument tuple is
  the unnamed `Object` of the parameters carrying `maxDynamicData`, as C++
  (`psio::reflect<T>::method_arg_tuple<I>`) writes it, and a schema-driven
  reader derives the same width and bytes. Each `argumentTuple(...)` is a
  type of its own. A parameter's bound is `max` (no `min` / `exact`), on a
  type that has a bounded form.
- In text the arguments are the object of the parameters, which is what the
  Schema IR describes. (The C++ typed codec writes its `method_arg_tuple`
  as the tuple, a JSON array.)

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
| `TupleView` (tuples and pairs) | `at(index)`, `length` |
| `ListView` (lists and arrays; a map is the list of its `[key, value]` entries and a set the list of its elements, in wire order) | `length`, `at(index)`, iteration |
| `VariantView` | `type`, `index`, `value` |
| all of them | `toValue({ rejectUnknown })` (full, checked decode of that value), `bytes` (its encoding, a subarray) |

A scalar, text, optional (`null`) or bit value is returned as its value;
bytes and raw strings alias the input. A nested value is the view of its
inner value over the inner bytes (the bytes themselves when deferred /
opaque); a transparent or custom type is its member's / wire type's view. From a fetched schema the views are
the same objects, untyped (`schema.type('Order').view(bytes) as RecordView`).

**What a view checks.** Every frame on the path to what is read (header,
slot, offset, selector, span, cap) and the value read, with the errors
`decode` gives: corrupt bytes are a `PsszError` when they are reached, never
a wrong value from outside the frame. A view does not look at what it is not
asked for, so the whole-value rules — trailing optionals pruned, member
length bounds, text validity of unread fields, the order of a map / set, the
invariants of custom types (a handler's `check`) — are those of `validate` /
`decode` (and of `toValue()` for the value it decodes). For bytes from an untrusted source whose canonical form matters
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

The key of a map / set is the key of the list of its entries / elements in
ascending key order (so maps order lexicographically by their sorted
entries); `decodeKey` refuses a key whose entries are out of order or
repeat. The key of a nested value is the key of the byte list holding its
inner encoding, not of the inner value. A transparent or custom type has the
key of its member / wire type — a text form plays no part, an invariant
does: with the type's handler registered, a value that violates its
invariant has no key and its key is refused.

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

A custom type is a type with an identity of its own over its wire type, of
any shape — a record, a transparent type, an enum, a variant, a tuple, a
list: a **text form** (an account name over a `u64`, `major.minor` over a
record), an **invariant** (a key that must be a DER sequence, `lo <= hi`),
or both. It is `custom(id, wire, handler)`: on the wire and as a JavaScript
value it is exactly its wire type; the handler supplies the text form
(`toText` / `fromText`: one JSON string in both formats) and / or the
invariant (`check`). In the Schema IR it is `Custom { type: <wire type>, id }`.

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

- A handler works on the **decoded value of the wire type**; every member is
  optional. `toText(value)` returns the text, `fromText(text)` returns a
  value, which is checked against the wire type. Either may throw; that is
  an ordinary `PsszError` (`'json'` on read, `'value'` on write).
- With a text form the presentation is exactly its text: the plain form is
  not also accepted. Without one the type reads and writes as its wire type.
- One handler per id: declaring the same id with two different handlers in
  one schema is a `'schema'` error.

**Invariants** (`docs/pssz-spec.md` §18). `check(value)` says whether a
structurally valid value of the wire type is a value of the type: it returns
a message (or throws an `Error` with one) when it is not.

```typescript
import { custom, transparent, record, bytes, u32 } from 'psio';

const DerKey = custom('DerKey', transparent('DerKey', bytes), {
  check: (b) => (b.length > 0 && b[0] === 0x30 ? undefined : 'DerKey: not a DER sequence'),
});
const Range = custom('Range', record('Range', { lo: u32, hi: u32 }), {
  check: (r) => (r.lo <= r.hi ? undefined : 'Range: lo is greater than hi'),
});

Range.encode({ lo: 1, hi: 5 });                 // the bytes of the record
Range.encode({ lo: 5, hi: 1 });                 // throws: 'Range: lo is greater than hi' (kind 'value')
Range.validate(reversedBytes);                  // { ok: false, error }: the same message (kind 'bytes')
Range.jsonToValue('{"lo":5,"hi":1}');           // throws the same (kind 'json')
```

- The check is part of the type: with the handler, **every** entry point
  runs it — `encode`, `decode`, `validate`, `view(...).toValue()`,
  `encodeKey` / `decodeKey`, `jsonToValue` / `valueToJson` / `jsonToPssz` /
  `psszToJson`, under every width profile. A value that violates it has no
  encoding, no key and no text, and bytes or text of such a value are
  rejected. A view reads in place and runs no check.
- It runs after the structure of exactly that value was accepted (it never
  sees a malformed value), values inside it first: record members, list
  elements, optional and variant payloads, the keys and values of a map (the
  key's failure is reported before its value's), the value inside nested
  bytes, a key read from a JSON object member name. The first failure in
  wire order is the one reported.
- The error is the type's own: `error.message` is the message unchanged and
  `error.invariant` is `true`.
- The order of a map / set is defined for every key and computing it runs no
  check: an entry whose key violates its invariant and is in its place is
  refused for the invariant, not for the order.
- A handler that only checks leaves the text to the wire type. In
  particular it does not decide the text form of a map keyed by the type:
  the type under it does.

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

- A handler is `{ toText?, fromText?, check? }` and works on the **decoded
  value of the wire type** (what `decode` returns for it), not on its
  bytes: `toText(value)` returns the text, `fromText(text)` returns a value,
  `check(value)` returns a message when the value violates the type's
  invariant. The value `fromText` returns is checked against the wire type
  (and normalized as `decode` would give it); either text function may
  throw, which is an ordinary `PsszError` (`'json'` on read, `'value'` on
  write). A handler with no member just declares the id known.
- The text is carried as one JSON string in both formats. With a handler
  registered the presentation is exactly its text: the plain form is not also
  accepted.
- Handlers are given once, as `{ id: handler }` or a `Map`: to `new
  PsszSchema` / `PsszSchema.decode` / `PsszSchema.fromJson`, with
  `schema.withHandlers(...)`, or for one type with `schema.type(root, {
  handlers })`. A handler is bound to the type when it is compiled
  (`schema.type`), not looked up per value.
- **The three registries.** What the codec does with a custom node depends
  only on the registry it is given:

  | registry | text | invariant |
  |---|---|---|
  | no handler for the id | the wire type's own text (the account as its number, the version as `{"major":1,"minor":2}`), which reads back to the same bytes | **not checked**: the node is its wire type, structure only |
  | a handler for the id | the handler's text, or the wire type's when it has only `check` | checked, on the binary and the text entry points alike |
  | strict (`requireHandlers: true`) and no handler for the id | an error, "no handler registered for a custom type" | the same error, on the binary entry points too (`encode`, `decode`, `validate`, keys), wherever a value of the type occurs |

  A caller that must not accept or produce a value it cannot check and
  present exactly uses a strict registry.
- The ids of the binary form (`bool`, `char`, `string`, `utf8`, `hex`,
  `bitvector`, `bitlist`, `map`, `set`, `box`, `unknown`) take no handler.


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
| Timestamps | `timePoint` / `duration` / `monotonic` types; `timePointToIso`, `timePointFromIso`, `timePointToDate`, `timePointFromDate` (time points only: a monotonic time point has no date) |

## Errors and untrusted input

Every failure of the PSSZ, key, JSON and view entry points is a `PsszError`
with a `kind`:

| kind | meaning |
|---|---|
| `'schema'` | the Schema IR or a typed declaration is malformed, or uses something PSSZ cannot carry |
| `'value'` | a JavaScript value does not fit the type, violates its invariant, or has no text |
| `'bytes'` | PSSZ or key bytes were rejected |
| `'json'` | JSON / I-JSON text was rejected (with a byte `offset`) |
| `'internal'` | a defect in this library; never expected |

When the invariant of a custom type (a handler's `check`) refused a
structurally valid value, `error.invariant` is `true` and `error.message` is
the invariant's own message, under whichever of `'value'` / `'bytes'` /
`'json'` the entry point reports.

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
| duration / time point / monotonic time point (`Custom` id `duration-…` / `time-point-…` / `monotonic-…` over the tick `Int`) | `bigint` ticks at every width. JSON: a duration is its tick integer; a time point is an RFC 3339 UTC string with exactly the unit's fraction digits; a monotonic time point (a clock other than the system clock: no date) is its tick integer, exactly like a duration, and never a date-time string. Built in: no handler is needed or accepted, and `requireHandlers` does not apply |
| floats | `number`; a NaN carrying a payload decodes to a `PsszNaN` (its `bits` re-encode exactly) |
| UTF-8 string (`utf8` adapter: C++ `utf8_string`, Rust `String`) | `string` (validated UTF-8 on the wire; an unpaired surrogate is an error) |
| raw string (`string` adapter: C++ `std::string`, Rust `RawString`) | `Uint8Array`: any bytes, not validated. `encode` and `valueToJson` also accept a JavaScript `string` (its UTF-8) |
| boxed value (`box`) | the value of the boxed type |
| custom type (any other `Custom` id) | the value of its wire type; a handler gives it a text form and / or an invariant |
| map (`Custom` id `map` over a list of 2-tuples) | `Map`, iterating in canonical (key) order; `encode` takes a `Map` or any iterable of `[key, value]` entries. A float key that is −0 is `PsszNegativeZero.value` |
| set (`Custom` id `set` over a list) | `Set`, in the same order; `encode` takes a `Set` or any iterable |
| nested value (`Nested`) | the inner value; with `nestedBytes: 'opaque'` its bytes, a `Uint8Array` |
| named alias (a transparent type) | the value of the type it names |
| bytes (`hex` adapter) | `Uint8Array` |
| record | object, members in declaration order; a missing optional member is `null` |
| list, bounded list, array, tuple (a pair is the 2-tuple) | array |
| variant | `{ type: 'Name', value }` |
| optional | `null` or the value; an optional of an optional is `null` or `{ some: inner }` |
| `bitvector` / `bitlist` | `{ bytes, bitCount }` (no text form) |

A view (`type.view(bytes)`) returns the same values for scalars and text, a
subarray of the input for bytes and raw strings, and `RecordView` /
`TupleView` / `ListView` / `VariantView` for the composites (a map / set is
the `ListView` of its entries / elements; a nested value the view of its
inner value).

### Text

`'json'` writes every integer as a bare number, bytes as an array of numbers
and a variant as `[index, value]`. `'ijson'` writes a 64-bit integer as a bare
number while its magnitude is at most 2^53 - 1 and as a decimal string beyond
(both are read), bytes as uppercase hex (either case is read) and a variant as
`{"Name": value}`. Member attributes `jsonInt` / `ijsonInt` and the variant
attribute `ijsonName` are honoured. A repeated key is read again and the last
one wins; unknown keys are skipped.

**Maps, sets and nested values.** A set is an array of its elements. A map
is an object `{"key": value, ...}` when its key type is a string by type — a
`utf8` / raw `string` node, reached through named aliases, boxes and custom
nodes whose handler has no text form, or a custom node whose registered
handler has one (the member name is then the handler's text) — and an array
of `[key, value]` pairs otherwise; a custom key type with no handler falls
back, as everywhere, to the form of its wire type. A member's `jsonInt` /
`ijsonInt` reaches the elements of a set; a map entry is a tuple, whose
elements take the natural form. Entries are read in any order and written
in canonical order; a repeated key or element is an error on read (two keys
are the same when their key bytes are: `[["5","a"],[5,"b"]]` under
`'ijson'`), and a member name is read exactly as the same text would be read
as a string of the key type, the key type's invariant included. A nested
value is the inner value's own text; reading it stores the canonical
encoding of what was read.

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
The bytes inside a nested value are the automatic profile under every width
(and so are checked by its rules, `maxFields` included).

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

### The schema as JSON

A schema travels as PSSZ bytes or as text, and the text is the same in C++,
Rust and JavaScript, byte for byte:

```typescript
const schema = new PsszSchema({ Point: { Object: { x: { Int: { bits: 32, isSigned: true } } } } });

schema.toJson();          // I-JSON (the default)
// '{"package":{"name":"","version":"","attributes":[]},"interfaces":[],"worlds":[],"uses":[],' +
// '"types":[{"name":"Point","type":{"value":{"Object":{"members":[{"name":"x","type":{"value":' +
// '{"Int":{"bits":32,"isSigned":true}}},"attributes":[]}],"attributes":[]}}},"attributes":[]}],"ir_version":1}'
schema.toJson('json');    // strict JSON: the type is {"value":[1,{...}]}, the member's {"value":[8,{...}]}

PsszSchema.fromJson(schema.toJson());                  // the same schema
PsszSchema.fromJson(schema.toJson('json'), 'json');
```

It is not a format of its own: it is the text the JSON codec of this package
gives the value `schema.ir` under the schema of `psio::Schema`
(`PsszSchema.meta()`), exactly as C++ `psio::encode(psio::ijson{}, schema)`
and Rust `json::to_string::<IJson, _>(&schema)` give it from the IR's
reflected records. So the [text rules](#text) apply unchanged: members in
declaration order; a type is the record `{"value": <variant>}` and the
variant `{"Int": {...}}` under `'ijson'`, `[8, {...}]` under `'json'`; an
unknown key is skipped, a missing member is an error unless it is optional
(`ir_version`, a function's `result`, an attribute's `value`), a repeated
key is read again and the last one wins. `Array.len`,
`BoundedList.maxCount` and `Enumerator.value` are 64-bit: a `bigint` in
`schema.ir`, a bare number up to 2^53 − 1 and a decimal string beyond under
`'ijson'`, read exactly (never through `JSON.parse`). Text nested deeper
than 64 arrays / objects is rejected, which allows a type 19 levels of
inline list nesting (the PSSZ form has the same limit); deeper types go
through a named type. The shared fixture
`conformance/schema-json-vectors.json` pins the bytes and both texts of
several schemas, and texts that must be rejected, for all three languages
(`src/test/schema-json-conformance.test.ts`).

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
`hex`, `bitvector`, `bitlist`, `box`, and `map` / `set`
(`{ Custom: { type: { List: { Tuple: [K, V] } }, id: 'map' } }`,
`{ Custom: { type: { List: K }, id: 'set' } }`; a `BoundedList` for a maximum
count), which sit directly on their list. Any other id is a custom type over
its inner type ([custom types](#custom-types)). A nested value is
`{ Nested: T }`; a transparent type is a named alias (`Id: 'u64-type-name'`
or `Id: U64`).

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
| Schema-driven | `PsszSchema`, `PsszType`, `PsszError`, `PsszNaN`, `PsszNegativeZero`, `IR_VERSION`, `jsonToPssz`, `psszToJson`, `validatePssz`, `keyPrefixUpperBound` |
| Views | `PsszView`, `RecordView`, `TupleView`, `ListView`, `VariantView` |
| Typed definitions | `TypeDef`, `generateSchema`, `record`, `field`, `argumentTuple`, `variant`, `alt`, `optional`, `list`, `boundedList`, `array`, `tuple`, `pair`, `map`, `set`, `nested`, `transparent`, `enumeration`, `timePoint`, `duration`, `monotonic`, `custom`, `box`, `lazy`, `bool`, `char`, `u8` `u16` `u32` `u64` `u128` `u256`, `i8` `i16` `i32` `i64` `i128`, `f32` `f64`, `utf8`, `boundedUtf8`, `rawString`, `bytes`, `boundedBytes`, `fixedBytes`, `bitvector`, `bitlist`, `Field`, `Alt` |
| Time | `timePointToIso`, `timePointFromIso`, `timePointToDate`, `timePointFromDate`, `timeSchemaId`, `parseTimeSchemaId`, `reservedTimeId`, `timeFractionDigits` |
| Other | `emitWit`, `toHex`, `fromHex`, `nameToNumber`, `numberToName`, `hashName`, `isHashName`, `isCompressedName` |
| pjson | `pjsonEncode`, `pjsonDecode`, `pjsonValidate`, `PJSON_WIRE_REVISION`, `PjsonError`, `PjsonDecimal`, `PjsonFloat`, `PjsonString`, `PjsonTypedArray`, `PjsonRows`, `PjsonObject` |
| Types | `Infer`, `InferView`, `CustomHandler`, `AssocOptions`, `FieldOptions`, `PositionOptions`, `ArgumentTupleOptions`, `Bounded`, `BoundedView`, `AltOptions`, `RecordOptions`, `LengthBoundOption`, `IntText`, `TypeDefOptions`, `JsonFormat`, `PsszWidth`, `PsszErrorKind`, `PsszValidation`, `PsszKeyValidation`, `PsszDecodeOptions`, `PsszTypeOptions`, `PsszSchemaOptions`, `PsszCustomHandler`, `PsszCustomHandlers`, `PsszNestedBytes`, `PsszSchemaInput`, `PsszTypeInput`, `CompactType`, `CompactEnumerator`, `Some`, `VariantValue`, `BitsValue`, `TimeKind`, `TimeUnit`, `TickType`, `PjsonValue`, and the IR: `IrSchema`, `IrAnyType`, `IrTypeValue`, `IrMember`, `IrAttribute`, `IrEnum`, `IrEnumerator`, `IrFunc`, `IrResource`, `IrInterface`, `IrWorld`, `IrUse` |

## Performance

`npm test` prints a small benchmark (`src/test/bench.test.ts`; reported, it
never fails on timing): encode, decode, validate and view of a typical record
and of a list of 1,000 records, through the typed layer and through a fetched
schema. The two paths run the same codec and measure the same. On an Apple
M-series laptop under Node 22, for an 87-byte record: encode about 2 µs,
decode 1.3 µs, one field through a view 0.5 µs; for the list of 1,000
(88 KB): decode 1.5 ms, one field of one element through a view under 1 µs,
one field of every element 0.6 ms.
