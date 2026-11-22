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
  `Custom` node with a handler ("Custom types through a schema" below).

### Enums

An enum is its underlying integer on the wire (PSSZ and every binary
format); the names exist for text.

| | declaration |
|---|---|
| C++ | `enum class Status : std::int32_t { pending = -1, active = 2 };` then `PSIO_REFLECT_ENUM(Status, pending, active)` — one line, any namespace or in a class body. Without it the enum has no names and is written and read as its number. |
| Rust | the derive takes the variant identifiers; `#[json(name = "active")]` renames one. |
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
non-canonical or out-of-range number are errors. A number that names no
enumerator is accepted where the reader's type can hold it: always in C++
and through a schema; in Rust only when the enum has a catch-all variant
(`#[pssz(other)] Unknown(i32)`), otherwise it is an error. When two
enumerators share a value, the first declared name is written.

### Maps and sets

A set is an **array** of its elements. A map is an **object** when the text
of its key type is a JSON string *by type*, and an **array of `[key, value]`
pairs** otherwise. The rule is the same in `json` and `ijson`.

| key type | map text |
|---|---|
| a string kind: C++ `std::string`, `utf8_string<N>`; Rust `String`, `RawString`, bounded strings; Schema IR `string` / `utf8` | `{"apple":1,"pear":3}` |
| a type with a custom text form: C++ `text_category` adapter, Rust `TextForm`, a schema `Custom` presented by its handler | `{"alice":5,"bob":7}` |
| anything else — integers, enums, time values, byte sequences, tuples, records, floats, ... | `[[1,"one"],[256,"x"]]` |

A key that merely *happens* to be written as a string (an enumerator name, a
time point, a 64-bit integer above 2^53 under `ijson`, hex bytes) is not an
object key: the form of a map depends on the key type alone, never on the
values.

- **Writers** emit the entries in canonical order: ascending key bytes of
  the key (`docs/pssz-spec.md` §5.8), the order of the binary form. That is
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
  bound, a custom text form its own `validate`.
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

### Variant alternative names

C++ uses `get_type_name` of the alternative: a record's reflected name, and
`bool int8 uint8 int16 uint16 int32 uint32 int64 uint64 float32 double
string` for primitives, `T[]` for a vector, `T[N]` for an array, `T?` for an
optional, `tuple_A_B` (also for a `std::pair<A, B>`), `map_K_V` for a map and
`set_K` for a set (whatever container holds them). Alternatives must have distinct names (checked at
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
`codec_exception` (aborts when exceptions are disabled). For `psio::json`
only, it keeps one leniency of the old decoder: a missing non-optional
member is left value-initialized. `validate` and `decode_json_checked` are
the entry points for untrusted text.

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

## Schema-driven path (no C++ / Rust type)

A consumer that has only a Schema IR (`schema_ir.hpp`, Rust
`psio::pssz::schema_ir`) — a CLI, an HTTP "pack this action" endpoint, an
indexer — converts and validates with:

| | C++ (`psio/pssz_ir_json.hpp`, namespace `psio::pssz_ir`) | Rust (`psio::pssz::dynamic`) |
|---|---|---|
| text → PSSZ | `json_to_pssz(schema, root, psio::ijson{}, text, bytes)` | `json_to_pssz::<IJson>(&schema, &root, text)` |
| PSSZ → text | `pssz_to_json(schema, root, psio::ijson{}, bytes, text, &has_unknown)` | `pssz_to_json::<IJson>(&schema, &root, bytes)` |
| validate PSSZ | `validate_pssz(schema, root, bytes, &has_unknown)` | `validate_pssz(&schema, &root, bytes)` |

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
  bound), integers of 8-256 bits,
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

A type with a custom text form — an account name over a `u64`, a time
point, `major.minor` over a record — is, in the Schema IR, the existing
`Custom` node: `Custom { type: <the wire type>, id: "<presentation>" }`.
The wire type says exactly what the bytes are; the id names how the value
reads as text. (The ids PSIO itself gives a binary meaning to — `bool`,
`char`, `string`, `utf8`, `hex`, `bitvector`, `bitlist`, `map`, `set`, `box`
— are fixed; every other id is a text presentation and never changes the
bytes, the widths or validation.)

**Maps and sets.** `Custom{ List{ Tuple{[K, V]} }, "map" }` and
`Custom{ List{ K }, "set" }` (`BoundedList` in place of `List` under a
maximum count) are a map and a set: the list's entries are in strictly
ascending key order, which the schema-driven codecs check on every read and
produce on every write (a dynamic map value is the sequence of its
`[key, value]` entries, in any order when handed to an encoder). They take no
handler. Their text is that of "Maps and sets" above, decided from the key
type's node: an object when it is a `string` / `utf8` node, or a custom
presentation **whose handler is registered** (the key text is the handler's);
the pairs form otherwise. A custom presentation with no handler falls back,
as everywhere, to the plain form of its wire type — and the map's form
follows it: an account-keyed map is `{"alice":5}` with the account handler
and `[[235814801429757952,5]]` without. A strict registry
(`require_all`) makes the missing handler an error instead.

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

The schema-driven text codecs take a **handler registry**: id → a pair of
functions between the PSSZ bytes of the wire type and the presentation text.
The text is carried as one JSON string in both formats, the rule of a typed
text adapter.

| | emits the node | registers a handler |
|---|---|---|
| C++ | `SchemaBuilder`, for a type with a `text_category` adapter that is a reflected record or an enum (a strong integer typedef). Id: the adapter's `static constexpr std::string_view schema_id`, else the reflected name. | `psio::pssz_ir::CustomHandlers h; h.add<AccountNumber>();` (the type's own text adapter, registered once) or `h.add("Id", {to_text, from_text})`; passed as the trailing argument of `json_to_pssz` / `pssz_to_json` / `json_to_value` / `value_to_json`, or bound once per schema with `JsonNames names(schema, h)`. |
| Rust | `ToPsszSchema` derive with `#[pssz(custom)]` (id: the type name) or `#[pssz(custom = "AccountName")]`; with `#[pssz(transparent)]` the wire type is the inner field's. | `let mut h = psio::pssz::dynamic::CustomHandlers::new(); h.add_type::<Account>()?;` (the type's own `TextForm`), `h.add_type_as::<T>("Id")`, or `h.add("Id", to_text, from_text)` over the wire type's PSSZ bytes; `h.set_strict(true)`; passed to `json_to_pssz_with` / `pssz_to_json_with` / `json_to_value_with` / `value_to_json_with`. Looked up by id per custom value (the Rust dynamic path has no bind step). |
| JS | (reads the node from the schema) | `PsszSchema.decode(irBytes, { handlers: { Version: versionHandler, AccountName: accountHandler }, requireHandlers })`, `schema.withHandlers(...)` or `schema.type(root, { handlers })`; a handler is `{ toText(value): string, fromText(text): value }` over the decoded JS value of the wire type, bound once when the type is compiled. |

**Fallback.** An id with no registered handler reads and writes as its wire
type: the account as its number, the version as `{"major":1,"minor":2}`.
Binary is exact either way; only the text is plainer, and the plain text
reads back to the same bytes. A caller that must produce the exact
presentation sets the registry to strict (C++ `require_all = true`): a
missing handler is then an error ("no handler registered for a custom
type") instead of the fallback. With a handler registered the presentation
is exactly its text: the plain form is not also accepted.

The JSON fixture records, for every value of a type holding a custom form
(`Version`, `Release`, `Account`, `Transfer`, and the maps keyed by one:
`map<Account,u64>`, `map<Version,string>`, `Registry`), the typed text (`json`,
`ijson`) and the fallback text (`plain_json`, `plain_ijson`); all three
languages reproduce both from the bytes, and the bytes from both.

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

## Open

- Bit types (`psio::bitvector` / `bitlist`, Rust `Bitvector` / `Bitlist`)
  have no JSON form in either language; one has to be chosen before either
  can implement it.
- Rust `validate` materializes the value and drops it.
- Rust has one syntax layer (the hand-written `Reader`); its writer has not
  had the C++ writer's work (about 285 MB/s against 2-3.5 GB/s).
