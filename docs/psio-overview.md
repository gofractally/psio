# psio — cross-language overview

**Status:** authoritative architectural reference. Defines what psio
**is** and what every host-language implementation must provide for a
user to recognize "this is the psio library I expect, expressed in
this language's idioms."

This document is **language-agnostic**. Specific format wire layouts
(pjson, pSSZ, etc.) live in their own `<format>-spec.md` files.
Implementation details for a given host (C++ template machinery, Rust
derive macros, etc.) live in that language's source.

---

## 1. What psio is

psio is a **multi-format, schema-driven, reflection-powered
serialization library**. A user declares a type once, with
annotations that capture text-representation hints, and gets:

- **Encoding to any of N wire formats** — pjson, pSSZ, capnp,
  flatbuf, ssz, JSON text, CBOR, MessagePack — without rewriting the
  type.
- **Decoding from any of those formats** back into the same typed
  structure, byte-equivalent across host languages.
- **Zero-copy views** over a buffer in any of those formats — typed
  field accessors that don't allocate or materialize a value tree.
- **Format-agnostic validators** that reject malformed input without
  decoding.
- **Cross-format consistency**: the same `User` type produces
  byte-identical pjson on C++ and Rust hosts. The same `User`
  produces byte-identical pSSZ on C++ and Rust hosts. Etc.

pSSZ is the primary schema-driven binary format.
Fracpack is an experimental format (`psio::experimental`, `psio/frac.hpp`),
not a supported wire format. It is kept to measure one design trade-off against
pSSZ: fracpack stores a size with every variable-size value, pSSZ stores none
and infers each size from the offsets around the value. It is not
byte-compatible with psibase's fracpack in general.

The headline value: **declare the type once, serve it everywhere.**

---

## 2. Core concepts

Every psio host implementation MUST surface these concepts, named per
the host's idioms but conceptually identical:

### 2.1 Type

A user-declared aggregate (struct, record, class) with named fields
of typed values. The type's **declaration is the wire contract** —
adding/removing/renaming fields is a wire change.

Field values are scalars, strings, lists, arrays, optionals, variants,
tuples, nested types, and **pairs, maps and sets**: a pair is the 2-tuple,
and a map / set is the list of its entries / elements in ascending key order
— one rule for every core format and language (`docs/pssz-spec.md` §5.9;
text forms in `docs/json.md`, key form in `docs/key-spec.md`). C++
`std::pair`, `std::map`, `std::set`, `std::unordered_map`,
`std::unordered_set`; Rust tuples, `BTreeMap`, `BTreeSet`, `HashMap`,
`HashSet`; JS `Map` / `Set`.

**Per-host:**
- C++: any class with `PSIO_REFLECT(...)` macro applied
- Rust: any struct with `#[derive(psio::Reflect)]`
- (Future) Python: any class with `@psio.reflect` decorator
- (Future) Go: any struct with the `psio:"..."` field-tag convention

### 2.2 Format

A wire-format-tag type that selects an encoding. Each format has its
own tag and provides `encode`, `decode`, `validate`, and (where
applicable) `view` operations dispatched by the tag.

**Built-in formats** that every host implementation MUST support:

| Format tag | Wire spec | Notes |
|------------|-----------|-------|
| `pjson` | `docs/pjson-spec.md` | Primary schemaless binary format; binary peer of JSON |
| `pssz` | `docs/pssz-spec.md` | Primary schema-driven binary format |
| `json` | RFC 8259 | Text serialization for human/JS interop |
| `ijson` | RFC 7493 (`docs/json.md`) | I-JSON: 64-bit integers as strings, bytes as hex, variants keyed by the alternative's label |
| `capnp` | Cap'n Proto wire | Cross-language IDL-driven format |
| `flatbuf` | FlatBuffers wire | IDL-driven, alignment-strict |
| `ssz` | Eth2 SSZ | Beacon-chain and content-addressed payloads |

The experimental `fracpack` implementation (`psio::experimental::frac32`,
`frac16`) is kept as a benchmark comparison; it is not a supported wire
format and is outside the required production-format set above.

A host MAY support additional formats (msgpack, CBOR, protobuf, etc.)
via the same dispatch mechanism. **Adding a format does not require
changes to the type declarations** — only a new format-tag.

### 2.3 View

A zero-copy typed accessor over a buffer in a specific format:

```text
view: View<T, F>      // T = type, F = format
view.field<I>()       // typed access to field I
view.field_by_name(s) // dynamic key lookup
```

Constant-time access; no allocation; no value-tree materialization.
**Bounded work per read** is the load-bearing invariant.

A format MAY also offer a mutable view. pssz does, for exactly one
operation: overwriting a fixed-size value in place
(`psio::mutable_view<T, pssz>`, `docs/pssz-spec.md` §17). Any change of
size or presence is a decode → modify → encode.

### 2.4 Encode / Decode / Validate (CPOs)

Three customization-point operations dispatched by the format tag:

- **Encode**: `encode<F>(value: T) -> bytes`
- **Decode**: `decode<F, T>(bytes) -> T`
- **Validate**: `validate<F, T>(bytes) -> ok | error`

Each is a per-format implementation that walks the type's fields
(via reflection) and performs the wire-format-specific work.

In C++ these are the generic layer, called with the format tag as an
argument. Code that names its format uses the per-format helper names built
on them — `to_pssz(v)`, `from_pssz<T>(bytes)` (validate, then decode),
`validate_pssz<T>(bytes)`, `pssz_view<T>` — listed for every format
in `docs/helpers.md`.

### 2.5 Annotation channel

Per-type and per-member metadata that is part of the type without changing
the member's runtime type. A type is declared and annotated **once**; every
format reads the annotations it has a use for and ignores the rest. The
reference is [annotations.md](annotations.md) (tutorial:
[tutorial-annotations.md](tutorial-annotations.md); rules for agents and
reviewers: [annotations-for-agents.md](annotations-for-agents.md)).

The annotations that exist, in their C++ spelling (the other languages'
spellings are in the [master table](annotations.md#4-master-table)):

| annotation | states | read by |
|---|---|---|
| `max<N>`, `exact<N>`, `min<N>` | the length of a string, list, map, set or nested value | pssz (offset widths, enforced), json / ijson and key (enforced), the Schema IR, WIT |
| `utf8` | a string is UTF-8 text, not raw bytes; always enforced | pssz, json / ijson, key (writers and readers), the Schema IR |
| `element(...)`, `keys(...)`, `values(...)`, `at<I>(...)` | the same, for the positions below a member | as the two rows above |
| `maxDynamicData(N)` | the limit of a record's (or an argument list's) encoded size; its offset width for its lifetime; the bound that selects the widths of the lists, maps and tuples beneath it | pssz, the Schema IR, WIT |
| `definitionWillNotChange()` | the record never gains a field: no header | pssz, bin / borsh / bincode fast paths |
| `maxFields(N)` | a cap on the number of fields of a record, or of parameters of a method (N ≤ 255); no byte, no width | enforced at compile time, by `pssz_evolves_from` and by the schema-driven codecs; the Schema IR, WIT |
| `sorted`, `unique`, `unique_keys` | the elements of a list or array are in order / pairwise different (order of their key bytes) | pssz, json / ijson, key writers and checking readers; **not** in the Schema IR |
| `transparent()` | the type is its one member; the member may carry `utf8`, `max<N>` and position specs, and the type is then that bounded type | pssz, key, json / ijson, views, schema, GraphQL |
| `invariant(fn)` | a predicate every value of the type satisfies | pssz, key, json / ijson writers and checking readers |
| `json_string`, `json_number`, `ijson_string`, `ijson_number` | the text form of an integer member | json, ijson |
| `PSIO_ADAPTER(T, psio::text_category, Impl)`, `as<Tag>` | a custom text form (a text-only adapter leaves every binary format unchanged) | json, ijson, GraphQL |
| `PSIO_REFLECT_ENUM(E, …)` | the names of an enum's values | json, ijson, GraphQL |
| `field<N>` | a protobuf field number | protobuf |

What does **not** exist: there is no `@bytes_encoding`, `@numeric_string`,
`@key_suffix`, `@as_decimal`, `@sorted_keys`, `@is_sorted`, `since_version`
or `extension_subtype` annotation in any language. A byte list is hex in
I-JSON and a number array in JSON by its type; a 64-bit integer is a string
in I-JSON by the format's rule or by `ijson_string`; an ordered list says
`sorted` / `unique`
([annotations.md §5.13](annotations.md#513-sorted-unique-unique_keys)), and
a map or set is ordered by its type. A handful of spec names are declared
in C++ and **reserved** — nothing reads them
([annotations.md §5.14](annotations.md#514-reserved-vocabulary)). One rule
holds for all of them: an annotation is enforced by every codec that has a
use for it, or it is reserved; none is checked by a "strict" mode only.
[§6](#6-annotation-taxonomy-initial) groups the annotations by what they do.

### 2.6 Nested value

A value that **is the pssz encoding of another value**, carried as a byte
list: the member of a message that holds a typed payload (action
arguments, a reply body) which other parties see, forward and store as
bytes.

| | |
|---|---|
| pssz, key | exactly the byte list holding `encode(pssz, inner)`; checking readers also validate the bytes as the inner type (`docs/pssz-spec.md` §5.8, `docs/key-spec.md` §2) |
| json, ijson | the inner value's own text, not hex (`docs/json.md`, "Nested values"). Text round trips are value round trips: a holder that must keep foreign bytes exactly uses a byte list |
| Schema IR | `Nested{type}` (`docs/schema-builder.md`) |
| view | the bytes, and the view of the inner type over them |
| validation policy | by default every checking reader of the enclosing value descends into the bytes; a member may be declared *deferred*, in which case the bytes are opaque to enclosing readers and the holder validates them itself |

**Per-host:**
- C++: `psio::nested<T>`, `psio::nested<T, psio::nested_validation::deferred>`
  (`#include <psio/nested.hpp>`)
- Rust: `psio::Nested<T>` (a second type parameter / marker for deferred)
- JS/TS: typed `nested(T)`, `nested(T, {validate: 'deferred'})`; the
  schema-driven `PsszSchema` reads the IR node

It is a value kind of the core formats only (pssz, key, json, ijson, the
Schema IR and the schema-driven codecs); the other formats do not have it.

---

## 3. Cross-language consistency

The single load-bearing invariant: **the same type, with the same
annotations, produces byte-identical wire output across all host
languages, for every supported format.**

```text
// C++
auto bytes_cpp = psio::encode<pjson>(user);
auto bytes_cpp_pssz = psio::encode<pssz>(user);

// Rust
let bytes_rust = psio::encode::<Pjson>(&user)?;
let bytes_rust_pssz = psio::encode::<Pssz>(&user)?;

assert(bytes_cpp == bytes_rust);
assert(bytes_cpp_pssz == bytes_rust_pssz);
```

Enforced via **the conformance corpus**: shared `*.json` fixtures
with input value, expected wire bytes, expected JSON projection. Both
host-language driver binaries must produce identical output for every
fixture across every format.

This is the strongest signal that psio is the same library in every
language. If a fixture passes on C++ but fails on Rust, it's a port
defect, not a language preference.

---

## 4. Per-language implementation expectations

Every host language implementation MUST provide:

### 4.1 Reflection mechanism

A way to walk a type's fields at compile time (or runtime if the
host doesn't have compile-time facilities). The mechanism is
host-idiomatic:

| Host  | Mechanism |
|-------|-----------|
| C++   | `PSIO_REFLECT(T, field1, field2, ...)` macro that emits a `psio::reflect<T>` specialization |
| Rust  | `#[derive(psio::Reflect)]` proc-macro generating a `Reflect for T` impl |
| Python| `@psio.reflect` class decorator using PEP 484 type hints |
| Go    | Field-tag convention `` `psio:"name"` `` walked via the `reflect` package at runtime |
| Java  | Annotation processor or `Field` reflection |
| JS/TS | Schema definition function or TypeScript type → reflected metadata |

The output of the reflection step is uniform: an iterable list of
`(name, type, annotations)` tuples per type, plus type-class
information (struct vs union vs enum).

#### C++: fields, methods and interfaces from one declaration

`PSIO_REFLECT` lists a type's fields and its methods together, and is
written either after the type (any namespace, unqualified name) or inside
the class body, where it also sees private members:

```cpp
namespace chain::svc {
   class token {
      std::uint64_t supply_ = 0;
      PSIO_REFLECT(token, supply_,
                   method(balance_of, owner),
                   method(transfer, from, to, amount))
     public:
      std::uint64_t balance_of(const std::string& owner) const;
      void          transfer(std::string from, std::string to, std::uint64_t amount);
   };
   PSIO_INTERFACE(token)          // functions come from reflect<token>
}
```

`psio::reflect<T>` then exposes, next to the field API, `method_count`,
`method_pointer<I>`, `method_type<I>`, `method_name<I>` / `method_names`,
`method_param_names<I>`, `method_traits<I>`, `method_result<I>`,
`method_args<I>`, `method_params<I>`, `method_is_static<I>`,
`method_is_const<I>`, `index_of_method(name)`, `find_method_index(name)`,
`index_of_method_hash<Hash>(key)` (a caller-supplied 64-bit hash; two
methods with one key do not compile), `for_each_method(fn)` and
`invoke_method<I>(object, argument_tuple)`. Overloaded member functions
cannot be listed (their address is ambiguous).

Rules of the method surface:

- **Every reflected type has all of it.** A type that lists no `method(...)`
  has the same members with the same names and kinds: `method_count` is 0,
  `method_names` is empty, the lookups find nothing, `for_each_method` calls
  nothing. Generic code may name `reflect<T>::method_param_names<I>` (or any
  other member) in a per-method callback for any reflected `T`. Using a
  per-index member with an index that is not below `method_count` is a
  compile error, "psio: method index out of range".
- **The walk gives the index and the name.** `for_each_method(fn)` calls
  `fn(std::integral_constant<std::size_t, I>{}, name)` per method, in the
  order listed. Everything else about method `I` is a member of
  `reflect<T>` named with that index:

  ```cpp
  psio::reflect<T>::for_each_method([&](auto index, std::string_view name) {
     constexpr std::size_t I = decltype(index)::value;
     using params = typename psio::reflect<T>::template method_params<I>;
     describe(name, psio::reflect<T>::template method_param_names<I>,
              std::type_identity<params>{});
  });
  ```
- **Metadata does not need the methods' definitions.** Only
  `method_pointer<I>` and `invoke_method<I>` refer to the functions. The
  count, the names, the parameter names, `method_type<I>` and what derives
  from it (`method_traits`, `method_result`, `method_args`, `method_params`,
  `method_is_static`, `method_is_const`), the lookups, `for_each_method`, an
  interface taken from the reflected type (`interface_info`, the schema
  builder's `insert_interface`, the WIT generators), a GraphQL schema's
  `sdl()` and a proxy (below) are
  computed from the declarations. A program that describes a type — its
  schema, the encoding of a call to it — links without the type's
  implementation.

Annotations are part of the same declaration, for a type in any (nested)
namespace, in the class body or after the type:

```cpp
namespace chain::state {
   struct account {
      std::uint64_t              id = 0;
      std::string                owner;
      std::vector<std::uint16_t> tags;
      std::uint32_t balance_of(std::uint32_t asset) const;
      PSIO_REFLECT(account,
                   attr(id, json_string),          // field: JSON integer form
                   attr(owner, max<32>),           // field: length bound
                   attr(tags, max<4> | sorted),    // field: several specs, all enforced
                   method(balance_of, asset),
                   definitionWillNotChange(),      // type: fixed layout
                   maxFields(8),                   // type: field-count cap
                   maxDynamicData(200))            // type: encoded-size cap
   };
}
```

The type-level keywords:

| keyword | meaning | query |
|---|---|---|
| `definitionWillNotChange()` | fixed layout: no extensibility header, no appended fields | `psio::is_dwnc_v<T>` |
| `maxFields(N)` | cap on the number of fields the record will ever have (N ≤ 255): more fields do not compile, and a later revision may not exceed or raise it. Also `method(name, ..., maxFields(N))` for a method's parameters | `psio::max_fields_v<T>` |
| `maxDynamicData(N)` | cap on the encoded size; selects the offset width | `psio::max_dynamic_data_v<T>` |
| `transparent()` | the type has exactly one data member and **is** that member in every core format (pssz, key, json, ijson, Schema IR), while staying its own type for a text adapter, an invariant, its schema node and its names (`psio/transparent.hpp`; `docs/pssz-spec.md` §5.10) | `psio::is_transparent_v<T>`, `psio::transparent_inner_t<T>` |
| `invariant(fn)` | the type's semantic invariant: `fn(const T&) noexcept -> psio::codec_status`, run by every checking reader and every writer of the core formats (`psio/invariant.hpp`; `docs/pssz-spec.md` §18) | `psio::has_invariant_v<T>`, `psio::check_invariant(value)` |

```cpp
struct SubjectPublicKeyInfo {
   std::vector<unsigned char> data;
   static psio::codec_status check(const SubjectPublicKeyInfo& k) noexcept {
      return !k.data.empty() && k.data[0] == 0x30
                ? psio::codec_ok()
                : psio::codec_fail("SubjectPublicKeyInfo: not a DER sequence", 0,
                                   "SubjectPublicKeyInfo");
   }
   PSIO_REFLECT(SubjectPublicKeyInfo, transparent(), invariant(check), data)
};
```

`transparent()` is also `PSIO_TYPE_ATTRS(T, psio::transparent)`. It does not
compile with zero or several data members, with `definitionWillNotChange` /
`maxFields` / `maxDynamicData` (a transparent type has no record layout),
or over a `std::optional`. Its member may carry the annotations that make it
a bounded type — `attr(text, utf8 | max<80>)`: the wrapper is then exactly
`psio::utf8_string<80>` in bytes, width inference, schema and checks
(`docs/pssz-spec.md` §5.10) — and no other (`exact` / `min`, `sorted`, an
integer text form, `as<Tag>` do not compile there). `invariant(fn)`
takes any expression callable as `fn(const T&)`: in a class body a static
member declared anywhere in the class, at namespace scope a name looked up
where the macro is written, or a lambda. It must be `noexcept` and return
`psio::codec_status`; a type has at most one. A type that is not a reflected
record (an enum, a `std::variant`, a `std::tuple`) registers its invariant
with `PSIO_INVARIANT(Type, fn)` or `PSIO_INVARIANT(Type, fn, "SchemaId")` at
global scope.

`attr(field, spec)` lists the field and annotates it; its spec expression is
evaluated with `using namespace psio` in effect. The annotations are members
of the reflection metadata, and `psio::annotate<&T::field>` /
`psio::annotate<psio::type<T>{}>` — what every format reads — find them
there. `PSIO_FIELD_ATTRS(T, field, spec)`,
`PSIO_TYPE_ATTRS(T, spec)` and `PSIO_ATTRS(T, (field, (spec)), ...)` remain
as separate statements, legal at any namespace scope (the type's own, an
enclosing one, the global one). The two forms compose; the same spec type
declared twice for one field or type, or a separate statement placed after
the first use of the annotations it adds to, does not compile.

### Bounds by annotation

Every annotation, per type and per format, in C++, Rust and TypeScript:
the reference [annotations.md](annotations.md), the tutorial
[tutorial-annotations.md](tutorial-annotations.md), and the rules for AI
coding agents and reviewers [annotations-for-agents.md](annotations-for-agents.md).

A bound does not have to change a member's type. The member stays the
`std::vector<std::string>` (or map, tuple, optional, …) the code wants, and
its `attr` states the bounds, at any depth:

```cpp
struct Post {
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
```

| spec | applies to | states |
|---|---|---|
| `max<N>` | string, list, map, set, nested value | at most N bytes (string, nested) / elements (list, map, set) |
| `exact<N>`, `min<N>` | the same, on the member itself | exactly / at least N |
| `utf8` | string | the bytes are UTF-8 (a plain `std::string` is raw bytes) |
| `element(specs)` | list, `std::array`, set | `specs` for every element |
| `keys(specs)`, `values(specs)` | map | `specs` for every key / mapped value |
| `at<I>(specs)` | tuple, pair, variant | `specs` for element / alternative I |

`specs` is one spec or a `spec | spec` list and may itself hold position specs
(`element(element(max<4>))` for a list of lists). A `std::optional` is looked
through: what is written for it applies to its payload. An annotation on the
wrong shape — `element(...)` on a string, `keys(...)` on a list, `at<2>` on a
pair, `max<N>` on an integer element — does not compile, with a message
naming the mistake.

The same bounds can be stated in the type instead; a user who prefers the
type to enforce the bound at construction picks the wrapper. The two spellings
are one type on the wire: the same bytes, the same offset widths, the same
`inferred_bound`, the same Schema IR.

| constraint | annotation on the plain type | wrapper type | Schema IR node |
|---|---|---|---|
| UTF-8 string | `std::string` + `utf8` | `psio::utf8_string<>` | `Custom{List<u8>, "utf8"}` |
| UTF-8 string, ≤ N bytes | `std::string` + `utf8 \| max<N>` | `psio::utf8_string<N>` | `Custom{BoundedList<u8, N>, "utf8"}` |
| raw string, ≤ N bytes | `std::string` + `max<N>` | — | `Custom{BoundedList<u8, N>, "string"}` |
| list of ≤ N elements | `std::vector<T>` + `max<N>` | `psio::bounded<T, N>` | `BoundedList<T, N>` (bytes: `Custom{…, "hex"}`) |
| ≤ N strings of ≤ M bytes | `std::vector<std::string>` + `max<N> \| element(utf8 \| max<M>)` | `psio::bounded<psio::utf8_string<M>, N>` | `BoundedList<Custom{BoundedList<u8, M>, "utf8"}, N>` |
| elements of an array | `std::array<std::string, K>` + `element(utf8 \| max<M>)` | `std::array<psio::utf8_string<M>, K>` | `Array<…, K>` |
| map of ≤ N entries, bounded keys / values | `std::map<K, V>` + `max<N> \| keys(…) \| values(…)` | `std::map<bounded K, bounded V>` + `max<N>` | `Custom{BoundedList<Tuple<K', V'>, N>, "map"}` |
| tuple / pair element, variant alternative | `at<I>(…)` | the element's wrapper type | `Tuple` / `Variant` with the bounded member |
| exact / minimum length (member only) | `exact<N>`, `min<N>` | — | member attribute `lengthBound` |

Every bound is enforced wherever the value crosses a codec: `psio::pssz`
refuses a value outside the tree on encode (`encode` throws,
`encode_pssz_checked` reports) and a frame that holds one on `validate`,
`validate_strict`, the checked decode and the fail-fast decode; `psio::json`
/ `psio::ijson` refuse it when writing and reading; `psio::key` refuses it
when writing a key (`to_key` throws, `encode_key_checked` reports) and when
reading one; the schema-driven codecs enforce the bounded nodes the
schema carries. A record, a tuple, a pair, a map entry, an array of variable
elements and a list, map or set of variable elements each take their offset
width from their own bound, and the annotations of their position narrow it
exactly as the wrapper types would: a `std::vector<std::string>` under
`max<8> | element(max<16>)` has a 1-byte offset table, the unannotated vector
a 4-byte one (`docs/pssz-type-width.md`). A record's `maxDynamicData` is also
an implicit bound of the lists, maps, tuples and arrays beneath it (down to
the next record): under a cap of 255 a plain `std::vector<std::string>` member
has a 1-byte table. A view of a member whose annotations
narrow a width carries them (`psio::pssz_at`); the one exception is a variant
whose alternatives are narrowed by `at<I>(…)`, which is not addressable
through a `pssz_view` (state the bound in the alternative's type to view it).
A member with `element` / `keys` / `values` / `at` annotations is not
addressable through a mutable view.

### Method parameters and the argument cap

`method(name, ...)` takes the same annotations for its parameters, a byte
cap (`maxDynamicData(N)`) and a parameter-count cap (`maxFields(N)`, N ≤ 255)
for its argument tuple, and annotations for its return value, in one flat
list ([annotations.md §5.11](annotations.md#511-methodname-)):

```cpp
struct token {
   void        transfer(std::string to, std::uint64_t amount, std::string memo);
   std::string symbol(std::uint32_t id) const;
   PSIO_REFLECT(token,
                method(transfer, to, amount, attr(memo, utf8 | max<80>), maxDynamicData(256)),
                method(symbol, id, returns(utf8 | max<8>)))
};
```

`attr(p, specs)` names parameter `p` and annotates it; a bare name is a
parameter without annotations; `maxDynamicData(N)` caps the encoded argument
tuple and — as for a record — fixes its offset width for the method's
lifetime, reserving room for optional arguments appended later;
`returns(specs)` annotates the return value. Existing `method(name, p...)`
declarations are unchanged.

- `reflect<T>::method_annotations<I>` is the metadata (`.params`, one spec
  tuple per listed parameter; `.result`; `.max_dynamic_data`), and
  `reflect<T>::method_max_dynamic_data<I>` the cap.
- `reflect<T>::method_arg_tuple<I>` is the argument tuple type that carries
  them: `psio::annotated_tuple<…, decayed parameters…>`. It derives from and
  converts from `std::tuple<decayed parameters…>`; encode, decode and
  validate it like any value and the parameter bounds select its width and
  are enforced, and the cap rules:

  ```cpp
  using Args = psio::reflect<token>::method_arg_tuple<0>;
  static_assert(psio::auto_pssz_width_v<Args> == 2);            // the cap: 256
  auto bytes = psio::to_pssz(Args{to, amount, memo});           // refuses a memo of 81 bytes
  auto args  = psio::from_pssz<Args>(bytes);                    // refuses one too
  std::apply([&](auto&... a) { t.transfer(a...); }, args.tuple());
  ```

- A trailing `std::optional` argument added under the cap keeps the width:
  `psio::pssz_evolves_from<OldArgs, NewArgs>` is true. Without a cap the width
  is the inferred bound's; an added argument that crosses 255 or 65 535 bytes
  changes it, and `pssz_evolves_from` is false at compile time.
- A result is written, read and viewed as the type `returns(...)` makes of
  it: `psio::method_result_to_pssz<T, I>(value)` (the fast write;
  `method_result_to_pssz_checked` holds the value against `returns(...)`),
  `method_result_from_pssz<T, I>(bytes)` (or `psio::trusted{bytes}`),
  `validate_method_result<T, I>(bytes)`,
  `validated_method_result_view<T, I>(bytes)`; `encode_method_result` /
  `decode_method_result` are the same returning a status, and
  `psio::check_method_result<T, I>(value)` only checks
  (`docs/helpers.md`, "The result of a method"). An empty optional result
  whose payload cannot be empty is zero bytes. In a schema without the
  `Interface` envelope the method is
  `SchemaBuilder::insert_method<T, I>()`. A bare string has no
  width of its own, so its bytes do not depend on the bound; a bounded list
  of variable-size elements has the narrow offset table of the bounded type.
- `psio::inferred_bound<Args>()` is the bound inferred from the
  parameters, as for a record; the cap is `psio::max_dynamic_data_v<Args>`;
  `psio::max_encoded_size<Args>()` is the largest encoding (the parameters
  at the width the cap selects, and at most the cap).
- In JSON an argument list whose parameters are all named is written as an
  object keyed by parameter name and read as that object or as the
  positional array; an unnamed list is the array. Binary is always the
  positional tuple (`docs/json.md`, "Method arguments").
- In the Schema IR of a `PSIO_INTERFACE` whose functions come from
  `reflect<T>`, the parameters and the result are the bounded types and the
  caps are the function's `maxDynamicData` and `maxFields` attributes;
  `emit_wit` writes `@maxDynamicData(N)` above the function. An argument
  list as a type is an `Object` with the attribute `tuple = "named"` or
  `"positional"`. The shared rows are `conformance/interface-vectors.json`.

There is one proxy, `psio::reflect<T>::proxy<P>`. It inherits `P`; each
field is a member `name()` that returns `P::get<I, &T::name>()` and each
method a member `name(args...)` that returns
`P::call<I, &T::name>(args...)` — index and member pointer are template
value arguments. An interface's `interface_info<Tag>::proxy<P>` uses the
same call convention. A call dispatcher declares

```cpp
template <std::size_t I, auto MemberPtr, typename... Args>
auto call(Args&&... args);
```

Rules of a proxied call:

- **Arguments are forwarded as references.** `Args&&...` are forwarding
  references: `call` sees an lvalue argument as `A&` (or `const A&`) and an
  rvalue as `A`, and nothing is copied or moved between the caller and
  `call`. There is no by-value mode. A dispatcher that needs decayed or
  owned types — to choose an encoding from `std::decay_t<Args>`, or to keep
  the arguments after the call returns — decays them itself.
- **A proxy does not need the method's definition.** The member pointer is
  a template argument; a dispatcher that encodes a call instead of making it
  links without the reflected type's implementation.
- **`call` is not told the reflected type.** `MemberPtr` is `&T::name` as
  the language types it: for a method `T` inherits it is
  `R (Base::*)(...)`, and for a static method a plain function pointer. The
  class recovered from the pointer's type is then the base — in whose
  reflection index `I` is another method, or none — or no class at all. A
  dispatcher that needs `T` takes it as its own template parameter, which
  is right for every method (whoever writes `reflect<T>::proxy<P>` knows
  `T`):

  ```cpp
  template <typename T>
  struct recorder {
     template <std::size_t I, auto MemberPtr, typename... Args>
     void call(Args&&...) { log(psio::reflect<T>::template method_name<I>); }
  };
  psio::reflect<token>::proxy<recorder<token>> api;
  ```

  A dispatcher that recovers the class from the pointer anyway uses
  `psio::proxied_class<I, MemberPtr>`: the class of `MemberPtr`, checked to
  be the type whose reflection has `MemberPtr` as method `I`. For an
  inherited or a static method it does not compile, with a message that
  says to pass the reflected type.

`PSIO_INTERFACE(Tag, [types(...)], [funcs(...)], [name("...")],
[package(pkg)])` registers a WIT interface from any namespace. With
`funcs(...)` omitted the functions are `reflect<Tag>`'s methods. An
instance member function is a WIT function with the object implicit (a
resource method — `[method]resource.name` — when the tag is a resource);
a static member function is a function over exactly its parameters. The
interface's package is, in order: its `package(pkg)` item; the single
`PSIO_PACKAGE` of the enclosing namespace (nearest first; a global
package applies to global interfaces only); the package named by
`#define PSIO_CURRENT_PACKAGE_` under the same scope rule; otherwise a
package named after the tag's namespace with no version.

### 4.2 Annotation channel

A way to attach per-type and per-member metadata, accessible during the
reflection walk. Same vocabulary across hosts (§2.5); every spelling is in
the [master table of annotations.md](annotations.md#4-master-table).

| Host  | Mechanism |
|-------|-----------|
| C++   | `attr(field, spec)` and the type-level keywords inside `PSIO_REFLECT` (any namespace, in-class or after the type), or `PSIO_FIELD_ATTRS` / `PSIO_TYPE_ATTRS` / `PSIO_ATTRS` as separate statements; wrapper types (`psio::bounded<T, N>`, `psio::utf8_string<N>`) say the same in the type; read through `psio::annotate<X>` |
| Rust  | Derive attributes: `#[psio(...)]` describes the type for every derive (`#[psio(max = 32)]`, `#[psio(element(max = 16))]`, `#[psio(transparent)]`, `#[psio(definition_will_not_change)]`, `#[psio(invariant = "path")]`); `#[pssz(max_dynamic_data = N)]` and `#[json(json_string)]` hold what is specific to one format. There is no `#[psio(annotation = "value")]` form. Bounded wrapper types (`BoundedString<N>`, `BoundedList<T, N>`) exist as well |
| JS/TS | No decorators. Types are values built with functions (`record`, `field`, `list`, …); annotations are the option objects: `field(rawString, { utf8: true, max: 32 })`, `record(name, fields, { maxDynamicData: N })` |
| Python| `typing.Annotated` metadata on the hints of a `@dataclass` (`Annotated[str, psio.max(32)]`, `psio.element(...)`), and decorators for the type level: `@psio.record(max_dynamic_data=N)`, `@psio.transparent`, `@psio.enum(...)`, `@psio.interface`, `@psio.method(...)`, `@psio.invariant` |
| Go, Java | no implementation in this repository |

### 4.3 Format-tag dispatch

A way to select the wire format at the call site. Whether via traits,
templates, function-overload sets, or runtime registry.

| Host  | Mechanism |
|-------|-----------|
| C++   | `psio::to_pssz(v)` where the format is named (`docs/helpers.md`); `psio::encode(format_tag{}, v)` in generic code — a customization point object dispatched via `tag_invoke` |
| Rust  | `psio::encode::<F: Format>(&v)` — generic over a `Format` trait |
| Python| `psio.encode(v, format='pjson')` — string lookup against a registry |
| Go    | `psio.Encode(v, psio.Pjson)` — interface-typed format parameter |

### 4.4 Conformance driver

Every host language implementation MUST provide a binary that:

1. Reads a conformance-corpus fixture on stdin.
2. Encodes the fixture's `input_value` per the format under test.
3. Compares to the fixture's `wire_hex`.
4. Decodes `wire_hex` back to a value.
5. Compares structurally to `input_value`.
6. Renders to JSON and compares to `json_compact` (when applicable).
7. Exits 0 on match, non-zero on any mismatch.

The driver runs as `psio_<format>_conformance_driver --check`. The
existing `pjson_conformance_driver` (in C++ and Rust) is the
canonical example; future formats and hosts mirror its shape.

### 4.5 Self-test mode

The driver MUST support a `--self-test` mode that asserts
implementation-property invariants (e.g., the §3 tag-byte predicates
in pjson). Mirrors what `cpp/conformance/pjson_conformance_driver`
and `rust/psio/src/bin/pjson_conformance_driver.rs` do today.

---

## 5. Format catalog and capability matrix

A new format-tag implementation in psio MUST provide:

| Capability | Required? | Notes |
|------------|-----------|-------|
| `encode<F>(v: T)` | yes | Walks reflection, produces wire bytes |
| `decode<F, T>(bytes)` | yes | Reads wire bytes, materializes T |
| `validate<F, T>(bytes)` | yes | Bounded-work check; no decode |
| `View<T, F>` | strongly preferred | Zero-copy field access |
| `from_json(text)` ↔ `to_json(F-buffer)` | when applicable | Required for text-formats and pjson; optional for binary-only formats |
| Self-test mode | yes | Driver `--self-test` arm |
| Conformance corpus | yes | Shared `*.json` fixtures matching the format's wire bytes |

A format that doesn't implement `validate` or `View` is **not
production-ready**; the dispatch CPO returns "not implemented" rather
than silently doing the wrong thing.

---

## 6. Annotation taxonomy (initial)

The annotation channel carries metadata that is part of the type and
meaningful across formats. This section groups what exists by what it
does; the reference for every spelling, format and language is
[annotations.md](annotations.md) and its
[master table](annotations.md#4-master-table). (An earlier revision of this
section listed a pjson-era vocabulary — `bytes_encoding`,
`as_numeric_string`, `as_decimal`, `escape_form`, `sorted_keys`,
`is_sorted`, `key_suffix`, `since_version`, `extension_subtype`. None of
those exists in any language.)

### 6.1 Size: bounds and caps

- `max<N>`, `exact<N>`, `min<N>` (and `min<A> | max<B>`, one range) — the
  length of a string, list, map, set or nested value. Enforced by pssz,
  json / ijson and key writers and readers; `max` / `exact` select pssz
  offset widths.
- `element(...)`, `keys(...)`, `values(...)`, `at<I>(...)` — the same for
  the positions below a member, to any depth.
- `maxDynamicData(N)` — the byte cap of a record or argument list, and its
  offset width for its lifetime.
- `maxFields(N)` — the cap on the number of fields or parameters (N ≤ 255);
  every record and tuple has at most 255 regardless.

### 6.2 What a value is

- `utf8` — a string is UTF-8 text; a plain `std::string` is raw bytes
  (Rust `String` / `RawString`). Bytes are a byte list by type: hex in
  I-JSON, a number array in JSON. There is no bytes-presentation hint.
- `sorted`, `unique`, `unique_keys` — the order or uniqueness of a list's
  elements, by their key bytes. Enforced by the typed codecs; not in the
  Schema IR. A map or set is ordered and unique by its type.
- `transparent()` — the type is its one member (which may be bounded).
- `invariant(fn)` — a predicate every value satisfies.
- `definitionWillNotChange()` — the record never gains a field.

### 6.3 Text presentation

- `json_string`, `json_number`, `ijson_string`, `ijson_number` — the text
  form of an integer member; without them `ijson` writes a 64-bit integer
  beyond 2^53 − 1 as a decimal string.
- `PSIO_ADAPTER(T, psio::text_category, Impl)` and `as<Tag>` on a member — a
  custom text form; the binary form is unchanged.
- `PSIO_REFLECT_ENUM` — enumerator names for text; the wire is the integer.

### 6.4 One format only

- `field<N>` — a protobuf field number.
- `deprecated_spec` and the `psio::graphql` annotations — GraphQL.

### 6.5 Reserved

Declared in C++, read by nothing: `hex_spec`, `skip_spec`,
`default_value_spec`, `default_factory_spec`, `since_spec`, `unstable_spec`,
`final_v`, `canonical`, `flags`, `padding`
([annotations.md §5.14](annotations.md#514-reserved-vocabulary)). There is
no versioning annotation: a type evolves by appending optional fields
under an unchanged width, and an existing bound may only widen
([annotations.md §8](annotations.md#8-evolution-what-may-change-after-publication),
[pssz-optional-evolution.md](pssz-optional-evolution.md)).

---

## 7. Conformance — the cross-language gate

Every commit that touches encoding behavior MUST pass:

```sh
$ tools/run-conformance.sh
cpp:    N/N passed
rust:   N/N passed
cross-validation: M/M matched   # < the load-bearing line
```

`cross-validation` runs every fixture's `input_value` through both
host-language drivers and asserts they produce identical
`wire_hex` and `json_compact`. A mismatch is a port defect; it
blocks merge.

`docs/spec-compliance.md` enumerates every normative claim from the
format specs. Every row must be ✅ on every implemented host before
that format is considered shipped on that host.

---

## 8. Versioning

Format wire formats version at the format-spec level (`pjson-spec.md`
§13, `pssz-spec.md`, etc.). The psio cross-language API
versions independently — adding a new annotation or format tag is a
psio API change, not a wire-format change.

A psio implementation MAY support older format versions for
backward-compat reading; it MUST NOT silently produce older wire
bytes when newer would be canonical.

---

## 9. What this overview does NOT define

- Specific format wire formats (those have their own `<format>-spec.md`).
- Per-host build/dependency conventions (see `docs/dependency-pattern.md`).
- Per-host API ergonomics (return types, error model, async).
- Performance targets (each format has its own §1 headline numbers).

What it DOES define: **the conceptual model and binding requirements
that any host implementation must satisfy to be called "psio."**

---

## 10. Adding a new host language

Concrete checklist for porting psio to a new host (Python, JS, Java,
etc.):

1. Pick the host's reflection mechanism (§4.1) and document it in
   `docs/dependency-pattern.md`.
2. Pick the host's annotation syntax (§4.2) and document the mapping
   from §2.5's taxonomy to the host's syntax.
3. Pick the host's format-tag dispatch (§4.3).
4. Implement at least the `pjson` format end-to-end.
5. Implement the conformance driver (§4.4) and pass the existing
   conformance corpus.
6. Add a `<host>` row to the conformance harness in
   `tools/run-conformance.sh` and the cross-validation step.
7. Add ✅ to the relevant matrix rows in `docs/spec-compliance.md`
   when each row passes for the new host.

If all six steps complete, the new host is "psio-conforming" — users
can write a type once and serve it from this host as expected.
