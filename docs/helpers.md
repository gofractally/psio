# The C++ helper names

Most code names one format at each call. For that there is one set of names
per format, all in namespace `psio`, declared by one line in the format's
header:

```cpp
PSIO_FORMAT_HELPERS(pssz)   // cpp/include/psio/pssz.hpp
```

```cpp
#include <psio/pssz.hpp>

struct Account { std::uint64_t balance = 0; std::string owner; };
PSIO_REFLECT(Account, balance, owner)

std::vector<char> bytes = psio::to_pssz(account);       // encode
Account back = psio::from_pssz<Account>(bytes);         // validate, then decode
if (auto st = psio::validate_pssz<Account>(bytes); !st.ok())
   report(st.error().what);                             // status only
std::size_t n = psio::pssz_size(account);               // size of the encoding
```

Views and typed buffers (`#include <psio/pssz_view.hpp>` for the read view,
`<psio/mutable_view.hpp>` for the mutable one, or `<psio/psio.hpp>` for both):

```cpp
psio::pssz_view<Account> r{bytes};           // reads borrowed bytes
std::string_view owner = r.get<1>().view_();

psio::pssz_mutable_view<Account> w{bytes};   // writes into borrowed bytes
w.get<0>().set(std::uint64_t{42});

psio::pssz_buffer<Account> owned{psio::to_pssz(account)};   // owns its bytes
psio::shared_pssz<Account> tx{psio::to_pssz(account)};      // shared ownership
psio::shared_pssz<Account> other = tx;                      // same bytes, no copy
auto v = psio::as_view(other);                              // read view of them
```

## The names

For a format `F`:

| Name | Meaning | It is |
|------|---------|-------|
| `to_F(v)` | encode into a fresh buffer | `psio::encode(psio::F{}, v)` |
| `to_F(v, sink)` | encode into an existing buffer | `psio::encode(psio::F{}, v, sink)` |
| `from_F<T>(bytes)` | validate, then decode | `psio::validate<T>(psio::F{}, bytes)`, `psio::codec_abort` on failure, then `psio::decode<T>(psio::F{}, bytes)` |
| `from_F<T>(psio::trusted{bytes})` | decode without the validation pass | `psio::decode<T>(psio::F{}, bytes)` |
| `validate_F<T>(bytes)` | `psio::codec_status`; never throws or aborts | `psio::validate<T>(psio::F{}, bytes)` |
| `F_size(v)` | size of the encoding of `v` | `psio::size_of(psio::F{}, v)` |
| `F_view<T>` | read-only view over borrowed bytes | `psio::view<T, psio::F>` |
| `F_view<const T>` | the same type as `F_view<T>` | `psio::view<T, psio::F>` |
| `F_mutable_view<T>` | view that writes into borrowed bytes | `psio::mutable_view<T, psio::F>` |
| `F_buffer<T>` | typed bytes, owned | `psio::buffer<T, psio::F, psio::storage::owning>` |
| `shared_F<T>` | typed bytes, reference-counted and immutable | `psio::buffer<T, psio::F, psio::storage::shared>` |

`bytes` is a `std::span<const char>`; a `std::vector<char>`, `std::string`,
`std::string_view` or `std::array<char, N>` converts. The text formats also
take a string literal: `psio::from_json<T>(R"({"id":7})")`.

A name exists only where the format has the operation. Using a missing one
is a constraint failure that names it, for example
`constraints not satisfied for alias template 'json_view'`.

### Per format

| `F` | header | `to_F(v)` returns | `to_F(v, sink)` | `from_F`, `validate_F`, `F_size` | `F_view<T>` | `F_mutable_view<T>` |
|-----|--------|-------------------|-----------------|----------------------------------|-------------------|-------------|
| `pssz` | `psio/pssz.hpp` | `std::vector<char>` | appends to `std::vector<char>` | yes | yes (`psio/pssz_view.hpp`) | yes (`psio/mutable_view.hpp`) |
| `ssz` | `psio/ssz.hpp` | `std::vector<char>` | appends to `std::vector<char>` | yes | yes (`psio/ssz_view.hpp`) | no |
| `bin` | `psio/bin.hpp` | `std::vector<char>` | appends to `std::vector<char>` | yes | no | no |
| `borsh` | `psio/borsh.hpp` | `std::vector<char>` | appends to `std::vector<char>` | yes | no | no |
| `bincode` | `psio/bincode.hpp` | `std::vector<char>` | appends to `std::vector<char>` | yes | no | no |
| `key` | `psio/key.hpp` | `std::array<char, N>` for a key of fixed size, else `std::vector<char>` | appends to `std::vector<char>`; writes to any psio stream | yes | no | no |
| `json` | `psio/json.hpp` | `std::string` | appends to `std::string` | yes | no | no |
| `ijson` | `psio/json.hpp` | `std::string` | appends to `std::string` | yes | no | no |
| `msgpack` | `psio/msgpack.hpp` | `std::vector<char>` | appends to `std::vector<char>` | yes | no | no |
| `avro` | `psio/avro.hpp` | `std::vector<char>` | appends to `std::vector<char>` | yes | no | no |
| `bson` | `psio/bson.hpp` | `std::vector<char>` | appends to `std::vector<char>` | yes | no | no |
| `protobuf` | `psio/protobuf.hpp` | `std::vector<char>` | appends to `std::vector<char>` | yes | no | no |
| `capnp` | `psio/capnp.hpp` | `std::vector<char>` | **replaces** the content of a `std::vector<char>` | yes | no | no |
| `flatbuf` | `psio/flatbuf.hpp` | `std::vector<char>` | **replaces** the content of a `std::vector<char>` | yes | no | no |
| `flatbuf_lib` | `psio/flatbuf_lib.hpp` | `std::vector<char>` | no | yes | no | no |
| `wit` | `psio/wit.hpp` | `std::vector<char>` | no | yes | yes (`psio/wit_view.hpp`) | no |

`F_buffer<T>` and `shared_F<T>` exist for every format.

The expansion of `to_F(v)` differs from the table for one format: `to_key(v)`
is `psio::key::to_fresh(v)`, which returns a `std::array` on the stack when
the size of the key is a compile-time constant. `psio::encode(psio::key{}, v)`
gives the same bytes in a `std::vector<char>`.

Fracpack is not in the table: it is an experimental format
(`psio::experimental::frac32`, `psio/frac.hpp`), not a supported wire format,
and has no helper names.

pjson is not in the table. It has no format tag in the generic layer; its
entry points are in `psio/pjson.hpp` and `psio/pjson_typed.hpp`.

A value outside the bounds its type declares — a member `max<N>`, the bounds
of its elements, keys and values (`element` / `keys` / `values` / `at<I>`), a
`psio::bounded` or `psio::utf8_string`, a `maxDynamicData` cap — has no
encoding: `to_F` throws, and `from_F` / `validate_F` refuse bytes that hold
one. See [bounds by annotation](psio-overview.md#bounds-by-annotation).

## Checked and trusted decode

`from_F<T>(bytes)` is the checked decode. It returns a `T`, or it does not
return: on bytes that `validate_F<T>` rejects it calls `psio::codec_abort`
with the validator's error, which

- throws `psio::codec_exception` in a build with exceptions
  (`PSIO_EXCEPTIONS_ENABLED=1`, the default), and
- calls `std::abort` in a build without them (`PSIO_EXCEPTIONS_ENABLED=0`,
  `-fno-exceptions`; a WebAssembly guest).

Code that must handle bad input without exceptions asks first, then decodes
what was accepted:

```cpp
if (auto st = psio::validate_pssz<Account>(bytes); st.ok())
   use(psio::from_pssz<Account>(psio::trusted{bytes}));
else
   report(st.error());
```

`psio::trusted{bytes}` is the caller's statement that the bytes are a valid
`T` in this format: they were validated earlier, or were produced by `to_F`
and kept. The construction is explicit, so a decode never skips validation
by accident. For the binary formats, a trusted decode of bytes that are not
valid is undefined behaviour.

The checked decode accepts exactly what `validate_F<T>` accepts:

- `pssz`: a frame with unknown appended optional members is accepted and the
  unknown members are dropped (the compatible rule of `psio::validate`).
  `psio::validate_strict<T>(psio::pssz{}, bytes)` rejects such a frame;
  `psio::decode_pssz_checked` reports whether there were any.
- `json`, `ijson`: every member must be present. `psio::decode<T>(psio::json{}, text)`,
  and so `from_json<T>(psio::trusted{text})`, fills a missing member with its
  default instead.
- `json`, `ijson`, `key`: the reader checks as it reads, so the checked decode
  reads the input once, and the trusted decode still fails through
  `psio::codec_abort` on malformed input.

## Views and buffers

`F_view<T>` and `F_mutable_view<T>` borrow: the bytes must outlive the view.
Like every psio view they assume validated bytes and do not validate.

A view only reads. Nothing turns one into a view that writes:

- `F_view<T>` is `psio::view<T, F>`, whose storage kind defaults to
  `storage::const_borrow`. `F_view<const T>` is that same type.
- `psio::view<const T, F>` is accepted as another spelling of the read-only
  view. It is a distinct class derived from `psio::view<T, F>` that adds
  nothing and converts implicitly both ways, so either can be passed where
  the other is expected.
- Writing is always spelled out: `F_mutable_view<T>`, which is
  `psio::mutable_view<T, F>`. It exists for pssz only and has one write, the
  overwrite of a fixed-size value (`docs/pssz-spec.md` §17). It is made from
  mutable bytes (`std::span<char>`, a `std::vector<char>&`, or
  `psio::as_mutable_view` of a non-const owning buffer). It cannot be made
  from const bytes, a const buffer, a shared buffer or a read-only view.
- `F_mutable_view<const T>` and `psio::mutable_view<const T, F>` do not
  compile, and neither does `psio::view<T, F, storage::mut_borrow>`: the
  error names `psio::mutable_view`.
- A view borrows; it never owns. `psio::view<T, F, storage::owning>` and
  `psio::view<T, F, storage::shared>` do not compile: the errors name
  `psio::buffer<T, F>` (`F_buffer<T>`) and
  `psio::buffer<T, F, storage::shared>` (`shared_F<T>`), the types that keep
  the bytes alive, read through `psio::as_view(buffer)`.

`F_buffer<T>` owns a `std::vector<char>`. `shared_F<T>` holds immutable
bytes with shared ownership: a copy is another owner of the same bytes, made
by copying a pointer and incrementing an atomic count, and the bytes live
until the last copy is destroyed. It is the type for an encoded block or
transaction that several parts of a program hold at once. Constructing one
from a `std::vector<char>&&` takes the vector's allocation; constructing one
from a span copies the bytes once.

A buffer has no member functions. Its operations are free functions:

| Function | Meaning |
|----------|---------|
| `psio::bytes(b)` | the bytes, `std::span<const char>` |
| `psio::mutable_bytes(b)` | `std::span<char>`; owning and mutably borrowing buffers only |
| `psio::size(b)` | number of bytes |
| `psio::as_view(b)` | the read view, `psio::view<T, F, storage::const_borrow>` |
| `psio::as_mutable_view(b)` | the mutable view; owning and mutably borrowing buffers only |
| `psio::to_buffer(b)` | an owning copy |
| `psio::to_shared(b)` | a shared buffer: takes the bytes of an owning rvalue, shares a shared buffer, copies otherwise |

## The generic layer

The helpers are one-line calls of the customization point objects in
`psio/cpo.hpp`, which take the format tag as their first argument:

```cpp
psio::encode(fmt, v)            psio::encode(fmt, v, sink)
psio::decode<T>(fmt, bytes)     psio::size_of(fmt, v)
psio::validate<T>(fmt, bytes)   psio::validate_strict<T>(fmt, bytes)
```

Use them in code that takes the format as a template parameter, and for the
operations that have no helper name (`validate_strict`, `make_boxed`,
`validate_or_throw`). `psio::view<T, Fmt, Store>` and
`psio::buffer<T, Fmt, Store>` are the generic forms of the view and buffer
names. For a buffer `Store` is one of `storage::owning`,
`storage::mut_borrow`, `storage::const_borrow`, `storage::shared`. A view
borrows read-only: `storage::const_borrow`, the default, is the only `Store`
a view accepts (the others are compile errors naming the type to use). The
mutable borrow is `psio::mutable_view<T, Fmt>`; ownership is a buffer's.

## Adding the names for a format

In the format's header, after the tag:

```cpp
PSIO_FORMAT_HELPERS(myfmt)                         // names end in myfmt, tag psio::myfmt
PSIO_FORMAT_HELPERS_AS(myfmt, ::psio::my_tag<4>)   // names end in myfmt, another tag type
```

The macro generates each name constrained on the operation the tag provides
through `tag_invoke`. Two optional static members on the tag refine it:

- `to_fresh(v)`: the result of `to_F(v)` when it is not what
  `psio::encode(tag, v)` returns.
- `decode_checked(bytes, out)` returning `codec_status`: validate and decode
  in one pass, accepting exactly what `validate` accepts.
