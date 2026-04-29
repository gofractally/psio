# psio dependency pattern

psio is **standalone-buildable**. A `git clone` + `cmake -B build &&
cmake --build build` works on any host with a C++23 compiler, system
Boost headers, and (optionally) simdjson — no other org repos in the
include path, no submodules, no fetch step.

This doc lists every dep the library and its tests / benches reach,
explains *which category* each falls in, and writes down the
discipline for adding a new one. It mirrors the convention captured at
[`psitri-multiindex/docs/dependency-pattern.md`][1] but is keyed to
psio's actual surface.

[1]: https://github.com/gofractally/psitri-multiindex/blob/main/docs/dependency-pattern.md

## 1. Three categories

| Category | When | How psio gets it |
|---|---|---|
| **Vendored** | Single-header (or near-single-file) stable 3rd-party where we want zero configure-time friction | Copy under `external/<name>/` with a one-line provenance comment at the top of each file |
| **`find_package`** | Stable 3rd-party where consumers reasonably already have it installed (system / vcpkg / Conan / Homebrew) | `find_package(<Name> [QUIET])` in `CMakeLists.txt`; `target_link_libraries(psio PUBLIC ...)` when found, compile-time gate with `PSIO_HAVE_<NAME>` define |
| **`FetchContent`** | Internal libraries we co-edit (no current psio consumers fall here yet) | `FetchContent_Declare(... SOURCE_DIR external/<name> ...)`; first-declare-wins lets a parent project override pins |

The categories shake out cleanly because psio sits at the leaves of
the dep graph — it pulls in stable 3rd-party but has zero internal
stack deps of its own. (Consumers like `psiserve`, `psizam`,
`psitri-multiindex` reach psio, not the other way around.)

## 2. Concrete catalog

### Required

| Dep | Category | Where | Why |
|---|---|---|---|
| C++23 stdlib | toolchain | compiler | `<expected>`, `<span>`, `<concepts>`, `std::is_arithmetic_v`, ranges-style |
| Boost.Preprocessor | `find_package(Boost REQUIRED)` | system | `BOOST_PP_*` for `PSIO_REFLECT(...)` codegen — header-only, `Boost::headers` target only |
| xxhash 0.8.2 | vendored | `external/xxhash/xxhash.h` | `XXH_INLINE_ALL` single-header used by pjson key prefilter and psch schema digest |
| `find_byte` | vendored | `include/psio/detail/find_byte.hpp` | SWAR 8-byte byte search — copy of `ucc::find_byte` from psitri's small-utility lib (one symbol, ~50 lines) |
| Catch2 v2 (tests only) | vendored | `external/catch2/catch.hpp` | Test framework. Single-header v2.7.2; v3 split into compiled units, deliberately not used |

### Optional (per-feature)

| Dep | Category | Define | Effect when present |
|---|---|---|---|
| simdjson | `find_package(simdjson QUIET)` | `PSIO_HAVE_SIMDJSON=1` | `psio::pjson::from_json` (JSON text → pjson binary) |
| libflatbuffers | `find_package(flatbuffers QUIET)` (in tests) | — | Enables `psio::flatbuf_lib` parity tests |
| msgpack-cxx | `find_package(msgpack-cxx QUIET CONFIG)` (in benches) | `PSIO_HAVE_MSGPACK` | `psio_format_perf_external` head-to-head bench |
| libcapnp | `find_package(CapnProto QUIET)` (in benches) | `PSIO_HAVE_CAPNP` | capnp adapter column in `psio_bench_vs_externals` |
| libprotobuf | `find_package(Protobuf QUIET)` (in benches) | `PSIO_HAVE_PROTOBUF` | protobuf adapter column in `psio_bench_vs_externals` |

When optional deps are absent the build proceeds and the affected
features compile out via `#ifdef`s; psio's own format tags still work.

## 3. Why vendor in some cases and `find_package` in others

The `psitri-multiindex` doc argues the diamond-resolution case for
*internal* libraries via FetchContent. Same instinct applies to *3rd
party* — but the analysis is different because nobody else owns
xxhash, Catch2, etc. as something we want a parent project to pin
override.

The split we land on:

- **Vendor** when a single-header dep is small enough that the cost of
  shipping the bytes is less than the cost of asking every consumer to
  install it. xxhash (~3 KLOC), find_byte (~50 lines), Catch2 (~15
  KLOC) all fit this. Vendoring trades disk for "git clone & build
  works everywhere." Provenance comments at the top of each vendored
  file prevent the file from drifting from upstream silently.
- **`find_package`** when the dep is reasonably common in the
  ecosystem (Boost, simdjson, flatbuffers, capnp, protobuf,
  msgpack-cxx) and the consumers we have / expect already have it
  installed via Homebrew / apt / vcpkg / Conan. Friction cost of
  configure-time fetch is higher than expecting the install.
- **FetchContent** would apply to internal libraries we co-develop.
  None today, but the placeholder pattern below is what would be added
  when one appears.

## 4. Adding a new dep — which category?

Decision tree:

1. **Is it a single-header library?** → Vendor it under `external/<name>/<file>.h`. Add a one-line provenance comment naming the upstream URL + version / commit. No CMake fetch step required.
2. **Is it a stable, ecosystem-common multi-file library?** (Boost, simdjson, capnp, protobuf, etc.) → `find_package(<Name> QUIET)` with a `PSIO_HAVE_<NAME>` gate. Either feature compiles out cleanly when absent, or it's marked REQUIRED if psio cannot work without it (today only Boost is REQUIRED).
3. **Is it an internal library we own and co-edit?** → `FetchContent_Declare(... SOURCE_DIR external/<name> ...)` and let parent projects (`psiserve`, `psizam`, etc.) override the pin. Pattern as in [`psitri-multiindex/docs/dependency-pattern.md`][1] §6.
4. **Is it test-only and small?** → vendor under `external/<name>/`, gate behind `PSIO_ENABLE_TESTS`. Catch2 fits this case.
5. **Is it large, compiled, or platform-specific?** → `find_package`; do NOT vendor.

## 5. Build-time discipline

- `external/` is part of the source tree — vendored content lives here
  and IS tracked in git. (Distinct from the `psitri-multiindex` doc's
  convention where `external/` is *gitignored* because it holds
  FetchContent populations. Psio doesn't use FetchContent today, so
  `external/` is purely for hand-vendored single-header deps.)
- Provenance comments live in the vendored files themselves — not in
  a sidecar manifest. `grep -rn "Vendored from" external/` enumerates
  them. Bumping a vendored file is a one-shot edit + provenance update;
  the diff is reviewable.
- New `find_package`-acquired optional deps get a `PSIO_HAVE_<NAME>`
  define and a `message(STATUS "psio: <name> ... ")` line in `CMakeLists.txt`
  so configure output makes the matrix visible.

## 6. Consuming psio from a parent project

Three patterns, increasing degrees of integration:

### 6a. As a vendored / cloned subdirectory

```cmake
# parent/CMakeLists.txt
add_subdirectory(external/psio)   # path of your choosing
target_link_libraries(my_target PRIVATE psio)
```

Works under any acquisition method — submodule, FetchContent, plain
clone, copy. psio's own `CMakeLists.txt` brings in `Boost::headers`
and the optional `simdjson::simdjson` automatically.

### 6b. Via FetchContent (recommended for stack-of-libraries setups)

```cmake
include(FetchContent)
FetchContent_Declare(psio
   GIT_REPOSITORY https://github.com/gofractally/psio.git
   GIT_TAG        main           # or a specific SHA
   SOURCE_DIR     ${CMAKE_SOURCE_DIR}/external/psio)
FetchContent_MakeAvailable(psio)
target_link_libraries(my_target PRIVATE psio)
```

This matches the convention in
[`psitri-multiindex/docs/dependency-pattern.md`][1]. Set
`FETCHCONTENT_SOURCE_DIR_PSIO=$HOME/dev/psio` to override with a local
checkout for in-development edits.

### 6c. Via system `find_package` (future)

Not yet exported. When psio installs a `psioConfig.cmake`, this lands.
Open issue:
[`psio-standalone-dependencies.md`](../../.issues/psio-standalone-dependencies.md)
("Nice-to-have").

## 7. License posture

Every vendored file carries the upstream license verbatim:

- `external/xxhash/xxhash.h` — BSD 2-clause (Yann Collet)
- `external/catch2/catch.hpp` — Boost Software License 1.0 (Catch2)
- `include/psio/detail/find_byte.hpp` — relicensed under psio's terms
  with attribution comment to psitri's `ucc::find_byte`

When adding a vendored dep, the license must be permissive (Boost
License, BSD, MIT, Apache 2). Anything copyleft (GPL, LGPL) is a
no-go for a header-only vendor.
