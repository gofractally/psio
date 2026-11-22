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
  `unsigned char` or `std::byte` use the hex adapter.
- **Wide integers.** `uint128` / `int128` are `Int{128}`, `uint256` is
  `Int{256, unsigned}`.
- **Floats** are `Float{8, 24}` and `Float{11, 53}` (the exponent width was
  one short before; the Rust builder and the codec use these values).

Not recorded: a member `as<Tag>` override, text / binary adapters, and `Box`
(transparent). See "Not expressible" in `docs/json.md`.

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
