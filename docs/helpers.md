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
std::string_view owner = r.owner();          // every member by its name
std::uint64_t balance  = r.balance();
Account copy           = r.unpack();         // the owned value

psio::pssz_mutable_view<Account> w{bytes};   // writes into borrowed bytes
w.balance() = 42;                            // or w.balance().set(42)

psio::pssz_buffer<Account> owned{account};                  // a packed Account, owned
psio::shared_pssz<Account> tx{account};                     // a packed Account, shared
psio::shared_pssz<Account> other = tx;                      // same bytes, no copy
other->owner();                                             // read through it
Account a = other.unpack();
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
| `pssz` | `psio/pssz.hpp` | `std::vector<char>` | appends to `std::vector<char>` or `std::string`; writes to any psio stream | yes | yes (`psio/pssz_view.hpp`) | yes (`psio/mutable_view.hpp`) |
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
one. That is the behaviour of `pssz`, `json` and `ijson`. `to_key` is an
exception: the C++ key encoder does not check member bounds or `utf8`;
`from_key` and `validate_key` do. Which format reads which annotation:
[annotations.md §1](annotations.md#1-one-declaration-every-format) and
[§7.2](annotations.md#72-what-is-checked-where); see also
[bounds by annotation](psio-overview.md#bounds-by-annotation).

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
- `json`, `ijson`: every member that is not optional must be present, in the
  checked and in the trusted decode alike (`psio::decode<T>(psio::json{}, text)`,
  `from_json<T>(psio::trusted{text})`).
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

`F_buffer<T>` (`psio::buffer<T, F>`) owns a `std::vector<char>`;
`shared_F<T>` (`psio::buffer<T, F, storage::shared>`) holds immutable bytes
with shared ownership. The borrowing kinds (`storage::const_borrow`,
`storage::mut_borrow`) are a typed span and nothing more.

Free functions, for every buffer kind:

| Function | Meaning |
|----------|---------|
| `psio::bytes(b)` | the bytes, `std::span<const char>` |
| `psio::mutable_bytes(b)` | `std::span<char>`; owning and mutably borrowing buffers only |
| `psio::size(b)` | number of bytes |
| `psio::as_view(b)` | the read view, `psio::view<T, F, storage::const_borrow>` |
| `psio::as_mutable_view(b)` | the mutable view; owning and mutably borrowing buffers only |
| `psio::to_buffer(b)` | an owning copy |
| `psio::to_shared(b)` | a shared buffer: takes the bytes of an owning rvalue, shares a shared buffer, copies otherwise |

## Packed values: `nested<T>`, `F_buffer<T>`, `shared_F<T>`

An owning or shared buffer is a value that *is* the encoding of a `T`, kept
packed. It always holds a valid encoding:

| | |
|---|---|
| `B{}` | the encoding of `T{}`. No allocation: all default-constructed buffers of one type read one process-wide constant, built on first use |
| `B{value}`, `B::from(value)` | encodes. (`B{value}` is left out for a `T` that could be mistaken for bytes, such as `std::vector<char>`: use `from`.) |
| `B{bytes}` | **validates**, then holds the bytes; `psio::codec_abort` (an exception where enabled) if they are not a `T`. A `std::vector<char>&&` is adopted without a copy; a span is copied once |
| `B{psio::trusted{bytes}}` | holds bytes the caller vouches for, unchecked |
| `B::unchecked(vector)` | adopts a vector the caller vouches for, without a copy |
| `B::checked(bytes, out)` | validates into `out`; returns a `codec_status` and leaves `out` unchanged on failure |
| `B::from_checked(value, out)` | encodes into `out`; returns a `codec_status` |
| `b.view()`, `*b`, `b.get()` | the read-only view of the `T`; `b->member()` |
| `b.unpack()` (also `decode()`) | the `T` |
| `b.decode_checked(out)` | validate, then decode |
| `b.validate()`, `b.validate_strict()` | |
| `b.bytes()`, `b.data()`, `b.size()` | the encoding |
| `a == b` | same encoding |
| `std::move(b).release()` | owning only: the bytes, as a vector |

A copy of a `shared_F<T>` is another owner of the same bytes: a pointer copy
and an atomic increment, and the bytes live until the last copy is
destroyed. They are never written after construction, so copies may be read
from different threads. It is the type for an encoded block or transaction
that several parts of a program hold at once.

There is no null state. An optional packed value is a
`std::optional<psio::shared_pssz<T>>`, which is also what it is on the wire
(an optional of a nested value).

The three packed-value types for PSSZ differ only in who owns the bytes:

| Type | Owns | Copy | In-place writes |
|------|------|------|-----------------|
| `psio::nested<T>` | a `std::vector<char>` | copies the bytes | through `psio::mutable_view` of the enclosing frame |
| `psio::pssz_buffer<T>` | a `std::vector<char>` | copies the bytes | `psio::as_mutable_view(b)` |
| `psio::shared_pssz<T>` | a share of immutable bytes | shares them | none |

As a **member of a reflected record** (or an element of a list, the payload
of an optional, an alternative of a variant) all three are the same thing
to every format — `psio::detail::is_nested` holds for each:

| Format | A packed `T` member is |
|--------|------------------------|
| pssz | the bytes of a byte list holding `to_pssz(T)`; every checking reader of the enclosing value validates them as `T` |
| key | the key of that byte list |
| json, ijson | the inner value's own text (`{"transaction": {"exp": 1, ...}}`), not hex |
| schema | `Nested{T}` |
| view | a nested view: `bytes()`, and `*` / `->` / `get()` giving the view of the `T`; `unpack()` gives the member's own type |

So a record can declare `psio::shared_pssz<Transaction> transaction;` and be
read by a peer that declares `psio::nested<Transaction>` or
`std::vector<char>`: the bytes are identical. Decoding the record adopts the
member's bytes once; copying the record then shares them.

`psio::nested<T, psio::nested_validation::deferred>` remains the one form
whose bytes a reader of the enclosing value does not look at.

## Reading through a view

`#include <psio/pssz_view.hpp>`. A view is a pair of pointers into bytes that
have been validated; nothing is decoded until it is asked for.

### Members by name

The view of a reflected record has one accessor per reflected member, with
the member's name. (It derives from `psio::reflect<T>::proxy<...>`, the class
`PSIO_REFLECT` generates.) `get<N>()` is the same access by index.

```cpp
struct Header { std::uint32_t num = 0; std::string producer; };
PSIO_REFLECT(Header, num, producer)
struct Block  { Header header; std::vector<Header> prev; std::optional<Header> next; };
PSIO_REFLECT(Block, header, prev, next)

psio::pssz_view<Block> block{bytes};
block.header().num();                 // std::uint32_t, by value
block.header().producer();            // a string view: converts to std::string_view
block.prev()[0].num();                // elements of a list are views too
block.next()->producer();             // through an optional
for (auto h : block.prev()) use(h.num());
```

What an accessor returns:

| Member type | `v.member()` is | Read it with |
|-------------|-----------------|--------------|
| arithmetic, `bool` | the value | |
| enum, duration / time point, `uint256` | a scalar view | converts to the value; `.get()` |
| `std::string`, `psio::utf8_string<N>` | a string view | converts to `std::string_view`; `.view_()`, `size()`, `empty()` |
| reflected record | its view | its own accessors |
| transparent type (`transparent()`) | the view of its member | as the member; `.unpack()` gives the transparent type |
| `std::vector<T>`, `std::array<T, N>` | a list view | `size()`, `empty()`, `[i]`, `at(i)`, `front()`, `back()`, `begin()` / `end()` (random access), `rbegin()` / `rend()`; a `std::vector<char>` view converts to `std::span<const char>` |
| `std::optional<T>` | an optional view | `has_value()`, `bool`, `*`, `->`, `value()` (checked), `value_or(x)` |
| `std::variant<Ts...>` | a variant view | `index()`, `visit(fn)`, `get_if<I>()` / `get_if<Alt>()` (a `std::optional` of the alternative's view), `holds_alternative<Alt>()`; also `psio::get_if<Alt>(v)` |
| `std::tuple`, `std::pair` | a tuple view | `get<N>()`; `first()` / `second()`; structured bindings |
| `std::map`, `std::set` (and unordered) | the list view of its entries, in key order | the list operations, and `contains(k)`, `find(k)`, `lower_bound(k)` (indices; binary search), `at(k)` and `value_or(k, x)` for a map |
| `psio::nested<T>` | a nested view | `bytes()`, `*` / `->` / `get()` (the view of the `T`) |

The same names exist on `psio::view<const T, F>`, on a view of a member that
sits under a `maxDynamicData` cap or a bounding annotation (its format is
`psio::pssz_at<...>`: the widths of that position), on `psio::pssz_<W>` and
`psio::ssz` views, on `psio::view<T, psio::pjson_format>`, and on
`psio::mutable_view` (below).

**Names the view itself uses.** A record view has few members of its own, so
that members of the record are not hidden: `get`, `unpack`, `element_type`,
`format_type`, `storage_kind`, `view`, and names beginning `psio_` or
`_psio_`. A reflected member with one of those names is still part of the
view — the view's own name wins and the member is read by index:
`v.get<*psio::reflect<T>::index_of("unpack")>()`. (`size`, `data`, `bytes`,
`index`, `value`, `empty` and the like are *not* taken: a record view has no
such members, so a member named `size` is `v.size()`.) The bytes and size of
any view are the free functions `psio::bytes(v)` and `psio::size(v)`.
Reflected *methods* are not callable through a view (a compile error).

### From a view to its value

`v.unpack()` returns the owned C++ value of what the view designates — `T`
for a `psio::view<T, F>` — for every kind of view: record, list, string,
optional, variant, tuple, map, nested, scalar.

```cpp
Block b                  = block.unpack();
std::vector<Header> prev = block.prev().unpack();
std::string producer     = block.header().producer().unpack();
```

`psio::value_of(x)` is the same for generic code that is handed either a view
or a value (a record view returns arithmetic members by value): `x.unpack()`
for a view, a copy of `x` otherwise.

```cpp
template <typename V>                         // any record view
auto first_two(const V& v) {
   return std::pair{psio::value_of(v.template get<0>()), psio::value_of(v.template get<1>())};
}
```

`psio::from_pssz<T>(view)` and `psio::decode<T>(psio::pssz{}, view)` are
`view.unpack()` under the decode names.

Unpacking is a **trusted** operation, like every read through a view: the
bytes were validated when the view was made, and are not checked again. On
bytes that were never validated it is undefined behaviour, exactly like
`from_pssz<T>(psio::trusted{bytes})`.

Use `unpack()` and not `psio::from_pssz<T>(psio::trusted{psio::bytes(sub)})`
for a sub-view. The second is wrong whenever the member sits under a
`maxDynamicData` cap or a bounding annotation: the offsets inside the member
are then narrower than in a standalone value of its type, so its bytes are
not an encoding of that type (they do not even validate as one). The view's
format knows the position; the spelled-out form does not.

There is no implicit conversion from a view to an owned value: a conversion
that allocates is always written (`unpack()`). The conversions that borrow or
copy a scalar are implicit: a string view to `std::string_view`, a
`std::vector<char>` view to `std::span<const char>`, a scalar view to its
value.

### Comparison

A view compares as the value it designates, with `==` (and `<`, `<=>` where
the value type has them): with a value of its type or anything that type
compares with, and with another view.

```cpp
block.header().producer() == "alice"          // characters, in place
block.header() == other.header()              // needs Header::operator==
block.next() == std::nullopt
```

Scalars and strings are compared in place. Any other comparison unpacks both
sides, which allocates what the value type allocates; compare
`psio::bytes(a)` with `psio::bytes(b)` to compare encodings without that.

### Writing by name

`psio::mutable_view<T, F>` (`#include <psio/mutable_view.hpp>`) has the same
named accessors; each returns the mutable view of the member. A fixed-size
member is overwritten with `set`, which reports, or with `=`:

```cpp
psio::pssz_mutable_view<Block> w{bytes};
w.header().num() = 8;                       // psio::codec_abort if the position is absent
bool ok = w.prev().at(3).num().set(2u);     // false: no such element, nothing written
```

Its own names are `get`, `set`, `unpack`, `element_type`, `format_type`,
`storage_kind`, `assignable` and `psio_*` / `_psio_*`. Const safety is
unchanged: a read view's accessors return values and read views, nothing
converts a read view (or a named sub-view of one) to a mutable view, and a
mutable view cannot be made from const bytes.

### Compared with the view of the psiserve in-tree psio

The named readers are that library's design. What differs here:

| There | Here |
|-------|------|
| `view<T, Fmt>::from_buffer(ptr)` | `psio::pssz_view<T>{bytes}` (a span: the view knows its length) |
| `v.name()` is a `std::string_view` | a string view that converts to `std::string_view` and compares with one |
| `v.note()` of an absent optional string is an empty `std::string_view` | an optional view: `has_value()`, `value_or(...)` |
| `vec_view`, `set_view`, `map_view` | one list view; a map / set view adds `contains`, `find`, `lower_bound`, `at`, `value_or` |
| `frac_ref<T>` with `fields().member() = value`, including resizing writes | `psio::mutable_view<T, F>`: `w.member() = value` for fixed-size values only (a PSSZ frame has one canonical form; a write that changes a size is decode, modify, encode) |
| `explicit operator bool` (null view) | a view is never null; an empty optional is an optional view |

## Packing from values that do not own their data

`to_F`, `F_size` and the checked encoders take a value, or something that
only refers to one. A source is encoded as the owning type it stands for —
`psio::wire_type_t<Source>` (`psio/wire_type.hpp`) — and writes exactly the
bytes that type writes, without copying what it refers to:

| Source | Encoded as (`wire_type_t`) |
|--------|----------------------------|
| `std::string_view`, `const char*`, a string literal | `std::string` (a C string without its NUL; give a `std::string_view` for text with embedded NULs) |
| `std::span<E>`, `std::span<const E>` | `std::vector<E>` |
| `std::reference_wrapper<X>` (`std::ref`, `std::cref`) | `X` |
| `std::tuple` / `std::pair` of references or of sources (`std::tie(a, b)`, `std::forward_as_tuple(...)`) | the tuple / pair of the values |
| `std::vector<S>`, `std::optional<S>` of a source | the vector / optional of what `S` is encoded as |
| `psio::view<X, F>` (any view, given as a value) | `X` |
| a reflected record with members of the kinds above | the record with owning members (same layout, same widths, same bounds) |

```cpp
psio::to_pssz(std::tie(id, trace));                  // == to_pssz(std::tuple{id, trace}); nothing copied
psio::to_pssz(std::string_view{"abc"});              // == to_pssz(std::string{"abc"})
psio::to_pssz(std::span<const Item>{items});         // == to_pssz(items)

struct TxSuccessView { Checksum256 id; BlockNum blockNum; psio::pssz_view<const TransactionTrace> trace; };
PSIO_REFLECT(TxSuccessView, id, blockNum, trace)
psio::to_pssz(TxSuccessView{id, num, traceView});    // the bytes of the record holding a TransactionTrace
```

This is PSSZ (`to_pssz`, `pssz_size`, `encode_pssz_checked`,
`validate_pssz<Source>`, which validates as the wire type) and `key`
(`to_key`: `std::string_view`, C strings, `std::reference_wrapper`, views,
tuples of references). Decoding *into* a source does not compile: decode
`psio::wire_type_t<Source>`, or view the bytes.

**A view as a value** re-emits its bytes when they are what the position
needs, and is decoded and encoded again when they are not. The second case
is real: a member that sits under a `maxDynamicData` cap or a bounding
annotation has narrower offsets than the same type standing alone, so a
view of a standalone `std::vector<std::string>` written into a capped
record (or the view of such a member written standalone) cannot be copied
byte for byte. Which case applies is decided at compile time from the
view's format and the position; either way the bytes are those of the
value. The view is over validated bytes, so nothing is validated again;
bounds, caps and invariants of the enclosing value still apply.

Annotations on a source member are those of the type it is encoded as
(`attr(names, max<4>)` on a `std::span<const std::string_view>` member
bounds a list of strings); they are enforced on encode as usual.

`psio::wire_type` is a customization point: specialize it for a type of
your own that packs as another type.

For keys there is one more writer-side element, `psio::raw_key{bytes}`:
bytes that already are a key, written verbatim, so that a stored prefix is
extended without re-encoding it —
`psio::to_key(std::tuple{psio::raw_key{prefix}, id, std::string_view{name}})`.
See [key-spec.md §3](key-spec.md#3-composite-keys). (`psio::to_key(x)`
returns a `std::array` for a key of fixed size; `psio::encode(psio::key{}, x)`
always returns a `std::vector<char>`.)

### Encoding a source as a named type

`to_pssz(source)` writes `wire_type_t<Source>`. When the type to write is
one the source does not spell — a method's argument tuple, which carries
the method's cap and parameter annotations — name it:

```cpp
using Args = psio::reflect<Token>::method_arg_tuple<0>;
auto b = psio::to_pssz_as<Args>(std::forward_as_tuple(toView, amount, memo));
auto n = psio::pssz_size_as<Args>(std::forward_as_tuple(toView, amount, memo));

// The same from the arguments themselves; each may be the parameter's type, a
// source of it (a view, a std::string_view, ...) or a value that converts to it:
auto c = psio::method_args_to_pssz<Token, 0>(toView, 5, "memo");
psio::method_args_append_pssz<Token, 0>(out, toView, 5, "memo");
```

The source must be a source of exactly that type; anything else is a
compile error. `psio::encode_method_result<T, I>(value, out)` takes a source
of the result as well, so a method that returns a `psio::pssz_view<X>` has
its result written as an `X`.

## Sinks and sources

`psio::to_pssz(v, sink)` (and `psio::encode(psio::pssz{}, v, sink)`) writes
to:

| Sink | |
|------|---|
| `std::vector<char>`, `std::string` | appends |
| `psio::fixed_buf_stream{buffer, size}` | a caller's buffer, every write bounds-checked (overrun fails through `psio::codec_abort`) |
| `psio::fast_buf_stream{buffer, size}` | a buffer the caller has sized with `psio::pssz_size(v)`; unchecked |
| `psio::vector_stream{vec}`, `psio::string_stream{str}` | grows the container |
| `psio::size_stream` | counts only (`sink.size`) |
| any type satisfying `psio::stream_sink` | `write`, `rewrite`, `skip`, `written` (`psio/stream.hpp`) |

The value is checked and sized before the first byte is written, so a value
with no encoding writes nothing. A stream that already holds bytes is
appended to. `psio::to_key(v, sink)` takes the same sinks.

For reading a byte format by hand, one field after another,
`psio::input_stream` (`psio/stream.hpp`) is a bounds-checked cursor over
borrowed bytes, and `psio/bin.hpp` has the typed and LEB128 readers:

```cpp
psio::input_stream in{bytes};                    // a span, string_view or vector<char>
auto magic   = psio::from_bin<std::uint32_t>(in);   // one bin value; the cursor advances
auto count   = psio::varuint32_from_bin(in);        // also varuint64 / sleb32 / sleb64
auto payload = in.read_bytes(count);                // borrowed, not copied
auto flags   = in.get<std::uint16_t>();             // raw host-order bytes; read_raw(x), read(p, n)
in.skip(4);  in.position();  in.remaining();  in.empty();  in.rest();
```

A read past the end, or a malformed varint or value, fails through
`psio::codec_abort` and consumes nothing; test `remaining()` first where
that must not happen. The writers are `psio::varuint32_to_bin(v, sink)`,
`varuint64_to_bin`, `sleb32_to_bin`, `sleb64_to_bin`, and
`psio::to_bin(v, sink)`.

`psio::from_hex(text, destination)` (`psio/to_hex.hpp`) fills a destination
of fixed size — a `std::array<unsigned char, 32>`, a `std::span<char>`, a
`char[N]` — and returns false unless the text is exactly `2 * size` hex
digits; the `std::vector<char>&` form grows.

## Calling a reflected method with packed arguments

The arguments of method `I` of a reflected `T` travel as one value: the
encoding of `psio::reflect<T>::method_arg_tuple<I>`.

```cpp
struct Token {
   void transfer(psio::pssz_view<const Account> to, std::uint64_t amount, std::string_view memo);
   PSIO_REFLECT(Token, method(transfer, to, amount, memo, maxDynamicData(4096)))
};
```

| Name | Is |
|------|----|
| `reflect<T>::method_arg_tuple<I>` | the **wire** type of the arguments: a `psio::annotated_tuple` of `psio::wire_type_t<parameter>` carrying the method's cap and parameter annotations. Above: `(Account, std::uint64_t, std::string)`. The type to encode, validate and decode arguments as, and what a schema describes |
| `reflect<T>::method_params<I>` | how the method **takes** them: `std::tuple<remove_cvref_t<parameter>...>`. Above: `(psio::pssz_view<Account>, std::uint64_t, std::string_view)` |
| `psio::validate_method_args<T, I>(bytes)` | `codec_status`: are the bytes that argument tuple |
| `psio::method_args_view<T, I>` | the read view of the argument tuple (`get<N>()`, structured bindings); correct under the method's cap, where the view of the plain `std::tuple` reads wrong offsets |
| `psio::method_params_from_pssz<T, I>(psio::trusted{bytes})` | a `method_params<I>`: each argument as the method declares it |
| `psio::invoke_method_pssz<T, I>(obj, bytes)` | validates, then calls the method; returns what it returns |
| `psio::invoke_method_pssz<T, I>(obj, psio::trusted{bytes})` | calls without validating |
| `psio::call_method_pssz<T, I>(obj, psio::trusted{bytes}, result)` | calls and appends the encoded result (`psio::encode_method_result`, which honours `returns(...)`); a `codec_status` |
| `psio::method_args_to_pssz<T, I>(args...)` | the caller's side: the argument tuple encoded from the arguments themselves, none copied ([above](#encoding-a-source-as-a-named-type)) |
| `reflect<T>::invoke_method<I>(obj, tuple)` | unchanged: calls with a tuple of arguments as declared (`method_params<I>`) |

How a parameter is handed to the method is decided by its declared type:

| Declared as | Wire type | The method receives |
|-------------|-----------|---------------------|
| `psio::pssz_view<X>`, `psio::pssz_view<const X>`, `psio::view<const X, psio::pssz>` | `X` | a view into the argument bytes |
| `std::string_view` | `std::string` | the characters, borrowed from the argument bytes |
| `std::span<const char>` (or of another byte type) | `std::vector<char>` | the bytes, borrowed |
| anything else (`X`, `const X&`, `X&&`) | `X` | the unpacked value |

What is borrowed points into `bytes`, which must outlive the call.

The validated / unvalidated split is explicit. The span overload of
`invoke_method_pssz` checks the bytes and fails through `psio::codec_abort`
before the method runs; `psio::trusted{bytes}` is the caller's statement
that `validate_method_args` (or `validate_pssz<method_arg_tuple<I>>`)
accepted them. A dispatcher that reports errors itself validates, reports,
and then calls with `trusted`.

A view parameter needs the argument's bytes to be a standalone encoding of
its type. That is so for every reflected record, whatever the method's cap,
and for any type when the method has no cap and the parameter no
annotations. A parameter whose position narrows offset widths — a
`std::vector<std::string>` under the method's `maxDynamicData` — cannot be
taken as `psio::pssz_view`: `invoke_method_pssz` refuses at compile time and
says to take it by value (or to state the bound in the type).

`validate_pssz<Tuple>` of a tuple type that names a view (or any other
non-owning type) validates the tuple of wire types; decoding *into* such a
type does not compile (decode `psio::wire_type_t<Tuple>`, or view the bytes).

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
