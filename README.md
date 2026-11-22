# psio

Independent C++23, Rust, and JavaScript/TypeScript serialization packages.
The public C++ implementation defines the interoperability contract.
This checkout is under release preparation; `0.1.0` is a development version.
See [compatibility](docs/compatibility.md) for the exact tested surface and gaps.

pSSZ is psio's primary schema-driven binary format.
Fracpack is an experimental format (`psio::experimental`, `psio/frac.hpp`),
not a supported wire format. It is kept to measure one design trade-off against
pSSZ: fracpack stores a size with every variable-size value, pSSZ stores none
and infers each size from the offsets around the value. It is not
byte-compatible with psibase's fracpack in general.
See the [format comparison](docs/format-comparison.md) for the measurements.

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

Each format has one set of names; this is pSSZ:

```cpp
#include <psio/psio.hpp>

struct Account { std::uint64_t balance = 0; std::string owner; };
PSIO_REFLECT(Account, balance, owner)

std::vector<char> bytes = psio::to_pssz(account);        // encode
Account back = psio::from_pssz<Account>(bytes);          // validate, then decode
psio::pssz_view<Account> view{bytes};                    // read without decoding
psio::shared_pssz<Account> shared{std::move(bytes)};     // bytes with several owners
```

Bounds are annotations on the reflection, not changes to the types: a
`std::vector<std::string>` stays one, and its `attr` says how large it may get.
Bounded members give a record narrow offsets, and every codec enforces them.

```cpp
struct Post {
   std::string              title;
   std::vector<std::string> tags;
   PSIO_REFLECT(Post, attr(title, utf8 | max<64>),
                attr(tags, max<8> | element(utf8 | max<16>)))   // <= 8 tags of <= 16 bytes
};
static_assert(psio::auto_pssz_width_v<Post> == 1);              // 1-byte offsets
```

`psio::bounded<T, N>` and `psio::utf8_string<N>` state the same bounds in the
type, with the same bytes and schema; methods take the same annotations and a
cap for their arguments (`method(transfer, to, attr(memo, max<80>),
maxDynamicData(256))`). See
[bounds by annotation](docs/psio-overview.md#bounds-by-annotation).

The same names exist for the other formats (`to_json`, `from_key<T>`,
`ssz_view<T>`, …). A view only reads; the view that writes is spelled
`pssz_mutable_view<T>`. `from_F<T>(bytes)` validates before it decodes;
`from_F<T>(psio::trusted{bytes})` skips the validation for bytes already known
to be valid, and `validate_F<T>(bytes)` returns a status without decoding.
[The helper names](docs/helpers.md) lists them for every format, with the
generic layer (`psio::encode(fmt, v)`, `psio::view<T, Fmt, Store>`) they are
built on.

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

The package exports ESM and TypeScript declarations and has no runtime
dependencies. Node 20+ is the declared runtime floor. It implements pSSZ, the
sort-preserving key format, JSON and I-JSON from a schema or from typed
declarations, with read views over encoded bytes; see `js/README.md`. The
pjson codec is synchronous:

```ts
import { pjsonEncode, pjsonDecode, PjsonDecimal } from 'psio';
const bytes = pjsonEncode({ id: 42n, price: new PjsonDecimal(12345n, -2) });
const value = pjsonDecode(bytes);
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
