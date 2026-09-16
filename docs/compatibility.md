# Current compatibility and release gates

Status recorded during the September 16, 2026 recovery work. Passing the checks
below does not certify the whole library as stable. C++ is the authority.

| Format | C++ public fixture | Rust comparison | JS/TS comparison |
|---|---|---|---|
| frac32 / fracpack | records, nesting, variants, empty values, scalar/string/vector/optional | encode/decode | encode/decode |
| pjson | scalars, 274 doubles, typed containers, keys, rejection cases | encode/decode/validate | encode/decode/validate |
| SSZ | two records | encode/decode | implementation absent |
| PSSZ | two records | encode/decode | implementation absent |
| Cap'n Proto | two records | encode/decode | implementation absent |
| FlatBuffers | two records | encode/decode | implementation absent |
| WIT binary | two records | encode only in shared fixture test | implementation absent; WIT text generation is separate |
| frac16 | two records | not covered | implementation absent |
| bin, Borsh, bincode, Avro, key, BSON, MessagePack, Protobuf | two records each | public codecs not implemented | public codecs not implemented |
| JSON text | two records | conversion APIs exist; not compared here | conversion APIs exist; not compared here |

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
   This changes bytes from the recovered legacy vector encoder; migration from
   legacy stored data still needs an explicit compatibility policy.
  Pjson revision 2 changes earlier C++ bytes. Version selection must be external;
  automatic detection is unsafe (`35` changes from +5 to -5). The former Rust
  draft shares the new numbering but includes additional unsupported features.
- Retained applicable pre-existing uncommitted Rust fixes. The pre-edit patch is
  saved in the psiserve `outputs/psio-release-work-2026-09-16` directory.

## Still required for a stable release

1. Complete the missing format implementations if the release promises full C++
   format parity. The current package surface is visibly different by language.
2. Expand shared schema coverage: nested structs, variants, optional fields,
   schema evolution, resources, views and in-place mutation. Two records cannot
   establish the full Rust codecs' compatibility.
   Define the binding for arbitrary C++ `std::string` bytes as well: C++ fracpack
   validates their length without requiring UTF-8, Rust `String` requires UTF-8,
   and the current JS `str` decoder replaces malformed UTF-8. Raw byte bindings
   and shared fixtures are needed before claiming lossless parity for that case.
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
