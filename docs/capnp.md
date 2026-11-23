# Cap'n Proto

`psio::capnp` (`<psio/capnp.hpp>`, views and editing in
`<psio/capnp_view.hpp>`) reads and writes the encoding the official
implementation uses, from the same reflected C++ types every other format
uses. No dependency on libcapnp.

```cpp
struct Profile { std::uint64_t id = 0; std::string name; bool verified = false; };
PSIO_REFLECT(Profile, id, name, verified)

auto bytes = psio::to_capnp(profile);                    // std::vector<char>, framed message
auto st    = psio::validate_capnp<Profile>(bytes);       // typed check, codec_status
auto value = psio::from_capnp<Profile>(bytes);           // validates, then decodes
auto fast  = psio::from_capnp<Profile>(psio::trusted{bytes});  // no validation

psio::capnp_view<Profile> v{bytes};                      // bytes: validated
v.name();                                                // std::string_view-like, zero copy
```

(These snippets are the cases of `cpp/tests/capnp_tests.cpp` and
`capnp_interop_tests.cpp`, which is where they are compiled and run.)

## Interop guarantees

Checked in ctest against the official library (libcapnp 1.3.0) wherever
CapnProto is installed (`psio_capnp_interop_tests`,
schema `cpp/tests/capnp/interop.capnp`):

* a message psio writes is read by libcapnp with the same values —
  scalars, defaults, Text, Data, `List(Bool)`, lists of lists, lists of
  structs, unions, optional members;
* a message libcapnp writes — single segment, or forced into many segments
  so that every pointer is a far pointer — validates, views and decodes in
  psio to the same values;
* for records without unions written in member order, psio's bytes are
  identical to libcapnp's;
* a document edited in place by psio is read by libcapnp.

Struct layout is the official compiler's: `psio::capnp::layout<T>` is a
compile-time port of its allocator, compared with `capnp compile -ocapnp`
on generated records (`cpp/tests/capnp/layout_fuzz.py`, 360 records with
unions of mixed sizes and shuffled field numbers: no difference; 48 of
them are pinned in `capnp_layout_fixture.hpp` for machines without the
tool).

## Type mapping

| C++ | Cap'n Proto |
|---|---|
| `bool` | `Bool` (one bit) |
| `int8_t` … `uint64_t`, `float`, `double` | `Int8` … `UInt64`, `Float32`, `Float64` |
| enum | its underlying integer (not a capnp `enum`: those are always 16 bits) |
| `std::string` | `Text` |
| `std::vector<T>`, `std::array<T, N>` | `List(T)`; bytes are `Data`; bools are bits; records are an inline-composite list |
| reflected record | `struct` |
| `std::optional<P>`, P a string / list / record | the pointer; null = no value |
| `std::optional<S>`, S a scalar | `union { none @n :Void; some @n+1 :S; }` |
| `std::variant<A, B, …>` (two or more) | `union`, one member per alternative; `std::monostate` is `Void` |

Not mapped (a compile error names the type): `std::map` / `std::set` /
`std::tuple`, 128-bit integers, psio wrapper types (`bounded`,
`utf8_string`, time types, `nested`), a variant or optional scalar anywhere
but directly as a record member (lists of unions need a wrapping record, as
in Cap'n Proto itself).

## Annotations

* **Field numbers** — `attr(member, field<N>)` (the one number protobuf
  also uses; `psio/detail/field_number.hpp`). Ordinals are assigned in
  ascending field-number order, declaration order by default. A variant
  member takes one ordinal per alternative, an optional scalar two. Rule
  for compatible evolution: new members get a number above every number in
  use. A variant that may gain alternatives should be the last member of
  its record or live in a record of its own — an alternative added in the
  middle shifts the ordinals after it.
* **Defaults** — a scalar member's initialiser is its Cap'n Proto default
  (`std::int16_t level = 7;` is `level @2 :Int16 = 7;`): the wire holds
  value XOR default, and a missing struct or a struct from an older schema
  reads as the defaults. Pointer members default to empty, unions to their
  first alternative, optionals to no value.
* **Bounds, `utf8`, `sorted`, …** — not part of the wire. `validate_capnp`
  checks structure; `psio::validate_strict<T>(psio::capnp{}, bytes)` also
  checks the annotations on the decoded value. Text is NOT checked for
  UTF-8 by `validate_capnp` (neither does libcapnp).
* **`maxDynamicData`** — bounds the message size in `validate_capnp`.

## Validation

`validate_capnp<T>` is typed: it follows every pointer a `T` can reach and
checks its kind, element size, bounds within its segment, the NUL of Text,
union discriminants, fixed array lengths, far pointers and their landing
pads, under the official reader's limits (64 levels, 8 Mi words traversed;
`validate_capnp<T>(bytes, psio::capnp::limits{…})` to change them). It
rejects bytes after the message (`psio::capnp_message_size` finds the end
of the first message in a stream). Pointers the type does not know (a newer
schema's members) are not followed and not checked — a view never reads
them.

Views, `decode` and `from_capnp(psio::trusted{…})` assume validated bytes.
On bytes that were never validated they are the caller's risk.

## Views

`psio::capnp_view<T>` has the surface of every psio view: named readers,
`get<N>()`, list views (size, `[]`, `at`, iteration), optional and variant
views, `unpack()`, comparison with values. It reads a struct of any size:
members the writer did not write read as defaults. It follows far
pointers.

## Editing

* `psio::capnp_mutable_view<T>{std::span<char>}` — scalars in place
  (`m.customer().age().set(38)`); the result is the encoding of the
  modified value. `set` returns false for a member the writer's struct has
  no room for.
* `psio::capnp_document<T>` — owns the bytes; `doc.edit().name().set(…)`
  replaces a Text / list / record / optional pointer by appending the new
  object and rewriting the one pointer to it (no existing byte moves; this
  is what the official builders do). The replaced object stays in the
  message as unreachable bytes until `doc.compact()`, which re-encodes.
  Single-segment messages only (a multi-segment one is compacted on
  construction). Union members cannot be switched through the editor yet.

## Not supported

Packed encoding; capabilities and interfaces; `AnyPointer`; groups other
than the unions above; generics; reading a `List(struct)` that was written
as a list of primitives or pointers (the upgrade path older schemas used);
writing more than one segment; canonicalization of foreign messages
(`capnp_document::compact` produces psio's own one-segment form).

## Other languages

The Rust crate's capnp module lays unions out with one slot per alternative
and the discriminant last (the first generation's rule), which is not the
official layout and not what C++ now writes; records without variants agree.
Rust needs the allocator in `capnp.hpp` (`layout_top` / `layout_union` /
`layout_group`) and the optional mapping above before unions and optional
scalars can be exchanged. JS and Python have no Cap'n Proto support.
