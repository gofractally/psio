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
* **Bounds, `utf8`, `sorted`, invariants** — not part of the wire, and part
  of the type all the same: `validate_capnp` and `from_capnp` check the
  structure of the message and then hold the value it decodes to against
  what the type declares
  ([annotations.md §7.2](annotations.md#72-reading-and-writing-the-two-modes)):
  `capnp: member length_bound violated`, the `utf8_spec` / `sorted`
  messages, the invariant's own, at position 0. A type that declares
  nothing is not decoded, and a Text member that is not declared `utf8` is
  NOT checked for UTF-8 (neither does libcapnp). `to_capnp` is the fast
  write and does not check; `to_capnp_checked` / `encode_capnp_checked`
  refuse what `validate_capnp` refuses (Rust: `try_to_capnp`).
  Rust: the same for `validate_capnp` / `from_capnp` /
  `CapnpUnpack::capnp_unpack`; the view of the `CapnpView` derive checks
  the structure only.
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

## Schema

A type described once, by its reflection, has one Cap'n Proto schema, and
it is the schema of the bytes `psio::capnp` writes:

```cpp
psio::SchemaBuilder b;
b.insert<Order>("Order");
psio::Schema schema = b.build();

std::string text = psio::emit_capnp(schema);               // .capnp text for the compiler
psio::Schema back = psio::parse_capnp(text);               // .capnp text → Schema IR
auto layouts = psio::capnp_layout(back);                   // sections, offsets, discriminants
layouts.find("Order")->field("total")->bit_offset();
```

(`<psio/emit_capnp.hpp>`, `<psio/capnp_parser.hpp>`, `<psio/capnp_layout.hpp>`;
the cases of `cpp/tests/emit_capnp_tests.cpp`, `capnp_parser_tests.cpp`,
`capnp_schema_tests.cpp`.)

**What is guaranteed, and how it is checked** (against `capnp` 1.3.0,
wherever the tool and the library are installed):

* **Export.** `capnp compile` accepts `emit_capnp` of a reflected type and
  lays it out as the codec does. `psio_capnp_schema_interop_tests` is built
  from the code `capnp compile -oc++` generates for a schema that
  `psio::emit_capnp` writes at build time: every value is read and written
  member by member through the compiled schema (libcapnp's dynamic API over
  the generated types) — a message psio writes is read by libcapnp as the
  value, a message libcapnp writes validates and decodes in psio to it;
  defaults: a member at its C++ initialiser is zero on the wire and libcapnp
  reads the initialiser, and an empty libcapnp message decodes to the
  default-constructed value. For 54 reflected records (the 48 of
  `capnp_layout_fixture.hpp` among them) the compiler's layout of the
  emitted text is `psio::capnp::layout<T>`.
* **Import.** The layout `capnp_layout(parse_capnp(text))` computes is the
  one in the `CodeGeneratorRequest` `capnp compile -o-` writes for the text:
  data and pointer section sizes, every field's ordinal, offset, size and
  default (the bits XORed with the value), groups, the position of every
  union's discriminant and the value of each member. On two hand-written
  schemas (`cpp/tests/capnp/schema_all.capnp` has one of everything) and on
  300 generated ones — groups in unions, unions in groups in unions,
  ordinals in any order, a union with an ordinal of its own
  (`psio_capnp_schema_tests`). `capnp_layout` is a port of the compiler's
  allocator for any nesting; the codec's compile-time one handles the
  shapes a C++ type produces, and the two agree on those.
* **Round trip.** `emit_capnp(parse_capnp(text))` declares what `text`
  declares: the compiler's own listing of the two (`capnp compile -ocapnp`:
  ids, ordinals, types, defaults, annotations, layout) has the same lines.
  The emitted text is a fixed point, and `parse_capnp(emit_capnp(p)) == p`
  for a schema `p` that `parse_capnp` produced.
* **Untrusted text.** `parse_capnp` bounds its nesting (64 levels),
  allocates in proportion to the text and reports every malformed input as
  `psio::capnp_parse_error` (line, column). `psio_schema_text_fuzz_tests`
  (and the same with AddressSanitizer and UndefinedBehaviorSanitizer): every
  prefix and every byte of the test schemas, random edits and random bytes —
  109,098 texts, of which 8,387 were accepted and written back, each to a
  fixed point — and inputs built to exhaust a parser (200,000 nested
  `List(`, groups and structs; a struct of 65,536 fields; a union of 30,000
  members; 20,000 chained types and aliases).

**How a schema is read as Cap'n Proto** (the header of `capnp_layout.hpp`
is the full statement):

| Schema IR | Cap'n Proto |
|---|---|
| `Object`, `Struct` (a named type) | `struct` |
| `Enum` over `UInt16` with the values 0, 1, … | `enum`; any other enum is its integer, which is what the codec writes for a C++ enum |
| a member that is a `Variant` | `name :union { … }`, one member per alternative; the member named `""` is the struct's unnamed union |
| a member that is an `Object` written in place | `name :group { … }` |
| `Option<data type>` | `name :union { none @n :Void; some @n+1 :T; }` |
| `Option<pointer type>` | the field |
| `Resource` | `interface` |
| `Tuple{}` | `Void` |

Attributes: `ordinal` on every field of a parsed schema; without ordinals
(a reflected type) they follow the members in ascending order of `field`,
a union taking one per member — the codec's rule. `default` (a data
field's default in its normal form; a pointer field's as Cap'n Proto writes
it), `capnp_id` (a type's or the file's `@0x…`).

**Names.** Cap'n Proto requires `TypeName` and `fieldName` without
underscores. A member is written under the camelCase of its name with
`$Json.name("the_name")` — the official annotation of
`capnp/compat/json.capnp` — where the two differ, and `parse_capnp` reads
the member's name from it; the file then imports that schema. So C++ member
names survive the round trip, and libcapnp's JSON codec writes them.

**What the text cannot say.** A `std::optional<std::uint32_t>` and a
hand-written `union { none; some }` are the same declaration, and
`parse_capnp` reads both as the optional. A union's member values are the
ranks of their ordinals, not their order in the text.

**One difference from the compiler.** For a few shapes — a union member
that grows inside a group's location — `capnp compile` stops by default
(Cap'n Proto 0.5 laid them out wrongly; issue 344) and compiles them only
with `CAPNP_IGNORE_ISSUE_344` set. `capnp_layout` computes the layout the
compiler computes with the variable set. No type the codec maps has such a
shape.

Not done: `import` is kept and written back but not followed (the layout
refuses a type the text does not declare); generic types and methods are
refused; `const` and `annotation` declarations are kept as written and not
interpreted (a default that names a constant is refused). Rust, JS and
Python have no `emit_capnp` / `parse_capnp`.

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
