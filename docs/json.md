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

`ijson` differs in exactly four ways, with no per-type annotation:

| Shape | `json` | `ijson` |
|---|---|---|
| 64-bit integers (`u64`, `i64`) | `18446744073709551615` | `7`, `"18446744073709551615"` (see below) |
| 128- and 256-bit integers | `340282366920938463463374607431768211455` | `"340282366920938463463374607431768211455"` (always a string) |
| byte sequence | `[1,255]` | `"01FF"` (uppercase on output) |
| variant | `[3,{...}]` | `{"Inner":{...}}` |

**64-bit integers under `ijson`.** A value whose magnitude is at most
2^53 - 1 (the range a JavaScript number holds exactly) is written as a bare
number; a larger magnitude is written as a decimal string. So `0`, `-1` and
`9007199254740991` are bare, `"9007199254740992"`, `"-9007199254740992"`,
`"18446744073709551615"` and `"-9223372036854775808"` are strings. The form
depends on the value, not only on the type: a consumer must accept both.

**128- and 256-bit integers** (C++ `psio::uint128` / `int128` / `uint256`,
Rust `u128` / `i128` / `psio::pssz::Uint256`, Schema IR `Int{128}` / `Int{256}`) are
decimal. `json` writes a bare number (arbitrary precision is legal JSON);
`ijson` writes a decimal string for every value, small ones included, so a
currency amount has one form: `"7"`. Both formats read all of:

- a decimal string, `"340282366920938463463374607431768211455"`;
- a bare decimal number: any in-range value under `json`, a magnitude of at
  most 2^53 - 1 under `ijson` (the 64-bit rule);
- the older `"0x..."` string (`"0xff"`, `"-0x2a"` for a negative `int128`;
  hex digits of either case, at least one). It is accepted for backward
  compatibility only: output is always decimal. To keep writing the hex form
  for one member, annotate it `psio::as<psio::hex_tag>` (C++); the
  `text_category` adapter of these types (the hex form) still serves
  non-JSON text consumers.

A member forced with `json_string` / `ijson_number` etc. is written in and
reads exactly that form.

8/16/32-bit integers, `bool` and floats are numbers in both. Byte sequences
are C++ `std::vector` / `std::array` of `char`, `unsigned char` or
`std::byte` and `psio::byte_array<N>`; Rust `Vec<u8>` and `[u8; N]`.
`int8_t` / `i8` sequences are arrays of numbers.

Common to both formats:

- record: object with the member names, in declaration order;
- `std::vector` / `std::array` / `psio::bounded` / `Vec` / `[T; N]`: array;
- `std::tuple` / Rust tuple: array of the elements in order; the empty tuple
  is `[]`. A `std::pair` is the 2-tuple: `[a, b]`;
- set: array of the elements; map: an object when its key type's text is a
  string by type, else an array of `[key, value]` pairs — see "Maps and
  sets" below;
- optional: `null` or the value;
- a boxed value (C++ `psio::schema_types::Box<T>`, Rust `Box<T>`): the value
  it holds, as in PSSZ. An empty C++ box (a moved-from one) has no text;
- strings: C++ `utf8_string<N>` / Rust `String` and the raw strings C++
  `std::string` / Rust `RawString` are JSON strings;
- enum without payload: the enumerator's **name** as a string (`"active"`)
  when the value is a declared enumerator, the **number** otherwise (see
  "Enums" below);
- time values (`docs/time.md`): a duration and a monotonic time point are
  their tick count (follows the integer rule); a time point is an RFC 3339
  UTC string with exactly its unit's fraction digits,
  `"2026-11-22T10:15:30.123Z"`;
- floats: shortest round-trip decimal as `std::to_chars` writes it
  (`0.1`, `1e+21`, `1e-07`, `-0`, `5.960464477539063e-08`); NaN and the
  infinities have no text (writing one is an error in both formats);
- a type with a custom text form is a JSON string holding that text: C++
  `PSIO_ADAPTER(T, psio::text_category, ...)` or a member `psio::as<Tag>`;
  Rust `TextForm` + `psio::json_text_form!(T)`. Through a schema it is a
  `Custom` node with a handler ("Custom types through a schema" below);
- a nested value (C++ `psio::nested<T>`, Schema IR `Nested`): the text of the
  inner value ("Nested values" below).

### Nested values

A nested value is, in binary, a byte list holding the PSSZ encoding of a `T`
(`docs/pssz-spec.md` §5.8). Its text, in both formats, is **the inner value's
own text** — not hex, not an array of byte numbers:

```
struct Action { u32 method; nested<Transfer> args; }

{"method":9,"args":{"to":"bob","amount":5,"memo":"rent"}}
```

| | rule |
|---|---|
| writing | the bytes are validated as `T` (the compatible rule), decoded, and the value is written by `T`'s rules. Bytes that are not a valid `T` have no text: an error ("nested bytes are not a valid encoding of their type") |
| reading | a `T` is read by `T`'s rules (any member order, unknown keys skipped, a missing optional member, the last of a repeated key), and the nested value is the **canonical** PSSZ encoding of that `T`. A text that reads as a `T` with no encoding (a member outside its length bound, a value over a `maxDynamicData` cap) is rejected ("nested value has no encoding") |
| validating | the same reader: the value is built and encoded even when only validating |
| integer form | a member's `json_string` / `ijson_number` / ... reaches the inner value, as it reaches through an optional. Otherwise `T`'s own forms apply: `nested<u64>` holding 2^60 is `"1152921504606846976"` under `ijson` |
| variant alternative (`ijson`) | `nested_<name of T>`: `{"nested_Transfer":{...}}` |
| nesting limit | a nested value adds no array or object of its own |
| deferred validation (`nested<T, deferred>`) | the same text. The writer validates the bytes in every case |

**Text round trips are value round trips.** If the bytes were written by a
newer program whose inner record has appended optional fields, the text
shows the fields this reader knows, and reading that text back produces the
canonical encoding of those — the unknown fields are gone, as after any
typed read and re-write. A holder that must keep foreign bytes exactly uses
a byte list (hex in `ijson`) instead of a nested value.

A member `length_bound` on a nested member counts the bytes of the inner
encoding and is enforced by both the reader and the writer.

Through a schema the node is `Nested{type}` and the rules are the same:
`pssz_to_json` writes the inner value's text and `json_to_pssz` stores its
canonical encoding. The dynamic value of a nested node (C++
`psio::pssz_ir::Value`) is the inner value itself, so `json_to_value` /
`value_to_json` simply read and write the inner type; a member
`lengthBound`, which counts encoded bytes, is checked where the bytes exist
(`json_to_pssz`, `pssz_to_json`, and the binary codec).

### Enums

An enum is its underlying integer on the wire (PSSZ and every binary
format); the names exist for text.

| | declaration |
|---|---|
| C++ | `enum class Status : std::int32_t { pending = -1, active = 2 };` then `PSIO_REFLECT_ENUM(Status, pending, active)` — one line, any namespace or in a class body. Without it the enum has no names and is written and read as its number. |
| Rust | the derive takes the variant identifiers; `#[psio(name = "active")]` (or `#[serde(rename = "active")]`) renames one; `#[psio(rename_all = "...")]` renames all. |
| Schema IR | `Enum { type, members: [{name, value}] }` over the underlying `Int`. |

Output: `"active"` for a declared value; the number (by the integer rule of
its width and format) for any other value: `3`.

Input, in both formats, for a named enum:

| text | meaning |
|---|---|
| `"active"` | the name |
| `2` | the number |
| `"2"` | the number as a string |
| `"active:2"` | both; they must agree |

An unknown name, a name whose stated number disagrees (`"active:3"`), and a
non-canonical or out-of-range number are errors in every reader. A number
that names no enumerator is a well-formed value (`docs/pssz-spec.md` §4.5
states the rule per layer): the text layer and the schema-driven codecs
accept it everywhere, and a typed reader accepts it where its type can hold
it — always in C++ and JavaScript; in Rust only when the enum has a
catch-all variant (`#[psio(other)] Unknown(i32)`); in Python only with
`@psio.enum(repr, other=True)`. Otherwise it is an error of that typed
reader. When two enumerators share a value, the first declared name is
written.

### Maps and sets

A set is an **array** of its elements. A map is an **object** when the text
of its key type is a JSON string *by type*, and an **array of `[key, value]`
pairs** otherwise. The rule is the same in `json` and `ijson`.

| key type | map text |
|---|---|
| a string kind: C++ `std::string`, `utf8_string<N>`; Rust `String`, `RawString`, bounded strings; Schema IR `string` / `utf8` | `{"apple":1,"pear":3}` |
| a type with a custom text form, of any shape (a record, a variant, a transparent wrapper, ...): C++ `text_category` adapter, Rust `TextForm`, a schema `Custom` presented by its handler | `{"alice":5,"bob":7}`, `{"127.0.0.1:80":4}` |
| a transparent wrapper (`docs/pssz-spec.md` §5.10) of one of the above: its text is its member's. In a schema it is a named alias of the member's type, or a `Custom` whose handler only checks (an invariant, no text form) | `{"apple":1}` |
| anything else — integers, enums, time values, byte sequences, tuples, records, floats, ... | `[[1,"one"],[256,"x"]]` |

A key that merely *happens* to be written as a string (an enumerator name, a
time point, a 64-bit integer above 2^53 under `ijson`, hex bytes) is not an
object key: the form of a map depends on the key type alone, never on the
values.

- **Writers** emit the entries in canonical order: ascending key bytes of
  the key (`docs/pssz-spec.md` §5.9), the order of the binary form. That is
  not the order of the key texts (`{"9.5":…,"10.0":…}` for a version key
  `major.minor`; an account-name key orders by its number). A value with two
  entries of equal key bytes has no text (`json: map has duplicate keys` /
  `json: set has duplicate elements`).
- **Readers** accept the entries in any order, and reject a repeated key or
  element (`json: duplicate map key`, `json: duplicate set element`).
  Unlike a record's member names, where a repeated name is read again and
  the last one wins, a map's keys are data (and RFC 7493 forbids duplicate
  names). Two keys are the same when their key bytes are equal, whatever
  their texts: `[["5","a"],[5,"b"]]` under `ijson`, `[["red",1],[0,2]]` for
  an enum key, `{"a":1,"a":2}` and `[0.0,-0.0]` are all duplicates.
- An object key is read exactly as the same text would be read as a string
  of the key type: I-JSON refuses a noncharacter, a `utf8_string<N>` key its
  bound, a custom text form its own `validate` — and a key type with an
  invariant (`docs/pssz-spec.md` §18) its invariant, with the invariant's
  own message, as for a value read anywhere else. Schema-driven: the
  handler's `check` runs on a key read from, or written as, a member name.
- The entries of the pairs form are tuples: `[k, v]` with exactly two
  elements. An object where the pairs form is expected, or the reverse, is
  an error.
- A set of bytes (`set<u8>`) is an array of numbers in both formats: a set
  is not a byte string.
- A map or set is one nesting level (64 at most), like any array or object.
- A member's integer form (`json_string`, `ijson_number`, ...) reaches the
  elements of a set, as it reaches the elements of a list. The key and the
  value of a map entry take the natural form: an entry is a tuple.
- A C++ container whose comparator or equality merges keys the encoding
  distinguishes cannot hold what was read: `decode` reports `json: map
  container merged distinct keys`.

### Method arguments

The arguments of a method are, in binary, the tuple of its parameters
(`docs/pssz-spec.md` §16): positional, and that does not change. In text an
argument list has two forms, and which one is *written* depends on whether
the declaration has parameter names:

| the method declares | written | read |
|---|---|---|
| a name for every parameter | an **object** keyed by parameter name, members in declaration order: `{"to":"bob","amount":5,"memo":null}` | that object by the record rule — members in any order, the last of a repeated key, unknown keys skipped, a missing optional argument is None, any other missing argument an error — **or** the positional array `["bob",5,null]` with exactly one element per parameter |
| no names (or not for every parameter) | the positional **array**: `[1,2]` | the array only |

Named arguments are for people and for tools; the array is what a
dispatcher that only knows positions writes. Neither is how a call is
dispatched (calls are PSSZ), so a reader takes both. A method with no
parameters is `{}` (and reads `[]` too).

In the Schema IR an argument list is the `Object` of its parameters with the
attribute `tuple`: `"named"` (the members carry the parameter names) or
`"positional"` (the members are named `"0"`, `"1"`, …). The attribute
changes no byte — an `Object` is positional on the wire like every record —
and tells a schema-driven codec which of the two rows above applies. An
`Object` without it is a record: an object in text, never an array.

The parameters of a function in `Schema::interfaces` (`Func::params`) carry
the same names: the parameter names when the method names every parameter,
`"0"`, `"1"`, … otherwise — never an empty or a repeated name. C++
`psio::pssz_ir::argument_list(func)` gives the argument list of a `Func` as
that `Object` (with the function's `maxFields` / `maxDynamicData`), so a call
is packed or printed from `Schema::interfaces` alone:
`json_to_pssz(schema, argument_list(f), psio::ijson{}, text, bytes)`. An
interface may have no functions (`PSIO_INTERFACE` on a reflected type that
lists no method): its `funcs` is empty.

C++: `psio::reflect<T>::method_arg_tuple<I>` (a `psio::annotated_tuple`) is
the argument list, named when `method(name, p1, p2, ...)` lists every
parameter. Rust: `args::<method>` (parameters always have names).
JavaScript: `argumentTuple({...})`. Python: `psio.arguments(...)`. The
shared rows are `conformance/interface-vectors.json`.

### Variant alternative names

C++ uses `get_type_name` of the alternative: a record's reflected name, and
`bool int8 uint8 int16 uint16 int32 uint32 int64 uint64 float32 double
string` for primitives, `T[]` for a vector, `T[N]` for an array, `T?` for an
optional, `tuple_A_B` (also for a `std::pair<A, B>`), `map_K_V` for a map and
`set_K` for a set (whatever container holds them). Alternatives must have distinct names (checked at
compile time). In Rust the name is the enum variant's identifier unless
`#[psio(name = "uint32")]` renames it; use that to pair with a C++ variant.

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

### Transparent types, custom text, invariants

- A **transparent type** (`docs/pssz-spec.md` §5.10) is its member's text:
  `Id{42}` is `42`, a wrapped byte list is the array / hex string. A record
  member of a transparent integer type takes the member's integer form.
  Under ijson a variant names a transparent alternative by the wrapper's own
  type name.
- A **text adapter** (C++ `PSIO_ADAPTER(T, psio::text_category, Impl)`)
  gives a type of any shape — record, transparent wrapper, enum,
  `std::variant`, `std::tuple`, list — one JSON string as its text, in both
  formats; the adapter replaces the text the shape would have, and the
  shape's own text is then rejected. An adapter on a `std::variant` or
  `std::tuple` applies to that exact type wherever it is used; a distinct
  identity is a transparent wrapper around it.
- A **type invariant** (`docs/pssz-spec.md` §18) is enforced by every
  writer (`encode`, `encode_json_checked`) and every reader (`validate`,
  `validate_strict`, `validate_json`, `decode`, `decode_json_checked`,
  `make_boxed`): a value that violates it has no text, and text that reads
  as such a value is rejected with the invariant's own status. A text
  adapter's `validate` runs on the text first; the invariant then runs on
  the value read. A reader that only validates materialises the values of
  types that have an invariant, and only those. Every syntax layer behaves
  identically. Unknown keys are skipped without being read as any type.
- **What PSIO requires of the code a type supplies.** An invariant check
  and a text adapter's `validate` run on untrusted input, inside entry
  points that promise a status and never a throw or an abort, in builds
  with and without exceptions. A check is `noexcept` and returns
  `psio::codec_status` (both enforced at compile time by `invariant(fn)` /
  `PSIO_INVARIANT`); an adapter's `validate(span)` returns one. Neither may
  throw or abort on any input: a check built on a library that throws or
  aborts on malformed data (a DER or PEM parser) must catch that, or test
  the structure itself first. An adapter's `decode` is called by the
  checking readers only on text its `validate` accepted, and may rely on
  that. The status is passed through unchanged; a message that names the
  offending value is made with `psio::codec_fail_owned(text, 0, "Type")`
  (`psio/error.hpp`), a fixed one with `psio::codec_fail("...", 0, "Type")`.
  That holds for an invariant's status (and a schema handler's `check`),
  which is kept whole; the message of an adapter's `validate` must be a
  literal, because the text readers keep only its view.

## Reading

One typed reader per language defines the accepted set. In C++ it backs
`validate`, `validate_strict`, `decode`, `make_boxed` and
`decode_json_checked`; in Rust `psio::from_json` / `from_ijson`, `validate_json` /
`validate_ijson` and the tag-generic `psio::json::decode` / `validate`.

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
  reads as zero). `NaN`, `Infinity`, `-Infinity`, any other spelling of a
  non-finite value, and strings are rejected.
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
`codec_exception` (aborts when exceptions are disabled). It runs the same
reader as `validate` and `decode_json_checked` and accepts exactly the same
texts, in both formats: a missing member that is not optional is an error
here too. (`psio::json` used to leave such a member value-initialized in
this one entry point; a type that wants a member to be omissible declares
it `std::optional`.)

## Member lookup

A key is resolved without scanning the member list:

1. **Expected order first.** Writers emit members in declaration order, so
   the reader compares each incoming key with the next member it expects (a
   compare against a constant) before any lookup. A member the text leaves
   out costs one failed compare.
2. **Perfect hash otherwise.** A key that is out of order, repeated or
   unknown is looked up through a table generated at compile time from the
   member names (`psio/name_lookup.hpp`, C++ `reflect<T>::find_index`): a
   fixed number of loads and multiplies and exactly one confirming compare,
   whatever the member count, with no run-time initialization. An `ijson`
   variant's alternative name is looked up the same way. (Rust: see "Rust"
   under "Two layers" below.)

Neither step changes what is accepted: any order, unknown keys skipped,
repeated keys read again in a single pass with the last one winning.

The schema-driven path (below) knows its member names only at run time. It
applies the same expected-order compare, and a `psio::pssz_ir::JsonNames`
built once for a schema (`JsonNames names(schema);`, passed as the last
argument of `json_to_value` / `json_to_pssz`) turns its fallback from a
member scan into a hash probe for every decode that reuses it.

`reflect<T>` exposes the same tables directly: `index_of(name)` /
`find_index(name)`, `index_of_field_number(n)` (a direct table) and
`index_of_hash<Hash>(key)` for a 64-bit key the caller computes with its own
constexpr hash; two members with the same key under that hash are a compile
error (`cpp/tests/name_lookup_collision_fail.cpp`, run by ctest as a
must-not-compile check).

## Length bounds

A bounded wrapper (C++ `psio::bounded<T, N>`, `utf8_string<N>`; Rust
`BoundedList`, `BoundedString`, `BoundedRawString`) and a C++ member
annotated `length_bound{.max/.exact/.min}` (looking through `optional`) are
enforced on both sides: the reader rejects a string or list outside its
bound (`validate`, `decode_json_checked`, `decode`), and the writer refuses
to produce one. The count is bytes for a string, elements for a list or
set, and entries for a map.

## Writing can fail

A value with no JSON text is an error, never lossy or invalid output:

- a string or list outside its length bound (above);
- a raw string (C++ `std::string`, Rust `RawString`) whose bytes are not
  valid UTF-8, in both formats;
- a non-finite float (NaN, +Infinity, -Infinity), in both formats and in
  every language: there is no JSON text for it;
- under `ijson`, a string holding a noncharacter.

C++ `psio::encode` fails fast (`codec_exception`, or abort without
exceptions); `psio::encode_json_checked(fmt, value, out)` returns a
`codec_status` and leaves `out` unchanged on failure. Rust returns `Err`;
the JS package throws.

## C++: views, documents built by hand, sinks

Beyond "one typed value to or from a `std::string`", `psio/json.hpp` has
what code that builds or picks apart documents by hand needs. Everything in
this section is the same for `psio::json` and `psio::ijson`.

### Text and bytes from non-owning sources

A writer takes text and bytes from where they are:

| C++ | text |
|---|---|
| `std::string_view`, `const char*`, a string literal / `char[N]` (its characters up to the terminator) | a string, by the rules of `std::string` (UTF-8; no noncharacter under `ijson`). A null `const char*` is an error, "json: null C string" |
| `std::span<T>` (const or not, any extent) | the list of its elements, `[1,2,3]` |
| `std::span` of `char` / `unsigned char` / `std::byte` | the byte sequence it views: `"01AB"` under `ijson`, `[1,171]` under `json` |

```cpp
psio::to_ijson(std::string_view{"x"}, out);        // "x"
psio::to_ijson("literal");                         // "literal"
psio::to_ijson(std::tuple{name_view, "n", 3});     // ["...","n",3]
psio::to_ijson(std::span<const unsigned char>{b}); // "01AB"
```

They are for writers only: text has no non-owning reader, so a record with
such a member can be written and not read.

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
   `finish`. Two optional fast paths a cursor may add: `scalar` (a number's
   characters, unchecked, for a caller whose own parse validates them) and
   `real` (a number as its exact `double`).
2. **Types** — a walk that checks the cursor's events against the type:
   member names, arity, integer range and form, hex digits, variant names,
   length bounds, missing members. It holds no text positions and does no
   tokenizing.

A cursor is forward-only and single-pass (values consumed once, in document
order; a string view is valid until the next call). The walks need no random
access and no second pass: record members are stored as they arrive (a
repeated key is read again and replaces the earlier value), and a variant's
selector precedes its value in both text forms (`[index, value]`,
`{"Name": value}`).

In C++ both walks are written against the cursor: the typed readers of
`json.hpp` (`decode`, `validate`, `decode_json_checked`, ...) and the
schema-driven walk of `pssz_ir_json.hpp`. Three cursors exist:

| Cursor | Syntax layer | Built when |
|---|---|---|
| `json_text_cursor` | the hand-written lexer (`detail/json_syntax.hpp`): no padding, no copy, no allocation except for escaped strings; builds for a WASI guest with exceptions and RTTI off | always |
| `simdjson_index_cursor` | simdjson stage 1 (one SIMD pass: token index, whole-document UTF-8 check, control characters in strings) + PSIO's own walk over the index | `PSIO_HAVE_SIMDJSON` |
| `simdjson_cursor` | simdjson's on-demand API | `PSIO_HAVE_SIMDJSON` |

Rust has the hand-written `Reader` only (see "Rust" below).

### What every syntax layer does

The type rules rest on these, and all three cursors implement them; the
differential tests (`cpp/tests/json_parser_tests.cpp`) and the shared fixture
run every text through all of them and require the same outcome:

- reports, for a string, whether it was written with escapes (integer and
  hex strings must be written literally);
- rejects Unicode noncharacters in `ijson` strings and keys, also inside
  skipped values; rejects raw control characters, unpaired surrogate
  escapes and invalid UTF-8;
- hands a number over as text. Integers are parsed by PSIO, exactly: the
  full `u64` range, bare big integers in strict `json`, the "bare only up to
  2^53 - 1" rule of `ijson`; `-0`, leading zeros, fractions and exponents
  are told apart. simdjson's 64-bit accessors are not used; only a `double`
  target takes simdjson's number parser (exact, same grammar);
- checks a skipped value like a read one;
- limits nesting to `kMaxValidationDepth` (64), on skipped values too;
- never throws or aborts.

Where simdjson's on-demand API is more lenient than that, the cursor adds
the check itself: on-demand skips a value by counting brackets (so `[1 2]`
or `nul` inside an unknown key would pass), does not look at what follows a
string's closing quote inside the token (`"a"x`), does not require a
consumed number or literal to be well-formed unless its accessor is called,
and skips a leading UTF-8 byte order mark. None of these reach the type
layer: `simdjson_cursor` walks skipped values, checks the token tail, parses
or compares every scalar, and rejects a BOM.

Repeated keys need nothing from the syntax layer (last one wins).

### Choosing the syntax layer

`psio::json_parser` selects it per call; every choice accepts the same texts
and reads the same value:

```cpp
psio::decode_json_checked(psio::ijson{}, text, out);                              // automatic
psio::decode_json_checked(psio::ijson{}, text, out, psio::json_parser::text);     // hand-written
psio::decode_json_checked(psio::ijson{}, text, out, psio::json_parser::simdjson_index);
psio::decode_json_checked(psio::ijson{}, text, out, psio::json_parser::simdjson); // on-demand
psio::validate_json<T>(psio::ijson{}, text, parser);
```

`automatic` (also what `decode`, `validate`, `validate_strict` and
`make_boxed` use) is the fastest measured: `simdjson_index_cursor` for a
document of at least 64 bytes (`PSIO_JSON_SIMDJSON_MIN_SIZE`) whose root is
an object or array, the hand-written lexer otherwise — small documents,
root scalars, a read started from inside another read on the same thread,
and every build without simdjson.

**Padding.** simdjson reads up to `psio::json_padding` (64) bytes past the
document. A `std::span<const char>` is copied once into a padded per-thread
buffer. A caller that owns padded storage passes `psio::padded_json` and the
document is parsed in place:

```cpp
std::string text = ...;
psio::decode_json_checked(psio::ijson{}, psio::padded_json::of(text), out);  // grows capacity, not size
psio::decode_json_checked(psio::ijson{}, psio::padded_json::assume(span), out);  // caller vouches
```

**Build.** PSIO fetches simdjson itself, pinned to release 5.2.0 by archive
hash (`cpp/CMakeLists.txt`); a system install is not looked up.
`-DFETCHCONTENT_SOURCE_DIR_SIMDJSON=<dir>` substitutes a local source tree,
and a parent project's `simdjson::simdjson` target is used as is.
`-DPSIO_ENABLE_SIMDJSON=OFF` (the default for WASI) builds with
`PSIO_HAVE_SIMDJSON=0`: the hand-written lexer alone, same accepted set.

### Writing

The writer formats into the tail of the output string through a raw pointer
(one capacity check per value, the size set once at the end). Each member
key is a literal built at compile time — separator, quoted and escaped name,
colon (`,"name":`). A value whose text has a small constant bound (numbers,
bool, and optionals, fixed arrays, tuples and records of them) reserves once,
as does a list of such elements. Integers and floats are formatted in place
with `std::to_chars`, hex through a 256-entry pair table, strings eight
plain bytes at a time, and the `ijson` 2^53 test is one unsigned compare.

### Measurements

`cpp/benchmarks/bench_json_text.cpp` (arm64, Apple M-series, clang 22 -O3;
minimum of repeated short trials; the machine was running other work, so
treat differences under ~10% as noise). ns per document, MB/s in brackets.
"before" is the same benchmark built against the previous reader
(hand-written, linear member scan, `std::string` appends).

**Decode** (typed; `text`, `index` and `on-demand` all use the perfect-hash lookup and the expected-order fast path)

| shape | format | bytes | before | text | on-demand | index | index, padded |
|---|---|---:|---:|---:|---:|---:|---:|
| small | json | 92 | 90 ns (1024) | 92 ns (999) | 132 ns (695) | 69 ns (1335) | 64 ns (1444) |
| small | ijson | 92 | 92 ns (1003) | 93 ns (992) | 145 ns (634) | 77 ns (1201) | 72 ns (1280) |
| medium | json | 2492 | 2814 ns (886) | 2809 ns (887) | 2373 ns (1050) | 1999 ns (1247) | 1933 ns (1289) |
| medium | ijson | 2499 | 2778 ns (900) | 2863 ns (873) | 2671 ns (935) | 2290 ns (1091) | 2286 ns (1093) |
| large | json | 884816 | 1.00 ms (882) | 971.1 µs (911) | 811.4 µs (1090) | 675.4 µs (1310) | 656.4 µs (1348) |
| large | ijson | 884816 | 1.00 ms (882) | 981.6 µs (901) | 901.2 µs (982) | 753.2 µs (1175) | 737.2 µs (1200) |
| wideint | json | 932 | 1220 ns (764) | 942 ns (989) | 878 ns (1062) | 705 ns (1322) | 694 ns (1344) |
| wideint | ijson | 1006 | 1131 ns (890) | 839 ns (1199) | 1058 ns (951) | 880 ns (1143) | 851 ns (1183) |
| wide200 | json | 4627 | 10.4 µs (446) | 2742 ns (1687) | 2328 ns (1988) | 1996 ns (2318) | 1971 ns (2348) |
| wide200 | ijson | 4627 | 10.6 µs (436) | 2780 ns (1665) | 2778 ns (1665) | 2310 ns (2003) | 2283 ns (2026) |
| wide200r | json | 4627 | 10.4 µs (445) | 5333 ns (868) | 5417 ns (854) | 4581 ns (1010) | 4473 ns (1034) |
| wide200r | ijson | 4627 | 10.6 µs (435) | 5378 ns (860) | 5560 ns (832) | 4979 ns (929) | 4834 ns (957) |

**Encode**

| shape | format | bytes | before | after |
|---|---|---:|---:|---:|
| small | json | 92 | 104 ns (881) | 44 ns (2110) |
| small | ijson | 92 | 105 ns (877) | 43 ns (2160) |
| medium | json | 2492 | 2751 ns (906) | 684 ns (3643) |
| medium | ijson | 2499 | 2734 ns (914) | 699 ns (3574) |
| large | json | 884816 | 940.5 µs (941) | 350.0 µs (2528) |
| large | ijson | 884816 | 952.4 µs (929) | 340.6 µs (2598) |
| wideint | json | 932 | 691 ns (1350) | 450 ns (2071) |
| wideint | ijson | 1006 | 715 ns (1407) | 454 ns (2214) |
| wide200 | json | 4627 | 3647 ns (1269) | 528 ns (8762) |
| wide200 | ijson | 4627 | 3759 ns (1231) | 533 ns (8675) |

Shapes: `small` a flat 5-member record; `medium` a nested record with strings,
a variant, optionals and a 20-element vector of records; `large` an array of
10 000 records; `wideint` u64 beyond 2^53 and u128 values; `wide200` a
200-member record with keys in declared order, `wide200r` the same in reversed
order. `index, padded` is `padded_json` input (no copy); the other simdjson
columns include the copy into a padded buffer.

Read: the index walk is the fastest reader on every shape of 64 bytes or
more except one and is the default there. The exception is the `ijson`
wideint shape (mostly quoted integers and hex strings), where the
hand-written lexer is about 5% ahead; the default is chosen by size, not by
content, so that shape also goes to the index walk. Below 64 bytes the
hand-written lexer is the default because it wins (a 28-byte record: 35 ns
against 48 ns; a 16-byte one: 21 ns against 36 ns). On-demand is not the
fastest anywhere. Against the previous reader the hand-written lexer alone
is unchanged on 5- to 10-member records (member lookup was not their cost),
3.8x faster on a 200-member record in declared order and 1.9x in reversed
order, where every key after the first goes through the perfect hash.

**WASI.** simdjson's single-header build (`singleheader/simdjson.cpp`)
compiles for `wasm32-wasi` with `-fno-exceptions -fno-rtti`
(wasi-sdk 29, clang 21), and a PSIO program using all three cursors links
and runs correctly under wasmtime. simdjson has no WebAssembly kernel: it
runs its scalar `fallback` implementation there, so it buys no speed, and it
adds about 200 kB of object code. A guest would need that translation unit
built for the guest target and `PSIO_HAVE_SIMDJSON=1`; nothing else in PSIO
changes. This is not wired into the build: WASI configures with
`PSIO_ENABLE_SIMDJSON=OFF` and reads with the hand-written lexer.

### Rust

The Rust derive matches a key with a `match` on the member names (rustc
compiles it to a switch on the key's length followed by compares). A record
of more than 64 members first compares each key with the member after the
last one read. Measured (`rust/psio/benches/json_text_bench.rs`, same
machine, before -> after): unchanged on the 5-member record (116 -> 112 ns),
the nested record (3.42 -> 3.34 µs) and the 10 000-record array
(1.28 -> 1.27 ms); 10% faster on the 200-member record in declared order
(5.22 -> 4.70 µs); unchanged in reversed order (5.08 -> 5.07 µs). Two
variants were measured and dropped: the expected-order compare on small
records (8% slower than the plain match) and a perfect hash built by the
derive for the fallback (no faster than the match, slower under `ijson`).

Candidate syntax layers, raw parse speed on the same inputs (no typed value
built), against the typed PSIO read:

| input | PSIO typed read | `serde_json` (`IgnoredAny`) | `serde_json::Value` | `simd-json` tape |
|---|---:|---:|---:|---:|
| small, 92 B | 112 ns (820 MB/s) | 74 ns (1238) | 432 ns (213) | 174 ns (528) |
| medium, 2.5 kB | 3.34 µs (745) | 1.94 µs (1285) | 11.4 µs (218) | 1.26 µs (1985) |
| large, 885 kB | 1.27 ms (699) | 0.80 ms (1104) | 4.53 ms (195) | 0.51 ms (1742) |
| wide200, 3.6 kB | 4.70 µs (760) | 2.55 µs (1397) | 13.5 µs (264) | 1.60 µs (2234) |

`simd-json`'s tape is 1.6x to 3x the speed of the whole typed read above
100 bytes and slower below it, so a cursor over it could at best approach
the C++ result (the typed work stays). Neither crate was put under
`JsonCursor` in this pass: the typed derive reads through `Reader` directly,
not through the trait, and `simd-json` needs a mutable, owned input buffer.
`simd-json` is a dev-dependency of the bench only; the library's
dependencies are unchanged.

#### Rust: derive attributes, variants, serde

`#[derive(psio::JsonText)]` reads `#[psio(...)]` (what describes the type
for every format: `name`, `rename_all`, `transparent`, `invariant`, `skip`,
`other`, and the bounds `max` / `element` / `keys` / `values` / `at`, which
the writer and the reader enforce) and `#[json(...)]` (the integer forms
`json_number`, `json_string`, `ijson_number`, `ijson_string`, on a field or
as the type's default).

An enum with a variant that holds fields is a variant; the text of an
alternative is the text of its payload, the payload `PsszPack` encodes and
`ToPsszSchema` describes:

| Rust variant | payload | IJson | Json | C++ alternative |
|---|---|---|---|---|
| `One(u32)` | that value | `{"One":7}` | `[1,7]` | `T` |
| `Empty` | the empty record | `{"Empty":{}}` | `[0,{}]` | an empty reflected record |
| `Rect { width: u32, height: u32 }` | a record | `{"Rect":{"width":3,"height":4}}` | `[3,{...}]` | a reflected record |
| `Pair(u64, String)` | their tuple | `{"Pair":["1","p"]}` | `[2,[1,"p"]]` | `std::tuple<...>` |

A field-less enum needs `#[repr(int)]` (it is its enumerator's name) or
`#[psio(variant)]`; a union is a compile error.

**serde.** `#[derive(psio::Serde)]` gives a `JsonText` type
`serde::Serialize` / `Deserialize` whose behaviour is this document's IJson
rules (`psio::serde_bridge`): reading with `serde_json` accepts and rejects
exactly what `from_ijson` does, and writing gives the IJson text except for
two spellings serde's data model does not carry (a non-integer float and
the escapes of U+0008 / U+000C). The PSIO derives also read `#[serde(...)]`
on the type they describe (`psio::serde_attrs` has every attribute):

| serde attribute | the PSIO derives |
|---|---|
| `transparent`, `rename`, `rename_all`, `rename_all_fields`, `skip` | honoured, as the `#[psio(...)]` key of the same meaning |
| `default` on an `Option` member, `skip_serializing_if = "Option::is_none"` | accepted: PSIO's own rule (a missing optional member is `None`; `None` is written `null`) |
| `default` elsewhere, `default = "path"`, `flatten`, `alias`, `deny_unknown_fields`, `tag` / `content` / `untagged`, `with` / `serialize_with` / `deserialize_with`, `from` / `try_from` / `into`, one-directional `rename` or `skip_*` | a compile error on the derives that describe text (`JsonText`, `Serde`, `ToPsszSchema`): PSIO's text has one form, defined by the member's type |
| `bound`, `borrow`, `crate`, `getter`, `expecting` | ignored |

`psio` enables one `serde_json` feature, `raw_value`, which adds a type and
changes nothing else: a program that links `psio` has the `serde_json` it
would have without it (no `arbitrary_precision`, `preserve_order` or
`float_roundtrip`; `rust/psio-serde-json-stock` asserts it). The
consequence is confined to reading a PSIO type out of a `serde_json::Value`
or through the buffer serde's derive puts in front of untagged /
internally tagged enums and `flatten`: there a number arrives as the
`u64` / `i64` / `f64` `serde_json` parsed, as from any other format.

## Schema-driven path (no C++ / Rust type)

A consumer that has only a Schema IR (`schema_ir.hpp`, Rust
`psio::pssz::schema_ir`) — a CLI, an HTTP "pack this action" endpoint, an
indexer — converts and validates with:

| | C++ (`psio/pssz_ir_json.hpp`, namespace `psio::pssz_ir`) | Rust (`psio::pssz::dynamic`) | JS (`PsszSchema`) |
|---|---|---|---|
| text → PSSZ | `json_to_pssz(schema, root, psio::ijson{}, text, bytes)` | `json_to_pssz::<IJson>(&schema, &root, text)` | `schema.type(root).jsonToPssz(text, 'ijson')` |
| PSSZ → text | `pssz_to_json(schema, root, psio::ijson{}, bytes, text, &has_unknown)` | `pssz_to_json::<IJson>(&schema, &root, bytes)` | `schema.type(root).psszToJson(bytes, 'ijson')` |
| validate PSSZ | `validate_pssz(schema, root, bytes, &has_unknown)` | `validate_pssz(&schema, &root, bytes)` | `schema.type(root).validate(bytes)` |
| ... with a handler registry ("Custom types through a schema" below) | a trailing `const CustomHandlers*` on every entry point: `validate_pssz(schema, root, bytes, &has_unknown, &handlers)`, `pssz_to_json(..., &has_unknown, &handlers)`, `json_to_pssz(..., names, parser, &handlers)`, `decode_auto` / `encode_auto(..., &handlers)` | the `_with` forms: `validate_pssz_with(&schema, &root, bytes, &handlers)`, `pssz_to_json_with`, `json_to_pssz_with`, `json_to_value_with`, `value_to_json_with`; binary: `encode_with` / `decode_exact_with` / `decode_compatible_with` / `validate_with` / `to_key_with(..., Options::handlers(&handlers))` | bound to the schema: `PsszSchema.decode(irBytes, { handlers, requireHandlers })`, `schema.withHandlers(handlers, { requireHandlers })`, or per type `schema.type(root, { handlers })`; every method of the type then uses them |
| ... leaving nested bytes unexamined | `validate_pssz(schema, root, bytes, &has_unknown, nullptr, nested_bytes::opaque)` (the registry, then the option; the same trailing pair on `decode_auto` / `encode_auto`) | `validate_with(&schema, &root, bytes, Options::opaque_nested())`, or `Options { handlers: Some(&h), nested: NestedBytes::Opaque }` on any `_with` binary entry point | `schema.type(root, { nestedBytes: 'opaque' })` (or the same option on the schema) |

With nested bytes opaque the dynamic value of a `Nested` node is its bytes
(C++ `Bytes`, Rust `Value::Bytes`, JS `Uint8Array`) instead of the inner
value, read and written as they are: the behaviour of a typed
`nested<T, deferred>` member, and what a forwarder that must keep foreign
bytes exactly uses. The option exists on the binary entry points; the text
entry points always open a nested value (its text is the inner value's).

The C++ text reader runs on the same syntax layers as the typed readers
(trailing `json_parser` argument; the default is the same automatic choice).

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

**IR version.** The IR root (`Schema`) carries `ir_version`, the version
of the IR's meaning the schema is written in (C++ `psio::schema_ir_version`,
Rust `IR_VERSION`, JS `IR_VERSION`; currently 1: boxes are marked `box`,
UTF-8 strings `utf8`, and `string` is a raw string). The field was appended
to the record, so the IR's own encoding is backward compatible: a schema
written before it existed still decodes, with no version. Such a schema
predates the markers — read as version 1 it would derive other offset
widths and skip UTF-8 validation — so every schema-driven codec in all
three languages refuses it ("no ir_version marker ... regenerate it"), as it
refuses a version newer than its own, instead of guessing. A schema built
in code (`SchemaBuilder`, a default-constructed `Schema`, Rust
`Schema::default()`, a JS map of named types) is in the current version; a
caller that knows what an unversioned schema means states it by setting the
field.

**Malformed schemas.** A schema is checked before anything is read or
written with it, and all three languages refuse the same ones with an
error (fixture rows `schema_rejects`; their well-formed neighbours are
`schema_accepts`):

- an adapter — a `Custom` id with a binary meaning (`bool`, `char`,
  `string`, `utf8`, `hex`, `bitvector`, `bitlist`) or a time id — sits
  **directly** on its wire type, and an `Enum` directly on an integer:
  named aliases are followed, but an `Enum`, a `box` or a text presentation
  in that place (an enum over an enum, a boxed byte container under a string
  adapter, a string of boxed or enumerated bytes) is malformed. The reverse
  is fine: a `box`, a presentation or an alias around any well-formed type;
- a reserved time spelling that is not a time id, and a time id over
  anything but an 8/16/32/64-bit integer (`docs/time.md`);
- unresolved or cyclic aliases, duplicate members, unknown attributes, and
  types with no PSSZ form, as before.

The IR carries what that needs (the C++ `SchemaBuilder` and the Rust
`ToPsszSchema` derive write it):

- shape: records (`definitionWillNotChange` for fixed layout), tuples,
  variants, options, lists, arrays, bounded lists, the `string` (raw bytes)
  / `utf8` (UTF-8) / `hex` / `bool` / `char` / `bitvector` / `bitlist`
  adapters, `box` (a boxed value: the inner type, with no inferred size
  bound), `Nested` (a byte list holding the PSSZ encoding of its inner
  type: "Nested values" above), integers of 8-256 bits,
  enums (`Enum`: the underlying integer plus the enumerator names and
  values; a C++ enum without `PSIO_REFLECT_ENUM` is just its integer),
  floats;
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
(integers from their token text into `BigInt`), not `JSON.parse`.

**Explicit-width profiles** (C++ `encode<W>` / `decode_exact<W>` /
`decode_compatible<W>`, Rust and JS `width: 1 | 2 | 4`) do what the typed
`psio::pssz_<W>` codecs do with `maxDynamicData`: the type is accepted, the
one inherited width is kept, and the cap of the **top-level** type bounds
the whole encoding (over it: an error on write and on read). A nested
record's cap is not looked up. The automatic profile derives widths from
caps and enforces every record's.

**Width fidelity.** The offset width a schema-only reader derives equals the
typed codec's for every type; both fixtures record the typed width of each
type (`auto_width`) and all three languages assert it. In particular a boxed
value (C++ `psio::schema_types::Box<T>`, Rust `Box<T>`) has no inferred size
bound in the typed codecs, and the IR records the box (`Custom{T, "box"}`)
so that the schema derives the same: `record{id: u32, leaf: box<Leaf>}`
uses 4-byte offsets where the same record without the box uses 1, unless it
carries an explicit cap. A schema written before the marker existed has the
boxed type inline and derives the narrower width: re-export it.

### Custom types through a schema

A **custom type** is a type with an identity of its own beyond its wire
form: a custom text form (an account name over a `u64`, `major.minor` over
a record, `"127.0.0.1:8080"` over a variant), an invariant
(`docs/pssz-spec.md` §18), or both. In the Schema IR it is the `Custom`
node, **whatever the type's shape** — record, transparent wrapper, enum,
variant, tuple, list:

```
Custom { type: <the wire type>, id: "<the type's id>" }
```

The wire type says exactly what the bytes are; the id names the type.
A transparent type with neither a text form nor an invariant is not custom:
it is a named alias of its member's type.

**Reserved ids.** PSIO's own types use `Custom` nodes too, and the
schema-driven codecs read a fixed meaning into each of their ids:

| id | on | meaning |
|---|---|---|
| `bool` | `Int{1}` | a boolean |
| `char` | `Int{8}` | a character |
| `string`, `utf8` | a list of `u8` | a raw string, a UTF-8 string |
| `hex` | a list or array of bytes | bytes, written as hex in `ijson` |
| `bitvector`, `bitlist` | an array / a bounded list of `Int{1}` | the bit types |
| `map`, `set` | a list (of 2-tuples for a map) | entries in canonical key order |
| `box` | any type | the type, held indirectly |
| `unknown` | — | a type the schema builder had no node for; not readable |
| `time-point…`, `duration…`, `monotonic…` | an 8/16/32/64-bit `Int` | a time value (`docs/time.md`); the word alone or followed by `-<unit>` |

A custom type cannot take one of these ids — it would be read as the
built-in type. Every language refuses it where the id is declared: C++ at
compile time (`schema_custom_id`: the id of a text adapter, of
`PSIO_INVARIANT`, or the reflected name) and in `CustomHandlers::try_add` /
`add`; Rust in the derives and in `SchemaBuilder::insert`; JavaScript in
`custom()` and when a handler is registered; Python in `custom(...)` and
`register`. A schema in which a reserved id sits on any other wire shape
(`Custom{List, "bool"}`, `Custom{String, "time-point-seconds"}`, a time word
with a unit that does not parse) is malformed, and every schema-driven
reader rejects it. Every other id never changes the bytes or the widths.

**Maps and sets.** `Custom{ List{ Tuple{[K, V]} }, "map" }` and
`Custom{ List{ K }, "set" }` (`BoundedList` in place of `List` under a
maximum count) are a map and a set: the list's entries are in strictly
ascending key order, which the schema-driven codecs check on every read and
produce on every write (a dynamic map value is the sequence of its
`[key, value]` entries, in any order when handed to an encoder). They take no
handler. Their text is that of "Maps and sets" above, decided from the key
type's node: an object when it is a `string` / `utf8` node, or a custom
presentation **whose handler is registered and has a text form** (the key
text is the handler's); the pairs form otherwise. A custom node whose
handler only checks (an invariant) does not decide the form: the node under
it does, so a transparent string with an invariant keys an object with or
without its handler. A custom presentation with no handler falls back,
as everywhere, to the plain form of its wire type — and the map's form
follows it: an account-keyed map is `{"alice":5}` with the account handler
and `[[235814801429757952,5]]` without. A strict registry makes the
missing handler an error instead.

Malformed schemas, refused by every schema-driven entry point in every
language (`schema_rejects` rows of the JSON fixture): `map` anywhere but
directly on a `List` / `BoundedList` whose element is (an alias of) a `Tuple`
of exactly two members; `set` anywhere but directly on a `List` /
`BoundedList`; a key type with no key encoding (`bitlist`, a one-bit
integer, a boxed type — `docs/key-spec.md`). "Directly" allows named
aliases and nothing else: not a `box`, an `Option`, a presentation or
another adapter in between.

**Built-in presentations.** The time ids (`time-point-<unit>`,
`duration-<unit>`, `monotonic-<unit>`, `docs/time.md`) are presentations
PSIO supplies itself in every language: a duration and a monotonic time
point are their tick integer, a time point an RFC 3339 string. They need no
handler, cannot be given one, and are presented also under a strict
registry. `time-point` / `duration` / `monotonic`, alone or followed by
`-`, are reserved: a reserved id that is not a time id, or a time id over
anything but an 8/16/32/64-bit integer, is a malformed schema.

What an id adds is code, which a schema cannot carry. The schema-driven
codecs — the text conversions **and** the binary entry points (validate,
decode, encode, the explicit-width forms, the key encoding) — take a
**handler registry**: id → handler. A handler has up to three parts, each
optional:

| part | role |
|---|---|
| `to_text` / `from_text` (JS `toText` / `fromText`) | the text form, between a value of the wire type and its text. The text is carried as ONE JSON string in both formats, the rule of a typed text adapter. Without them the type's text is its wire type's |
| `check` | the invariant: whether a structurally valid value of the wire type is a value of the type |

In C++ and Rust a handler works on the **PSSZ bytes of the wire type**
(automatic profile: the bytes of the typed codec); in JS on the **decoded JS
value** of the wire type.

With the registry, `check` runs on **every** value of a custom node that
has one, on the binary and the text entry points alike: on reads after the
value's structure (and its text) is accepted, on writes before any byte or
text is returned; nested values first — the bytes of a `Nested` node
included, unless they are opaque — and a value under several ids innermost
first. Its status (JS: its message) is passed through unchanged. Text that
a handler's `from_text` refuses is "json: text is not a value of the custom
type".

A registry is **strict** when it requires a handler for every custom node
(C++ `require_all = true`, Rust `set_strict(true)`, JS
`requireHandlers: true`): a custom node with no handler is then an error,
"no handler registered for a custom type", **on the binary entry points
too**, not only where text is produced. The ids PSIO gives a meaning itself
(the binary-form ids, `box`, the time ids) take no handler and are not
subject to it.

| | emits the node | registers a handler |
|---|---|---|
| C++ | `SchemaBuilder`, for any type with a `text_category` adapter and / or an invariant (`psio::schema_custom_v<T>`). Id (`psio::schema_custom_id<T>()`): the adapter's `static constexpr std::string_view schema_id`, else the id given to `PSIO_INVARIANT`, else the reflected record or enum name. A type with none of these (a `std::variant`, a `std::tuple`) must provide one: a compile error otherwise. An adapter that is not the type's schema presentation says `static constexpr bool schema_custom = false` (the `0x` text of the integers wider than 64 bits). | `psio::pssz_ir::CustomHandlers h;` (`psio/pssz_ir_custom.hpp`). `h.add<AccountNumber>()` — under the type's schema id, or `h.add<T>("Id")`: `to_text` / `from_text` from the type's text adapter if it has one, `check` from its invariant if it has one (the typed `validate<T>(psio::pssz{}, bytes)`, which runs the invariants of its members too). By hand: `h.add("Id", {to_text, from_text})`, `h.add("Id", {.check = fn})`, `h.add("Id", {to_text, from_text, check})`; each is a `std::function` returning `codec_status`, and `add` replaces the whole handler of the id. `h.require_all = true` for a strict registry. Passed as the trailing `const CustomHandlers*` of every schema-driven entry point, binary (`validate_pssz`, `validate_auto`, `decode_auto`, `encode_auto`, `encode<W>` / `decode_exact<W>` / `validate_canonical_for_width<W>`, `to_key`) and text, or bound once per schema with `JsonNames names(schema, h)`. |
| Rust | `ToPsszSchema` derive with `#[psio(custom)]` (id: the type name) or `#[psio(custom = "AccountName")]` (an id reserved for PSIO's own types is a compile error); `#[psio(invariant = "...")]` for a type with an invariant; with `#[psio(transparent)]` the wire type is the inner field's. | `let mut h = psio::pssz::dynamic::CustomHandlers::new();` then, each keeping what the id already has of the other kind: `h.add("Id", to_text, from_text)` (the text form, over the wire type's PSSZ bytes); `h.add_check("Id", check)` (the invariant: `Fn(&[u8]) -> Result<(), PsszError>`); `h.add_type::<Account>()?` / `h.add_type_as::<T>("Id")` (the type's own `TextForm` **and** its typed validation as the check); `h.add_invariant_type::<Range>()?` / `h.add_invariant_type_as::<T>("Id")` (the typed validation as the check, for a type with an invariant and no text form). `h.set_strict(true)` for a strict registry. Text entry points: `json_to_pssz_with` / `pssz_to_json_with` / `json_to_value_with` / `value_to_json_with(..., &h)`. Binary entry points: `validate_pssz_with(&schema, &root, bytes, &h)`, and `encode_with` / `decode_exact_with` / `decode_compatible_with` / `validate_with` / `to_key_with`, which take a `dynamic::Options { handlers: Option<&CustomHandlers>, nested: NestedBytes }` (`Options::handlers(&h)`, `Options::opaque_nested()`, `NestedBytes::Typed` (default) / `NestedBytes::Opaque`). Looked up by id per custom value (the Rust dynamic path has no bind step). |
| JS | (reads the node from the schema) | The option `handlers` — `{ [id]: handler }` or a `Map` — of `new PsszSchema(ir, options)`, `PsszSchema.decode(irBytes, options)`, `PsszSchema.fromJson(text, format, options)`, `schema.withHandlers(handlers, { requireHandlers, nestedBytes })` (added to, and replacing by id, the ones the schema has) and `schema.type(root, { handlers, requireHandlers, nestedBytes })`. A handler is `{ toText?(value): string, fromText?(text): value, check?(value): string \| null \| undefined \| void }` over the decoded JS value of the wire type: `check` rejects by returning a message or throwing an `Error` with one, and the message is the error's. `requireHandlers: true` for a strict registry; `nestedBytes: 'typed'` (default) / `'opaque'`. Bound once when the type is compiled; every method of the `PsszType` (`encode`, `decode`, `validate`, the key methods, the text methods) then runs them. A handler cannot be registered for a binary-form id, `box` or a time id. |

**The three registries.** What a schema-driven codec does with a custom
node depends only on the registry it is given:

| registry | text | invariant |
|---|---|---|
| none, or no handler for the id | the wire type's own text: the account as its number, the version as `{"major":1,"minor":2}`, the endpoint as the variant | **not checked**: the node is its wire type, structure only |
| a handler for the id | the handler's text (exactly: the wire type's text is not also accepted), or the wire type's when the handler has only `check` | checked by `check`, on binary and text entry points alike |
| strict (C++ `require_all = true`, Rust `set_strict(true)`, JS `requireHandlers: true`) and no handler for the id | an error, "no handler registered for a custom type" | the same error, on the binary entry points too, wherever a value of the type occurs |

Binary is exact in every case; the fallback text reads back to the same
bytes. A caller that must not accept or produce a value it cannot check and
present exactly uses a strict registry.

The JSON fixture records, for every value of a type holding a custom text
form (the maps keyed by one included: `map<Account,u64>`,
`map<Version,string>`, `Registry`), the typed text (`json`, `ijson`) and the fallback text (`plain_json`,
`plain_ijson`); all three languages reproduce both from the bytes, and the
bytes from both. Its `invariant` rows hold values that violate an invariant
— the typed text, the bytes, the fallback texts and the invariant's message
— for the three registries above (`conformance/custom-types.md`).

A handler cannot express a presentation that is not a single JSON string
(an object, a bare number, a different shape per format), one that depends
on the surrounding value, or a different binary form — the wire type is
fixed by the schema.

Not expressible in the IR, so not available through a schema:

- a member `as<Tag>` override and binary adapters: the IR has the declared
  type;
- C++ `char` elements are bytes 0..255 in the IR; the typed `json` writer
  prints a `std::vector<char>` element with the platform's `char` sign.

No JSON form through a schema (an error, not a guess), because the typed
text codecs have none to agree with: `bitvector` / `bitlist`.

**Raw strings and UTF-8 strings.** The type says which: C++ `std::string` /
Rust `RawString` is the Custom `string` (any bytes), C++ `utf8_string` /
Rust `String` is the Custom `utf8`, each over its byte container. The
schema-driven codecs do what the typed ones do: a `utf8` value is validated
on every read and write; a raw `string` holding bytes that are not UTF-8 is
valid PSSZ (`validate_pssz` accepts it, it decodes and re-encodes
unchanged) but has no JSON or I-JSON text, so `pssz_to_json` returns
"string is not valid UTF-8" and produces nothing. The fixture class
`string-raw-non-utf8` pins this in all three languages. A schema written
before the `utf8` id existed records every string as `string`: through it
UTF-8 strings are no longer validated at the PSSZ level (their text is
still refused when invalid); re-export it to get the check back.

`dynamic_json.hpp` (`ijson_encode_dynamic` / `ijson_decode_dynamic`, over
the older runtime `psio::schema`) is unchanged apart from the 64-bit integer
rule; it has no variant or tuple and its reader is unchecked. New code
should use the Schema IR path above. `view_to_json.hpp` renders a schemaless
pjson view and has no type information.

## The Schema IR as JSON

A Schema IR (`psio::Schema`) has three serialized forms, the same in C++,
Rust and JavaScript: its PSSZ bytes (how a schema is stored and hashed) and
its `json` and `ijson` texts (how a client fetches one).

**Rule.** The text is not a format of its own. It is what the rules above
give the IR's own reflected records (`schema_ir.hpp`: `Schema`, `Member`,
`AnyType`, ...), with a box transparent as it is in PSSZ. Nothing is
special-cased:

- every record is an object with its members in declaration order;
- a type (`AnyType`) is the record `{"value": <variant>}`. Its cached
  `resolved` pointer is not reflected and is in no form;
- the variant is `{"<alternative>": {...}}` under `ijson` and
  `[index, {...}]` under `json`. The names are the alternatives' reflected
  names and the index their position: `Struct` 0, `Object` 1, `Array` 2,
  `List` 3, `BoundedList` 4, `Option` 5, `Variant` 6, `Tuple` 7, `Int` 8,
  `Float` 9, `Nested` 10, `Custom` 11, `Resource` 12, `Type` 13, `Enum` 14;
- optional members are `null` or the value: `Schema.ir_version` (a number),
  `Func.result`, `Attribute.value`, `UseItem.alias`;
- `Array.len`, `BoundedList.maxCount` and `Enumerator.value` are 64-bit
  integers: bare under `json`; under `ijson` bare up to 2^53 - 1 and a
  decimal string beyond (`"len":"18446744073709551615"`);
- reading follows "Reading" above: members in any order, unknown keys
  skipped, a missing member an error unless it is optional (a text without
  `ir_version` is a schema without one, which the schema-driven codecs
  refuse to use), a repeated key read again with the last one winning, the
  `ijson` variant object exactly one member.

**Example.** `struct Point { std::int32_t x; std::uint64_t id; };` inserted
as `Point` (`SchemaBuilder`), under `ijson`:

```json
{"package":{"name":"","version":"","attributes":[]},"interfaces":[],"worlds":[],"uses":[],
 "types":[
  {"name":"Point","type":{"value":{"Object":{"members":[
     {"name":"x","type":{"value":{"Type":{"type":"@1"}}},"attributes":[]},
     {"name":"id","type":{"value":{"Type":{"type":"@2"}}},"attributes":[]}],"attributes":[]}}},"attributes":[]},
  {"name":"@1","type":{"value":{"Int":{"bits":32,"isSigned":true}}},"attributes":[]},
  {"name":"@2","type":{"value":{"Int":{"bits":64,"isSigned":false}}},"attributes":[]}],
 "ir_version":1}
```

and under `json`, where only the variants differ:

```json
{"package":{"name":"","version":"","attributes":[]},"interfaces":[],"worlds":[],"uses":[],
 "types":[
  {"name":"Point","type":{"value":[1,{"members":[
     {"name":"x","type":{"value":[13,{"type":"@1"}]},"attributes":[]},
     {"name":"id","type":{"value":[13,{"type":"@2"}]},"attributes":[]}],"attributes":[]}]},"attributes":[]},
  {"name":"@1","type":{"value":[8,{"bits":32,"isSigned":true}]},"attributes":[]},
  {"name":"@2","type":{"value":[8,{"bits":64,"isSigned":false}]},"attributes":[]}],
 "ir_version":1}
```

(The writers emit no whitespace; the line breaks are for this page.)

**Entry points.**

| | schema → text | text → schema |
|---|---|---|
| C++ | `psio::encode(psio::ijson{}, schema)`, `psio::encode_json_checked(psio::ijson{}, schema, out)` | `psio::decode_json_checked(psio::ijson{}, text, schema)` (any `json_parser`), `psio::validate<psio::Schema>(psio::ijson{}, text)`, `psio::decode<psio::Schema>(psio::ijson{}, text)` |
| Rust | `psio::to_ijson(&schema)` (`Schema: JsonText`; it is also `ToPsszSchema`, so a schema is a member or an argument: `schema_for::<Schema>()` is the fixture's `schema-of-schema`) | `psio::from_ijson::<Schema>(text)`, `psio::validate_ijson::<Schema>(text)` |
| JS | `schema.toJson('ijson')` on a `PsszSchema` | `PsszSchema.fromJson(text, 'ijson', options)` |

`psio::json{}` / `Json` / `'json'` give the other text. The typed C++ and
Rust codecs read the IR's reflected types directly. JavaScript has no typed
IR: it runs its schema-driven text codec over the schema of `psio::Schema`
(`PsszSchema.meta()`, the IR of the IR). C++ can do the same —
`SchemaBuilder::insert<psio::Schema>("Schema")`, then `pssz_ir::pssz_to_json`
/ `json_to_pssz` with root `"Schema"` — and gives the typed text and bytes
(the fixture's emitter and the ctest consumer require it for every row).
The serde derives on the Rust IR types are a different, serde-shaped form
(64-bit integers always bare, no `json` variant form) and are not this text.

**Nesting.** The readers' limit of 64 nested arrays / objects applies to a
schema's text like any other. A type nested inline costs three levels (the
`AnyType` record, the variant, the alternative's record), under the three of
the schema, its `types` array and the named member: a list of a list of ...
an integer reads up to 19 lists deep and is rejected at 20; a record or
variant member adds two more (the `members` array and the member). Deeper
types are written through named types (`Type`), as `SchemaBuilder` does for
every type it interns. PSSZ has a node-depth limit of the same size, and
its checked encoder refuses that list chain at the same point (19 encodes,
20 does not), so the text limit takes nothing away from what a schema's
PSSZ form can hold. The text writers themselves do not check depth: for a
hand-built schema nested beyond the limit they write a text that no reader
accepts.

**Fixture.** `conformance/schema-json-vectors.json`
(`cpp/tests/schema_json_conformance_emit.cpp`; ctest
`psio_schema_json_conformance_regen` fails when the checked-in file is not
what C++ generates) holds, for the schemas the PSSZ, JSON and time/key
fixtures record, the schema of `psio::Schema`, an empty schema, a hand-built
one with every alternative, attributes, interfaces, worlds, uses, a package,
64-bit values on both sides of 2^53 - 1 and no `ir_version`, and a type
nested to the depth limit: `ir_hex` (the PSSZ bytes), `json` and `ijson`.
`accepts` rows are texts no writer produces (reordered members, unknown and
repeated keys, a missing or `null` `ir_version`, a small 64-bit integer as a
string) with the bytes they read as; `rejects` rows are texts that are not a
schema (an unknown alternative name, a variant object with two members or
none, a missing member, an index out of range or of another alternative, a
bare 64-bit integer beyond 2^53 - 1 under `ijson`, one nesting level too
many, ...). Consumed by `cpp/tests/schema_json_conformance_tests.cpp`,
`rust/psio/tests/schema_json_conformance.rs` and
`js/src/test/schema-json-conformance.test.ts`: bytes → schema → text equals
the recorded text, text → schema → bytes equals `ir_hex`, every reject is
rejected. `cpp/tests/schema_json_noexceptions.cpp` runs the C++ entry
points with exceptions and RTTI disabled.

## Open

- Bit types (`psio::bitvector` / `bitlist`, Rust `Bitvector` / `Bitlist`)
  have no JSON form in either language; one has to be chosen before either
  can implement it.
- Rust `validate` materializes the value and drops it.
- Rust has one syntax layer (the hand-written `Reader`); its writer has not
  had the C++ writer's work (about 285 MB/s against 2-3.5 GB/s).
