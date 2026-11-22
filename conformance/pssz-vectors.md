# PSSZ cross-language conformance vectors

`pssz-vectors.json` is one fixture consumed by both implementations:

- C++: `cpp/tests/pssz_conformance_tests.cpp` (ctest `psio_pssz_conformance_tests`)
- Rust: `rust/psio/tests/pssz_conformance.rs` (`cargo test --test pssz_conformance`),
  run once against `pssz_struct!` records and once against the proc-macro
  derives. It checks the type manifest (shape, automatic width, bound, cap,
  fixedness), every vector on every reader entry point, a truncation /
  corruption sweep over the valid frames, and that values written natively
  in Rust encode to the fixture bytes. Its type mapping is
  `rust/psio/tests/conformance/types.rs`.

It is generated from the C++ implementation by
`cpp/tests/pssz_conformance_emit.cpp`. The fixture types live in
`cpp/tests/pssz_conformance_types.hpp` (namespace `pssz_conf`).

## Regenerating

```
cmake --build <build> --target psio_pssz_conformance_emit
<build>/cpp/tests/psio_pssz_conformance_emit conformance/pssz-vectors.json
<build>/cpp/tests/psio_pssz_conformance_emit --stats      # class counts
```

Generation is deterministic (stable order, no timestamps): regenerating
without a codec change is byte-identical. ctest `psio_pssz_conformance_regen`
runs `--check` and fails when the checked-in file is not what the
implementation generates. These are wire bytes: regenerate only for an
intended wire-format change, never to make a test pass.

Outcomes are computed from the implementation. Every hand-made frame and every
evolution pair also carries the outcome expected from the format rules (and,
for hand-made rejections, the expected error); the emitter aborts when the
implementation disagrees. The class marked OBSERVED ONLY is exempt.

## File format

```
{ "version": 1,
  "schema_ir": "<hex>",
  "types":   [ { "id", "ir", "shape", "custom_id"?, "invariant"?, "auto_width",
                 "max_encoded_size", "max_dynamic_data", "fixed", "fixed_size" } ... ],
  "vectors": [ { "name", "class", "type", "format", "hex", "expect",
                 "reason", "reencoded", "value", "writer" } ... ] }
```

`schema_ir` is the `psio::pssz` encoding (lowercase hex) of one Schema IR
(`psio::Schema`) that holds every fixture type under its id. The schema-driven
tests (`cpp/tests/schema_conformance_tests.cpp`,
`rust/psio/tests/schema_conformance.rs`,
`js/src/test/pssz-conformance.test.ts`) use it and nothing else: IR + bytes
must give the outcome recorded for the typed codec, for every vector, and the
offset width derived from the IR must equal every type's `auto_width`.

Types:

- `ir` - whether the Schema IR describes the type faithfully (the emitter
  aborts if an `ir: true` type's IR-derived width differs from `auto_width`).
- `id` - stable type id. A record or enum is a named type and its id is its
  name. Every other type is structural and its id is its shape string.
- `shape` - the logical type (grammar below).
- `custom_id` - present for a custom type (one with a custom text form and /
  or an invariant): the id of its Schema IR `Custom` node, under which its
  handler is registered with a schema-driven codec.
- `invariant` - present (`true`) when an invariant constrains values of the
  type: its own, or that of a type it holds (`custom-types.md`).
- `auto_width` - `auto_pssz_width_v<T>`: 1, 2 or 4.
- `max_encoded_size` - `psio::max_encoded_size<T>()`, or null when unbounded.
- `max_dynamic_data` - the explicit cap, or null.
- `fixed`, `fixed_size` - whether the type is fixed-size, and its size (0 if not).

Vectors:

- `name` - unique: `<class>/<description>/as/<type>/<format>`.
- `class` - see the table below.
- `type` - the reader's type id.
- `format` - `auto` (`psio::pssz`, type-derived widths), `pssz8`, `pssz16`,
  `pssz32` (one inherited width).
- `hex` - the frame, lowercase, possibly empty.
- `expect`
  - `canonical`: every entry point accepts, no unknown fields are reported,
    and `encode(decode(bytes)) == bytes`.
  - `unknown`: compatible validation and decoding accept and report unknown
    fields, strict validation rejects, and `encode(decode(bytes)) == reencoded`.
  - `reject`: every checked entry point rejects. (For class `invariant` this
    is the outcome of the typed codec, and of a schema-driven codec given
    the handlers; a schema-driven codec with no handler accepts the frame:
    `custom-types.md`.)
- `reason` - for `reject`, the C++ `validate` error message (for class
  `invariant`, the invariant's own message); otherwise empty.
- `reencoded` - for `unknown`, the hex of the reader's re-encoding; otherwise empty.
- `value` - id of the logical value written, or empty for a hand-made frame.
- `writer` - the writer's type id, or empty for a hand-made frame.

Entry points checked by the C++ test: `validate`, `validate_or_throw`,
`validate_strict`, `validate_pssz_compatible`, `decode_pssz_checked`, `decode`
and re-encode identity. The unchecked `psio::decode` is not a validating entry
point: for `reject` vectors it is asserted to throw only for the errors it
shares with the validator (`pssz_conf::decode_also_rejects`), and for none at
all in the `nested*` classes: it adopts the bytes of a nested value without
reading them. For a bad bool,
a variant index out of range, malformed list / array offset tables, bitlist and
bitvector padding and excess depth it must not be called on unvalidated bytes.

## Shape grammar

```
u8 u16 u32 u64 i8 i16 i32 i64 u128 i128 u256 f32 f64 bool
string              unbounded UTF-8            (C++ psio::utf8_string<>, Rust String)
string<=N           UTF-8, at most N bytes     (C++ psio::utf8_string<N>)
raw_string          raw bytes                  (C++ std::string, Rust RawString)
raw_string<=N       std::string member with length_bound max N
list<T>             std::vector<T>
list<T><=N          std::vector<T> member with length_bound max N
array<T,N>  optional<T>  variant<A,B,...>  tuple<A,B,...>  box<T>
nested<T>           a byte list holding the automatic-profile encoding of a T,
                    validated as T by every checking reader (C++ psio::nested<T>)
nested<T,deferred>  the same bytes and Schema IR node; enclosing readers do not
                    look inside (C++ psio::nested<T, nested_validation::deferred>)
nested<T><=N        nested member with length_bound max N (bytes of the encoding)
map<K,V>            entries in ascending key order (C++ std::map / std::unordered_map,
                    Rust BTreeMap / HashMap); on the wire list<tuple<K,V>>
set<K>              elements in ascending key order (C++ std::set / std::unordered_set,
                    Rust BTreeSet / HashSet); on the wire list<K>
map<K,V><=N  set<K><=N   map / set member with length_bound max N (entries)
bitvector<N>  bitlist<N>
enum<u8>{A=0,B=1}   enumerator names and values
record{name:T,...}  extensible record       dwnc{name:T,...}  definition_will_not_change
record{...}@cap=N   explicit maxDynamicData
transparent{name:T} a transparent type: on the wire it is T (docs/pssz-spec.md 5.10)
```

A `std::pair<A,B>` is the tuple and has the tuple's shape and id
(`tuple<u16,raw_string>` is a `std::pair` in the C++ fixture types). The
container holding a map or set is not part of its shape.

A nested record, enum or transparent type is referred to by its id; every id referred to is in
`types`. Field names are part of the shape. `<=N` on a `raw_string`, `list` or `nested`
exists only for record members. In the Schema IR a nested value is the node
`Nested{T}` whatever its validation policy, and a nested member's bound is the
member attribute `lengthBound`.

## Classes

308 types, 6589 vectors.

| class | vectors | contents |
|---|---|---|
| `corpus` | 956 | Canonical encodings of named values; `writer` == `type`. |
| `evolution-forward` | 660 | Old data read by a newer revision of its lineage. Always `canonical`: missing trailing optionals are None. |
| `evolution-backward` | 1380 | New data read by an older revision. `unknown` when the writer stored slots the reader does not know, else `canonical`. |
| `pruning` | 212 | Hand-made frames that store a trailing None (rejected by readers that know the field, `unknown` for older readers), plus present-but-empty values (canonical). |
| `malformed` | 419 | Hand-made frames every entry point rejects. Two `canonical` entries document that at four-byte offsets an appended required `u32` is indistinguishable from an appended optional slot. |
| `optional-presence` | 400 | Every presence class, None and Some, standalone and as middle / trailing record fields. |
| `width` | 309 | Type-derived widths, thresholds, caps, mixed parent/child widths, cap overflow. |
| `cap-overflow-explicit-nested` | 15 | A nested value over its type's `maxDynamicData` cap under an explicit-width tag. Recorded as the typed C++ codec behaves: an explicit width enforces the cap of the top-level type only, so these are `canonical` (one is a `reject` for an unrelated reason: the frame does not fit one-byte offsets). The schema-driven codecs of all three languages do the same: they accept `maxDynamicData` under an explicit width and enforce it on the top-level type (`width/cap/...` holds the top-level rejections). |
| `tuple` | 60 | Tuples as positional extensible records, including pruning. |
| `variant` | 48 | One index byte plus the alternative; every index. |
| `enum` | 76 | Enums as their underlying integer. |
| `enum-unknown-value` | 36 | A value that names no enumerator: structurally valid (spec §4.5), accepted in every entry point. C++ enums hold it; the Rust mirror types take it in their `#[pssz(other)]` catch-all variant (a Rust enum without one rejects it, which Rust's own tests pin). |
| `string` | 308 | UTF-8 and bound rules for `string`, `string<=N`, `raw_string`. |
| `string-raw-non-utf8` | 44 | Non-UTF-8 bytes in a `raw_string`: valid PSSZ, recorded as `canonical`, for the typed codecs and for the schema-driven ones alike (the IR tells a raw string, Custom `string`, from a UTF-8 one, Custom `utf8`). The one exception is C++ `validate_strict`, which checks every plain string. Such a value has no JSON / I-JSON text: writing it is an error in every language. |
| `length-bound-overflow` | 32 | Frames exceeding a member `length_bound`, as a writer that skips the bound check would produce them. Every reader rejects them; the C++ and Rust encoders refuse the values. |
| `box` | 100 | Boxed values (`box<T>`: C++ / Rust `Box<T>`). The bytes are T's; a box gives its container no inferred size bound, so `BoxRec` uses 4-byte offsets where `BDirect` uses 1. The IR records the box (Custom `box`) and a schema-only reader derives the same widths. |
| `bytes` | 88 | Byte lists and arrays, bitvector, bitlist, u128/i128/u256. |
| `zero-fixed` | 100 | Zero-field and all-fixed records and their containers. |
| `depth` | 28 | Node depth 63 / 64 accepted, 65+ rejected (`kMaxValidationDepth` = 64). |
| `nested` | 252 | Nested values (`docs/pssz-spec.md` §5.8). Canonical values in every position (top level, record member, nested of nested, optional, list element, variant alternative, tuple element, bounded member, self-nesting chain); the same frame read as the byte-list record (`NActionRaw`) and the byte-list frame read typed; rejections because the inner bytes are not the inner type (not a record, empty, header past the frame, a stored trailing None, a fixed-size inner value with a trailing or missing byte, each position of `NShapes`), a nested member over its `length_bound` — also where the bound is not the inner value's own length: a record (`NCapRec`: `cap_record_at_bound` at 28 bytes, `cap_record_one_over` at 29) and a list of `u16` (`NCapWords`, at most 5 bytes: `cap_words_two` is 4 bytes, `cap_words_three` is 3 elements but 6 bytes, rejected) —, and node depth across the nesting boundary (`depth_63` / `depth_64` accepted, `depth_65` / `depth_66` rejected: the inner root is one level below the nested member). |
| `nested-unknown` | 20 | Frames whose nested bytes were written by a newer inner type (`NArgs2` read as `NArgs`). `unknown` for the typed readers; `reencoded` equals `hex`, because a typed nested value keeps its bytes. A schema-driven reader decodes the inner value, so its default re-encoding is the canonical frame instead: the `hex` of the vector `nested/<value>/as/<type>/<format>` when `value` is set (and in every case a frame that validates as canonical and differs from `hex`); with its "nested bytes are opaque" option it reproduces `reencoded`. `action_as_bytes` is the same frame read by the byte-list record: `canonical`. |
| `nested-deferred` | 24 | `nested<T,deferred>` readers: any inner bytes (valid, garbage, empty, unknown fields) are `canonical`, strict validation included. The Schema IR does not record the policy, so a schema-driven reader gives these outcomes with its "nested bytes are opaque" option (the consumers run this class that way). |
| `map-set` | 266 | Pairs, maps and sets (`docs/pssz-spec.md` §5.9), all `canonical`. Every map / set value appears twice with the same bytes: read as the map / set (`type` = `writer`) and read as the list it is (`type` = the list, `writer` = the map / set), so each language checks that its encoder writes the bytes of the sorted list. Keys: strings, integers (numeric order, not wire-byte order), signed integers, tuples, records, floats including NaNs and `-0`; values: scalars, lists, records, nested maps and sets; an optional set, a map as a variant alternative, maps / sets / a pair as record members (`AssocRec`, whose list spelling is `AssocLists`). |
| `map-set-order` | 352 | Lists that are **not** strictly ascending by key — swapped entries, a repeated key, a repeated key with another value, `+0` with `-0`, a NaN out of place, at the top level and nested in records, optionals, lists, variants and maps. Each appears twice: `reject` as the map / set (`reason`: `pssz: map entries are not in strictly ascending key order` or `pssz: set elements are not in strictly ascending order`) and `canonical` as the list type (`writer`). Encoders cannot produce these frames from a map / set. |
| `transparent` | 160 | Values of transparent types (`Id`, `Blob`, `Ref`, `DerKey`), bare and in lists, arrays, optionals and tuples; every frame is also read as the member's type (`type` is the member's type, `writer` the wrapper): the bytes, the fixedness and the optional form are the member's. Two rejects: `Id` framed as a one-field record, and a short frame. |
| `custom` | 116 | Values of the custom types (a text form, an invariant, or both: `custom-types.md`) and of `Guarded`, which holds each in every position. The bytes are the wire type's. |
| `invariant` | 128 | Structurally valid frames of values that violate an invariant, as a writer that does not check would produce them, bare and nested (list, array, optional, variant alternative, record member, two violations in one record). `expect` is `reject` and `reason` the invariant's message: the outcome of the typed codec — every checked entry point rejects, the trusted decode reads the value, no writer writes it back — and of a schema-driven codec given the handlers. A schema-driven codec with no handler accepts every one of these frames; with a strict registry lacking the handler it refuses the type. |

Custom types in maps, sets and nested values (`Tag`, `map<Tag,u32>`,
`map<Name,Range>`, `set<Level>`, `nested<Range>`; `custom-types.md`, "In maps,
sets and nested values") have rows in `transparent` (`tag/...`: a transparent
string key, read also as `map<raw_string,u32>`), `custom` and `invariant`
(`map/key_name`, `map/value_range`, `map/key_and_value`, `set/element_level`,
`nested/range_reversed`).

Nested values in maps and sets — `map<raw_string,nested<NArgs>>`,
`set<nested<u32>>`, `map<nested<u32>,u8>` — and a map inside nested bytes,
`nested<map<u32,raw_string>>`, have rows in the classes of both features. In
`map-set` / `map-set-order`: the order of a nested key or element is that of
the **bytes of its inner encoding** (`nested<u32>` holding 256, bytes
`00 01 00 00`, precedes the one holding 1), read as the map / set and as the
list of nested values it is. In `nested`: the inner bytes of an entry are
validated as their type (`map_value_inner_not_a_record`,
`set_element_inner_short`, `map_key_inner_trailing_byte`), and a map held in
nested bytes is checked for its order by every reader of the nested value
(`nested_map_by_hand`, `nested_map_unsorted`, `nested_map_duplicate`).

Evolution vector names carry the lineage and its automatic width
(`w4-fixed-string-vector`, `w1-capped`, `w2-capped`, `w1-inferred`, ...) and
the position of the evolving record (`top`, `vector`, `optional`, `variant`,
`tuple`, `array`, `parent`).
