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
| `F_trusted_view<T>` | read-only view over bytes the caller vouches for: no per-access checks (pssz, ssz, wit) | `psio::view<T, psio::F, psio::storage::trusted_borrow>` |
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
| `json` | `psio/json.hpp` | `std::string` | appends to `std::string` or `std::vector<char>` | yes | no | no |
| `ijson` | `psio/json.hpp` | `std::string` | appends to `std::string` or `std::vector<char>` | yes | no | no |
| `msgpack` | `psio/msgpack.hpp` | `std::vector<char>` | appends to `std::vector<char>` | yes | no | no |
| `avro` | `psio/avro.hpp` | `std::vector<char>` | appends to `std::vector<char>` | yes | no | no |
| `bson` | `psio/bson.hpp` | `std::vector<char>` | appends to `std::vector<char>` | yes | no | no |
| `protobuf` | `psio/protobuf.hpp` | `std::vector<char>` | appends to `std::vector<char>` | yes | no | no |
| `capnp` | `psio/capnp.hpp` | `std::vector<char>` | **replaces** the content of a `std::vector<char>` | yes | no | no |
| `flatbuf` | `psio/flatbuf.hpp` | `std::vector<char>` | **replaces** the content of a `std::vector<char>` | yes | yes, safe and trusted (`psio/flatbuf_view.hpp`) | yes: fixed-size fields that are stored (`psio/flatbuf_view.hpp`) |
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
  `-fno-exceptions`; a WebAssembly guest), after giving the error to the
  [abort handler](#getting-the-error-text-without-exceptions) if there is one.

### Getting the error text without exceptions

A build without exceptions can still learn *why* before it aborts. Every
fail-fast path — the checked decode, `to_F` of a value with no encoding, a
bounds-checked stream that overruns, `value()` of an absent optional view —
hands its `psio::codec_error` to the embedder's abort handler first. There
are two ways to provide one (`psio/error.hpp`):

```cpp
// 1. At run time: a process-wide function pointer.
void on_psio_abort(const psio::codec_error& e) noexcept {
   host_abort_message(e.what.data(), e.what.size());   // may not return
}
psio::set_codec_abort_handler(on_psio_abort);          // returns the previous handler

// 2. At build time: compile every translation unit with
//       -DPSIO_CODEC_ABORT_HANDLER=my_psio_abort
//    and define once:
extern "C" void my_psio_abort(const char* what, std::size_t what_size, std::uint32_t byte_offset,
                              const char* format, std::size_t format_size) { ... }
```

| | Run-time handler | Build-time handler |
|---|---|---|
| Cost when no failure occurs | none | none |
| Cost on the failure path | one load and null test | a direct call |
| Data | one pointer, constant-initialized to null | none |
| Set-up | one call, any time before the first failure — including from a static initializer (the slot has no dynamic initialization to order) | none: nothing runs at start-up |
| Rule | install once, before other threads decode (the slot is read without synchronization) | the macro must have the same definition in **every** translation unit that includes psio (`codec_abort` is inline: a mixed build violates the one-definition rule) |

Neither needs exceptions or RTTI. The text (`error.what`) is not
NUL-terminated and is valid for the duration of the call; `format_name` says
which codec failed. If the handler returns, `std::abort()` follows. A build
that uses neither pays one null test on the failure path and nothing else.
In a build *with* exceptions the handler is not called: the
`psio::codec_exception` is the report.

A WebAssembly guest normally uses the build-time form, since it needs no
initialization and adds no data.

### Asking first

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

**Strict validation** — `psio::validate_strict<T>(psio::pssz{}, bytes)` — is
"valid, canonical, and nothing this reader does not know". It accepts `bytes`
exactly when they are the encoding of the value they decode to
(`to_pssz(from_pssz<T>(bytes)) == bytes`), with no unknown appended members
at any depth (nested values included), and the value's semantic specs and
invariants hold. It returns a `codec_status` and gives the same answers in a
build without exceptions (`-fno-exceptions -fno-rtti`,
`PSIO_EXCEPTIONS_ENABLED=0`) as natively, for every type: records with binary
or text adapters, invariants, transparent types, bounded and ordered members,
constructed records.

It is **one walk over the bytes, in place**: the value is not built. What
makes an encoding canonical is checked where it lies:

| Rule | Checked on |
|---|---|
| A record's header is its fixed region; a header shorter than the layout ends at a member boundary and omits optional members only; no unknown appended slot | the header |
| The first offset of a record is the end of the fixed region, of a list the size of its offset table; offsets do not decrease and stay inside the span; each payload ends where the next begins and the last at the end of the span — no gap, overlap or trailing byte | the offsets |
| The last written slot of an extensible record is not an absent optional (pruning is mandatory) | the last slot |
| `optional<V>`: None is the empty span when `V` cannot be empty, otherwise the selector `00` alone; Some is `V` (after selector `01`) | the span, the selector |
| A fixed-size value fills its span exactly; a list of fixed elements is a whole number of them; a zero-size element list is empty | the span length |
| `bool` is 0 or 1; a variant index names an alternative; unused bits of a `bitvector` are zero; a `bitlist` ends in its delimiter byte and is within its capacity | the byte |
| Offset widths (1, 2 or 4 bytes) are those of the type: its `maxDynamicData`, or the bound inferred from its members | compile time |
| `max` / `exact` / `min`, `utf8`, `psio::bounded`, `psio::utf8_string`, `maxDynamicData` | the span (length, count, bytes) |
| Map and set entries are in strictly ascending key order | each key, one at a time |
| A nested value (`psio::nested<T>`, `shared_pssz<T>`) is itself strictly valid as a `T` | the same walk, on its bytes |
| A semantic spec with a `validate(span)` (a third-party spec, `element_spec`) on a string member | the member's bytes |

The walk uses the stack of its recursion (at most 64 nodes deep) and **no
heap**, whatever the size of the message; it costs about a `validate_pssz`.
Only values whose encoding is user code are materialized, each one from its
own span and never the message around it:

- a type with a binary adapter, and a member written through `as<Tag>`: its
  payload is put to the adapter's `validate_strict`, then decoded and encoded
  again and compared (`pssz: noncanonical encoding`) — an adapter may read
  spellings it does not write;
- the keys of a map or set that does not iterate in key order by
  construction (a hash container, a custom comparator, a key whose `<` is
  user code): bytes whose keys the container merges are not the encoding of
  any value (a map whose mapped type cannot be default-constructed is
  decoded and encoded again whole);
- what `validate_pssz` itself materializes: a value of a type with an
  `invariant`, a member with `sorted` / `unique` / `unique_keys`, and one map
  or set key at a time for the order check.

- `json`, `ijson`: every member that is not optional must be present, in the
  checked and in the trusted decode alike (`psio::decode<T>(psio::json{}, text)`,
  `from_json<T>(psio::trusted{text})`).
- `json`, `ijson`, `key`: the reader checks as it reads, so the checked decode
  reads the input once, and the trusted decode still fails through
  `psio::codec_abort` on malformed input.

## Views and buffers

`F_view<T>`, `F_trusted_view<T>` and `F_mutable_view<T>` borrow: the bytes
must outlive the view. None of them validates.

### Safe and trusted views

There are two read views of the same bytes. They have the same members
(they are the same templates) and read the same values from a valid
encoding. They differ in what happens when the bytes are *not* a valid
encoding of `T`.

| | `psio::pssz_view<T>` — safe | `psio::pssz_trusted_view<T>` — trusted |
|---|---|---|
| Made from | any bytes: `psio::pssz_view<T>{bytes}` | `psio::pssz_trusted_view<T>{psio::trusted{bytes}}`, `psio::validated_pssz_view<T>(bytes)`, `buffer.view()` |
| Every offset read from the bytes | compared with the size of what it points into before it is used | used as it is |
| Bytes that are not a valid `T` | navigation never leaves the bytes; what is not there reads as zero / empty | **undefined behaviour** |
| Cost (PSSZ, ns: all 5 members of a nested record / iterate 16 records) | 2.5 / 10.6 | 0.7 / 2.7 |
| `unpack()` | a trusted decode in both: undefined on bytes that are not valid | |

**Which one.**

- The bytes are a row your own service wrote, or you just produced them
  with `to_pssz`: the trusted view.
- The bytes come from outside (an action's arguments, a row another service
  wrote, a message off the network) and you will read most of them:
  validate once, and read through the trusted view the validating helper
  returns.
- The bytes come from outside and you will read one or two members of a
  large value, or you cannot afford the validation pass: the safe view. Its
  navigation is safe on anything; do not call `unpack()` on it (or on any
  view of bytes that were not validated).
- You do not know: the safe view. It is the default name for that reason.

```cpp
// 1. A row this service wrote: trusted.
std::span<const char> bytes = kv_get(account_key);          // our own table
psio::pssz_trusted_view<Account> row{psio::trusted{bytes}};
if (row.balance() < amount) abort_message("overdrawn");
auto memo = row.memo();                                     // std::string_view-like, borrowed

// 2. An action argument: validate once, then the trusted view.
auto args = psio::validated_pssz_view<Transfer>(action_data);
if (!args)                                                  // psio::checked_view<Transfer, psio::pssz>
   abort_message(args.error().what);
credit(args->to(), args->amount());                         // psio::pssz_trusted_view<Transfer>

// 3. A row another service wrote: the safe view, or validate first.
psio::pssz_view<TheirRow> theirs{foreign_bytes};            // safe: reads inside the bytes whatever they are
auto owner = theirs.owner();
auto checked = psio::validated_pssz_view<TheirRow>(foreign_bytes);   // or: check, then trust
```

**What the caller of a trusted view guarantees.** The bytes

1. were accepted by `psio::validate_F<T>` **for this `T`** (that is what
   `psio::validated_view` / `psio::validated_pssz_view` do), or were produced
   by `psio::to_F` of a `T`, or are a row this program itself wrote as a `T`;
   and
2. are not modified while the view, or any view obtained from it, is in use.

If either does not hold the behaviour is undefined — reads outside the
bytes — exactly as for `psio::from_F<T>(psio::trusted{bytes})`.

"For this `T`" matters when a type evolves. PSSZ lets a record grow by
appending `std::optional` members, and both views read a row written before
the member existed correctly (the member reads as absent). A row written
before a **non-optional** member was appended is not a valid encoding of the
new type: `validate_pssz` refuses it, the safe view reads the member as
zero, and the trusted view reads past the row. Append optionals only.

**Trust is in the type.**

- It is only obtained by saying so. A trusted view has no constructor from
  a span, a vector or a string: `psio::pssz_trusted_view<T>{bytes}` does not
  compile; `psio::pssz_trusted_view<T>{psio::trusted{bytes}}` does.
  (`psio::pssz_view<T>{psio::trusted{bytes}}` does not compile either: the
  safe view's name never yields a trusted view.)
- Every sub-view of a trusted view is trusted: `row.customer()`,
  `row.items()[i]`, `*row.maybe()`, the alternative handed to `visit`, a map
  entry. One exception: the inner view of a
  `psio::nested<T, psio::nested_validation::deferred>` member is the safe
  view, because validating the enclosing value never looked at those bytes.
- A trusted view converts implicitly to the safe view of the same bytes, so
  it can be passed to any function that takes `psio::pssz_view<T>`. There is
  no conversion from safe to trusted; a function that takes
  `psio::pssz_trusted_view<T>` cannot be handed unvalidated bytes by
  accident.

**What a trusted view still does.** `list.at(i)` is bounds-checked and
returns an empty view past the end (`list[i]` is not: its precondition is
`i < list.size()`). A default-constructed trusted view reads as zero / empty.
What is wire meaning rather than a check stays: a trailing optional the
writer did not write reads as absent, fields a newer writer appended are
skipped, an empty list is zero bytes. It is read-only, like every view.

**Validating helpers.**

| | Returns |
|---|---|
| `psio::validated_view<T>(psio::F{}, bytes)` | `psio::checked_view<T, psio::F>`: `ok()` / `explicit operator bool`, `error()`, and `*r` / `r->member()` / `r.view()` — the trusted view (an empty view, reading as zero, when `!ok()`); `r.value()` is the view or `psio::codec_abort`. Never throws or aborts itself. For pssz, pssz8/16/32, ssz, wit |
| `psio::validated_pssz_view<T>(bytes)` | the same for `psio::pssz` |
| `psio::validated_pssz_view(fmt, bytes, result, has_unknown)` | a `codec_status`; fills `std::optional<psio::pssz_trusted_view<T>>` (or `std::optional<psio::pssz_view<T>>`) and reports whether the bytes carry fields this `T` does not know |
| `F_buffer<T>` / `shared_F<T>`: `b.view()`, `*b`, `b->member()`, `b.get()` | the trusted view: the buffer validated on construction (see below for the constructors that take the caller's word instead) |
| `psio::as_view(b)` | the safe view of a buffer's bytes |

**Debugging.** Compile with `-DPSIO_DEBUG_VIEWS` (the whole program: it
changes inline functions) and a trusted view makes every compare it
otherwise skips, and calls `psio::detail::view_trust_violation` — a message
on stderr, then `std::abort` — when one fails:

```
psio: trusted view over bytes that are not a valid encoding: a record's header announces more than the record holds
```

**Other views.** `psio::mutable_view` has no trusted form: it already
requires validated bytes for writing, and the reads it offers are of
fixed-size values at fixed positions. The Cap'n Proto view
(`psio::capnp_view<T>`, `docs/capnp.md`) is a cursor view with a single kind
that relies on `psio::validate_capnp`; there is no `capnp_trusted_view`
name, and it has not been split into a safe and a trusted kind yet.
The FlatBuffers views (`docs/flatbuffers.md`) are cursor views too and have
both kinds: `psio::flatbuf_view<T>` compares every offset, vtable entry and
length with the buffer before it reads and may be constructed over any
bytes; `psio::flatbuf_trusted_view<T>` (from `psio::validated_view`, or
`psio::trusted{bytes}`) compares nothing. The trusted one is a single
pointer and does not carry the buffer's bounds, so — unlike the PSSZ / SSZ /
WIT trusted views — it does not convert to the safe view.
`psio::flatbuf_mutable_view<T>` navigates as the safe view does.

A view only reads. Nothing turns one into a view that writes:

- `F_view<T>` is `psio::view<T, F>`, whose storage kind defaults to
  `storage::const_borrow`. `F_view<const T>` is that same type.
  `F_trusted_view<T>` is `psio::view<T, F, storage::trusted_borrow>`.
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
| `psio::as_view(b)` | the safe read view, `psio::view<T, F, storage::const_borrow>` (an owning or shared buffer's own `b.view()` is the trusted view) |
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
| `B{psio::trusted{bytes}}` | holds bytes the caller vouches for, unchecked. The buffer's views are trusted views, so bytes that are not a valid `T` are undefined behaviour to read (`psio::as_view(b)` is the safe view) |
| `B::unchecked(vector)` | adopts a vector the caller vouches for, without a copy; as above |
| `B::checked(bytes, out)` | validates into `out`; returns a `codec_status` and leaves `out` unchanged on failure |
| `B::from_checked(value, out)` | encodes into `out`; returns a `codec_status` |
| `b.view()`, `*b`, `b.get()` | the read-only view of the `T`; `b->member()`. It is the **trusted** view, `F_trusted_view<T>` (pssz, ssz, wit): the buffer holds a valid encoding, so no access checks it again. It converts implicitly to `F_view<T>` |
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

`#include <psio/pssz_view.hpp>`. A view is a pair of pointers into bytes;
nothing is decoded until it is asked for. Everything in this section holds
for the safe view (`psio::pssz_view<T>`) and for the trusted view
(`psio::pssz_trusted_view<T>`) alike — see "Safe and trusted views" above
for which to use.

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
| reflected record | its view | its own accessors; converts to the record when that is `definitionWillNotChange`, fixed-size and trivially copyable |
| transparent type (`transparent()`) | the view of its member | as the member; `.unpack()` gives the transparent type |
| `std::vector<T>`, `std::array<T, N>` | a list view | `size()`, `empty()`, `[i]`, `at(i)`, `front()`, `back()`, `begin()` / `end()` (random access), `rbegin()` / `rend()`; a view of bytes converts to `std::span<const B>`, a view of a fixed array of fixed values to the array ([below](#from-a-view-to-its-value)) |
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

**Implicit conversions.** One rule for every view:

| The view designates | Converts implicitly to |
|---------------------|------------------------|
| a value that is trivially copyable and of fixed encoded size: a scalar, an enum, a duration / time point, `uint256`, a `std::array` of such, a `definitionWillNotChange` record of such (a 32-byte checksum, an account number: a one-member record with a text adapter), a transparent type over one | the value. Copying it costs what reading it costs |
| a byte sequence | what borrows it: a string to `std::string_view`; a `std::vector<B>` of a byte type `B` (`char`, `signed char`, `unsigned char`, `std::byte`, `std::int8_t`, `std::uint8_t`) to `std::span<const B>`; a `std::array<B, N>` to `std::span<const B, N>` and `std::span<const B>` (and to the array, by the first row). `data()` gives the pointer |
| anything else: a string as `std::string`, a list, an optional, a variant, a tuple, an extensible record, a record with a variable-size member, a type that is not trivially copyable | nothing: write `unpack()`. A conversion that allocates or walks is always spelled |

```cpp
void use(AccountNumber, const Checksum256&, std::span<const unsigned char>);
use(row.owner(), row.id(), row.data());      // no unpack()
```

A span is over the element type only (`std::vector<unsigned char>` does not
convert to `std::span<const char>`), and never over elements wider than a
byte: the wire is not aligned for them. The rule is
`psio::view_layout::value_convertible_v<Format, T>` for the first row; it is
implemented for the PSSZ formats (`psio::pssz`, `pssz_<W>`, members under a
cap).

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
tuples of references). Decoding *into* a source compiles for the shapes
that can borrow from the bytes ([below](#decoding-into-a-record-that-borrows));
for the others, decode `psio::wire_type_t<Source>`, or view the bytes.

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

// The same from the arguments themselves, as a call would be written:
auto c = psio::method_args_to_pssz<Token, 0>(toView, 5, "memo");
psio::method_args_append_pssz<Token, 0>(out, toView, 5, "memo");
```

The source must be a source of exactly that type; anything else is a
compile error. `psio::method_result_to_pssz<T, I>(value)` takes a source
of the result as well, so a method that returns a `psio::pssz_view<X>` has
its result written as an `X`
([below](#the-result-of-a-method)).

**Arguments are taken as an ordinary C++ call takes them.**
`method_args_to_pssz<T, I>(args...)` accepts what `obj.method(args...)`
accepts, and nothing else:

| The argument | Is |
|--------------|----|
| already a source of the parameter (`psio::wire_type_t<Argument>` is the parameter's wire type): the value, a view of it, a `std::string_view` or literal for a string, a span for a list, `std::cref(x)` | written as it stands, by reference. Nothing is copied, converted or unpacked |
| anything else | converted to the parameter's type **implicitly** (copy-initialization, `P p = std::forward<A>(arg);`), then written |

Explicit constructors and explicit conversion operators are not used, as in
a call: for `struct Quantity { Quantity(std::uint64_t); explicit Quantity(double); }`
the argument `5` is the `std::uint64_t` constructor, and a type that only
converts explicitly is a compile error (convert it at the call). Arguments
are forwarding references, so a conversion operator that is not `const`
(`operator AccountNumber()` of a non-const object) or is `&&`-qualified is
used exactly when the call would use it. The number of arguments is checked
first ("too few" / "too many arguments passed to the method").
`psio::method_arguments<T, I, Args...>` is the same test as a concept, for
constraining a proxy's call operator.

### Decoding into a record that borrows

A record may declare members that point into the bytes it is decoded from
instead of owning a copy. `psio::from_pssz<T>(bytes)` (validating),
`psio::from_pssz<T>(psio::trusted{bytes})`, `psio::decode<T>(psio::pssz{}, bytes)`
and `psio::decode_pssz_checked` fill them; the result is valid for as long
as `bytes` is.

```cpp
template <typename Body> struct BasicHttpRequest { std::string host; ...; Body body; };
auto req = psio::from_pssz<BasicHttpRequest<std::span<const char>>>(bytes);  // body not copied
psio::method_args_to_pssz<Svc, 0>(req);   // an argument for BasicHttpRequest<std::vector<char>>
```

| Member declared as | Borrows |
|--------------------|---------|
| `std::string_view` | a string member's characters |
| `std::span<const B>`, `B` a byte type (`char`, `signed char`, `unsigned char`, `std::byte`, `std::int8_t`, `std::uint8_t`), dynamic extent | a `std::vector<B>` member's bytes |
| `psio::pssz_view<X>` (`psio::view<const X, psio::pssz>`) | a member `X`, where its bytes are a standalone encoding of `X`: every reflected record, and any type when neither the record's cap nor a member annotation narrows its offset widths |
| `std::optional` of one of these | present or absent |
| a reflected record with such members, to any depth, and `std::vector` / `std::optional` / `std::array` of such records | member by member |

Owning members of the same record are decoded as usual. The types of the
first three rows can also be decoded by themselves
(`psio::from_pssz<std::string_view>(bytes)`).

Compile errors, each naming this list: a `std::span<const E>` of elements
wider than a byte (the wire is not aligned for `E`) or of variable-size
elements, a span with a fixed extent, a `std::vector<std::string_view>`, a
`std::reference_wrapper`, a `const char*`, a tuple of references, and a view
member whose position narrows its widths. `psio::from_pssz<T>` of a
temporary `std::vector<char>` is deleted for a `T` that borrows
(`psio::pssz_borrows_v<T>`).

Validation is of the owning type (`validate_pssz<T>` validates
`wire_type_t` member by member), bounds and `utf8` on a borrowed member are
enforced as on the owning one, and such a record `psio::packs_as` the owning
record: it can be packed again, stored, or passed as a method argument.

### "A `U` packs as a `T`": `psio::packs_as`

`psio::packs_as<U, T>` is the compile-time relation behind all of the above:
it is true exactly when `psio::to_pssz(const U&)` yields the bytes of a `T`,
so that they validate, decode and view as a `T`.

| `U` | packs as `T` when |
|-----|-------------------|
| `T`, or a source of it: `psio::pssz_view<T>`, `std::cref(t)`, `std::string_view` for `std::string`, `std::span<const E>` for `std::vector<E>`, `std::tie(a, b)` for the tuple of values | always (`psio::wire_type_t<U>` is `T`) |
| a reflected record | its members, in order, pack as the members of `T` (views, string views, spans, references standing for owning members, nested to any depth), with the same `definitionWillNotChange`, the same offset widths and caps class, and the same member bounds. Member names do not matter |
| `std::vector` / `std::optional` / `std::array` / `std::variant` / tuple of such | element by element |
| `psio::shared_pssz<X>`, `psio::pssz_buffer<X>`, `psio::nested<X>` | as a whole value: when `X` packs as `T` (its bytes are the bytes of the `X`). As a *member*, a packed `X` is a byte list and packs as a packed `X` only |

Everything else is false: another member count, order or integer size, a
tuple against a record, other bounds, a type with a binary adapter against
anything but itself. The relation is symmetric.

```cpp
struct TxSuccessView { Checksum256 id; BlockNum blockNum; psio::pssz_view<const TransactionTrace> trace; };
PSIO_REFLECT(TxSuccessView, id, blockNum, trace)
static_assert(psio::packs_as<TxSuccessView, TxSuccessRecord>);

template <typename U> requires psio::packs_as<U, T>      // Table<T>::put
void put(const U& row) { store(psio::to_pssz(row)); }
```

A mismatch is one short error that names both types:
`because 'psio::packs_as<Wrong, Rec>' evaluated to false`.

`psio::to_pssz_as<T>(source)` and `psio::pssz_size_as<T>(source)` are
constrained by `psio::pssz_source_of<Source, T>` — `packs_as<Source, T>`,
or, for a method's argument tuple `T`, `packs_as` of the tuple of its
elements — and each argument of `psio::method_args_to_pssz` is written as it
stands when `psio::packs_as<Argument, Parameter>` and converted implicitly
otherwise. So a record of views is an argument for a parameter of the owning
record type.

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
| `psio::method_args_buffer<T, I>`, `psio::shared_method_args<T, I>` | the argument tuple held packed, owned / shared: `psio::pssz_buffer` / `psio::shared_pssz` of `method_arg_tuple<I>` |
| `psio::method_params_from_pssz<T, I>(psio::trusted{bytes})` | a `method_params<I>`: each argument as the method declares it |
| `psio::invoke_method_pssz<T, I>(obj, bytes)` | validates, then calls the method; returns what it returns |
| `psio::invoke_method_pssz<T, I>(obj, psio::trusted{bytes})` | calls without validating |
| `psio::call_method_pssz<T, I>(obj, psio::trusted{bytes}, result)` | calls and appends the encoded result ([the result as `returns(...)` types it](#the-result-of-a-method)); a `codec_status` |
| `psio::method_args_to_pssz<T, I>(args...)` | the caller's side: the argument tuple encoded from the arguments themselves, converted implicitly as a call converts them, none copied ([above](#encoding-a-source-as-a-named-type)) |
| `psio::method_arguments<T, I, Args...>` | concept: `args...` can be passed to the method |
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
non-owning type) validates the tuple of wire types; decoding *into* a tuple
of views or references does not compile (decode `psio::wire_type_t<Tuple>`,
view the bytes, or use `method_params_from_pssz`).

### The result of a method

What travels back is the mirror of the above. A method's `returns(...)`
makes its result a bounded type — with the offset widths of that type, its
caps, `utf8` and element rules — so a result is written and read through
the method, never with the plain codec of the return type:

```cpp
struct Directory {
   std::vector<std::string> names(std::uint32_t limit) const;
   std::optional<Entry>     find(std::uint64_t id) const;
   PSIO_REFLECT(Directory, method(names, limit, returns(max<4> | element(utf8 | max<16>))),
                           method(find, id))
};
auto bytes = psio::method_result_to_pssz<Directory, 0>(names);        // 1-byte offset table
auto back  = psio::method_result_from_pssz<Directory, 0>(bytes);      // validates, then decodes
auto view  = psio::validated_method_result_view<Directory, 0>(bytes); // validates, then views
```

| Name | Is |
|------|----|
| `psio::method_result_t<T, I>` | the type a result is encoded as: `psio::wire_type_t` of what the method returns (`X` for a `psio::pssz_view<X>`); `void` when it returns nothing |
| `psio::method_result_to_pssz<T, I>(value)` | the callee's side: a fresh buffer. `value` is the result or any source of it. Fails through `psio::codec_abort` for a value outside `returns(...)` |
| `psio::method_result_append_pssz<T, I>(out, value)` | the same, appended |
| `psio::method_result_from_pssz<T, I>(bytes)` | the caller's side: validates the bytes as the `returns(...)` type and decodes; fails through `psio::codec_abort` |
| `psio::method_result_from_pssz<T, I>(psio::trusted{bytes})` | decodes without validating |
| `psio::validate_method_result<T, I>(bytes)` | `codec_status`: are the bytes a result of the method. It accepts exactly what `decode_method_result` / `method_result_from_pssz` accept, the `sorted` / `unique` / `unique_keys` of `returns(...)` included |
| `psio::validated_method_result_view<T, I>(bytes)` | validates, then hands back the trusted view in a `psio::checked_view` (`ok()`, `error()`, `*r`, `r->…`) |
| `psio::method_result_view<T, I>`, `psio::method_result_trusted_view<T, I>` | the safe / trusted read view of a result. Where `returns(...)` narrows an offset width these read the result's own offsets; `psio::pssz_view<ReturnType>` would not |
| `psio::encode_method_result<T, I>(value, out)`, `psio::decode_method_result<T, I>(bytes, out)` | the write and the validating read returning a `codec_status` instead of failing |
| `psio::check_method_result<T, I>(value)` | only checks a value against `returns(...)` |
| `psio::method_result_buffer<T, I>`, `psio::shared_method_result<T, I>` | a result held packed, owned / shared ([below](#a-result-held-packed)) |

**An empty optional result is zero bytes** when its payload cannot be empty
(`std::optional<Entry>`, `std::optional<std::uint32_t>`:
[pssz-spec.md §5.5.1](pssz-spec.md)). "No bytes came back" and "`None` came
back" are then one frame: the empty span validates and decodes as
`std::nullopt`, its view is an empty optional view, and a present value is
its own encoding with nothing added. An optional whose payload can itself
be empty (`std::optional<std::string>`, an optional list: §5.5.2) has a
selector byte — `None` is `00`, a present value `01` and its encoding — and
no bytes are not a result of such a method. A method that returns nothing
has no result bytes: `validate_method_result` accepts exactly the empty
span, and `method_result_from_pssz` returns `void`.

#### A result held packed

`psio::method_result_buffer<T, I>` and `psio::shared_method_result<T, I>`
are to a method's result what `psio::pssz_buffer<X>` and
`psio::shared_pssz<X>` are to an `X`
([Packed values](#packed-values-nestedt-f_buffert-shared_ft)): bytes that
*are* a result of the method, owned or shared, with every member of that
table.

```cpp
psio::shared_method_result<Directory, 0> r{bytes};   // validate_method_result, then holds them
auto copy = r;                                       // same bytes, no copy
r->size();  r.view()[1];                             // psio::method_result_trusted_view<Directory, 0>
std::vector<std::string> names = r.unpack();         // read with the result's own offset widths
auto mine = psio::method_result_buffer<Directory, 0>::from(names);   // method_result_to_pssz
```

| | Is |
|---|---|
| `B{value}`, `B::from(value)`, `B::from_checked(value, out)` | `method_result_to_pssz` / `encode_method_result`: the value is checked against `returns(...)` |
| `B{bytes}`, `B::checked(bytes, out)`, `b.validate()` | `validate_method_result` |
| `b.view()`, `*b`, `b->` | `psio::method_result_trusted_view<T, I>`; `psio::as_view(b)` is `psio::method_result_view<T, I>` |
| `b.unpack()` | `method_result_from_pssz<T, I>(psio::trusted{…})`; `b.decode_checked(out)` is `decode_method_result` |

They are `psio::buffer<psio::method_result_t<T, I>, psio::pssz_method_result<T, I>, Store>`:
the format tag `psio::pssz_method_result<T, I>` is the codec of the result as
`returns(...)` types it. It is never `psio::pssz`, also when `returns(...)`
changes no byte, so `psio::pssz_buffer<ReturnType>` — which validates the
return type and knows nothing of `returns(...)` — is a different type. (A
`psio::buffer` spelled with a result view's own format,
`psio::buffer<R, psio::method_result_view<T, I>::format_type>`, has
`unpack()` too; prefer the names above.) Not for a method that returns
nothing.

The arguments need no tag of their own, their tuple type carries the
method's cap and annotations: `psio::method_args_buffer<T, I>` and
`psio::shared_method_args<T, I>` are `psio::pssz_buffer` / `psio::shared_pssz`
of `reflect<T>::method_arg_tuple<I>`, and
`psio::method_args_trusted_view<T, I>` is the trusted form of
`psio::method_args_view<T, I>`.

**In a schema.** `psio::SchemaBuilder::insert_interface<T>()` gives each
`Func` the bounded result type. A schema that lists functions without the
`Interface` envelope takes them one method at a time:
`builder.insert_method<T, I>()` is the `FunctionType{params, result}`,
`builder.insert_method_args<T, I>()` and
`builder.insert_method_result<T, I>()` are its two nodes. (Not
`builder.insert<psio::method_result_t<T, I>>()`: that is the plain return
type, without `returns(...)`.) Rust: `M::result_to_pssz` /
`result_from_pssz` / `result_from_pssz_trusted` / `validate_result` /
`result_view` on the argument record `M`, and
`SchemaBuilder::insert_method::<M>()`.

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
