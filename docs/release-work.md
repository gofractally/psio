# Release work

The release baseline is the public C++ implementation. Rust and
JavaScript/TypeScript must produce compatible bytes and accept the same
supported encodings. Pjson now adopts the audited tag numbering as wire revision 2; see
`wire-contract.md`. C++ is the reference for that agreed contract.

## Step 1: independent psio

- [x] Recover the existing TypeScript package from psiserve's legacy tree.
- [x] Support root CMake configuration, source consumption, and an installed
  `psio::psio` package without psiserve or psitri.
- [x] Repair the C++ binary16 validator/decoder inconsistency; all 51 CTest
  targets pass in a fresh Release build on macOS ARM64.
- [x] Generate fixtures using public C++ codecs: currently 366 fixtures spanning
  17 formats plus numeric, typed-container, and rejection groups, rather than a separate reference encoder.
- [x] Adopt the improved pjson tag numbering in C++, Rust and JS/TS, including
  negative-inline integers, views and JSON output. Retain independent safety fixes.
- [ ] Expand the existing Rust and TypeScript fixture coverage, including
  decode, encode, views where supported, malformed input, and boundaries.
- [ ] Complete the missing language/format implementations. The recovered
  TypeScript package now implements fracpack, pjson and JSON conversion; its WIT
  module generates interface definitions, rather than implementing a WIT
  binary codec. Do not claim full parity from its passing 417 tests.
- [x] Make Rust and npm package verification independent of source files
  outside their packages, and test packaged artifacts.
- [x] Mark conflicting draft documentation as historical; add reproducible
  independent verification and an explicit compatibility matrix.

## Step 2: independent psizam and compiler bootstrap

After Step 1, make the engine independently consumable with explicit,
pinned dependencies and reproduce this chain:

1. Build the pinned LLVM C++ source into the initial `clang.wasm` and
   `wasm-ld.wasm` using the documented host toolchain.
2. Compile these Wasm tools into PZAM artifacts using a supported backend.
3. Run `clang.pzam` (and the corresponding linker) to compile the same
   LLVM C++ sources into a second `clang.wasm`.
4. Run that compiler and repeat the stage as needed to compare artifacts
   under reproducible build settings. Record toolchain/sysroot hashes,
   exact commands, resource requirements, and functional checks.

A successful hello-world or `--version` run alone is a preliminary check. The
independent engine checkout at `/Users/dlarimer/gofractally/psizam` now passes
the full functional compiler bootstrap: its PZAM tools rebuild LLVM, and the
rebuilt tools compile/link a C++ probe that runs successfully. A byte-identical
fixed point and a portable pinned toolchain bundle remain unverified. See that
checkout's `docs/compiler-bootstrap.md` for the commands and evidence.

Existing uncommitted changes were snapshotted before edits in
`/Users/dlarimer/psiserve/outputs/psio-release-work-2026-09-16/`.
Build and proposal-test logs are under `/tmp/psio-stable-work/`.

## Verified checkpoint

The independent verification stages pass on macOS ARM64 after the revision 2
fixture updates (the npm consumer was rerun separately after its old tag
assertion was updated):
51 CTest targets, 676 Rust library tests plus 14 public conformance tests and
16 doctests (historical draft driver excluded), 424 JS tests, 11,252 differential
validation inputs, CMake source/install consumers, both Cargo archives and an
npm archive consumer. Separate address/undefined sanitizer validation passes.
The updated fracpack and structural-validation C++ test targets also pass in
a fresh build with both AddressSanitizer and UndefinedBehaviorSanitizer enabled
(`sanitizer-{configure,build,tests}.log` in the same work directory).
See `docs/compatibility.md` for the outstanding release gates.

The tag migration additionally passes 382 focused C++ assertions with ASan/UBSan
and `halt_on_error=1`, covering dynamic/typed encoding, views, JSON object
conversion, and rejection. A broader pjson sanitizer attempt found existing
alignment defects and root-scalar JSON parsing trouble; see `compatibility.md`.
Current evidence is retained under psiserve's
`outputs/tag-and-pzam-fixes-2026-09-16/`.
