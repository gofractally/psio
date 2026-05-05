# pjson — Binary JSON with Zero-Copy Random Access

For anyone looking for a more compact, faster, self-describing
format that can losslessly represent any valid JSON grammar —
including large integers and perfect floating-point round-trip.
Single-pass JSON transcoding at simdjson speeds (no intermediate
value tree); canonical-typed field reads at memcpy speeds on the
resulting binary.

**Status:** v1 (stable). Reference implementation in
`cpp/include/psio/pjson*.hpp` (with a Rust port at
`rust/psio/src/pjson*.rs`). This document is the
normative specification.

**Audience:** anyone implementing a pjson parser, encoder, or
interoperable consumer in another language.

---

## 1. Purpose and Position

pjson is a binary serialization format for **schemaless data** —
documents whose structure is not known to the consumer at compile
time. It is the binary peer of JSON: it can losslessly carry any value
JSON's grammar can express, and it can be converted to JSON text and
back.

### Headline numbers

**Wire size — the big wins.** pjson collapses to compact dedicated
forms when JSON has structure: arrays of homogeneous-shape objects
(`row_array`, §5.2.1) and dense primitive arrays (`typed_array`,
§5.1.1). These two shapes dominate API responses, database query
results, and bulk transit:

| shape                                                  | JSON     | pjson    | ratio |
|--------------------------------------------------------|----------|----------|-------|
| 100 × `{"x":1,"y":2,"z":3}` as row_array               | 2 001 B  | 823 B    | 0.41× |
| 1 000-element `u16` typed_array (`[1,2,…,1000]`)       | 3 894 B  | 2 003 B  | 0.51× |
| 10-element `u8` typed_array (`[1,2,…,10]`)             | 22 B     | 13 B     | 0.59× |
| Single `User` record with 6 fields (mixed types)       | 96 B     | 83 B     | 0.86× |
| Single `{"x":1,"y":2,"z":3}` (no shape repetition)     | 19 B     | 19 B     | 1.00× |

(Bytes computed from the encoding rules in this document. The
bench at `cpp/benchmarks/bench_pjson_walk.cpp` prints the live
`pjson size: NNN bytes (X.X% of JSON)` ratio for representative
API payloads in its corpus.)

Heterogeneous one-off objects roughly tie compact JSON — the
slot-table overhead amortizes poorly over 3 single-character
fields. The format pays off the moment a shape is repeated or
elements are densely typed.

**Read latency — constant time regardless of payload size.** A
canonical-typed view runs a single `memcmp` against a precomputed
schema-hash template, then indexes the offset table directly:

| shape (canonical, in cache) | bytes | `view_one` ns |
|-----------------------------|-------|---------------|
| Point                       | 16    | 4.9           |
| NameRecord                  | 35    | 5.4           |
| FlatRecord                  | 51    | 4.1           |
| Record                      | 59    | 4.2           |
| Validator                   | 169   | 4.1           |

(Apple M3, clang 22.1, Release; bench snapshot
`perf_20260429T064728Z_7dfb0fb.csv` rows tagged `view_one`. Read
latency is flat across a > 10× payload size range — the lookup
is a fixed memcmp plus an offset-table index.)

Schemaless reads through `pjson_view` (no precomputed template)
run the hash-prefilter scan from §11 in ~15–25 ns on cached
buffers — no JSON re-parse, no DOM allocation.

**Transcode at simdjson speed.** JSON → pjson uses simdjson's
on-demand parser to drive a single forward-write pjson encoder —
no intermediate value tree, no DOM allocation. pjson → JSON walks
the buffer once. Both directions run at simdjson-class parse
speeds.

### Wire size by encoder aggressiveness

Wire-form size depends on the encoder's level of aggressiveness in
detecting compactable shapes (§12.1). At the minimum-cost transcode
level — emit-as-you-parse, no shape detection — pjson is approximately
the same size as compact JSON text (~102% on representative API
payloads). At canonical levels — using `row_array` for arrays of
homogeneous-shape objects (§5.2.1) and `typed_array` for
homogeneous-primitive arrays (§5.1.1) — pjson is **smaller than
JSON text** on the shapes that dominate API responses, query
results, and log payloads (typically 60–75% of compact JSON), while
still permitting random-access reads. Static-typed encode paths
(producers that know the source type at compile time) reach the
canonical size at zero detection cost.

The format is designed for:

* **Efficient random-access reads** by name or by index — sub-20 ns
  lookups on cached buffers in practice (see §11 for the algorithm and
  why a hash-prefilter scan outperforms binary-search-of-strings at
  realistic JSON field counts).
* **Order-preserving.** Fields appear on the wire in the order the
  producer emitted them. pjson does **not** impose a key-sort rule.
  Slot-by-index iteration returns fields in declaration / encounter
  order, which matches what consumers of typed records, JSON
  re-emitters, and presentation-layer iterators expect. Applications
  that want sort-based canonicalization (e.g. for cross-producer
  byte-equality) run that sort in their own layer before encoding;
  pjson's role is to preserve whatever order the layer above
  declared as significant. See §15.4.
* **Single-allocation, single-pass encode** — no DOM tape, no
  intermediate value tree. The encoder appends children forward and
  writes the index at the tail.
* **Persistence and transit** — pjson is a single self-contained byte
  string, suitable for database rows, file storage, RPC payloads, and
  content-addressed hashing.

Compared with:

* **JSON text** — pjson supports random access; JSON text requires a
  re-parse on every read.
* **Postgres `jsonb`** — pjson is publicly specified; `jsonb` is a
  Postgres-internal layout with no third-party readers.
* **MessagePack / CBOR / BSON** — pjson supports indexed lookup by
  hash, where these formats require a linear scan per object.
* **simdjson DOM** — pjson is a persistent byte form that does not
  reference the original JSON text. simdjson DOM cannot exist
  separated from its source buffer.

---

## 2. Conventions

* All multi-byte integers are **little-endian**.
* `u8`, `u16`, `u32`, `u64`, `i8`, `i32`, `i64` denote their usual
  fixed-width unsigned/signed integer types.
* `i128` is a 128-bit signed integer in two's complement.
* Bit positions are MSB = bit 7 within a byte unless otherwise stated.
* Byte ranges are written `[start, end)` (start inclusive, end
  exclusive).
* "Caller" means whatever code provides the buffer to the parser. The
  caller is always responsible for telling the parser the buffer's
  total size; pjson values do not store their own length except where
  noted.

A pjson **value** is a contiguous byte sequence whose total length is
known to the consumer. The first byte is always the **tag**.

---

## 3. Tag Byte

```
bit 7 6 5 4 3 2 1 0
    └─type─┘ └─low─┘
```

The high nibble (`bits 7..4`) is the **type code**. The low nibble
(`bits 3..0`) is **payload-specific** — for some types it carries
encoded data; for others it is reserved.

| code | type          | low nibble use                                 | raw bits after tag |
|------|---------------|------------------------------------------------|--------------------|
| 0    | `null`        | reserved (must be 0)                           | 0 bytes |
| 1    | `bool`        | the boolean value: 0 = false, 1 = true (others reserved) | 0 bytes |
| 2    | reserved      |                                                | — |
| 3    | `uint_inline` | unsigned value 0..15 (the integer)             | 0 bytes |
| 4    | `uint`        | magnitude byte count − 1 (range 1..16)         | `bc` bytes (raw LE unsigned magnitude) |
| 5    | `decimal`     | mantissa byte count − 1 (range 1..16)          | `bc` mantissa bytes + varscale (1..4 bytes) |
| 6    | `ieee_float`  | bits 2..0 = `log₂(byte_count)` — 1 = binary16, 2 = binary32, 3 = binary64, 4 = binary128 (others reserved); bit 3 = sci-source hint (0 = decimal-form source, 1 = scientific-notation source). See §4.6. | 2 / 4 / 8 / 16 bytes (raw IEEE-754, LE) |
| 7    | `negint`      | magnitude byte count − 1 (range 1..16); all-zero payload reserved | `bc` bytes (raw LE unsigned `\|value\|`) |
| 8    | `string`      | encoding flag (see §4.7); 0..2 valid, others reserved | (size − 1) bytes (length implicit from `size`) |
| 9    | reserved      |                                                | — |
| 10 (A) | `bytes`     | reserved (must be 0)                           | (size − 1) bytes (raw octets) — see §4.8 |
| 11 (B) | `array`     | layout selector — see §5.1: 0 = generic, 1..10 = typed homogeneous (element type code = low_nibble − 1), 11..15 reserved | container body — see §5 |
| 12 (C) | `object`    | layout selector — see §5.2: 0 = single object, 1 = row_array (homogeneous-shape array of objects), 2..15 reserved | container body — see §5 |
| 13–15  | reserved   |                                                | — |

Implementations must reject (return error) on any reserved tag code or
non-zero low-nibble bits in tags that mark them reserved.

---

## 4. Scalar Encodings

### 4.1 `null` (code 0)

Tag byte `0x00` alone. Container `size` is exactly 1 byte.

### 4.2 `bool` (code 1)

Single byte. The low nibble carries the boolean value:

```
0x10  →  false
0x11  →  true
```

Low-nibble values 2..15 are reserved and must cause a parse error.
Container `size` is exactly 1 byte.

### 4.3 `uint_inline` (code 3)

Tag byte alone. The value is the **unsigned** integer encoded in the
low nibble (range `0..15`). Container `size` is exactly 1 byte.

The name reflects the encoding: only non-negative values 0..15 fit
this form. Non-negative values ≥ 16 use `uint` (code 4); negative
values use `negint` (code 7).

### 4.4 `uint` (code 4) and `negint` (code 7)

Two parallel tags carry arbitrary-magnitude integers. The tag code
encodes the sign; the payload carries the magnitude as raw little-
endian unsigned bytes. There is no zigzag step, no per-value bit
transform.

```
uint   tag (1 B):  high = 4, low = bc − 1   where bc ∈ {1..16}
negint tag (1 B):  high = 7, low = bc − 1   where bc ∈ {1..16}
payload (bc B):    magnitude as raw little-endian unsigned bytes
```

A `uint` value is the unsigned integer in the payload. A `negint`
value is the negation of the unsigned integer in the payload — e.g.,
a `negint` with payload `1` decodes to logical value `−1`, payload
`128` decodes to `−128`.

For `bc ≤ 8` the magnitude fits `u64` (so `uint` covers the full
`u64` range and `negint` covers `[−2⁶³ .. −1]` plus down to
`−2⁶⁴ + 1`). For `bc ∈ 9..16` the magnitude extends through 128
unsigned bits.

`negint` with all-zero payload is **reserved**: there is no distinct
"negative zero" integer. Decoders must reject this form.

Encoders should choose the **smallest `bc`** that exactly represents
the magnitude. Decoders must accept any `bc ∈ 1..16`. There is no
canonical-encoding requirement at the format level — see §15.2 for
canonical rules.

The `uint_inline` form (code 3) is preferred over `uint` for
non-negative values 0..15.

**Encode / decode is a pure raw-byte copy.** A typed unsigned source
(`u32`, `u64`, `usize`) maps unambiguously to `uint`; a typed signed
source inspects its sign and dispatches to `uint` or `negint`. No
zigzag arithmetic, no branchy bit-fiddling — the bulk-decode
SWAR/SIMD paths in §10 handle the payload as a fixed-width unsigned
load. The split mirrors Postgres numeric, BSON, WIT canonical-ABI,
and SSZ practice — sign in the type tag, magnitude in the bytes.

### 4.5 `decimal` (code 5)

```
tag (1 B):       high = 5, low = bc − 1   where bc ∈ {1..16}
mantissa (bc B): zigzag-encoded mantissa, little-endian
varscale (1..4 B): scale, see §4.4.1
```

Represents `mantissa × 10^scale` as an exact decimal value.

#### 4.5.1 Varscale — 2-bit-prefix variable-length signed integer

The scale uses a compact variable-length encoding. The first byte's
top 2 bits give the total byte count of the varscale; the remaining
bits hold zigzag-encoded scale data, little-endian.

> **Note: this is NOT the QUIC variable-length integer encoding.**
> QUIC (RFC 9000) uses a 1/2/4/8-byte length ladder (length codes
> map to byte counts 2⁰/2¹/2²/2³). Our encoding uses a 1/2/3/4-byte
> linear ladder. Both share the "top-2-bits-encode-length" idea
> in the first byte, but the length values and per-form capacities
> differ. We chose the linear ladder because pjson's typical values
> (decimal scale, long-key excess) cluster in the 1-3-byte range
> where the linear ladder is more byte-efficient than QUIC's
> power-of-2 jumps.

```
First byte:
   bits 7..6: total byte count − 1   (0 → 1 byte, 1 → 2, 2 → 3, 3 → 4)
   bits 5..0: low 6 bits of zigzag(scale)

Subsequent bytes (if any): bits 6..(6 + 8·(n-1)) of zigzag(scale).
```

Capacities:

| total bytes | zigzag bits | scale range          |
|-------------|-------------|----------------------|
| 1           | 6           | −32..31              |
| 2           | 14          | −8 192..8 191        |
| 3           | 22          | −2 097 152..2 097 151 |
| 4           | 30          | −536 870 912..536 870 911 |

Encoders must use the smallest byte count that fits the value.

#### 4.5.2 Decimal vs IEEE float

A floating-point value may be encoded either as `decimal`
(mantissa × 10^scale) or `ieee_float` (raw IEEE-754 binary{16,32,
64,128} — see §4.6). When both encodings can represent the value
exactly:

* If the **decimal form is shorter** in bytes than the smallest
  ieee_float width that round-trips the value, encoders **should**
  choose `decimal`.
* Otherwise encoders **should** choose `ieee_float` at the
  smallest width that round-trips the value bit-exact.

Decoders must accept either form for any numeric value.

Round-tripping a JSON number through pjson preserves the **value**,
not the **textual form**. Trailing zeros (`100.00`) and scientific
notation (`1.5e10`) are normalized away.

### 4.6 `ieee_float` (code 6)

```
tag (1 B):  high = 6
            bits 2..0 = log₂(byte_count)
            bit  3    = sci-source hint
bits (N B): IEEE-754 binary{N×8}, little-endian
```

The low nibble splits into a **width selector** (bits 2..0) and a
**sci-source hint** (bit 3). The payload is the raw little-endian
bit pattern of the selected format, copied byte-for-byte:

| `low` | width bits | sci bit | byte_count | type        | sci-source hint |
|-------|------------|---------|------------|-------------|-----------------|
| 1     | 0b001      | 0       | 2          | binary16    | no              |
| 2     | 0b010      | 0       | 4          | binary32    | no              |
| 3     | 0b011      | 0       | 8          | binary64    | no              |
| 4     | 0b100      | 0       | 16         | binary128   | no              |
| 9     | 0b001      | 1       | 2          | binary16    | yes             |
| 10    | 0b010      | 1       | 4          | binary32    | yes             |
| 11    | 0b011      | 1       | 8          | binary64    | yes             |
| 12    | 0b100      | 1       | 16         | binary128   | yes             |

Width-selector codes 0 and 5..7 (and their sci-bit-set partners
8 and 13..15) are reserved; decoders **must** reject.

**Sci-source hint (bit 3, optional).** When set, the bit advises a
JSON re-emitter that the source JSON token used scientific
notation (e.g. `1.5e10`, `2E-3`). It is purely cosmetic — it does
not affect the value, and decoders that don't care about
re-emission may ignore it. Producers that don't track source
notation (or that don't care to preserve it) leave the bit clear
and the value still round-trips correctly numerically. The hint
applies only on the `ieee_float` tag — see §7.1 for the coverage
limits this implies.

The bytes are the raw IEEE-754 little-endian representation; no
endian swap, no padding. Producers without native support for a
given width may emit only the widths their language exposes — a
C-only producer typically emits binary32 and binary64; a producer
on a platform with native `_Float16` may also emit binary16.

NaN and Infinity are representable in every IEEE width but are
forbidden in JSON text. pjson encoders sourced from JSON will not
produce NaN or Infinity values; pjson encoders sourced from other
inputs may emit them. For deterministic / content-addressable
output, NaN and ±Inf use a **canonical bit pattern** specified in
§15.2 — without that rule, a NaN value has many valid bit
patterns and two encoders given "the same NaN" can produce
byte-different output.

### 4.7 `string` (code 8)

```
tag (1 B): high = 8, low = encoding flag
content (size − 1 B): UTF-8 text; interpretation per the flag
```

The low nibble carries an **encoding flag** that tells the JSON
emitter what to do with the stored text bytes.

| flag | name          | meaning                                                       |
|------|---------------|---------------------------------------------------------------|
| 0    | `raw_text`    | UTF-8 text NOT in JSON-escape form. JSON emit must run a per-character escape pass (handle `"`, `\`, control chars, etc.). |
| 1    | `escape_form` | Text already in JSON-escape form (`\"`, `\n`, `\uXXXX` etc are LITERAL bytes). JSON emit just wraps in surrounding quotes — no per-byte work. |
| 2..15 | reserved     | Implementations must reject.                                  |

The string length is always `size − 1` (no length prefix).

**Encoder choices.** A producer should set the flag according to its
source:

* JSON parser → `escape_form` (the bytes between quotes are taken
  verbatim from the source JSON).
* Typed text value where the producer doesn't know whether escape
  characters are present → `raw_text`.
* Typed text value that the producer has already validated to contain
  no characters needing escape → `escape_form` (cheaper emit later).

Raw binary blobs are NOT a string sub-flag — they have their own
`bytes` tag (§4.8).

### 4.8 `bytes` (code 10)

```
tag (1 B): high = A, low = 0 (reserved)
content (size − 1 B): raw binary bytes
```

The `bytes` tag carries a contiguous run of raw binary octets. There
is no length prefix; the length is `size − 1` from the caller-
provided value `size`. The low nibble is reserved and must be 0;
implementations must reject non-zero values.

**JSON round-trip.** JSON has no native binary literal. JSON emitters
**should** base64-encode the bytes and emit a quoted string. JSON
parsers cannot infer "this string was supposed to be bytes" from the
text alone — convention is to use a key suffix (e.g. `"avatar.b64"`)
so consumers know to base64-decode on input (§7).

**Why a separate tag (not a `string` sub-flag).** Text and binary
are conceptually distinct: text is interpreted as UTF-8 codepoints
and re-encoded for JSON; binary is opaque octets. Carrying them as
separate tags keeps the type discrimination explicit at the wire
layer and removes the binary value from the per-string flag table.

---

## 5. Containers

Containers (`array`, `object`) use a **tail-indexed** layout: child
values are written first, the index is appended at the tail. The
caller-provided `size` of the container is required to locate the
index by walking backward from the end.

The `array` tag (code 11) has **two layout variants** selected by
the low nibble:

* **Generic array** — low nibble = 0. Children may be any pjson
  value. Per-element tags + slot table. See §5.1.
* **Typed homogeneous array** — low nibble = 1..10. Children are
  all the same fixed-width primitive type. No per-element tags,
  no slot table. See §5.1.1.

The remaining low-nibble values (11..15) are reserved.

### 5.1 Generic array layout (tag = 0xB0)

```
[tag: u8 = 0xB0]
[width byte: u8]                 low 2 bits = slot_w_code (0=u8, 1=u16,
                                  2=u24, 3=u32); bits 2..7 reserved (0)
[value_data: variable]
   for each i in 0..N:
      [child value: tag + raw bits]
[slot[N]: N × slot_w bytes]      offset within value_data, LE
[count: u16 LE]                  N — number of children
```

`count` is the **last 2 bytes** of the container.

The encoder picks `slot_w_code` from the actual `value_data` size of
*this* container — independently of any outer container. A small
sub-array inside a 1 MiB document still uses `slot_w = u8` if its own
value_data fits in 256 bytes. This recursion is the highest-leverage
size optimization in pjson: most overhead bytes are slot-table bytes,
and most containers are small.

Each child's byte range within the container is:

```
child_i_start = container_start + 2 + slot[i]    // +2: skip tag + width byte
child_i_end   = container_start + 2 + (i + 1 < N
                                       ? slot[i+1]
                                       : value_data_size)
child_i_size  = child_i_end − child_i_start
```

where `value_data_size = container_size − 2 − slot_w·N − 2`
(subtracting tag, width byte, slot table, and count).

### 5.1.1 Typed homogeneous array layout (tag = 0xB{1..A})

```
[tag: u8 = 0xB{code+1}]              code ∈ {0..9}
[element_0 .. element_{N-1}]         N × element_size raw bytes, LE
[count: u16 LE]                      N — number of elements
```

The wire low-nibble is `code + 1` (so that low_nibble = 0 stays
reserved for the generic-array layout). The element type code is
recovered as `low_nibble − 1`.

Total size: `1 + N × element_size + 2`. Random access reduces to a
single offset computation: `element_i = container_start + 1 + i ×
element_size`. There is no per-element tag and no slot table.

**Element type codes** (low_nibble − 1):

| code | element type | bytes | range / format             |
|------|--------------|-------|----------------------------|
| 0    | `i8`         | 1     | −128..127                  |
| 1    | `i16`        | 2     | −32 768..32 767            |
| 2    | `i32`        | 4     | −2³¹..2³¹−1                |
| 3    | `i64`        | 8     | −2⁶³..2⁶³−1                |
| 4    | `u8`         | 1     | 0..255                     |
| 5    | `u16`        | 2     | 0..65 535                  |
| 6    | `u32`        | 4     | 0..2³²−1                   |
| 7    | `u64`        | 8     | 0..2⁶⁴−1                   |
| 8    | `f32`        | 4     | IEEE-754 binary32          |
| 9    | `f64`        | 8     | IEEE-754 binary64          |
| 10–14 | reserved    | —     | implementations must reject (low_nibble values 11..15) |

Implementations must reject reserved codes.

**When to use vs. generic.** A producer may always encode a
homogeneous primitive array as the generic form instead — the
format does not require typed. The choice is producer-side
**density vs. encoder work**:

| producer                                                | typical choice |
|---------------------------------------------------------|----------------|
| typed-struct encoder with declared fixed-width array    | typed (zero scan — element type known statically) |
| JSON parser, default mode                               | generic (no homogeneity scan) |
| JSON parser, **strict canonical** mode (§15)            | typed when all elements fit one primitive type, smallest type that fits |
| RPC / DB writer with already-typed columns              | typed |

Wire-size comparison for an `i32[100]`:

| encoding | bytes |
|---|---|
| generic with per-element `uint`/`negint` tags | 1 (tag) + ≤ 5×100 (per-element tag+payload) + 4×100 (slot) + 2 = ~903 |
| typed `i32`                         | 1 + 4×100 + 2 = 403 |

A decoder must accept both forms for any homogeneous primitive
array. Random access is faster on the typed form (constant-stride
pointer arithmetic) than the generic form (slot-table indirect
read), but the time difference is small for small N.

**Encoding rules.**

* Element bytes are little-endian, fixed-width per the element
  type code. Floats use IEEE-754 binary representation as raw
  bytes.
* `count` is `u16 LE` (max 65 535 elements), matching §5.5.
* Element bytes need not be aligned in the wire stream; readers
  must use byte-by-byte copy rather than direct typed loads.

**JSON emission.** A typed array emits to JSON as a JSON array of
numbers. Integer codes (0..7) emit as decimal-integer text; float
codes (8, 9) emit as shortest-round-trippable decimal text. There
is no JSON-side marker indicating the array was stored as typed —
JSON consumers see a plain `[..]` of numbers.

**JSON ingestion.** A JSON parser **may** detect homogeneous
primitive arrays and emit the typed form; a default-mode parser
typically emits generic (simpler, no scan pass). When detection
runs, the parser picks the smallest signed/unsigned/float type
that holds every element exactly. A canonical encoder (§15)
**must** emit the typed form when applicable.

### 5.2 Object layout (code 12)

```
[tag: u8 = 0xC0]
[width byte: u8]                 low 2 bits = slot_w_code (0=u8, 1=u16,
                                  2=u24, 3=u32); bits 2..7 reserved (0)
[value_data: variable]
   for each i in 0..N (in encounter order):
      if slot[i].key_size <  0xFF:
         [key bytes (slot[i].key_size B)][child value]
      else (long-key escape):
         [key_excess: 2-bit-prefix varuint]
         [key bytes (0xFF + key_excess B)][child value]
[hash[N]: N × u8]
[slot[N]: N × (slot_w + 1) bytes]   offset (slot_w bytes LE) + key_size (1 byte)
[count: u16 LE]                  N — number of fields
```

`count` is the **last 2 bytes** of the container.

Each `slot[i]` is `slot_w + 1` bytes:

```
bytes [0 .. slot_w):  offset within value_data (LE)
byte  [slot_w]:       key_size (0..0xFE inline, 0xFF long-key escape)
```

The encoder picks `slot_w_code` per container from this container's
own value_data size. Recursive: a small sub-object inside a large
document uses `slot_w = u8` regardless of the outer document's size.

`hash[i]` is the 8-bit prefilter hash for field `i`'s key (see §5.3).

Each entry's byte range:

```
entry_i_start = container_start + 2 + slot[i].offset    // +2: skip tag + width
entry_i_end   = container_start + 2 + (i + 1 < N
                                       ? slot[i+1].offset
                                       : value_data_size)
```

where `value_data_size = container_size − 2 − (slot_w + 1)·N − N − 2`
(subtracting tag, width byte, slot table, hash table, and count).

Within an entry:

```
if slot[i].key_size != 0xFF:
   key_bytes        = entry[0 ..  slot[i].key_size)
   child_value_starts at entry[slot[i].key_size]

else (long-key):
   varuint excess at entry[0]
   varuint_size = 1..4 (per §4.4.1, but encoding 'unsigned' — see §5.4)
   key_size = 0xFF + excess
   key_bytes = entry[varuint_size .. varuint_size + key_size)
   child_value_starts at entry[varuint_size + key_size]
```

Field encounter order is preserved (no canonical sort).

### 5.2.1 Row-array layout (tag = 0xC1)

A **row_array** is a homogeneous-shape array of objects: every record
has the same K keys in the same order. The shared schema (key bytes
+ key prefilter hashes) lives **once** at the array level instead of
being repeated in every record. This is the dominant case for
JSON-over-API payloads (lists of records, query results, log lines)
and removes substantial per-element overhead.

```
[tag: u8 = 0xC1]
[width byte: bit 0..1 = slot_w, bit 2..3 = recoff_w, bits 4..7 reserved (must be 0)]
                width code: 0 = u8, 1 = u16, 2 = u24, 3 = u32
[K: 2-bit-prefix varuint (number of fields per record)]
[shared key slots: K × u32 LE]      packed { key_offset:24, key_size:8 }
                                     (offsets into the keys area)
[hash[K]: K × u8]                    shared 8-bit prefilter hashes
[shared keys area: contiguous key bytes, length = sum of key_sizes]
[records body: variable]
   for each i in 0..N:
      [value_data_i]                contiguous value bytes
      [slot_i[K]: K × slot_w bytes] per-record value offsets
                                     (relative to value_data_i start)
[record_offsets[N]: N × recoff_w bytes]   start of record i
                                           (relative to records-body start)
[count: u16 LE]                      N — number of records
```

`count` is the **last 2 bytes** of the container, matching §5.1 / §5.2.

#### 5.2.1.1 Adaptive widths

The encoder picks `slot_w`, `recoff_w` to fit the largest value in
each width-scaled field, packing them into the single width byte:

| width code | bytes | max value |
|---|---|---|
| 0 | 1 | 255 |
| 1 | 2 | 65 535 |
| 2 | 3 | 16 777 215 |
| 3 | 4 | 4 294 967 295 |

`slot_w` scales with the **largest single record's body size** (since
slot offsets are relative to record start). `recoff_w` scales with
the **total records-body size** (since record offsets are relative
to the records-body start).

For a 1000-record array of small objects (each ≤ 256 B, total body
< 64 KB): `slot_w = u8`, `recoff_w = u16` — minimal overhead. For
arrays beyond 16 MB body: `recoff_w = u32`.

The shared key slots use a fixed `u32` width (24-bit offset + 8-bit
size, matching the §5.2 single-object slot format) — keys areas are
always small (≤ a few KB), so adaptive width here adds complexity
without measurable benefit.

#### 5.2.1.2 Per-record body

Each record carries its own slot table at the **tail** of its body:

```
record_i_size = (i + 1 < N
                 ? record_offsets[i+1]
                 : records_body_size) − record_offsets[i]

slot_i_pos    = record_i_start + record_i_size − K × slot_w
value_data_i  = record_i_start[0 .. slot_i_pos)
```

Slot `slot_i[j]` is the offset (within record i's value_data) where
field j's value begins. Field j's value extends to the next slot's
offset (or to `slot_i_pos` for the last field).

#### 5.2.1.3 Lookup `arr[i][key]`

```
1. Read N from the last 2 bytes; compute records-body extents.
2. Read width byte → (slot_w, recoff_w).
3. record_i_start = records_body_start + record_offsets[i].
4. Hash-prefilter the SHARED hash[K] for `key_hash8(key)`. On hit,
   verify the candidate via the shared keys area → field index j.
5. Read slot_i[j] from record i's tail.
6. Value bytes live at record_i_start[slot_i[j] .. slot_i[j+1] or
   record_i_size − K·slot_w).
```

The hash-prefilter scan happens **once per array**, not once per
record — this is the primary lookup-perf win over generic `t_object`
× N elements.

#### 5.2.1.4 Wire savings vs. generic

For an array of N records with K fields each, fixed key bytes
totalling Kb bytes of keys, and fixed value sizes totalling Vb
bytes of values:

| layout | bytes (excl. constant overhead) |
|---|---|
| generic `array` of `object` | N × (1 + Kb + K + 4K + 2 + Vb) + 4N + 3 |
| `row_array` | 4K + K + Kb + N × (Vb + K·slot_w) + N·recoff_w + 4 |

The savings are `N × (1 + Kb + K + 2 + 4K − K·slot_w) − 4N + ...`,
dominated by removing per-record key bytes (`N × Kb`) and per-record
hash table (`N × K`). For typical small records the row_array form
is 30–50% smaller.

#### 5.2.1.5 Encoder rule

A producer **may** emit row_array when every JSON object in a source
array has:
- the same number of keys K,
- the same K keys (byte-equal) in the same order,
- byte-equal hash-array (derivable from keys),

When the encoder detects this on a single homogeneity-detect pass,
it emits row_array; otherwise it falls back to generic
`t_array` × `t_object`. The detection cost is one pass over the
JSON array's first object's key list, then byte-compares against
each subsequent object's first K key bytes.

Whether a producer **runs** that detection pass depends on its
aggressiveness level (§12.1). Statically-typed producers (e.g.
encoding a reflected `Vec<R>`) skip detection entirely and emit
row_array unconditionally because the schema is known; dynamic
JSON-to-pjson transcoders pay the detection cost only when the
caller asks for it.

Strict canonical encoders (§15) **must** emit row_array when
applicable.

### 5.3 Key prefilter hash

Each object's `hash[i]` byte is the **low 8 bits of XXH3-64** computed
over the field's key with any **trailing `.tag` suffix stripped**.

```
key_hash8(k):
   strip k from the last '.' onward (if any)   // "amount.decimal" → "amount"
   return XXH3_64(stripped) & 0xFF
```

Suffix stripping enables presentation-tag-suffix matching: a stored key
`"amount.decimal"` and a query key `"amount"` hash to the same byte,
allowing a fast prefilter to find candidates regardless of suffix
attachment. A full byte-equality compare on the matching slot
distinguishes them.

Decoders **must** verify the stored hash byte matches `key_hash8` of
the stored key bytes; mismatch is an error.

#### Collision behavior

The hash is 8 bits — there are 256 possible values. For an object of
N fields with random keys, the expected number of hash collisions per
query is ≈ N/256. Typical JSON objects have N ≤ 30, so the average
lookup hits one hash candidate, runs one byte-equal-string-compare,
and returns. For very wide objects (N ≈ 256) you'd expect ~1 false
positive per query — still cheap.

The lookup algorithm (§11) handles collisions correctly: on a hash
hit that fails the byte-equal key check, it advances and re-scans
the remainder. So worst-case N collisions degrade lookup to O(N) of
**string compares** rather than just O(N) of byte compares. For
N ≤ 1000 this remains acceptable (microseconds, not milliseconds).

**Adversarial / hash-flooding inputs.** An attacker who controls the
JSON keys can construct N keys that all hash to the same byte (only
~256 attempts to find one collision; finding N colliders by birthday
attack is `O(N · √256) = O(16N)` work for the attacker — trivial
preprocessing). This degrades a single lookup to N string compares.
For pjson's intended use cases (database storage of API request
payloads, internal RPC, configuration files) JSON keys are typically
**schema-defined by the application**, not attacker-controlled, so
this attack vector is rare.

If keys ARE attacker-controlled in your deployment (e.g., a generic
key-value store storing user dictionaries), an implementation may:

* Switch to a **seeded** hash. XXH3-64 takes a 64-bit seed; pick a
  random seed at process startup and use it for every `key_hash8`
  computation. Attackers can no longer precompute colliders.
  Producers and consumers in the same process share the seed; if
  pjson values are persisted across processes (database, file)
  the seed must also be persisted alongside the value or recomputed
  per-doc.
* Cap the max fields per container at the application layer below
  pjson's 65535 hard limit, bounding worst-case lookup work.
* Skip hash verification on read (see §9 errors) and rely solely on
  byte-equal key compare. Drops to O(N) per lookup unconditionally
  but eliminates the hash-flood asymmetry; ~3-5× slower for typical
  workloads.

The reference C++ implementation does **not** seed the hash —
`key_hash8` is deterministic, which makes content-addressable
hashing (two pjson buffers from the same logical input hash to the
same content-id) work cleanly. This is the right tradeoff when
keys are not attacker-controlled. Deployments that need both
deterministic hashing AND attacker-resistance should canonicalize
inputs at a higher layer (e.g., reject objects with > K fields, or
hash the key set externally).

### 5.4 Long-key escape

If a key's byte length is ≥ 255, the slot's `key_size` byte is set to
`0xFF` and the actual key length is encoded inline in the entry's
value_data region as a 2-bit-prefix **unsigned** varuint (same
encoding as §4.4.1 minus the zigzag step) for the **excess**:

```
actual_key_length = 0xFF + varuint_excess_value
```

The varuint is read forward starting at the entry's first byte. After
it, the key bytes start, then the value bytes.

Capacities (varuint of `excess`):

| varuint bytes | excess range | actual key length |
|---|---|---|
| 1 | 0..63 | 255..318 |
| 2 | 0..16 383 | 255..16 638 |
| 3 | 0..2²² − 1 | up to ≈ 4 M + 255 |
| 4 | 0..2³⁰ − 1 | up to ≈ 1 G + 255 |

In practice, JSON keys are short identifiers; the long-key escape is
defensive. Encoders should assume keys < 255 chars in the common path.

### 5.5 `count` width

The count field is a fixed `u16 LE` (2 bytes), supporting up to 65 535
fields per container. Implementations **must** reject containers with
counts beyond this on the wire. (For deeper limits, see §8 — pjson is
not designed for million-field containers.)

### 5.6 Offset width and container size limit

The `offset` field within each `slot` is **adaptive** — `slot_w` bytes
per slot, where `slot_w ∈ {1, 2, 3, 4}` is selected by the encoder
per container from the width byte's `slot_w_code` (low 2 bits — see
§5.1, §5.2). The encoder picks the smallest width that fits the
container's actual `value_data` size, so each container pays only
for the bytes it needs:

| `slot_w_code` | `slot_w` (offset bytes) | max `value_data` |
|---------------|-------------------------|------------------|
| 0             | 1 (`u8`)                | 256 B            |
| 1             | 2 (`u16`)               | 64 KiB           |
| 2             | 3 (`u24`)               | 16 MiB           |
| 3             | 4 (`u32`)               | 4 GiB            |

Picking `u8` for a small container saves 3 × N bytes versus a fixed
`u32` slot table; the cost is one branch on container open to decode
`slot_w` and a variable-width load to read each offset (typically 1–3
cycles per offset, amortized over the lookup-vs.-walk path). Canonical
typed views (§11.1) compile the width into the schema-hash template
and pay zero runtime cost for adaptivity.

Documents whose largest container exceeds 4 GiB must split into
multiple top-level containers; pjson does not currently spec a
`u64`-offset extension.

---

## 6. Document — top-level

A pjson document **is** a single value. There is no envelope, magic
number, or version marker. The format defines no document framing of
its own; consumers know they are reading pjson because their
application protocol said so (HTTP `Content-Type`, file extension,
database column type, etc.).

A consumer parses a pjson document by being given a `(ptr, size)` pair
of bytes. The first byte is the tag, and the value occupies all `size`
bytes.

If multi-document framing is needed at the application layer, a simple
prefix scheme works:

```
[size: 2-bit-prefix varuint][document bytes]    repeated
```

This framing is application-defined and not part of pjson.

---

## 7. JSON Round-trip

### 7.1 Value-preserving, not byte-preserving

JSON ↔ pjson round-trips preserve **values**, not source-text
byte-for-byte. Specifically:

* Whitespace and indentation are not preserved (compact emit only).
* Trailing zeros in decimals are normalized (`100.00` → `100`).
* Scientific notation **may** be preserved as a hint when the
  value's canonical encoding is `ieee_float` (§4.6 sci-source
  bit); for values that canonically go through `decimal` (which
  is the common case — `1.5e10` decodes to a 3-byte decimal,
  smaller than a 9-byte ieee_float), the source notation is not
  preserved and re-emit normalizes (`1.5e10` → `15000000000`).
  The bit is advisory only — a JSON re-emitter may also choose
  scientific notation based on value magnitude regardless of the
  hint.
* Field encounter order **is** preserved.

For applications requiring byte-identical round-trip (e.g., signature
verification on the exact source JSON), pjson is insufficient — use a
canonical-JSON form on both sides, or keep the original text alongside
the pjson.

### 7.2 Type mappings

| JSON | pjson tag |
|---|---|
| `null` | 0 (null) |
| `false` | 1 (bool, low nibble = 0) |
| `true` | 1 (bool, low nibble = 1) |
| integer 0..15 | 3 (uint_inline) |
| integer ≥ 16, fits `u128` | 4 (uint) with smallest bc |
| integer < 0, fits `i128` magnitude | 7 (negint) with smallest bc on `\|v\|` |
| fractional / exponent | 5 (decimal) when shortest, else 6 (ieee_float) |
| `"..."` | 8 (string) — bytes between quotes, escape-form preserved |
| `[ ... ]` (heterogeneous) | 11 (array, low nibble = 0 — generic) |
| `[ ... ]` (homogeneous primitive, canonical mode) | 11 (array, low nibble = code+1 — typed) |
| `{ ... }` | 12 (object) |

There is no JSON literal for `bytes`. JSON encoders may emit them as
base64-encoded strings under a key-suffix convention:

```
// JSON form
{"avatar.b64": "iVBORw0KGgo..."}

// pjson form
object with key "avatar" (suffix stripped on lookup hash) whose value
is a `bytes` tag holding the base64-decoded raw bytes.
```

This convention is established at the application layer; pjson stores
the bytes raw regardless of how they were textually represented.

### 7.3 Unrepresentable JSON values

JSON forbids `NaN`, `+Infinity`, `−Infinity`. pjson encoders sourcing
from JSON will not emit these. Decoders converting pjson to JSON must
reject them or signal an error.

### 7.4 Suffix vocabulary (recommended)

| suffix | meaning | typical pjson tag |
|---|---|---|
| `.b64` | base64-encoded binary | bytes (10) |
| `.hex` | lowercase hex string | bytes (10) |
| `.base58` | base58 string | bytes (10) |
| `.decimal` | decimal-digit string | int (4) or decimal (5) |
| `.rfc3339` | RFC 3339 timestamp | int (4) — nanoseconds since epoch |
| `.unix_ms` | milliseconds since Unix epoch | int (4) |
| `.unix_us` | microseconds since Unix epoch | int (4) |

The suffix is purely a JSON-side hint: pjson stores the binary form;
the consumer reattaches the suffix when emitting JSON. The hash byte
strips the suffix so `view["amount"]` and a stored key
`"amount.decimal"` land in the same hash bucket.

---

## 8. Limits

| limit | value |
|---|---|
| max fields per container | 65 535 (`u16` count) |
| max value_data bytes per container | 4 294 967 295 at `slot_w = u32`; 16 MiB at `u24`; 64 KiB at `u16`; 256 B at `u8` (encoder picks per container — see §5.6) |
| max key length | unbounded via long-key escape (§5.4); short-key path covers 0..254 bytes |
| max integer magnitude | 128 unsigned bits (16-byte raw LE) per `uint` / `negint` |
| max decimal scale | ±536 870 911 (4-byte varscale) |
| max nesting depth | implementation-defined; suggested cap: 256 |

---

## 9. Errors

A parser must detect and reject:

* Tag with reserved type code (2, 7, 9, 13–15).
* Tag with reserved low-nibble bits (e.g., `bool` low nibble > 1;
  `string` low nibble > 1; `array` low nibble 11..15; `bytes` low
  nibble != 0).
* `uint`, `negint`, or `decimal` with bc < 1 or bc > 16; `negint`
  with all-zero payload (negative-zero, reserved).
* Container with stated count yielding `slot_table_pos < 1` or
  `value_data_size > size − overhead`.
* `slot[i].offset` ≥ `value_data_size`, or `slot[i].offset` ≥
  `slot[i+1].offset` (children must be ordered, no overlap).
* `hash[i] != key_hash8(stored_key_bytes_for_i)`.
* `varscale` or long-key varuint with claimed length exceeding the
  remaining buffer.
* Invalid UTF-8 in a string (parser may opt to reject; the spec does
  not mandate UTF-8 validation, but consumers needing it should
  validate).

---

## 10. Reference Algorithm — Read

```
parse_value(ptr, size) -> Value:
   tag  = ptr[0]
   type = tag >> 4
   low  = tag & 0x0F

   case type of
   0: return Null()                                       // tag 0x00
   1: assert low ≤ 1                                      // 0x10 = false, 0x11 = true
      return Bool(low == 1)
   3: return UInt(low)                                    // 0..15
   4: bc = low + 1                                        // uint
      assert 1 + bc == size
      mag = read_le_uint(ptr + 1, bc)
      return UInt(mag)
   5: bc = low + 1
      mantissa_zz = read_le_uint(ptr + 1, bc)
      (scale, scale_bytes) = read_varscale(ptr + 1 + bc, size - 1 - bc)
      assert 1 + bc + scale_bytes == size
      return Decimal(zz_decode(mantissa_zz), scale)
   6: width_bits = low & 0x07
      sci         = (low & 0x08) != 0
      assert width_bits ∈ {1, 2, 3, 4}                     // 1..4 = log₂(byte_count)
      w = 1 << width_bits                                  // 2, 4, 8, or 16
      assert 1 + w == size
      return Float(read_le_float(ptr + 1, w), sci=sci)
   7: bc = low + 1                                        // negint
      assert 1 + bc == size
      mag = read_le_uint(ptr + 1, bc)
      assert mag != 0                                      // negative-zero reserved
      return NegInt(mag)                                   // logical value = −mag
   8: assert low ≤ 1
      return String(ptr[1 .. size), encoding_flag = low)
   10: assert low == 0
       return Bytes(ptr[1 .. size))
   11: if low == 0:
          return parse_array(ptr, size)                   // generic
       else if 1 ≤ low ≤ 10:
          return parse_typed_array(ptr, size, code = low − 1)
       else:
          error                                           // reserved
   12: if low == 0:
          return parse_object(ptr, size)                  // single object
       else if low == 1:
          return parse_row_array(ptr, size)
       else:
          error                                           // reserved
   else: error

parse_array(ptr, size):
   N = read_u16_le(ptr + size − 2)
   slot_table = ptr + size − 2 − 4·N
   value_data_start = ptr + 1
   value_data_size = (slot_table − ptr) − 1

   for i in 0..N:
      off_i    = slot_offset(read_u32_le(slot_table + i·4))
      off_next = (i+1 < N)
                  ? slot_offset(read_u32_le(slot_table + (i+1)·4))
                  : value_data_size
      child = parse_value(value_data_start + off_i, off_next − off_i)
      append child

parse_typed_array(ptr, size, code):
   element_size = sizeof_for_code(code)                   // 1, 2, 4, or 8
   assert element_size > 0
   N = read_u16_le(ptr + size − 2)
   assert 1 + N·element_size + 2 == size
   for i in 0..N:
      element_bytes = ptr[1 + i·element_size .. 1 + (i+1)·element_size)
      append decode_element(element_bytes, code)

parse_object(ptr, size):
   N = read_u16_le(ptr + size − 2)
   slot_table = ptr + size − 2 − 4·N
   hash_table = slot_table − N
   value_data_start = ptr + 1
   value_data_size = (hash_table − ptr) − 1

   for i in 0..N:
      slot     = read_u32_le(slot_table + i·4)
      off_i    = slot & 0x00FFFFFF
      key_size_byte = slot >> 24
      off_next = (i+1 < N)
                  ? slot_offset(read_u32_le(slot_table + (i+1)·4))
                  : value_data_size
      entry      = value_data_start + off_i
      entry_size = off_next − off_i

      if key_size_byte != 0xFF:
         klen, klen_bytes = key_size_byte, 0
      else:
         excess, klen_bytes = read_varuint(entry, entry_size)
         klen = 0xFF + excess

      key   = entry[klen_bytes .. klen_bytes + klen)
      assert hash_table[i] == key_hash8(key)
      child = parse_value(entry + klen_bytes + klen,
                          entry_size − klen_bytes − klen)
      append (key, child)
```

## 11. Reference Algorithm — Object Lookup by Key

```
find(ptr, size, query_key) -> Value or NotFound:
   if (ptr[0] >> 4) != 12: return NotFound
   N          = read_u16_le(ptr + size − 2)
   slot_table = ptr + size − 2 − 4·N
   hash_table = slot_table − N
   value_data_start = ptr + 1
   value_data_size  = (hash_table − ptr) − 1
   want = key_hash8(query_key)

   i = 0
   while i < N:
      // Hash prefilter — SIMD scan equivalent
      skip = position of first byte == want in hash_table[i .. N)
      if skip not found: return NotFound
      i = i + skip

      // Slot read
      slot     = read_u32_le(slot_table + i·4)
      off_i    = slot & 0x00FFFFFF
      key_size_byte = slot >> 24
      off_next = (i+1 < N)
                   ? slot_offset(read_u32_le(slot_table + (i+1)·4))
                   : value_data_size
      entry      = value_data_start + off_i
      entry_size = off_next − off_i

      if key_size_byte != 0xFF:
         klen, klen_bytes = key_size_byte, 0
      else:
         excess, klen_bytes = read_varuint(entry, entry_size)
         klen = 0xFF + excess

      // Verify
      stored_key = entry[klen_bytes .. klen_bytes + klen)
      if klen == |query_key| and stored_key == query_key:
         return parse_value(entry + klen_bytes + klen,
                            entry_size − klen_bytes − klen)

      // Hash collision — advance and re-scan
      i = i + 1
   return NotFound
```

### 11.1 Why this is fast

Object key lookup is **O(N) per level**, where N is the field count of
the one object being searched. In the asymptotic sense binary search
on sorted keys would be O(log N), but in practice the hash-prefilter
linear scan beats it at every realistic JSON field count:

* The prefilter scans N **single bytes** sequentially. Implementations
  use SWAR (8 bytes per cycle, branchless) or SIMD intrinsics. For an
  object of 25 fields the entire scan touches one cache line.
* On a hash hit (rare; ~1/256 false-positive rate per byte), one
  string compare. For typical-sized identifier keys this is also a
  single cache-line read.
* No sorted-key data structure → no dependent-load chain, no branchy
  tree traversal.

A balanced binary-search-of-strings at the same N pays log₂(N)
**string** compares, each with a data-dependent branch and pointer
chase to a non-adjacent node — all cache-unfriendly. The reference
implementation measures ~17 ns per object lookup on a 25-field object;
a comparable tree-of-strings implementation lands in the 50–100 ns
range. The asymptotic crossover where a sorted/tree-based structure
would actually win is thousands of fields per object — far beyond
what JSON documents contain in practice.

The format trades the asymptotic optimality of log(N) for cache-
locality optimality at small-to-medium N, which is where every
realistic JSON object lives.

## 12. Reference Algorithm — Encode

```
encode_value(value, out: byte buffer):
   case value of
      Null()        -> append 0x00
      Bool(false)   -> append 0x10
      Bool(true)    -> append 0x11
      Int(v) where 0 ≤ v ≤ 15
                    -> append 0x30 | v
      Int(v) where v > 0:                              // uint
         bc = byte_count(v)             // smallest bc ∈ 1..16 fitting v
         append 0x40 | (bc − 1)
         append v as bc bytes LE
      Int(v) where v < 0:                              // negint
         mag = −v                       // unsigned magnitude
         bc  = byte_count(mag)          // smallest bc ∈ 1..16
         append 0x70 | (bc − 1)
         append mag as bc bytes LE
      Decimal(m, s):
         zz = zz_encode(m)
         bc = byte_count(zz)
         append 0x50 | (bc − 1)
         append zz as bc bytes LE
         append varscale(s)
      Float(f, sci=false):                                 // any IEEE binary{16,32,64,128}
         w   = byte_count_of(format_of(f))                  // 2, 4, 8, or 16
         lo  = log2(w)                                      // 1, 2, 3, or 4
         lo |= (sci ? 0b1000 : 0)                           // bit 3 = sci-source hint
         append 0x60 | lo
         append f as w IEEE bytes LE
      String(text, encoding_flag):                       // flag ∈ {0, 1}
         append 0x80 | encoding_flag
         append text
      Bytes(b):
         append 0xA0
         append b
      Array(children):                                    // generic form
         append 0xB0
         start = out.position
         records = []
         for child in children:
            records.append((offset = out.position − (start + 1)))
            encode_value(child, out)
         for r in records:
            append slot_pack(r.offset, 0) as u32 LE
         append count = |children| as u16 LE
      TypedArray(elements, code):                         // code ∈ {0..9}
         append 0xB0 | (code + 1)                         // low_nibble = code + 1
         for e in elements:
            append e as element_size(code) bytes LE
         append count = |elements| as u16 LE
      Object(fields):
         append 0xC0
         start = out.position
         records = []
         for (key, child) in fields:
            records.append((hash = key_hash8(key),
                            offset = out.position − (start + 1),
                            key_size = min(|key|, 0xFF)))
            if |key| ≥ 0xFF:
               append varuint(|key| − 0xFF)
            append key bytes
            encode_value(child, out)
         for r in records:
            append r.hash as u8
         for r in records:
            append slot_pack(r.offset, r.key_size) as u32 LE
         append count = |fields| as u16 LE
```

The encoder is **single-pass forward-write**. No memmove, no
header reservation, no backpatching beyond writing the index at the
tail.

### 12.1 Static vs. dynamic detection (encoder aggressiveness)

Several pjson container forms — `row_array` (§5.2.1), `typed_array`
(§5.1.1), canonical numeric encodings (§15.2) — give the same
logical value a substantially smaller wire form than the generic
fall-back. **Whether** a given encoder uses them depends on how it
learns the schema:

**Static (compile-time) detection.** When the producer is generated
from a typed source — for example, encoding a reflected record
type, or a `vector<R>` where `R`'s field list is known at compile
time — the schema is fully known before encoding begins. The
encoder emits `row_array` / `typed_array` unconditionally; there is
no probing pass and no fall-back path. Detection is **free**.
This is the path native code uses when emitting pjson from typed
values.

**Dynamic (transcode-time) detection.** When the producer is
reading JSON text (or another self-describing source) and emitting
pjson on the fly, the schema is not known until the array's
elements have been parsed. To use `row_array` or `typed_array`,
the encoder must **probe** each element as it arrives to confirm
the array is still homogeneous. Probing costs cycles on the
encode side; emitting the compactable form saves bytes on the
wire. Implementations expose this trade-off as an
**aggressiveness level**:

| level | name           | dynamic detect                                                            | wire size                            | encode cost          |
|-------|----------------|---------------------------------------------------------------------------|--------------------------------------|----------------------|
| 0     | none           | off                                                                       | always generic; ≈ JSON               | minimum              |
| 1     | shape-only     | row_array, typed_array                                                    | smaller than JSON on uniform arrays  | + one probe pass     |
| 2     | canonical      | level 1 + numeric canonicalization (§15.2) + smallest adaptive widths     | minimum (matches static encode)      | maximum              |

All levels preserve field encounter order (§15.4). Key sorting is
**not** part of pjson canonicalization at any level — it is the
application layer's responsibility, performed before pjson's
encoder is called, and only when the application has declared
order-independence as part of its logical equality contract.

* **Level 0 — none.** Transcoder emits `t_array` of `t_object`
  unconditionally and never combines neighbouring primitives into
  `typed_array`. Wire size matches the §1 baseline (~102% of
  compact JSON). Use when transcode latency dominates and storage
  size is not constrained (e.g. high-throughput RPC where the
  payload is decoded immediately on arrival).
* **Level 1 — shape-only.** On each new element of an array,
  compare its shape against the first element's. For an array of
  objects: byte-compare the key list. For an array of primitives:
  check the element type code. Match → continue buffering.
  Mismatch → restart the array as generic and emit the buffered
  elements using `t_object` / generic-array. Net cost: O(N·K)
  extra key-byte compares per array; net saving: 30–50% on
  uniform object arrays, 50–80% on dense numeric arrays. This is
  the §5.2.1.5 / §5.1.1 encoder rule applied dynamically.
* **Level 2 — canonical.** Apply §15 in full: typed_array for
  homogeneous-primitive arrays; row_array for homogeneous-shape
  object arrays; smallest adaptive widths; smallest numeric
  encoding per §15.2. **Field order is preserved** — pjson does
  not sort keys; the encoder writes fields in encounter order
  (§15.4). Output is strictly canonical: bit-equal across encoders
  for the same logical input under the application's order policy
  (which, for pjson, is "the order the input declared" — sorting,
  if wanted, is the layer above's job). Decode-time random-access
  by name and by slot index is unaffected. This is the level a
  database storing pjson rows or a content-addressable store
  should run; transcode time grows by a small constant factor
  (typically < 2× over level 0) and storage shrinks meaningfully
  versus JSON text.

A statically-typed encoder effectively runs at "level ∞" (every
applicable optimization, by knowing the schema in advance) at zero
extra cost. The aggressiveness ladder exists only to give dynamic
producers a way to spend encode cycles on wire-size reductions
that the static path gets for free.

Implementations of the dynamic JSON → pjson transcoder should
expose the level as a parameter on the entry point. The minimum
contract: level 0 must be available (it is the baseline single-
pass encoder); higher levels are recommended where the storage
side of the round-trip is the dominant cost.

---

## 13. Versioning

This is **pjson v1**. Future revisions of the format will:

* Allocate previously-reserved tag codes (2, 9, 13–15) for new types.
* Allocate previously-reserved low-nibble bits as flags or extensions
  (e.g. new `string` flags, new typed-array element codes in
  `array`'s 11..15 reserved range).
* Be detectable through application-layer means (file headers, MIME
  types, RPC protocol versions). The pjson value format itself does
  not carry a version byte.

A v1 parser encountering a v2 wire (e.g., a value with tag `0xD0`)
must reject the buffer rather than guess.

---

## 14. Conformance Tests

A conforming implementation must round-trip the following corpus
byte-for-byte through encode → decode → encode:

* All primitive values: `null`, `true`, `false`, integers covering each
  byte_count from 1 to 16, decimals with each varscale tier (1, 2, 3,
  4 bytes), all eight-byte IEEE doubles including 0.0, 1.0, NaN
  (encoded only — JSON forbids), small / medium / large strings, raw
  byte blobs of varying sizes, empty arrays, empty objects.
* Random nested structures up to depth 8 with mixed types.
* Object key collision on the hash byte (multiple fields share the
  same hash byte; lookup must return the right one for each).
* Long-key escape: object with a key of 254 bytes (no escape) and a
  key of 255 bytes (escape).
* Typed homogeneous arrays for each element code (i8, i16, i32, i64,
  u8, u16, u32, u64, f32, f64), including empty (N=0) and non-empty
  forms.

A reference test corpus lives at `cpp/tests/pjson_tests.cpp`
(with parallel Rust coverage in `#[cfg(test)]` modules across
`rust/psio/src/pjson*.rs`, including
`rust/psio/src/pjson_cross_validation_tests.rs`).

---

## 15. Canonical Encoding

The pjson format permits multiple wire encodings of the same logical
value (e.g. a small integer can use `uint_inline` or `uint`). For
content-addressable hashing, signature verification, deduplication,
and equality comparison via byte-compare, implementations need to
agree on a single canonical form for every value.

### 15.1 Encoding rule (priority order)

A canonical encoder selects the encoding by applying these rules in
order:

1. **Round-trip JSON without precision loss.** Choose only encodings
   that preserve the value's information through a JSON-text →
   pjson → JSON-text round-trip. (Strings preserve their escape form
   per §4.7; numbers preserve their value bit-exactly per the rules
   below.)
2. **Smallest possible byte size.** Among encodings that satisfy
   rule 1, choose the encoding that produces the fewest bytes.
3. **Fastest decode on ties.** When two encodings produce the same
   byte count AND both preserve precision, choose the one that
   decodes fastest. For numbers this means: prefer `ieee_float` over
   `decimal` (a raw 8-byte load is cheaper than mantissa-and-scale
   reconstruction).

This makes the canonical encoding **deterministic per value** —
given the same logical value, every conforming encoder produces the
same bytes for that value. (Field encounter order is excluded; see
§15.4.)

### 15.2 Numbers — concrete rules

For an integer value `v`:

| value range                                              | canonical encoding                              |
|----------------------------------------------------------|-------------------------------------------------|
| 0 ≤ v ≤ 15                                               | `uint_inline` (1 byte)                          |
| 16 ≤ v, fits `u64`                                       | `uint` with smallest bc (1..8)                  |
| u64::MAX < v, fits `u128`                                | `uint` with smallest bc (9..16)                 |
| v = −1, fits `−2⁶³ ≤ v ≤ −1`                             | `negint` with smallest bc on `\|v\|` (1..8)      |
| beyond `i64` magnitude, fits `i128` magnitude            | `negint` with smallest bc on `\|v\|` (9..16)     |

For an integer-valued double (e.g. `42.0`): treat as integer and
apply the table above. The fractional part is zero, so it round-trips
through any integer encoding losslessly.

For a fractional value `f` (non-zero fractional part) sourced as
some IEEE binary format:

1. Compute the **smallest IEEE width** `w ∈ {2, 4, 8, 16}` for
   which `f` round-trips bit-exact. The candidate widths are
   binary16, binary32, binary64, binary128. Let
   `ieee_size = 1 + w`.
2. Compute the **shortest round-trip decimal** `(mantissa, scale)`
   such that `from_float(f, w)` produces this pair (Ryu / Grisu /
   Dragon4-equivalent at width `w`). Trim trailing zeros from the
   mantissa (incrementing `scale`) until either the last digit is
   non-zero or `scale = 0`. Let
   `decimal_size = 1 + mantissa_bc + varscale_size(scale)`.
3. If `decimal_size < ieee_size` → encode as `decimal`.
4. Else if `decimal_size > ieee_size` → encode as `ieee_float` at
   width `w`.
5. Else (tie) → encode as `ieee_float` at width `w` (faster
   decode — a fixed-width raw load beats mantissa-and-scale
   reconstruction).

The `ieee_float` sci-source hint (§4.6 bit 3) is **outside** the
canonical comparison: setting or clearing it does not change the
numeric value or alter encoding-shortest selection. For
strict-canonical / content-addressable encoding, the bit MUST be
cleared (so that producers tracking source notation and producers
not tracking it agree on the canonical bytes).

For NaN, ±Inf, and other values that have no representable
shortest decimal: encode as `ieee_float` at the **smallest width
that represents the value** (binary16 for any NaN or ±Inf, since
both are representable in every IEEE width). Use the canonical
bit pattern specified in §15.2.1 below — multiple bit patterns
encode "a NaN" in IEEE-754, and a deterministic encoder must pick
exactly one.

For a `(mantissa, scale)` pair supplied directly by a typed numeric
source (not derived from a double): encode the `(mantissa, scale)`
form regardless of whether `ieee_float` would be shorter — the
typed value is **already** a `decimal` semantically; collapsing to
double would lose its exact-decimal identity. Producers that want
the smallest encoding regardless of identity should convert through
double first and apply the rules above.

#### 15.2.1 Canonical NaN and Infinity bit patterns

IEEE-754 admits many bit patterns for "a NaN" — every value with
exponent all 1s and a non-zero significand qualifies. For a
deterministic / content-addressable encoding, exactly one pattern
must be chosen. pjson's canonical encoder uses the **quiet NaN
with zero payload** at every width: sign = 0, exponent = all 1s,
top mantissa bit = 1 (the IEEE-754 quiet-bit), all other mantissa
bits = 0.

| width      | canonical NaN (LE bytes)                                 | canonical NaN (LE u{N}) |
|------------|----------------------------------------------------------|--------------------------|
| binary16   | `00 7E`                                                  | `0x7E00`                 |
| binary32   | `00 00 C0 7F`                                            | `0x7FC00000`             |
| binary64   | `00 00 00 00 00 00 F8 7F`                                | `0x7FF8000000000000`     |
| binary128  | `00…00 00 80 FF 7F`  (16 bytes; first 14 are zero)       | `0x7FFF8000_00000000_00000000_00000000` |

Encoders observing any other NaN bit pattern in the source value
**must** rewrite to the canonical pattern at the chosen width
before emitting `ieee_float`. The original payload bits are not
preserved — pjson treats "NaN" as a single logical value, not a
family.

For ±Infinity the bit pattern is unambiguous (sign + exponent
all 1s + mantissa all 0s). The sign bit is preserved from the
source; no canonicalization is needed beyond that.

| width      | +Infinity (LE)                                           | −Infinity (LE)                   |
|------------|----------------------------------------------------------|----------------------------------|
| binary16   | `0x7C00`                                                 | `0xFC00`                         |
| binary32   | `0x7F800000`                                             | `0xFF800000`                     |
| binary64   | `0x7FF0000000000000`                                     | `0xFFF0000000000000`             |
| binary128  | `0x7FFF0000_00000000_00000000_00000000`                  | `0xFFFF0000_00000000_00000000_00000000` |

A canonical encoder chooses the smallest width that represents
the value (always binary16 for NaN, ±Inf, ±0, and any other
"width-invariant" extreme).

Decoders accept any well-formed IEEE bit pattern at any width;
canonicalization is an encoder requirement, not a decoder rule.
Strict-canonical validators (§15.7) **must** reject `ieee_float`
payloads whose bits are a NaN but not the canonical pattern.

### 15.3 Strings

The wire form of a string carries an encoding flag (§4.7) that
**identifies the source** rather than canonicalizing it:

- A string from a JSON text source encodes as `escape_form` —
  byte-identical to the source bytes between the JSON quotes.
- A string from a typed text value encodes as `raw_text` — the
  program-visible text, with no escape pass.
- Raw binary uses the separate `bytes` tag (§4.8), not a string
  flag.

Two pjson values for the same logical text "Hello world" — one
from JSON, one from a typed string — will NOT be byte-equal
because they deliberately preserve their different source
representations. This is the format's **byte-equality means same
source** property.

For applications that want logical equality across sources, decode
both as their text form and compare the unescaped bytes (which
requires applying JSON unescape rules to an `escape_form` value).
Or canonicalize at a higher layer before encoding.

### 15.4 Containers

- **Field encounter order is application-defined**, not canonical.
  Two pjson objects with the same fields in different orders are NOT
  byte-equal, even if their data is logically the same. Applications
  that need order-independent equality must canonicalize key order
  at a higher layer (typically: sort keys lexicographically before
  encoding).
- **Slot offsets** must be ascending and exactly cover the
  value_data region (no gaps, no overlap). The encoder's natural
  forward-write produces this; non-canonical layouts (e.g. jumbled
  slot offsets reordering the children) are valid pjson but not
  canonical.
- **Hash bytes** are deterministic from key bytes (low byte of
  XXH3-64 over suffix-stripped key per §5.3). Always canonical.
- **num_fields** is the actual N. Always canonical.

### 15.5 Objects — generic vs. row_array

For an array whose elements are all objects with **identical
schemas** (same K keys, same order, byte-equal key strings), the
canonical encoder **must** emit the **row_array** form (§5.2.1)
rather than a generic `t_array` of N `t_object`s. The shared
schema removes the per-record overhead, so the row_array form is
always the smaller-byte choice — no further size comparison needed.

A canonical encoder must also pick the **smallest adaptive widths**
(`slot_w`, `recoff_w`) that fit the actual record body sizes:

```
slot_w   = smallest width fitting max(record_body_size_i) for i in 0..N
recoff_w = smallest width fitting total records-body size
```

Mismatched objects (extra fields, missing fields, different
byte-order of keys) make the array non-homogeneous; canonical
encoders fall back to generic `t_array` of `t_object`s in that case.

For dynamic JSON → pjson transcoders, this rule is reachable only
at aggressiveness level 1 or higher (§12.1). Level 0 transcoders
always emit generic `t_array` of `t_object` — they are not
canonical encoders, by definition.

### 15.6 Arrays — generic vs. typed

For an array whose elements are all of one fixed-width primitive
type (signed integer, unsigned integer, float), the canonical
encoder **must** emit the **typed** form (§5.1.1) using the
**smallest element-type code** that holds every element exactly:

1. If any element is fractional (non-zero fractional part): use the
   smallest IEEE float that round-trips every element. `f32` if
   every element round-trips through a 32-bit float, else `f64`.
2. Else if every element is non-negative: use the smallest unsigned
   width that fits every element: `u8`, `u16`, `u32`, or `u64`.
3. Else: use the smallest signed width that fits every element:
   `i8`, `i16`, `i32`, or `i64`.

For an array with at least one non-primitive element (string,
container, mixed type), or whose primitive elements span multiple
unrelated families (mixed integer-and-float, mixed signed-and-very-
large-unsigned beyond `i64`), the canonical encoder uses the
**generic** form (§5.1).

Rationale: the typed form satisfies §15.1 priority rule 2
(smallest size) for homogeneous primitive arrays; the size win is
substantial (often 2× or more on dense arrays) and is the only
pjson encoding choice that gives byte-different output for the
"same" logical array. Strict-canonical encoders therefore must
choose typed when applicable.

### 15.7 Strict canonical (byte-equality contract)

A pjson value is **strictly canonical** iff:

- Every nested numeric value satisfies §15.2.
- Every nested string carries the canonical flag for its source
  (§15.3).
- Every nested container has slots in ascending offset order with
  no gaps (§15.4).
- Every nested array satisfies §15.6 (typed form when homogeneous
  primitive, with the smallest element-type code that holds every
  element exactly).
- Every nested array of homogeneously-shaped objects satisfies
  §15.5 (row_array form with smallest adaptive widths).
- Field order matches the application's chosen canonicalization
  policy (which itself must be deterministic from the logical
  value — typically lexicographic key sort, or original input
  order if input order is part of the logical identity).

Two strictly-canonical pjson buffers are byte-equal iff their
logical values are equal under the application's order policy.

### 15.8 Producer guidance

Implementations marketed as "canonical pjson encoders" must:

- Apply §15.1 priority rules for every value.
- Document their field-order policy (lex-sort, source-order, etc.)
  so consumers can match.
- Never emit reserved low-nibble flag values (§3) on canonical
  output.

The reference C++ encoder follows §15.2 rules for numbers and
preserves source-order field encoding. It does NOT sort keys; that's
left to a higher layer.

## 16. Type-level caps

pjson honors two type-level annotations declared in `psio/annotate.hpp`
and emitted via `PSIO_REFLECT(...)`:

* `maxFields(N)` — caps the number of declared fields on a reflected
  record (`max_fields_spec`). Surfaced through `psio::max_fields_v<T>`
  and the composite `psio::effective_max_fields_v<T>` (`min(cap,
  reflect<T>::member_count)` on Reflected types).
* `maxDynamicData(N)` — caps the total encoded payload size in bytes
  (`max_dynamic_data_spec`). Surfaced through
  `psio::max_dynamic_data_v<T>` and composed with the per-field
  `length_bound`-derived bound from `max_encoded_size<T>` into
  `psio::effective_max_dynamic_v<T>` (cap wins downward; never widens).

### 7.1 Encoder behavior

`psio::to_pjson(t, out)` and `psio::from_struct(t)` compute the encoded
total before allocation. If `psio::max_dynamic_data_v<T>` is present
and the total exceeds the cap, the encoder throws
`psio::codec_exception` carrying `format_name = "pjson"`. Silent
widening would defeat predictable wire size; honest failure is the
contract.

### 7.2 Decoder / view behavior

* `psio::view<T, pjson_format>::from_pjson(raw)` takes the input span
  unchecked; downstream call sites that accept untrusted pjson should
  use `view<T, pjson_format>::from_pjson_checked(raw)`, which calls
  `enforce_max_dynamic_cap<T>(raw.size(), "pjson")` before constructing
  the typed view. Oversized buffers are rejected up front rather than
  pulling field accessors over partially-validated bytes.

### 7.3 Width selection

Slot-table and offset widths in §5.1.1, §5.2, §5.2.1.1, and §5.6 are
selected at runtime from the actual encoded body size. The cap acts as
a ceiling, not a forced narrower-width selector — the encoder still
picks the smallest width that addresses the actual content. The cap's
job is to bound that content; the width selector's job is to address
it efficiently within that bound.

### 7.4 Schema-form surfacing

When a Schema IR is produced from C++ reflection (via
`psio::schema_builder`), the caps surface as `Attribute{name="maxFields",
value="N"}` and `Attribute{name="maxDynamicData", value="N"}` on the
record's `Object` / `Struct` IR node, alongside the existing
`definitionWillNotChange` attribute. Downstream emitters render these
per their target IDL conventions: WIT → `@maxFields(N)` native
attribute; capnp / protobuf / fbs → preceding-line comments
(`# @maxFields(N)` / `// @maxFields(N)`).

End of spec.
## Appendix A. Glossary

| term | meaning |
|---|---|
| **value_data** | the contiguous bytes between the container's tag byte and its tail index, holding all child values (and inline keys, for objects). |
| **slot** | a packed `u32` per child holding `{ offset (24 bits), key_size (8 bits) }`. The high byte is the key length for objects, unused (must be 0) for arrays. |
| **slot table** | the array of N slots at the tail of a generic container. Per-slot stride = `slot_w` for arrays, `slot_w + 1` for objects (offset + key_size). slot_w is encoded in the width byte at byte 1 of the container. |
| **width byte** | byte 1 of every generic-array (`0xB0`) and generic-object (`0xC0`) container. Low 2 bits encode `slot_w_code` (0=u8 .. 3=u32); high 6 bits reserved. Picked per container from value_data size; recursive. |
| **hash table** | the array of N hash bytes (objects only), located immediately before the slot table. |
| **count** | the `u16 LE` at the last 2 bytes of every container, giving N. |
| **canonical layout** | for an object, `hash[i] == key_hash8(field_i_name)` in declaration order matches a producer's expected order, enabling byte-equal fast-path validation against a precomputed hash array. |
| **suffix stripping** | the hash function ignores any trailing `.<suffix>` on the key, supporting presentation-tag conventions. |

## Appendix B. Worked Example

Compact JSON `{"a":1,"b":"two"}` (17 bytes) encodes as **20 bytes** of pjson:

```
0xC0                       tag: object
                           ── value_data (10 bytes) ──
0x61                       'a' (key for entry 0; key_size in slot)
0x31                       uint_inline 1 (entry 0 value)
0x62                       'b' (key for entry 1)
0x80                       string tag (entry 1 value tag)
0x74 0x77 0x6F             't', 'w', 'o' (string content)
                           ── hash[2] (2 bytes) ──
0xH1                       low byte of XXH3_64("a")
0xH2                       low byte of XXH3_64("b")
                           ── slot[2] (8 bytes) ──
0x00 0x00 0x00 0x01        slot[0]: offset=0, key_size=1
0x02 0x00 0x00 0x01        slot[1]: offset=2, key_size=1
                           ── count (2 bytes) ──
0x02 0x00                  N = 2
```

Reading this back from an arbitrary `(ptr, size = 20)`:

```
N      = read_u16_le(ptr + 18) = 2
slot_table = ptr + 18 − 8 = ptr + 10
hash_table = ptr + 10 − 2 = ptr + 8
value_data_start = ptr + 1
value_data_size  = 8 − 1 = 7
```

Field `"a"`:
* hash[0] at `ptr[8]`, slot[0] at `ptr[10..14]` → offset 0, key_size 1.
* entry at `ptr+1`, key bytes `ptr[1..2)` = `'a'`.
* value at `ptr+2`, size = (slot[1].offset − slot[0].offset) − 1 = 1 byte.
* `parse_value(ptr+2, 1)` → tag `0x31` → uint_inline = 1.

Field `"b"`:
* hash[1] at `ptr[9]`, slot[1] at `ptr[14..18]` → offset 2, key_size 1.
* entry at `ptr+3`, key bytes `ptr[3..4)` = `'b'`.
* value at `ptr+4`, size = value_data_size − slot[1].offset − 1 = 4 bytes.
* `parse_value(ptr+4, 4)` → tag `0x80` → string with content `"two"`.

---

## Appendix C. Reference shapes used in §1 numbers

The "view_one ns" latency table in §1 references five reflected
record shapes drawn from the pjson benchmark suite. They are
reproduced here in schema notation so this document is self-
contained. The C++ definitions used to generate the perf snapshot
live at `cpp/benchmarks/shapes.hpp`.

### Point — 8 B fixed, DWNC

```
record Point {
    x : i32
    y : i32
} dwnc
```

Two signed 32-bit integers. DWNC ("definition will not change"),
so encoders skip the extensibility header and the canonical-typed
view's memcmp template covers the entire payload.

### NameRecord — 16 B fixed, DWNC

```
record NameRecord {
    account : u64
    limit   : u64
} dwnc
```

Two unsigned 64-bit integers. Same DWNC fast path as Point.

### FlatRecord — variable, extensible

```
record FlatRecord {
    id     : u32
    label  : String
    values : Vector(u16)
}
```

Mixed fixed + variable: a numeric ID, a UTF-8 label, and a
homogeneous `u16` array. Carries the extensibility header.

### Record — variable, extensible, with optional

```
record Record {
    id     : u32
    label  : String
    values : Vector(u16)
    score  : Optional(u32)
}
```

`FlatRecord` plus an optional trailing field. Exercises
trailing-default pruning and the `Optional` encoding.

### Validator — packed, DWNC

```
record Validator {
    pubkey_lo          : u64
    pubkey_hi          : u64
    withdrawal_lo      : u64
    withdrawal_hi      : u64
    effective_balance  : u64
    slashed            : bool
    activation_epoch   : u64
    exit_epoch         : u64
    withdrawable_epoch : u64
} dwnc, packed
```

The canonical Ethereum beacon-state Validator entry — eight `u64`
fields plus a `bool`. The reflected layout is **packed**: total
in-memory bytes equal the sum of field sizes (no platform
alignment padding), so a memcpy of the live struct produces the
canonical wire bytes byte-for-byte. This matches the layout used
by pssz / ssz / bin DWNC memcpy fast paths and lets the
canonical-typed view validate via a single memcmp against a
precomputed schema-hash template.

The 169-byte pjson size in §1 reflects the wire form for a
populated Validator (object header, per-field key bytes, value
encodings at the encoder's smallest-`bc` choice for each field,
hash table, slot table, count). A field-by-field byte budget is
out of scope for this appendix; the live bench corpus computes
the exact byte count for each populated instance.
