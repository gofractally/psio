# JSON and I-JSON text formats

Two format tags share one reflection-driven engine in each language.

| | C++ | Rust |
|---|---|---|
| RFC 8259 JSON | `psio::json` | `psio::json::Json` |
| RFC 7493 I-JSON | `psio::ijson` | `psio::json::IJson` |

Both languages write byte-identical text for the same value and accept and
reject the same input. `conformance/json-vectors.json` (emitted by
`cpp/tests/json_conformance_emit.cpp`, checked for drift by ctest, consumed
by `cpp/tests/json_conformance_tests.cpp` and
`rust/psio/tests/json_conformance.rs`) is the shared evidence: every row
carries a value's `json` and `ijson` texts, or a text one format must accept
(with its canonical re-encoding) or reject.

## Rules

`json` is unchanged from before `ijson` existed: every integer is a bare
number, a byte sequence is an array of numbers, a variant is
`[index, value]`.

`ijson` differs in exactly three ways, with no per-type annotation:

| Shape | `json` | `ijson` |
|---|---|---|
| 64-bit integers (`u64`, `i64`) | `18446744073709551615` | `"18446744073709551615"` |
| byte sequence | `[1,255]` | `"01FF"` (uppercase on output) |
| variant | `[3,{...}]` | `{"Inner":{...}}` |

8/16/32-bit integers, `bool` and floats are numbers in both. Byte sequences
are C++ `std::vector` / `std::array` of `char`, `unsigned char` or
`std::byte` and `psio::byte_array<N>`; Rust `Vec<u8>` and `[u8; N]`.
`int8_t` / `i8` sequences are arrays of numbers.

Common to both formats:

- record: object with the member names, in declaration order;
- `std::vector` / `std::array` / `psio::bounded` / `Vec` / `[T; N]`: array;
- `std::tuple` / Rust tuple: array of the elements in order; the empty tuple
  is `[]`. `std::pair` is not supported (no other PSIO format supports it);
- optional: `null` or the value;
- strings: C++ `utf8_string<N>` / Rust `String` and the raw strings C++
  `std::string` / Rust `RawString` are JSON strings;
- enum without payload: its underlying integer (follows the integer rule);
- floats: shortest round-trip decimal as `std::to_chars` writes it
  (`0.1`, `1e+21`, `1e-07`, `-0`);
- a type with a custom text form is a JSON string holding that text: C++
  `PSIO_ADAPTER(T, psio::text_category, ...)` or a member `psio::as<Tag>`;
  Rust `TextForm` + `psio::json_text_form!(T)`.

### Variant alternative names

C++ uses `get_type_name` of the alternative: a record's reflected name, and
`bool int8 uint8 int16 uint16 int32 uint32 int64 uint64 float32 double
string` for primitives, `T[]` for a vector, `T[N]` for an array, `T?` for an
optional, `tuple_A_B`. Alternatives must have distinct names (checked at
compile time). In Rust the name is the enum variant's identifier unless
`#[json(name = "uint32")]` renames it; use that to pair with a C++ variant.

### Integer form per member

A member (or, as the default for all its members, a record type) can force a
number or a string, independently per format:

```cpp
PSIO_FIELD_ATTRS(T, id,    psio::json_string)                       // "id":"7" in json
PSIO_FIELD_ATTRS(T, count, psio::ijson_number)                      // bare in ijson
PSIO_FIELD_ATTRS(T, both,  psio::json_string | psio::ijson_number)
PSIO_TYPE_ATTRS(T, psio::ijson_number)
```

```rust
#[derive(psio::JsonText)]
#[json(ijson_number)]                 // type level
struct T {
    #[json(json_string)] id: u64,
    #[json(json_string, ijson_string)] small: u32,
}
```

The form reaches the member's integer through optional, vector and array;
it does not reach into nested records, tuples or variants. It applies to
integers of any width.

## Reading

One typed reader per language defines the accepted set. In C++ it backs
`validate`, `validate_strict`, `decode`, `make_boxed` and
`decode_json_checked`; in Rust `from_slice` / `from_str` / `validate` and the
crate-root `psio::decode` / `psio::validate`.

- Exactly one value of the type's shape, JSON whitespace between tokens,
  nothing after it.
- Integers match `0 | -?[1-9][0-9]*` and must fit the type: no leading zero,
  plus sign, `-0`, fraction or exponent. The string form holds exactly those
  characters (no whitespace, no escapes).
- **Wide integers under ijson** are read from the string form and also from
  a bare number in the range JavaScript holds exactly (|n| <= 2^53 - 1). A
  larger bare number is rejected: an I-JSON producer could not have written
  it without loss. Output is always the string, so there is one canonical
  form. A member with a forced form accepts exactly that form; narrow
  integers and every `json` integer accept numbers only.
- Hex: an even number of hex digits of either case; exactly 2N for a
  fixed-size array.
- Floats: any RFC 8259 number that is finite in the target type (underflow
  reads as zero). `NaN`, `Infinity` and strings are rejected.
- Strings: valid UTF-8, `\u` escapes with correctly paired surrogates, no raw
  control characters. `ijson` also rejects Unicode noncharacters (RFC 7493).
- Arrays, tuples and fixed arrays require their exact element count.
- Records: members in any order. **Unknown keys are skipped** (the value
  must be well-formed JSON); this is what the previous `psio::json` decoder
  intended ("unknown keys fall through") but it never advanced past the
  value. **A missing member is an error unless its type is optional**, which
  reads as empty; tuples are positional and have no such rule.
  **A repeated key** is an error under `ijson`, also among unknown keys and
  inside skipped values; under `json` the last one wins.
- Variants: `ijson` needs an object with exactly one known alternative name;
  `json` a two-element array with an index in range.
- Depth: more than 64 nested arrays/objects is rejected
  (`kMaxValidationDepth`), on typed and skipped values alike.

The C++ fail-fast `psio::decode` raises the reader's error as
`codec_exception` (aborts when exceptions are disabled). For `psio::json`
only, it keeps one leniency of the old decoder: a missing non-optional
member is left value-initialized. `validate` and `decode_json_checked` are
the entry points for untrusted text.

## Writing can fail

A value with no JSON text is an error, never lossy or invalid output:

- a raw string (C++ `std::string`, Rust `RawString`) whose bytes are not
  valid UTF-8, in both formats;
- under `ijson`, a non-finite float and a string holding a noncharacter.

C++ `psio::encode` fails fast (`codec_exception`, or abort without
exceptions); `psio::encode_json_checked(fmt, value, out)` returns a
`codec_status` and leaves `out` unchanged on failure. Rust returns `Err`.
C++ `psio::json` still prints a non-finite float as `nan` / `inf` (not JSON,
unchanged); Rust `Json` returns an error instead.

## Exception-disabled builds

`cpp/tests/ijson_noexceptions.cpp` builds with `-fno-exceptions -fno-rtti`
and exercises `validate`, `validate_strict`, `decode_json_checked` and
`encode_json_checked` for both formats on valid, truncated, mutated and
depth-bomb input.

## Runtime-schema path

`dynamic_json.hpp` has `ijson_encode_dynamic` / `ijson_decode_dynamic` and
the `encode_dynamic` / `decode_dynamic` CPOs for `psio::ijson`: 64-bit
integers as strings, `Bytes` and sequences of `Uint8` as uppercase hex. The
runtime `psio::schema` has no variant or tuple shape, so those rules have no
dynamic form, and the dynamic reader (like the `json` one) assumes
well-formed text and reports no errors. `view_to_json.hpp` renders a
schemaless pjson view and has no type information to apply the ijson rules;
it is unchanged. The PSSZ schema-IR runtime (`pssz_ir.hpp`, Rust
`pssz::dynamic`) has no JSON path.

## Open

- **128- and 256-bit integers.** `ext_int.hpp` registers a text adapter for
  `uint128` / `int128` / `uint256` that writes `"0x..."` lowercase hex, and a
  text adapter applies in `ijson` exactly as in `json`. That conflicts with
  writing wide integers as decimal strings; the adapter form stands in both
  formats until decided. Rust has no JSON form for `u128` / `i128` /
  `Uint256`.
- Rust `BoundedString` / `BoundedRawString` / `BoundedList`, bit types and
  `psio::bitvector` / `bitlist` have no JSON form.
- Rust `validate` materializes the value and drops it.
