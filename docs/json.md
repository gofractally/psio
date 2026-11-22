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
| 64-bit integers (`u64`, `i64`) | `18446744073709551615` | `7`, `"18446744073709551615"` (see below) |
| byte sequence | `[1,255]` | `"01FF"` (uppercase on output) |
| variant | `[3,{...}]` | `{"Inner":{...}}` |

**64-bit integers under `ijson`.** A value whose magnitude is at most
2^53 - 1 (the range a JavaScript number holds exactly) is written as a bare
number; a larger magnitude is written as a decimal string. So `0`, `-1` and
`9007199254740991` are bare, `"9007199254740992"`, `"-9007199254740992"`,
`"18446744073709551615"` and `"-9223372036854775808"` are strings. The form
depends on the value, not only on the type: a consumer must accept both.

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
- **64-bit integers under ijson** are read from the string form (any
  in-range value, so `"7"` is accepted) and from a bare number in the range
  JavaScript holds exactly (|n| <= 2^53 - 1). A larger bare number is
  rejected: an I-JSON producer could not have written it without loss. The
  writer emits one form per value (bare within the range, string beyond it),
  so re-encoding `"7"` gives `7`. A member with a forced form
  (`ijson_number`, `ijson_string`) is written in and accepts exactly that
  form; narrow integers and every `json` integer accept numbers only.
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
  **A repeated key is not an error in either format: the last one wins.**
  JSON (RFC 8259) allows repeated keys, and last-wins lets the reader work
  in a single pass with no seen-key bookkeeping. Each occurrence is decoded
  as it is seen, so an earlier occurrence that is later overridden must
  still be a well-formed value of the member's type (`{"a":"x","a":2}` is
  rejected for an integer `a`); a repeated list member replaces the earlier
  list. This holds for known members, unknown keys and keys inside skipped
  values. RFC 7493 forbids I-JSON *senders* from emitting repeated keys;
  PSIO's writers never emit one, while its readers accept them. The one
  object that may not repeat a key is the `ijson` variant form, which has
  exactly one member.
- Variants: `ijson` needs an object with exactly one member, named by a known alternative (a second key, repeated or not, is an error);
  `json` a two-element array with an index in range.
- Depth: more than 64 nested arrays/objects is rejected
  (`kMaxValidationDepth`), on typed and skipped values alike.

The C++ fail-fast `psio::decode` raises the reader's error as
`codec_exception` (aborts when exceptions are disabled). For `psio::json`
only, it keeps one leniency of the old decoder: a missing non-optional
member is left value-initialized. `validate` and `decode_json_checked` are
the entry points for untrusted text.

## Length bounds

A bounded wrapper (C++ `psio::bounded<T, N>`, `utf8_string<N>`; Rust
`BoundedList`, `BoundedString`, `BoundedRawString`) and a C++ member
annotated `length_bound{.max/.exact/.min}` (looking through `optional`) are
enforced on both sides: the reader rejects a string or list outside its
bound (`validate`, `decode_json_checked`, `decode`), and the writer refuses
to produce one. The count is bytes for a string and elements for a list.

## Writing can fail

A value with no JSON text is an error, never lossy or invalid output:

- a string or list outside its length bound (above);
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

## Two layers: syntax and types

Reading JSON is split in two, so the parser can be replaced without touching
any type rule:

1. **Syntax** — a *cursor* that owns the text: whitespace, punctuation,
   string escapes and UTF-8, the RFC 8259 number grammar, the nesting limit,
   skipping a value. C++ `psio::JsonCursor` (`json_cursor.hpp`), Rust
   `psio::json::JsonCursor`. Events: `kind` (peek), `begin_object` /
   `next_field`, `begin_array` / `next_element`, `string` (unescaped, with an
   `escaped` flag), `number` (as its token text), `boolean`, `null`, `skip`,
   `finish`.
2. **Types** — a walk that checks the cursor's events against the type:
   member names, arity, integer range and form, hex digits, variant names,
   length bounds, missing members. It holds no text positions and does no
   tokenizing.

A cursor is forward-only and single-pass (values consumed once, in document
order; a string view is valid until the next call), which is the contract of
simdjson's on-demand API. The walks need no random access and no second
pass: record members are stored as they arrive (a repeated key is read again
and replaces the earlier value), and a variant's selector precedes its value
in both text forms (`[index, value]`, `{"Name": value}`).

Today the only cursor is the hand-written reader (`psio::json_text_cursor`,
Rust `Reader`), and the schema-driven walk below is the only walk written
against the cursor. The typed readers (`json.hpp` `read_value`, Rust
`JsonText::read_json`) still read the text directly; moving them onto the
cursor is a mechanical follow-up.

What the hand-written syntax layer does that a replacement must also do (or
that the walk would have to take over):

- reports, for a string, whether it was written with escapes (integer and
  hex strings must be written literally);
- rejects Unicode noncharacters in `ijson` strings and keys, also inside
  skipped values; rejects raw control characters, unpaired surrogate
  escapes and invalid UTF-8;
- hands a number over as text (integers are parsed exactly, including the
  full `u64` range and the "bare only up to 2^53 - 1" rule; `-0`, leading
  zeros, fractions and exponents are told apart);
- limits nesting to `kMaxValidationDepth` (64), on skipped values too;
- never throws or aborts, needs no padding or copy of the input, and builds
  for a WASI guest with exceptions and RTTI disabled.

Repeated keys need nothing from the syntax layer (last one wins).

## Schema-driven path (no C++ / Rust type)

A consumer that has only a Schema IR (`schema_ir.hpp`, Rust
`psio::pssz::schema_ir`) — a CLI, an HTTP "pack this action" endpoint, an
indexer — converts and validates with:

| | C++ (`psio/pssz_ir_json.hpp`, namespace `psio::pssz_ir`) | Rust (`psio::pssz::dynamic`) |
|---|---|---|
| text → PSSZ | `json_to_pssz(schema, root, psio::ijson{}, text, bytes)` | `json_to_pssz::<IJson>(&schema, &root, text)` |
| PSSZ → text | `pssz_to_json(schema, root, psio::ijson{}, bytes, text, &has_unknown)` | `pssz_to_json::<IJson>(&schema, &root, bytes)` |
| validate PSSZ | `validate_pssz(schema, root, bytes, &has_unknown)` | `validate_pssz(&schema, &root, bytes)` |

`root` is an `AnyType`; a named type is `Type{"Name"}` (C++ also accepts the
name itself: `json_to_pssz(schema, "Transfer", ...)`). Use `psio::json{}` /
`Json` for the other text format.

The result is the typed codecs' result for the type the IR describes: the
same PSSZ bytes as `psio::pssz` (type-derived offset widths, mandatory
pruning, caps and length bounds enforced) and the same text as `psio::json`
/ `psio::ijson`. Both shared fixtures record the IR of their types and both
languages re-derive every vector from the IR alone
(`cpp/tests/schema_conformance_tests.cpp`,
`rust/psio/tests/schema_conformance.rs`). Everything is checked and
status-returning; it builds with exceptions disabled
(`cpp/tests/pssz_ir_noexceptions.cpp`).

The IR carries what that needs (the C++ `SchemaBuilder` and the Rust
`ToPsszSchema` derive write it):

- shape: records (`definitionWillNotChange` for fixed layout), tuples,
  variants, options, lists, arrays, bounded lists, the `string` / `hex` /
  `bool` / `char` / `bitvector` / `bitlist` adapters, integers of 8-256 bits
  (an enum is its underlying integer), floats;
- width inputs: the record attribute `maxDynamicData`; bounded lists and
  strings; a member `length_bound{.max = N}` on a string or list is the
  bounded form of the member's type, any other bound the member attribute
  `lengthBound = "exact=N"` / `"min=A,max=B"`;
- text forms: member attributes `jsonInt` / `ijsonInt` (`"number"` /
  `"string"`), and the variant-member attribute `ijsonName` (the alternative's
  I-JSON name; without it the member name is used).

**JavaScript / TypeScript.** The `psio` package (`js/`) implements only this
schema-driven path: `PsszSchema.decode(schemaBytes).type("Transfer")` gives
`jsonToPssz`, `psszToJson`, `validate`, `encode`, `decode`, `jsonToValue` and
`valueToJson` (`js/README.md`). `js/src/test/pssz-conformance.test.ts` replays
both shared fixtures from the recorded IR. Text is read by an exact parser
(integers from their token text into `BigInt`), not `JSON.parse`; a non-finite
float has no text in either format, as in Rust. Its explicit-width profiles
(`width: 1 | 2 | 4`) accept `maxDynamicData` and, like the typed
`psio::pssz_<W>` codecs, enforce it on the top-level type only.

Not expressible in the IR, so not available through a schema:

- a custom text form (`PSIO_ADAPTER(..., text_category, ...)`, a member
  `as<Tag>`, Rust `TextForm`) and binary adapters: the IR has the declared
  type;
- `Box<T>` is transparent in the IR, while the typed codecs give a boxed
  member no inferred bound. A record whose bound would be finite but for a
  non-recursive `Box` member gets a narrower width from the IR than from the
  type. (Recursive types are unbounded both ways.) Do not box a member of a
  type that relies on an inferred width; give it an explicit cap;
- C++ `char` elements are bytes 0..255 in the IR; the typed `json` writer
  prints a `std::vector<char>` element with the platform's `char` sign.

No JSON form through a schema (an error, not a guess), because the typed
text codecs have none to agree with: integers wider than 64 bits (their text
form is undecided), `bitvector` / `bitlist`.

**Raw strings.** The IR has one `string` adapter for a UTF-8 string (C++
`utf8_string`, Rust `String`) and a raw string (C++ `std::string`, Rust
`RawString`), and the schema-driven codec treats it as UTF-8. A raw string
holding bytes that are not UTF-8 is therefore **rejected** by
`validate_pssz` and by `pssz_to_json` (a status; no text is produced),
although the typed `validate` accepts it. The typed text writers refuse the
same value ("string is not valid UTF-8"), so no text is lost, but schema
validation is stricter than the type here. The fixture class
`string-raw-non-utf8` pins this.

`dynamic_json.hpp` (`ijson_encode_dynamic` / `ijson_decode_dynamic`, over
the older runtime `psio::schema`) is unchanged apart from the 64-bit integer
rule; it has no variant or tuple and its reader is unchecked. New code
should use the Schema IR path above. `view_to_json.hpp` renders a schemaless
pjson view and has no type information.

## Open

- **128- and 256-bit integers.** `ext_int.hpp` registers a text adapter for
  `uint128` / `int128` / `uint256` that writes `"0x..."` lowercase hex, and a
  text adapter applies in `ijson` exactly as in `json`. That conflicts with
  writing wide integers as decimal strings; the adapter form stands in both
  formats until decided. Rust has no JSON form for `u128` / `i128` /
  `Uint256`.
- Bit types (`psio::bitvector` / `bitlist`, Rust `Bitvector` / `Bitlist`)
  have no JSON form in either language; one has to be chosen before either
  can implement it.
- Rust `validate` materializes the value and drops it.
