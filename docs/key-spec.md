# The `key` format

`key` encodes a value as a byte string whose **bytewise (memcmp) order is the
value's order**. It is the format for database keys: C++ `psio::key`
(`psio/key.hpp`), Rust `psio::key`, JS `PsszSchema.type(id).encodeKey`.
The shared fixture is `conformance/time-key-vectors.json`.

## 1. The two properties

For every key type `T`:

1. **Order-preserving.** `a < b` ⇔ `key(a) <bytes key(b)`, and `a == b` ⇔
   `key(a) == key(b)`. (Two documented exceptions for floats, §4.)
2. **Prefix-free.** No `key(a)` is a proper prefix of a `key(b)`.

Property 2 is what makes concatenation safe, and everything composite follows
from it by induction: if every element type has both properties, so does the
concatenation of the elements, compared lexicographically.

## 2. Encodings

| Type | Bytes |
|---|---|
| bool | `00` / `01` |
| unsigned integer, N bytes | big-endian |
| signed integer, N bytes | two's complement with the sign bit flipped, big-endian |
| u128 / u256, i128 | as above at 16 / 32 bytes |
| enum without payload (C++ `enum`, with or without `PSIO_REFLECT_ENUM`; Rust field-less `#[repr(int)]` enum, including its `#[psio(other)]` catch-all; Schema IR `Enum`) | its underlying integer. Enumerator names exist in JSON only; key order is the order of the values |
| char, byte | the byte itself (unsigned order) |
| f32 / f64 | IEEE bits; `-0` is first replaced by `+0`; then if the sign bit is set flip **every** bit, otherwise flip the sign bit; big-endian |
| duration, time point, monotonic time point | the tick count as its integer type (`docs/time.md`) — chronological order |
| string (raw — Schema IR `string` — or UTF-8 — `utf8`; bounded or not) | the bytes with each `00` written as `00 01`, then the terminator `00 00`. The two kinds have one key form; a UTF-8 string is validated when read, a raw string holds any bytes (JS: `string` for UTF-8, `Uint8Array` for raw) |
| list of octets (u8, i8, char, byte; bounded or not) | each element's one key byte (i8: sign-flipped), escaped and terminated exactly like a string |
| list of any other `T` (bounded or not) | for each element `01` + key(element); then `00` |
| optional | `00` for none; `01` + key(value) |
| fixed array `[T; N]`, byte array | the N element keys concatenated, no framing |
| bitvector<N> | its `(N+7)/8` bytes (equality only: bit 0 is the low bit of byte 0) |
| variant | one byte: the alternative's index; then key(alternative) |
| tuple, pair, record | the element / field keys concatenated in order. `tuple<>` and an empty record are zero bytes. A pair (C++ `std::pair`) is the 2-tuple |
| map, set (`docs/pssz-spec.md` §5.9) | the key of the list of its entries / elements **in ascending key order**: `01` + key(k) + key(v) per entry, then `00`; for a set `01` + key(element) per element, then `00`; and the octet framing for a set of octets. So `set<u8>` has the key of the sorted `vector<u8>`, and maps order lexicographically by their sorted entries |
| transparent type (`docs/pssz-spec.md` §5.10) | the key of its member: a transparent wrapper of a fixed-width key is fixed-width, of a composite key is that composite key |
| boxed value, custom type (Schema IR `Custom` ids that are not part of the binary form) | the key of the inner type: a text form plays no part in a key |
| nested value (C++ `psio::nested<T>`, Schema IR `Nested`; `docs/pssz-spec.md` §5.8) | the key of the byte list holding the PSSZ encoding of the inner value: the octet-list framing above (`00` → `00 01`, terminator `00 00`). **Not** the key of the inner value |

Not key types (compile errors in C++): `bitlist`, and the containers that are
not PSIO values (`std::multimap`, `std::multiset` and their unordered
forms). A variant has at most 256 alternatives.

**Keys order maps and sets.** The entries of a map and the elements of a set
are ordered, in every format, by the bytes this document defines for their
key type, and two entries are duplicates when those bytes are equal
(`docs/pssz-spec.md` §5.9). The key type of a map must therefore be a key
type by the table above — not one that supplies its own bytes (below): its
order would not be visible in the schema.

**Custom types** provide their own bytes: C++ registers a
`sortable_binary_category` adapter (`PSIO_ADAPTER`), Rust implements the key
traits. `key` adds no framing, so the custom encoding must itself have both
properties of §1 (fixed width, or self-terminating).

Why the framings work: a string's terminator `00 00` sorts below every
continuation (`00 01` or a non-zero byte), so a shorter string sorts first
and what follows the string is never compared against string content. A list
element marker `01` sorts above the list terminator `00` for the same reason,
and an absent optional `00` sorts below a present one `01`. Empty string
(`00 00`), absent optional (`00`), present-empty optional (`01 00 00`) and
the one-NUL string (`00 01 00 00`) are all distinct.

## 3. Composite keys

The key of `tuple<A, B>` is a byte-prefix of the key of `tuple<A, B, C>` for
equal `A`, `B`; tuple keys order lexicographically by element, including when
a variable-length element is followed by more elements. A record's key is the
key of the tuple of its fields; reflected methods are not part of it. A pair
is a composite key of two elements. A
transparent wrapper of a composite key type is that composite key type: its
elements are the wrapped type's (`key_prefix<Wrapper>(a)` is
`key_prefix<tuple<A, B>>(a)`).

A database layer needs three operations; each implementation provides them:

* **Prefix key** — the key of the first K elements of a composite key type,
  with each given element converted to and encoded as the key type's element
  type at that position (so a literal `7` for a `u32` element is four bytes).
  K = 0 is the empty prefix (every key); K = N is the full key.
  C++ `psio::key_prefix<Key>(e0, …)`, Rust `psio::key::key_prefix::<Key, _>(&(e0, …))`,
  JS `type.encodeKeyPrefix([e0, …])`.
* **Prefix upper bound** — the smallest byte string greater than every byte
  string that starts with `prefix`: drop trailing `FF` bytes, then increment
  the last byte. If nothing is left (empty or all-`FF` prefix) there is **no
  bound**: the scan runs to the end of the key space. The keys with the
  prefix are exactly `prefix <= k < bound`.
  C++ `psio::key_prefix_upper_bound(bytes)` → `optional`, Rust
  `key_prefix_upper_bound(&[u8])` → `Option<Vec<u8>>`, JS
  `keyPrefixUpperBound(bytes)` → `Uint8Array | null`.
* **Decode** a full key back to its tuple: the ordinary `decode`.

**Extending a key that is already bytes (C++).** A stored or received
prefix — the key of a table and index, say — is extended without decoding
or re-encoding it: `psio::raw_key{bytes}` is an element whose key is its
bytes verbatim.

```cpp
std::vector<char> prefix = psio::encode(psio::key{}, std::tuple{table, index});
auto k = psio::to_key(std::tuple{psio::raw_key{prefix}, account, std::string_view{name}});
// the bytes of to_key(std::tuple{table, index, account, std::string{name}})
auto p = psio::key_prefix<std::tuple<psio::raw_key, Account, std::string>>(psio::raw_key{prefix}, account);
```

The caller vouches that the bytes are whole keys of whatever they stand for;
that is what keeps the composite prefix-free and ordered. A `raw_key` is
write-only (its bytes do not say where they end, so a type holding one has
no `decode` / `validate`: a compile error), and it is not a schema type: the
schema of such a key is that of the elements the bytes stand for.

`psio::to_key` also takes values that do not own their data and writes the
key of what they refer to: a `std::string_view` or C string (the key of the
`std::string`), a `std::reference_wrapper`, a tuple of references
(`std::tie`), a `psio::view`. These too are write-only.

## 4. Where byte order is not the language's `<`

* **Float NaN.** `<` is unordered for NaN; the key is not: a NaN with the sign
  bit clear sorts above `+inf`, one with it set sorts below `-inf`, and NaNs
  with different payloads are different keys.
* **Float zero.** `-0` and `+0` are equal and share one key; it decodes to
  `+0`. The byte pattern that would decode to `-0` is rejected.
* **`char` lists (C++).** `char` is a raw byte, so a `vector<char>` orders
  like the `std::string` with the same bytes — unsigned — whatever the
  platform's `char` signedness. `int8_t` lists order as signed numbers.
* **Strings** order by their bytes (for UTF-8, code point order), not by any
  collation.
* **Nested values** order by the bytes of their inner PSSZ encoding, which
  is not the order of the inner values: integers are little-endian there
  (`nested<u32>` holding 256, bytes `00 01 00 00`, sorts before the one
  holding 1, bytes `01 00 00 00`) and a record starts with its header. A
  nested value is an equality / grouping key element; where the inner
  value's order matters, make the inner fields key elements themselves.
  (C++ `psio::nested<T>` has `==` and no `<`.)

## 5. Reading

`validate` accepts exactly the canonical encodings of one value spanning the
whole buffer: bool and presence / element markers are `00` / `01`, escapes
are `00 01`, strings and lists are terminated, bounded types are within
bounds (a bounded type's own bound, and a record member's declared length
bound — C++ `length_bound`, Schema IR `lengthBound` — exactly as in PSSZ),
UTF-8 string types hold UTF-8, a variant index is in range, unused
bitvector bits are zero, a float is not the image of `-0`, the bytes of a
nested value are a valid PSSZ encoding of its inner type (the compatible
rule for `validate` and `decode`, the exact rule — no unknown fields,
canonical — for `validate_strict`; not checked for a C++ member declared
`nested<T, nested_validation::deferred>`), the entries of a map and the
elements of a set are strictly ascending (`key: map entries are not in
strictly ascending key order` / `key: set elements are not in strictly
ascending order`), and nothing trails. `decode` runs the same checks and
fails on anything `validate`
rejects; it never reads out of bounds.

Encoding a map or set writes the ascending order whatever the container's
own order is, and fails on two entries with equal key bytes (`-0` and `+0`
held apart by a custom comparator).

**Type invariants** (`docs/pssz-spec.md` §18) are part of both directions:
a value that violates its type's invariant has no key — `encode` and
`to_key` fail on it (a sizing pass alone does not check) — and `validate`,
`validate_strict` and `decode` reject the key of such a value with the
invariant's own status, after its bytes were read. Nested values are
checked where they occur, innermost first. `key_prefix` encodes elements:
each is checked as the value it is, the composite as a whole is not (a
prefix is not a value of the composite type). A reader that knows only the
wire type (a schema with no handler) accepts such a key: the fixture kind
`key-invariant` (`conformance/time-key-vectors.md`).

### 5.1 Writing: bounds and UTF-8

What a reader rejects, a writer does not write. A value outside a length
bound or a UTF-8 constraint of its type — a bounded string or list, a UTF-8
string, a record member's declared bound at any depth (`docs/pssz-spec.md`
§3.5) — **has no key**, exactly as it has no PSSZ encoding (§16 there):

| | writer |
|---|---|
| C++ | `psio::to_key` / `psio::encode(psio::key{}, ...)` / `psio::key_prefix` fail through `codec_exception` (abort without exceptions); `psio::encode_key_checked(value, out)` returns the status and leaves `out` unchanged. A key type with no bound and no UTF-8 constraint is not checked at all (decided at compile time) |
| Rust | the derived `Key::key_pack` checks a member's annotations; the schema-driven key encoder checks member bounds |
| JavaScript, Python | `toKey` / `to_key` raise |

The messages of the C++ writer are `key: member length_bound violated`,
`key: utf8_string exceeds its bound`, `key: utf8_string is not valid UTF-8`
and `key: bounded list exceeds its bound` (and the UTF-8 validator's own
for an annotated member). The shared fixture's `KBound` rows
(`conformance/time-key-vectors.md`) give keys of values at the bounds and
the keys an unchecked writer would produce past them, which every reader
rejects.

## 6. Keys from a schema

A reader that has only the Schema IR encodes the key of a dynamic value with
C++ `psio::pssz_ir::to_key(schema, root, value, out)` (Rust
`psio::pssz::dynamic`, JS `type.encodeKey`): the bytes of the table above for
the type the schema describes. The value is checked against the schema
first; the entries of a map / set may be given in any order. This is the
encoder the schema-driven codecs use to check and produce the order of maps
and sets. Two things the schema does not carry: a C++ / Rust type's own key
adapter (the key is that of its wire type), and whether an 8-bit integer
list element was an enum without names (it is an octet).
