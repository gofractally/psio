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

`conformance/flatbuf-vectors.json` pins 21 buffers and 14 that must be
refused; Rust decodes and re-encodes the rows of the shapes its derive can
declare to the same bytes (`conformance/flatbuf-vectors.md`).

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

## Other languages

Rust (`psio::flatbuf`): the derive covers tables of scalars, strings,
vectors of scalars / strings / tables and nested tables, and writes the
same bytes as C++ for them (field order, vtable sharing and empty strings
were aligned in this stage; `conformance/flatbuf-vectors.json`). The derive
has no structs, unions, optional scalars, sorted-vector lookup, non-zero
defaults or field ids, and the crate has **no verifier**: `fb_unpack` returns an error, stops with a slice-index panic,
or accepts an invalid buffer depending on what it happens to index
(`rust/psio/tests/flatbuf_conformance.rs` pins which). JavaScript and Python
have no FlatBuffers support.
