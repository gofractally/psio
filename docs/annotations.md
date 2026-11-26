# PSIO type annotations — reference

One reflected declaration with annotations is the type in every PSIO format:
PSSZ bytes, database keys, JSON / I-JSON text, the Schema IR, WIT, GraphQL.
This document says what every annotation means, where it may be written,
which formats honour it, where it is enforced, and what may change after a
type is published.

- Guided introduction: [tutorial-annotations.md](tutorial-annotations.md).
- Rules and checklists for AI coding agents and reviewers:
  [annotations-for-agents.md](annotations-for-agents.md).
- Wire format: [pssz-spec.md](pssz-spec.md). Text: [json.md](json.md).
  Keys: [key-spec.md](key-spec.md). Helper names: [helpers.md](helpers.md).

**How this document is checked.** Every C++ snippet is a run of consecutive
lines of [`cpp/tests/doc_annotations_examples.cpp`](../cpp/tests/doc_annotations_examples.cpp)
(test `psio_doc_annotations_examples`); the section name given with a
snippet (`ref-length`, `tut-2`, …) is the comment that starts it in that
file. The Rust, TypeScript and Python snippets are **not compile-checked
from this document**: nothing verifies that their text is the text of a
test. Each language has a test that declares the same types and pins the
same bytes
([Rust, TypeScript and Python snippets](#rust-typescript-and-python-snippets)).

## Contents

1. [One declaration, every format](#1-one-declaration-every-format)
2. [Where annotations are written](#2-where-annotations-are-written)
3. [Width: what a bound buys](#3-width-what-a-bound-buys)
4. [Master table](#4-master-table)
5. [The annotations](#5-the-annotations)
6. [How do I annotate …](#6-how-do-i-annotate-)
7. [Reading: views, validation, trust](#7-reading-views-validation-trust)
8. [Evolution: what may change after publication](#8-evolution-what-may-change-after-publication)
9. [Common mistakes and their compiler errors](#9-common-mistakes-and-their-compiler-errors)
10. [Rust, TypeScript and Python snippets](#rust-typescript-and-python-snippets)

Looking for a number to write in `maxDynamicData`? [3.4 How to pick a cap](#34-how-to-pick-a-cap).

## 1. One declaration, every format

Headers: `<psio/psio.hpp>`, or the single ones (`psio/reflect.hpp`,
`psio/pssz.hpp`, `psio/json.hpp`, `psio/key.hpp`, `psio/max_size.hpp`, …).

```cpp
struct Account
{
   friend bool operator==(const Account&, const Account&) = default;
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(Account,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance)
};
// tags:   8 slots + 8 x 16 = 136 <= 255: the list's own table is 1 byte per slot.
// record: (header + 2 slots) x 1 byte + id 8 + owner 32 + tags 136 + balance 8.
static_assert(psio::inferred_bound<Account>() == 3 + 8 + 32 + (8 + 8 * 16) + 8);
static_assert(psio::inferred_bound<Account>() == 187);        // <= 255: 8-bit offsets
static_assert(psio::auto_pssz_width_v<Account> == 1);
```

(`tut-2b`.) The member types are the ones the program wants — `std::string`,
`std::vector<std::string>`. The annotations say what the values may be, and
every format follows from that one statement:

| Format | What the annotations above do |
|---|---|
| **pssz** (`psio::to_pssz`, `to_pssz_checked`, `from_pssz`, `validate_pssz`) | The record's offsets are 8-bit instead of 32-bit (the bounds add up to 187 bytes), and so is the offset table of `tags`. Bytes that hold a value outside a bound are rejected by every validating reader; the validated write (`to_pssz_checked`) refuses the value; the fast write (`to_pssz`) refuses it only when the offsets cannot express it ([7.2](#72-reading-and-writing-the-two-modes)). |
| **key** (`psio::to_key`, `to_key_checked`, `from_key`, `validate_key`) | No change to the bytes. The readers reject a key that holds a value outside a bound or a `utf8` member that is not UTF-8; the validated write (`to_key_checked` throws, `psio::encode_key_checked` returns a status) refuses the value; the fast write (`to_key`) writes it ([7.2](#72-reading-and-writing-the-two-modes)). |
| **json / ijson** (`psio::to_json`, `to_json_checked`, …) | No change to the text. The readers and the validated write refuse a value outside a bound; the fast write does not look. |
| **Schema IR** (`psio::SchemaBuilder`) | `owner` is `Custom{BoundedList<u8, 32>, "utf8"}`, `tags` is `BoundedList<Custom{BoundedList<u8, 16>, "utf8"}, 8>`: a reader that has only the schema derives the same width and enforces the same bounds. |
| **WIT** (`psio::emit_wit`) | A bounded list is `list<T>/* @psio:max=N */`; a record cap is `@maxDynamicData(N)`. A string's bound is not shown. |
| **GraphQL** (`psio/graphql.hpp`) | Bounds are not part of the GraphQL schema ([graphql.md](graphql.md)). |
| **flatbuf, capnp, protobuf, bin, ssz, borsh, bincode, msgpack, avro, bson, wit ABI** (`psio::validate_F`, `from_F`, `to_F_checked`, …) | No change to the bytes: they are the ones the format's own specification defines. Every validating read path refuses a value outside a member's bound, a `utf8` member that is not UTF-8, a list outside its order spec and a value its invariant rejects — the structure of the bytes first, then the declarations on the value — and the validated write refuses the same value; the fast write (`to_F`) does not look ([7.2](#72-reading-and-writing-the-two-modes)). `validate` also applies a root type's `maxDynamicData` to the size of its own input ([5.4](#54-maxdynamicdatan)); protobuf, capnp and flatbuf read `field<N>`, flatbuf also `deprecated_spec` ([5.12](#512-psio_reflect_enum-and-fieldn)). |

The owner's rules this design implements:

- **Annotate once.** Nothing is repeated per format.
- **Keep your types.** A bound is written on the member, not by changing
  its type. A wrapper type (`psio::bounded<T, N>`, `psio::utf8_string<N>`)
  says the same thing in the type for code that prefers that; the two
  spellings are byte-identical ([2.3](#23-wrapper-types)).
- **Width is part of the type.** An explicit cap always rules; otherwise
  the inferred bound, rounded up to the next offset width. The only spare
  capacity is the rounding ([3](#3-width-what-a-bound-buys)). Every type
  that has offsets — a record, a tuple, a list, a map — has its own width.
- **Bounds are enforced**, not advisory.

## 2. Where annotations are written

### 2.1 Inside `PSIO_REFLECT`

`PSIO_REFLECT(Type, item...)` goes right after the type at namespace scope
(any namespace, unqualified type name) or inside the class body (which also
sees private members). A function-local struct cannot be reflected. Items:

| Item | Meaning |
|---|---|
| `field` | a data member, no annotation |
| `attr(field, spec)` / `attr(field, spec \| spec \| …)` | a data member with annotations |
| `method(name, item…)` | a member function, its parameters, its caps and its result ([5.11](#511-methodname-)) |
| `definitionWillNotChange()` | fixed layout ([5.5](#55-definitionwillnotchange)) |
| `maxDynamicData(N)` | size cap and width budget ([5.4](#54-maxdynamicdatan)) |
| `maxFields(N)` | cap on the number of fields, N ≤ 255 ([5.6](#56-maxfieldsn)) |
| `transparent()` | the type is its one member ([5.7](#57-transparent)) |
| `invariant(fn)` | a value check ([5.8](#58-invariantfn)) |
| `schemaId("Id")` | the type's id in a schema, and nothing else ([5.15](#515-schemaidid)) |

The spec expression of `attr` is evaluated with `using namespace psio` in
effect, so the helpers are written bare: `max<32>`, `utf8`, `element(...)`.
Order of items does not matter except that data members are reflected in
the order listed, and that order is the wire order.

### 2.2 Separate statements

For a type whose `PSIO_REFLECT` you cannot or do not want to touch, at
namespace scope (the type's namespace, an enclosing one, or global), with
qualified helper names:

```cpp
struct Separate    // separate statements, after the type
{
   std::string                title;
   std::vector<std::uint32_t> ids;
};
PSIO_REFLECT(Separate, title, ids)
PSIO_FIELD_ATTRS(Separate, title, psio::utf8 | psio::max<32>)
PSIO_FIELD_ATTRS(Separate, ids, psio::max<4>)
PSIO_TYPE_ATTRS(Separate, psio::max_dynamic_data_spec{200})
```

(`ref-spellings`.) `PSIO_ATTRS(Type, (field, (spec)), …)` is the
several-fields form of `PSIO_FIELD_ATTRS`. Rules:

- The two forms compose when they give different spec types. The same spec
  type declared twice for one member or type is a compile error.
- A separate statement must come before the first use of the annotations it
  adds to; a later one is a compile error, never silently ignored.
- Type-level keywords have these spec values: `psio::definition_will_not_change{}`
  (or `psio::dwnc`), `psio::max_dynamic_data_spec{N}`, `psio::max_fields_spec{N}`,
  `psio::transparent`.

### 2.3 Wrapper types

```cpp
struct InTypes     // wrapper types
{
   psio::utf8_string<32>            title;
   psio::bounded<std::uint32_t, 4>  ids;
   PSIO_REFLECT(InTypes, title, ids, maxDynamicData(200))
};

static_assert(psio::auto_pssz_width_v<InReflect> == 1);
static_assert(psio::auto_pssz_width_v<Separate> == 1);
static_assert(psio::auto_pssz_width_v<InTypes> == 1);
```

(`ref-spellings`, with `InReflect` below.) The three declarations —

```cpp
struct InReflect   // inside PSIO_REFLECT
{
   friend bool operator==(const InReflect&, const InReflect&) = default;
   std::string                title;
   std::vector<std::uint32_t> ids;
   PSIO_REFLECT(InReflect, attr(title, utf8 | max<32>), attr(ids, max<4>), maxDynamicData(200))
};
```

— `Separate` and `InTypes` are one type on the wire: the same pssz bytes,
the same offset widths, the same JSON, the same key, the same Schema IR
nodes (test `reference: the three spellings are one type`).

| Constraint | Annotation on the plain type | Wrapper type |
|---|---|---|
| UTF-8 string | `std::string` + `utf8` | `psio::utf8_string<>` |
| UTF-8 string, ≤ N bytes | `std::string` + `utf8 \| max<N>` | `psio::utf8_string<N>` |
| raw string, ≤ N bytes | `std::string` + `max<N>` | — |
| list, ≤ N elements | `std::vector<T>` + `max<N>` | `psio::bounded<T, N>` |
| exactly N opaque bytes | `std::array<std::uint8_t, N>` | `psio::byte_array<N>` |
| map / set, ≤ N entries | `max<N>` on the member | — (there is no bounded map type) |
| a named, first-class bounded type (a memo, a tag list) | — | a `transparent()` type whose one member is annotated ([5.7](#57-transparent)) |

What differs is the C++ side only: a wrapper checks its length bound at
construction and `push_back` (it throws `std::length_error`, or aborts in a
build without exceptions); an annotated plain member is checked when it
crosses a codec. Do not state a bound both ways: a `length_bound` written
over a wrapper type is enforced but does not change the inferred bound or
the width, which stay the wrapper's `N` (`psio/max_size.hpp`).

### 2.4 Precedence

For one member: member annotation > the member type's wrapper-inherent
annotation > the enclosing type's type-level annotation > the format's
default. Type-level annotations that act as a default for members are the
integer text forms (`PSIO_TYPE_ATTRS(T, psio::ijson_number)`).

### 2.5 Rust, TypeScript and Python

Rust: `#[psio(...)]` describes the type for every derive (`PsszPack`,
`PsszUnpack`, `PsszView`, `JsonText`, `Key`, `ToPsszSchema`);
`#[pssz(...)]` and `#[json(...)]` hold what is specific to one format. A key
under the wrong attribute is a compile error that names the right one
(`rust/psio/src/lib.rs`, "Annotations").

TypeScript: types are values built with the functions of `js/src/types.ts`
(`record`, `field`, `list`, …); annotations are the option objects of
`field(type, options)` and `record(name, fields, options)`.

Python (`python/`, [README](../python/README.md)): the types are the ones
the program has — a `@dataclass`, `str`, `list`, `dict`, `Optional`, an
`enum.Enum`. Member annotations are `typing.Annotated` metadata
(`Annotated[str, psio.max(32)]`); type-level ones are the decorators
`@psio.record(...)`, `@psio.transparent`, `@psio.enum(...)`,
`@psio.interface` / `@psio.method(...)`, and the method markers
`@psio.invariant`, `@psio.to_text`, `@psio.from_text`. An integer needs a
width, so integer hints are `psio.u8` … `psio.u64`, `psio.i8` … (aliases of
`Annotated[int, ...]`). Import the names you use: `from psio import *`
would shadow the builtins `max` and `min`.

## 3. Width: what a bound buys

Every pssz type that has offsets writes them 1, 2 or 4 bytes wide:

- a **record**, a **tuple** and a **pair** write a header and one offset
  slot per variable-size member;
- a **list**, a **map**, a **set** and an **array** of variable-size
  elements write an offset table of one slot per element.

The width `W` is a compile-time property of the type, and **every type that
has offsets has its own**: a record can have 2-byte slots and hold a list
whose table is 1 byte wide ([pssz-spec.md §3](pssz-spec.md#3-width-selection)).
Three rules:

1. **An explicit cap always rules.** A record (or a method's argument list)
   with `maxDynamicData(N)`: `N ≤ 255` → 1 byte, `N ≤ 65535` → 2 bytes,
   otherwise 4. The cap is not reduced to what the current fields need.
2. **Otherwise the inferred bound**, `psio::inferred_bound<T>()`: the
   largest encoding the bounds allow, rounded up to the next width. (For a
   type with no cap this is also `psio::max_encoded_size<T>()`, the largest
   encoding; under a cap the two differ, [5.4](#54-maxdynamicdatan).) If any
   part is unbounded there is no bound and `W = 4`. Lists, maps, sets,
   arrays, tuples and pairs have no cap of their own and always use this
   rule — together with the cap of the record they are in
   ([3.3](#33-a-cap-narrows-what-lies-beneath-it)).
3. **The only spare capacity is the rounding.** A type whose bound is 187
   has 68 bytes of slack before it would become 16-bit; a type whose bound
   is 1206 has until 65535.

`W` is never chosen from the size of a value: every value of a type is
written with that type's widths, and has exactly one encoding.

Check it where the type is declared:

```cpp
static_assert(psio::inferred_bound<Account>() == 3 + 8 + 32 + (8 + 8 * 16) + 8);
static_assert(psio::inferred_bound<Account>() == 187);        // <= 255: 8-bit offsets
static_assert(psio::auto_pssz_width_v<Account> == 1);
```

### 3.1 How the inferred bound is counted

([pssz-spec.md §3.2](pssz-spec.md#32-inferred_boundt).) A type that has
offsets consists of `k` words of `W` bytes each — its header, its slots,
its table entries — and of other bytes, at most `r`: the fixed-size members
and the payloads. Its bound counts the `k` words **at the width the type
ends up with**:

```text
fit(k, r) = k + r      if  k + r  <= 255       W = 1
          = 2k + r     if 2k + r  <= 65535     W = 2
          = 4k + r     otherwise               W = 4
```

| Part | Contribution |
|---|---|
| extensible record, tuple, pair | `fit(1 + v, members)`: a header and `v` slots, `v` = the number of variable-size members |
| `definitionWillNotChange()` record | `fit(v, members)`: no header |
| fixed-size member (integer, bool, enum, float, fixed array, `bitvector<N>`, DWNC record of fixed members) | its size; no slot |
| variable-size member | one slot (counted in `v`) and its own bound |
| string / byte list with `max<N>` or `exact<N>` | N (no bookkeeping of its own) |
| list of ≤ N fixed-size elements | N × element size (no table) |
| list or array of N variable-size elements | `fit(N, N × element bound)`: a table of N slots, then the elements |
| `std::optional<T>` | 1 + bound of T (one selector byte is always reserved) |
| `std::variant<…>` | 1 + the largest alternative |
| map of ≤ N entries | as a list of N `(key, value)` tuples; set: as a list of N keys |
| `psio::nested<T>` with member `max<N>` | N |
| a member whose type has its own `maxDynamicData(N)` | N in full, not the member's inferred bound |
| `psio::bitvector<N>` | ⌈N/8⌉ bytes, fixed size ([6.20](#620-bit-types)) |
| `psio::bitlist<N>` | ⌈N/8⌉ + 1 (the bits and the delimiter bit), variable size |
| unbounded string / list / map / set / nested; a boxed value; a member with an `as<Tag>` override whose adapter writes the binary form | no bound → the enclosing type is 4-byte |
| a member with a text-only `as<Tag>` (`as<hex_tag>` on a wide integer) | the member's own contribution: the annotation is not seen by the binary formats ([5.10](#510-custom-text-psio_adapter-astag)) |

A type holding another uses that type's finished bound, which already
contains the inner type's bookkeeping at the inner type's own width, and
adds its own words at its own width. For `Account`: `tags` is
`fit(8, 8 × 16)` = 8 + 128 = 136 (a 1-byte table), and the record is
`fit(1 + 2, 8 + 32 + 136 + 8)` = 3 + 184 = 187. For `AccountWide`
(`tags` up to 64 strings): 64 + 1024 does not fit in 255, so the list has
2-byte slots and is 128 + 1024 = 1152; the record is 2 × 3 + 1200 = 1206
(`tut-2a`).

`min<N>` alone bounds nothing.

### 3.2 Where the width changes

Because a type's own words are counted at its own width, a type is narrow
exactly when its worst case can be addressed at that width:

```cpp
struct Raw253   // header 1 + slot 1 + 253 = 255: the last 1-byte record of this shape
{
   std::string s;
   PSIO_REFLECT(Raw253, attr(s, max<253>))
};
struct Raw254   // 1 + 1 + 254 = 256 does not fit: header and slot are 2 bytes
{
   std::string s;
   PSIO_REFLECT(Raw254, attr(s, max<254>))
};
static_assert(psio::inferred_bound<Raw253>() == 255 && psio::auto_pssz_width_v<Raw253> == 1);
static_assert(psio::inferred_bound<Raw254>() == 258 && psio::auto_pssz_width_v<Raw254> == 2);
struct Raw65531   // 2 x 2 + 65531 = 65535: the last 2-byte record of this shape
{
   std::string s;
   PSIO_REFLECT(Raw65531, attr(s, max<65531>))
};
struct Raw65532   // header and slot are 4 bytes
{
   std::string s;
   PSIO_REFLECT(Raw65532, attr(s, max<65532>))
};
static_assert(psio::inferred_bound<Raw65531>() == 65535 && psio::auto_pssz_width_v<Raw65531> == 2);
static_assert(psio::inferred_bound<Raw65532>() == 65540 && psio::auto_pssz_width_v<Raw65532> == 4);
// A list measures itself the same way: one slot per element, then the elements.
using Tags15x16 = psio::bounded<psio::utf8_string<16>, 15>;   // 15 + 15 x 16 = 255
using Tags15x17 = psio::bounded<psio::utf8_string<17>, 15>;   // 15 + 255 > 255: 2 x 15 + 255
static_assert(psio::inferred_bound<Tags15x16>() == 255 && psio::auto_pssz_width_v<Tags15x16> == 1);
static_assert(psio::inferred_bound<Tags15x17>() == 285 && psio::auto_pssz_width_v<Tags15x17> == 2);
```

| Shape | Last 1-byte | Last 2-byte |
|---|---|---|
| extensible record of one raw / UTF-8 string `max<N>` | N = 253 (1 + 1 + 253 = 255) | N = 65531 (2 + 2 + 65531 = 65535) |
| extensible record of fixed-size members only | 254 bytes of members | 65533 |
| list of N strings of ≤ M bytes | N + N × M ≤ 255 (15 × 16) | 2N + N × M ≤ 65535 (255 × 255) |

(An earlier revision of the format counted every header and slot as 4
bytes in the bound and wrote every list table 4 bytes wide; the steps were
at 247 / 248 and 65527 / 65528 for the first row. A type declared under
that rule whose bound lies in between is now one class narrower:
re-check its `static_assert`.)

### 3.3 A cap narrows what lies beneath it

A record with `maxDynamicData(N)` never encodes to more than N bytes, so
nothing inside it does. The cap is therefore the implicit bound of every
list, map, set, array, tuple and pair beneath the record, **for the purpose
of their offset widths**
([pssz-spec.md §3.6](pssz-spec.md#36-a-records-cap-bounds-what-lies-beneath-it)):

```text
W of a list / map / set / array / tuple / pair
   = width class of min(its own bound at that position, the cap of the nearest enclosing record)
```

```cpp
struct Batch   // plain member types; the cap is the only bound
{
   friend bool operator==(const Batch&, const Batch&) = default;
   std::uint32_t                      id = 0;
   std::vector<std::string>           tags;     // under the cap: a 1-byte offset table
   std::map<std::string, std::string> labels;   // 1-byte table, 1-byte entry header and slots
   PSIO_REFLECT(Batch, id, tags, labels, maxDynamicData(255))
};
struct BatchOpen   // the same members with no cap: everything is 4 bytes wide
{
   std::uint32_t                      id = 0;
   std::vector<std::string>           tags;
   std::map<std::string, std::string> labels;
   PSIO_REFLECT(BatchOpen, id, tags, labels)
};
static_assert(psio::auto_pssz_width_v<Batch> == 1);
static_assert(psio::auto_pssz_width_v<BatchOpen> == 4);
```

```cpp
const Batch     capped{1, {"a", "bc"}, {{"k", "v"}}};
const BatchOpen open{1, {"a", "bc"}, {{"k", "v"}}};
// tags is the table 02 03, then "a" "bc"; labels is the table 01, then one
// entry: header 02, slots 02 03, "k" "v". Every one of them is 1 byte wide.
CHECK(hex(psio::to_pssz(capped)) ==
      "06 01 00 00 00 06 0b 02 03 61 62 63 01 02 02 03 6b 76");
CHECK(psio::to_pssz(capped).size() == 18);
// Without the cap the same value has 4-byte headers, slots and tables.
CHECK(psio::to_pssz(open).size() == 45);
CHECK(psio::from_pssz<Batch>(psio::to_pssz(capped)) == capped);
// A view follows the same widths.
const auto                   bytes = psio::to_pssz(capped);
const psio::pssz_view<Batch> view{bytes};
CHECK(view.get<1>()[1].view_() == "bc");
```

What the rule does and does not do:

- **It only selects widths.** It adds no check (the record's own cap check
  already covers every byte beneath it), changes no inferred bound (a
  capped record contributes its cap to whatever holds it), and does not
  change the schema or the member's C++ type: `tags` is still a
  `std::vector<std::string>`, and an unbounded list in the Schema IR.
- **The nearest enclosing record decides.** The walk up from a value passes
  through lists, arrays, maps, sets, optionals, variants, tuples, pairs,
  boxes and transparent types and stops at the first record — a reflected
  record or a method's argument list — taking that record's cap, or
  nothing if it has none. It also stops, with nothing, at a
  `psio::nested<T>` boundary: the inner value is a root of its own.
- **A tighter bound of the position's own wins; a looser one is clamped.**
  `max<8> | element(max<16>)` (136 bytes) under a cap of 60000 has a 1-byte
  table. `max<1000> | element(max<1000>)` under a cap of 255 has a 1-byte
  table too; the bound is accepted without a diagnostic and still limits
  the count and the element size.
- **Strings, byte lists and lists of fixed-size elements have nothing to
  narrow.** Their payload is the raw bytes with no length prefix and no
  table; all they cost is one slot in the record that holds them, as wide
  as that record. Under a cap of 255 a `std::string` member costs exactly
  one byte beyond its characters, bounded or not.
- **The explicit profiles** (`psio::pssz_<1>`, `pssz_<2>`, `pssz_<4>`)
  ignore it, as they ignore every inference.

**A worked example.** An action's arguments under the largest cap that
keeps one-byte offsets:

```cpp
struct Transfer
{
   friend bool operator==(const Transfer&, const Transfer&) = default;
   std::uint32_t                            token  = 0;
   std::uint64_t                            from   = 0;
   std::uint64_t                            amount = 0;
   std::string                              memo;
   std::optional<std::vector<std::uint8_t>> data;    // unbounded: the cap is its only limit
   PSIO_REFLECT(Transfer, token, from, amount, attr(memo, utf8 | max<80>), data,
                maxDynamicData(255))
};
static_assert(psio::auto_pssz_width_v<Transfer> == 1);   // 255: the largest cap with 1-byte offsets
```

```cpp
// Framing: header 1 + token 4 + from 8 + amount 8 + 2 slots = 23 bytes.
// The other 232 bytes are shared by memo and data; a present data spends
// one of them on its selector byte.
const std::string memo(80, 'm');
const Transfer    full{1, 2, 3, memo, std::vector<std::uint8_t>(151, 0)};
CHECK(psio::to_pssz(full).size() == 23 + 80 + 1 + 151);               // 255: exactly the cap
CHECK(psio::from_pssz<Transfer>(psio::to_pssz(full)) == full);
// One byte more has no encoding ...
CHECK_THROWS_AS(psio::to_pssz(Transfer{1, 2, 3, memo, std::vector<std::uint8_t>(152, 0)}),
                psio::codec_exception);
// ... and a short memo leaves its bytes to data.
CHECK(psio::to_pssz(Transfer{1, 2, 3, "", std::vector<std::uint8_t>(231, 0)}).size() == 255);
// An absent trailing data is pruned with its slot: 22 bytes of framing.
CHECK(psio::to_pssz(Transfer{1, 2, 3, "rent", std::nullopt}).size() == 22 + 4);
```

The framing is 23 bytes — a 1-byte header, 20 bytes of integers and two
1-byte slots — and the remaining 232 bytes are shared by `memo` and `data`.
`memo` has its own limit (80, checked as such); `data` has none, and needs
none to be narrow: the cap is its limit. With a cap of 256 the same type
has 2-byte offsets.

**A record keeps its own width.** A reflected record beneath a capped
record is *not* narrowed: the width of a record is part of the record
type, and its bytes are the same wherever it appears. That is what lets
`psio::pssz_view<Child>`, `psio::nested<Child>` and every schema that names
`Child` read it without knowing what holds it. The positions beneath the
inner record take the *inner* record's cap, or none.

```cpp
struct Item   // unbounded: 4-byte offsets, wherever it appears
{
   friend bool operator==(const Item&, const Item&) = default;
   std::uint32_t id = 0;
   std::string   name;
   PSIO_REFLECT(Item, id, name)
};
struct Order
{
   std::uint32_t     id = 0;
   std::vector<Item> items;   // the list's table is 1 byte; each Item still has 4-byte offsets
   PSIO_REFLECT(Order, id, items, maxDynamicData(255))
};
static_assert(psio::auto_pssz_width_v<Item> == 4);
static_assert(psio::pssz_child_wider_than_cap_v<Order>);   // Item cannot use its width here

struct SmallItem   // bounded: header 1 + id 4 + slot 1 + 32 = 38, 1-byte offsets
{
   friend bool operator==(const SmallItem&, const SmallItem&) = default;
   std::uint32_t id = 0;
   std::string   name;
   PSIO_REFLECT(SmallItem, id, attr(name, utf8 | max<32>))
};
struct SmallOrder
{
   std::uint32_t          id = 0;
   std::vector<SmallItem> items;
   PSIO_REFLECT(SmallOrder, id, items, maxDynamicData(255))
};
static_assert(psio::auto_pssz_width_v<SmallItem> == 1);
static_assert(!psio::pssz_child_wider_than_cap_v<SmallOrder>);
```

```cpp
// The list's table is 1 byte wide under the cap; each Item is the same
// bytes it is everywhere else, with its own 4-byte header and slot.
const Item item{7, "x"};
CHECK(psio::to_pssz(item).size() == 4 + 4 + 4 + 1);
CHECK(psio::to_pssz(Order{1, {item}}).size() == (1 + 4 + 1) + 1 + 13);
// The bounded child is 1 + 4 + 1 + 1 = 7 bytes.
CHECK(psio::to_pssz(SmallOrder{1, {SmallItem{7, "x"}}}).size() == (1 + 4 + 1) + 1 + 7);
```

`psio::pssz_child_wider_than_cap_v<T>` is true when `T` has an explicit cap
and a record (or method argument list) wider than the cap's width class
lies beneath it with no other record in between. It is a query, not an
error: such types are valid and sometimes intended (a capped envelope
around an open-ended payload record). When it is not intended, give the
child a `maxDynamicData` or bound its members, and assert
`!psio::pssz_child_wider_than_cap_v<Parent>` next to the parent.

### 3.4 How to pick a cap

1. **Enforced limits first.** Write the limits the application really has
   as member bounds (`max<N>`, `element(...)`, `utf8`). They are checked
   one by one, in every format that enforces bounds — the text formats and
   keys included, where the cap is not applied to members. The cap is a
   second, coarser limit on the whole encoding — also enforced: a value
   whose encoding exceeds it has no encoding.
2. **Choose the width class, then the number.** Only the class changes
   bytes: `N ≤ 255` → 1-byte offsets, `N ≤ 65535` → 2-byte, above → 4-byte.
   Inside a class, N is purely the size limit.

   | Kind of type | Aim for | Why |
   |---|---|---|
   | database row, table record | **16-bit**: a cap ≤ 65535 | 64 KB is more than a row should hold; costs 1 byte per header / slot / table entry over 8-bit, saves 2 over 32-bit |
   | action / method arguments | 16-bit; 8-bit (255) for small fixed-purpose calls such as the transfer above | arguments are published API and gain parameters |
   | small value that will stay small (a key, a short label record) | 8-bit: a cap ≤ 255 | every byte of framing counts |
   | closed forever (a point, a hash, an id pair) | no cap: `definitionWillNotChange()` | no header at all ([5.5](#55-definitionwillnotchange)) |
   | document, blob, anything with genuinely unbounded content | no cap, or a cap > 65535 | 32-bit offsets |

3. **Reserve headroom with `maxDynamicData`, at first publication.** Adding
   an optional field must not change the width
   ([8.1](#81-the-compatible-change)). A type without a cap has only the
   slack between its inferred bound and 255 / 65535 — `Account` has 68
   bytes — and the cap cannot be added later if that changes `W`. Make N
   at least the inferred bound of today's fields plus everything the type
   may gain.
4. **When unsure, go one size up.** A class that is too small can never be
   left (the wider type is a different type, and stored data must be
   rewritten); a class that is too large costs 1 or 2 bytes per header,
   slot and table entry. Within a class, prefer the top (255, 65535) unless
   the size limit itself matters.
5. **A cap makes the plain containers beneath it narrow**
   ([3.3](#33-a-cap-narrows-what-lies-beneath-it)), so under a cap you do
   not need to invent bounds only to get small tables. Child *records* are
   the exception: give each its own cap or bounds, and assert
   `!psio::pssz_child_wider_than_cap_v<T>`.
6. **Assert what you chose** next to the type: `psio::auto_pssz_width_v<T>`,
   and `psio::max_dynamic_data_v<T>` or `psio::inferred_bound<T>()`
   (`psio::max_encoded_size<T>()` when you size a buffer).
7. **The same N in every language** that declares the type.
8. **A byte cap is not a field cap.** `maxDynamicData(N)` limits the encoded
   *bytes* and selects the width. `maxFields(N)` limits the *number of
   fields* (or of a method's parameters) the type will ever have, N ≤ 255;
   it changes no byte and no width ([5.6](#56-maxfieldsn)). Declare
   `maxFields` when a consumer sizes something by the field count — a
   dispatch table, a generated binding — and wants the promise in the
   schema; it does not replace the byte cap, and it can never be raised or
   dropped afterwards.
9. **Leave room in the member bounds too.** After publication a member
   bound may only **widen**, and only while every offset width stays the
   same; it can never tighten
   ([8.3](#83-annotations-bounds-may-only-widen)). A string's bound moves
   no width of its own (under a cap, none at all); a list's, map's or
   tuple's bound that crosses 255 / 65535 moves that container's offset
   table and is refused, cap or no cap. Pick bounds you will not need to
   lower, and list bounds you will not need to raise past a width step.
10. **A bounded value with a name is a type.** When the same bounded string
    or list appears in several records and argument lists — a memo, a URL,
    a tag list — declare it once as a `transparent()` type over an annotated
    member instead of repeating `utf8 | max<80>` at every use
    ([5.7](#57-transparent)). The bytes, widths and schema are those of the
    annotated member.

The cap limits the pssz encoding. JSON text is not limited by it
([5.4](#54-maxdynamicdatan), "the cap and text").

Practical guidance for types without a cap:

- A type with any unbounded member is 32-bit. Bounding *some* members buys
  nothing (`tut-2c`: `AccountCountOnly`).
- If the type may ever gain a field, give it an explicit cap now
  ([8](#8-evolution-what-may-change-after-publication)).

## 4. Master table

"n/a" = the language states this in the type; "—" = not applicable or not
planned; **not yet available** = the C++ behaviour of the consistency round
(merge `85e72cf`) has no committed counterpart in that language at the
time this table was written (the Rust, TypeScript and Python cells were
last checked against the tree in which all three run every row of
`conformance/interface-vectors.json` and `conformance/evolution-vectors.json`). The Python column uses the spellings of
`python/src/psio` ([README](../python/README.md)). Schema IR nodes are written as in
[json.md](json.md#the-schema-ir-as-json). Tooling that is not an annotation
— the compatibility check, the result codec, the name coders — is in
[4.1](#41-tooling-per-language).

| Annotation | C++ (`PSIO_REFLECT`) | Rust | TypeScript | Python | Schema IR | Honoured by |
|---|---|---|---|---|---|---|
| length ≤ N | `attr(f, max<N>)`; `psio::bounded<T, N>`, `psio::utf8_string<N>` | `#[psio(max = N)]`; `BoundedList<T, N>`, `BoundedString<N>`, `BoundedRawString<N>`, `BoundedMap<K, V, N>`, `BoundedSet<K, N>`, `Nested<T, V, N>` | `field(T, { max: N })`; `boundedList(T, N)`, `boundedUtf8(N)`, `boundedBytes(N)`, `map(K, V, { max: N })`, `set(K, { max: N })` | `Annotated[T, psio.max(N)]`; in a type alias: `Annotated[str, psio.bounded(N)]` | `BoundedList{type, maxCount}` (under `Custom "utf8"` / `"string"` / `"hex"` / `"map"` / `"set"`); `lengthBound="max=N"` on a `Nested` member | pssz (width + enforced), json/ijson, key and every binary format (enforced by the validating readers and the validated writes: [7.2](#72-reading-and-writing-the-two-modes)), schema-driven codecs, WIT (comment on lists) |
| length = N, length ≥ N, a range | `attr(f, exact<N>)`, `attr(f, min<N>)`, `attr(f, min<A> \| max<B>)` (= `psio::length_bound{.max = B, .min = A}`) — member only | — | `field(T, { lengthBound: { exact, min, max } })` — member only | `psio.exact(N)`, `psio.min(N)`, `psio.length(min=A, max=B)` — member only | member attribute `lengthBound="exact=N"` / `"min=A,max=B"` | as above (`min` alone gives no width) |
| UTF-8 text | `attr(f, utf8)`; `psio::utf8_string<>` | n/a: `String` is UTF-8, `RawString` is raw bytes | `utf8` / `boundedUtf8(N)`; `field(rawString, { utf8: true })` | n/a: `str` is UTF-8, `psio.RawString` is raw bytes (`bytes` in Python) | `Custom{…, "utf8"}` (raw: `"string"`) | pssz, json/ijson, key and every binary format (the validating readers and the validated writes; a protobuf `string` and JSON text are UTF-8 by their own format), schema-driven codecs |
| elements | `element(specs)` | `element(...)` | `element: { … }` | `psio.element(...)` | the element type's bounded node | as length |
| map keys / values | `keys(specs)`, `values(specs)` | `keys(...)`, `values(...)` | `keys: { … }`, `values: { … }` | `psio.keys(...)`, `psio.values(...)` | the entry tuple's bounded members | as length |
| tuple element / variant alternative | `at<I>(specs)` | tuple: `at(I, ...)`; variant: annotate the enum variant's fields | `at: { 0: { … } }` (tuple index, variant name or index) | `psio.at(i, ...)` (tuple index; union alternative index or name) | the member's bounded node | as length |
| size cap, width budget | `maxDynamicData(N)` | `#[pssz(max_dynamic_data = N)]` | `record(name, fields, { maxDynamicData: N })` | `@psio.record(max_dynamic_data=N)` | record attribute `maxDynamicData="N"` | pssz (width + enforced), schema-driven codecs, WIT `@maxDynamicData(N)`; the C++ `validate` of the other binary formats (bin, ssz, borsh, …; not json, not key) applies it to the root input size |
| fixed layout | `definitionWillNotChange()` | `#[psio(definition_will_not_change)]` | `record(name, fields, { fixed: true })` | `@psio.record(definition_will_not_change=True)` | record attribute `definitionWillNotChange` | pssz (no header), bin / borsh / bincode fast paths, evolution check |
| field-count cap | `maxFields(N)`, N ≤ 255, on a record or in `method(...)` | `#[psio(max_fields = N)]` on a record or an interface function | `record(name, fields, { maxFields: N })`, `argumentTuple(params, { maxFields: N })`, `method(params, { maxFields: N })` | `@psio.record(max_fields=N)`, `@psio.method(max_fields=N)` | attribute `maxFields="N"` on the record's `Object`, on an argument list's `Object` and on its `Func` | where the type is declared (C++ and Rust: compile time; TypeScript and Python: at declaration), the schema-driven codecs (a schema with more members than its `maxFields` is invalid), the compatibility checks of every language ([4.1](#41-tooling-per-language)), WIT `@maxFields(N)`. No byte, no width ([5.6](#56-maxfieldsn)) |
| the type is its member | `transparent()`; the member may carry `utf8`, `max<N>` and position specs | `#[psio(transparent)]`; the member may carry `#[psio(max = N)]` | `transparent(name, T)` with a bounded `T` (`boundedUtf8(80)`) | `NewType("Name", T)`; `@psio.transparent` on a one-field class, whose field may carry `psio.max(N)` and position bounds | a named alias of the member's (bounded) type | pssz, key, json/ijson, views, schema, GraphQL |
| value check | `invariant(fn)`, `PSIO_INVARIANT(T, fn)` | `#[psio(invariant = "path")]` | `custom(id, wire, { check })` | `@psio.invariant` on a method; `psio.custom(id, check=…)` | `Custom{wire type, id}` + a handler | the validating readers and the validated writes of every format; not the fast write, not the trusted decode, not views |
| schema id only | `schemaId("Id")` | `#[psio(custom = "Id")]` (without a text form or an invariant) | `custom(id, wire)` (no handler) | `psio.custom("Id")` (no functions) | `Custom{wire type, id}` | the Schema IR only: a schema-driven tool may register a handler for the id. No codec, validator or view reads it ([5.15](#515-schemaidid)) |
| integer text form | `attr(f, json_string)`, `json_number`, `ijson_string`, `ijson_number`; type level via `PSIO_TYPE_ATTRS` | `#[json(json_string)]`, … on a field or the type | `field(T, { jsonInt: 'string', ijsonInt: 'number' })` | `Annotated[u64, psio.json_string]`, `psio.json_number`, `psio.ijson_string`, `psio.ijson_number` | member attributes `jsonInt`, `ijsonInt` = `"number"` / `"string"` | json, ijson, schema-driven text |
| custom text form | `PSIO_ADAPTER(T, psio::text_category, Impl)`; per member `attr(f, as<Tag>)` | `#[psio(custom)]` + `psio::json_text_form!(T)` | `custom(id, wire, { toText, fromText })` | `@psio.to_text` / `@psio.from_text` methods; `psio.custom(id, to_text=…, from_text=…)` | `Custom{wire type, id}` (a member `as<Tag>` is not expressible) | json, ijson, GraphQL scalar; binary unchanged |
| enumerator names | `PSIO_REFLECT_ENUM(E, a, b, …)` | the derive on a `#[repr(int)]` enum; `#[psio(name = "…")]`, `#[psio(other)]` | `enumeration(u8, { a: 0, b: 2 })` | `@psio.enum(psio.u8)` on an `enum.Enum`; `other=True` | `Enum{type, members}` | json, ijson, GraphQL; binary is the integer |
| method arguments and result | `method(name, attr(p, specs)…, maxDynamicData(N), maxFields(N), returns(specs))` | `#[psio::interface]`: `#[psio(...)]` on a parameter, `#[pssz(max_dynamic_data = N)]`, `#[psio(max_fields = N)]`, `#[psio(positional)]` and `#[psio(returns(...))]` on the function | `defineInterface(name, { m: method(params, { returns, maxDynamicData: N, maxFields: N }) })`; a parameter and `returns` are `field(T, { … })`; `argumentTuple(params, { maxDynamicData: N })` for an argument list on its own | `@psio.interface` on a class; parameters and the return hint are `Annotated[...]`; `@psio.method(max_dynamic_data=N, max_fields=N)` | `Func{params, result, attributes: maxDynamicData, maxFields}`; the argument list is an `Object` with `tuple="named"` / `"positional"` | as records; JSON: a named list is an object or the positional array ([5.11](#511-methodname-)) |
| field number | `attr(f, field<N>)` | `#[psio(field = N)]` on a field | `field(T, { number: N })` | `psio.number(N)` | member attribute `field` = `"N"` (absent: the 1-based position) | protobuf (the field number), capnp (ordinals in ascending order of it), flatbuf (field ids in ascending order of it); `emit_protobuf`, `emit_capnp`, `emit_fbs` and the layouts computed from a schema |
| deprecation | `attr(f, deprecated_spec{.version = "…"})` | `#[psio(deprecated)]` on a field | `field(T, { deprecated: '…' })` | `psio.deprecated`, `psio.deprecated("…")` | member attribute `deprecated` = the version | GraphQL (`@deprecated`); flatbuf (the member keeps its field id and is neither written nor read); `emit_fbs` (`(deprecated)`) |
| default of a scalar member | the member initialiser (`std::int16_t level = 7;`) | `#[psio(default = ..)]` on a field | `field(T, { default: v })` | `psio.default(v)` | member attribute `default` = the value as text (absent: zero) | flatbuf and capnp (a member equal to its default is not written / is zero on the wire, and an absent one reads as it); `emit_fbs`, `emit_capnp` (`= value`). Not read by pssz, key, JSON |
| order, uniqueness of a list's elements | `sorted`, `unique` (`sorted_unique`), `sorted_spec{.ascending = false}`, `unique_keys` | `#[psio(sorted)]`, `#[psio(unique)]` (`sorted_unique`), `#[psio(unique_keys)]` on a field, also inside `element(...)` and friends (ascending only) | `field(T, { sorted: true \| 'descending', unique: true, uniqueKeys: true })` | `psio.sorted`, `psio.unique`, `psio.sorted(descending=True)`, `psio.unique_keys` | — (not in the Schema IR: typed codecs only) | the validating readers and the validated writes of the typed pssz, json/ijson and key codecs (every language) and of the other binary formats (C++; Rust: flatbuf, capnp, protobuf, wit); order is that of the elements' key bytes ([5.13](#513-sorted-unique-unique_keys)) |
| reserved vocabulary | `hex_spec`, `skip_spec`, `default_value_spec`, `default_factory_spec`, `since_spec`, `unstable_spec`, `final_v`, `canonical`, `flags`, `padding` | Rust has `#[psio(skip)]` (a different, implemented meaning) | — | — | — | RESERVED: nothing reads them ([5.14](#514-reserved-vocabulary)) |

### 4.1 Tooling per language

What a language offers around the annotations. "not yet available" as in
the table above.

| | C++ | Rust | TypeScript | Python |
|---|---|---|---|---|
| compatibility check of two revisions | `psio::pssz_evolves_from<Old, New>` (compile time): fields, widths, bounds that only widen, `utf8`, order specs, caps, `maxFields` ([8](#8-evolution-what-may-change-after-publication)) | `pssz::pssz_evolves_from::<Old, New>()` (`const fn`): the C++ rule — fields, widths, bounds that only widen, caps, `max_fields`, alternatives appended to an enum with data. At run time, over two Schema IRs (C++ `psio::pssz_ir::evolves_from` / `equivalent`): `pssz::dynamic::evolves_from(&old, &old_type, &new, &new_type, Compare::Contract \| Compare::Wire)` and `equivalent(...)` → `Compatibility { ok, code, reason, path }`, with `evolves_from_func` / `_interface` / `_schema` and `equivalent_func` / `_interface` / `_schema` | `psszEvolvesFrom(Old, New, { compare: 'contract' \| 'wire' })` and `psszEquivalent(A, B, { compare })` → `{ ok: true }` or `{ ok: false, code, reason, path }`, at run time over two definitions or two fetched schema types, methods, interfaces or schemas: the rule of C++ `psio::pssz_ir::evolves_from` / `equivalent` (`'contract'`, the default, also compares names, custom ids, enums and text forms; `'wire'` the bytes only) | `psio.evolves_from(Old, New, mode="contract" \| "wire")` and `psio.equivalent(A, B, mode=…)` → a `psio.Compatibility` (`ok`, `code`, `reason`, `path`), at run time over two declarations or two schema types, methods, interfaces or `psio.Schema` values: the rule of C++ `psio::pssz_ir::evolves_from` / `equivalent` |
| the inferred bound and the largest encoding ([5.4](#54-maxdynamicdatan)) | `psio::inferred_bound<T>()`, `psio::max_encoded_size<T>()` (compile time) | `pssz::inferred_bound::<T>()`, `pssz::max_encoded_size::<T>()` (`const fn`; `PsszPack::INFERRED_BOUND`, `MAX_ENCODED_SIZE`), `Option<usize>` | `T.inferredBound`, `T.maxEncodedSize`: `bigint` or `null`, of a definition or a fetched schema type | `psio.codec(T).inferred_bound`, `.max_encoded_size`: `int` or `None`, of a declaration or a schema type |
| the two write modes (the validated write checks bounds, UTF-8, order, invariants) | fast: `psio::to_F(v)`, `psio::encode`. Validated: `psio::to_F_checked(v)` (throws), `psio::encode_F_checked(v, out)` (status), `psio::encode_checked(F{}, v, out)`, for every format; `psio::validate_value(v)` | fast: `psio::to_F`, `to_F_into`. Validated: `psio::try_to_F`, `try_to_F_into` — pssz, key, json, ijson, flatbuf, capnp, protobuf, wit | fast: `T.encode`, `T.encodeKey`, `T.valueToJson`, `method.resultToPssz`. Validated: `T.encodeChecked`, `T.encodeKeyChecked`, `T.valueToJsonChecked`, `method.resultToPsszChecked` | fast: `psio.to_pssz`, `to_key`, `to_json`, `to_ijson`. Validated: `psio.to_pssz_checked`, `to_key_checked`, `to_json_checked`, `to_ijson_checked`, `result_to_pssz_checked` |
| JSON form of an argument list | writes an object when every parameter is named, else an array; reads either for a named list | writes the object and reads either; a function marked `#[psio(positional)]` is the array only | a named list writes the object and reads either; `method([...])` / `argumentTuple([...])` without names is the array only | a named list writes the object and reads either; a list without names (`psio.arguments(..., named=False)`) is the array only |
| result encoded as its `returns(...)` type | `psio::encode_method_result<T, I>` (fast) / `encode_method_result_checked` (validated), `decode_method_result`, `validate_method_result`, `check_method_result` | `Method::result_to_pssz` (fast) / `try_result_to_pssz` (validated) / `result_from_pssz` of `args::<method>` | `defineInterface(...).methods.<m>.result` (a codec: `encode` / `decode` / `validate`); from a fetched schema `schema.interface(name).method(m).result` | `result_to_pssz` / `result_from_pssz` / `validate_result` of `psio.interface_of(T)[name]` |
| interface → Schema IR `Interface` / `Func` | `PSIO_INTERFACE` + `SchemaBuilder::insert_interface<T>()` | `#[psio::interface]` + `SchemaBuilder::insert_interface::<I>()` | `defineInterface(...).schema`; `generateSchema(types, { interfaces: [...] })` | `@psio.interface`, `psio.interface_of(T).schema()` |
| account-name coder (`name_to_number`) | `psio::name_to_number`, `psio::number_to_name` (`psio/name.hpp`) | `psio::compress_name::name_to_number`, `number_to_name` | `nameToNumber`, `numberToName` | `psio.name_to_number`, `psio.number_to_name` |
| method-name coder (`method_to_number`) | `psio::detail::method_to_number`, `number_to_method` (`psio/compress_name.hpp`); `psio::hash_name` | `psio::compress_name::method_to_number`, `number_to_method` | `methodToNumber`, `numberToMethod`, `isHashName`, `isCompressedName` | `psio.method_to_number`, `psio.number_to_method`, `psio.is_hash_name` |
| tests driven by the new shared fixtures `conformance/interface-vectors.json` and `conformance/name-vectors.json` | `psio_interface_conformance_regen`, `psio_name_conformance_regen` (the emitters check the committed files) | `rust/psio/tests/compress_name.rs` (names), `rust/psio/tests/interface_conformance.rs`; `conformance/evolution-vectors.json`: `rust/psio/tests/evolution_conformance.rs` | `js/src/test/compress-name.test.ts` (names), `js/src/test/interface-conformance.test.ts`; `js/src/test/evolution-conformance.test.ts` | `python/tests/test_consistency.py` (names), `python/tests/test_interface_conformance.py`; `python/tests/test_evolution_conformance.py` |

## 5. The annotations

Each section: purpose, where it applies, spellings, Schema IR, effect per
format, enforcement, wire / width impact, evolution, mistakes.

### 5.1 `max<N>`, `exact<N>`, `min<N>` — length bounds

**Purpose.** Bound the length of a variable-size value. One spec struct,
`psio::length_bound{.exact, .max, .min}`; the helpers are
`max<N>` = `{.max = N}`, `exact<N>` = `{.exact = N}`, `min<N>` = `{.min = N}`.
A range is `min<2> | max<8>`, which is the one spec
`psio::length_bound{.max = 8, .min = 2}`.

**Counts.** Bytes of a string or of a `psio::nested<T>`; elements of a
list or set; entries of a map.

**Applies to.** On the member itself: `std::string`, `psio::utf8_string<N>`,
`std::vector<T>`, `psio::bounded<T, N>`, maps and sets (`std::map`,
`std::set`, `std::unordered_map`, `std::unordered_set`),
`psio::nested<T>`, `psio::bitlist<N>`; through `std::optional` (the bound
applies to the payload). Below the member (inside `element` / `keys` /
`values` / `at`): `max<N>` only, on `std::string`, `std::vector`, map or set.

```cpp
struct Limits
{
   friend bool operator==(const Limits&, const Limits&) = default;
   std::vector<std::uint8_t>  digest;
   std::string                code;
   std::vector<std::uint32_t> ids;
   PSIO_REFLECT(Limits,
                attr(digest, exact<32>),                                   // exactly 32 bytes
                attr(code, min<2> | max<8>),                               // 2..8 bytes
                attr(ids, max<4>))                                         // <= 4 elements
};
// max<N> and exact<N> give the inferred bound its N; min<N> alone bounds nothing.
// (header + 3 slots) x 1 byte + digest 32 + code 8 + ids 4 x 4
static_assert(psio::inferred_bound<Limits>() == 4 + 32 + 8 + 4 * 4);
static_assert(psio::auto_pssz_width_v<Limits> == 1);
// min<A> | max<B> is one spec, the range psio::length_bound{.max = B, .min = A}.
static_assert(std::get<psio::length_bound>(psio::min<2> | psio::max<8>) ==
              psio::length_bound{.max = 8, .min = 2});

struct AtLeast
{
   std::string code;
   PSIO_REFLECT(AtLeast, attr(code, min<2>))
};
static_assert(!psio::inferred_bound<AtLeast>().has_value());   // no upper bound: 32-bit
```

| | |
|---|---|
| C++ | `attr(f, max<N>)`; separate: `PSIO_FIELD_ATTRS(T, f, psio::max<N>)`; wrapper: `psio::bounded<T, N>`, `psio::utf8_string<N>` |
| Rust | `#[psio(max = N)]` on a field, an enum variant's field or an interface parameter. `exact` / `min`: not available. Wrappers: `BoundedString<N>`, `BoundedRawString<N>`, `BoundedList<T, N>`, `BoundedMap`, `BoundedSet`, `Nested<T, V, N>` |
| TypeScript | `field(T, { max: N })`; `field(T, { lengthBound: { exact, min, max } })`; wrappers `boundedUtf8(N)`, `boundedBytes(N)`, `boundedList(T, N)`, `map(K, V, { max })`, `set(K, { max })` |
| Python | `Annotated[T, psio.max(N)]`, `psio.exact(N)`, `psio.min(N)`, `psio.length(min=A, max=B)`. `psio.bounded(N)` says `max` as part of a type alias (`Tag = Annotated[str, psio.bounded(16)]`; same bytes, the schema records the bound in the type) |
| Schema IR | `max` alone on a string, byte list, list, map or set: the bounded node (`BoundedList{type, maxCount: N}`, inside `Custom{…, "utf8" \| "string" \| "hex" \| "map" \| "set"}` for those kinds). `exact` / `min`, and any bound on a `Nested` member: member attribute `lengthBound = "exact=N"` / `"min=A,max=B"` / `"max=N"` |

| Format | Effect |
|---|---|
| pssz | `max` / `exact` enter the inferred bound (width). Enforced by the validated write — `to_pssz_checked` (throws `codec_exception`: `pssz: member length_bound violated`), `encode_pssz_checked` (status) — and by `validate_pssz`, `validate_strict`, `decode_pssz_checked` and `from_pssz`. The trusted read (`from_pssz(psio::trusted{…})`, `psio::decode`) does not look. The fast write (`to_pssz`, `psio::encode`) refuses a value only when its payload does not fit the offset width the bound gave the type ([7.2](#72-reading-and-writing-the-two-modes)) |
| key | Bytes unchanged. The validated write refuses a value outside the bound: `to_key_checked` throws `codec_exception` (`key: member length_bound violated`), `psio::encode_key_checked(value, out)` returns the status and leaves `out` unchanged. `validate_key` and `from_key` reject a key that holds one; the trusted read (`from_key(psio::trusted{…})`, `psio::decode`) does not look. The fast write (`to_key`, `psio::encode(psio::key{}, …)`, `key_prefix`) writes the key of any value |
| json / ijson | Text unchanged. The validated write (`to_json_checked` throws; `encode_json_checked` returns a status) and the readers refuse a value outside the bound; the fast write (`to_json`) does not look |
| schema-driven codecs (`psio::pssz_ir`) | The same width and the same rejections, from the IR alone |
| WIT | `list<T>/* @psio:max=N */` for a bounded list, map or set; a bounded string is `string` |
| flatbuf, capnp, protobuf, bin, ssz, borsh, bincode, msgpack, avro, bson, wit ABI | Bytes unchanged. `validate_F`, `from_F`, the validated views and the validated write (`to_F_checked`, `encode_F_checked`) refuse a value outside the bound (`flatbuf: member length_bound violated`, `bin: …`, …), at the member and at every position below it; in protobuf the bound is held against the finished value (a member written twice: the last scalar, the merged list). The fast write (`to_F`) does not look |
| GraphQL | Not read |

**Wire / width.** A bound is part of the type: it can change the `W` of
the record, and the `W` of the list, map, set, array, tuple, pair or map
entry it bounds — the member itself or a position below it. A
`std::vector<std::string>` under `max<8> | element(max<16>)` has a 1-byte
offset table; unannotated (and with no cap above it) a 4-byte one. A
string, a byte list and a list of fixed-size elements have no offsets, and
their bytes never change.

**A range.** `min<A> | max<B>` merges into one spec,
`psio::length_bound{.max = B, .min = A}`; the order of the two, and what
stands between them in the `|` list, do not matter. The same limit written
twice with one value is one statement.

**Evolution.** A published bound may only **widen** — the new range must
contain the old one, so `max` may grow, `min` may shrink, and a bound may be
removed — and only while every offset width stays where it was. It may never
tighten: the new reader would reject stored values. `pssz_evolves_from`
checks exactly this, at the member and at every position below it
([8.3](#83-annotations-bounds-may-only-widen)). Widening is one-directional:
a reader that still has the old bound rejects what a new writer may now
produce, so upgrade the readers first.

**Mistakes.**

- Contradictory bounds on one member or position — the same limit twice
  with different values (`max<8> | max<16>`), `min<N>` above `max<M>`,
  `exact<N>` outside `min` / `max`:
  `psio: contradictory length bounds on one member or position: a limit is stated twice with different values (max<8> | max<16>), min<N> is greater than max<M>, or exact<N> lies outside min / max`
- `max<N>` on an integer, bool, enum or record member:
  `psio: a spec in the effective annotations is attached to a field whose shape is not in the spec's applies_to set. …`
- `exact<N>` / `min<N>` below the member:
  `psio: below the member itself a length bound is max<N>; exact<N> and min<N> apply to the member only`
- `max<N>` on a position that has no length (an integer element):
  `psio: max<N> inside element(...), keys(...), values(...) or at<I>(...) bounds a std::string (bytes) or a std::vector, map or set (elements); this position is none of those`
- Bounding the count of a list but not its elements leaves the type
  unbounded (`tut-2c`).

### 5.2 `utf8` — text or raw bytes

**Purpose.** State that a string's bytes are UTF-8. A plain `std::string`
is **raw bytes**; `utf8` (or the type `psio::utf8_string<N>`) makes it
text. Across languages:

| | UTF-8 text | raw bytes |
|---|---|---|
| C++ | `std::string` + `utf8`, `psio::utf8_string<>`, `psio::utf8_string<N>` | `std::string` |
| Rust | `String`, `BoundedString<N>` | `RawString`, `BoundedRawString<N>` |
| TypeScript | `utf8`, `boundedUtf8(N)`, `field(rawString, { utf8: true })` → JS `string` | `rawString` → JS `Uint8Array` |
| Python | `str` | `psio.RawString` (a `bytes` value); `bytes` itself is a byte list, hex in I-JSON |
| Schema IR | `Custom{List<u8> \| BoundedList<u8, N>, "utf8"}` | `Custom{…, "string"}` |

```cpp
struct Names
{
   friend bool operator==(const Names&, const Names&) = default;
   std::string raw;       // raw bytes        (Rust RawString)
   std::string text;      // UTF-8            (Rust String)
   std::string label;     // UTF-8, <= 16 bytes
   PSIO_REFLECT(Names, raw, attr(text, utf8), attr(label, utf8 | max<16>))
};
struct NamesW
{
   std::string           raw;
   psio::utf8_string<>   text;
   psio::utf8_string<16> label;
   PSIO_REFLECT(NamesW, raw, text, label)
};

struct NamesSpelled   // psio::utf8_spec{} is psio::utf8
{
   std::string text;
   PSIO_REFLECT(NamesSpelled, attr(text, psio::utf8_spec{}))
};
```

**Applies to.** `std::string` (member or position), through `std::optional`.

| Format | Effect |
|---|---|
| pssz | Bytes unchanged. A `utf8` string that is not UTF-8 is refused by the validated write (`to_pssz_checked`, `encode_pssz_checked`: `utf8_spec: invalid leading byte` and similar), `validate_pssz`, `validate_strict` and the checked decode; the fast write (`to_pssz`) writes it and the trusted read reads it. A raw string holds any bytes for every one of them: `validate_strict` means exact schema and canonical form, not "strings are text" (`tutorial 5`) |
| key | Bytes unchanged (raw and UTF-8 strings have one key form). The validated write (`to_key_checked` throws, `encode_key_checked` reports) and the readers (`validate_key`, `from_key`) refuse a `utf8` string that is not UTF-8, for the annotation and for `psio::utf8_string` alike (`tutorial 7`); the fast write (`to_key`) writes it. In Rust, TypeScript and Python a text value is UTF-8 by its type |
| json / ijson | Both kinds are JSON strings. JSON text is UTF-8, so a **raw string that is not UTF-8 has no JSON**: the writer fails |
| flatbuf, capnp, bin, ssz, borsh, bincode, msgpack, avro, bson, wit ABI | Bytes unchanged (these formats hold any bytes for a C++ `std::string`). `validate_F`, `from_F`, the validated views and the validated write refuse a `utf8` string that is not UTF-8 (`utf8_spec: invalid leading byte` and similar). In Rust `String` is UTF-8 by its type and `RawString` is raw |
| protobuf | A `std::string` is a protobuf `string`, which is UTF-8 by the format: refused with or without the annotation (`protobuf: a string is not UTF-8`) |
| Schema IR | `"utf8"` vs `"string"`; schema-driven codecs enforce `"utf8"` |

**Wire / width.** None.

**Evolution.** Frozen: adding `utf8` rejects stored non-UTF-8 values;
removing it lets writers produce bytes that `utf8` readers reject.
`pssz_evolves_from` is false when `utf8` is added or removed, at the member
or at any position below it (`tut-4c`: `AccountRawOwner`).

**Notes.** `utf8` is always enforced: the annotation states what the
string *is*, exactly as the type `psio::utf8_string` does. `psio::utf8` is
`psio::utf8_spec{}`, and the spelled-out `attr(f, psio::utf8_spec{})` is
the same statement. No annotation is checked by `validate_strict` alone:
an annotation is enforced by every validating reader and every validated
write, or it is reserved
([5.14](#514-reserved-vocabulary); test
`reference: utf8 annotation and wrapper`, `NamesSpelled`).

**Mistakes.** `utf8` on a non-string member is the `applies_to` error of
[5.1](#51-maxn-exactn-minn--length-bounds); below the member:
`psio: utf8 inside element(...), keys(...), values(...) or at<I>(...) applies to a string (std::string, psio::utf8_string); this position is not a string`.

### 5.3 `element`, `keys`, `values`, `at<I>` — positions below the member

**Purpose.** Annotate what a member *holds* without changing its type.

| Position spec | Reaches |
|---|---|
| `element(specs)` | every element of a list (`std::vector`, `psio::bounded`), a `std::array`, or a set |
| `keys(specs)` | every key of a map |
| `values(specs)` | every mapped value of a map |
| `at<I>(specs)` | element `I` of a `std::tuple` / `std::pair`; alternative `I` of a `std::variant` |

`specs` is one spec or a `spec | spec` list of `max<N>`, `utf8` and further
position specs, to any depth. A `std::optional` is looked through.

```cpp
struct Post
{
   friend bool operator==(const Post&, const Post&) = default;
   std::string                               title;
   std::vector<std::string>                  tags;
   std::map<std::string, std::uint64_t>      votes;
   std::optional<std::vector<std::uint8_t>>  thumb;
   std::pair<std::string, std::uint32_t>     author;
   PSIO_REFLECT(Post,
                attr(title, utf8 | max<64>),                       // UTF-8, <= 64 bytes
                attr(tags, max<8> | element(utf8 | max<16>)),      // <= 8 strings of <= 16 bytes
                attr(votes, max<32> | keys(utf8 | max<16>)),       // <= 32 entries, keys <= 16 bytes
                attr(thumb, max<1024>),                            // when present: <= 1024 bytes
                attr(author, at<0>(utf8 | max<32>)))               // pair / tuple element 0
};
static_assert(psio::auto_pssz_width_v<Post> == 2);   // every part is bounded: 16-bit offsets

struct Grid   // a list of lists
{
   friend bool operator==(const Grid&, const Grid&) = default;
   std::vector<std::vector<std::uint16_t>> rows;
   PSIO_REFLECT(Grid, attr(rows, max<3> | element(max<4>)))
};
// header 1 + slot 1 + rows: 3 slots of 1 byte + 3 x (4 x 2)
static_assert(psio::inferred_bound<Grid>() == 1 + 1 + (3 + 3 * (4 * 2)));
static_assert(psio::auto_pssz_width_v<Grid> == 1);

struct Choice   // a variant alternative
{
   friend bool operator==(const Choice&, const Choice&) = default;
   std::variant<std::uint32_t, std::string> v;
   PSIO_REFLECT(Choice, attr(v, at<1>(utf8 | max<16>)))
};
// header 1 + slot 1 + selector 1 + the widest alternative 16
static_assert(psio::inferred_bound<Choice>() == 1 + 1 + 1 + 16);
```

An annotated member **is** the type its annotations describe:
`std::vector<std::string>` under `max<8> | element(utf8 | max<16>)` is
`psio::bounded<psio::utf8_string<16>, 8>` — same bound, same widths at
every depth, same bytes, same schema, same checks
(`cpp/tests/annotation_tree_tests.cpp`).

| | |
|---|---|
| Rust | `#[psio(max = 8, element(max = 16))] tags: Vec<String>`, `#[psio(max = 4, keys(max = 16), values(max = 2))] m: BTreeMap<String, Vec<u32>>`, `#[psio(at(0, max = 16), at(2, max = 5))] t: (String, u32, Vec<u8>)`. There is no `utf8` key (the type says it). A variant's alternatives are annotated on the fields of the enum's variants: `Text(#[psio(max = 16)] String)` |
| TypeScript | `field(list(rawString), { max: 8, element: { utf8: true, max: 16 } })`, `field(map(K, V), { max: 4, keys: { max: 16 }, values: { max: 2 } })`, `field(tuple(…), { at: { 0: { utf8: true, max: 16 } } })`; for a variant `at` takes the alternative's name or index |
| Python | `Annotated[list[str], psio.max(8), psio.element(psio.max(16))]`, `Annotated[dict[str, list[u32]], psio.max(4), psio.keys(psio.max(16)), psio.values(psio.max(2))]`, `psio.at(0, psio.max(16))` (tuple index; for a union the alternative's index or name). There is no `utf8` to write: `str` is UTF-8 |
| Schema IR | No attribute: the member's type is written as the bounded nodes |

**Formats, enforcement, width.** As [5.1](#51-maxn-exactn-minn--length-bounds)
and [5.2](#52-utf8--text-or-raw-bytes), at every depth. Every list, map,
set, array of variable elements, tuple, pair and map entry at or below the
member takes its own `W` from the bounds of its position
([3.1](#31-how-the-inferred-bound-is-counted)).

**Views and limits.**

- **Views.** A member whose annotations (or whose record's cap) narrow an
  offset width at or below it is readable through a `pssz_view` like any
  other: the view of such a member has the format `psio::pssz_at<A>`,
  which carries the member's annotations down the view exactly as the
  codec carries them. You do not name that format; `view.get<I>()` returns
  it.

  One case is not addressable: a `std::variant` member in which an
  `at<I>(...)` annotation narrows the offset width of a value nested in an
  alternative (one view format serves all alternatives). The error starts
  `psio: a pssz view cannot address this variant: an at<I>(...) annotation narrows`.
  State that bound in the alternative's type (`psio::bounded<T, N>`,
  `psio::utf8_string<N>`) to view it. A bound that narrows nothing — a
  string alternative under `at<1>(utf8 | max<16>)`, as in `Choice`
  ([6.10](#610-variant)) — is viewable.
- A member with `element` / `keys` / `values` / `at` annotations is not
  addressable through a mutable view:
  `a member whose annotations constrain its content is not addressable in place`.

```cpp
// names: std::array<std::string, 3> under element(utf8 | max<16>) has a
// 1-byte table; the view reads it at that width.
const Slots s{{1, 2, 3, 4}, {"a", "", "last"}};
const auto  slot_bytes = psio::to_pssz(s);
const psio::pssz_view<Slots> slots{slot_bytes};
CHECK(slots.get<1>()[2].view_() == "last");

// votes: a map under max<32> | keys(utf8 | max<16>); author: a pair under at<0>(...).
const Post p{"hello", {"a", "b"}, {{"alice", 3}}, std::nullopt, {"bob", 9}};
const auto post_bytes = psio::to_pssz(p);
const psio::pssz_view<Post> post{post_bytes};
CHECK(post.get<1>()[1].view_() == "b");
CHECK(post.get<2>()[0].get<0>().view_() == "alice");
CHECK(post.get<2>()[0].get<1>() == 3u);
CHECK(post.get<4>().get<0>().view_() == "bob");
```

**Mistakes** (each a compile error with this text):

| Written | Error |
|---|---|
| `element(...)` on a string or a map | `psio: element(...) annotates the elements of a list (std::vector, psio::bounded), a std::array or a set; this member or position is none of those (a map takes keys(...) / values(...), a tuple, pair or variant takes at<I>(...))` |
| `keys(...)` on a list | `psio: keys(...) annotates the keys of a map (std::map, std::unordered_map); this member or position is not a map (a set, list or array takes element(...))` |
| `values(...)` on a list | `psio: values(...) annotates the mapped values of a map (std::map, std::unordered_map); this member or position is not a map (a set, list or array takes element(...))` |
| `at<0>(...)` on a list | `psio: at<I>(...) annotates element I of a std::tuple / std::pair or alternative I of a std::variant; this member or position is none of those (a list, array or set takes element(...))` |
| `at<2>(...)` on a pair | `psio: at<I>(...): I is not an element of this tuple / pair` |
| `at<5>(...)` on a 2-alternative variant | `psio: at<I>(...): I is not an alternative of this variant` |

Each is followed by `psio: a member annotation does not fit the member's type`
(tests `psio_annotation_tree_*_compile_fail`).

### 5.4 `maxDynamicData(N)`

**Purpose.** Three things at once: the **limit of the encoded size** of a
value of the type (the whole encoding, header included — despite the
name); the type's **width budget for its lifetime**, which is how a type
reserves room for fields added later; and the bound that selects the
offset widths of the **lists, maps, sets, arrays and tuples beneath it**
([3.3](#33-a-cap-narrows-what-lies-beneath-it)).

```cpp
struct AccountV1
{
   friend bool operator==(const AccountV1&, const AccountV1&) = default;
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountV1,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                maxDynamicData(1024))
};
// The cap rules: 16-bit offsets, although today's fields need at most 187 bytes.
static_assert(psio::max_dynamic_data_v<AccountV1> == 1024);
static_assert(psio::auto_pssz_width_v<AccountV1> == 2);
```

(`tut-3`.)

Three numbers, three names:

| | `AccountV1` | what it is |
|---|---|---|
| `psio::inferred_bound<T>()` | 187 | what the fields add up to, counted as if there were no cap. It selects the width of a type that has no cap, and never reads the type's own cap |
| `psio::effective_max_dynamic_v<T>` | 1024 | the number the width is the class of: the cap when one is declared, else the inferred bound |
| `psio::max_encoded_size<T>()` | 190 | the largest encoding: no `AccountV1` is longer. The fields at the width the cap selects (header and two slots of 2 bytes, not 1), and never more than the cap |

The inferred bound is **not** an upper bound of a capped type's encoding:
a cap in a wider class makes the header and slots wider than the bound
counted them (187 → 190). Size a buffer with `max_encoded_size`. When
something inside is unbounded, `inferred_bound` is empty and
`max_encoded_size` is the cap; with no cap and an unbounded member neither
exists. ([pssz-spec.md §3.7](pssz-spec.md#37-max_encoded_sizet-the-largest-encoding).)

The same two numbers in the other languages, with the same values (187 and
190 for `AccountV1`): Rust `psio::pssz::inferred_bound::<T>()` and
`max_encoded_size::<T>()` (the cap-or-bound number is `PsszPack::AUTO_BOUND`);
TypeScript `T.inferredBound` and `T.maxEncodedSize`; Python
`psio.codec(T).inferred_bound` and `.max_encoded_size`.

**Applies to.** A reflected record (not a transparent one), and a method's
argument tuple ([5.11](#511-methodname-)).

| | |
|---|---|
| C++ | `maxDynamicData(N)` in `PSIO_REFLECT`; `PSIO_TYPE_ATTRS(T, psio::max_dynamic_data_spec{N})`; read with `psio::max_dynamic_data_v<T>` |
| Rust | `#[pssz(max_dynamic_data = N)]` on the struct (or on an interface function) |
| TypeScript | `record(name, fields, { maxDynamicData: N })`, `argumentTuple(params, { maxDynamicData: N })` |
| Python | `@psio.record(max_dynamic_data=N)` above the `@dataclass`; `@psio.method(max_dynamic_data=N)` on an interface method; read with `psio.codec(T).max_dynamic_data` |
| Schema IR | record attribute `{"name":"maxDynamicData","value":"N"}`; on a `Func` for a method |

| Format | Effect |
|---|---|
| pssz | The record's `W` follows from `N` alone. The encoder refuses a value whose encoding exceeds `N` (`pssz: encoded size exceeds maxDynamicData cap`); every reader rejects a longer frame. Enforced on nested values of the type too. Lists, maps, sets, arrays, tuples and pairs beneath the record, down to the next record, take the width class of `min(their own bound, N)` |
| a parent record | reserves `N` bytes for a member of this type (`ref-records`: `Line`) |
| json / ijson | Not applied: the cap limits the pssz encoding, not the length of a text, and neither the JSON writer nor the JSON readers measure it. See "the cap and text" below |
| the other C++ binary formats (bin, ssz, borsh, bincode, msgpack, avro, bson, protobuf, capnp, flatbuf, wit; not key) | `validate` applies the **root** type's cap to the size of **its own input** (`cpp/tests/caps_parity_tests.cpp`) |
| schema-driven codecs, WIT | the same width and limit; `@maxDynamicData(N)` above the record or function |

```cpp
// A cap is the limit of the whole encoded value, header included.
CHECK(psio::to_pssz(ref::Capped{std::string(14, 'x')}).size() == 16);     // 1 + 1 + 14
CHECK_THROWS_AS(psio::to_pssz(ref::Capped{std::string(15, 'x')}), psio::codec_exception);
auto frame = psio::to_pssz(ref::Capped{std::string(14, 'x')});
frame.push_back('x');                                                     // 17 bytes
CHECK_FALSE(psio::validate_pssz<ref::Capped>(frame).ok());
```

**The cap and text.** `maxDynamicData` is defined on the encoded pssz size.
A JSON text of a capped type may be longer than the cap, and the text
codecs do not apply the cap at all — a text can hold a value that has no
pssz encoding, and the cap is met when that value is written as pssz:

```cpp
// maxDynamicData(16) limits the pssz encoding. It is not a limit on the
// length of the JSON text: this value is 16 bytes of pssz and 22 of text.
const Capped value{std::string(14, 'x')};
CHECK(psio::to_pssz(value).size() == 16);
const std::string text = psio::to_json(value);
CHECK(text == R"({"s":"xxxxxxxxxxxxxx"})");
CHECK(psio::validate_json<Capped>(text).ok());
CHECK(psio::from_json<Capped>(text).s.size() == 14);
// The text codecs do not apply the cap at all: a text can hold a value
// that has no pssz encoding. The cap is met when the value is written as pssz.
const Capped over = psio::from_json<Capped>(R"({"s":"xxxxxxxxxxxxxxx"})");   // 15: 17 bytes of pssz
CHECK_THROWS_AS(psio::to_pssz(over), psio::codec_exception);
```

Member bounds are enforced in text; state the limits that must hold for
text input as member bounds.

**Choosing N.** [3.4 How to pick a cap](#34-how-to-pick-a-cap). In short:
at least the inferred bound of the current fields plus everything the type
may gain; 65535 keeps 16-bit offsets and is enough for most database rows
and action arguments; when unsure, one class up.
`psio::effective_max_dynamic_v<T>` is the explicit cap when there is one and
the inferred bound otherwise (never the smaller of the two):
`psio::auto_pssz_width_v<T>` is its width class.

**Evolution.** `N` may not move across a width class (255 / 65535): that
changes the record's own offsets and those of the containers beneath it.
Within its class it may be **raised, never lowered** (`tut-4c`:
`AccountCapRaised`); old readers still reject values above their own `N`.
A cap may be added to a type that had none only if it is at least the old
type's inferred bound, and dropped only if no width moves. All of this is
what `pssz_evolves_from` checks ([8.3](#83-annotations-bounds-may-only-widen)).

**Mistakes.** Listing it twice: `psio: PSIO_REFLECT lists a type-level attribute twice`.
With `transparent()`: `psio: transparent() cannot be combined with definitionWillNotChange / maxFields / maxDynamicData: they describe a record layout, and a transparent type is its member`.

### 5.5 `definitionWillNotChange()`

**Purpose.** Promise that the record's fields will never change. The
record then has no header and no extension point (pssz-spec §5.1.2); a
record of only fixed-size members becomes a fixed-size value with zero
overhead.

```cpp
struct Point   // fixed layout: no header, cannot gain fields
{
   friend bool operator==(const Point&, const Point&) = default;
   std::int32_t x = 0;
   std::int32_t y = 0;
   PSIO_REFLECT(Point, x, y, definitionWillNotChange())
};
struct PointOpen   // extensible: a header, may gain optional fields
{
   std::int32_t x = 0;
   std::int32_t y = 0;
   PSIO_REFLECT(PointOpen, x, y)
};
static_assert(psio::is_dwnc_v<Point> && !psio::is_dwnc_v<PointOpen>);
static_assert(psio::inferred_bound<Point>() == 8);
static_assert(psio::inferred_bound<PointOpen>() == 1 + 8);
```

```cpp
CHECK(psio::to_pssz(Point{1, 2}).size() == 8);          // DWNC: the fields, nothing else
CHECK(psio::to_pssz(PointOpen{1, 2}).size() == 1 + 8);  // extensible: a 1-byte header
```

| | |
|---|---|
| C++ | `definitionWillNotChange()`; `PSIO_TYPE_ATTRS(T, psio::definition_will_not_change{})`; read with `psio::is_dwnc_v<T>` |
| Rust | `#[psio(definition_will_not_change)]` |
| TypeScript | `record(name, fields, { fixed: true })` |
| Python | `@psio.record(definition_will_not_change=True)` |
| Schema IR | record attribute `definitionWillNotChange` (empty value) |

**Formats.** pssz: no header, never pruned, never extended. bin / borsh /
bincode use it for bulk-copy fast paths. JSON, key: no effect.

**Evolution.** None: no field may be added, the keyword may not be added
or removed later (`pssz_evolves_from` is false for all three). Use it for
values that are mathematically closed — a point, a hash, a key, an id pair
— not for rows.

### 5.6 `maxFields(N)`

**Purpose.** A cap on the **number of fields** a record — or of parameters a
method — has and will ever have. It counts fields; `maxDynamicData` counts
bytes. It changes no byte and no width.

```cpp
struct Settings
{
   std::uint32_t                volume = 0;
   std::optional<std::uint32_t> balance;
   PSIO_REFLECT(Settings, volume, balance, maxFields(3), maxDynamicData(64))
};
static_assert(psio::max_fields_v<Settings> == 3);
static_assert(psio::effective_max_fields_v<Settings> == 3);   // the cap; without one, today's count
static_assert(psio::effective_max_fields_v<Point> == 2);
static_assert(psio::max_record_fields == 255);                // every record; N of maxFields(N) too

struct SettingsGrown   // a third field: within the cap
{
   std::uint32_t                volume = 0;
   std::optional<std::uint32_t> balance;
   std::optional<std::uint32_t> treble;
   PSIO_REFLECT(SettingsGrown, volume, balance, treble, maxFields(3), maxDynamicData(64))
};
static_assert(psio::pssz_evolves_from<Settings, SettingsGrown>);

struct SettingsRaised   // a revision may not raise the cap ...
{
   std::uint32_t                volume = 0;
   std::optional<std::uint32_t> balance;
   std::optional<std::uint32_t> treble;
   PSIO_REFLECT(SettingsRaised, volume, balance, treble, maxFields(8), maxDynamicData(64))
};
static_assert(!psio::pssz_evolves_from<Settings, SettingsRaised>);

struct SettingsDropped   // ... or drop it
{
   std::uint32_t                volume = 0;
   std::optional<std::uint32_t> balance;
   std::optional<std::uint32_t> treble;
   PSIO_REFLECT(SettingsDropped, volume, balance, treble, maxDynamicData(64))
};
static_assert(!psio::pssz_evolves_from<Settings, SettingsDropped>);

// A method's argument list takes the same cap.
struct Mixer
{
   void set(std::uint32_t channel, std::uint32_t level);
   PSIO_REFLECT(Mixer, method(set, channel, level, maxFields(4), maxDynamicData(64)))
};
using SetArgs = psio::reflect<Mixer>::method_arg_tuple<0>;
static_assert(psio::reflect<Mixer>::method_max_fields<0> == 4);
static_assert(psio::max_fields_v<SetArgs> == 4);
```

**Limits.** `N` is at most 255, and every record has at most 255 fields
whether or not it says `maxFields`: a record, a tuple and an argument list
are one positional shape and share one limit
([pssz-spec.md §8.4](pssz-spec.md#84-element-counts);
`psio::max_record_fields`, `psio::max_tuple_elements`).

**Where it is enforced.**

| | |
|---|---|
| compile time | a record with more fields than its `maxFields(N)`, an `N` above 255, a record of more than 255 fields, a method with more parameters than its `maxFields(N)`: errors below |
| evolution | `pssz_evolves_from<Old, New>`: `New` has no more fields than `Old`'s cap allows and declares a `maxFields` that is not larger — the cap is never raised and never dropped (lowering it to a count the type still fits is allowed) |
| Schema IR | attribute `maxFields="N"` on the record's `Object`; for a method on the argument list's `Object` and on the `Func`. WIT: `@maxFields(N)` |
| schema-driven codecs | an `Object` / `Struct` with more than 255 members, with more members than its `maxFields`, or with a `maxFields` that is not a number ≤ 255 is a malformed schema, refused before any value is read or written; `psio::pssz_ir::validate_schema` checks a whole schema, every `Func`'s parameter list included (at most 255, at most its `maxFields`) |
| pssz / key / json values | no run-time check: the typed codecs read and write the fields the type has, and the count was checked when the type was compiled |

```cpp
// The cap is in the schema: the record's, and the argument list's.
CHECK(contains(schema_text<Settings>("Settings"), R"({"name":"maxFields","value":"3"})"));
CHECK(contains(schema_text<SetArgs>("SetArgs"), R"({"name":"maxFields","value":"4"})"));

// A schema-driven reader refuses a schema whose record has more members
// than its maxFields, or a maxFields above 255, before reading any value.
using namespace psio::schema_types;
const auto reads = [](std::size_t members, const char* max_fields) {
   Object record;
   for (std::size_t i = 0; i < members; ++i)
      record.members.push_back(Member{"m" + std::to_string(i), Box<AnyType>{AnyType{Int{8, false}}}, {}});
   record.attributes.push_back(Attribute{"maxFields", max_fields});
   std::size_t width = 0;
   return psio::pssz_ir::auto_width(Schema{}, AnyType{std::move(record)}, width).ok();
};
CHECK(reads(3, "3"));
CHECK_FALSE(reads(4, "3"));
CHECK_FALSE(reads(3, "256"));
```

| | |
|---|---|
| C++ | `maxFields(N)` in `PSIO_REFLECT` or in `method(...)`; `PSIO_TYPE_ATTRS(T, psio::max_fields_spec{N})`; read with `psio::max_fields_v<T>`, `psio::effective_max_fields_v<T>` (the cap when declared, else today's field count), `psio::reflect<T>::method_max_fields<I>` |
| Rust | `#[psio(max_fields = N)]` on a record or on an interface function: refused at compile time beyond N or 255; `pssz::dynamic::validate_schema` checks a whole schema. `pssz_evolves_from` and the run-time `pssz::dynamic::evolves_from` apply the `maxFields` rule (code `max-fields`) |
| TypeScript | `record(name, fields, { maxFields: N })`, `argumentTuple(params, { maxFields: N })`, `method(params, { maxFields: N })`: enforced at declaration and in the schema-driven codec; `psszEvolvesFrom` applies the rule (code `max-fields`) |
| Python | `@psio.record(max_fields=N)`, `@psio.method(max_fields=N)`: enforced at declaration and in schemas (`Schema.validate()` checks a whole schema) |

**Mistakes** (compile errors):
`psio: the record has more fields than its maxFields(N) allows`;
`psio: maxFields(N): N is at most 255, the most fields a record can have (docs/pssz-spec.md 8.4)`;
`psio: a record has at most 255 fields (docs/pssz-spec.md 8.4)`;
`psio: the method has more parameters than its maxFields(N) allows`;
`psio: method(..., maxFields(N)): N is at most 255, the most parameters a method can have (docs/pssz-spec.md 8.4)`;
`psio: method(...) lists maxFields(N) more than once`.

### 5.7 `transparent()`

**Purpose.** A named type that **is** its single member in every core
format: an id, a key, a quantity with its own C++ identity, invariant and
text form.

```cpp
struct AccountId
{
   friend auto operator<=>(const AccountId&, const AccountId&) = default;
   std::uint64_t value = 0;
   PSIO_REFLECT(AccountId, transparent(), value)
};
// It is its member: fixed-size, 8 bytes, no header.
static_assert(psio::is_transparent_v<AccountId>);
static_assert(psio::inferred_bound<AccountId>() == 8);
```

```cpp
struct PublicKey
{
   friend auto operator<=>(const PublicKey&, const PublicKey&) = default;
   psio::bounded<std::uint8_t, 65> data;   // the bound is in the member's type
   PSIO_REFLECT(PublicKey, transparent(), data)
};
static_assert(psio::inferred_bound<PublicKey>() == 65);
```

**A bounded member: first-class bounded types.** The one member may carry
the annotations that make a type a *bounded type*: `utf8` on a
`std::string`, `max<N>` on a `std::string`, `std::vector`, map or set, and
`element` / `keys` / `values` / `at<I>` below it. The wrapper is then that
bounded type wherever it appears — the bytes, the inferred bound, the
widths, the checks and the Schema IR of the wrapper-type spelling:

```cpp
struct Memo
{
   friend auto operator<=>(const Memo&, const Memo&) = default;
   std::string contents;
   PSIO_REFLECT(Memo, transparent(), attr(contents, utf8 | max<80>))
};
// Memo is the bounded type its member's annotations describe: on the wire it
// is psio::utf8_string<80>.
static_assert(psio::is_transparent_v<Memo>);
static_assert(psio::inferred_bound<Memo>() == 80);
static_assert(psio::inferred_bound<Memo>() == psio::inferred_bound<psio::utf8_string<80>>());

struct MemoInType   // the same type with the bound in the member's type
{
   psio::utf8_string<80> contents;
   PSIO_REFLECT(MemoInType, transparent(), contents)
};

struct Receipt   // a record that holds one: header 1 + id 8 + slot 1 + memo 80
{
   friend bool operator==(const Receipt&, const Receipt&) = default;
   std::uint64_t id = 0;
   Memo          memo;
   PSIO_REFLECT(Receipt, id, memo)
};
static_assert(psio::inferred_bound<Receipt>() == 1 + 8 + 1 + 80);
static_assert(psio::auto_pssz_width_v<Receipt> == 1);
```

```cpp
// Memo has the bytes, the key and the text of psio::utf8_string<80>.
const Memo memo{"rent"};
CHECK(psio::to_pssz(memo) == psio::to_pssz(psio::utf8_string<80>{"rent"}));
CHECK(psio::to_key(memo) == psio::to_key(psio::utf8_string<80>{"rent"}));
CHECK(psio::to_json(memo) == R"("rent")");
CHECK(psio::from_pssz<Memo>(psio::to_pssz(memo)) == memo);

// Its bound and its UTF-8 rule hold wherever a Memo appears.
CHECK_THROWS_AS(psio::to_pssz_checked(Memo{std::string(81, 'm')}), psio::codec_exception);
CHECK_THROWS_AS(psio::to_pssz_checked(Memo{"\xff"}), psio::codec_exception);
CHECK_THROWS_AS(psio::to_key_checked(Memo{std::string(81, 'm')}), psio::codec_exception);
CHECK_THROWS_AS(psio::to_pssz_checked(Receipt{1, Memo{std::string(81, 'm')}}), psio::codec_exception);
CHECK_FALSE(psio::validate_pssz<Memo>(std::vector<char>(81, 'm')).ok());
CHECK_FALSE(psio::validate_json<Memo>('"' + std::string(81, 'm') + '"').ok());

// Inside a record it costs one 1-byte slot: 1 + 8 + 1 + 4.
const Receipt receipt{1, memo};
CHECK(psio::to_pssz(receipt).size() == 14);
CHECK(psio::to_json(receipt) == R"({"id":1,"memo":"rent"})");
CHECK(psio::from_pssz<Receipt>(psio::to_pssz(receipt)) == receipt);

// The Schema IR is the wrapper-type spelling's: a named alias of the
// bounded UTF-8 string.
CHECK(shape_text<Memo>() == shape_text<MemoInType>());
CHECK(contains(schema_text<Memo>("Memo"), R"("maxCount":80}}},"id":"utf8")"));
```

`Memo` and `MemoInType` are one type on the wire and in the schema
(`Memo → Custom{BoundedList<u8, 80>, "utf8"}`). Use this for a value that
has a name in the application — a memo, a URL, a tag list — and appears in
several records and argument lists: the bound is stated once, and a
parameter of type `Memo` needs no `attr`
([5.11](#511-methodname-), `tut-9c`). Rust: `struct Memo(#[psio(max = 80)] String)`
with `#[psio(transparent)]`; TypeScript: `transparent('Memo', boundedUtf8(80))`;
Python: `@psio.transparent` on a dataclass whose one field is
`Annotated[str, psio.max(80)]`. The shared rows are the `transparent-bounds`
class of `conformance/pssz-vectors.json` (types `Memo`, `WrapMemo`,
`MemoRec`).

| | |
|---|---|
| C++ | `PSIO_REFLECT(T, transparent(), member)`; `PSIO_TYPE_ATTRS(T, psio::transparent)`; `psio::is_transparent_v<T>`, `psio::transparent_inner_t<T>` |
| Rust | `#[psio(transparent)]` on a struct with one member |
| TypeScript | `transparent(name, T)` |
| Python | `AccountId = NewType("AccountId", u64)` (an `int` at run time), or `@psio.transparent` on a class of one field |
| Schema IR | a named alias: `{"name":"AccountId","type": <the member's type>}` |

**Formats.** pssz, key, json / ijson, views, the Schema IR and the
schema-driven codecs: exactly the member. GraphQL: a scalar named after
the type when the member is a scalar. bin / borsh / bincode encode a
one-member struct as its member anyway; ssz, frac and the foreign-schema
formats see the struct.

**Evolution.** A transparent type evolves only from a transparent type of
the same name whose member evolves; the member's bound may only widen and
its `utf8` may not change, as for any member
([8.3](#83-annotations-bounds-may-only-widen)).

**Mistakes** (compile errors, `psio/transparent.hpp`): not exactly one data
member (`psio: a transparent() type has exactly one data member`); combined
with `definitionWillNotChange` / `maxFields` / `maxDynamicData` (text in
[5.4](#54-maxdynamicdatan)); the member is a `std::optional`; an annotation
on the member that is not one of the above — `exact<N>` / `min<N>`,
`sorted` / `unique`, an integer text form, `as<Tag>`:
`psio: the member of a transparent() type takes only the annotations that make it a bounded type: utf8 on a std::string, max<N> on a std::string, std::vector, map or set, and element / keys / values / at<I>(...) below it. exact / min, sorted / unique, integer text forms and as<Tag> are not a type: declare an invariant`.

### 5.8 `invariant(fn)`

**Purpose.** One predicate that says whether a decoded value is a value of
the type at all. It is part of the type: a value that violates it has no
encoding, and bytes or text that decode to one are rejected
([pssz-spec.md §18](pssz-spec.md#18-type-invariants)).

```cpp
struct Symbol
{
   friend bool operator==(const Symbol&, const Symbol&) = default;
   std::string code;
   static psio::codec_status check(const Symbol& s) noexcept
   {
      for (char c : s.code)
         if (c < 'A' || c > 'Z')
            return psio::codec_fail("symbol is not upper case", 0, "Symbol");
      return psio::codec_ok();
   }
   PSIO_REFLECT(Symbol, attr(code, max<8>), invariant(check))
};
static_assert(psio::has_invariant_v<Symbol>);
```

```cpp
// A transparent wrapper is its member in pssz, key and JSON.
CHECK(psio::to_pssz(AccountId{42}) == psio::to_pssz(std::uint64_t{42}));
CHECK(psio::to_key(AccountId{42}) == psio::to_key(std::uint64_t{42}));
CHECK(psio::to_json(AccountId{42}) == "42");

// A value that violates its invariant has no encoding ...
CHECK_THROWS_AS(psio::to_pssz_checked(Symbol{"usd"}), psio::codec_exception);
CHECK_THROWS_AS(psio::to_json_checked(Symbol{"usd"}), psio::codec_exception);
// ... and bytes or text that hold one are refused, with the type's message.
std::vector<char> bytes = psio::to_pssz(Symbol{"USD"});
bytes.back() = 'd';                                             // "USd"
const auto status = psio::validate_pssz<Symbol>(bytes);
CHECK_FALSE(status.ok());
CHECK(status.error().what == std::string_view{"symbol is not upper case"});
CHECK_FALSE(psio::validate_json<Symbol>(R"({"code":"usd"})").ok());
```

| | |
|---|---|
| C++ | `invariant(fn)` in `PSIO_REFLECT`; for an enum, `std::variant`, `std::tuple` or other class: `PSIO_INVARIANT(Type, fn[, "SchemaId"])` at global scope. `fn(const T&) noexcept -> psio::codec_status`. (A schema id without a check is `schemaId("Id")`, [5.15](#515-schemaidid), not an invariant that accepts everything: a type with an invariant has no mutable view) |
| Rust | `#[psio(invariant = "path")]`, `fn(&Self) -> Result<(), E>` |
| TypeScript | `custom(id, wire, { check })`; `check` returns an error string or nothing |
| Python | `@psio.invariant` on a method that returns `None` for a value of the type and a message otherwise; over any type: `Annotated[T, psio.custom("Id", check=fn)]` |
| Schema IR | `Custom{wire type, id}`; a schema cannot carry a predicate, so schema-driven codecs take it as the `check` of the handler registered for `id` |

**Where it runs.** In every format: the validated write (`to_F_checked`,
`encode_F_checked`) and the validating readers (`validate_F`, `from_F`,
`validate_strict`; pssz `decode_pssz_checked`);
`psio::validate_value(v)`; GraphQL arguments and results. **Not** in the
fast write (`to_F`, `psio::encode`), not in the trusted read
(`from_F<T>(psio::trusted{bytes})`, `psio::decode`), not in views.

**Contract.** `noexcept`, returns its verdict, never throws or aborts,
depends only on the value. One per type; arithmetic types take none.

**Evolution.** Not on the wire. Tightening it rejects stored data.

**Mistakes.** `psio: invariant(fn) in PSIO_REFLECT(T) needs fn callable as fn(const T&), declared noexcept and returning psio::codec_status`.

### 5.9 `json_number`, `json_string`, `ijson_number`, `ijson_string`

**Purpose.** Force how one integer member is written in one text format.
Defaults ([json.md](json.md#rules)): `json` writes every integer as a
number; `ijson` writes 64-bit integers as a number up to 2^53 − 1 and a
decimal string beyond, and 128 / 256-bit integers always as a string.

```cpp
struct Totals
{
   friend bool operator==(const Totals&, const Totals&) = default;
   std::uint64_t supply   = 0;
   std::uint64_t holders  = 0;
   std::uint32_t decimals = 0;
   PSIO_REFLECT(Totals,
                attr(supply, json_string | ijson_string),   // always a string
                attr(holders, ijson_number),                // always a number
                decimals)
};
```

```cpp
struct AmountsNumbers   // every integer member a number in I-JSON
{
   std::uint64_t small = 0;
   std::uint64_t large = 0;
};
PSIO_REFLECT(AmountsNumbers, small, large)
PSIO_TYPE_ATTRS(AmountsNumbers, psio::ijson_number)
```

```cpp
const Amounts a{7, 9007199254740991ull, 9007199254740992ull, -9007199254740992ll, true};
CHECK(psio::to_json(a) ==
      R"({"count":7,"small":9007199254740991,"large":9007199254740992,"signed_":-9007199254740992,"frozen":true})");
CHECK(psio::to_ijson(a) ==
      R"({"count":7,"small":9007199254740991,"large":"9007199254740992","signed_":"-9007199254740992","frozen":true})");
// The I-JSON reader accepts either form of a 64-bit integer.
CHECK(psio::from_ijson<Amounts>(
         R"({"count":7,"small":"9007199254740991","large":"9007199254740992","signed_":"-9007199254740992","frozen":true})") ==
      a);
// Type-level ijson_number: every integer member a number.
CHECK(psio::to_ijson(AmountsNumbers{1, 9007199254740992ull}) == R"({"small":1,"large":9007199254740992})");
```

**Applies to.** An integer member of any width, through `std::optional`,
`std::vector` and `std::array`; not into nested records, tuples or
variants. At type level it is the default for every integer member.

| | |
|---|---|
| C++ | `attr(f, json_string)`, `attr(f, json_string \| ijson_number)`; `PSIO_TYPE_ATTRS(T, psio::ijson_number)` |
| Rust | `#[json(json_string)]`, `#[json(json_string, ijson_number)]` on a field or the type |
| TypeScript | `field(T, { jsonInt: 'string', ijsonInt: 'number' })` |
| Python | `Annotated[u64, psio.json_string, psio.ijson_string]`, `psio.json_number`, `psio.ijson_number` on a member |
| Schema IR | member attributes `jsonInt` / `ijsonInt` = `"number"` / `"string"` |

**Formats.** json, ijson and schema-driven text only. No effect on any
binary format, key or width.

**Enforcement.** A reader accepts **exactly** the forced form; without an
annotation the I-JSON reader accepts both forms of a 64-bit integer.

**Evolution.** Changing it breaks text clients (readers of the old form);
binary data is unaffected.

### 5.10 Custom text: `PSIO_ADAPTER`, `as<Tag>`

**Purpose.** Give a type one JSON string as its text, in `json` and
`ijson`, while its binary form stays that of its shape.

```cpp
struct Color
{
   friend bool operator==(const Color&, const Color&) = default;
   std::uint8_t r = 0, g = 0, b = 0;
   PSIO_REFLECT(Color, r, g, b, definitionWillNotChange())
};
struct color_text   // "#rrggbb"
{
   static std::size_t packsize(const Color&) noexcept { return 7; }
   template <typename Sink>
   static void encode(const Color& c, Sink& s)
   {
      static constexpr char d[] = "0123456789abcdef";
      const char            text[] = {'#',         d[c.r >> 4], d[c.r & 15], d[c.g >> 4],
                                      d[c.g & 15], d[c.b >> 4], d[c.b & 15]};
      s.insert(s.end(), text, text + 7);
   }
   static int digit(char c) noexcept
   {
      return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
   }
   static psio::codec_status validate(std::span<const char> t) noexcept
   {
      if (t.size() != 7 || t[0] != '#')
         return psio::codec_fail("color: expected #rrggbb", 0, "Color");
      for (std::size_t i = 1; i < 7; ++i)
         if (digit(t[i]) < 0)
            return psio::codec_fail("color: expected #rrggbb", 0, "Color");
      return psio::codec_ok();
   }
   static psio::codec_status validate_strict(std::span<const char> t) noexcept
   {
      return validate(t);
   }
   static Color decode(std::span<const char> t) noexcept   // called on validated text
   {
      const auto byte = [&](std::size_t i) {
         return static_cast<std::uint8_t>(digit(t[i]) * 16 + digit(t[i + 1]));
      };
      return Color{byte(1), byte(3), byte(5)};
   }
};
}  // namespace ref
PSIO_ADAPTER(ref::Color, psio::text_category, ref::color_text)
```

```cpp
const Pixel p{3, Color{255, 0, 16}};
CHECK(psio::to_json(p) == R"({"x":3,"color":"#ff0010"})");
CHECK(psio::from_json<Pixel>(R"({"x":3,"color":"#ff0010"})") == p);
CHECK_FALSE(psio::validate_json<Pixel>(R"({"x":3,"color":"red"})").ok());
// The binary form is the record's own: 3 bytes.
CHECK(psio::to_pssz(Color{255, 0, 16}).size() == 3);
```

`PSIO_ADAPTER(Type, Tag, Impl)` is written at global scope (the example
closes its namespace first). `Impl` provides `packsize`, `encode`,
`decode`, `validate`, `validate_strict` (`psio/adapter.hpp`). `validate`
runs on untrusted text: it returns a status and never throws; the checking
readers call `decode` only on text `validate` accepted.

A member can pick a named adapter of its type with `attr(f, as<Tag>)`
(`psio::as<Tag>`); the tag types shipped are `hex_tag`, `base58_tag`,
`decimal_tag`, `rfc3339_tag`, `unix_us_tag`, `unix_ms_tag`, and adapters
exist only where a type registers one. The wide integers register
`hex_tag`:

```cpp
struct Wide
{
   friend bool operator==(const Wide&, const Wide&) = default;
   std::uint32_t id = 0;
   psio::uint128 amount{};
   PSIO_REFLECT(Wide, id, attr(amount, as<hex_tag>))   // "0x..." in JSON and I-JSON
};
struct WidePlain   // the same record without the presentation
{
   std::uint32_t id = 0;
   psio::uint128 amount{};
   PSIO_REFLECT(WidePlain, id, amount)
};
// A text-only adapter does not reach the binary formats: the same bound and width.
static_assert(psio::inferred_bound<Wide>() == 1 + 4 + 16);
static_assert(psio::inferred_bound<Wide>() == psio::inferred_bound<WidePlain>());
static_assert(psio::auto_pssz_width_v<Wide> == 1);
```

```cpp
const Wide w{7, psio::uint128{255}};
CHECK(psio::to_json(w) == R"({"id":7,"amount":"0xff"})");
CHECK(psio::to_ijson(w) == R"({"id":7,"amount":"0xff"})");
CHECK(psio::to_json(WidePlain{7, psio::uint128{255}}) == R"({"id":7,"amount":255})");
CHECK(psio::from_json<Wide>(psio::to_json(w)) == w);
// Binary and key: exactly the record without the annotation.
CHECK(psio::to_pssz(w) == psio::to_pssz(WidePlain{7, psio::uint128{255}}));
CHECK(psio::to_key(w) == psio::to_key(WidePlain{7, psio::uint128{255}}));
CHECK(psio::from_pssz<Wide>(psio::to_pssz(w)) == w);
```

A member override is honoured by a binary format only when its adapter can
write bytes. A **text-only** adapter, such as the wide integers' `hex_tag`,
changes JSON and I-JSON and nothing else: the record compiles under every
binary format, and its pssz bytes, key, inferred bound, width and Schema IR
are those of the record without the annotation.

| | |
|---|---|
| Rust | `#[psio(custom)]` / `#[psio(custom = "id")]` and `psio::json_text_form!(T)` with a `TextForm` implementation |
| TypeScript | `custom(id, wire, { toText, fromText })` |
| Python | `@psio.to_text` on a method and `@psio.from_text` on a `classmethod` of the class; over any type: `Annotated[T, psio.custom("Id", to_text=…, from_text=…)]` |
| Schema IR | `Custom{wire type, id}` (id: the adapter's `schema_id`, else the reflected name). A member `as<Tag>` override is not expressible in the IR |

**Limits.** A member whose `as<Tag>` adapter writes the binary form has no
inferred bound (the enclosing record is 4-byte unless capped,
[pssz-spec.md §3.2](pssz-spec.md#32-inferred_boundt)). A member with any
`as<Tag>` is not addressable through a mutable view. A `sortable_binary_category` adapter replaces a
type's key bytes ([key-spec.md](key-spec.md#2-encodings)).

### 5.11 `method(name, …)`

**Purpose.** Reflect a member function, and annotate its arguments and
result. The arguments of a call are the tuple of the parameters; they are
bounded, capped and evolved exactly like a record.

```cpp
struct Bank
{
   void        open(std::string owner, std::vector<std::string> tags);
   void        transfer(AccountId from, AccountId to, std::uint64_t amount, std::string memo);
   std::string symbol(std::uint32_t token) const;
   PSIO_REFLECT(Bank,
                method(open, attr(owner, utf8 | max<32>),
                       attr(tags, max<8> | element(utf8 | max<16>))),
                method(transfer, from, to, amount, attr(memo, utf8 | max<80>),
                       maxDynamicData(1024)),
                method(symbol, token, returns(utf8 | max<8>)))
};
PSIO_INTERFACE(Bank, types())

using OpenArgs     = psio::reflect<Bank>::method_arg_tuple<0>;
using TransferArgs = psio::reflect<Bank>::method_arg_tuple<1>;
// open has no cap: (header + 2 slots) x 1 byte + owner 32 + tags 136 = 171, 8-bit.
static_assert(psio::inferred_bound<OpenArgs>() == 3 + 32 + (8 + 8 * 16));
static_assert(psio::auto_pssz_width_v<OpenArgs> == 1);
// transfer is capped: 16-bit for its lifetime.
static_assert(psio::max_dynamic_data_v<TransferArgs> == 1024);
static_assert(psio::auto_pssz_width_v<TransferArgs> == 2);
// The inferred bound is that of the parameters and has no cap in it:
// header 1 + slot 1 + from 8 + to 8 + amount 8 + memo 80.
static_assert(psio::inferred_bound<TransferArgs>() == 2 + 8 + 8 + 8 + 80);
// The largest argument list is 108 bytes: under the cap the header and the
// slot are 2 bytes each.
static_assert(psio::max_encoded_size<TransferArgs>() == 2 * 2 + 8 + 8 + 8 + 80);
```

Items after the method name, in one flat list:

| Item | Meaning |
|---|---|
| `p` | a parameter name, no annotation |
| `attr(p, specs)` | a parameter name with the annotations a field's `attr` takes |
| `maxDynamicData(N)` | cap (bytes) and width budget of the argument tuple |
| `maxFields(N)` | cap on the number of parameters the method will ever have, N ≤ 255 ([5.6](#56-maxfieldsn)) |
| `returns(specs)` | annotations of the return value: the result is encoded as the type they describe |

`psio::reflect<T>::method_arg_tuple<I>` is the type to encode, decode and
validate arguments as: a `psio::annotated_tuple` that derives from and
converts from `std::tuple<decayed parameters…>`.

An argument list is a tuple and a record at once:

- it has **at most 255 parameters** (the tuple limit,
  [6.11](#611-tuple-and-pair));
- its bound and width are those of the extensible record of its
  parameters: `OpenArgs` is (header + 2 slots) + 32 + 136 = 171, 8-bit;
- its `maxDynamicData(N)` is a record cap in every respect, including
  that it narrows the lists, maps and tuples among the parameters
  ([3.3](#33-a-cap-narrows-what-lies-beneath-it));
- `psio::inferred_bound<TransferArgs>()` is the bound inferred from the
  parameters (106), as for a record; the cap (1024) is
  `psio::max_dynamic_data_v<TransferArgs>` and is what rules the width and
  what the list contributes to the inferred bound of a type that holds it;
- `psio::max_encoded_size<TransferArgs>()` is the largest argument list:
  108 bytes, the same parameters with the 2-byte header and slot the cap
  selects ([5.4](#54-maxdynamicdatan)).

```cpp
// The arguments of a call are the tuple of the parameters, with the
// annotations and the cap of method(...).
const TransferArgs args{AccountId{1}, AccountId{2}, 500, "rent"};
const auto         bytes = psio::to_pssz(args);
CHECK(bytes.size() == 2 + 8 + 8 + 8 + 2 + 4);                 // 16-bit header and slot
const auto back = psio::from_pssz<TransferArgs>(bytes);       // validated: use for untrusted calls
CHECK(std::get<3>(back) == "rent");

// A memo of 81 bytes has no encoding, and a frame holding one is refused.
CHECK_THROWS_AS(psio::to_pssz_checked(TransferArgs{AccountId{1}, AccountId{2}, 500, std::string(81, 'm')}),
                psio::codec_exception);

// The return value is checked against returns(...).
CHECK(psio::check_method_result<Bank, 2>(std::string("USD")).ok());
CHECK_FALSE(psio::check_method_result<Bank, 2>(std::string(9, 'X')).ok());

// An optional argument appended under the cap: old callers still work.
CHECK(psio::to_pssz(TransferArgsV2{AccountId{1}, AccountId{2}, 500, "rent", std::nullopt}) == bytes);
CHECK(std::get<4>(psio::from_pssz<TransferArgsV2>(bytes)) == std::nullopt);
```

**A first-class type as a parameter.** A parameter whose type is a
transparent type over a bounded member ([5.7](#57-transparent)) brings its
bound with it; the argument list is the same type as the one with the
annotated `std::string`:

```cpp
struct Payments
{
   void pay(AccountId from, AccountId to, std::uint64_t amount, Memo memo);
   PSIO_REFLECT(Payments, method(pay, from, to, amount, memo, maxDynamicData(1024)))
};
using PayArgs = psio::reflect<Payments>::method_arg_tuple<0>;
// The bound travels with the type: no attr on the parameter, and the argument
// list is the one of Bank::transfer.
static_assert(psio::inferred_bound<PayArgs>() == psio::inferred_bound<TransferArgs>());
static_assert(psio::max_dynamic_data_v<PayArgs> == 1024);
static_assert(psio::auto_pssz_width_v<PayArgs> == 2);
```

```cpp
// A Memo parameter: the same bytes and the same text as Bank::transfer.
const PayArgs pay{AccountId{1}, AccountId{2}, 500, Memo{"rent"}};
CHECK(psio::to_pssz(pay) == bytes);
CHECK(psio::to_json(pay) == psio::to_json(args));
CHECK_THROWS_AS(psio::to_pssz_checked(PayArgs{AccountId{1}, AccountId{2}, 500, Memo{std::string(81, 'm')}}),
                psio::codec_exception);
```

| | |
|---|---|
| Rust | `#[psio::interface] trait Bank { #[pssz(max_dynamic_data = 1024)] fn transfer(&mut self, …, #[psio(max = 80)] memo: String); #[psio(returns(max = 8))] fn symbol(&self, token: u32) -> String; }`. The argument record is `bank::args::transfer`; the result codec is its `Method::result_to_pssz` / `result_from_pssz` / `result_from_pssz_trusted` / `validate_result` / `result_view` (a view of `Method::ResultAs`, the type `returns` makes of the result). `SchemaBuilder::insert_method::<bank::args::transfer>()` is the method's `FunctionType` (`insert_method_args`, `insert_method_result` its parts). `#[psio(max_fields = N)]` caps the parameter count; `#[psio(positional)]` drops the parameter names from the interface (JSON: the array only) |
| TypeScript | `defineInterface('bank', { transfer: method({ …, memo: field(rawString, { utf8: true, max: 80 }) }, { maxDynamicData: 1024 }), symbol: method({ token: u32 }, { returns: field(rawString, { utf8: true, max: 8 }) }) })`: `Bank.methods.transfer.args` is the argument codec, `Bank.methods.symbol.result` the result codec (the type `returns` describes; `resultToPssz` / `resultFromPssz` / `validateResult` / `resultView` of the method use it; `new SchemaBuilder().insertMethod(method)` is its `FunctionType`, `insertMethodArgs` / `insertMethodResult` its parts), `Bank.schema` the Schema IR with its `Interface`. `method(params, { maxFields: N })` caps the parameter count; `method([...])` without parameter names is positional (JSON: the array only). `argumentTuple(params, { maxDynamicData: 1024 })` is an argument list on its own |
| Python | `@psio.interface(name="bank") class Bank(Protocol)`: parameters are `Annotated[...]` hints, `@psio.method(max_dynamic_data=1024)` caps one method, the return hint carries the result's annotations (`-> Annotated[str, psio.max(8)]`). `psio.interface_of(Bank)["transfer"].args` is the argument codec; `result_to_pssz` / `result_from_pssz` / `validate_result` / `result_view` of a method encode, read and view its result as the type the return hint describes; `result_from_pssz_trusted` reads validated bytes; `method.function_type(psio.SchemaBuilder())` is its `FunctionType`, `Interface.function_types()` all of them. `@psio.method(max_fields=N)` caps the parameter count |
| Schema IR | `PSIO_INTERFACE(T, types())` + `SchemaBuilder::insert_interface<T>()`: `Func{name, params, result, attributes}`; parameters and result are the bounded types; the caps are the function attributes `maxDynamicData` and `maxFields`. The argument list as a type is an `Object` with the attribute `tuple="named"` (every parameter has a name) or `"positional"` (members named `"0"`, `"1"`, …). `emit_wit` writes `@maxDynamicData(N)` above the function. The shared rows are [`conformance/interface-vectors.json`](../conformance/interface-vectors.md) |

**Enforcement.** Arguments: as record members.

**The result.** `returns(specs)` makes the result the type the annotations
describe, and a result is written, read, validated and viewed **as that
type** — the mirror of the argument helpers
([helpers.md](helpers.md#the-result-of-a-method)):

| | |
|---|---|
| `psio::method_result_to_pssz<T, I>(value)`, `method_result_append_pssz<T, I>(out, value)` | the callee's side: the bytes of the result — the fast write ([7.2](#72-reading-and-writing-the-two-modes)): the value is not held against `returns(...)` |
| `psio::method_result_to_pssz_checked<T, I>(value)`, `method_result_append_pssz_checked<T, I>(out, value)` | the validated write: refuses a value outside `returns(...)`, as `validate_method_result` refuses its bytes |
| `psio::method_result_from_pssz<T, I>(bytes)` | the caller's side: validates as the `returns(...)` type, then decodes |
| `psio::method_result_from_pssz<T, I>(psio::trusted{bytes})` | decodes bytes already validated |
| `psio::validate_method_result<T, I>(bytes)` | a `codec_status`. It refuses exactly what the checked decode refuses — the `sorted` / `unique` / `unique_keys` of `returns(...)` included ([5.13](#513-sorted-unique-unique_keys)) — so bytes it accepts may be viewed or decoded as trusted |
| `psio::method_result_buffer<T, I>`, `psio::shared_method_result<T, I>` | a result held packed, owned or shared: the `psio::pssz_buffer` / `psio::shared_pssz` of a result (`B{value}`, `B{bytes}` validating as the result, `view()`, `unpack()`); `psio::method_args_buffer<T, I>` / `shared_method_args<T, I>` for the arguments |
| `psio::validated_method_result_view<T, I>(bytes)` | validates, then views in place: a `psio::checked_view` of `psio::method_result_trusted_view<T, I>`; `psio::method_result_view<T, I>` is the safe view |
| `psio::encode_method_result` / `encode_method_result_checked` / `decode_method_result<T, I>(…, out)` | the fast write, the validated write and the validating read returning a `codec_status` instead of failing through `psio::codec_abort`; `psio::check_method_result<T, I>(value)` only checks |
| `psio::method_result_t<T, I>` | the type a result is encoded as: `X` for a method that returns a `psio::pssz_view<X>`, `void` for one that returns nothing |

A bare string has no width of its own, so `returns(utf8 | max<8>)` does not
change its bytes; a list of variable-size elements does — its offset table
takes the width of the bounded type, which is why the plain codec of the
return type (`psio::to_pssz` / `from_pssz<R>`, `psio::pssz_view<R>`) is the
wrong one for a result:

```cpp
struct Listing
{
   friend bool operator==(const Listing&, const Listing&) = default;
   std::uint64_t id = 0;
   std::string   note;
   PSIO_REFLECT(Listing, id, attr(note, utf8 | max<32>))
};
struct Directory
{
   std::vector<std::string> names(std::uint32_t limit) const;
   std::optional<Listing>     find(std::uint64_t id) const;
   void                     touch(std::uint64_t id);
   PSIO_REFLECT(Directory,
                method(names, limit, returns(max<4> | element(utf8 | max<16>))),
                method(find, id),
                method(touch, id))
};
```

```cpp
const std::vector<std::string> two{"alice", "bob"};
std::vector<char>              bytes;
REQUIRE(psio::encode_method_result<Directory, 0>(two, bytes).ok());
// returns(max<4> | element(utf8 | max<16>)): a 1-byte offset table (02 07) ...
CHECK(hex(bytes) == "02 07 61 6c 69 63 65 62 6f 62");
// ... where the plain std::vector<std::string> has a 4-byte one.
CHECK(psio::to_pssz(two).size() == 2 * 4 + 8);
CHECK(psio::validate_method_result<Directory, 0>(bytes).ok());
std::vector<std::string> back;
REQUIRE(psio::decode_method_result<Directory, 0>(bytes, back).ok());
CHECK(back == two);
// The validated write refuses a result outside returns(...).
std::vector<char> none;
CHECK_FALSE(psio::encode_method_result_checked<Directory, 0>(std::vector<std::string>(5, "a"), none).ok());
CHECK_FALSE(psio::encode_method_result_checked<Directory, 0>(std::vector<std::string>{"\xff"}, none).ok());
CHECK(none.empty());
```

```cpp
const std::vector<std::string> two{"alice", "bob"};
// The callee: the result as returns(...) types it.
const std::vector<char> bytes = psio::method_result_to_pssz<Directory, 0>(two);
CHECK(hex(bytes) == "02 07 61 6c 69 63 65 62 6f 62");
// The caller: validate and decode, decode what is already validated, or view in place.
CHECK(psio::method_result_from_pssz<Directory, 0>(std::span<const char>{bytes}) == two);
CHECK(psio::method_result_from_pssz<Directory, 0>(psio::trusted{bytes}) == two);
const auto view = psio::validated_method_result_view<Directory, 0>(bytes);
REQUIRE(view.ok());
CHECK(view->size() == 2);
CHECK(std::string_view{(*view)[1]} == "bob");
// An empty optional result is zero bytes when its payload cannot be empty ...
CHECK(psio::method_result_to_pssz<Directory, 1>(std::optional<Listing>{}).empty());
CHECK(psio::method_result_from_pssz<Directory, 1>(std::span<const char>{}) == std::nullopt);
// ... and a present one is the value's own encoding.
CHECK(psio::method_result_to_pssz<Directory, 1>(std::optional<Listing>{Listing{7, "ok"}}) ==
      psio::to_pssz(Listing{7, "ok"}));
// A method that returns nothing has no result bytes.
CHECK(psio::validate_method_result<Directory, 2>({}).ok());
```

**An empty optional result is zero bytes** when its payload cannot be empty
(a number, a record, a variant — [pssz-spec.md §5.5.1](pssz-spec.md)): "the
callee returned no bytes" and "the callee returned `None`" are the same
frame, and a present value is its own encoding with nothing added. An
optional whose payload can itself be empty (a string, a list — §5.5.2)
carries a selector byte: `None` is `00`, a present value `01` followed by
its encoding, and no bytes are not a result of such a method. A method that
returns nothing has no result bytes: `validate_method_result` accepts only
the empty span.

Without `returns(...)` the result is the plain codec of the result type.

**The result in a schema.** `SchemaBuilder::insert_interface<T>()` writes
each method as a `Func` whose `result` is the bounded type. A schema that
lists functions its own way — a map of name to `FunctionType`, without the
`Interface` envelope — takes the same nodes one method at a time:

| | |
|---|---|
| `builder.insert_method<T, I>()` | a `FunctionType{params, result}`: the argument list and the result as `returns(...)` types it |
| `builder.insert_method_args<T, I>()` | the argument list: the node of `reflect<T>::method_arg_tuple<I>` |
| `builder.insert_method_result<T, I>()` | the result node (`std::optional`; empty for a method that returns nothing) |

`builder.insert<psio::method_result_t<T, I>>()` is **not** the result: it is
the plain return type, without `returns(...)`.

```cpp
// A schema that lists functions its own way, without the Interface envelope.
psio::SchemaBuilder                builder;
const psio::schema_types::FunctionType names = builder.insert_method<Directory, 0>();
const psio::schema_types::FunctionType touch = builder.insert_method<Directory, 2>();
const psio::Schema                 schema = std::move(builder).build();
// The result node is the bounded type of returns(...): at most 4 strings ...
const auto& list = std::get<psio::schema_types::BoundedList>((*names.result)->value);
CHECK(list.maxCount == 4);
// ... each UTF-8 of at most 16 bytes.
CHECK(std::get<psio::schema_types::Custom>(list.type->value).id == "utf8");
CHECK_FALSE(touch.result.has_value());
// A schema-driven reader checks a result against that node.
bool unknown = false;
CHECK(psio::pssz_ir::validate_auto(schema, **names.result, bytes, &unknown).ok());
```

A `returns(...)` item with no type form — `exact<N>` / `min<N>`, a bound on
a `psio::utf8_string` or a nested value, an integer text form — is the
`Func` attribute `result.<name>`; a bare result node cannot carry it, so
`insert_method_result` reports an error (`builder.status()`) instead of a
schema that says less than the declaration.

**Text.** Binary is always the positional tuple. In JSON and I-JSON an
argument list has two forms ([json.md](json.md#method-arguments)):

| the method declares | written | read |
|---|---|---|
| a name for every parameter | an object keyed by parameter name, in declaration order | that object by the record rule (any order, unknown keys skipped, a missing optional argument is `None`, any other missing argument an error) **or** the positional array with one element per parameter |
| no names, or not for every parameter | the positional array | the array only |

```cpp
// In text a named argument list is written as an object keyed by parameter
// name; binary stays the positional tuple.
CHECK(psio::to_json(args) == R"({"from":1,"to":2,"amount":500,"memo":"rent"})");
// It is read as that object (members in any order) or as the positional array.
CHECK(std::get<3>(psio::from_json<TransferArgs>(R"({"memo":"rent","amount":500,"to":2,"from":1})")) == "rent");
CHECK(std::get<3>(psio::from_json<TransferArgs>(R"([1,2,500,"rent"])")) == "rent");
// The bounds hold in both forms; a missing required argument is an error.
CHECK_FALSE(psio::validate_json<TransferArgs>(R"({"from":1,"to":2,"amount":500})").ok());
CHECK_FALSE(psio::validate_json<TransferArgs>(R"([1,2,500])").ok());
// A missing optional argument is None.
CHECK(std::get<4>(psio::from_json<TransferArgsV2>(R"({"from":1,"to":2,"amount":500,"memo":"rent"})")) ==
      std::nullopt);
```

The schema-driven codecs write and read the same forms from the `tuple`
attribute.

**Evolution.** Append `std::optional` parameters only; declare
`maxDynamicData` from the first version; a declared `maxFields` is never
exceeded, raised or dropped (`tut-9b`, test
`method annotations: a trailing optional argument evolves under the cap`
in `cpp/tests/method_annotation_tests.cpp`).

**Mistakes.** An overloaded, template or ref-qualified member function
cannot be reflected. `psio: method(...) lists more parameters than the function has`;
`psio: method(...) lists maxDynamicData(N) more than once`;
the `maxFields` errors of [5.6](#56-maxfieldsn);
`psio: method(...) lists returns(...) more than once`;
`psio: a returns(...) annotation does not fit the method's return type`.
More than 255 parameters: the tuple-limit error of [6.11](#611-tuple-and-pair).

### 5.12 `PSIO_REFLECT_ENUM` and `field<N>`

`PSIO_REFLECT_ENUM(Enum, enumerator…)` gives an enum its names for text
([6.3](#63-enums)). `attr(f, field<N>)` sets a member's **field number**
(default: 1-based declaration order). Three formats read it, so that a
record evolves the same way in all of them — a new member takes a number
above every number in use: protobuf uses the number itself; Cap'n Proto
assigns ordinals, and FlatBuffers field ids (vtable slots), in ascending
order of it ([capnp.md](capnp.md), [flatbuffers.md](flatbuffers.md)). No
other format reads it:

```cpp
struct Message
{
   std::uint32_t code = 0;
   std::string   text;
   PSIO_REFLECT(Message, attr(code, field<3>), attr(text, field<7>))
};
```

(test `reference: protobuf field numbers`: `Message{1, "x"}` is
`18 01 3a 01 78`). A `std::variant` member is a protobuf `oneof` and takes
one number per alternative, counting up from its own; the numbers of a
message are checked at compile time ([protobuf.md](protobuf.md), "Field
numbers").

protobuf also reads `pb_fixed` and `pb_sint` (`psio/annotate.hpp`) on an
integer member, or on an optional / list / set of integers:
`attr(hash, pb_fixed)` is `fixed32` / `sfixed32` / `fixed64` / `sfixed64` by
the member's width and sign, `attr(delta, pb_sint)` is `sint32` / `sint64`
(signed members only). No other format reads them. In the Schema IR they are
the member attribute `protobuf` = `"fixed"` / `"sint"`, which
`emit_protobuf` writes as the field's type and the schema-driven PSSZ / JSON
codecs of the four languages accept and do not read.

### 5.13 `sorted`, `unique`, `unique_keys`

**Purpose.** Constrain the *content* of a list or array — at the member or
at a position below it (`element(sorted)`) — without changing any byte,
width or bound.

| Spec | The elements are |
|---|---|
| `sorted` (`sorted_spec{}`) | in non-descending order |
| `unique` / `sorted_unique` (`sorted_spec{.unique = true}`) | in strictly ascending order: sorted, no two equal |
| `sorted_spec{.ascending = false}` | the same, descending |
| `unique_keys` | pairwise different by key — the element itself, or element 0 of a pair / tuple element. No order is required |

**Order and equality are those of maps and sets**: bytewise comparison of
the elements' `key` encodings ([key-spec.md](key-spec.md)), the same in
every language. For integers, strings, times and tuples of them that is
the order of the values.

```cpp
struct Ranked
{
   friend bool operator==(const Ranked&, const Ranked&) = default;
   std::vector<std::uint32_t>                         scores;    // non-descending
   std::vector<std::string>                           names;     // strictly ascending: no duplicate
   std::vector<std::pair<std::string, std::uint32_t>> entries;   // no two with the same .first
   PSIO_REFLECT(Ranked,
                attr(scores, sorted | max<4>),
                attr(names, unique),
                attr(entries, unique_keys))
};
struct Unranked   // the same members without the constraints
{
   std::vector<std::uint32_t>                         scores;
   std::vector<std::string>                           names;
   std::vector<std::pair<std::string, std::uint32_t>> entries;
   PSIO_REFLECT(Unranked, attr(scores, max<4>), names, entries)
};
// An order constraint is part of the type: it cannot be added or dropped later.
static_assert(!psio::pssz_evolves_from<Unranked, Ranked>);
static_assert(!psio::pssz_evolves_from<Ranked, Unranked>);
```

```cpp
const Ranked ok{{1, 1, 2}, {"a", "ab", "b"}, {{"x", 1}, {"a", 1}}};
CHECK(psio::from_pssz<Ranked>(psio::to_pssz(ok)) == ok);
// The constraints change no byte: the frame is the unconstrained record's.
CHECK(psio::to_pssz(ok) == psio::to_pssz(Unranked{ok.scores, ok.names, ok.entries}));

// A value out of order has no encoding, in pssz, JSON and key alike.
const Ranked descending{{3, 1, 2}, {}, {}};
CHECK_THROWS_AS(psio::to_pssz_checked(descending), psio::codec_exception);
CHECK_THROWS_AS(psio::to_json_checked(descending), psio::codec_exception);
CHECK_THROWS_AS(psio::to_key_checked(descending), psio::codec_exception);
CHECK_THROWS_AS(psio::to_pssz_checked(Ranked{{}, {"a", "a"}, {}}), psio::codec_exception);            // a duplicate
CHECK_THROWS_AS(psio::to_pssz_checked(Ranked{{}, {}, {{"a", 1}, {"a", 2}}}), psio::codec_exception);  // a key twice

// Every checking reader rejects what a writer without the constraint produces.
const auto bytes = psio::to_pssz(Unranked{{3, 1, 2}, {}, {}});
CHECK_FALSE(psio::validate_pssz<Ranked>(bytes).ok());
CHECK_THROWS_AS(psio::from_pssz<Ranked>(bytes), psio::codec_exception);
CHECK_FALSE(psio::validate_json<Ranked>(R"({"scores":[3,1,2],"names":[],"entries":[]})").ok());
```

**Enforcement.** The validated writes of every format (`to_F_checked`,
`encode_F_checked`; the fast write `to_F` does not look) refuse a value that
violates one (`sorted: elements are not in order`,
`sorted: elements are not in strictly ascending order`,
`unique_keys: two elements have the same key`), and every checking reader
— pssz `validate_pssz`, `validate_strict`, `from_pssz`, the checked
decode; json; key — rejects bytes or text that hold one. Not the trusted
read, not views. The same holds on a method parameter
(`validate_method_args`, `invoke_method_pssz`) and in `returns(...)`
(`validate_method_result`, `validated_method_result_view`,
`decode_method_result`, `method_result_from_pssz`): a validator and the
checked decode of the same name accept the same set (test
`cpp/tests/validate_decode_parity_tests.cpp`).

**Schema IR.** **Not in the Schema IR**: a schema-driven reader of the same
schema does not know the constraint. Treat it as a typed-codec check until
the IR carries it ([pssz-spec.md §3.5](pssz-spec.md#35-bounds-by-annotation)).
Python: `psio.sorted`, `psio.unique`, `psio.sorted(descending=True)`,
`psio.unique_keys` in the `Annotated[...]` metadata, enforced the same way.
Rust: `#[psio(sorted)]`, `#[psio(unique)]` (`sorted_unique`), `#[psio(unique_keys)]` on a
field (ascending). TypeScript: `field(T, { sorted: true | 'descending', unique: true,
uniqueKeys: true })`. Both enforced the same way.

**Notes.** `std::map` and `std::set` carry `sorted` / `unique_keys`
inherently, as a statement of what the type is; the codecs enforce their
order as part of the type and the annotation is not checked a second time.
A member annotated with an order spec is not addressable through a mutable
view. **Evolution:** part of the type — `pssz_evolves_from` is false when
one is added, removed or changed.

**Mistakes.** An order spec on something that is not a list or array is a
compile error: at the member the `applies_to` error of
[5.1](#51-maxn-exactn-minn--length-bounds); below it
`psio: sorted / unique / unique_keys inside element(...), keys(...), values(...) or at<I>(...) constrains the elements of a list (std::vector, psio::bounded) or a std::array; this position is none of those`.

### 5.14 Reserved vocabulary

These spec values exist in `psio/annotate.hpp` and are **RESERVED — not
implemented**: they can be attached and read back with `psio::find_spec`,
and no codec, validator or schema emitter reads them. Attaching one changes
nothing; the names are kept so that a later implementation does not collide
with user code. `hex_spec` (`psio::spec::hex<N>`), `skip_spec`
(`psio::spec::skip`), `default_value_spec` (`psio::spec::default_val<V>`),
`default_factory_spec`, `since_spec`, `unstable_spec`, `final_v`
(`final_spec`), `canonical`, `flags`, `padding`. Do not rely on them.

The rule behind the list: **an annotation is enforced by every codec or it
is reserved.** There is no annotation that only `validate_strict` checks
(`hex_spec` used to be one). Bytes that should read as hex text are a byte
list, whose I-JSON form is hex ([6.5](#65-bytes-and-hex)); a string that
must be UTF-8 says `utf8` ([5.2](#52-utf8--text-or-raw-bytes)).

Read by the formats that number their members only: `field<N>` by protobuf,
capnp and flatbuf ([5.12](#512-psio_reflect_enum-and-fieldn)). The Schema
IR carries what those formats need of a member — its field number when one
is stated, `deprecated`, and a scalar's default when it is not zero — as
the member attributes `field`, `deprecated` and `default`; the schema-driven
PSSZ and JSON codecs accept them and do not read them
([flatbuffers.md](flatbuffers.md#schema), [capnp.md](capnp.md#schema)).
`deprecated_spec{.version = "…"}` is read by GraphQL (the member is
`@deprecated`) and by flatbuf (the member keeps its field id and is neither
written nor read: FlatBuffers' `deprecated`). GraphQL has its own annotations
(`psio::graphql::name_as`, `doc`, `hidden`, `id`, `cost`, `deprecated` —
[graphql.md](graphql.md#customisation)).

### 5.15 `schemaId("Id")`

**Purpose.** Name a record in the Schema IR so that a schema-driven tool can
present it its own way — and change nothing else. The record's schema node
becomes `Custom{<the record>, "Id"}`; a tool that walks bytes with the
schema (`psio::pssz_ir::pssz_to_json` and friends) looks up the handler
registered for `"Id"` and, when there is one, lets it write and read the
value. The typical case is a record whose bytes only another schema
explains: a packed call, shown with its arguments decoded.

```cpp
struct Call
{
   std::uint64_t     sender  = 0;
   std::uint64_t     service = 0;
   std::uint32_t     method  = 0;
   std::vector<char> data;   // the arguments, packed: only the service's schema explains them
   PSIO_REFLECT(Call, schemaId("Action"), sender, service, method, data)
};
static_assert(psio::schema_custom_v<Call>);                 // Custom{record, "Action"} in a schema
static_assert(psio::schema_custom_id<Call>() == "Action");
static_assert(!psio::has_invariant_v<Call>);                // no check
static_assert(!psio::detail::mutation_blocked_v<Call>);     // addressable by psio::mutable_view
```

```cpp
const psio::Schema schema = psio::SchemaBuilder{}.insert<Call>("Call").build();
CHECK(contains(psio::to_ijson(schema), R"("id":"Action")"));

Call call{1, 2, 3, {'h', 'i'}};
auto bytes = psio::to_pssz(call);
// Without a handler the node is its wire type: the record's own text.
std::string text;
CHECK(psio::pssz_ir::pssz_to_json(schema, "Call", psio::json{}, std::span<const char>{bytes}, text).ok());
CHECK(text == psio::to_json(call));
// A handler registered for the id presents it: any one JSON value.
psio::pssz_ir::CustomHandlers handlers;
handlers.add("Action",
             {.to_json = [](std::span<const char> pssz, const psio::pssz_ir::JsonContext&,
                            std::string& json) -> psio::codec_status {
                 const Call c = psio::from_pssz<Call>(psio::trusted{pssz});
                 json += R"({"call":")" + std::to_string(c.service) + "::" +
                         std::to_string(c.method) + R"(","args":")" +
                         std::string{c.data.begin(), c.data.end()} + R"("})";
                 return psio::codec_ok();
              }});
text.clear();
CHECK(psio::pssz_ir::pssz_to_json(schema, "Call", psio::json{}, std::span<const char>{bytes}, text,
                                  nullptr, &handlers)
         .ok());
CHECK(text == R"({"call":"2::3","args":"hi"})");
// The record is overwritten in place like any other.
CHECK(psio::mutable_view<Call, psio::pssz>{bytes}.get<0>().set(9));
call.sender = 9;
CHECK(bytes == psio::to_pssz(call));
```

| | |
|---|---|
| C++ | `schemaId("Id")` in `PSIO_REFLECT`, anywhere in the list. `psio::has_schema_id_v<T>`, `psio::reflected_schema_id<T>()`; `psio::schema_custom_v<T>` is true and `psio::schema_custom_id<T>()` is the id |
| Rust | `#[psio(custom = "Id")]` on a type without a `TextForm` and without `#[psio(invariant)]` |
| TypeScript | `custom(id, wire)` without a handler |
| Python | `Annotated[T, psio.custom("Id")]` without functions |
| Schema IR | `Custom{wire type, id}` |

**What it does not do.** The bytes, the offset width, the key, the typed
`json` / `ijson` text, validation (`validate_pssz`, `validate_strict`),
views and GraphQL are those of the record without the keyword. It is not an
invariant: nothing is checked, and — unlike a type with `invariant(fn)` —
the record is addressable by `psio::mutable_view`. It is not an annotation:
`reflect<T>::type_annotations` does not hold it. Without a handler for the
id a schema-driven codec treats the node as its wire type.

**Which declaration to use.** A text form of its own (one JSON string):
`PSIO_ADAPTER` ([5.10](#510-custom-text-psio_adapter-astag)). A rule the
value must satisfy: `invariant(fn)` ([5.8](#58-invariantfn)). Only a name
for tools: `schemaId`. A type has one schema id: `schemaId` together with a
text adapter's `schema_id` or the id of `PSIO_INVARIANT(T, fn, "Id")` is a
compile error; `schemaId` together with `invariant(fn)` names the checked
type.

**Evolution.** Not on the wire. The schema comparison reports a changed id
(`custom-id-changed`), as for every custom type.

**Mistakes.** An id PSIO gives a meaning itself
([json.md](json.md#custom-types-through-a-schema)):
`psio: schemaId(...) in PSIO_REFLECT(T) names an id reserved for one of PSIO's own types (bool, char, string, utf8, hex, bitvector, bitlist, map, set, box, json, unknown, time-point..., duration..., monotonic...)`.
Twice: `psio: PSIO_REFLECT(T) declares schemaId(...) twice: a type has one schema id`.
Empty: `psio: schemaId("Id") in PSIO_REFLECT(T) needs an id that is not empty`.

## 6. How do I annotate …

### 6.1 Integers and 64-bit JSON

Nothing to annotate for the binary formats: an integer is its
little-endian bytes in pssz and big-endian (sign-flipped if signed) in a
key. In text, choose per member only when a client needs one fixed form:

```cpp
struct Amounts
{
   friend bool operator==(const Amounts&, const Amounts&) = default;
   std::uint32_t count  = 0;
   std::uint64_t small  = 0;
   std::uint64_t large  = 0;
   std::int64_t  signed_ = 0;
   bool          frozen = false;
   PSIO_REFLECT(Amounts, count, small, large, signed_, frozen)
};
static_assert(psio::inferred_bound<Amounts>() == 1 + 4 + 8 + 8 + 8 + 1);   // a 1-byte header

struct AmountsNumbers   // every integer member a number in I-JSON
{
   std::uint64_t small = 0;
   std::uint64_t large = 0;
};
PSIO_REFLECT(AmountsNumbers, small, large)
PSIO_TYPE_ATTRS(AmountsNumbers, psio::ijson_number)
```

Output of `ref-integers-run` ([5.9](#59-json_number-json_string-ijson_number-ijson_string)):
`json` writes `"large":9007199254740992`, `ijson` writes
`"large":"9007199254740992"` and `"small":9007199254740991`. A JavaScript
client should read I-JSON. `psio::uint128`, `psio::int128`,
`psio::uint256` are decimal strings in `ijson`.

### 6.2 `bool`

No annotation. One byte `00` / `01` (any other byte is rejected), `true` /
`false` in text, Schema IR `Custom{Int{1}, "bool"}`.

### 6.3 Enums

An enum is its underlying integer in every binary format and in a key.
Names exist for text only:

```cpp
enum class Status : std::uint8_t { pending, active = 2, closed };
PSIO_REFLECT_ENUM(Status, pending, active, closed)

struct Ticket
{
   friend bool operator==(const Ticket&, const Ticket&) = default;
   std::uint32_t id = 0;
   Status        status{};
   PSIO_REFLECT(Ticket, id, status, definitionWillNotChange())
};
// An enum is its underlying integer: 4 + 1 bytes, and no header (DWNC).
static_assert(psio::inferred_bound<Ticket>() == 5);
```

```cpp
const Ticket t{7, Status::active};
CHECK(hex(psio::to_pssz(t)) == "07 00 00 00 02");                 // the underlying integer
CHECK(psio::to_json(t) == R"({"id":7,"status":"active"})");       // the name in text
CHECK(psio::from_json<Ticket>(R"({"id":7,"status":2})") == t);    // the number is read too
// A value with no listed name is written as its number and read back.
CHECK(psio::to_json(Ticket{7, Status{9}}) == R"({"id":7,"status":9})");
CHECK(psio::from_pssz<Ticket>(psio::to_pssz(Ticket{7, Status{9}})) == Ticket{7, Status{9}});
```

- Without `PSIO_REFLECT_ENUM` the enum is written and read as its number.
- Fix the underlying type explicitly (`: std::uint8_t`).
- **Evolution: only adding enumerators is compatible.** Never change the
  underlying type, renumber or reuse a value. A C++ reader accepts a number
  that names no enumerator (binary and text); a Rust reader needs a
  catch-all (`#[psio(other)] Unknown(u8)`). In text an old reader rejects
  a *name* it does not know (`json: unknown enumerator name`).
- `pssz_evolves_from` compares enums by C++ type: it cannot see added
  enumerators of one enum, and two distinct enum types never evolve from
  one another.
- Rust: `#[repr(u8)] enum Status { Pending = 0, Active = 2, #[psio(other)] Unknown(u8) }`.
  TypeScript: `enumeration(u8, { pending: 0, active: 2, closed: 3 })`.
  Python: `@psio.enum(psio.u8)` above `class Status(enum.Enum)`;
  `@psio.enum(psio.u8, other=True)` keeps an undeclared value as its `int`.

### 6.4 Strings: raw or UTF-8

[5.2](#52-utf8--text-or-raw-bytes). Human-readable text: `utf8 | max<N>`.
Opaque byte strings that happen to be in a `std::string`: `max<N>` alone.
Names shown to users, memos, URLs: text. Hashes, signatures, packed
structures: bytes ([6.5](#65-bytes-and-hex)).

### 6.5 Bytes and hex

A byte sequence is `std::vector` / `std::array` of `std::uint8_t`
(`unsigned char`), `char` or `std::byte`, or `psio::byte_array<N>`. No
annotation is needed for hex: `json` writes an array of numbers, `ijson`
writes an uppercase hex string and reads either case. Bound a variable
one with `max<N>`. The three spellings of the byte are one type in every
respect, the Schema IR included: `Custom{List<u8>, "hex"}`.

```cpp
struct Blob
{
   friend bool operator==(const Blob&, const Blob&) = default;
   std::vector<std::uint8_t>   data;      // variable: hex in I-JSON
   std::array<std::uint8_t, 4> tag{};     // fixed, 4 bytes, no offset
   PSIO_REFLECT(Blob, attr(data, max<64>), tag)
};
static_assert(psio::inferred_bound<Blob>() == 2 + 64 + 4);   // header + slot, data, tag
```

```cpp
const Blob b{{1, 255}, {0xde, 0xad, 0xbe, 0xef}};
CHECK(psio::to_json(b) == R"({"data":[1,255],"tag":[222,173,190,239]})");
CHECK(psio::to_ijson(b) == R"({"data":"01FF","tag":"DEADBEEF"})");
CHECK(psio::from_ijson<Blob>(R"({"data":"01ff","tag":"deadbeef"})") == b);   // either case
```

Schema IR: `Custom{List<u8> | BoundedList<u8, N> | Array<u8, N>, "hex"}`.
`std::vector<std::int8_t>` is a list of numbers, not bytes.

### 6.6 Fixed arrays

`std::array<T, N>` of fixed-size `T` is `N × size` bytes inline with no
count and no offsets. An array of variable-size elements has its own
offset table, whose width comes from the elements' bounds:

```cpp
struct Slots
{
   friend bool operator==(const Slots&, const Slots&) = default;
   std::array<std::uint32_t, 4> counts{};   // fixed: 16 bytes inline
   std::array<std::string, 3>   names;      // variable elements: its own offset table
   PSIO_REFLECT(Slots, counts, attr(names, element(utf8 | max<16>)))
};
// header 1 + counts 16 + slot 1 + names (3 slots of 1 byte + 3 x 16)
static_assert(psio::inferred_bound<Slots>() == 1 + 16 + 1 + (3 + 3 * 16));
static_assert(psio::auto_pssz_width_v<Slots> == 1);
```

`element(...)` is the only position spec an array takes; an array has no
length to bound. `names` has a 1-byte table (3 + 3 × 16 = 51), and a
`pssz_view` reads it at that width
([5.3](#53-element-keys-values-ati--positions-below-the-member)).

### 6.7 Vectors

`attr(v, max<N>)` bounds the count. A list of fixed-size elements is the
elements, back to back, with no table. A list of variable-size elements
starts with an offset table of one slot per element, and the table has the
list's own width:

| The list is | Table slot |
|---|---|
| bounded in count and in its elements, `N + N × element bound ≤ 255` | 1 byte |
| the same, `2N + N × element bound ≤ 65535` | 2 bytes |
| unbounded in count or in its elements, in a record with `maxDynamicData(C)` | the width class of C (of `min(bound, C)` if it is bounded) |
| unbounded, with no capped record above it | 4 bytes |

So if the element is variable-size, bound it too with `element(...)`, or
the type stays unbounded (`tut-2c`) — unless the record has a cap
([3.3](#33-a-cap-narrows-what-lies-beneath-it)). `tags` of `Account`
(8 strings of ≤ 16 bytes: 136) has 1-byte slots; in the frame of
[tutorial step 2](tutorial-annotations.md#step-2--add-bounds-until-the-width-drops)
they are the bytes `02 05`.

### 6.8 Nested vectors

```cpp
struct Grid   // a list of lists
{
   friend bool operator==(const Grid&, const Grid&) = default;
   std::vector<std::vector<std::uint16_t>> rows;
   PSIO_REFLECT(Grid, attr(rows, max<3> | element(max<4>)))
};
// header 1 + slot 1 + rows: 3 slots of 1 byte + 3 x (4 x 2)
static_assert(psio::inferred_bound<Grid>() == 1 + 1 + (3 + 3 * (4 * 2)));
static_assert(psio::auto_pssz_width_v<Grid> == 1);
```

One `element(...)` per level: `max<2> | element(max<2> | element(max<3>))`.
Each level has its own table width: here a row is 4 × 2 fixed bytes with no
table, and the outer list is 3 slots + 3 × 8 = 27 bytes with a 1-byte
table.

### 6.9 Optional

Write the annotations of the payload on the optional member; they apply
when it is present.

```cpp
struct Profile
{
   friend bool operator==(const Profile&, const Profile&) = default;
   std::uint64_t                id = 0;
   std::optional<std::string>   bio;     // the annotation applies to the payload
   std::optional<std::uint32_t> age;
   PSIO_REFLECT(Profile, id, attr(bio, utf8 | max<100>), age)
};
// header 1 + id 8 + bio (slot 1 + selector 1 + 100) + age (slot 1 + selector 1 + 4)
static_assert(psio::inferred_bound<Profile>() == 1 + 8 + (1 + 1 + 100) + (1 + 1 + 4));
```

```cpp
const Profile none{7, std::nullopt, std::nullopt};
const Profile some{7, "hi", 30};
CHECK(psio::to_pssz(none).size() == 1 + 8);                      // trailing None is pruned
CHECK(psio::to_json(none) == R"({"id":7,"bio":null,"age":null})");
CHECK(psio::from_pssz<Profile>(psio::to_pssz(some)) == some);
CHECK_FALSE(encodes(Profile{7, std::string(101, 'b'), std::nullopt}));
```

An optional member costs one offset slot and, in the inferred bound, one
selector byte (reserved whether or not the wire uses it). Trailing absent
optionals are pruned from the encoding. `std::optional` is how a field is
added after publication ([8](#8-evolution-what-may-change-after-publication)).

### 6.10 Variant

`at<I>(specs)` annotates alternative `I`:

```cpp
struct Choice   // a variant alternative
{
   friend bool operator==(const Choice&, const Choice&) = default;
   std::variant<std::uint32_t, std::string> v;
   PSIO_REFLECT(Choice, attr(v, at<1>(utf8 | max<16>)))
};
// header 1 + slot 1 + selector 1 + the widest alternative 16
static_assert(psio::inferred_bound<Choice>() == 1 + 1 + 1 + 16);
```

A variant is one selector byte plus the alternative (at most 256
alternatives). Text: `[index, value]` in `json`, `{"label": value}` in
`ijson`, where the label is the alternative's one name
([below](#variant-alternative-labels)). **Alternatives may be
appended** ([pssz-spec.md §7.5](pssz-spec.md#75-what-you-can-change-without-breaking-old-decoders)):
`pssz_evolves_from<variant<A, B>, variant<A, B, C>>` is true. The new
reader reads everything the old writer produced; an old reader rejects a
value that holds the new alternative, as it rejects any value outside its
type, so upgrade readers before writers. Removing, reordering, inserting or
retyping an alternative is not compatible. A new alternative that is larger
than the others raises the variant's bound (1 + the largest alternative);
if that moves the offset width of what holds the variant — a record without
a cap, a tuple, an array, a bounded list or map — the check refuses it,
exactly as it refuses an appended field that moves a width
([8.4](#84-summary)).

#### Variant alternative labels

An alternative has **one label**. It is the key of the tagged text form
(`{"label": value}`), the name of the alternative in the Schema IR
(`Variant.members[i].name`) and what a schema comparison calls it. A binary
format has no labels: an alternative is selected by its position, so a
label can change without changing a byte.

The rule is the same in every language:

1. **The name the declaration gives the alternative**, where the language
   names alternatives: a Rust enum variant's identifier, the key of a
   TypeScript `variant({...})`.
2. **Else the name of the alternative's type, as its language declares
   it.** A `std::variant` or a Python `Union` names no alternative, so the
   label is the type's name: a reflected record's (enum's, transparent
   type's) name, and for the types that have no declared name

   | type | label |
   |---|---|
   | `bool`; integers; floats | `bool`; `int8` `uint8` `int16` `uint16` `int32` `uint32` `int64` `uint64` `int128` `uint128`; `float32` `double` |
   | a string (raw or UTF-8, bounded or not) | `string` |
   | a list / bounded list of `T`; bytes | `T[]`; `uint8[]` (C++ `std::vector<char>`: `char[]`) |
   | an array of `N` `T` | `T[N]` |
   | an optional `T` | `T?` |
   | a tuple / pair of `A`, `B` | `tuple_A_B` |
   | a map from `K` to `V`; a set of `K` | `map_K_V`; `set_K` |
   | a variant of `A`, `B` | `variant_A_B` |

   built from the labels of the types they hold (`string[]`, `Circle?`,
   `tuple_uint32_string`).
3. **Else its position**, `"0"`, `"1"`, …: for a type with no name at all,
   and for an alternative whose type name another alternative of the same
   variant has too. `std::variant<std::uint32_t, std::string, std::string>`
   is labelled `uint32`, `1`, `2`.

A variant therefore always has labels and they are always distinct. To
**state** a label — to match a name imposed from outside, or to give two
alternatives of one type names of their own — use the language's
annotation:

| | |
|---|---|
| C++ | specialize `psio::variant_labels<V>` with one label per alternative, in order (`psio/variant_label.hpp`); `psio::variant_label<V, I>()` is the label in force |
| Rust | `#[psio(name = "…")]` (or `#[serde(rename = "…")]`, the enum's `rename_all`) on the enum variant |
| TypeScript | the key *is* the label: `variant({ uint32: u32, string: utf8 })`; `variant([['uint32', u32], ['1', utf8], ['2', utf8]])` when a label is an integer that is not first (JavaScript reorders integer keys) |
| Python | `Annotated[T, psio.alt("…")]` on the alternative |

Stated labels are nonempty and distinct (a compile-time / declaration-time
error otherwise).

```cpp
struct Circle
{
   friend bool operator==(const Circle&, const Circle&) = default;
   std::uint32_t radius = 0;
   PSIO_REFLECT(Circle, radius)
};
struct Square
{
   friend bool operator==(const Square&, const Square&) = default;
   std::uint32_t side = 0;
   PSIO_REFLECT(Square, side)
};
// The name of the alternative's type: a reflected type's name, the table for the rest.
using Shape = std::variant<Circle, Square, std::uint32_t, std::vector<std::string>>;
static_assert(psio::variant_label<Shape, 0>() == "Circle");
static_assert(psio::variant_label<Shape, 2>() == "uint32");
static_assert(psio::variant_label<Shape, 3>() == "string[]");
// A type name two alternatives share is no label: they are labelled by position.
using Repeated = std::variant<std::uint32_t, std::string, std::string>;
static_assert(psio::variant_label<Repeated, 0>() == "uint32");
static_assert(psio::variant_label<Repeated, 1>() == "1");
static_assert(psio::variant_label<Repeated, 2>() == "2");
using Sized = std::variant<Circle, Square, std::uint32_t>;   // its labels are stated below
```

```cpp
// Stated labels: one per alternative, in order.
template <>
struct psio::variant_labels<ref::Sized>
{
   static constexpr std::array<std::string_view, 3> names{"circle", "square", "side"};
};
static_assert(psio::variant_label<ref::Sized, 2>() == "side");
```

```cpp
// The label is the key of the tagged text form ...
CHECK(psio::encode(psio::ijson{}, Shape{Circle{3}}) == R"({"Circle":{"radius":3}})");
CHECK(psio::encode(psio::ijson{}, Shape{std::uint32_t{7}}) == R"({"uint32":7})");
CHECK(psio::encode(psio::ijson{}, Repeated{std::in_place_index<2>, "b"}) == R"({"2":"b"})");
CHECK(psio::encode(psio::ijson{}, Sized{Square{4}}) == R"({"square":{"side":4}})");
CHECK(psio::decode<Sized>(psio::ijson{}, std::string_view{R"({"side":5})"}) ==
      Sized{std::uint32_t{5}});
// ... and the name of the alternative in the schema.
psio::SchemaBuilder builder;
const auto          type   = builder.insert<Sized>();
const psio::Schema  schema = std::move(builder).build();
const auto& members = std::get<psio::schema_types::Variant>(type.resolve(schema)->value).members;
CHECK(members[0].name == "circle");
CHECK(members[2].name == "side");
// Binary has no labels: an alternative is its position.
CHECK(hex(psio::to_pssz(Sized{std::uint32_t{5}})) == "02 05 00 00 00");
```

The implementing language is the naming authority: the twin of a type in
another language renames to match it. The Rust twin of
`std::variant<std::uint32_t, std::string>` is
`enum V { #[psio(name = "uint32")] Number(u32), #[psio(name = "string")] Text(String) }`;
the C++ twin of a Rust `enum Shape { Circle(Circle), Side(u32) }` states
`{"Circle", "Side"}` with `psio::variant_labels`. The Schema IR carries
nothing else about an alternative's name (there is no `ijsonName`
attribute), so two declarations with the same labels are one schema node.
The shared rows are
[`conformance/variant-label-vectors.json`](../conformance/variant-label-vectors.md).

GraphQL: a `std::variant` of records is a union of the alternatives' object
types, each under its GraphQL type name — the label, unless
`psio::graphql::type_name` renames the type. A label cannot rename a member
of a union: GraphQL names types, not alternatives.

**Evolution.** A label is part of the text contract: `psio::pssz_ir::evolves_from`
/ `equivalent` under `compare::contract` report a changed label as
`alternative-renamed`; under `compare::wire` labels are not compared. Rule 3
means that appending a second alternative of a type the variant already
holds relabels the first (`string` becomes `1`): state the labels of a
published variant that may grow that way.

### 6.11 Tuple and pair

`at<I>(specs)` annotates element `I` (`author` in `Post`,
[5.3](#53-element-keys-values-ati--positions-below-the-member)). A tuple is
laid out as the extensible record of its elements — a header and one slot
per variable-size element — and takes its own width from their bounds (or
from the cap of the record it is in); a `std::pair` is the 2-tuple. Text: a
JSON array.

```cpp
using Entry = std::tuple<std::uint32_t, psio::utf8_string<16>>;
// header 1 + u32 4 + slot 1 + 16
static_assert(psio::inferred_bound<Entry>() == 1 + 4 + 1 + 16);
static_assert(psio::auto_pssz_width_v<Entry> == 1);
static_assert(psio::max_tuple_elements == 255);   // a larger tuple does not compile
```

**A tuple has at most 255 elements.** A `std::tuple` with more — and so a
method with more than 255 parameters — is not a PSSZ type: every pssz
entry point, the width and bound traits and the schema builder refuse it
at compile time with

`psio: a tuple has at most 255 elements (docs/pssz-spec.md 8.4); this one has more. Use a record, a list, or nested tuples`

(tests `psio_pssz_tuple_limit_*_compile_fail`), and a schema-driven reader
rejects a schema that holds such a `Tuple` before reading any value. The
limit changes no byte: a tuple's header is the size of its fixed region,
not a count. A record has the same limit of 255 fields
([5.6](#56-maxfieldsn)); a variant has at most 256 alternatives
([pssz-spec.md §8.4](pssz-spec.md#84-element-counts)).

### 6.12 Map and set

```cpp
struct Book
{
   friend bool operator==(const Book&, const Book&) = default;
   std::map<std::string, std::uint64_t> balances;
   std::set<std::uint32_t>              members;
   PSIO_REFLECT(Book, attr(balances, max<4> | keys(utf8 | max<8>)), attr(members, max<4>))
};
// balances: an entry is header 1 + key (slot 1 + 8) + 8 = 18; the map is 4 slots + 4 x 18 = 76
// record:   (header + 2 slots) x 1 byte + balances 76 + members 4 x 4
static_assert(psio::inferred_bound<Book>() == 3 + (4 + 4 * 18) + 4 * 4);
static_assert(psio::auto_pssz_width_v<Book> == 1);
```

```cpp
const Book book{{{"bob", 5}, {"al", 7}}, {3, 1}};
CHECK(psio::to_json(book) == R"({"balances":{"al":7,"bob":5},"members":[1,3]})");
CHECK(psio::from_pssz<Book>(psio::to_pssz(book)) == book);
CHECK_FALSE(encodes(Book{{{std::string(9, 'k'), 1}}, {}}));       // a key of 9 bytes
CHECK_FALSE(encodes(Book{{}, {1, 2, 3, 4, 5}}));                  // 5 members
```

`max<N>` bounds the entry count, `keys(...)` / `values(...)` the entries
(`element(...)` for a set). On the wire a map is the list of its
`(key, value)` entries in ascending key-byte order and a set the list of
its elements ([pssz-spec.md §5.9](pssz-spec.md#59-pairs-maps-and-sets--paira-b-mapk-v-setk));
readers reject entries out of order or duplicated. The widths follow the
list rule ([6.7](#67-vectors)): `balances` has a 1-byte table (4 slots +
4 × 18), and each entry is a tuple with its own 1-byte header and key slot.
Text: an object when the key's text is a string, else an array of pairs.
There is no bounded map *type*; the count is always an annotation.

### 6.13 `psio::nested<T>`

A member that holds the pssz encoding of a `T` as a byte list (an action's
arguments inside a transaction). `max<N>` bounds the byte count:

```cpp
struct Payload
{
   friend bool operator==(const Payload&, const Payload&) = default;
   std::string text;
   PSIO_REFLECT(Payload, attr(text, utf8 | max<32>))
};
struct Action
{
   std::uint64_t         sender = 0;
   psio::nested<Payload> args;       // on the wire: a byte list holding a Payload
   PSIO_REFLECT(Action, sender, attr(args, max<64>))
};
// header 1 + sender 8 + args (slot 1 + 64)
static_assert(psio::inferred_bound<Action>() == 1 + 8 + (1 + 64));
static_assert(psio::auto_pssz_width_v<Action> == 1);
```

```cpp
const Action a{5, psio::nested<Payload>::from(Payload{"hi"})};
// The member is a byte list holding the Payload's own encoding (01 01 68 69).
CHECK(hex(psio::to_pssz(a)) == "09 05 00 00 00 00 00 00 00 09 01 01 68 69");
// In text it is the inner value, not hex.
CHECK(psio::to_ijson(a) == R"({"sender":5,"args":{"text":"hi"}})");
CHECK(psio::from_pssz<Action>(psio::to_pssz(a)).args.decode() == Payload{"hi"});
```

Without a bound a nested member is unbounded. Its bytes are validated as a
`T` by every checking reader of the enclosing value, unless declared
`psio::nested<T, psio::nested_validation::deferred>`. Schema IR:
`Nested{T}` plus the member attribute `lengthBound="max=N"`. As a key
element it orders by its bytes, not by the inner value. Rust:
`Nested<T, V, N>`; TypeScript: `nested(T)` with `field(…, { max: N })`;
Python: `Annotated[T, psio.nested(), psio.max(N)]`. The inner value is a
root of its own: a cap of the enclosing record does not reach into it
([3.3](#33-a-cap-narrows-what-lies-beneath-it)).

### 6.14 Records

A record is extensible by default: a `W`-byte header and one `W`-byte slot
per variable-size member. Decide three things when you declare it:

1. Is every variable-size member bounded? ([5.1](#51-maxn-exactn-minn--length-bounds))
2. May it gain fields? Then `maxDynamicData(N)` ([5.4](#54-maxdynamicdatan)).
3. Is it closed forever? Then `definitionWillNotChange()` ([5.5](#55-definitionwillnotchange)).

A record has at most 255 fields; `maxFields(N)` promises a lower number for
this type's lifetime ([5.6](#56-maxfieldsn)).

A member whose type is another record contributes that record's cap, or
its inferred bound:

```cpp
struct Line   // a child's cap is what the parent reserves for it
{
   tut::AccountV1 a;   // maxDynamicData(1024)
   PSIO_REFLECT(Line, a)
};
// (header + slot) x 2 bytes + the child's cap
static_assert(psio::inferred_bound<Line>() == 2 * 2 + 1024);
static_assert(psio::auto_pssz_width_v<Line> == 2);
```

Each record owns its width: a child keeps its own `W` wherever it is
nested, also beneath a parent with a smaller cap
([3.3](#33-a-cap-narrows-what-lies-beneath-it),
[pssz-type-width.md](pssz-type-width.md)). `Line` is (header + slot) × 2
bytes + the child's cap of 1024.

### 6.15 Transparent wrappers

[5.7](#57-transparent). Use one for every id / key / quantity that should
not be confused with a bare integer or byte list, and for every bounded
string or list that has a name of its own (a memo, a URL). State the bound
on the member — `attr(contents, utf8 | max<80>)` — or in the member's type
(`psio::utf8_string<80>`); the two are one type.

### 6.16 Custom text types and adapters

[5.10](#510-custom-text-psio_adapter-astag). The binary form is unaffected, so
annotate bounds as for the underlying shape.

### 6.17 Time points

No annotation. `std::chrono` types are recognised by type
([time.md](time.md)):

```cpp
struct Event
{
   friend bool operator==(const Event&, const Event&) = default;
   std::chrono::sys_time<std::chrono::milliseconds> at;    // a time point: a date in text
   std::chrono::seconds                             ttl{}; // a duration: its tick count
   PSIO_REFLECT(Event, at, ttl, definitionWillNotChange())
};
static_assert(psio::inferred_bound<Event>() == 8 + 8);
```

```cpp
const Event e{std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{1795342530123}},
              std::chrono::seconds{30}};
CHECK(psio::to_pssz(e).size() == 16);                                    // two 64-bit tick counts
CHECK(psio::to_ijson(e) == R"({"at":"2026-11-22T10:15:30.123Z","ttl":30})");
CHECK(psio::from_ijson<Event>(psio::to_ijson(e)) == e);
```

Binary and key: the tick count as its integer. Text: a `system_clock`
time point is an RFC 3339 UTC string; a duration or a time point of
another clock is its tick count. Schema IR: `Custom{Int, "time-point-millis"}`,
`Custom{Int, "duration-seconds"}`. Rust: `psio::time::TimePoint<Millis>`,
`Duration<Seconds>`. TypeScript: `timePoint('millis')`, `duration('seconds')`
(`bigint` ticks).

### 6.18 Methods, action arguments, return values

[5.11](#511-methodname-). Treat every action's argument tuple as a
published type: bound each variable-size parameter, declare
`maxDynamicData`, validate arguments that arrive from outside. Write a
result with `psio::method_result_to_pssz<T, I>` (`_checked` to hold it
against `returns(...)` where it is written) and read it with
`psio::method_result_from_pssz<T, I>` (or view it with
`psio::validated_method_result_view<T, I>`) so that it has the bytes of its
`returns(...)` type; put it in a schema with
`SchemaBuilder::insert_method<T, I>()`. In JSON a named argument list is an object; accept
the positional array too.

### 6.19 Tables and keys

A table row is a record; its keys are key-format encodings of one or more
of its fields ([key-spec.md](key-spec.md)).

```cpp
// The primary key of the accounts table, and a secondary index by owner.
using AccountKey = std::uint64_t;
using ByOwner    = std::tuple<std::string, std::uint64_t>;   // (owner, id)

struct OwnerKey
{
   friend bool operator==(const OwnerKey&, const OwnerKey&) = default;
   std::string   owner;
   std::uint64_t id = 0;
   PSIO_REFLECT(OwnerKey, attr(owner, utf8 | max<32>), id)
};
```

```cpp
// An integer key is big-endian: byte order is numeric order.
CHECK(hex(psio::to_key(AccountKey{258})) == "00 00 00 00 00 00 01 02");
// A composite key is the concatenation; a string is terminated by 00 00.
CHECK(hex(psio::to_key(ByOwner{"bob", 7})) == "62 6f 62 00 00 00 00 00 00 00 00 00 07");
// All keys of one owner: a prefix scan from the prefix to its upper bound.
const auto prefix = psio::key_prefix<ByOwner>("bob");
CHECK(hex(prefix) == "62 6f 62 00 00");
CHECK(hex(*psio::key_prefix_upper_bound(prefix)) == "62 6f 62 00 01");
CHECK(psio::from_key<ByOwner>(psio::to_key(ByOwner{"bob", 7})) == ByOwner{"bob", 7});

// A reflected record is the key of its fields in order, with the same
// bytes as the tuple: annotations do not change a key.
CHECK(psio::to_key(OwnerKey{"bob", 7}) == psio::to_key(ByOwner{"bob", 7}));

// The key writer checks what the pssz writer checks: a value outside a
// bound, or a utf8 member that is not UTF-8, has no key.
const OwnerKey too_long{std::string(33, 'x'), 7};                 // one byte past max<32>
CHECK_THROWS_AS(psio::to_key_checked(too_long), psio::codec_exception);
std::vector<char> out;
CHECK_FALSE(psio::encode_key_checked(too_long, out).ok());        // the status form
CHECK(out.empty());
CHECK_THROWS_AS(psio::to_key_checked(OwnerKey{"\xff", 7}), psio::codec_exception);
// The readers refuse the key bytes a writer without the bound produces.
const auto unbounded = psio::to_key(ByOwner{std::string(33, 'x'), 7});
CHECK_FALSE(psio::validate_key<OwnerKey>(unbounded).ok());
CHECK_THROWS_AS(psio::from_key<OwnerKey>(unbounded), psio::codec_exception);
```

- Annotations do not change key bytes; there is no width in a key.
- Key types: integers, bool, enums, strings, byte lists, lists, optionals,
  fixed arrays, variants, tuples / pairs / records, maps / sets,
  transparent wrappers, time values, `nested<T>` (ordered by bytes).
  Not `bitlist`.
- Put the most significant element first; a tuple key is a byte prefix of
  the longer tuple, so prefix scans work
  ([key-spec.md §3](key-spec.md#3-composite-keys)).
- The row itself: bound it and cap it at 65535 or less unless it holds
  genuinely unbounded content ([3.4](#34-how-to-pick-a-cap)).
- `to_key` is the fast write: every value has a key, whatever its type
  declares. The validated write checks member bounds, `utf8`, order specs
  and invariants: `to_key_checked` throws, `psio::encode_key_checked(value,
  out)` returns a status. Validate a key that arrives from outside
  (`validate_key` / `from_key`).

### 6.20 Bit types

`psio::bitvector<N>` is exactly N bits: a fixed-size member of ⌈N/8⌉ bytes,
inline, with no offset slot. `psio::bitlist<N>` is at most N bits: a
variable-size member (one slot) of at most ⌈N/8⌉ + 1 bytes, the bits and a
delimiter bit. Both count in the inferred bound, so a record whose only
variable-size members are bit types is bounded and narrow without a cap:

```cpp
struct Flags
{
   std::uint8_t        kind = 0;
   psio::bitvector<12> mask;   // fixed: 2 bytes inline, no slot
   psio::bitlist<12>   seen;   // variable: at most 12 bits and the delimiter bit
   PSIO_REFLECT(Flags, kind, mask, seen)
};
// header 1 + kind 1 + mask 2 + seen (slot 1 + 2 + 1)
static_assert(psio::inferred_bound<Flags>() == 1 + 1 + 2 + (1 + 2 + 1));
static_assert(psio::auto_pssz_width_v<Flags> == 1);
```

No annotation applies to a `bitvector`; `max<N>` applies to a `bitlist`
member. A `bitlist` is not a key type.

### 6.21 Names as numbers

Not an annotation, but part of what an interface publishes: an account
name and a method name each have a 64-bit number, and the two coders
follow one rule — **a number is given only to text it decodes back to.**

```cpp
// An account name: a number is given only to text it decodes back to.
static_assert(psio::name_to_number("alice") == 6697231);
static_assert(psio::name_to_number("x--b") == 0);    // no number decodes to it: not a name
static_assert(psio::name_to_number("Alice") == 0);   // outside the alphabet
// A method name: compressed when the number decodes to the case-folded name ...
static_assert(psio::detail::method_to_number("transfer") == psio::detail::method_to_number("Transfer"));
static_assert(!psio::detail::is_hash_name(psio::detail::method_to_number("transfer")));
// ... and a hash of the name as written otherwise (here the code needs more than 64 bits).
static_assert(psio::detail::method_to_number("getSymbolType") == 0xd55df3070b8758ebull);
static_assert(psio::detail::is_hash_name(psio::detail::method_to_number("getSymbolType")));
static_assert(psio::hash_name("getSymbolType") == 0xd55df3070b8758ebull);
static_assert(psio::detail::method_to_number("#abc") == 0);   // malformed hash text: not a name
```

```cpp
CHECK(psio::number_to_name(6697231) == "alice");
CHECK(psio::detail::number_to_method(psio::detail::method_to_number("Transfer")) == "transfer");
// A hash number is shown as '#' and 16 letters, and reads back.
CHECK(psio::detail::number_to_method(0xd55df3070b8758ebull) == "#hmlnslheseoypnnp");
CHECK(psio::detail::method_to_number("#hmlnslheseoypnnp") == 0xd55df3070b8758ebull);
```

- `psio::name_to_number(text)` (`psio/name.hpp`): the number of an account
  name, or 0 when the text is not a name — outside the alphabet, a doubled
  hyphen, or a code that does not fit in 64 bits. `psio::number_to_name`
  reads it back.
- `psio::detail::method_to_number(text)` (`psio/compress_name.hpp`): the
  compressed number when it decodes to the case-folded name (so
  `transfer` and `Transfer` are one method), otherwise a **hash** of the
  name as written (case matters there), shown as `#` and 16 letters.
  Malformed `#` text is 0. `psio::hash_name` / `psio::is_compressed_name`
  (`psio/get_type_name.hpp`) are the same function.
- The shared rows are `conformance/name-vectors.json`; the other
  languages' coders are in [4.1](#41-tooling-per-language).

## 7. Reading: views, validation, trust

### 7.1 The entry points

| Call | What it does |
|---|---|
| `psio::validate_pssz<T>(bytes)` | returns a `codec_status`; never throws or aborts |
| `psio::from_pssz<T>(bytes)` | validates, then decodes; on invalid bytes throws `codec_exception` (aborts in a build without exceptions) |
| `psio::from_pssz<T>(psio::trusted{bytes})` | decodes without the validation pass |
| `psio::pssz_view<T>{bytes}` | reads in place; validates nothing |
| `psio::validated_pssz_view(psio::pssz{}, bytes, out, has_unknown)` | validates, then gives the view |
| `psio::validate_strict<T>(psio::pssz{}, bytes)` | exact schema: also rejects unknown appended fields |
| `psio::decode_pssz_checked(psio::pssz{}, bytes, out, has_unknown)` | status-returning decode that reports unknown fields |

The same names exist per format: `from_json`, `validate_json`, `from_key`,
`validate_key`, … ([helpers.md](helpers.md)).

```cpp
const AccountV2 alice{7, "alice", {"vip", "beta"}, 100, "alice@example.com"};
const std::vector<char> bytes = psio::to_pssz(alice);

// Untrusted bytes: validate first. validate_pssz never throws.
REQUIRE(psio::validate_pssz<AccountV2>(bytes).ok());

// A view reads in place: nothing is copied or allocated.
const psio::pssz_view<AccountV2> view{bytes};
CHECK(view.get<0>() == 7u);
CHECK(view.get<1>().view_() == "alice");
CHECK(view.get<2>().size() == 2);
CHECK(view.get<2>()[1].view_() == "beta");

// from_pssz<T>(bytes) validates and decodes; trusted skips the validation
// pass for bytes this program already validated or wrote itself.
CHECK(psio::from_pssz<AccountV2>(bytes) == alice);
CHECK(psio::from_pssz<AccountV2>(psio::trusted{bytes}) == alice);
```

### 7.2 Reading and writing: the two modes

Every format has the same four calls. A read validates unless the caller
vouches for the bytes; a write is fast unless the caller asks for the
check.

| | ordinary call | the other mode, asked for by name |
|---|---|---|
| **read** | **validating**: `from_F<T>(bytes)`, `validate_F<T>(bytes)`, `psio::validated_view` — the structure of the bytes and everything `T` declares | **trusted**: `from_F<T>(psio::trusted{bytes})`, `psio::decode<T>(F{}, bytes)`, a view and its `unpack()` — nothing is held against the declaration |
| **write** | **fast**: `to_F(v)`, `to_F(v, sink)`, `psio::encode(F{}, v)` — nothing is held against the declaration | **validated**: `to_F_checked(v)`, `to_F_checked(v, sink)` (fail as `from_F` does), `encode_F_checked(v, out)`, `psio::encode_checked(F{}, v, out)` (a `codec_status`; `out` unchanged on failure) |

"What `T` declares": member bounds (`max` / `min` / `exact`, at the member
and below it), `utf8`, the order specs, invariants, and the bounds of
`psio::bounded` / `psio::utf8_string`. The validated write refuses exactly
what the validating reader of the same format refuses, with its message.
`psio::validate_value(v)` runs the same check without a format
(`psio: member length_bound violated`, the `utf8_spec` / order messages, the
invariant's own): validate once, then write fast to several formats.

**The fast write and what it means for a service.** `to_pssz(row)` no
longer tells a service that `row` violates its own type. The bytes are
written; a validating reader — `from_pssz`, `validate_pssz`, another
service's action dispatch, a schema-driven tool — refuses them later, and a
reader that trusts them never notices. Use the validated write where bytes
leave the writer's hands and the mistake would surface somewhere else, or
nowhere: rows of a table that is later read with `psio::trusted`, arguments
built for another service's action, action results, replies to HTTP / RPC
callers, events. Keep the fast write for a value that was just validated
(re-encoding a decoded argument) and for paths whose values the program
constructs under test.

**What the fast write still refuses.** A fast write never produces bytes
that a trusted reader of the same type reads as a different value or reads
outside the buffer. Where a declaration decides the layout, a value the
layout cannot hold has no bytes, and the write fails — a comparison the
encoder makes anyway while it sizes the value:

| Format | A declaration changes the layout? | The fast write still fails on | Left to the validated write |
|---|---|---|---|
| pssz | yes: bounds and caps choose the offset width (1 / 2 / 4 bytes) of a record, list, map, tuple | a payload that needs an offset its type's width cannot express; a total over the root's `maxDynamicData`, a nested record over its own cap (the cap is the width budget); a total over 4 GiB; a nonempty vector of zero-size elements; a null `Box`; nesting over the depth limit | a bound exceeded while every offset still fits (in a record the last variable member ends where the record ends, so it can be far over its bound and still have a frame), `min`, `exact`, `utf8` and `utf8_string`, `bounded<T, N>`, order specs, bitlist capacity, invariants |
| key | no | nothing a type declares | all of it |
| json / ijson | no | what is not JSON whatever the type says: a string that is not UTF-8, a non-finite float, a null `Box`, a time point outside years 0000..9999 | member bounds, order specs, invariants |
| bin, ssz, borsh, bincode, avro, msgpack, bson, wit ABI | no | the format's own limits | all of it |
| flatbuf, capnp, protobuf | no | the format's own limits (2 GiB) | all of it |

**What is checked where**, per call:

| | structure | member bounds, `utf8`, order specs | `maxDynamicData` | invariants |
|---|---|---|---|---|
| pssz `to_pssz` / `encode` (fast) | — | only where the offsets cannot express the value | yes | **no** |
| pssz `to_pssz_checked`, `encode_pssz_checked` (validated) | — | yes | yes | yes |
| pssz `validate_pssz`, `from_pssz`, `validate_strict`, `decode_pssz_checked` | yes | yes | yes | yes |
| pssz `from_pssz(psio::trusted{…})` / `psio::decode` / `unpack()` (trusted) | every span it follows lies in the bytes ([on bytes that are not a frame](#on-bytes-that-are-not-a-frame)); not the canonical form | **no** | the cap of a record, a comparison on its span | **no** |
| `pssz_view`, `pssz_trusted_view` | every member it reads is placed inside the bytes, or the read fails; not the canonical form | **no** | **no** | **no** |
| key `to_key`, `key_prefix` (fast) | — | **no** | — | **no** |
| key `to_key_checked`, `encode_key_checked` (validated) | — | yes | — | yes |
| key `validate_key`, `from_key` | yes | yes | — | yes |
| key `from_key(psio::trusted{…})` / `psio::decode` (trusted) | yes (a key is parsed to be read) | **no** | — | **no** |
| json / ijson `to_json` (fast) | — | **no** (a string must be UTF-8: it is JSON text) | — | **no** |
| json / ijson `to_json_checked`, `encode_json_checked`, `validate_json`, `from_json` | yes | yes | — (the cap is not a limit on text) | yes |
| json / ijson `from_json(psio::trusted{…})` / `psio::decode` (trusted) | yes (a text is parsed to be read) | **no** | — | **no** |
| the other binary formats, `to_F` (fast) | — | **no** | **no** | **no** |
| the other binary formats, `to_F_checked`, `encode_F_checked`, `validate_F`, `from_F`, `validate_strict`, `psio::validated_view` | yes | yes | the root type's, on the size of the input | yes |
| the other binary formats, `from_F(psio::trusted{…})`, `F_trusted_view` | **no** | **no** | **no** | **no** |

**The trusted read.** It holds nothing against what a type declares, in
any format: a row a service wrote with the fast write is read back by the
trusted read as the value that was written, whether or not it is inside
its bounds (`psio::bounded` and `psio::utf8_string` are filled as written).
What a trusted read still does is find its way through the bytes: the pssz
decode keeps the span comparisons of its own walk (a value span inside the
buffer, an offset table inside its list, an element size that divides a
list's span, an optional selector that is 0 or 1, a record's cap) — which
is not a validation of what the type declares, but is enough that bytes
which are not a frame fail instead of being read outside
([below](#on-bytes-that-are-not-a-frame)) — and the key, json and protobuf
readers parse as they always do, because that is how they find their
values. Read your own tables trusted;
validate what comes from an action's caller or another service.

#### On bytes that are not a frame

A trusted read holds nothing against what a type *declares*. It is still
handed bytes that may not be a frame of the type at all: a row cut short, a
row an incompatible version of the type wrote. What every trusted read and
every view does then:

1. **No read outside the bytes, no undefined behaviour, bounded work.**
   Nothing is sized, and no loop is run, by a number the bytes do not back.
2. **A member that cannot be placed is a failure**, the one a decode gives:
   `psio::codec_abort` in C++ (a `psio::codec_exception` where exceptions
   are enabled; the abort handler and `std::abort` in a guest), a panic
   with a PSIO message in Rust (`from_F_trusted` returns `T`; that is its
   contract), a `PsszError` / `DecodeError` in TypeScript / Python. It is
   never an empty string or a zero in the member's place.
3. **Bytes that place every member read as some value of the type**, which
   is unspecified: another type's row whose offsets happen to fit, a
   corrupted number, text that is not UTF-8 in a C++ `std::string_view`, a
   `bool` byte other than 0 / 1 (true), members that overlap, a
   non-canonical form, trailing bytes. Only a validating read refuses
   those.

A view of nothing is not a member that could not be placed: a
default-constructed view, `list.at(i)` past the end and a trailing optional
the writer did not write read as zero / empty / absent.

Where that contract holds, and where a trusted read is still for validated
bytes only:

| | On bytes that are not a frame |
|---|---|
| C++ pssz: `psio::pssz_view<T>`, `psio::pssz_trusted_view<T>`, `buffer.view()`, `shared_pssz<T>` | 1, 2, 3. Fails with `view: the bytes are not a frame of the type: …`. A record's header is compared when the view is constructed, an offset when it is read |
| C++ pssz: `from_pssz<T>(psio::trusted{…})`, `psio::decode`, `view.unpack()`, every `pssz_<W>` profile | 1, 2, 3. Fails with the decoder's message (`pssz: record fixed region truncated`, `pssz: required record field missing`, …) |
| C++ ssz, wit (canonical ABI): `F_view<T>`, `F_trusted_view<T>` | 1, 2, 3, as the pssz views (the same frame) |
| C++ ssz, wit: `from_F<T>(psio::trusted{…})` | **validated bytes only**: the decoder compares nothing; on other bytes it reads outside them |
| C++ flatbuf: `psio::flatbuf_view<T>` (safe) | 1 and 3; what does not fit reads as *absent* (in FlatBuffers an absent field is its default) rather than failing |
| C++ flatbuf: `psio::flatbuf_trusted_view<T>`, `from_flatbuf<T>(psio::trusted{…})` | **validated bytes only** (one pointer, no bounds) |
| C++ capnp: `psio::capnp_view<T>`, `from_capnp<T>(psio::trusted{…})` | **validated bytes only** |
| C++ key, json / ijson, protobuf: `from_F<T>(psio::trusted{…})` | 1, 2, 3: they parse to find their values, as the validating read does |
| C++ msgpack: `from_msgpack<T>(psio::trusted{…})` | 1, 2, 3; the failure is a `std::runtime_error` of the parser, not a `psio::codec_exception` |
| C++ bin, borsh, bincode, avro, bson: `from_F<T>(psio::trusted{…})` | **validated bytes only**: on other bytes they read outside them |
| Rust `from_pssz_trusted`, `PsszView` | 1, 2, 3. `from_pssz_trusted: the bytes are not a frame of app::Row: the offset of a member needs bytes 8..12 of 10`; `try_from_pssz_trusted` returns the failure as an error. No `unsafe` on the read path |
| Rust `from_key_trusted`, `from_json_trusted`, `from_flatbuf_trusted`, `from_wit_trusted`, `from_ssz_trusted` | 1, 2, 3: a panic with the reader's own message |
| Rust `from_capnp_trusted` (and `from_capnp`) | safe (no `unsafe` on the path), but **open**: the typed read can panic with a slice message, or walk a count the message does not back |
| TypeScript `T.view(bytes)` (lazy), Python `psio.pssz_view`, `psio.from_pssz(T, psio.trusted(data))` | 1, 2, 3: `PsszError` of kind `bytes` / `DecodeError`, when the member is read |

**Bytes that validate are a frame.** A view of either kind and the trusted
decode never fail on bytes `validate_F<T>` accepted, whatever else those
bytes are (a newer writer's unknown trailing fields, pruned trailing
optionals), and read inside them. Validating an outer value validates a
packed member (`psio::pssz_buffer<T>`, `psio::shared_pssz<T>`, an optional
or a list of them, `psio::nested<T>`) as a `T`, so a message read with the
validating `from_pssz` may have its packed members walked through trusted
views with no further check. `psio::nested<T,
psio::nested_validation::deferred>` (Rust `Nested<T, Deferred>`, TypeScript
`nested(T, { deferred: true })`, Python `psio.nested(T, deferred=True)`) is
the exception: its bytes are not looked at until its holder validates them.
(`cpp/tests/accepted_frames_probe.cpp` walks every accepted frame of a
mutation corpus; the pssz validator accepts no frame that is not the
canonical bytes of its value apart from unknown trailing fields. The ssz
and canonical-ABI validators accept a `bool` byte other than 0 / 1, which
their views read as true.)

`cpp/tests/trusted_structure_tests.cpp` (also built with AddressSanitizer
and UndefinedBehaviorSanitizer), `rust/psio/tests/trusted_structure.rs`,
`js/src/test/view-structure.test.ts` and
`python/tests/test_view_structure.py` pin the rows marked 1, 2, 3 over every
truncation and single-byte corruption of valid frames and random edits.

```cpp
// A view checks nothing. Over bytes that were not validated it reads
// whatever is there: here a 33-byte owner under max<32>.
Account over{1, std::string(33, 'x'), {}, 0};
const auto bad = unchecked(over);
CHECK_FALSE(psio::validate_pssz<Account>(bad).ok());
const psio::pssz_view<Account> blind{bad};
CHECK(blind.get<1>().view_().size() == 33);

// validated_pssz_view builds the view only if the frame validates.
std::optional<psio::pssz_view<Account>> checked;
bool                                    has_unknown = false;
CHECK_FALSE(psio::validated_pssz_view(psio::pssz{}, std::span<const char>(bad), checked, has_unknown)
               .ok());
CHECK_FALSE(checked.has_value());

// The trusted decode skips invariants (and all structural checks).
std::vector<char> lower = psio::to_pssz(Symbol{"USD"});
lower.back()            = 'd';
CHECK(psio::from_pssz<Symbol>(psio::trusted{lower}).code == "USd");
CHECK_THROWS_AS(psio::from_pssz<Symbol>(lower), psio::codec_exception);
```

#### Which annotation is enforced by which format's validator

A validator is what stands between bytes nobody vouches for and the
program: `validate_F<T>(bytes)`, the checked decode `from_F<T>(bytes)` and
a validating view. The validated write of the format refuses the same
values. "yes" = a value that violates the declaration is refused; "—" =
the declaration does not apply to that format.

| Declaration | pssz | key | json / ijson | flatbuf | capnp | protobuf | bin, ssz, borsh, bincode, msgpack, avro, bson, wit ABI |
|---|---|---|---|---|---|---|---|
| `max<N>`, `min<N>`, `exact<N>` on a string, byte list, list, map, set | yes | yes | yes | yes | yes (no maps or sets in the mapping) | yes | yes (where the format's mapping has the type) |
| a bound below the member: `element`, `keys`, `values`, `at<I>` | yes | yes | yes | yes | yes | yes | yes |
| `utf8` | yes | yes | yes | yes | yes | yes (every protobuf `string`) | yes |
| `sorted`, `unique`, `unique_keys` | yes | yes | yes | yes | yes | yes | yes |
| `invariant(fn)` | yes | yes | yes | yes | yes | yes | yes |
| `maxDynamicData(N)` | yes, at every record | — | — | the root type's, on the size of the input | as flatbuf | as flatbuf | as flatbuf |
| `maxFields(N)` | at the declaration (compile time), and by the compatibility check: no run-time check in any format ([5.6](#56-maxfieldsn)) | | | | | | |

The optional `flatbuf_lib` adapter (the official library behind the same
helpers) has no validating read: its bytes are FlatBuffers, and
`psio::validate_flatbuf<T>` validates them.

**The other languages.** The same four modes, in each language's names:

| | fast write | validated write | validating read | trusted read |
|---|---|---|---|---|
| Rust | `to_F`, `to_F_into` | `try_to_F`, `try_to_F_into` | `from_F`, `validate_F` | `from_F_trusted` |
| TypeScript | `T.encode`, `T.encodeKey`, `T.valueToJson` | `T.encodeChecked`, `T.encodeKeyChecked`, `T.valueToJsonChecked` | `T.decode`, `T.validate`, `T.decodeKey`, `T.jsonToValue` | — (none; a view reads in place) |
| Python | `psio.to_pssz`, `to_key`, `to_json`, `to_ijson` | `psio.to_pssz_checked`, `to_key_checked`, `to_json_checked`, `to_ijson_checked` | `psio.from_F`, `validate_F` | `psio.from_pssz(T, psio.trusted(data))` |

**Every write entry point follows the one pattern.** An unsuffixed writer
is the fast write; its `_checked` (Rust: `try_`) twin is the validated one.
That holds for the helpers around a method as it does for a value:

| | fast write | validated write |
|---|---|---|
| C++, a source as a type | `psio::to_pssz_as<T>(source)`, `(source, out)` | `psio::to_pssz_as_checked<T>(source)`, `(source, out)` |
| C++, the arguments of a call | `psio::method_args_to_pssz<T, I>(args...)`, `method_args_append_pssz`, `method_args_packed` inside `to_pssz` | `psio::method_args_to_pssz_checked<T, I>(args...)`, `method_args_append_pssz_checked` |
| C++, the result of a call | `psio::method_result_to_pssz<T, I>(v)`, `method_result_append_pssz`, `encode_method_result` (a `codec_status`) | `psio::method_result_to_pssz_checked<T, I>(v)`, `method_result_append_pssz_checked`, `encode_method_result_checked` |
| C++, dispatch (the result it writes) | `psio::call_method_pssz<T, I>(obj, trusted{args}, out)` | `psio::call_method_pssz_checked<T, I>(obj, trusted{args}, out)` |
| C++, the format tags of a position / a result | `psio::pssz_at<A>::encode`, `psio::pssz_method_result<T, I>::encode` | their `encode_checked` |
| Rust | `M::result_to_pssz`, `dispatch`, `dispatch_name`; `pssz::encode`, `pssz::encode_into`, `json::encode`, `json::encode_into`; the generic `psio::encode::<F, _>` in every format | `M::try_result_to_pssz`, `dispatch_checked`; `pssz::try_encode`, `pssz::try_encode_into`, `json::try_encode`, `json::try_encode_into`; the generic `psio::try_encode::<F, _>` in every format |
| TypeScript | `m.resultToPssz`, `m.args.encode` | `m.resultToPsszChecked`, `m.args.encodeChecked` |
| Python | `m.to_pssz(*args)`, `m.result_to_pssz`, `interface.dispatch`, a `proxy` call | `m.to_pssz_checked(*args)`, `m.result_to_pssz_checked`, `interface.dispatch_checked` |

Dispatch always reads the ARGUMENTS validating — they are another
contract's data — and the caller reads the result validating; the fast
write of a result is the callee writing its own value.

The writers that validate under an unsuffixed name, and why each does:

| Writer | Why it validates |
|---|---|
| `psio::nested<T>{value}`, `nested<T>::from(value)`; Rust `Nested::new(&value)` | a `nested<T>` is a type whose meaning is "these bytes are a valid `T`": every reader skips validating them. `nested<T>{psio::trusted{bytes}}` / `Nested::unchecked` take bytes the caller vouches for |
| `psio::pssz_buffer<T>::from(value)`, `B{value}` for every `F_buffer` / `shared_F`, `psio::method_result_buffer<T, I>::from(value)`; Rust `Buffer::<T, F>::encode(&value)` | the same promise: a buffer is a validated encoding, and its `unpack()` / view is a trusted read. `B{psio::trusted{bytes}}` / `Buffer::trusted(bytes)` are the forms that skip it |
| JSON text to binary: C++ `json_to_pssz(schema, root, format, text, out)`; Rust `pssz_dynamic` `json_to_pssz`; TypeScript `T.jsonToPssz(text)`; Python `psio.json_to_pssz(T, text)` | the value came from text someone else wrote: the converter is a validating read followed by the write of what it read |
| the schema-driven writers of a dynamic value: C++ `pssz_ir.hpp` `encode` / `encode_auto` / `to_key` / `value_to_json`; Rust `pssz_dynamic::encode` | they return a status, never abort, and their input is a value built from external data by a tool that has no type to trust |

A validating call validates **whatever the caller is in the middle of**: a
`from_pssz` or a `to_pssz_checked` made from code that runs inside a fast
write or a trusted read (a hand-written text handler, an invariant, a
getter) is not switched off by it. In C++ the mode is an argument of the
call; in Rust, TypeScript and Python it is per-thread state that every
validating entry point sets for its own extent.

Rust has pssz, key, json, flatbuf, capnp, protobuf and wit
(`#[psio(max = N)]`, `element(...)`, `keys(...)`, `values(...)`, `sorted`,
`unique`, `unique_keys`, `#[psio(invariant = "path")]`; no `min` /
`exact`); `to_pssz` is infallible, so the one thing the fast pssz write
still refuses — a value whose offsets do not fit — is a panic there
(`to_pssz_into` returns it as an error). The view a `CapnpView` derive
makes (`FooView::from_message`) checks the structure of the message only —
validate with `psio::validate_capnp::<Foo>` first. TypeScript and Python
have pssz, key and json / ijson; a bound on a `nested` member is held by
all three codecs. Their key and JSON readers have no trusted mode (Python:
`psio.trusted` applies to pssz).

How the formats other than pssz, key and json check
(`psio/value_contract.hpp`, Rust `psio::contract`):

- **The structure first.** Bytes that are not valid for the format are
  refused for that, with the message and the position they were refused
  with before.
- **Then the declarations, on the value.** The bytes are decoded and the
  value is held against what its type declares: the members in declaration
  order; for each member what it holds first, then its own annotations; the
  invariant of a type last. The first violation is reported, at position 0:
  `flatbuf: member length_bound violated` (`capnp: …`, `bin: …`, …), the
  `utf8_spec` / `sorted` / `unique_keys` messages pssz gives, the
  invariant's own status. In protobuf a member may be written more than
  once, so only the finished value has a length: the last scalar, the
  merged list.
- **A type that declares nothing pays nothing** and accepts exactly what it
  accepted before (`psio::has_value_contract_v<T>` is false; the validator
  does not decode).
- **The validated write** of these formats writes fast and runs the
  validator on the bytes it wrote: the verdict and the message are the
  reader's by construction.
- C++ and Rust give the same verdict, message and position for flatbuf and
  protobuf: the rows `Bounded/…` of `conformance/flatbuf-vectors.json` and
  `PbBounded/…` of `conformance/protobuf-vectors.json`.

```cpp
// The declaration is the type in FlatBuffers, Cap'n Proto and protobuf
// too. A writer that does not know it (here: AccountPlain, the same
// members without annotations) writes a 33-byte owner as valid
// FlatBuffers; read as an Account, the buffer is refused.
const AccountPlain      wide{1, std::string(33, 'x'), {}, 0};
const std::vector<char> fb = psio::to_flatbuf(wide);
CHECK(psio::validate_flatbuf<AccountPlain>(fb).ok());
const psio::codec_status refused = psio::validate_flatbuf<Account>(fb);
REQUIRE_FALSE(refused.ok());
CHECK(refused.error().what == std::string_view{"flatbuf: member length_bound violated"});
CHECK_THROWS_AS(psio::from_flatbuf<Account>(fb), psio::codec_exception);
CHECK_FALSE(psio::validate_capnp<Account>(psio::to_capnp(wide)).ok());
CHECK_FALSE(psio::validate_protobuf<Account>(psio::to_protobuf(wide)).ok());

// Inside the bounds, the same bytes are an Account.
const AccountPlain      fits{1, "alice", {"vip"}, 100};
CHECK(psio::from_flatbuf<Account>(psio::to_flatbuf(fits)) == Account{1, "alice", {"vip"}, 100});
```

### 7.3 The trust rule

- **Untrusted bytes are validated before use**: action arguments,
  anything from another service or the network, anything a user supplied.
  Use `from_pssz<T>(bytes)`, or `validate_pssz` followed by a view or a
  trusted decode.
- **Your own stored data may be read trusted**: bytes this service wrote
  with `to_pssz` and kept. `psio::trusted{bytes}` is an explicit statement;
  a trusted decode of bytes that are not valid is undefined behaviour for
  the binary formats.
- A view is a trusted read. Construct views over validated or own bytes
  only.

### 7.4 Const safety

A read view cannot write, and nothing turns one into a view that writes:

```cpp
template <typename V>
concept can_set = requires(V v) { v.template get<0>().set(std::uint64_t{1}); };
static_assert(!can_set<psio::pssz_view<ref::Point>>);
static_assert(std::is_same_v<psio::pssz_view<const ref::Point>, psio::pssz_view<ref::Point>>);
```

`psio::pssz_view<const T>` is the same type as `psio::pssz_view<T>`.
Writing is always spelled `psio::pssz_mutable_view<T>`; it is made from
mutable bytes only and has one operation, overwriting a fixed-size value:

```cpp
std::vector<char> bytes = psio::to_pssz(ref::Point{1, 2});
psio::pssz_mutable_view<ref::Point> w{bytes};
CHECK(w.get<0>().set(std::int32_t{42}));   // false if the value cannot be written
CHECK(psio::from_pssz<ref::Point>(bytes) == ref::Point{42, 2});
```

Strings, lists, optionals, variants, extensible records and members whose
annotations constrain their content cannot be written in place (compile
errors, [pssz-spec.md §17](pssz-spec.md#17-in-place-mutation)).

## 8. Evolution: what may change after publication

A type is published once bytes of it exist outside the process that wrote
them: in a database, in a block, in another service.

### 8.1 The compatible change

**Append `std::optional` members at the end, without changing the type's
width.** That is the only change of *layout*. The annotations of existing
members may change in one way only: a bound may widen
([8.3](#83-annotations-bounds-may-only-widen)).

```cpp
struct AccountV2
{
   friend bool operator==(const AccountV2&, const AccountV2&) = default;
   std::uint64_t              id = 0;
   std::string                owner;
   std::vector<std::string>   tags;
   std::uint64_t              balance = 0;
   std::optional<std::string> email;   // new: optional, last
   PSIO_REFLECT(AccountV2,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                attr(email, utf8 | max<64>),
                maxDynamicData(1024))
};
static_assert(psio::auto_pssz_width_v<AccountV2> == 2);
static_assert(psio::pssz_evolves_from<AccountV1, AccountV2>);
```

```cpp
const AccountV1 v1{7, "alice", {"vip"}, 100};
const AccountV2 v2_none{7, "alice", {"vip"}, 100, std::nullopt};
const AccountV2 v2_some{7, "alice", {"vip"}, 100, "alice@example.com"};

// A new writer that leaves the field out writes the old bytes: trailing
// absent optionals are pruned.
CHECK(psio::to_pssz(v2_none) == psio::to_pssz(v1));

// Old bytes read as the new type: the field is absent.
CHECK(psio::from_pssz<AccountV2>(psio::to_pssz(v1)) == v2_none);

// New bytes read as the old type: the unknown field is skipped ...
CHECK(psio::from_pssz<AccountV1>(psio::to_pssz(v2_some)) == v1);
// ... and a reader that wants to know is told.
AccountV1 out;
bool      has_unknown = false;
CHECK(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>(psio::to_pssz(v2_some)), out,
                                has_unknown)
         .ok());
CHECK(has_unknown);
// The exact-schema check refuses unknown fields.
CHECK_FALSE(psio::validate_strict<AccountV1>(psio::pssz{},
                                             std::span<const char>(psio::to_pssz(v2_some)))
               .ok());
```

- New fields are `std::optional<T>` and go last.
- A trailing absent optional is pruned: a new writer that leaves the field
  out writes exactly the old bytes. Pruning is mandatory — a stored
  trailing `None` is rejected.
- An old reader skips fields it does not know (`decode_pssz_checked`
  reports them; `validate_strict` rejects them).
- **You cannot add an optional field if it would change the implicit
  width.** A type without an explicit cap has only the slack between its
  inferred bound and 255 / 65535. Declare `maxDynamicData` when the type is
  first published if it may grow ([3.4](#34-how-to-pick-a-cap)).

  > **The case that surprises: a type whose members are all fixed-size.**
  > `(int32)` — a record `{ std::int32_t socket; }`, a `std::tuple<std::int32_t>`,
  > or the arguments of `void close(std::int32_t socket)` — has an inferred
  > bound of 5 bytes, so its header is **1 byte**. `(int32,
  > optional<vector<u64>>)` is unbounded, so its header and slot are **4
  > bytes**. The second is *not* a revision of the first: the new reader
  > rejects every old value (`pssz: record fixed region truncated`), the old
  > reader rejects every new one (`pssz: fixed record has trailing payload`),
  > and both checks refuse the pair — `psio::pssz_evolves_from<Old, New>` is
  > false, `psio::require_pssz_evolves_from<Old, New>()` says "the automatic
  > offset width of the type changes", `psio::pssz_ir::evolves_from` answers
  > `width-changed` at `(root)`. A record with a `std::string` member does not
  > show this (it is 4 bytes wide from the start), which is why the rule
  > looks like "fixed members cannot evolve"; the rule is about the width.
  >
  > Adding an unbounded optional to an uncapped bounded type is never a
  > compatible change. A type that expects to grow declares
  > `maxDynamicData(N)` in its **first** published version — on the record,
  > or on the `method(...)` for an argument list — with N in the width class
  > it will keep; or every member it will ever gain is bounded so that the
  > inferred bound stays in the class (`optional<uint8>` after `(int32)` is
  > fine: 7 bytes). The cap cannot be added afterwards when it changes the
  > width.
- The same holds one level down: a field appended to a record that is an
  element of a bounded list raises that list's bound and can move the
  list's own width. A cap on the element record fixes what the list
  reserves for it.

### 8.2 What is not compatible, and how the tooling catches it

`psio::pssz_evolves_from<Old, New>` is a compile-time check; keep a copy of
each published revision and assert it. It is true iff the new reader reads
every value the old writer can produce, and the old reader reads every new
value that is also a value of the old type
([pssz-optional-evolution.md](pssz-optional-evolution.md)).

```cpp
struct AccountGrown
{
   std::uint64_t              id = 0;
   std::string                owner;
   std::vector<std::string>   tags;
   std::uint64_t              balance = 0;
   std::optional<std::string> bio;
   PSIO_REFLECT(AccountGrown,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                attr(bio, utf8 | max<100>))
};
// (header + 3 slots) x 2 bytes + id 8 + owner 32 + tags 136 + balance 8 + bio (selector 1 + 100)
static_assert(psio::inferred_bound<AccountGrown>() == 2 * 4 + 8 + 32 + 136 + 8 + (1 + 100));
static_assert(psio::auto_pssz_width_v<AccountGrown> == 2);
static_assert(!psio::pssz_evolves_from<Account, AccountGrown>);
```

(`tut-4b`, case 1: no cap, and the new field needs more than the 68 bytes
of slack `Account` has, so it moves the width. A smaller field would have
fit — which is the trap: the next one would not.) The same
section of the examples file asserts four more, each
`!psio::pssz_evolves_from<AccountV1, …>`: a new field that is not optional
(`AccountRequired`), an optional field that is not last
(`AccountReordered`), an existing field whose type changed
(`AccountRetyped`), and a cap moved across a width class, 1024 → 100000
(`AccountRecapped`). They are shown in full in
[tutorial step 4](tutorial-annotations.md#step-4--add-a-field-compatibly).

Also incompatible, and rejected by the check: adding or removing
`definitionWillNotChange()`; adding a field to a DWNC record; adding or
removing `transparent()`; removing, reordering, inserting or retyping a
variant alternative (appending one is compatible,
[6.10](#610-variant)); changing an array length; replacing an enum by
another enum type or changing a time unit.

### 8.3 Annotations: bounds may only widen

The check compares the annotations that are part of the type, at the member
and at every position below it (`element`, `keys`, `values`, `at<I>`). The
rule, as [pssz-optional-evolution.md](pssz-optional-evolution.md) states it:

1. **Fields.** `New`'s fields are `Old`'s, in order, followed only by
   optional fields; DWNC status is unchanged and a DWNC record gains none.
   **Alternatives.** `New`'s variant alternatives are `Old`'s, in order,
   followed by any number of new ones.
2. **Widths.** The record's offset width is unchanged, and so is the width
   of every list, map, set, array, tuple and pair at and below every member.
3. **Length bounds may only widen.** `New`'s range contains `Old`'s —
   `[min, max]`, where `exact<N>` is `[N, N]` and no bound is `[0, ∞)`.
   Tightening keeps the bytes of old values but makes the new reader reject
   the old records outside the new bound.
4. **`utf8`** is the same on both sides at every position.
5. **`sorted` / `unique` / `unique_keys`** are the same on both sides.
6. **`maxDynamicData`.** `New`'s cap, if it has one, is at least `Old`'s cap
   — or, when `Old` has none, at least `Old`'s inferred bound. Dropping a
   cap is allowed when rule 2 still holds.
7. **`maxFields`.** If `Old` has one, `New` has no more fields than it
   allows and declares a `maxFields` that is not larger.
8. **A bound stated in a wrapper type** (`psio::bounded<T, N>`,
   `psio::utf8_string<N>`) is part of the C++ type: the two revisions must
   use the same type there. To widen such a member, first move the bound to
   an annotation (same bytes, same schema), then widen it.

```cpp
// The check compares the annotations too. A wider bound is compatible in one
// direction: the new reader reads every record the old writer produced.
struct AccountWider
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountWider,
                id,
                attr(owner, utf8 | max<64>),   // was max<32>
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                maxDynamicData(1024))
};
static_assert(psio::pssz_evolves_from<AccountV1, AccountWider>);
static_assert(!psio::pssz_evolves_from<AccountWider, AccountV1>);    // never back

// A tighter bound is refused: the new reader would reject stored records.
struct AccountTighter
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountTighter,
                id,
                attr(owner, utf8 | max<16>),   // was max<32>
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                maxDynamicData(1024))
};
static_assert(!psio::pssz_evolves_from<AccountV1, AccountTighter>);

// So is a wider bound that moves an offset width: 8 tags of <= 40 bytes are
// 8 + 320 > 255, and the list's table would go from 1 byte to 2.
struct AccountWideTags
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountWideTags,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<40>)),   // was max<16>
                balance,
                maxDynamicData(1024))
};
static_assert(!psio::pssz_evolves_from<AccountV1, AccountWideTags>);

// utf8 or raw is what the member is: it cannot be added or dropped.
struct AccountRawOwner
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountRawOwner,
                id,
                attr(owner, max<32>),          // was utf8 | max<32>
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                maxDynamicData(1024))
};
static_assert(!psio::pssz_evolves_from<AccountV1, AccountRawOwner>);
static_assert(!psio::pssz_evolves_from<AccountRawOwner, AccountV1>);

// The cap may grow inside its width class, and may not shrink.
struct AccountCapRaised
{
   std::uint64_t            id = 0;
   std::string              owner;
   std::vector<std::string> tags;
   std::uint64_t            balance = 0;
   PSIO_REFLECT(AccountCapRaised,
                id,
                attr(owner, utf8 | max<32>),
                attr(tags, max<8> | element(utf8 | max<16>)),
                balance,
                maxDynamicData(4096))          // was 1024
};
static_assert(psio::pssz_evolves_from<AccountV1, AccountCapRaised>);
static_assert(!psio::pssz_evolves_from<AccountCapRaised, AccountV1>);
```

```cpp
// What the check promises, on bytes: an old record at its bound is a record
// of the widened type, and is not one of the tightened type.
const AccountV1 wide_owner{7, std::string(32, 'o'), {}, 0};
CHECK(psio::validate_pssz<AccountWider>(psio::to_pssz(wide_owner)).ok());
CHECK_FALSE(psio::validate_pssz<AccountTighter>(psio::to_pssz(wide_owner)).ok());
// One direction only: a new writer can produce what an old reader refuses.
const AccountWider wider{7, std::string(33, 'o'), {}, 0};
CHECK_FALSE(psio::validate_pssz<AccountV1>(psio::to_pssz(wider)).ok());
```

The relation is transitive and deliberately **one-directional** for bounds
and caps: `pssz_evolves_from<Old, New>` does not imply
`pssz_evolves_from<New, Old>`. A widened bound means a new writer can
produce values an old reader rejects, so roll the new readers out first.
(Rust `pssz_evolves_from`, TypeScript `psszEvolvesFrom` and Python
`psio.evolves_from` apply the same rule.)

**What the check still does not see** — review by hand, and treat as frozen
once published:

- **enumerators** of one enum type (only adding one is compatible,
  [6.3](#63-enums));
- **invariants** (tightening one rejects stored data);
- JSON forms and names (text clients);
- annotations that are not on the wire and not in the list above.

### 8.4 Summary

| Change | Compatible? |
|---|---|
| append `std::optional` field, width unchanged | yes |
| append `std::optional` field, inferred width changes | **no** — reserve with `maxDynamicData` |
| append a field beyond the type's `maxFields`; raise or drop `maxFields` | no |
| append a required field; insert, remove, reorder or retype a field | no |
| widen a member bound (larger `max`, smaller `min`, bound removed), no width moves | yes, one way: new readers read old data; old readers reject the new larger values |
| widen a bound so that a list's, map's or tuple's offset width moves | no |
| tighten a member bound; add a `min` or `exact` | no |
| add or remove `utf8`, `sorted`, `unique`, `unique_keys` | no |
| raise `maxDynamicData` within its width class | yes, one way (old readers reject larger values) |
| lower `maxDynamicData`; move it across 255 / 65535; add one below the old inferred bound | no |
| add a parameter or element that takes a tuple past 255 elements, a field that takes a record past 255 fields | no: not a type |
| add an enumerator at a new value | yes in binary; text readers reject an unknown name (not seen by the check) |
| change an enum's underlying type; renumber | no |
| append a variant alternative, no width moves | yes, one way: new readers read old data; old readers reject a value holding the new alternative |
| append a variant alternative that is larger, so that the width of the record / tuple / array / bounded list holding the variant moves | **no** — reserve with `maxDynamicData` |
| remove, reorder, insert or retype a variant alternative | no |
| append an optional method parameter under the method's cap | yes |
| change `json_string` / `ijson_number` | binary yes; breaks text clients |
| rename a field | binary and key yes; breaks text and schema clients |

## 9. Common mistakes and their compiler errors

| Mistake | What happens |
|---|---|
| Changing a member's type to express a bound | Works, but forces the wrapper on all code. Annotate instead |
| Bounding a list's count but not its variable-size elements | Compiles; without a cap the type stays 32-bit. `static_assert` the width |
| A member bound larger than the record's cap (`max<1000>` under `maxDynamicData(255)`) | Compiles, no diagnostic: the bound still limits the count, the cap selects the width ([3.3](#33-a-cap-narrows-what-lies-beneath-it)) |
| An unbounded child record inside a parent with a small cap | Compiles; the child keeps 4-byte offsets. `static_assert(!psio::pssz_child_wider_than_cap_v<Parent>)` |
| A `std::tuple` or parameter list of more than 255 elements (a record: the same limit, its own message) | `psio: a tuple has at most 255 elements (docs/pssz-spec.md 8.4); this one has more. Use a record, a list, or nested tuples` |
| Expecting `attr(f, utf8_spec{})` to be a weaker `utf8` | It is `utf8`: enforced by every validated write and validating reader |
| `max<8> \| max<16>`, `min<9> \| max<8>` on one member | `psio: contradictory length bounds on one member or position: …` (`min<A> \| max<B>` with A ≤ B is fine: one range) |
| More fields than `maxFields(N)`; `maxFields(300)`; a record of 256 fields | `psio: the record has more fields than its maxFields(N) allows` and the other texts of [5.6](#56-maxfieldsn) |
| A bound on a non-length member | `psio: a spec in the effective annotations is attached to a field whose shape is not in the spec's applies_to set. …` |
| A position spec on the wrong shape | the messages of [5.3](#53-element-keys-values-ati--positions-below-the-member) |
| The same spec in `PSIO_REFLECT` and in `PSIO_FIELD_ATTRS` | `psio: an annotation is declared twice — in PSIO_REFLECT (attr / definitionWillNotChange / maxFields / maxDynamicData / transparent) and again with PSIO_FIELD_ATTRS / PSIO_TYPE_ATTRS / PSIO_ATTRS` |
| `PSIO_FIELD_ATTRS` after the type was already used | `psio: PSIO_FIELD_ATTRS(T, f) comes after the first use of the field's annotations` |
| `exact<N>`, `min<N>`, `sorted`, `json_string` or `as<Tag>` on a transparent type's member | `psio: the member of a transparent() type takes only the annotations that make it a bounded type: …` ([5.7](#57-transparent)); `utf8`, `max<N>` and position specs are accepted |
| Tightening a published bound, dropping `utf8`, lowering a cap | compiles; `pssz_evolves_from` is false ([8.3](#83-annotations-bounds-may-only-widen)) |
| An unsorted value in a member declared `sorted` | the validated writes (`to_F_checked`) throw, the readers reject; the fast write (`to_F`) writes it ([5.13](#513-sorted-unique-unique_keys), [7.2](#72-reading-and-writing-the-two-modes)) |
| Expecting `to_pssz` / `to_key` / `to_json` to refuse a value outside a member bound | They are the fast write. `to_F_checked` throws `codec_exception`; `psio::encode_F_checked` returns a status ([7.2](#72-reading-and-writing-the-two-modes)) |
| An invariant that is not `noexcept` | `psio: invariant(fn) in PSIO_REFLECT(T) needs fn callable as fn(const T&), declared noexcept and returning psio::codec_status` |
| Declaring an invariant that accepts everything to give a record a schema id | the record loses its mutable view (`psio::mutable_view: in-place mutation is blocked for this type`); write `schemaId("Id")` ([5.15](#515-schemaidid)) |
| `schemaId("string")`, `schemaId("hex")`, … | `psio: schemaId(...) in PSIO_REFLECT(T) names an id reserved for one of PSIO's own types …` |
| `PSIO_REFLECT` in a function-local struct | does not compile (`templates cannot be declared inside of a local class`) |
| A field and a method with one name; a member named `psio_get_proxy` | compile error (duplicate / reserved name) |
| Adding a field to a published type without a cap | compiles; `pssz_evolves_from` is false when the width moves; otherwise you used up slack |
| Reading untrusted bytes through a view or `psio::trusted` | compiles; nothing is checked |
| Expecting `hex_spec`, `padding`, `skip_spec`, `default_value_spec`, `since_spec`, `final_v`, `canonical`, `flags` to do something | RESERVED: nothing reads them ([5.14](#514-reserved-vocabulary)) |
| Expecting `maxDynamicData` to limit JSON input | it limits the pssz encoding only ([5.4](#54-maxdynamicdatan)); bound the members |
| Expecting a schema-driven reader to enforce `sorted` / `unique` | the order specs are not in the Schema IR |

## Rust, TypeScript and Python snippets

**Not compile-checked from this document.** The C++ snippets above are
verified to be lines of a test; these are not. Each language has a test
that declares the same types and asserts the same bytes as the C++
`Account` of [tutorial step 2](tutorial-annotations.md#step-2--add-bounds-until-the-width-drops)
— `rust/psio/tests/doc_annotations.rs`,
`js/src/test/doc-annotations.test.ts`, and the `python` blocks of
[`python/README.md`](../python/README.md), run by
`python/tests/test_readme.py` — but nothing ties the text below to those
files.

The frame all four languages write for
`Account{7, "alice", {"vip", "beta"}, 100}` (33 bytes; inferred bound 187,
width 1):

```text
12 07 00 00 00 00 00 00 00 12 17 64 00 00 00 00 00 00 00 61 6c 69 63 65 02 05 76 69 70 62 65 74 61
```

```rust
use psio::pssz::{auto_pssz_width, pssz_evolves_from, RawString};
use psio::{JsonText, PsszPack, PsszUnpack, ToPsszSchema};

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema)]
#[pssz(max_dynamic_data = 1024)]
struct AccountV2 {
    id: u64,
    #[psio(max = 32)]
    owner: String,                       // String is UTF-8: C++ `utf8 | max<32>`
    #[psio(max = 8, element(max = 16))]
    tags: Vec<String>,
    balance: u64,
    #[psio(max = 64)]
    email: Option<String>,               // new: optional, last
}

const _: () = {
    assert!(auto_pssz_width::<AccountV2>() == 2);
    assert!(pssz_evolves_from::<AccountV1, AccountV2>());   // AccountV1: the same without `email`
};

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText)]
struct Note {
    #[psio(max = 64)]
    blob: RawString,                     // raw bytes: C++ std::string
    #[psio(max = 64)]
    text: String,                        // UTF-8: C++ `utf8`
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText)]
struct Totals {
    #[json(json_string, ijson_string)]
    supply: u64,
    #[json(ijson_number)]
    holders: u64,
    decimals: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack, JsonText, psio::Key)]
#[psio(transparent)]
struct AccountId(u64);

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, psio::Key)]
#[psio(transparent)]
struct Memo(#[psio(max = 80)] String);   // C++ transparent() + attr(contents, utf8 | max<80>)

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText)]
#[psio(invariant = "upper_case")]       // fn upper_case(s: &Symbol) -> Result<(), &'static str>
struct Symbol {
    #[psio(max = 8)]
    code: RawString,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText)]
#[psio(definition_will_not_change)]
struct Ticket { id: u32, status: Status }

#[psio::interface]
pub trait Bank {
    fn open(&mut self, #[psio(max = 32)] owner: String, #[psio(max = 8, element(max = 16))] tags: Vec<String>);
    #[pssz(max_dynamic_data = 1024)]
    fn transfer(&mut self, from: u64, to: u64, amount: u64, #[psio(max = 80)] memo: String);
    #[psio(returns(max = 8))]
    fn symbol(&self, token: u32) -> String;
}
// auto_pssz_width::<bank::args::transfer>() == 2
// inferred_bound::<bank::args::transfer>() == Some(106), max_encoded_size::<bank::args::transfer>() == Some(108)
```

Rust entry points: `psio::to_pssz(&v)` (panics on a value with no
encoding) / `psio::try_to_pssz(&v)`; `psio::from_pssz::<T>(&bytes)`
(validated, returns `Result`); `psio::to_json`, `psio::to_ijson`,
`psio::try_to_json`; `psio::to_key`, `psio::try_to_key`, `psio::from_key`.
`psio::pssz::auto_pssz_width::<T>()`, `inferred_bound::<T>()`,
`max_encoded_size::<T>()` and `pssz_evolves_from::<Old, New>()` are
`const fn`. Not available in Rust: `exact` / `min`, field numbers, a `utf8`
key (the type decides).

```typescript
import { argumentTuple, boundedUtf8, custom, field, list, optional, rawString, record, transparent, u32, u64, type Infer } from 'psio';

const accountFields = {
  id: u64,
  owner: field(rawString, { utf8: true, max: 32 }),                             // C++ utf8 | max<32>
  tags: field(list(rawString), { max: 8, element: { utf8: true, max: 16 } }),   // C++ max<8> | element(utf8 | max<16>)
  balance: u64,
};
const Account   = record('Account', accountFields);                              // Account.width === 1
const AccountV1 = record('AccountV1', accountFields, { maxDynamicData: 1024 });  // AccountV1.width === 2
// AccountV1.inferredBound === 187n, AccountV1.maxEncodedSize === 190n
const AccountV2 = record('AccountV2', {
  ...accountFields,
  email: field(optional(rawString), { utf8: true, max: 64 }),                   // new: optional, last
}, { maxDynamicData: 1024 });
type Account = Infer<typeof Account>;   // { id: bigint; owner: string; tags: string[]; balance: bigint }

const Totals = record('Totals', {
  supply: field(u64, { jsonInt: 'string', ijsonInt: 'string' }),
  holders: field(u64, { ijsonInt: 'number' }),
  decimals: u32,
});

const AccountId = transparent('AccountId', u64);
const Memo      = transparent('Memo', boundedUtf8(80));                          // C++ transparent() + attr(contents, utf8 | max<80>)
const Point     = record('Point', { x: u32, y: u32 }, { fixed: true });          // definitionWillNotChange
const Symbol    = custom('Symbol', record('Symbol', { code: field(rawString, { max: 8 }) }), {
  check: (s) => (/^[A-Z]*$/.test(new TextDecoder().decode(s.code)) ? undefined : 'symbol is not upper case'),
});

const Transfer = argumentTuple(
  { from: u64, to: u64, amount: u64, memo: field(rawString, { utf8: true, max: 80 }) },
  { maxDynamicData: 1024 });                                                    // Transfer.width === 2
```

TypeScript entry points: `T.encode(value)`, `T.decode(bytes, { rejectUnknown })`
(always validated), `T.validate(bytes)` → `{ ok, hasUnknown }`,
`T.view(bytes)`, `T.encodeKey`, `T.decodeKey`, `T.psszToJson(bytes, 'json' | 'ijson')`,
`T.valueToJson`, `T.jsonToValue`, `T.width`, `T.inferredBound`,
`T.maxEncodedSize`. The compatibility checks are `psszEvolvesFrom(Old, New, { compare })`
and `psszEquivalent(A, B, { compare })`; interfaces are `defineInterface(name, { m: method(...) })`
(argument and result codecs, the Schema IR); the name coders are `nameToNumber` /
`numberToName` and `methodToNumber` / `numberToMethod`.
Not available in TypeScript: a trusted decode, field numbers
([4](#4-master-table), [4.1](#41-tooling-per-language)). 64-bit and wider integers are `bigint`.

```python
from dataclasses import dataclass
from typing import Annotated, NewType, Optional, Protocol
import psio
from psio import RawString, u32, u64

@dataclass
class Account:
    id: u64
    owner: Annotated[str, psio.max(32)]                                    # C++ utf8 | max<32>
    tags: Annotated[list[str], psio.max(8), psio.element(psio.max(16))]    # C++ max<8> | element(utf8 | max<16>)
    balance: u64
# psio.codec(Account).width == 1, .inferred_bound == 187, .max_encoded_size == 187

@psio.record(max_dynamic_data=1024)
@dataclass
class AccountV2:
    id: u64
    owner: Annotated[str, psio.max(32)]
    tags: Annotated[list[str], psio.max(8), psio.element(psio.max(16))]
    balance: u64
    email: Annotated[Optional[str], psio.max(64)] = None                   # new: optional, last
# psio.codec(AccountV2).width == 2

@dataclass
class Note:
    blob: Annotated[RawString, psio.max(64)]      # raw bytes: C++ std::string
    text: Annotated[str, psio.max(64)]            # UTF-8: C++ utf8

@dataclass
class Totals:
    supply: Annotated[u64, psio.json_string, psio.ijson_string]
    holders: Annotated[u64, psio.ijson_number]
    decimals: u32

AccountId = NewType("AccountId", u64)             # transparent

@dataclass
class Symbol:
    code: Annotated[str, psio.max(8)]

    @psio.invariant
    def _upper_case(self) -> Optional[str]:
        return None if self.code.isascii() and self.code.isupper() else "symbol is not upper case"

@psio.record(definition_will_not_change=True)
@dataclass
class Point:
    x: u32
    y: u32

@psio.interface(name="bank")
class Bank(Protocol):
    def open(self, owner: Annotated[str, psio.max(32)],
             tags: Annotated[list[str], psio.max(8), psio.element(psio.max(16))]) -> None: ...

    @psio.method(max_dynamic_data=1024)
    def transfer(self, from_: AccountId, to: AccountId, amount: u64,
                 memo: Annotated[str, psio.max(80)]) -> None: ...

    def symbol(self, token: u32) -> Annotated[str, psio.max(8)]: ...
# psio.interface_of(Bank)["transfer"].args.width == 2
```

Python entry points: `psio.to_pssz(value[, T])`, `psio.from_pssz(T, data)`
(validates, then decodes), `psio.from_pssz(T, psio.trusted(data))`,
`psio.validate_pssz(T, data)` → a `Status` (`ok`, `reason`,
`has_unknown`), `psio.pssz_view(T, data)`, `psio.to_key`, `psio.from_key`,
`psio.to_json`, `psio.to_ijson`, `psio.schema_of(T)`, `psio.codec(T)`
(`width`, `inferred_bound`, `max_encoded_size`, `max_dynamic_data`). Every failure is a
`psio.PsioError`. The compatibility check is `psio.evolves_from(Old, New)`.
Not available in Python: a mutable view, WIT / GraphQL emitters, protobuf
field numbers.
