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
| `ijson` | RFC 7493 (`docs/json.md`) | I-JSON: 64-bit integers as strings, bytes as hex, named variants |
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

Per-type and per-field metadata that drives wire-form selection
without changing the field's runtime type. The annotation channel is
**format-agnostic** — the same annotation can be honored differently
by each format, but its presence is uniform.

**Examples:**

```
@bytes_encoding("hex")        — bytes field rendered as lowercase hex in JSON-text formats
@numeric_string                — integer field rendered as quoted JSON string (snowflake IDs)
@key_suffix(".unix_ms")        — object key written with this suffix
@as_decimal                    — float-valued field encoded as exact decimal (not IEEE binary)
@sorted_keys                   — object's keys are guaranteed sorted (sets the is_sorted hint)
@is_sorted                     — array's elements are guaranteed sorted
```

Every host MUST support a uniform annotation taxonomy. Format
implementations consult the annotation channel during dispatch; the
binding is **annotation determines wire form**, not the value.

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
                   attr(tags, max<4> | sorted),    // field: several specs
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
| `maxFields(N)` | cap on the field count | `psio::max_fields_v<T>` |
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
with an annotation on its member, or over a `std::optional`. `invariant(fn)`
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
`max_encoded_size`, the same Schema IR.

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
when reading a key; the schema-driven codecs enforce the bounded nodes the
schema carries. A list's own offset table is 4 bytes wide whatever bounds it;
a record, a tuple, a pair, a map entry and an array of variable elements take
their width from their bound, and the annotations of their position narrow it
exactly as the wrapper types would. A member whose annotations narrow such a
nested value is not addressable through a `pssz_view` (state the bound in the
type to view it), and a member with `element` / `keys` / `values` / `at`
annotations is not addressable through a mutable view.

### Method parameters and the argument cap

`method(name, ...)` takes the same annotations for its parameters, a cap for
its argument tuple, and annotations for its return value, in one flat list:

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
- `psio::check_method_result<T, I>(value)` checks a return value against
  `returns(...)`. (A bare string or list has no width of its own, so its
  bytes do not depend on the bound.)
- In the Schema IR of a `PSIO_INTERFACE` whose functions come from
  `reflect<T>`, the parameters and the result are the bounded types and the
  cap is the function's `maxDynamicData` attribute; `emit_wit` writes it as
  `@maxDynamicData(N)` above the function.

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

A way to attach per-type and per-field metadata, accessible during
the reflection walk. Same taxonomy across hosts (§2.5).

| Host  | Mechanism |
|-------|-----------|
| C++   | `attr(field, spec)` and the type-level keywords inside `PSIO_REFLECT` (any namespace, in-class or after the type), or `PSIO_FIELD_ATTRS` / `PSIO_TYPE_ATTRS` as separate statements; read through `psio::annotate<X>` |
| Rust  | `#[psio(annotation = "value")]` attribute on the field/struct |
| Python| Class decorator with field metadata, OR `Annotated[T, psio.AnnotationName]` from `typing` |
| Go    | Extended field-tag syntax: `` `psio:"name,bytes_encoding=hex"` `` |
| Java  | `@PsioAnnotate(...)` field annotation |
| JS/TS | Decorators (TC39) or schema-builder fluent API |

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

The annotation channel carries metadata that's meaningful across
formats. Below is the **starter taxonomy** every host MUST recognize.
Format implementations MAY honor more, but these are the cross-format
baseline:

### 6.1 Bytes-presentation hints

- `bytes_encoding = base64` (default) | `hex` | `base58` | `base64url`
  — controls JSON-text rendering of byte fields. Wire format MAY
  carry this hint inline (pjson §4.10); other formats encode raw
  bytes and rely on the schema annotation alone.

### 6.2 Integer-presentation hints

- `as_numeric_string` — emit as quoted JSON string in JSON-text
  formats; in binary formats, may use a wrapper type (pjson
  `numeric_string`, §4.8) or just behave normally.
- `as_decimal` — for float-valued fields: encode as exact decimal
  (mantissa + scale). pjson uses `decimal` (§4.7), other formats
  may use a string representation.

### 6.3 String-presentation hints

- `escape_form` — text is already JSON-escape-encoded; emitter
  doesn't re-escape. (pjson §4.9 flag 1)
- `raw_text` (default) — text is unescaped Unicode; emitter runs the
  escape pass.

### 6.4 Container-shape hints

- `sorted_keys` (object) — keys are in lex-sorted byte order.
  Enables binary-search lookups (pjson §E-001).
- `is_sorted` (array) — element values are sorted. Enables binary
  search in array views.
- `key_suffix = ".tag"` — object key carries this suffix for the
  text-format projection.

### 6.5 Format-version hints (future)

- `since_version = 2` — field added in spec version 2; older
  decoders must skip-with-default.
- `extension_subtype = N` — value is a pjson `extension` (§4.11)
  with sub-type id N.

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
