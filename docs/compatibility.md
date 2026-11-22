# Current compatibility and release gates

Status recorded during the September 16, 2026 recovery work. Passing the checks
below does not certify the whole library as stable. C++ is the authority.

pSSZ is the primary schema-driven binary format.
Fracpack is an experimental format (`psio::experimental`, `psio/frac.hpp`),
not a supported wire format. It is kept to measure one design trade-off against
pSSZ: fracpack stores a size with every variable-size value, pSSZ stores none
and infers each size from the offsets around the value. It is not
byte-compatible with psibase's fracpack in general.
Known differences from psibase's fracpack: `std::tuple` is not supported, and
`std::vector<std::optional<std::string>>` encodes differently. In C++ the
tags are `psio::experimental::frac32` and `psio::experimental::frac16`, and
`<psio/psio.hpp>` does not include them. The Rust crate still exports its
fracpack traits and derives (`Pack`, `Unpack`, `Fracpack`, `FracView`) at the
crate root; the JS package has no fracpack codec. The fracpack rows below
describe benchmark implementation coverage.

| Format | C++ public fixture | Rust comparison | JS/TS comparison |
|---|---|---|---|
| frac32 / fracpack | records, nesting, variants, empty values, scalar/string/vector/optional | encode/decode | removed: the JS package implements the current formats only |
| pjson | scalars, 274 doubles, typed containers, keys, rejection cases | encode/decode/validate | encode/decode/validate |
| SSZ | two records | encode/decode | implementation absent |
| pSSZ | two records in the public corpus; `conformance/pssz-vectors.json` (generated from the C++ codec, checked by ctest) | encode/decode/validate/views against the whole PSSZ fixture, for `pssz_struct!` records and derives | encode/decode/validate/views against the whole PSSZ fixture, schema-driven and through typed definitions (`js/README.md`) |
| Cap'n Proto | two records | encode/decode | implementation absent |
| FlatBuffers | two records | encode/decode | implementation absent |
| WIT binary | two records | encode only in shared fixture test | implementation absent; WIT text from a Schema IR is `emitWit` |
| frac16 | two records | not covered | implementation absent |
| bin, Borsh, bincode, Avro, key, BSON, MessagePack, Protobuf | two records each | public codecs not implemented | `key` implemented (`conformance/time-key-vectors.json`); the others not implemented |
| JSON / I-JSON text | two records in the public corpus; `conformance/json-vectors.json` (990 vectors generated from the C++ codec, checked by ctest; `docs/json.md`) | encode/decode/validate against the whole JSON fixture | encode/decode against the whole JSON fixture, schema-driven and through typed definitions |

The standalone packages now build without psiserve. Installed CMake and
FetchContent consumers pass. Rust archive verification includes the macro crate
and schema fixtures inside the package. JS packaging includes compiled ESM and
declarations, not test code.

## What changed

- Recovered the TypeScript package from `psiserve/libraries/psio1/js`.
- Added root CMake configuration, namespaced target, install/export support.
- Adopted pjson wire revision 2: audited high-nibble assignments and negative-inline
  encoding across C++, Rust and JS/TS; compact floating-point policy follows C++.
- Corrected Rust FlatBuffers field order/empty strings and WIT empty pointers.
- Added the JS binary pjson codec and shared public-library fixtures.
- Repaired C++ half-float validation and an out-of-bounds row-key read; added
  decode depth bounds, row offset checks, and signed-minimum arithmetic fixes.
- Bounded Rust parsing before recursion and fixed signed i128 decode bounds.
- Repaired C++ fracpack validation of nested records and variable vectors followed
  by siblings. Rust/JS vector encoding now follows current C++ complete-element
  encoding (including empty strings and optional elements), with matching views.
  Rust/JS now also reject legacy zero/one vector-slot sentinels that the current
  C++ validator rejects; eight shared malformed fixtures cover those offsets.
  These changes to the legacy prototype are recorded for benchmark reproducibility.
  Pjson revision 2 changes earlier C++ bytes. Version selection must be external;
  automatic detection is unsafe (`35` changes from +5 to -5). The former Rust
  draft shares the new numbering but includes additional unsupported features.
- Retained applicable pre-existing uncommitted Rust fixes. The pre-edit patch is
  saved in the psiserve `outputs/psio-release-work-2026-09-16` directory.

## PSSZ: Rust and C++

`conformance/pssz-vectors.json` is emitted by `cpp/tests/pssz_conformance_emit.cpp`
from the C++ implementation and consumed by `cpp/tests/pssz_conformance_tests.cpp`
(ctest, which also fails if the checked-in file differs from a fresh emission)
and `rust/psio/tests/pssz_conformance.rs` (cargo test). Each vector names a
type id that both languages map to a concrete type, a format (`auto`,
`pssz8`, `pssz16`, `pssz32`), the bytes, and the outcome: valid and canonical,
valid with unknown fields (and the reader's re-encoding), or rejected. Rust
must decode and re-encode every valid vector to identical bytes and reject
every rejected one, and it checks the fixture's width / bound / cap / fixed
manifest for every type. See `conformance/pssz-vectors.md` for the classes.

The fixture has 5167 vectors over 204 types. Rust reaches the C++ outcome on
5131 of them. The other 36 record a deliberate difference of type design, and
the Rust test asserts that Rust rejects exactly those:

| Difference | C++ | Rust |
|---|---|---|
| Enum value that is not an enumerator (`enum-unknown-value`) | accepted: an enum is any value of its underlying integer, so a reader keeps accepting data from a writer that added enumerators. `pssz-spec.md` 4.5 (`enum_out_of_range`) says a decoder rejects it, so C++ and the spec disagree here | rejected, as the spec says: a Rust enum cannot hold it |

A member `length_bound` is enforced on read in both languages: the 28
`length-bound-overflow` vectors and the four `width/cap/checkpoint_at_cap`
frames are rejections in the fixture. C++ `validate`, `validate_strict` and
the checked readers built on them (`decode_pssz_checked`,
`validated_pssz_view`) reject a string or list member outside its declared
`max` / `exact` / `min`, also through `std::optional`. The C++ encoder
checks it too (`encode` raises `codec_exception`, or aborts without
exceptions; `encode_pssz_checked` returns a status and leaves the sink
unchanged), as does the fail-fast `psio::decode` (the same exception /
abort for a member outside its bound). Rust `BoundedRawString<N>` /
`BoundedList<T, N>` check it on encode and decode as well.

Other differences that remain. None changes the bytes of a value both
languages can represent.

- **Unvalidated C++ `decode`.** C++ `decode` is a fail-fast path that assumes
  `validate` ran: on its own it accepts any nonzero bool, does not check
  sequence offset tables, bit padding or variant indices, and under the
  explicit tags does not apply the `maxDynamicData` cap. Rust has no such
  path: `from_pssz` accepts exactly what C++ `validate` accepts.
- **Strict validation.** C++ `validate_strict` also materializes the value,
  runs member annotation specs and compares the canonical re-encoding. The
  Rust exact readers reject unknown fields and non-UTF-8 raw strings;
  canonical comparison is `validate_canonical_for_profile`; Rust has no
  member annotations.
- **Schema for raw strings.** A raw string is `"string"` in the schema in
  both languages: C++ `std::string` and Rust `RawString` /
  `BoundedRawString<N>` emit the same `"string"` adapter over a byte list as
  the UTF-8 types (C++ `utf8_string<>`, Rust `String`). Byte lists and arrays
  stay `"hex"`. The emitted schema carries no marker that distinguishes a
  raw string from a UTF-8 string, and both schema-driven codecs UTF-8-check
  every `"string"`: they reject a raw string holding non-UTF-8 bytes that
  the typed codecs pass through. Whether the schema should mark raw strings
  is open.
- **Schema-driven codec.** Rust `pssz::dynamic` supports only the
  explicit-width tags; it rejects the automatic profile with an error.
- **Rust-only restrictions.** No member-level annotations or `as<Tag>`
  adapters; tuples up to 12 elements; `pssz_struct_dwnc!` requires `Copy`
  fields and a directly recursive `pssz_struct!` record has no validator (use
  the derives for both); a directly recursive type has no const `SHAPE`, so
  `pssz_evolves_from` cannot be evaluated for it.
- **Explicit tags and nested caps.** Under `pssz8/16/32` both languages check
  only the top-level type's cap, so a nested value over its own cap is
  accepted (fixture class `cap-overflow-explicit-nested`); the automatic
  profile rejects it at every level in both.
- **Appended required fields.** At four-byte offsets a reader whose schema
  appended a required `u32` cannot tell it from an appended optional slot and
  reads such a frame as valid in both languages (two `canonical` entries in
  the `malformed` class). Only the schema check (`pssz_evolves_from`) prevents
  that upgrade.
- **Rust-only types.** `Vec<bool>` works in Rust; C++ pssz cannot encode
  `std::vector<bool>`.
- **C++-only types.** `psio::bounded<T, N>`, `byte_array<N>` and
  `std::monostate` are not PSSZ types in C++ and have no pairing. A Rust unit
  enum alternative is a zero-field fixed-layout record alternative in C++.
- JavaScript / TypeScript implements PSSZ schema-driven (`js/src/pssz-core.ts`); its typed
  definitions generate the Schema IR of the C++ schema builder.

## Still required for a stable release

1. Complete the missing format implementations if the release promises full C++
   format parity. The current package surface is visibly different by language.
2. Expand shared schema coverage: nested structs, variants, optional fields,
   schema evolution, resources, views and in-place mutation. Two records cannot
   establish the full Rust codecs' compatibility.
   Define the binding for arbitrary C++ `std::string` bytes as well: C++ fracpack
   validates their length without requiring UTF-8 and Rust `String` requires UTF-8
   (the JS package no longer has a fracpack codec). Raw byte bindings
   and shared fixtures are needed before claiming lossless parity for that case.
   For PSSZ this is settled: Rust `String` pairs with C++ `psio::utf8_string`
   and Rust `RawString` with C++ `std::string`, with shared fixtures.
3. Decide and enforce numeric/API limits. C++'s dynamic pjson value uses an i128
   mantissa and cannot faithfully represent the entire wire u128 range. Several
   encoders still need overflow/count rejection and consistent error contracts.
4. Reconcile pjson JSON-text projection, canonical validation and public policy
   behavior. Binary fixture agreement does not settle these APIs.
   Repair the advertised exception-disabled build as well: headers still refer
   to `codec_exception` when its definition is disabled, and contain unguarded
   throws. A guest-compiler integration probe for the C++ consumer compiles with
   `PSIO_EXCEPTIONS_ENABLED=1`, but cannot link against this bootstrap's WASI
   SDK because `__cxa_allocate_exception` / `__cxa_throw` are unavailable. A
   tested exception-free psio profile (or a separately supported Wasm exception
   toolchain/runtime) is needed for that integration.
5. Run the new independent checks on Linux/x86_64 and supported toolchain/runtime
   versions; only local macOS/ARM64 results have been observed so far. Add sustained
   sanitizer/fuzz jobs across all formats before accepting untrusted inputs.
6. Supply the intended first-party license/notice files (manifests currently say
   MIT), review third-party notices, freeze versions and publish immutable tags.
   No artifacts have been published by this work.

Step 2's full LLVM compiler bootstrap remains separate from these library gates.

## Additional findings from the tag migration checks

- C++ `pjson_json::from_json` currently rejects a top-level scalar at
  `doc.get_value()` (`pjson_json: doc.value`). The migration checks cover numbers
  inside JSON objects and scalar JSON output; root-scalar ingress needs a fix.
- A broader pjson ASan/UBSan run reports pre-existing unaligned loads in
  `detail/find_byte.hpp` and typed-array span access. Wire revision 2 does not
  resolve that API/alignment issue. The focused tag migration tests are checked
  separately with sanitizers; do not report the whole pjson view surface as
  sanitizer-clean.
