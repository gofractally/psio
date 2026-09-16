# psio

Independent C++23, Rust, and JavaScript/TypeScript serialization packages.
The public C++ implementation defines the interoperability contract.
This checkout is under release preparation; `0.1.0` is a development version.
See [compatibility](docs/compatibility.md) for the exact tested surface and gaps.

## C++

Requires a C++23 compiler, CMake 3.20+, and Boost headers. simdjson is optional;
when found, JSON-to-pjson conversion is enabled and becomes an exported dependency.
On macOS, use Homebrew LLVM rather than Apple Clang.

```sh
CC=/opt/homebrew/opt/llvm/bin/clang CXX=/opt/homebrew/opt/llvm/bin/clang++ \
  cmake -S . -B build -G Ninja -DPSIO_ENABLE_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/psio-install
```

Consumers can use `add_subdirectory`, CMake FetchContent at the repository root,
or `find_package(psio 0.1 CONFIG REQUIRED)` with `CMAKE_PREFIX_PATH` pointing to
an installation. Link `psio::psio`. See [consumer example](examples/consumer).
No psiserve, psitri, or legacy psio checkout is needed.

## Rust

```sh
cd rust
cargo test --workspace --locked
cargo package --workspace --allow-dirty
```

The two crates are `psio` and `psio-macros`. Packaging both together verifies the
unpublished macro dependency against the workspace package. The packaging check
has been exercised with Cargo 1.94.1. For source consumers use a path dependency
on `rust/psio`; both crates must be retained together.

## JavaScript / TypeScript

```sh
cd js
npm ci
npm run build
npm test
npm pack
```

The package exports ESM and TypeScript declarations. Node 20+ is the declared
runtime floor; local verification used Node 25.9.0. The recovered fracpack API
is available at the package root. The pjson codec initializes its XXH3 helper
once, then provides synchronous operations:

```ts
import { createPjson, PjsonDecimal } from 'psio';
const pjson = await createPjson();
const bytes = pjson.encode({ id: 42n, price: new PjsonDecimal(12345n, -2) });
const value = pjson.decode(bytes);
```

Integers decode as `bigint`. Objects decode as ordered `PjsonObject` entries to
preserve duplicate keys. IEEE values, exact decimals, typed arrays and row arrays
have explicit wrapper types. `PjsonString` preserves escaped or non-UTF-8 bytes.
Ordinary JS numbers follow the C++ compact double encoding. This is binary pjson;
it does not implement the separate JSON-text projection API.

## Verification

```sh
CC=/path/to/clang CXX=/path/to/clang++ bash tools/verify-independent.sh /tmp/psio-check
```

The script builds and tests all three packages, checks C++-generated fixtures,
compares validators, installs the C++ library and builds downstream consumers,
and verifies the Rust and npm archives. It verifies the implemented surface;
it is not evidence that every C++ format has been ported to every language.
