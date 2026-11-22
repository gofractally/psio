# psio — JavaScript/TypeScript serialization

Schema-first binary serialization with full TypeScript type inference, zero-copy views, and cross-language compatibility with C++ and Rust.

pSSZ is psio's primary schema-driven binary format; this package implements it
schema-driven (from a Schema IR, no generated types), together with the JSON
and I-JSON text forms of the same values: see [PSSZ, JSON and
I-JSON](#pssz-json-and-i-json) below. The fracpack API documented after it
implements a legacy prototype retained for benchmark comparisons of
data-layout designs. This package also provides the pjson codec described in
the [repository README](../README.md#javascript--typescript).

## PSSZ, JSON and I-JSON

A client that has a Schema IR (`psio::Schema`, normally fetched as its PSSZ
bytes) converts between JavaScript values, PSSZ bytes, JSON and I-JSON with no
other knowledge of the types. The results are those of the C++ and Rust codecs
(`docs/pssz-spec.md`, `docs/json.md`): the test suite replays every vector of
`conformance/pssz-vectors.json` and `conformance/json-vectors.json`.

```typescript
import { PsszSchema } from 'psio';

const schema = PsszSchema.decode(schemaBytes);        // or new PsszSchema({...}), PsszSchema.fromJson(text)
const Transfer = schema.type('Transfer');

const bytes = Transfer.jsonToPssz('{"from":"alice","amount":"18446744073709551615"}');
Transfer.validate(bytes);                             // { ok: true, hasUnknown: false }
Transfer.decode(bytes);                               // { from: 'alice', amount: 18446744073709551615n }
Transfer.psszToJson(bytes);                           // I-JSON: 64-bit beyond 2^53-1 as a string
Transfer.psszToJson(bytes, 'json');                   // strict JSON: {"from":"alice","amount":18446744073709551615}
Transfer.encode({ from: 'alice', amount: 5n });       // Uint8Array
```

### API

| | |
|---|---|
| `new PsszSchema(ir, options)` | From a `psio::Schema` value, or a compact `{ Name: type }` map (below). `options`: `{ handlers, requireHandlers }` ([custom text handlers](#custom-text-handlers)). |
| `PsszSchema.decode(bytes, options)` / `.fromJson(text, format, options)` | From the PSSZ / I-JSON encoding of `psio::Schema`. `schema.encode()` writes it back; `schema.ir` is the IR. |
| `schema.withHandlers(handlers, { requireHandlers })` | The same schema with these custom text handlers added. |
| `schema.type(root, { width, handlers, requireHandlers })` | A `PsszType` for a type name or an inline type. `width`: `'auto'` (default; type-derived offset widths, the `psio::pssz` format) or `1` / `2` / `4` (one inherited width, `psio::pssz_<W>`; see [explicit widths](#explicit-widths)). Throws when the type is not expressible. |
| `type.encode(value)` | Canonical bytes. Trailing absent optionals are pruned; length bounds and caps are enforced. |
| `type.decode(bytes, { rejectUnknown })` | The value. The bytes are fully validated first. Unknown appended optional fields are dropped unless `rejectUnknown`. |
| `type.validate(bytes)` | `{ ok: true, hasUnknown }` or `{ ok: false, error }`. Never throws. |
| `type.jsonToPssz(text, format)` / `type.psszToJson(bytes, format)` | Text to bytes and back; `format` is `'ijson'` (default) or `'json'`. |
| `type.jsonToValue(text, format)` / `type.valueToJson(value, format)` | Text to value and back. |
| `jsonToPssz(schema, root, format, text)`, `psszToJson(schema, root, format, bytes)`, `validatePssz(schema, root, bytes)` | The same, under the C++ / Rust names. |

Every failure is a `PsszError` with a `kind`: `'schema'` (the IR), `'value'` (a
JavaScript value that does not fit the type, or has no text), `'bytes'`
(rejected PSSZ), `'json'` (rejected text, with a byte `offset`). Untrusted
bytes and text never raise anything else, nest at most 64 levels, and no
length they claim is trusted before it is checked against the input.

### Values

| Schema | JavaScript |
|---|---|
| `bool` | `boolean` |
| integers of 8, 16, 32 bits; `char` | `number` |
| integers of 64, 128, 256 bits | `bigint` (an exact `number` is accepted on encode). A 256-bit integer is 32 little-endian bytes on the wire and one `bigint` here |
| enum (`Enum`) | the value of its underlying integer: `number` for 8 to 32 bits, `bigint` for 64. Never the name; names exist only in text |
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

### Custom text handlers

A type with its own text form (an account name over a `u64`, `major.minor`
over a record) is, in the Schema IR, `Custom { type: <wire type>, id }` with
an id that is not one of the binary adapters. On the wire it is exactly its
wire type: widths, bytes, validation and the decoded JavaScript value do not
depend on the id. Its text comes from a handler registered for the id:

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

### Schemas written by hand

The compact spelling is the one `generateSchema` produces, plus `BoundedList`,
`Enum` and member attributes:

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
its inner type ([custom text handlers](#custom-text-handlers)).

`Enum` is `{ type, members }`: `type` is the underlying integer (8 to 64 bits
for a text form) and `members` a `{ name: value }` map or a list of
`{ name, value }`. In the IR (`schema.ir`, and the PSSZ / JSON encoding of a
schema) an enumerator's `value` is a `bigint`: the value sign-extended to 64
bits and read as unsigned, so `-1` is `18446744073709551615n`; the compact
spelling accepts the negative number and converts it. `Enum` is the last
alternative of the IR's `AnyType` (index 14); a schema encoded before it
existed decodes unchanged.

## Install

```bash
npm install psio
```

## Quick Start

```typescript
import { struct, str, u32, bool, vec, optional } from 'psio';

// Define a schema — TypeScript infers the type automatically
const Person = struct({
  name: str,
  age: u32,
  active: bool,
  tags: vec(str),
  bio: optional(str),
});

// Pack to binary
const data = Person.pack({
  name: 'Alice',
  age: 30,
  active: true,
  tags: ['engineer', 'rust'],
  bio: 'Likes cats',
});

// Unpack from binary
const alice = Person.unpack(data);
console.log(alice.name); // 'Alice'

// Zero-copy view (lazy, no deserialization until field access)
const view = Person.view(data);
console.log(view.age); // 30 — only this field is decoded
```

## Type System

### Scalar Types

| Type | TypeScript | Size | Description |
|------|-----------|------|-------------|
| `bool` | `boolean` | 1 byte | 0 or 1 |
| `u8` | `number` | 1 byte | Unsigned 8-bit integer |
| `u16` | `number` | 2 bytes | Unsigned 16-bit integer |
| `u32` | `number` | 4 bytes | Unsigned 32-bit integer |
| `u64` | `bigint` | 8 bytes | Unsigned 64-bit integer |
| `i8` | `number` | 1 byte | Signed 8-bit integer |
| `i16` | `number` | 2 bytes | Signed 16-bit integer |
| `i32` | `number` | 4 bytes | Signed 32-bit integer |
| `i64` | `bigint` | 8 bytes | Signed 64-bit integer |
| `f32` | `number` | 4 bytes | 32-bit float |
| `f64` | `number` | 8 bytes | 64-bit float |

### Variable-Size Types

```typescript
import { str, bytes } from 'psio';

// UTF-8 string
const packed = str.pack('hello world');

// Raw bytes
const raw = bytes.pack(new Uint8Array([0xDE, 0xAD]));
```

### Containers

```typescript
import { optional, vec, array, tuple, map } from 'psio';

// Optional — null or value
const optAge = optional(u32);
optAge.pack(42);    // Some(42)
optAge.pack(null);  // None

// Variable-length list
const scores = vec(u32);
scores.pack([95, 87, 92]);

// Fixed-length array (no length prefix in binary)
const rgb = array(u8, 3);
rgb.pack([255, 128, 0]);

// Heterogeneous tuple
const pair = tuple(str, u32);
pair.pack(['Alice', 30]);

// String-keyed map
const config = map(str);
config.pack({ host: 'localhost', port: '8080' });
```

### Variant (Tagged Union)

```typescript
import { variant, tuple } from 'psio';

const Shape = variant({
  Circle: f64,                    // radius
  Rectangle: tuple(f64, f64),     // width, height
  Point: tuple(),                 // no data
});

// Pack with { type, value } syntax
Shape.pack({ type: 'Circle', value: 3.14 });
Shape.pack({ type: 'Rectangle', value: [10, 20] });
Shape.pack({ type: 'Point', value: [] });

// Unpack returns { type, value }
const shape = Shape.unpack(data);
if (shape.type === 'Circle') {
  console.log(`radius: ${shape.value}`);
}
```

### Struct (Extensible Record)

```typescript
import { struct, str, u32, bool, optional } from 'psio';

const Person = struct({
  name: str,
  age: u32,
  active: bool,
});

// Structs are forward-compatible — you can add trailing optional fields
// and old readers will still be able to read the data.
const PersonV2 = struct({
  name: str,
  age: u32,
  active: bool,
  email: optional(str),  // new field, optional
});

// V2 can read V1 data (missing email → null)
const v1data = Person.pack({ name: 'Alice', age: 30, active: true });
const person = PersonV2.unpack(v1data);
console.log(person.email); // null
```

### Fixed Struct (Non-Extensible Record)

```typescript
import { fixedStruct, f64 } from 'psio';

// No u16 size header — slightly smaller, slightly faster,
// but cannot add fields later without breaking compatibility.
const Point = fixedStruct({ x: f64, y: f64 });
const packed = Point.pack({ x: 1.5, y: 2.5 }); // exactly 16 bytes
```

### Nested Types

Types compose freely:

```typescript
const Address = struct({
  street: str,
  city: str,
  zip: str,
});

const Person = struct({
  name: str,
  age: u32,
  address: Address,
  phones: vec(str),
  scores: map(u32),
  shape: variant({ Circle: f64, Square: f64 }),
});
```

## Zero-Copy Views

`view()` returns a lazy Proxy that decodes fields on demand. For partial reads of large structs, this is significantly faster than `unpack()`.

```typescript
const BigStruct = struct({
  id: u64,
  name: str,
  // ... 20 more fields ...
});

const data = BigStruct.pack(value);

// unpack() decodes ALL fields upfront
const full = BigStruct.unpack(data);

// view() decodes nothing until you access a field
const view = BigStruct.view(data);
console.log(view.id); // only 'id' is decoded — ~5x faster for partial access
```

### TypedArray Views for Numeric Vectors

Scalar vecs (`vec(u32)`, `vec(f64)`, etc.) return zero-copy TypedArray views:

```typescript
const data = vec(u32).pack([1, 2, 3, 4, 5]);
const viewed = vec(u32).view(data);
// viewed is a Uint32Array pointing directly into the packed buffer
// No copying, no element-by-element decode — 500x+ faster than unpack
```

### Direct Field Access

`readField()` reads a single field without creating any intermediate objects:

```typescript
const name = Person.readField(data, 'name');  // zero-allocation decode
const age = Person.readField(data, 'age');
```

### In-Place Field Mutation

`setField()` updates a single field and returns a new buffer:

```typescript
// Fixed-size fields are patched in-place (fast, O(field_size))
const updated = Person.setField(data, 'age', 31);

// Variable-size fields trigger a full repack (correct, O(total_size))
const renamed = Person.setField(data, 'name', 'Bob');
```

## JSON Conversion

Schema-aware conversion between fracpack binary and JSON. Compatible with Rust's JSON conventions.

```typescript
import { fracToJson, jsonToFrac, valueToJson, jsonToValue } from 'psio';

const Person = struct({ name: str, age: u32, id: u64, avatar: bytes });

// Binary → JSON-friendly value
const json = fracToJson(Person, packedData);
// { name: 'Alice', age: 30, id: '999', avatar: 'deadbeef' }
//   u64 → string (JSON has no 64-bit int)
//   bytes → hex string
//   optional null → null
//   variant → { CaseName: value }

// JSON-friendly value → binary
const repacked = jsonToFrac(Person, json);
// Reverses all conversions: string → bigint, hex → bytes, etc.

// For working with already-unpacked JS values:
const jsonValue = valueToJson(Person, unpackedObject);
const packReady = jsonToValue(Person, jsonValue);
```

### Hex Utilities

```typescript
import { toHex, fromHex } from 'psio';

toHex(new Uint8Array([0xDE, 0xAD])); // 'dead'
fromHex('cafebabe'); // Uint8Array([0xCA, 0xFE, 0xBA, 0xBE])
```

## Validation

Validate packed binary data against a schema without unpacking:

```typescript
import { validate } from 'psio';

const result = validate(Person, data);
switch (result.status) {
  case 'valid':
    // Data matches schema exactly
    break;
  case 'extended':
    // Valid but has unknown fields (forward-compatible extension)
    console.log(result.message);
    break;
  case 'invalid':
    // Data is malformed
    console.error(result.message);
    break;
}
```

## Schema Generation

### AnyType Format (Rust-Compatible)

Generate JSON schema descriptions compatible with Rust's fracpack library:

```typescript
import { generateSchema, typeToAnyType } from 'psio';

const Person = struct({ name: str, age: u32 });

// Single type
const anyType = typeToAnyType(Person);
// { Object: { name: { Custom: { type: { List: ... }, id: 'string' } }, age: { Int: { bits: 32, isSigned: false } } } }

// Registry of named types
const schema = generateSchema({ Person });
// { Person: { Object: { ... } } }
```

### WIT Generation (WebAssembly Interface Types)

Generate WIT text format for WebAssembly component model interop:

```typescript
import { generateWit, typeToWitExpr } from 'psio';

const Person = struct({ name: str, age: u32, active: bool });

// Single type expression
typeToWitExpr(Person); // generates an inline reference

// Full world definition
const wit = generateWit('my-api', {
  types: { Person },
  exports: {
    getPerson: { params: { id: u32 }, result: Person },
    setPerson: { params: { person: Person } },
  },
  imports: {
    log: { params: { msg: str } },
  },
});
```

Output:

```wit
record person {
  name: string,
  age: u32,
  active: bool,
}

world my-api {
  import log: func(msg: string);
  export get-person: func(id: u32) -> person;
  export set-person: func(person: person);
}
```

## Name Compression

Compact name encoding for string identifiers (matches C++ `compress_name` and Rust equivalents):

```typescript
import { nameToNumber, numberToName, hashName, isHashName, isCompressedName } from 'psio';

// Short names (≤12 chars, limited charset) compress to 64-bit numbers
const n = nameToNumber('alice');
const s = numberToName(n); // 'alice'

// Longer or unusual names fall back to a hash
const h = hashName('some-long-identifier');
isHashName(h);       // true
isCompressedName(n); // true (no hash bit set)
```

## Varint Encoding

Variable-length integer encoding for compact wire format:

```typescript
import {
  packVaruint32, unpackVaruint32,
  packVarint32, unpackVarint32,
} from 'psio';

// Pack to Uint8Array
const buf = packVaruint32(300);    // [0xAC, 0x02]
const val = unpackVaruint32(buf);  // { value: 300, bytesRead: 2 }

// Signed
const sbuf = packVarint32(-1);
const sval = unpackVarint32(sbuf);
```

## Type Inference

`Infer<T>` extracts the TypeScript type from any `FracType`:

```typescript
import { type Infer } from 'psio';

const Person = struct({
  name: str,
  age: u32,
  tags: vec(str),
  bio: optional(str),
});

type Person = Infer<typeof Person>;
// { name: string; age: number; tags: string[]; bio: string | null }

function greet(p: Person) {
  console.log(`Hello ${p.name}, age ${p.age}`);
}
```

## Binary Format

Fracpack uses a fixed-section + heap layout, little-endian throughout:

- **Scalars**: inline at their natural size
- **Strings/bytes**: `u32 length` + data
- **Vec**: `u32 num_bytes` + elements (fixed parts inline, variable parts on heap)
- **Optional**: `u32 offset` (0 = empty container, 1 = None, >1 = relative offset to data)
- **Struct (extensible)**: `u16 fixed_size` + fixed fields + heap data. Trailing optional fields can be elided.
- **Fixed struct**: fields concatenated, no header
- **Variant**: `u8 case_index` + `u32 size` + case data
- **Tuple**: `u16 fixed_size` + fields + heap data (like struct but positional)
- **Array**: elements concatenated, no length prefix (length is part of the type)
- **Map**: encoded as `vec(struct({ key: str, value: V }))`

All offsets are relative (position of offset field + offset value = target position).

## Performance

Benchmarks on typical workloads (Node.js, Apple M-series):

| Operation | vs unpack |
|-----------|-----------|
| Struct view, partial access (1/10 fields) | ~5x faster |
| Vec of structs, single element access | ~115x faster |
| Scalar vec (10K u32s), TypedArray view | ~565x faster |
| `readField()` single field | ~6x faster |

## Cross-Language Compatibility

The binary format is byte-identical across JavaScript, C++ (`psio`), and Rust (`fracpack`). Data packed in one language can be unpacked in another with no conversion.

## API Reference

### Core

| Export | Description |
|--------|-------------|
| `FracType<T>` | Type descriptor interface — `pack()`, `unpack()`, `view()` |
| `StructType<T>` | Extends FracType with `readField()` and `setField()` |
| `Infer<T>` | Extract TypeScript type from FracType |
| `SchemaNode` | Union type describing type structure |
| `FracpackError` | Error class for pack/unpack failures |

### Type Constructors

| Export | Description |
|--------|-------------|
| `bool, u8, u16, u32, u64, i8, i16, i32, i64, f32, f64` | Scalar types |
| `str` | UTF-8 string |
| `bytes` | Raw byte array |
| `optional(inner)` | Nullable wrapper |
| `vec(element)` | Variable-length list |
| `array(element, len)` | Fixed-length array |
| `tuple(...elements)` | Heterogeneous tuple |
| `variant(cases)` | Tagged union |
| `struct(fields)` | Extensible record (returns `StructType`) |
| `fixedStruct(fields)` | Non-extensible record (returns `StructType`) |
| `map(valueType)` | String-keyed map |

### JSON

| Export | Description |
|--------|-------------|
| `fracToJson(type, data)` | Binary → JSON-friendly value |
| `jsonToFrac(type, json)` | JSON-friendly value → binary |
| `valueToJson(type, value)` | JS value → JSON-friendly value |
| `jsonToValue(type, json)` | JSON-friendly value → pack-ready JS value |
| `toHex(data)` | Uint8Array → hex string |
| `fromHex(hex)` | Hex string → Uint8Array |

### Validation

| Export | Description |
|--------|-------------|
| `validate(type, data)` | Validate binary data without unpacking |
| `ValidationResult` | `{ status: 'valid' \| 'extended' \| 'invalid', message? }` |

### Schema

| Export | Description |
|--------|-------------|
| `typeToAnyType(type)` | FracType → AnyType JSON (Rust-compatible) |
| `generateSchema(types)` | Named types → Schema registry |
| `AnyType` | JSON-serializable type description |
| `Schema` | `Record<string, AnyType>` |

### WIT

| Export | Description |
|--------|-------------|
| `generateWit(worldName, opts)` | Generate WIT world text |
| `typeToWitExpr(type)` | Single type → WIT expression |
| `toKebabCase(s)` | camelCase/snake_case → kebab-case |
| `WitFunc` | Function descriptor for WIT generation |
| `WitOptions` | Options for `generateWit()` |

### Name Compression

| Export | Description |
|--------|-------------|
| `nameToNumber(name)` | String → compressed 64-bit number |
| `numberToName(n)` | Compressed number → string |
| `hashName(name)` | String → hashed 64-bit number |
| `isHashName(n)` | Check if number is a hash (vs compressed) |
| `isCompressedName(n)` | Check if number is losslessly compressed |

### Varint

| Export | Description |
|--------|-------------|
| `packVaruint32(v)` | Unsigned varint → bytes |
| `unpackVaruint32(buf)` | Bytes → unsigned varint |
| `packVarint32(v)` | Signed varint → bytes |
| `unpackVarint32(buf)` | Bytes → signed varint |
| `encodeVaruint32(v, w)` | Encode into Writer |
| `decodeVaruint32(r)` | Decode from Reader |
| `encodeVarint32(v, w)` | Encode signed into Writer |
| `decodeVarint32(r)` | Decode signed from Reader |
