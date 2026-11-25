# Protocol Buffers

`psio::protobuf` (`<psio/protobuf.hpp>`) reads and writes proto3 messages
from the same reflected C++ types every other format uses, and
`psio::emit_protobuf` / `psio::parse_protobuf` (`<psio/emit_protobuf.hpp>`,
`<psio/protobuf_parser.hpp>`) write and read the `.proto` file that goes
with them. No dependency on the protobuf library.

```cpp
struct Profile { std::uint64_t id = 0; std::string name; bool verified = false; };
PSIO_REFLECT(Profile, id, name, verified)

auto bytes = psio::to_protobuf(profile);                    // std::vector<char>
psio::to_protobuf(profile, sink);                           // appends to a std::vector<char>
auto size  = psio::protobuf_size(profile);
auto st    = psio::validate_protobuf<Profile>(bytes);       // codec_status; stores nothing
auto value = psio::from_protobuf<Profile>(bytes);           // one pass: checks while it decodes

Profile out;
auto st2 = psio::protobuf::decode_checked(bytes, out);      // the same, as a status
auto st3 = psio::validate_protobuf<Profile>(bytes, {.max_depth = 16});

psio::SchemaBuilder b;
b.insert<Profile>("Profile");
std::string proto = psio::emit_protobuf(b.build());         // the .proto protoc compiles
psio::Schema ir   = psio::parse_protobuf(proto);            // and back
```

(These are the cases of `cpp/tests/protobuf_tests.cpp` and
`protobuf_interop_tests.cpp`, which is where they are compiled and run.)

## Interop guarantees

Checked against the official implementation (protoc / libprotobuf 34.0 from
Homebrew) by `psio_protobuf_interop_tests` and `psio_protobuf_schema_tests`:

* **The emitted schema is the codec's.** `protoc` compiles the `.proto`
  that `emit_protobuf` writes for the test shapes
  (`cpp/tests/protobuf_shapes.hpp`: every row of the mapping below); the
  test links the generated C++.
* **psio → official.** For 34,503 random values of 13 message types, psio's
  bytes parse with no unknown field anywhere, and the official
  deterministic serialisation of what was parsed is **the same bytes**.
* **official → psio.** Messages the official library builds from text
  format (13 of them, covering every shape, zero values that are present,
  enum values without a name) decode to the C++ value written in the test,
  and psio writes that value as the official bytes.
* **The two readers agree.** On 300,000 corruptions of valid messages
  (bytes replaced, flipped, inserted, removed; truncations; another
  message appended; fields inserted), everything psio accepts the official
  parser accepts **as the same value**. What the official parser accepts
  and psio refuses is listed under [Validation](#validation).
* **The nesting limit is the official one** (100): the same verdict at
  depths 99, 100, 101.
* **A `.proto` file keeps its descriptor** (what `protoc` compiles it to)
  through `parse_protobuf` → `emit_protobuf`: four hand-written files
  (proto3 with everything, proto2, no `syntax`, an imported file), the
  emitted schema of the shapes, and 300 generated schemas.

## The mapping

| C++ | proto3 | on the wire |
|---|---|---|
| `bool` | `bool` | varint |
| `int8_t`, `int16_t`, `int32_t` | `int32`; `pb_sint`: `sint32`; `pb_fixed`: `sfixed32` | varint (sign-extended to 64 bits) / zigzag / 4 bytes |
| `uint8_t`, `uint16_t`, `uint32_t`, `char` | `uint32`; `pb_fixed`: `fixed32` | varint / 4 bytes |
| `int64_t` | `int64`; `pb_sint`: `sint64`; `pb_fixed`: `sfixed64` | varint / zigzag / 8 bytes |
| `uint64_t` | `uint64`; `pb_fixed`: `fixed64` | varint / 8 bytes |
| `float`, `double` | `float`, `double` | 4 / 8 bytes |
| an enum with `PSIO_REFLECT_ENUM` (32 bits or fewer) | `enum` | varint of the value as an int32 |
| any other enum | its underlying integer | as that integer |
| `std::string` | `string` | length, bytes; **UTF-8 is checked when read** |
| `std::vector<char \| unsigned char \| std::byte>` | `bytes` | length, bytes |
| `std::array<char \| unsigned char \| std::byte, N>` | `bytes` | exactly N bytes (another length is refused) |
| a reflected record | `message` | length, fields |
| `std::optional<X>` | `optional X` | present when engaged, also when zero |
| `std::vector<X>`, `std::set<X>` | `repeated X` | scalars packed; strings, bytes, records one field each |
| `std::map<K, V>` (K an integer, `bool` or `std::string`) | `map<K, V>` | one entry message per pair, in key order, key (1) and value (2) both written |
| `std::variant<A…>` | `oneof` | the active alternative, also when zero |
| `std::variant<std::monostate, A…>` | `oneof` | `std::monostate`: none set |

X, V and an alternative are a scalar, a string, bytes or a record. What
protobuf does not have does not compile in the codec (a `static_assert`
naming the rule) and is a `std::runtime_error` in `emit_protobuf`: a list
of lists, an optional list, a list of optionals or variants, a tuple or
pair, a `std::array` that is not bytes, 128-bit integers,
`std::string_view` members (encode only), `psio::nested<T>` (the emitter
writes `bytes`; the codec does not take it).

`pb_fixed` and `pb_sint` (`psio/annotate.hpp`) apply to an integer member or
to the integers an optional / list / set member holds:
`PSIO_REFLECT(M, attr(delta, pb_sint), attr(hash, pb_fixed))`. On anything
else they do not compile. They are recorded in the Schema IR
([Schema](#schema)).

### Field numbers

A member's number is `attr(member, field<N>)`, else its 1-based declaration
position ([annotations.md](annotations.md), `field<N>`). A variant member
takes **one number per alternative, counting up from its own**
(`std::monostate` takes none): with `attr(pick, field<5>)` on a
`std::variant<std::monostate, int, std::string>`, the alternatives are 5 and
6. Numbers are checked at compile time: distinct, within 1..536,870,911,
outside 19,000..19,999. A variant with two alternatives followed by a member
without a `field<N>` therefore does not compile — give the members after it
their numbers.

Fields are written in ascending number order (what the official C++
implementation does), whatever the declaration order.

### Presence and default values

This is proto3:

* A member that is **not optional** has implicit presence: it is not
  written when it is its zero value — 0, `false`, `+0.0` (`-0.0` is
  written), an empty string / bytes / list / map, a `std::array` of zero
  bytes, a record all of whose members are zero — and an absent field reads
  as that zero value.
* `std::optional` and the alternatives of a variant have explicit presence:
  written when set, zero or not.
* **A C++ member initialiser is not a default.** `std::int32_t level = 7;`
  reads as 0 from a message without the field, as it does for every proto3
  peer, and 7 is written like any non-zero value. `from_protobuf` starts
  from the zero value of every member, not from `T{}`.

Why not the initialiser: proto3 has no declared defaults. A proto3 writer
omits a field that is 0, so a reader that filled an absent `level` with 7
would turn a peer's 0 into 7, and psio's own writer — which must omit zeros
to produce the bytes the official library produces — could not send a 0 to
itself. proto2's `[default = 7]` can express an initialiser, but it changes
the *reader's* meaning of absence for every peer and only exists in proto2
files; psio does not use it for reflected types (the parser and emitter keep
it for proto2 files — below). Use `std::optional` when "not set" must differ
from zero. (FlatBuffers and Cap'n Proto do take the initialiser as the
field's default — [flatbuffers.md](flatbuffers.md), [capnp.md](capnp.md) —
because those schema languages declare defaults.)

The initialiser still appears in the Schema IR as the member attribute
`default`; `emit_protobuf` writes it only into a proto2 file.

### Enums

A reflected enum is a proto3 enum, which is **open**: a value without a name
is kept (`static_cast<Color>(77)` round-trips), as long as the enum's
underlying type holds it — a value outside an 8-bit enum's range is refused,
not truncated. proto3 requires the first value of an enum to be zero, and an
absent field reads as zero: when the C++ enum has no zero enumerator, the
emitted enum gets one (`Level_UNSPECIFIED = 0`); when its zero is not first,
it is moved first. Enum values are names of the enclosing scope in protobuf;
a second enum using a name again gets the name prefixed
(`Record_mode_plain`).

### Reading

* Fields in any order; a singular field that occurs twice is the last one;
  a record field that occurs twice is **merged**; a oneof is the last
  alternative on the wire.
* Repeated scalars packed or unpacked or both (lists concatenate).
* Map entries with value before key, without key or value (zero), with
  unknown fields; a key that occurs twice is the last one.
* 64-bit varints for 32-bit fields are truncated; any non-zero varint is
  `true`.
* **Unknown fields are skipped and not kept**: a field number the type does
  not have, or a known number with another wire type (which is how every
  implementation treats it). A typed C++ record has nowhere to hold them;
  decode → encode drops them. Groups (wire types 3 / 4) are skipped as a
  whole.

### proto2 and editions

The codec implements proto3. Against a proto2 peer:

| | |
|---|---|
| `optional` scalars, strings, messages | ↔ `std::optional<X>`: the same wire |
| `repeated` | the same wire; psio writes scalars packed, which every proto2 parser since 2.3 reads whether or not the field says `[packed = true]` |
| `required` | **not supported**: psio does not write a zero member, and a proto2 reader refuses a message without a required field. (An engaged `std::optional` is written; the schema has to say `optional`.) |
| `[default = v]` | **not supported**: psio reads an absent field as zero |
| groups | skipped when read, never written |
| extensions | skipped as unknown fields |
| closed enums | psio's enums are open |

Editions (`edition = "2023";`) are not read by `parse_protobuf`, and the
codec has no feature switches; a 2023-edition message whose fields use the
proto3 behaviours (`field_presence = IMPLICIT`, packed, open enums, UTF-8
validation) has the wire psio writes.

`parse_protobuf` / `emit_protobuf` handle proto2 **files** in full except
groups and `extend` ([Schema](#schema)).

## Validation

`validate_protobuf<T>` and the decode are **one reader**: validation is the
decode with nowhere to store, so they accept the same bytes
(`psio_protobuf_fuzz_tests` compares the two verdicts on 522,270 inputs).
`from_protobuf<T>(bytes)` runs it once — it does not validate and then
decode. The reader is safe on arbitrary bytes:

* every read is bounded by the end of the enclosing message;
* a length is compared with the remaining input **before** anything is
  reserved or copied (a field that claims 2 GiB in a 7-byte message is
  refused at the length);
* nesting is bounded: `psio::protobuf::limits{.max_depth = 100}` — records
  inside the root, map entries and skipped groups all count;
* a message is at most 2 GiB − 1 (reading and writing);
* work is linear in the input, and memory is at most a constant times the
  input: the worst cases are an empty record of a list (2 bytes become one
  element) and a one-byte varint in a list of 64-bit integers.
  `maxDynamicData` on the root type bounds the input's size, as for every
  format.

**What the type declares.** A member's `max<N>` / `min<N>` / `exact<N>`,
a bound below it (`element`, `keys`, `values`), `sorted` / `unique` /
`unique_keys` and an invariant are part of the type in protobuf too
([annotations.md §7.2](annotations.md#72-what-is-checked-where)):
`validate_protobuf` and `from_protobuf` refuse a message whose value is
outside them (`protobuf: member length_bound violated`, the `sorted`
messages, the invariant's own; offset 0). A member may be written more
than once — the last scalar wins, repeated members and messages merge — so
the declarations are held against the finished value, after the reader has
accepted the bytes. For such a type `validate_protobuf` reads into a value
(it stores nothing for a type that declares nothing). `to_protobuf` does
not check. Rust: the same, with the same messages
(`PbMessage::pb_message_check_declared`).

Stricter than the official C++ parser, deliberately:

| psio refuses | the official parser | why |
|---|---|---|
| a value outside the range of an `int8_t` / `int16_t` / `uint8_t` / `uint16_t` / `char` member or of a narrow enum | accepts (the schema says `int32`) | truncating would decode to a value that is not the one in the message |
| bytes of another length for a `std::array<…, N>` | accepts (`bytes`) | the member has N bytes |
| a varint of ten bytes whose last byte is above 1 | drops the excess bits | no writer produces it (Go's implementation refuses it too) |
| a tag that does not fit 32 bits or takes more than five bytes | drops the excess bits | the same |

A string that is not UTF-8 is refused by both. Encoding does not check a
`std::string`'s content: a string that is not UTF-8 is written, and every
proto3 reader (psio's included) refuses the message.

Errors are `psio::codec_status` with the message (`protobuf: …`) and the
byte offset.

## Why there are no views

The other binary formats have `F_view<T>` because a field can be found
without reading what precedes it. A protobuf message has no index: a field
is found by scanning tags from the start (and the *last* occurrence wins,
so by scanning to the end), a nested record again inside it. A view would
be a scan per access; reading a message once into a value is the honest
interface, and `psio::protobuf_view` does not exist.

## Schema

`psio::emit_protobuf(schema)` writes the `.proto` for a Schema IR — one
built from reflected types by `psio::SchemaBuilder`, or one read by
`psio::parse_protobuf`. How the IR is read as protobuf is the header comment
of `psio/emit_protobuf.hpp`; in short:

* a named record is a message; a name `Outer.Inner` nests it in `Outer`;
  a record or enum without a name is named after its first user
  (`Order_kind`); other names are made identifiers (`a::Config` →
  `a__Config`);
* a member's number is its `field` attribute, else its position; a variant
  member is a `oneof` numbered as the codec numbers it (or by the `field`
  attribute of an alternative, which is how a parsed file keeps its
  numbers); field names are unique in a message, so an alternative's name
  is prefixed with the oneof's when it would repeat;
* `protobuf` = `sint` | `fixed` picks `sint32` / `sfixed32` / `fixed32` /
  …; `deprecated` is `[deprecated = true]`;
* what protobuf does not have is a `std::runtime_error` naming the member.

`psio::parse_protobuf(text)` keeps everything the wire or the meaning
depends on (`psio/protobuf_parser.hpp` lists each one):

| in the file | in the IR |
|---|---|
| `syntax` | package attribute `proto_syntax` = `proto2` (absent: proto3) |
| `package a.b;` | `Schema::package.name` (`a:b`) |
| `import [public\|weak] "x.proto";` | package attribute `proto_import` (kept, **not followed**: a type the file does not declare stays a reference by its name as written) |
| `option name = value;` | `proto_option` on the package / message / named enum / service / method (the text) |
| messages nested to any depth | `Outer.Mid.Leaf`; references resolved by protobuf's scoping rules to the declaration's name |
| field numbers | `field` |
| `sint*` / `fixed*` / `sfixed*` | `protobuf` = `sint` / `fixed` (`protobuf_key` / `protobuf_value` in a map) |
| `optional` / `repeated` / `required` | `Option` / `List` / attribute `required` |
| `map<K, V>` | `Custom{List{Tuple{K, V}}, "map"}` |
| `oneof` beside fields, several of them | a `Variant` member each (first alternative `none`), every alternative with its `field` |
| enum values (`= 5`, negative, aliases) | `Enum` over `Int{32, signed}` with the values as written |
| `[deprecated = true]`, `[default = v]` | `deprecated`, `default` |
| any other field / enum value option (`packed`, `json_name`, …) | `proto_option` / `proto_option.NAME` (the text) |
| `reserved`, `extensions` | `proto_reserved`, `proto_extensions` (the text) |
| `service` / `rpc` / `stream` | `Interface` / `Func` (`request` parameter, result; `proto_client_streaming`, `proto_server_streaming`) |

Names are kept as written. Refused with line and column
(`psio::protobuf_parse_error`): `edition`, `extend`, groups, a field number
protobuf refuses or a message uses twice, a type that is not declared in a
file without imports, a proto3 enum whose first value is not zero.

The parser is linear in the text (`psio_protobuf_parser_tests` holds
320,000 messages to a small multiple of eight times what 40,000 take; the
earlier parser was quadratic), nesting is bounded (64), and it
is fuzzed with the other schema parsers (`psio_schema_text_fuzz_tests`, also
under AddressSanitizer + UndefinedBehaviorSanitizer): every prefix and every
byte of the test files, random edits, random bytes, and inputs built to
exhaust it. Whatever it accepts, `emit_protobuf` writes text that parses
again to the same text.

### What the text cannot say

* `std::set` is `repeated`, a `std::array` of bytes is `bytes`, bounds
  (`max<N>`) and the width of an `int8_t` / `int16_t` are not in a `.proto`
  file: a schema parsed back from emitted text has `List`, unbounded bytes
  and 32-bit integers.
* A parsed `string` is the IR's `utf8`; a C++ `std::string` is the IR's
  `string` (raw bytes for the formats that do not check). Both are emitted
  as `string`.
* A member initialiser (`default`) is not written into a proto3 file.
* An empty rpc body `{}` is written as `;` (the descriptor differs by an
  empty options message); comments and the order of nested declarations
  relative to fields are not kept (the descriptor does not hold them).
* Options are kept as text and not interpreted: a custom option
  (`option (my.opt) = …`) needs its import, which is kept but not followed.

## Conformance vectors

`conformance/protobuf-vectors.json` (written and checked by
`psio_protobuf_conformance_emit`; the header of
`cpp/tests/protobuf_conformance_emit.cpp` documents the format): 243
messages psio writes (a reader decodes them and its writer gives the same
bytes back), 33 messages of other writers (unpacked, reordered, merged,
with unknown fields) with the bytes they re-encode to, 79 inputs that are
refused with the message and offset (among them a value outside what its
type declares, in a valid message: the `PbBounded/…` rows), and thirteen
digests: the verdict, message and offset on 30,000 corruptions of a message
of each type.

## Performance

`psio_bench_protobuf` (`cpp/benchmarks/bench_protobuf.cpp`): psio against
libprotobuf 34.0 on the same messages — the generated code of the emitted
schema, and values whose bytes are identical on both sides (the benchmark
checks that first). ns per operation, minimum of 11 trials, arm64 (Apple
M-series), Homebrew clang 22, `-O3`; two runs, on a machine that was not
quiet (1-minute load 5–7), so read differences under 10 % as noise.
`×` is official / psio: above 1, psio is faster.

Encode: `to_protobuf(v, reused vector)` against `ByteSizeLong` +
`SerializeWithCachedSizesToArray(reused buffer)`.

| shape | bytes | psio | official | × |
|---|---:|---:|---:|---:|
| Point (2 ints) | 14 | 7.0 / 7.0 | 7.0 / 6.7 | 1.00 / 0.96 |
| Scalars (15 fields) | 81 | 22.2 / 23.7 | 20.5 / 20.6 | 0.92 / 0.87 |
| Text (200-char string, 1 KiB bytes) | 1,318 | 32.3 / 33.0 | 37.1 / 37.5 | 1.15 / 1.14 |
| Repeated (3 × 256 numbers, 32 strings, 64 records) | 6,215 | 1,622 / 1,627 | 1,675 / 1,665 | 1.03 / 1.02 |
| Maps (3 × 32 entries) | 1,361 | 1,046 / 1,058 | 944 / 940 | 0.90 / 0.89 |
| Tree (341 nodes) | 4,333 | 2,917 / 2,902 | 3,461 / 3,447 | 1.19 / 1.19 |
| Order (composite) | 517 | 324 / 318 | 683 / 677 | 2.10 / 2.13 |

Decode: `protobuf::decode_checked(bytes, value)` against `ParseFromArray`,
into an object that is reused across calls and into a new one. Validate:
`validate_protobuf<T>` (the official library has no check without a parse).

| shape | psio, reused | official, reused | × | psio, new | official, new | × | psio validate |
|---|---:|---:|---:|---:|---:|---:|---:|
| Point | 3.6 / 3.6 | 12.2 / 12.5 | 3.4 / 3.5 | 3.6 / 3.5 | 13.1 / 12.4 | 3.6 / 3.5 | 3.4 / 3.4 |
| Scalars | 33.5 / 33.4 | 25.5 / 26.4 | 0.76 / 0.79 | 32.5 / 33.1 | 26.7 / 27.5 | 0.82 / 0.83 | 35.3 / 35.2 |
| Text | 43.5 / 44.0 | 60.3 / 59.2 | 1.39 / 1.35 | 83.6 / 85.6 | 144 / 145 | 1.72 / 1.70 | 22.0 / 22.3 |
| Repeated | 2,166 / 2,258 | 1,824 / 1,791 | 0.84 / 0.79 | 2,788 / 2,769 | 3,222 / 3,184 | 1.16 / 1.15 | 2,029 / 2,005 |
| Maps | 3,347 / 3,345 | 3,741 / 3,716 | 1.12 / 1.11 | 3,355 / 3,394 | 3,821 / 3,808 | 1.14 / 1.12 | 1,193 / 1,214 |
| Tree | 9,684 / 9,760 | 5,397 / 5,259 | 0.56 / 0.54 | 9,677 / 9,829 | 16,201 / 16,360 | 1.67 / 1.66 | 3,580 / 3,543 |
| Order | 871 / 873 | 856 / 859 | 0.98 / 0.98 | 1,006 / 1,021 | 1,511 / 1,537 | 1.50 / 1.50 | 439 / 440 |

Reading:

* **Encode** is at parity with the official library (0.87–1.19), and twice
  as fast on the composite message. Slower on a flat record of many scalars
  and on maps (about 10 %).
* **Decode into a new object** is faster except for the flat record of
  scalars (0.82: the official table-driven parser dispatches a field in
  less than psio's indirect call per field).
* **Decode into a reused object** is where the official library is ahead:
  a cleared protobuf message keeps its sub-messages and strings allocated
  and fills them again, a cleared C++ value (`std::vector<Tree>`) has freed
  them. Tree: 0.55; a list of numbers: 0.8 (the varints are decoded one by
  one into `push_back`). psio does not pool.
* **Validate** costs about what a decode costs for scalars (it is the same
  walk) and 40–60 % of one where the decode allocates.

Before this pass's last commit the writer tested for room on every write and
the reader compared field numbers one by one; encode of Scalars was 47 ns
and decode 54 ns.

## Not supported

* proto2 `required` and `[default = …]` in the codec; groups and
  extensions other than skipping them; closed enums; editions.
* Keeping unknown fields.
* `google.protobuf.Any` and the other well-known types have no special
  form (they are ordinary messages if declared); the JSON mapping of
  protobuf (psio's JSON is its own, [json.md](json.md)).
* Deterministic output for `std::unordered_map` (only `std::map` /
  `std::set` are mapped).
* `psio::nested<T>`, `psio::bounded` / `utf8_string` wrappers, `std::pair`
  / `std::tuple`, 128-bit integers.
* A dynamic (schema-driven) protobuf codec over a parsed `.proto` — F4.

## Rust

`psio::protobuf` (feature `protobuf`, in `all-formats`) is the same codec in
Rust: the same bytes and the same accepted set, proven on
`conformance/protobuf-vectors.json` (`rust/psio/tests/protobuf_conformance.rs`:
every accepted row decodes and re-encodes to its bytes, every row of
another writer re-encodes to the canonical bytes, every rejected row is
refused with the C++ message and offset, and the thirteen digests — the C++
verdict, message and offset on 390,000 corruptions — are reproduced).

```rust
#[derive(Debug, PartialEq, psio::Protobuf)]
struct Profile {
    id: u64,
    name: String,
    #[psio(pb_sint)]
    balance: i64,
    tags: Vec<String>,
}

let bytes = psio::to_protobuf(&profile);                      // Vec<u8>
psio::validate_protobuf::<Profile>(&bytes)?;                  // stores nothing
let value = psio::from_protobuf::<Profile>(&bytes)?;          // one pass
let n = psio::protobuf_size(&profile);
psio::validate_protobuf_with::<Profile>(&bytes, &psio::protobuf::PbLimits { max_depth: 16 })?;
```

One derive, `#[derive(Protobuf)]`: a struct is a message; a field-less
`#[repr(int)]` enum (32 bits or fewer) is a protobuf enum; an enum whose
variants hold one value each is a oneof for a member (a unit first variant
is "none set", the C++ `std::monostate`). `#[psio(field = N)]`,
`#[psio(pb_sint)]`, `#[psio(pb_fixed)]` are the C++ `field<N>`, `pb_sint`,
`pb_fixed`; numbers and the applicability of the two encodings are
compile-time assertions. The mapping is the C++ table with `String`,
`Vec<u8>` (bytes), `[u8; N]`, `Option`, `Vec`, `BTreeSet`, `BTreeMap`; a C++
`char` member is a `u8`. An absent member is its zero value, not `Default`.
`ToPsszSchema` writes the member attribute `protobuf` for the two keys.

Where Rust and C++ differ:

* An enum without a `#[psio(other)]` catch-all variant refuses a value that
  is not one of its enumerators (`protobuf: a value does not fit its
  type`): the Rust type cannot hold it. With the catch-all the accepted set
  is exactly the C++ one. Such an enum must declare a zero value.
* `validate_protobuf` does not apply `maxDynamicData`.
* `to_protobuf_into` panics on a message over 2 GiB − 1 (C++:
  `codec_abort`).
* `#[psio(skip)]` and type parameters are refused by the derive.
* The format tag is `psio::protobuf::ProtobufFormat` (the derive has the
  name `Protobuf`).

Not in Rust: `emit_protobuf` / `parse_protobuf`, `HashMap` / `HashSet`
members, typed buffer aliases, a benchmark.

## Other languages

JavaScript and Python have no protobuf codec, emitter or parser. Their
schema-driven PSSZ / JSON codecs accept the member attribute `protobuf`
(as `field`, `default`, `deprecated`) and do not read it.
