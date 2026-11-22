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
  "types":   [ { "id", "ir", "shape", "auto_width", "max_encoded_size",
                 "max_dynamic_data", "fixed", "fixed_size" } ... ],
  "vectors": [ { "name", "class", "type", "format", "hex", "expect",
                 "reason", "reencoded", "value", "writer" } ... ] }
```

`schema_ir` is the `psio::pssz` encoding (lowercase hex) of one Schema IR
(`psio::Schema`) that holds every fixture type under its id. The schema-driven
tests (`cpp/tests/schema_conformance_tests.cpp`,
`rust/psio/tests/schema_conformance.rs`) use it and nothing else: IR + bytes
must give the outcome recorded for the typed codec (see `string-raw-non-utf8`
for the one documented difference).

Types:

- `ir` - whether the Schema IR describes the type faithfully (the emitter
  aborts if an `ir: true` type's IR-derived width differs from `auto_width`).
- `id` - stable type id. A record or enum is a named type and its id is its
  name. Every other type is structural and its id is its shape string.
- `shape` - the logical type (grammar below).
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
  - `reject`: every checked entry point rejects.
- `reason` - for `reject`, the C++ `validate` error message; otherwise empty.
- `reencoded` - for `unknown`, the hex of the reader's re-encoding; otherwise empty.
- `value` - id of the logical value written, or empty for a hand-made frame.
- `writer` - the writer's type id, or empty for a hand-made frame.

Entry points checked by the C++ test: `validate`, `validate_or_throw`,
`validate_strict`, `validate_pssz_compatible`, `decode_pssz_checked`, `decode`
and re-encode identity. The unchecked `psio::decode` is not a validating entry
point: for `reject` vectors it is asserted to throw only for the errors it
shares with the validator (`pssz_conf::decode_also_rejects`). For a bad bool,
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
bitvector<N>  bitlist<N>
enum<u8>{A=0,B=1}   enumerator names and values
record{name:T,...}  extensible record       dwnc{name:T,...}  definition_will_not_change
record{...}@cap=N   explicit maxDynamicData
```

A nested record or enum is referred to by its id; every id referred to is in
`types`. Field names are part of the shape. `<=N` on a `raw_string` or `list`
exists only for record members.

## Classes

204 types, 5167 vectors.

| class | vectors | contents |
|---|---|---|
| `corpus` | 956 | Canonical encodings of named values; `writer` == `type`. |
| `evolution-forward` | 660 | Old data read by a newer revision of its lineage. Always `canonical`: missing trailing optionals are None. |
| `evolution-backward` | 1380 | New data read by an older revision. `unknown` when the writer stored slots the reader does not know, else `canonical`. |
| `pruning` | 212 | Hand-made frames that store a trailing None (rejected by readers that know the field, `unknown` for older readers), plus present-but-empty values (canonical). |
| `malformed` | 419 | Hand-made frames every entry point rejects. Two `canonical` entries document that at four-byte offsets an appended required `u32` is indistinguishable from an appended optional slot. |
| `optional-presence` | 400 | Every presence class, None and Some, standalone and as middle / trailing record fields. |
| `width` | 309 | Type-derived widths, thresholds, caps, mixed parent/child widths, cap overflow. |
| `cap-overflow-explicit-nested` | 15 | OBSERVED ONLY, no expectation. A nested value over its type's cap under an explicit-width tag: C++ checks only the top-level type's cap there. |
| `tuple` | 60 | Tuples as positional extensible records, including pruning. |
| `variant` | 48 | One index byte plus the alternative; every index. |
| `enum` | 76 | Enums as their underlying integer. |
| `enum-unknown-value` | 36 | A value that names no enumerator. C++ accepts it in every entry point. |
| `string` | 308 | UTF-8 and bound rules for `string`, `string<=N`, `raw_string`. |
| `string-raw-non-utf8` | 44 | Non-UTF-8 bytes in a `raw_string`. Recorded as `canonical`; the exceptions are C++ `validate_strict`, which checks every plain string, and the schema-driven codec (the IR has one `string` for raw and UTF-8 strings and treats it as UTF-8): both reject these. |
| `length-bound-overflow` | 28 | Frames exceeding a member `length_bound`, as a writer that skips the bound check would produce them. Every reader rejects them; the C++ and Rust encoders refuse the values. |
| `bytes` | 88 | Byte lists and arrays, bitvector, bitlist, u128/i128/u256. |
| `zero-fixed` | 100 | Zero-field and all-fixed records and their containers. |
| `depth` | 28 | Node depth 63 / 64 accepted, 65+ rejected (`kMaxValidationDepth` = 64). |

Evolution vector names carry the lineage and its automatic width
(`w4-fixed-string-vector`, `w1-capped`, `w2-capped`, `w1-inferred`, ...) and
the position of the evolving record (`top`, `vector`, `optional`, `variant`,
`tuple`, `array`, `parent`).
