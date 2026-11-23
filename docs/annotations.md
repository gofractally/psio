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
static_assert(psio::max_encoded_size<Account>() == 3 + 8 + 32 + (8 + 8 * 16) + 8);
static_assert(psio::max_encoded_size<Account>() == 187);        // <= 255: 8-bit offsets
static_assert(psio::auto_pssz_width_v<Account> == 1);
```

(`tut-2b`.) The member types are the ones the program wants — `std::string`,
`std::vector<std::string>`. The annotations say what the values may be, and
every format follows from that one statement:

| Format | What the annotations above do |
|---|---|
| **pssz** (`psio::to_pssz`, `from_pssz`, `validate_pssz`) | The record's offsets are 8-bit instead of 32-bit (the bounds add up to 187 bytes), and so is the offset table of `tags`. A value outside a bound has no encoding; bytes that hold one are rejected. |
| **key** (`psio::to_key`, `from_key`, `validate_key`) | No change to the bytes. The reader rejects a key outside a bound or with a `utf8` member that is not UTF-8; the C++ writer does not check ([limits](#72-what-is-checked-where)). |
| **json / ijson** (`psio::to_json`, `to_ijson`, …) | No change to the text. Writer and reader refuse a value outside a bound. |
| **Schema IR** (`psio::SchemaBuilder`) | `owner` is `Custom{BoundedList<u8, 32>, "utf8"}`, `tags` is `BoundedList<Custom{BoundedList<u8, 16>, "utf8"}, 8>`: a reader that has only the schema derives the same width and enforces the same bounds. |
| **WIT** (`psio::emit_wit`) | A bounded list is `list<T>/* @psio:max=N */`; a record cap is `@maxDynamicData(N)`. A string's bound is not shown. |
| **GraphQL** (`psio/graphql.hpp`) | Bounds are not part of the GraphQL schema ([graphql.md](graphql.md)). |
| bin, ssz, borsh, bincode, msgpack, avro, bson, protobuf, capnp, flatbuf, wit ABI | Member bounds and `utf8` are not read. `validate` applies a root type's `maxDynamicData` to the size of its own input ([5.4](#54-maxdynamicdatan)); protobuf reads `field<N>`. |

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
| `method(name, item…)` | a member function ([5.11](#511-methodname-)) |
| `definitionWillNotChange()` | fixed layout ([5.5](#55-definitionwillnotchange)) |
| `maxDynamicData(N)` | size cap and width budget ([5.4](#54-maxdynamicdatan)) |
| `maxFields(N)` | field-count cap ([5.6](#56-maxfieldsn)) |
| `transparent()` | the type is its one member ([5.7](#57-transparent)) |
| `invariant(fn)` | a value check ([5.8](#58-invariantfn)) |

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
2. **Otherwise the inferred bound**, `psio::max_encoded_size<T>()`: the
   largest encoding the bounds allow, rounded up to the next width. If any
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
static_assert(psio::max_encoded_size<Account>() == 3 + 8 + 32 + (8 + 8 * 16) + 8);
static_assert(psio::max_encoded_size<Account>() == 187);        // <= 255: 8-bit offsets
static_assert(psio::auto_pssz_width_v<Account> == 1);
```

### 3.1 How the inferred bound is counted

([pssz-spec.md §3.2](pssz-spec.md#32-max_encoded_sizet).) A type that has
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
| unbounded string / list / map / set / nested; a boxed value; a member with an `as<Tag>` override | no bound → the enclosing type is 4-byte |

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
static_assert(psio::max_encoded_size<Raw253>() == 255 && psio::auto_pssz_width_v<Raw253> == 1);
static_assert(psio::max_encoded_size<Raw254>() == 258 && psio::auto_pssz_width_v<Raw254> == 2);
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
static_assert(psio::max_encoded_size<Raw65531>() == 65535 && psio::auto_pssz_width_v<Raw65531> == 2);
static_assert(psio::max_encoded_size<Raw65532>() == 65540 && psio::auto_pssz_width_v<Raw65532> == 4);
// A list measures itself the same way: one slot per element, then the elements.
using Tags15x16 = psio::bounded<psio::utf8_string<16>, 15>;   // 15 + 15 x 16 = 255
using Tags15x17 = psio::bounded<psio::utf8_string<17>, 15>;   // 15 + 255 > 255: 2 x 15 + 255
static_assert(psio::max_encoded_size<Tags15x16>() == 255 && psio::auto_pssz_width_v<Tags15x16> == 1);
static_assert(psio::max_encoded_size<Tags15x17>() == 285 && psio::auto_pssz_width_v<Tags15x17> == 2);
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
   and `psio::max_dynamic_data_v<T>` or `psio::max_encoded_size<T>()`.
7. **The same N in every language** that declares the type.

Do not use a small cap on a type that is read or written as a JSON *root*
in C++ ([5.4](#54-maxdynamicdatan), "the cap and text").

Practical guidance for types without a cap:

- A type with any unbounded member is 32-bit. Bounding *some* members buys
  nothing (`tut-2c`: `AccountCountOnly`).
- If the type may ever gain a field, give it an explicit cap now
  ([8](#8-evolution-what-may-change-after-publication)).

## 4. Master table

"n/a" = the language states this in the type; "—" = not available
(not yet, or not applicable). The Python column uses the spellings of
`python/src/psio` ([README](../python/README.md)). Schema IR nodes are written as in
[json.md](json.md#the-schema-ir-as-json).

| Annotation | C++ (`PSIO_REFLECT`) | Rust | TypeScript | Python | Schema IR | Honoured by |
|---|---|---|---|---|---|---|
| length ≤ N | `attr(f, max<N>)`; `psio::bounded<T, N>`, `psio::utf8_string<N>` | `#[psio(max = N)]`; `BoundedList<T, N>`, `BoundedString<N>`, `BoundedRawString<N>`, `BoundedMap<K, V, N>`, `BoundedSet<K, N>`, `Nested<T, V, N>` | `field(T, { max: N })`; `boundedList(T, N)`, `boundedUtf8(N)`, `boundedBytes(N)`, `map(K, V, { max: N })`, `set(K, { max: N })` | `Annotated[T, psio.max(N)]`; in a type alias: `Annotated[str, psio.bounded(N)]` | `BoundedList{type, maxCount}` (under `Custom "utf8"` / `"string"` / `"hex"` / `"map"` / `"set"`); `lengthBound="max=N"` on a `Nested` member | pssz (width + enforced), json/ijson (enforced), key (enforced on read; Rust and JS also on write), schema-driven codecs, WIT (comment on lists) |
| length = N, length ≥ N | `attr(f, exact<N>)`, `attr(f, min<N>)`, `psio::length_bound{.max = B, .min = A}` — member only | — | `field(T, { lengthBound: { exact, min, max } })` — member only | `psio.exact(N)`, `psio.min(N)`, `psio.length(min=A, max=B)` — member only | member attribute `lengthBound="exact=N"` / `"min=A,max=B"` | as above (`min` alone gives no width) |
| UTF-8 text | `attr(f, utf8)`; `psio::utf8_string<>` | n/a: `String` is UTF-8, `RawString` is raw bytes | `utf8` / `boundedUtf8(N)`; `field(rawString, { utf8: true })` | n/a: `str` is UTF-8, `psio.RawString` is raw bytes (`bytes` in Python) | `Custom{…, "utf8"}` (raw: `"string"`) | pssz and json/ijson (writers and readers), key (readers; the C++ key encoder does not check), schema-driven codecs |
| elements | `element(specs)` | `element(...)` | `element: { … }` | `psio.element(...)` | the element type's bounded node | as length |
| map keys / values | `keys(specs)`, `values(specs)` | `keys(...)`, `values(...)` | `keys: { … }`, `values: { … }` | `psio.keys(...)`, `psio.values(...)` | the entry tuple's bounded members | as length |
| tuple element / variant alternative | `at<I>(specs)` | tuple: `at(I, ...)`; variant: annotate the enum variant's fields | `at: { 0: { … } }` (tuple index, variant name or index) | `psio.at(i, ...)` (tuple index; union alternative index or name) | the member's bounded node | as length |
| size cap, width budget | `maxDynamicData(N)` | `#[pssz(max_dynamic_data = N)]` | `record(name, fields, { maxDynamicData: N })` | `@psio.record(max_dynamic_data=N)` | record attribute `maxDynamicData="N"` | pssz (width + enforced), schema-driven codecs, WIT `@maxDynamicData(N)`; the C++ `validate` of json and of the other binary formats (not key) applies it to the root input size |
| fixed layout | `definitionWillNotChange()` | `#[psio(definition_will_not_change)]` | `record(name, fields, { fixed: true })` | `@psio.record(definition_will_not_change=True)` | record attribute `definitionWillNotChange` | pssz (no header), bin / borsh / bincode fast paths, evolution check |
| field-count cap | `maxFields(N)` | — | `{ maxFields: N }` | `@psio.record(max_fields=N)` | record attribute `maxFields="N"` | Schema IR and WIT `@maxFields(N)` only; no codec enforces it |
| the type is its member | `transparent()` | `#[psio(transparent)]` | `transparent(name, T)` | `NewType("Name", T)`; `@psio.transparent` on a one-field class | a named alias of the member's type | pssz, key, json/ijson, views, schema, GraphQL |
| value check | `invariant(fn)`, `PSIO_INVARIANT(T, fn)` | `#[psio(invariant = "path")]` | `custom(id, wire, { check })` | `@psio.invariant` on a method; `psio.custom(id, check=…)` | `Custom{wire type, id}` + a handler | pssz, key, json/ijson writers and checking readers; not the trusted decode, not views |
| integer text form | `attr(f, json_string)`, `json_number`, `ijson_string`, `ijson_number`; type level via `PSIO_TYPE_ATTRS` | `#[json(json_string)]`, … on a field or the type | `field(T, { jsonInt: 'string', ijsonInt: 'number' })` | `Annotated[u64, psio.json_string]`, `psio.json_number`, `psio.ijson_string`, `psio.ijson_number` | member attributes `jsonInt`, `ijsonInt` = `"number"` / `"string"` | json, ijson, schema-driven text |
| custom text form | `PSIO_ADAPTER(T, psio::text_category, Impl)`; per member `attr(f, as<Tag>)` | `#[psio(custom)]` + `psio::json_text_form!(T)` | `custom(id, wire, { toText, fromText })` | `@psio.to_text` / `@psio.from_text` methods; `psio.custom(id, to_text=…, from_text=…)` | `Custom{wire type, id}` (a member `as<Tag>` is not expressible) | json, ijson, GraphQL scalar; binary unchanged |
| enumerator names | `PSIO_REFLECT_ENUM(E, a, b, …)` | the derive on a `#[repr(int)]` enum; `#[psio(name = "…")]`, `#[psio(other)]` | `enumeration(u8, { a: 0, b: 2 })` | `@psio.enum(psio.u8)` on an `enum.Enum`; `other=True` | `Enum{type, members}` | json, ijson, GraphQL; binary is the integer |
| method arguments | `method(name, attr(p, specs)…, maxDynamicData(N), returns(specs))` | `#[psio::interface]`: `#[psio(...)]` on a parameter, `#[pssz(max_dynamic_data = N)]` and `#[psio(returns(...))]` on the function | `argumentTuple(params, { maxDynamicData: N })`; return value: — | `@psio.interface` on a class; parameters and the return hint are `Annotated[...]`; `@psio.method(max_dynamic_data=N)` | `Func{params, result, attributes: maxDynamicData}` | as records |
| protobuf field number | `attr(f, field<N>)` | — | — | — | — | protobuf only |
| deprecation | `attr(f, deprecated_spec{.version = "…"})` | — | — | — | — | GraphQL (`@deprecated`) only |
| sortedness, uniqueness | `sorted`, `unique`, `unique_keys` | — | — | — | — | not enforced by any codec ([5.13](#513-sorted-unique-unique_keys)) |
| reserved vocabulary | `skip_spec`, `default_value_spec`, `since_spec`, `unstable_spec`, `final_v`, `canonical`, `flags`, `padding` | Rust has `#[psio(skip)]` (a different, implemented meaning) | — | — | — | nothing reads them ([5.15](#515-reserved-vocabulary)) |

## 5. The annotations

Each section: purpose, where it applies, spellings, Schema IR, effect per
format, enforcement, wire / width impact, evolution, mistakes.

### 5.1 `max<N>`, `exact<N>`, `min<N>` — length bounds

**Purpose.** Bound the length of a variable-size value. One spec struct,
`psio::length_bound{.exact, .max, .min}`; the helpers are
`max<N>` = `{.max = N}`, `exact<N>` = `{.exact = N}`, `min<N>` = `{.min = N}`.
A range is one spec: `psio::length_bound{.max = 8, .min = 2}`.

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
                attr(code, psio::length_bound{.max = 8, .min = 2}),        // 2..8 bytes
                attr(ids, max<4>))                                         // <= 4 elements
};
// max<N> and exact<N> give the inferred bound its N; min<N> alone bounds nothing.
// (header + 3 slots) x 1 byte + digest 32 + code 8 + ids 4 x 4
static_assert(psio::max_encoded_size<Limits>() == 4 + 32 + 8 + 4 * 4);
static_assert(psio::auto_pssz_width_v<Limits> == 1);

struct AtLeast
{
   std::string code;
   PSIO_REFLECT(AtLeast, attr(code, min<2>))
};
static_assert(!psio::max_encoded_size<AtLeast>().has_value());   // no upper bound: 32-bit
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
| pssz | `max` / `exact` enter the inferred bound (width). Enforced by `to_pssz` / `psio::encode` (throws `codec_exception`: `pssz: member length_bound violated`), `encode_pssz_checked` (status), `validate_pssz`, `validate_strict`, `decode_pssz_checked`, `from_pssz`, and the fail-fast `psio::decode` |
| key | Bytes unchanged. `validate_key`, `from_key`, `psio::decode` reject a key outside the bound. The C++ key **encoder does not check** (Rust `try_to_key`, JS `encodeKey` and Python `psio.to_key` do) |
| json / ijson | Text unchanged. Writer (`to_json` throws; `encode_json_checked` returns a status) and readers refuse a value outside the bound |
| schema-driven codecs (`psio::pssz_ir`) | The same width and the same rejections, from the IR alone |
| WIT | `list<T>/* @psio:max=N */` for a bounded list, map or set; a bounded string is `string` |
| GraphQL, other binary formats | Not read |

**Wire / width.** A bound is part of the type: it can change the `W` of
the record, and the `W` of the list, map, set, array, tuple, pair or map
entry it bounds — the member itself or a position below it. A
`std::vector<std::string>` under `max<8> | element(max<16>)` has a 1-byte
offset table; unannotated (and with no cap above it) a 4-byte one. A
string, a byte list and a list of fixed-size elements have no offsets, and
their bytes never change.

**Evolution.** Treat a published bound as frozen. Tightening it makes
readers reject stored values; loosening it lets new writers produce values
old readers reject; either can change a width. `pssz_evolves_from` compares
widths, not bounds ([8.3](#83-what-pssz_evolves_from-does-not-see)).

**Mistakes.**

- `min<2> | max<8>` puts two `length_bound` specs on one member: the
  compiler reports libc++'s `type occurs more than once in type list`.
  Write `psio::length_bound{.max = 8, .min = 2}`.
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
| pssz | Bytes unchanged. A `utf8` string that is not UTF-8 is refused by the encoder (`utf8_spec: invalid leading byte` and similar), `validate_pssz`, `validate_strict`, the checked decode and the fail-fast decode. A raw string holds any bytes; the exact-schema check `validate_strict` is stricter than the type and refuses a raw string that is not UTF-8 too (`tutorial 5`) |
| key | Bytes unchanged (raw and UTF-8 strings have one key form). The readers (`validate_key`, `from_key`) refuse a `utf8` string that is not UTF-8. The C++ key encoder does not check, for the annotation or for `psio::utf8_string` (`tutorial 7`). In Rust, TypeScript and Python a text value is UTF-8 by its type |
| json / ijson | Both kinds are JSON strings. JSON text is UTF-8, so a **raw string that is not UTF-8 has no JSON**: the writer fails |
| Schema IR | `"utf8"` vs `"string"`; schema-driven codecs enforce `"utf8"` |

**Wire / width.** None.

**Evolution.** Frozen: adding `utf8` rejects stored non-UTF-8 values;
removing it lets writers produce bytes that `utf8` readers reject.
`pssz_evolves_from` treats `std::string` → `psio::utf8_string<>` as
incompatible, but does not see an added or removed `utf8` *annotation*.

**Notes.** `utf8` is always enforced: the annotation states what the
string *is*, exactly as the type `psio::utf8_string` does. `psio::utf8` is
`psio::utf8_spec{}`, and the spelled-out `attr(f, psio::utf8_spec{})` is
the same statement — there is no form that only `validate_strict` checks
(test `reference: utf8 annotation and wrapper`, `NamesSpelled`).

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
static_assert(psio::max_encoded_size<Grid>() == 1 + 1 + (3 + 3 * (4 * 2)));
static_assert(psio::auto_pssz_width_v<Grid> == 1);

struct Choice   // a variant alternative
{
   friend bool operator==(const Choice&, const Choice&) = default;
   std::variant<std::uint32_t, std::string> v;
   PSIO_REFLECT(Choice, attr(v, at<1>(utf8 | max<16>)))
};
// header 1 + slot 1 + selector 1 + the widest alternative 16
static_assert(psio::max_encoded_size<Choice>() == 1 + 1 + 1 + 16);
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
| json / ijson and the other C++ binary formats (not key) | `validate` applies the **root** type's cap to the size of **its own input** — the JSON text, the bin bytes (`cpp/tests/caps_parity_tests.cpp`). The JSON writer does not apply it. See the limit below |
| schema-driven codecs, WIT | the same width and limit; `@maxDynamicData(N)` above the record or function |

```cpp
// A cap is the limit of the whole encoded value, header included.
CHECK(psio::to_pssz(ref::Capped{std::string(14, 'x')}).size() == 16);     // 1 + 1 + 14
CHECK_THROWS_AS(psio::to_pssz(ref::Capped{std::string(15, 'x')}), psio::codec_exception);
auto frame = psio::to_pssz(ref::Capped{std::string(14, 'x')});
frame.push_back('x');                                                     // 17 bytes
CHECK_FALSE(psio::validate_pssz<ref::Capped>(frame).ok());
```

**Limit: the cap and text.** Because the C++ JSON validator compares the
cap with the length of the text, a capped root type can write JSON its own
reader refuses:

```cpp
// maxDynamicData(16): every format's validate applies the cap to the size
// of ITS input. The pssz encoding of this value is 16 bytes and valid; its
// JSON text is 22 bytes, which the JSON reader refuses.
const Capped value{std::string(14, 'x')};
CHECK(psio::validate_pssz<Capped>(psio::to_pssz(value)).ok());
const std::string text = psio::to_json(value);              // the writer does not apply the cap
CHECK(text == R"({"s":"xxxxxxxxxxxxxx"})");
CHECK_FALSE(psio::validate_json<Capped>(text).ok());        // 22 > 16
CHECK(psio::validate_json<Capped>(R"({"s":"xxxxxxxx"})").ok());   // 16 bytes of text
```

Do not read or write a small-capped type as a JSON *root* in C++; hold it
in a member, or give it a cap that also covers its text.

**Choosing N.** [3.4 How to pick a cap](#34-how-to-pick-a-cap). In short:
at least the inferred bound of the current fields plus everything the type
may gain; 65535 keeps 16-bit offsets and is enough for most database rows
and action arguments; when unsure, one class up.
`psio::effective_max_dynamic_v<T>` (`min(cap, inferred bound)`) is a
utility for the tighter of the two; it does not select the width.

**Evolution.** `N` may not move across a width class (255 / 65535): that
changes the record's own offsets and those of the containers beneath it.
Raising it within a class keeps the bytes and passes `pssz_evolves_from`,
but old readers still reject values above their own `N`.

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
static_assert(psio::max_encoded_size<Point>() == 8);
static_assert(psio::max_encoded_size<PointOpen>() == 1 + 8);
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

**Purpose.** A declared cap on the number of fields the record may ever
have. **No C++ codec enforces it and it does not affect pssz bytes or
width.** It is recorded in the Schema IR (`maxFields="N"`), written by
`emit_wit` as `@maxFields(N)`, and readable with `psio::max_fields_v<T>` /
`psio::effective_max_fields_v<T>`. Rust: not available. TypeScript:
`record(name, fields, { maxFields: N })` (attribute only). Python:
`@psio.record(max_fields=N)` (attribute only).

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
static_assert(psio::max_encoded_size<AccountId>() == 8);
```

```cpp
struct PublicKey
{
   friend auto operator<=>(const PublicKey&, const PublicKey&) = default;
   psio::bounded<std::uint8_t, 65> data;   // the bound is in the member's type
   PSIO_REFLECT(PublicKey, transparent(), data)
};
static_assert(psio::max_encoded_size<PublicKey>() == 65);
```

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
the same name whose member evolves.

**Mistakes** (compile errors, `psio/transparent.hpp`): not exactly one data
member; combined with `definitionWillNotChange` / `maxFields` /
`maxDynamicData` (text in [5.4](#54-maxdynamicdatan)); the member is a
`std::optional`; an annotation on the member —
`psio: the member of a transparent() type takes no annotation (attr / PSIO_FIELD_ATTRS): bound the member's type or declare an invariant`.
(Rust accepts a bound on a transparent type's member:
`struct Memo(#[psio(max = 80)] String)`. In C++ write
`psio::utf8_string<80>` as the member type.)

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
CHECK_THROWS_AS(psio::to_pssz(Symbol{"usd"}), psio::codec_exception);
CHECK_THROWS_AS(psio::to_json(Symbol{"usd"}), psio::codec_exception);
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
| C++ | `invariant(fn)` in `PSIO_REFLECT`; for an enum, `std::variant`, `std::tuple` or other class: `PSIO_INVARIANT(Type, fn[, "SchemaId"])` at global scope. `fn(const T&) noexcept -> psio::codec_status` |
| Rust | `#[psio(invariant = "path")]`, `fn(&Self) -> Result<(), E>` |
| TypeScript | `custom(id, wire, { check })`; `check` returns an error string or nothing |
| Python | `@psio.invariant` on a method that returns `None` for a value of the type and a message otherwise; over any type: `Annotated[T, psio.custom("Id", check=fn)]` |
| Schema IR | `Custom{wire type, id}`; a schema cannot carry a predicate, so schema-driven codecs take it as the `check` of the handler registered for `id` |

**Where it runs.** pssz `to_pssz` / `encode`, `encode_pssz_checked`,
`validate_pssz`, `validate_strict`, `decode_pssz_checked`, `from_pssz`;
key encode, validate, decode; json / ijson writers and readers; GraphQL
arguments and results. **Not** in the trusted decode
(`from_pssz<T>(psio::trusted{bytes})`, `psio::decode`), not in views, not
in the formats outside the core set.

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
exist only where a type registers one (the wide integers register
`hex_tag`: `attr(amount, as<hex_tag>)` on a `psio::uint128` member writes
`"0xff"` in JSON). This form is **not covered by the examples test**, and
has a limit observed while writing this document: a record with such a
member does not compile under `psio::pssz` (the wide integers' hex adapter
writes text only). Use it on types that are only written as text.

| | |
|---|---|
| Rust | `#[psio(custom)]` / `#[psio(custom = "id")]` and `psio::json_text_form!(T)` with a `TextForm` implementation |
| TypeScript | `custom(id, wire, { toText, fromText })` |
| Python | `@psio.to_text` on a method and `@psio.from_text` on a `classmethod` of the class; over any type: `Annotated[T, psio.custom("Id", to_text=…, from_text=…)]` |
| Schema IR | `Custom{wire type, id}` (id: the adapter's `schema_id`, else the reflected name). A member `as<Tag>` override is not expressible in the IR |

**Limits.** A member with an `as<Tag>` override has no inferred bound
(the enclosing record is 4-byte unless capped,
[pssz-spec.md §3.2](pssz-spec.md#32-max_encoded_sizet)) and is not
addressable through a mutable view. A `sortable_binary_category` adapter replaces a
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
static_assert(psio::max_encoded_size<OpenArgs>() == 3 + 32 + (8 + 8 * 16));
static_assert(psio::auto_pssz_width_v<OpenArgs> == 1);
// transfer is capped: 16-bit for its lifetime.
static_assert(psio::max_dynamic_data_v<TransferArgs> == 1024);
static_assert(psio::auto_pssz_width_v<TransferArgs> == 2);
```

Items after the method name, in one flat list:

| Item | Meaning |
|---|---|
| `p` | a parameter name, no annotation |
| `attr(p, specs)` | a parameter name with the annotations a field's `attr` takes |
| `maxDynamicData(N)` | cap and width budget of the argument tuple |
| `returns(specs)` | annotations of the return value |

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
  ([3.3](#33-a-cap-narrows-what-lies-beneath-it)).

```cpp
// The arguments of a call are the tuple of the parameters, with the
// annotations and the cap of method(...).
const TransferArgs args{AccountId{1}, AccountId{2}, 500, "rent"};
const auto         bytes = psio::to_pssz(args);
CHECK(bytes.size() == 2 + 8 + 8 + 8 + 2 + 4);                 // 16-bit header and slot
const auto back = psio::from_pssz<TransferArgs>(bytes);       // validated: use for untrusted calls
CHECK(std::get<3>(back) == "rent");

// A memo of 81 bytes has no encoding, and a frame holding one is refused.
CHECK_THROWS_AS(psio::to_pssz(TransferArgs{AccountId{1}, AccountId{2}, 500, std::string(81, 'm')}),
                psio::codec_exception);

// The return value is checked against returns(...).
CHECK(psio::check_method_result<Bank, 2>(std::string("USD")).ok());
CHECK_FALSE(psio::check_method_result<Bank, 2>(std::string(9, 'X')).ok());

// An optional argument appended under the cap: old callers still work.
CHECK(psio::to_pssz(TransferArgsV2{AccountId{1}, AccountId{2}, 500, "rent", std::nullopt}) == bytes);
CHECK(std::get<4>(psio::from_pssz<TransferArgsV2>(bytes)) == std::nullopt);
```

| | |
|---|---|
| Rust | `#[psio::interface] trait Bank { #[pssz(max_dynamic_data = 1024)] fn transfer(&mut self, …, #[psio(max = 80)] memo: String); #[psio(returns(max = 8))] fn symbol(&self, token: u32) -> String; }`. The argument record is `bank::args::transfer` |
| TypeScript | `argumentTuple({ …, memo: field(rawString, { utf8: true, max: 80 }) }, { maxDynamicData: 1024 })`. Return-value annotations: not available |
| Python | `@psio.interface(name="bank") class Bank(Protocol)`: parameters are `Annotated[...]` hints, `@psio.method(max_dynamic_data=1024)` caps one method, the return hint carries the result's annotations (`-> Annotated[str, psio.max(8)]`). `psio.interface_of(Bank)["transfer"].args` is the argument codec |
| Schema IR | `PSIO_INTERFACE(T, types())` + `SchemaBuilder::insert_interface<T>()`: `Func{name, params, result, attributes}`; parameters and result are the bounded types; the cap is the function attribute `maxDynamicData`. `emit_wit` writes `@maxDynamicData(N)` above the function |

**Enforcement.** Arguments: as record members. Return value:
`psio::check_method_result<T, I>(value)` checks it against `returns(...)`
(a bare string or list has no width of its own, so `returns` does not
change bytes).

**Text.** C++ writes a `method_arg_tuple` in JSON as the array of the
arguments (`["alice",5,"thanks"]`); the JS `argumentTuple` and the Schema
IR describe an object keyed by parameter name. Do not exchange C++-typed
argument JSON with schema-driven readers.

**Evolution.** Append `std::optional` parameters only; declare
`maxDynamicData` from the first version (`tut-9b`, test
`method annotations: a trailing optional argument evolves under the cap`
in `cpp/tests/method_annotation_tests.cpp`).

**Mistakes.** An overloaded, template or ref-qualified member function
cannot be reflected. `psio: method(...) lists more parameters than the function has`;
`psio: method(...) lists maxDynamicData(N) more than once`;
`psio: method(...) lists returns(...) more than once`;
`psio: a returns(...) annotation does not fit the method's return type`.
More than 255 parameters: the tuple-limit error of [6.11](#611-tuple-and-pair).

### 5.12 `PSIO_REFLECT_ENUM` and `field<N>`

`PSIO_REFLECT_ENUM(Enum, enumerator…)` gives an enum its names for text
([6.3](#63-enums)). `attr(f, field<N>)` sets a member's **protobuf** field
number (default: 1-based declaration order); no other format reads it:

```cpp
struct Message
{
   std::uint32_t code = 0;
   std::string   text;
   PSIO_REFLECT(Message, attr(code, field<3>), attr(text, field<7>))
};
```

(test `reference: protobuf field numbers`: `Message{1, "x"}` is
`18 01 3a 01 78`). protobuf also reads `pb_fixed` and `pb_sint`
(`psio/protobuf.hpp`).

### 5.13 `sorted`, `unique`, `unique_keys`

`psio::sorted` (`sorted_spec{}`), `psio::unique` / `psio::sorted_unique`
(`sorted_spec{.unique = true}`) and `psio::unique_keys` can be attached to
a list. **No codec checks them**:

```cpp
struct Ranked
{
   std::vector<std::uint32_t> scores;
   PSIO_REFLECT(Ranked, attr(scores, sorted | max<4>))
};
```

```cpp
const Ranked unsorted{{3, 1, 2}};
const auto   bytes = psio::to_pssz(unsorted);                                  // accepted
CHECK(psio::validate_pssz<Ranked>(bytes).ok());                                // accepted
CHECK(psio::validate_strict<Ranked>(psio::pssz{}, std::span<const char>(bytes)).ok());
```

Their only effect today: a member annotated with one is not addressable
through a mutable view. (`std::map` and `std::set` carry `sorted` /
`unique_keys` inherently, as a statement of what the type is.) If order or
uniqueness matters,
use `std::set` / `std::map` — every reader rejects entries that are not in
strictly ascending key order — or write an invariant.

### 5.14 Strict-only spec: `hex_spec{}`

`attr(f, hex_spec{})` (also `psio::spec::hex<N>`) on a `std::string` member
is checked by `psio::validate_strict` only: `validate`, the encoder and
the decoders accept any bytes. A third-party spec struct with a
`static codec_status validate(std::span<const char>) noexcept` member is
run the same way (`psio/validate_strict_walker.hpp`). This form is **not
covered by the examples test**.

`utf8` is not such a spec: `psio::utf8_spec{}` is `psio::utf8`, enforced
by every writer and reader ([5.2](#52-utf8--text-or-raw-bytes)).

### 5.15 Reserved vocabulary

These spec values exist in `psio/annotate.hpp`, can be attached and read
back with `psio::find_spec`, and are **not read by any codec or emitter in
this tree**: `skip_spec` (`psio::spec::skip`), `default_value_spec`
(`psio::spec::default_val<V>`), `default_factory_spec`, `since_spec`,
`unstable_spec`, `final_v` (`final_spec`), `canonical`, `flags`, `padding`.
Do not rely on them. `deprecated_spec{.version = "…"}` is read by GraphQL
only (the member is `@deprecated`); GraphQL has its own annotations
(`psio::graphql::name_as`, `doc`, `hidden`, `id`, `cost`, `deprecated` —
[graphql.md](graphql.md#customisation)).

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
static_assert(psio::max_encoded_size<Amounts>() == 1 + 4 + 8 + 8 + 8 + 1);   // a 1-byte header

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
static_assert(psio::max_encoded_size<Ticket>() == 5);
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
one with `max<N>`.

```cpp
struct Blob
{
   friend bool operator==(const Blob&, const Blob&) = default;
   std::vector<std::uint8_t>   data;      // variable: hex in I-JSON
   std::array<std::uint8_t, 4> tag{};     // fixed, 4 bytes, no offset
   PSIO_REFLECT(Blob, attr(data, max<64>), tag)
};
static_assert(psio::max_encoded_size<Blob>() == 2 + 64 + 4);   // header + slot, data, tag
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
static_assert(psio::max_encoded_size<Slots>() == 1 + 16 + 1 + (3 + 3 * 16));
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
static_assert(psio::max_encoded_size<Grid>() == 1 + 1 + (3 + 3 * (4 * 2)));
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
static_assert(psio::max_encoded_size<Profile>() == 1 + 8 + (1 + 1 + 100) + (1 + 1 + 4));
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
static_assert(psio::max_encoded_size<Choice>() == 1 + 1 + 1 + 16);
```

A variant is one selector byte plus the alternative (at most 256
alternatives). Text: `[index, value]` in `json`, `{"name": value}` in
`ijson`, where the name is the alternative's type name
([json.md](json.md#variant-alternative-names)). Adding an alternative is
**not** accepted by `pssz_evolves_from` (old readers cannot read the new
alternative).

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
static_assert(psio::max_encoded_size<Entry>() == 1 + 4 + 1 + 16);
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
not a count. A variant has at most 256 alternatives; the format sets no
limit on the fields of a record
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
static_assert(psio::max_encoded_size<Book>() == 3 + (4 + 4 * 18) + 4 * 4);
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
struct Memo
{
   friend bool operator==(const Memo&, const Memo&) = default;
   std::string text;
   PSIO_REFLECT(Memo, attr(text, utf8 | max<32>))
};
struct Action
{
   std::uint64_t      sender = 0;
   psio::nested<Memo> args;          // on the wire: a byte list holding a Memo
   PSIO_REFLECT(Action, sender, attr(args, max<64>))
};
// header 1 + sender 8 + args (slot 1 + 64)
static_assert(psio::max_encoded_size<Action>() == 1 + 8 + (1 + 64));
static_assert(psio::auto_pssz_width_v<Action> == 1);
```

```cpp
const Action a{5, psio::nested<Memo>::from(Memo{"hi"})};
// The member is a byte list holding the Memo's own encoding (01 01 68 69).
CHECK(hex(psio::to_pssz(a)) == "09 05 00 00 00 00 00 00 00 09 01 01 68 69");
// In text it is the inner value, not hex.
CHECK(psio::to_ijson(a) == R"({"sender":5,"args":{"text":"hi"}})");
CHECK(psio::from_pssz<Action>(psio::to_pssz(a)).args.decode() == Memo{"hi"});
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

A member whose type is another record contributes that record's cap, or
its inferred bound:

```cpp
struct Line   // a child's cap is what the parent reserves for it
{
   tut::AccountV1 a;   // maxDynamicData(1024)
   PSIO_REFLECT(Line, a)
};
// (header + slot) x 2 bytes + the child's cap
static_assert(psio::max_encoded_size<Line>() == 2 * 2 + 1024);
static_assert(psio::auto_pssz_width_v<Line> == 2);
```

Each record owns its width: a child keeps its own `W` wherever it is
nested, also beneath a parent with a smaller cap
([3.3](#33-a-cap-narrows-what-lies-beneath-it),
[pssz-type-width.md](pssz-type-width.md)). `Line` is (header + slot) × 2
bytes + the child's cap of 1024.

### 6.15 Transparent wrappers

[5.7](#57-transparent). Use one for every id / key / quantity that should
not be confused with a bare integer or byte list. Put the bound in the
member's type.

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
static_assert(psio::max_encoded_size<Event>() == 8 + 8);
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
`maxDynamicData`, validate arguments that arrive from outside.

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

// Limit: the key ENCODER does not check member bounds; the key READER does.
const auto too_long = psio::to_key(OwnerKey{std::string(33, 'x'), 7});   // no error
CHECK_FALSE(psio::validate_key<OwnerKey>(too_long).ok());
CHECK_THROWS_AS(psio::from_key<OwnerKey>(too_long), psio::codec_exception);
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
- Validate a key that arrives from outside (`validate_key` / `from_key`);
  the C++ key encoder does not check member bounds or `utf8` (the Rust, JS
  and Python key writers do).

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
static_assert(psio::max_encoded_size<Flags>() == 1 + 1 + 2 + (1 + 2 + 1));
static_assert(psio::auto_pssz_width_v<Flags> == 1);
```

No annotation applies to a `bitvector`; `max<N>` applies to a `bitlist`
member. A `bitlist` is not a key type.

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

### 7.2 What is checked where

| | structure | member bounds, `utf8` | `maxDynamicData` | invariants |
|---|---|---|---|---|
| pssz `to_pssz` / `encode` | — | yes | yes | yes |
| pssz `validate_pssz`, `from_pssz`, `validate_strict`, `decode_pssz_checked` | yes | yes | yes | yes |
| pssz `from_pssz(psio::trusted{…})` / `psio::decode` | **no** | checked on the way | checked on the way | **no** |
| `pssz_view` | **no** | **no** | **no** | **no** |
| key `to_key` (C++) | — | **no** | — | yes |
| key `validate_key`, `from_key` | yes | yes | — | yes |
| json / ijson writers and readers | yes | yes | root text size, in `validate_json` / `from_json` only | yes |

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
width.** Nothing else.

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
- The same holds one level down: a field appended to a record that is an
  element of a bounded list raises that list's bound and can move the
  list's own width. A cap on the element record fixes what the list
  reserves for it.

### 8.2 What is not compatible, and how the tooling catches it

`psio::pssz_evolves_from<Old, New>` is a compile-time check; keep a copy of
each published revision and assert it.

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
static_assert(psio::max_encoded_size<AccountGrown>() == 2 * 4 + 8 + 32 + 136 + 8 + (1 + 100));
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
removing `transparent()`; adding a variant alternative; changing an array
length; replacing an enum by another enum type or changing a time unit.

### 8.3 What `pssz_evolves_from` does not see

The check compares layouts and widths. It does not compare:

- **member bounds and `utf8`** that leave every width unchanged;
- **enumerators** of one enum type;
- **invariants**, JSON forms, names.

```cpp
// A changed bound that keeps the width is accepted by pssz_evolves_from, but
// a reader with the tighter bound rejects records the old writer produced.
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
static_assert(psio::pssz_evolves_from<AccountV1, AccountTighter>);   // not a proof of compatibility
```

Review those by hand: treat bounds, `utf8`, invariants and the meaning of
existing enumerators as frozen once published.

### 8.4 Summary

| Change | Compatible? |
|---|---|
| append `std::optional` field, width unchanged | yes |
| append `std::optional` field, inferred width changes | **no** — reserve with `maxDynamicData` |
| append a required field; insert, remove, reorder or retype a field | no |
| change `maxDynamicData` within its width class | bytes yes; old readers reject larger values |
| change `maxDynamicData` across 255 / 65535, add or remove it when that changes `W` of the record or of a container beneath it | no |
| add a parameter or element that takes a tuple past 255 elements | no: not a type |
| tighten or loosen a member bound; add or remove `utf8` | not safely (see 8.3) |
| add an enumerator at a new value | yes in binary; text readers reject an unknown name |
| change an enum's underlying type; renumber | no |
| add a variant alternative | no |
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
| A `std::tuple` or parameter list of more than 255 elements | `psio: a tuple has at most 255 elements (docs/pssz-spec.md 8.4); this one has more. Use a record, a list, or nested tuples` |
| Expecting `attr(f, utf8_spec{})` to be a weaker `utf8` | It is `utf8`: enforced by every writer and reader |
| `min<A> \| max<B>` | `type occurs more than once in type list` (libc++). Use `psio::length_bound{.max = B, .min = A}` |
| A bound on a non-length member | `psio: a spec in the effective annotations is attached to a field whose shape is not in the spec's applies_to set. …` |
| A position spec on the wrong shape | the messages of [5.3](#53-element-keys-values-ati--positions-below-the-member) |
| The same spec in `PSIO_REFLECT` and in `PSIO_FIELD_ATTRS` | `psio: an annotation is declared twice — in PSIO_REFLECT (attr / definitionWillNotChange / maxFields / maxDynamicData / transparent) and again with PSIO_FIELD_ATTRS / PSIO_TYPE_ATTRS / PSIO_ATTRS` |
| `PSIO_FIELD_ATTRS` after the type was already used | `psio: PSIO_FIELD_ATTRS(T, f) comes after the first use of the field's annotations` |
| An annotation on a transparent type's member | `psio: the member of a transparent() type takes no annotation (attr / PSIO_FIELD_ATTRS): bound the member's type or declare an invariant` |
| An invariant that is not `noexcept` | `psio: invariant(fn) in PSIO_REFLECT(T) needs fn callable as fn(const T&), declared noexcept and returning psio::codec_status` |
| `PSIO_REFLECT` in a function-local struct | does not compile (`templates cannot be declared inside of a local class`) |
| A field and a method with one name; a member named `psio_get_proxy` | compile error (duplicate / reserved name) |
| Adding a field to a published type without a cap | compiles; `pssz_evolves_from` is false when the width moves; otherwise you used up slack |
| Reading untrusted bytes through a view or `psio::trusted` | compiles; nothing is checked |
| Expecting `sorted`, `maxFields`, `padding`, `skip_spec`, `default_value_spec` to do something | nothing reads them |
| Writing a capped root type as JSON | the reader may refuse it ([5.4](#54-maxdynamicdatan)) |

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
```

Rust entry points: `psio::to_pssz(&v)` (panics on a value with no
encoding) / `psio::try_to_pssz(&v)`; `psio::from_pssz::<T>(&bytes)`
(validated, returns `Result`); `psio::to_json`, `psio::to_ijson`,
`psio::try_to_json`; `psio::to_key`, `psio::try_to_key`, `psio::from_key`.
Not available in Rust: `exact` / `min`, `maxFields`, field numbers, a
`utf8` key (the type decides).

```typescript
import { argumentTuple, custom, field, list, optional, rawString, record, transparent, u32, u64, type Infer } from 'psio';

const accountFields = {
  id: u64,
  owner: field(rawString, { utf8: true, max: 32 }),                             // C++ utf8 | max<32>
  tags: field(list(rawString), { max: 8, element: { utf8: true, max: 16 } }),   // C++ max<8> | element(utf8 | max<16>)
  balance: u64,
};
const Account   = record('Account', accountFields);                              // Account.width === 1
const AccountV1 = record('AccountV1', accountFields, { maxDynamicData: 1024 });  // AccountV1.width === 2
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
`T.valueToJson`, `T.jsonToValue`, `T.width`. Not available in TypeScript:
a compatibility check (`pssz_evolves_from`), a trusted decode,
return-value annotations, field numbers. 64-bit and wider integers are
`bigint`.

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
# psio.codec(Account).width == 1, .max_encoded_size == 187

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
(`width`, `max_encoded_size`, `max_dynamic_data`). Every failure is a
`psio.PsioError`. Not available in Python: a compatibility check
(`pssz_evolves_from`), a mutable view, WIT / GraphQL emitters, protobuf
field numbers.
