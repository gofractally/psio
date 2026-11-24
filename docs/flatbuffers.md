# FlatBuffers

`psio::flatbuf` (`<psio/flatbuf.hpp>`, views and in-place writes in
`<psio/flatbuf_view.hpp>`) reads and writes the encoding the official
implementation uses, from the same reflected C++ types every other format
uses. No dependency on the FlatBuffers library.

```cpp
struct Profile { std::uint64_t id = 0; std::string name; bool verified = false; };
PSIO_REFLECT(Profile, id, name, verified)

auto bytes = psio::to_flatbuf(profile);                      // std::vector<char>
auto st    = psio::validate_flatbuf<Profile>(bytes);         // the verifier, codec_status
auto value = psio::from_flatbuf<Profile>(bytes);             // validates, then decodes
auto fast  = psio::from_flatbuf<Profile>(psio::trusted{bytes});   // no validation

psio::flatbuf_view<Profile> v{bytes};                        // safe on any bytes
v.name();                                                    // std::string_view-like, zero copy

auto checked = psio::validated_view<Profile>(psio::flatbuf{}, bytes);
if (checked) checked->name();                                // psio::flatbuf_trusted_view<Profile>

psio::flatbuf_mutable_view<Profile> m{bytes};                // std::vector<char>& / std::span<char>
m.id().set(7);                                               // true: the field is in the buffer

auto framed = psio::to_flatbuf(profile, {.file_identifier = "PROF", .size_prefixed = true});
```

(These snippets are the cases of `cpp/tests/flatbuf_tests.cpp` and
`flatbuf_interop_tests.cpp`, which is where they are compiled and run.)

## Interop guarantees

Checked in ctest against the official library (FlatBuffers 25.12.19)
wherever it and `flatc` are installed (`psio_flatbuf_interop_tests`, schema
`cpp/tests/flatbuf/interop.fbs`, compiled with the object API and mutable
accessors):

* **layout** — psio's vtable slots are the `VT_` constants `flatc`
  generates (including `(id: N)` and a `deprecated` field), and its struct
  sizes and alignments are `sizeof` / `alignof` of the generated structs;
* **psio writes, the official library reads** — the buffer passes the
  official `Verifier`, `UnPack()` yields the same values, accessors and
  `LookupByKey` on psio's sorted vectors work; with a file identifier and
  with a size prefix too;
* **the official library writes, psio reads** — the buffer validates,
  decodes and views (safe and trusted) to the same values;
* **the same bytes** — for every value tested (all shapes of the mapping
  below, a recursive table, non-zero defaults, explicit ids) `psio::to_flatbuf`
  writes byte for byte what the official object API (`T::Pack`, then
  `Finish` / `FinishSizePrefixed`, with and without identifier) writes.
  One documented exception: a float member equal to `-0.0` whose default is
  `0.0` is written by psio (floats are compared by bits, so it round-trips)
  and omitted by the official builder (which compares with `==`);
* **in-place writes** — each side's mutators on the other's buffer;
* **both verifiers on corrupted buffers** — 300,000 corrupted buffers:
  whatever psio accepts the official verifier accepts, and both then read
  the same values. The reverse does not hold: psio refuses about 5 % of
  them that the official verifier takes (see "Validation").

`conformance/flatbuf-vectors.json` pins 27 buffers, 427 that must be
refused (with the reason and the position), 62 corrupted ones the verifier
accepts, and the verdicts on 300,000 further corruptions as ten digests.
Rust decodes and re-encodes all 27 to the same bytes and gives the same
answer on every other buffer (`conformance/flatbuf-vectors.md`; "Rust"
below).

## Type mapping

| C++ | FlatBuffers |
|---|---|
| `bool`, `int8_t` … `uint64_t`, `char`, `float`, `double` | `bool`, `byte` … `ulong`, `float`, `double` |
| enum | an `enum` of its underlying type |
| `std::string` | `string` |
| `std::vector<T>` | `[T]`: scalars (`std::vector<bool>` too), enums, structs, strings, tables |
| `std::array<T, N>` in a struct | a fixed array `[T:N]` |
| `std::array<T, N>` in a table | a vector that must hold exactly N elements |
| reflected record | `table` |
| reflected record that is `definitionWillNotChange()` and holds only scalars, enums, fixed arrays and such records | `struct`: fixed size, stored inline, no vtable |
| `std::optional<S>`, S a scalar or enum | an optional scalar (`= null`) |
| `std::optional<X>`, X anything else | the field; absent is "no value" (an engaged optional is always written, even an empty string) |
| `std::variant<A, B, …>` of tables | `union`; a leading `std::monostate` is `NONE` |
| `std::set<K>`, K a scalar or string | `[K]`, sorted |
| `std::map<K, V>` | `[Entry]` sorted by key, with `table Entry { key:K (key); value:V; }` |

Not mapped (the record is then not encodable — `to_flatbuf` does not
exist for it — and a `static_assert` names the reason where a member is at
fault): vectors of unions, unions of structs or strings, `std::optional`
of an optional or of a variant, a vector of vectors (wrap the inner one in a
record, as in a `.fbs` file), `std::tuple`, `std::set` / `std::map` with a
comparator other than `std::less`, 128-bit integers, psio wrapper types
(`bounded`, `utf8_string`, time types, `nested`).

**A struct cannot be the root.** FlatBuffers requires a table at the root
of a buffer, so `to_flatbuf(v)` does not exist for a record that maps to a
struct (a `definitionWillNotChange()` record of scalars): wrap it in a
record, or encode the record that contains it. This is a change from the
earlier `psio::flatbuf`, which wrote every record as a table.

## Field ids, defaults, evolution

* **Field ids** — `attr(member, field<N>)` (the one number protobuf and
  Cap'n Proto also use; `psio/detail/field_number.hpp`). Vtable slots are
  assigned in ascending field-number order, declaration order by default.
  A variant member takes two consecutive slots (its type byte, then its
  value), exactly as a union in a `.fbs` file does. The rule for compatible
  evolution is FlatBuffers' own: a new member takes a number above every
  number in use; members keep their numbers.
* **Deprecated members** — `attr(member, deprecated_spec{.version = "…"})`
  keeps the member's slot and neither writes nor reads it (FlatBuffers'
  `deprecated`): the C++ member stays, at its default.
* **Structs** have no field ids: members are laid out in declaration order,
  each at its natural alignment; the size is rounded up to the largest
  alignment.
* **Defaults** — a scalar member's initialiser is its FlatBuffers default
  (`std::int16_t level = 7;` is `level:short = 7;`): a value equal to it is
  not written, and an absent field reads as it — in `from_flatbuf` and in
  every view. An empty string, vector, set or map is not written; a nested
  table and a struct member always are.
* **Unions** — a variant without `std::monostate` always writes its active
  alternative; reading a union that is `NONE` or absent (an older writer)
  yields the default-constructed variant, i.e. its first alternative.
* **Bounds, `utf8`, `sorted`, …** — not part of the wire.
  `validate_flatbuf` checks structure;
  `psio::validate_strict<T>(psio::flatbuf{}, bytes)` also checks the
  annotations on the decoded value. Strings are NOT checked for UTF-8 by
  `validate_flatbuf` (neither does the official verifier by default).
* **`maxDynamicData`** — bounds the buffer size in `validate_flatbuf`.

## File identifier, size prefix

`psio::to_flatbuf(v, {.file_identifier = "ABCD", .size_prefixed = true})`
(`psio::flatbuf::options`). `psio::flatbuf_identifier(bytes)` /
`flatbuf_has_identifier(bytes, "ABCD")` read the identifier;
`psio::validate_flatbuf<T>(bytes, options)` requires it and / or checks the
prefix against the buffer's size (alignment is then checked from the start
of the prefix, as the builder aligned it). `psio::flatbuf_size_prefixed(bytes)`
returns the buffer inside the prefix — what views and `from_flatbuf` take —
and so finds the next buffer of a stream.

## Validation

`validate_flatbuf<T>` is the verifier. It is typed — it follows everything
a `T` can reach — and safe on arbitrary bytes. It checks:

* the root offset; every offset (not null, forward, in bounds, 4-aligned);
* every table: its position and alignment, its vtable (in bounds, even
  size of at least 4, 2-aligned), the table's size against the buffer, and
  **every field offset against the table's own size**;
* scalar and struct fields: bounds and alignment;
* strings: bounds and the terminating NUL;
* vectors: the length against the buffer (in 64-bit arithmetic), element
  alignment, a fixed-length vector's length;
* unions: a known type, a value when there is a type, no value when it is
  `NONE`;
* sets and maps: keys strictly ascending (so that key lookup is correct); a
  string key present;
* nesting depth (64) and the number of tables visited (1,000,000) — the
  official verifier's defaults; `validate_flatbuf<T>(bytes,
  psio::flatbuf::limits{…})` changes them.

Offsets are unsigned and point forward, so a buffer cannot contain a cycle;
what a hostile buffer can do is reach one sub-object along very many paths,
which the table budget bounds (`flatbuf_fuzz_tests.cpp` builds such a
buffer: 40^12 paths, refused after 1,000,000 tables).

It is **stricter than the official verifier**. On the corrupted buffers of
the interop test it refuses, and the official one accepts: a table size or
vtable size that is inconsistent, a field offset outside its table, set /
map keys out of order, an unknown union type, a union type without a value
or a `NONE` with one, a fixed-length vector of another length, a null
offset inside a vector. Each of these is something no builder writes.
Fields a newer schema added (slots beyond the ones `T` knows) are not
followed and not checked — a view never reads them.

Evidence: `psio_flatbuf_fuzz_tests` (every truncation and eight
replacements of every byte of five encoded values; 240,000 random
corruptions of bytes, offsets, vtable entries and lengths; 120,000 buffers
of random bytes) and the same file built with AddressSanitizer and
UndefinedBehaviorSanitizer (`psio_flatbuf_fuzz_tests_sanitized`), with the
buffer in a heap allocation of its exact size.

## Views

`psio::flatbuf_view<T>` has the surface of every psio view: named readers,
`get<N>()`, list views (size, `[]`, `at`, iteration), optional and variant
views, `unpack()`, comparison with values; struct views convert to their
value. Sorted vectors have key lookup: a set view `contains(k)`, `find(k)`,
`lower_bound(k)`; a map view those plus `at(k)` (an optional of the mapped
value's view) and `value_or(k, fallback)`, and iterates as `(key, value)`
pairs.

Two kinds, as for PSSZ (`docs/helpers.md`, "Safe and trusted views"):

* **`flatbuf_view<T>` is safe on any bytes.** Every offset, vtable entry
  and length is compared with the buffer before it is used; what does not
  fit reads as absent — a default, an empty string, an empty list. It never
  reads outside the bytes it was given (the fuzz tests walk it over every
  corrupted buffer and check every span it hands out).
* **`flatbuf_trusted_view<T>`** — what `psio::validated_view<T>(psio::flatbuf{},
  bytes)` returns, or `flatbuf_trusted_view<T>{psio::trusted{bytes}}` on the
  caller's word — compares nothing: a field read is the vtable lookup the
  official accessors do. It is one pointer. It does not carry the buffer's
  bounds, so it does not convert to the safe view (the PSSZ one does).

In both kinds an absent field reads as its default, and an absent table or
struct as a view whose members read as their defaults — there is no null
to test. `unpack()` is a trusted decode in both kinds, as everywhere in
PSIO.

## Writing in place

`psio::flatbuf_mutable_view<T>` over `std::span<char>` (or a
`std::vector<char>&`) writes values of fixed size **that the buffer already
holds**: a scalar field of a table (`m.customer().age().set(38)`), a member
of a struct, a whole struct (`m.origin().set(Vec3{…})`), an element of a
vector of scalars or structs, and the same through nested tables and
vectors of tables. Nothing moves and no offset, length, vtable or union
type is touched, so the result is a valid buffer — the encoding of the
modified value.

`set` returns `false`, and writes nothing, for a field the buffer does not
hold: a scalar that equalled its default when the buffer was written, an
empty optional, an element past the end. `has_value()` tells beforehand.
That is the official `mutate_` accessors' rule too.

There is deliberately no size-changing edit (replacing a string, a vector,
a table, switching a union): the first generation's splice-and-patch
mutation was not sound around optional members, and FlatBuffers has no
append-and-repoint form that keeps the buffer canonical (offsets are
unsigned, so a new object would have to be placed after everything that
refers to it). Decode, change, re-encode.

Navigation is the safe view's, so a write stays inside the buffer whatever
the bytes are. One caveat for hostile input: `validate_flatbuf` does not
prove that objects do not overlap (neither does the official verifier), so
in a buffer crafted that way a write through one field can change what
another reads; re-validate such a buffer before reading it through a
trusted view. Buffers written by psio or by the official builder never
overlap objects.

## Schema

`psio::fbs_layout(schema)` (`<psio/fbs_layout.hpp>`) computes the wire
layout of a Schema IR — vtable slots (declaration order or `(id: N)`, two
for a union, kept by a `deprecated` field) and struct offsets, sizes and
alignments. For `cpp/tests/flatbuf/interop.fbs` parsed by `psio::parse_fbs`
it equals the codec's compile-time layout (`psio::flatbuf::layout<T>`) for
every shape and the constants `flatc` generates (both are tested).
`parse_fbs` reads fixed arrays (`[T:N]`) and keeps an enum's underlying
type for this. What the schema side does not do yet is in
`docs/formats-migration-status.md` (F3).

## Performance

`psio_bench_view_generations flatbuf`, Apple silicon, Homebrew clang 22,
`-O3`, ns per operation, minimum of 11 interleaved trials, two runs
(1-minute load 1.9–2.0). Shape: `Order {id, customer: UserProfile{5
fields}, items: 16 × LineItem{product, qty, unit_price}, total, note}`, 888
bytes, the same bytes from both writers.

| Operation | psio safe view | psio trusted view | flatc accessors | first generation |
|---|---:|---:|---:|---:|
| one scalar of the root | 0.86 / 0.85 | 0.59 / 0.59 | 0.52 / 0.52 | 0.52 / 0.52 |
| all 5 fields of a nested table | 3.51 / 3.52 | 2.41 / 2.40 | 2.16 / 2.15 | 2.22 / 2.22 |
| nested scalar | 1.53 / 1.53 | 0.91 / 0.91 | 0.78 / 0.78 | 0.78 / 0.78 |
| nested string | 2.04 / 2.11 | 1.00 / 1.00 | 0.76 / 0.76 | 0.82 / 0.82 |
| string of the root | 1.22 / 1.20 | 0.68 / 0.68 | 0.51 / 0.51 | 0.58 / 0.59 |
| iterate 16 tables, one scalar each | 17.5 / 17.7 | 7.76 / 7.76 | 7.31 / 7.37 | 7.35 / 7.41 |
| iterate, all 3 fields | 39.7 / 39.7 | 18.9 / 18.9 | 13.6 / 13.6 | 13.9 / 13.8 |

| Operation | psio | official library | first generation |
|---|---:|---:|---:|
| encode, a buffer per message | 319 / 323 | 432 / 426 (a builder per message) | 346 / 348 (1000 bytes, vtables not shared) |
| encode, storage reused | 290 / 296 (sink reused) | 419 / 420 (builder cleared and reused) | |
| verify | 102 / 105 | 60 / 62 | 80 / 78 (not safe on arbitrary bytes) |
| decode to a C++ struct | 108 / 109 | not measured (the benchmark schema has no object API) | 107 / 109 |

What the numbers say:

* **Encode is 1.3–1.45× faster than the official builder**, writing the
  same bytes. (The first generation's 1.2× came with a 12 % larger buffer.)
* **Reads are not faster than the official accessors.** The trusted view is
  5–15 % slower on scalars and iteration and about 30 % slower where
  strings are read. Its code is the official accessor's plus one test for
  an empty buffer at the root; the official column also skips the null
  test a caller needs before reading a string or sub-table that may be
  absent, which psio's views do for it.
* **The safe view costs 1.6–2.9×** the official accessors: the price of
  being usable on unvalidated bytes.
* **The verifier is 1.7× slower than the official one**, and checks more
  (see "Validation").

## Not supported

Vectors of unions; unions of structs or strings; `nested_flatbuffer`;
`force_align`; shared strings on write (reading them works: they are
ordinary offsets); 64-bit offsets and vectors; FlexBuffers; `required`
other than a map's string key; a size-changing edit in place.

## Rust

`psio::flatbuf` (feature `flatbuf`): the same mapping, the same bytes, the
same verifier.

```rust
use psio::flatbuf::{FbView, FbOptions};

#[derive(Debug, PartialEq, psio::FbPack, psio::FbUnpack, psio::FbView)]
struct Profile {
    id: u64,
    name: String,
    verified: bool,
}

let bytes = psio::to_flatbuf(&profile);                        // Vec<u8>
psio::validate_flatbuf::<Profile>(&bytes)?;                    // the verifier, Result<(), FbError>
let value = psio::from_flatbuf::<Profile>(&bytes)?;            // verifies, then decodes
let fast = psio::from_flatbuf_trusted::<Profile>(&bytes);      // no verification

let view = FbView::<Profile>::new(&bytes)?;                    // verifies, then borrows
view.name();                                                   // &str, zero copy
let view = FbView::<Profile>::trusted(&bytes);                 // no verification

let opts = FbOptions { file_identifier: Some(*b"PROF"), size_prefixed: true };
let framed = psio::to_flatbuf_with(&profile, &opts);
```

(Compiled and run as `rust/psio/tests/flatbuf_api.rs` and the example at
the top of `rust/psio/src/flatbuf/mod.rs`.)

### Mapping

| Rust | FlatBuffers |
|---|---|
| `bool`, `i8` … `u64`, `f32`, `f64` | `bool`, `byte` … `ulong`, `float`, `double` |
| field-less `#[repr(int)]` enum | an `enum` of its repr type |
| `String` | `string`, verified to be UTF-8 |
| `psio::pssz::RawString` | `string`, any bytes (what C++ `std::string` holds) |
| `Vec<T>` | `[T]`: scalars, enums, structs, strings, tables |
| `[T; N]` in a struct | a fixed array `[T:N]` |
| `[T; N]` in a table | a vector that must hold exactly N elements |
| struct | `table` |
| struct that is `#[psio(definition_will_not_change)]` and holds only scalars, enums, fixed arrays and such structs | `struct` |
| `Option<S>`, S a scalar or enum | an optional scalar |
| `Option<X>`, X anything else | the field; absent is `None` (`Some` is always written, even an empty string) |
| enum whose variants hold one table each | `union`; a leading unit variant is `NONE` (C++ `std::monostate`) |
| `BTreeSet<K>`, K a scalar or string | `[K]`, sorted |
| `BTreeMap<K, V>` | `[Entry]` sorted by key |

As in C++ the choice between table and struct is made from the member
types, by constants (`FbPack::STRUCT`, `TABLE`, …): a
`definition_will_not_change` record with a string member is a table. A
type FlatBuffers cannot hold (`Vec<Vec<T>>`, `Option<Option<T>>`, a union
of structs, …) and a struct, scalar or vector at the root of a buffer are
compile-time errors that say so. The earlier crate wrapped a root scalar,
string or vector in a one-field table; that is gone.

Per member:

* `#[psio(field = N)]` — the field number (C++ `field<N>`): vtable slots in
  ascending order of it, the declaration position (from 1) by default;
* `#[psio(default = 7)]`, `#[psio(default = "Color::Green")]` — the default
  of a scalar member (C++: its initialiser; Rust has none, so it is
  written here): a literal, or an expression in a string. A member whose
  enum type has no enumerator 0 must declare one;
* `#[psio(deprecated)]` — keeps the slot; neither written nor read. The
  decoded member is its default.

### Verifier

`validate_flatbuf::<T>` (`FbUnpack::fb_validate`,
`validate_flatbuf_with(bytes, &FbOptions, FbLimits)`) is the C++ verifier
ported check for check: the same order, messages and positions, the same
limits (depth 64, 1,000,000 tables). It never panics, reads outside the
bytes or does unbounded work. `FbError` has `message()` and `offset()`.

Two checks are Rust's own, because the Rust type cannot hold what C++
accepts:

* a `String` member must be UTF-8 (`flatbuf: string is not valid UTF-8`).
  The scan is linear in the buffer however often a string is pointed at or
  overlapped: once more bytes have been scanned than the buffer has, the
  verifier remembers the ranges it has checked. `RawString` has no such
  check;
* an enum without a `#[psio(other)]` catch-all variant holds only its
  enumerators (`flatbuf: unknown enum value`), as in the crate's pSSZ. With
  the catch-all (`#[psio(other)] Other(u8)`) every value is accepted and
  kept.

With `RawString` and catch-all enums the accepted set is C++'s on every
buffer tested: `rust/psio/tests/flatbuf_conformance.rs` checks all 489
rejected and corrupted rows of the fixture for the same verdict, message
and position, and reproduces the fixture's ten digests — the verdict,
message and position of the C++ verifier on 300,000 corruptions (82,459
accepted) that Rust draws the same way. For the same types written with
`String` and plain enums it pins which rows differ (9 of the 62 corrupted
rows C++ accepts are refused; in 2 of the 427 rejected rows a Rust-only
check comes first) and checks, on the 300,000, that nothing C++ refuses is
accepted and that an extra refusal is one of the two above.

Evidence: `rust/psio/tests/flatbuf_fuzz.rs`, the cases of the C++ fuzz
test — every truncation and eight replacements of every byte of seven
encoded values (20,675 buffers, 8,387 valid), 300,000 random corruptions
(39,214 valid), 180,000 probes of random bytes, the shared-sub-object
buffer (40^12 paths, refused after 1,000,000 tables), 100,000 references
to one 400 KiB string. On every buffer the view is walked in full through
its accessors, with key lookups; when the buffer validates, what the
accessors read is what the decoder gives, and it re-encodes to a valid
buffer.

### Views

`FbView<'a, T>` reads in place. `FbView::<T>::new(bytes)` verifies first
(`new_with` for an identifier, a size prefix or other limits);
`FbView::<T>::trusted(bytes)` does not. Unlike in C++ there is one
implementation behind both: safe Rust cannot skip the bounds checks, so a
view from `trusted` is as safe as the C++ safe view — every offset, vtable
entry and length is compared with the buffer, what does not fit reads as
absent, nothing panics — and what `new` adds is the guarantee that the
values are the ones that were written. `#[derive(FbView)]` gives the view
one method per member, through a trait `<Record>FbAccessors`:

| member | reads as |
|---|---|
| scalar, enum | the value; its default when absent |
| `String` / `RawString` | `&str` / `&[u8]` |
| table, struct | `FbView` (an absent one reads as defaults) |
| `Vec<T>`, `[T; N]` | `FbVec`: `len`, `get(i)`, `iter`, `unpack`; `as_bytes` for `u8` |
| `Option<X>` | `Option` of X's view |
| `BTreeSet<K>` | `FbSet`: `len`, `get(i)`, `iter`, `contains`, `find`, `lower_bound` |
| `BTreeMap<K, V>` | `FbMap`: `len`, `key(i)`, `value(i)`, `iter`, `get(key)`, `contains_key`, `find`, `lower_bound` |
| union | `FbUnion`: `tag`, `is_none`, `unpack`, and one accessor per alternative (`payment.card()` is an `Option<FbView<Card>>`) |

`view.unpack()` is the value. `FlatbufBuffer<T>` / `SharedFlatbuf<T>` own
verified bytes and hand out the view (`as_view`). `psio::flatbuf::FbTable`
/ `FbVector` are the untyped cursors underneath (fields by vtable slot),
with the same safety.

### Where Rust and C++ differ

* `String` and closed enums: the two verifier checks above.
* `from_flatbuf_trusted` and a view from `trusted` are safe functions: on
  bytes that would not verify they give unspecified values, not undefined
  behaviour. They are not bounded by the verifier's limits (a hostile
  buffer can nest deeply or share sub-objects), so verify untrusted bytes.
* A struct member has no default: an absent struct reads as zeros (in C++
  as its members' initialisers).
* `maxDynamicData` does not bound the buffer in Rust (the attribute is
  pSSZ's there).
* No in-place writes with the typed views (C++ `flatbuf_mutable_view`); the
  untyped `FbMut` of the earlier crate remains and was not hardened.
* `FbSchema` / `to_fbs_schema` (the earlier crate's `.fbs` text) and the
  dynamic view know only the shapes they knew before.

## Other languages

JavaScript and Python have no FlatBuffers support.
