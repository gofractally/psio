# C++ schema graph generation

SchemaBuilder emits the actual ordered schema IR for plain tuples (including
zero fields), variants, bounded collections, UTF-8 wrappers and recursive Box
graphs. Plain tuples use Tuple IR, never a numeric-name Object surrogate.
`psio::bounded<T,N>` uses BoundedList; `utf8_string<N>` uses the string adapter
over BoundedList<u8,N>.

An unlabeled `std::variant` emits alternatives named "0", "1", and so on, in
declaration order. These names add no payload bytes; the wire selector remains
the u8 alternative index. A shared C++/Rust SDK type must select matching declared
labels when the API schema needs named alternatives. C++ type names are not
inferred to be labels. Customize metadata explicitly:

```cpp
using Choice = std::variant<std::uint16_t, bool>;
template <> struct psio::schema_variant_labels<Choice>
{
   static constexpr std::array<std::string_view, 2> names{"Code", "Ready"};
};
```

Labels must be nonempty, unique, and match the alternative count; invalid
customizations fail compilation. Reordering alternatives changes the wire ABI.

Bit metadata uses existing IR alternatives: Custom "bitvector" wraps
Array<Int1 unsigned,N>, and Custom "bitlist" wraps
BoundedList<Int1 unsigned,maxCount>. These preserve the logical bit counts,
which a byte-array schema alone cannot express. Their dedicated runtime Bits
adapters must be qualified before adopting generated bit schemas; the metadata
does not imply that a plain Array<Int1> itself uses packed bits.

Box is transparent in the type graph. Interning registers each type before
walking children, so recursive records/lists/options point back to the existing
definition. Private type identity uses per-type inline address tokens within one
linked image; RTTI is unnecessary. Addresses are never serialized, and no
cross-DSO identity API is promised. Output names/order retain the existing
reflection-name/@N policy. A two-translation-unit consumer verifies repeated,
nested generic and recursive interning with exceptions and RTTI disabled.

## Names of reflected records

A reflected record is a named type of the schema, under its reflected
(unqualified) name: `struct chain::Account` is `Account`. Two different C++
types never become one node. When two types of one schema have the same
unqualified name — `a::Config` and `b::Config`, or two instantiations of a
reflected class template — each of them is named as the compiler spells it,
with its namespaces and template arguments (`a::Config`, `b::Config`,
`WithBlock<Sent>`), whichever of the two the builder met first, and the
unqualified name is then nobody's. A schema without such a pair is
unchanged: every record keeps its unqualified name (the shared fixtures are
byte-identical).

```cpp
namespace a { struct Config { std::uint32_t x; };  PSIO_REFLECT(Config, x) }
namespace b { struct Config { std::string name; }; PSIO_REFLECT(Config, name) }
struct Both { a::Config first; b::Config second; };
PSIO_REFLECT(Both, first, second)

psio::SchemaBuilder builder;
auto schema = builder.insert<Both>("Both").build();
// types: Both, a::Config, b::Config        builder.collisions(): {"Config", {"a::Config", "b::Config"}}
```

Names of types are not part of a type: the run-time comparison
(`docs/pssz-ir-runtime.md`) follows them and does not compare them, and no
PSSZ byte or JSON text contains the name of a record. They matter to a
reader of the schema and to the bytes (so the hash) of the schema itself. A
compiler's spelling of a template instantiation is its own; to give a type a
name of your choosing, specialize `psio::schema_type_name`:

```cpp
template <> struct psio::schema_type_name<b::Config>
{
   static constexpr std::string_view value = "NodeConfig";
};
```

A chosen name is never changed, and takes no part in a collision.

`insert<T>(name)` adds the root `name`. A name that already means something
else is an error, not an overwrite: `builder.status()` is then a failed
`codec_status` whose message names the name (and the type that has it), and
`build()` fails through `codec_abort` (a `codec_exception`; in a build
without exceptions check `status()` first). The same holds for two types
with one chosen name. One case is not an error: `name` is the unqualified
name of a record that was named automatically — the record gives it up and
takes its qualified name. `builder.collisions()` lists the unqualified names
that were given up, with the names the types got, for a caller that wants to
refuse them.

Unknown type shapes continue to emit an explicit unknown adapter that checked
runtime codecs reject. Arbitrary custom adapter contracts and final width/profile
selection remain separate qualification gates.

## What the builder records for schema-driven codecs

The schema-driven codec (`pssz_ir.hpp`, `pssz_ir_json.hpp`) reproduces the typed
`psio::pssz`, `psio::json` and `psio::ijson` results from the IR alone, so the
builder records everything those depend on:

- **Length bounds.** A member `length_bound{.max = N}` on a `std::string` or
  `std::vector` (through `std::optional`) is emitted as the bounded form of the
  member's type, inline in the member: the string adapter over
  `BoundedList<u8,N>`, `BoundedList<T,N>`, or the hex adapter over
  `BoundedList<u8,N>` for a byte list. This is the IR a Rust `BoundedRawString`
  / `BoundedList` produces. A bound with `exact` or `min`, or on a
  `utf8_string`, is the member attribute `lengthBound` (`"exact=2"`,
  `"min=1,max=4"`).
- **Caps and layout.** `definitionWillNotChange`, `maxDynamicData`, `maxFields`
  on the record (as before).
- **Integer text forms.** `psio::json_number` / `json_string` / `ijson_number`
  / `ijson_string`, from the member or the type-level default, are the member
  attributes `jsonInt` / `ijsonInt` = `"number"` | `"string"`.
- **Variant names in I-JSON.** Every alternative carries the attribute
  `ijsonName`, the `get_type_name` the typed `psio::ijson` writes
  (`"uint32"`, the record's name, ...). The member *name* is still the label
  or the index: `ijsonName` is text-form metadata, not an API label.
- **Bytes.** `std::vector` / `std::array` / `psio::bounded` of `char`,
  `unsigned char` or `std::byte` use the hex adapter, and all three are the
  same node: the `"hex"` custom over a list / array of `u8`
  (`Custom{List<u8>, "hex"}`), the one Rust `Vec<u8>` and the JS / Python
  byte lists build. The element of a `char` sequence is `u8`, not the
  `"char"` custom a lone `char` member is; a reader still accepts the
  former `Custom{List<Custom{u8, "char"}>, "hex"}` of older schemas.
- **Wide integers.** `uint128` / `int128` are `Int{128}`, `uint256` is
  `Int{256, unsigned}`.
- **Pairs, maps and sets.** A `std::pair<A, B>` is `Tuple{[A, B]}`, the node
  of `std::tuple<A, B>`. A map (`std::map`, `std::unordered_map`) is
  `Custom{ List{ Tuple{[K, V]} }, "map" }` and a set (`std::set`,
  `std::unordered_set`) is `Custom{ List{ K }, "set" }`; with a member
  `length_bound{.max = N}` the list is `BoundedList{..., N}` (other bounds
  are the member attribute `lengthBound`, counting entries). `map` and `set`
  are part of the binary form — the entries are in strictly ascending key
  order (`docs/pssz-spec.md` §5.9) — and take no handler. A schema-driven
  codec refuses a `map` that is not directly on a list of two-member tuples,
  a `set` that is not directly on a list, and a key type without a key
  encoding (`docs/json.md`, "Custom types through a schema").
- **Floats** are `Float{8, 24}` and `Float{11, 53}` (the exponent width was
  one short before; the Rust builder and the codec use these values).
- **Nested values.** `psio::nested<T>` (`psio/nested.hpp`) is the node
  `Nested{type: T}`, described below. A member `length_bound` on a nested
  member, which counts the bytes of the inner encoding, is always the member
  attribute `lengthBound` (`"max=64"`): there is no bounded `Nested` node.
  The validation policy of `psio::nested<T, nested_validation::deferred>` is
  not recorded: both forms produce the same node.

- **Any JSON.** `psio::json_value` is `Custom{ Custom{List<u8>, "utf8"},
  "json" }`: a UTF-8 string on the wire, whose content is the value's JSON
  text and whose text form is that value itself (`docs/json.md`, "Any JSON
  value"). `json` is a reserved id.
- **Transparent types.** A `transparent()` type is inserted under its
  reflected name as a named alias of its member's type:
  `{name: "SubjectPublicKeyInfo", type: <the member's type>}`. Nothing else
  marks it; a schema-only tool needs nothing more.
- **Custom types.** A type with a `text_category` adapter, an invariant, or
  both — of any shape: record, transparent wrapper, enum, `std::variant`,
  `std::tuple`, list — is `Custom{<its wire type>, id}`
  (`psio::schema_custom_v<T>`, `psio::schema_custom_id<T>()`): for a
  transparent wrapper the Custom wraps the member's type. The id is the
  adapter's `schema_id`, else the id given to `PSIO_INVARIANT`, else the
  reflected record or enum name; a type with none (a variant, a tuple) must
  provide one or the builder does not compile. The handler for the id —
  text form and invariant — is registered with the schema-driven codecs
  (`docs/json.md`, "Custom types through a schema").

### The `Nested` node

`Nested { type: AnyType }` (alternative 10 of `AnyType`, between `Float` and
`Custom`; C++ `psio::schema_types::Nested`) means: **a byte list holding the
PSSZ encoding of a `type` value**.

| aspect | rule for a schema-driven codec |
|---|---|
| wire | exactly `List{Int{8}}`: variable-size, minimum size 0 (an `Option` of it uses the selector form), no inferred size bound; the offset width of a type holding it is derived as for a byte list. A member `lengthBound` bounds the byte count |
| inner bytes | `type` encoded in the **automatic** profile, also inside an explicit-width frame |
| validation | the inner bytes are validated as `type` by the rule in force (compatible: unknown appended optional fields are accepted and reported; exact: rejected), with the inner root one node level below the `Nested` node. Option: treat the bytes as opaque (C++ `nested_bytes::opaque` on `validate_pssz`, `validate_auto`, `decode_auto`, `decode_compatible<W>`, `decode_exact<W>`, `encode<W>`, `encode_auto`, `validate_canonical_for_width<W>`) |
| dynamic value | the inner value (C++ `pssz_ir::Value` of `type`); encoding writes its canonical encoding. Under the opaque option: `Bytes`, read and written as they are |
| text (JSON / I-JSON) | the inner value's text (`docs/json.md`, "Nested values") |
| key | the key of the byte list (`docs/key-spec.md`) |
| malformed schema | a null `type`; a `type` that is not itself a well-formed type under the automatic profile's rules |
| foreign IDLs | bytes: WIT `list<u8>`, Cap'n Proto `Data`, FlatBuffers `[ubyte]`, protobuf `bytes` |

The alternative was called `FracPack` and had no codec; it keeps its
position, so the encoding of a Schema IR is unchanged by the rename, and the
JSON spelling of the alternative is `Nested`.

Not recorded: a member `as<Tag>` override, binary adapters, `Box`
(transparent), and the deferred-validation policy of a nested member. See
"Not expressible" in `docs/json.md`.

## Serialized forms of a schema

A `Schema` is itself a reflected value and travels in three forms, identical
in C++, Rust and JavaScript: PSSZ (`psio::encode(psio::pssz{}, schema)`, the
`schema_ir` of the shared fixtures) and the two JSON texts
(`psio::encode(psio::ijson{}, schema)` / `psio::json{}`, read back with
`psio::decode_json_checked`). The texts are the generic JSON codec's output
for the IR's own records, with `Box` transparent as in PSSZ — a type is
`{"value":{"Int":{"bits":32,"isSigned":false}}}` under `ijson` and
`{"value":[8,{...}]}` under `json` — so nothing about them is specific to
schemas. Rules, example, per-language entry points and the shared fixture
(`conformance/schema-json-vectors.json`) are in `docs/json.md`, "The Schema
IR as JSON".

## JavaScript / TypeScript

The typed definitions of the JS package (`js/src/types.ts`: `record`, `list`,
`optional`, `variant`, `enumeration`, `custom`, ...) are a builder for the
same IR: `generateSchema({ name: definition, ... })` follows
`SchemaBuilder::insert<T>(name)` — a record under its own name, every other
type interned as `@N` in first-visit order with its slot taken before its
children, a `max`-only member length bound written into the member type,
`utf8_string<N>`'s own bound as the `lengthBound` attribute — and produces the
bytes `SchemaBuilder` produces for the equivalent C++ types.
`js/src/test/typed-ir.test.ts` declares every type of the three shared
fixtures this way and requires the generated schema to equal each fixture's
`schema_ir`. A variant alternative's `ijsonName` is stated explicitly there
(`alt(type, { ijsonName })`): JavaScript has no `get_type_name`.
