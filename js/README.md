# psio — Fracpack for JavaScript/TypeScript

Schema-first binary serialization with full TypeScript type inference, zero-copy views, and cross-language compatibility with C++ and Rust.

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
