# pssz (Psi Simple Serialization)

**Status:** v0.1, 2026-04-30.

pssz — **Psi Simple Serialization** — is psio's primary schema-driven
binary serialization format. It is **derived from Ethereum's Simple
Serialization (SSZ)** and inherits SSZ's compact wire layout,
container-relative offsets, and zero-length-prefix vector encoding.
On top of that foundation it incorporates the best-of-breed features
from the broader serialization ecosystem (fracpack's schema
extensibility, capnp's zero-copy view discipline, fracpack's memcpy
fast path for layout-stable records) and adds adaptive-width offset
slots for the per-type compactness no other format in the comparison
provides.

This document is the wire-format specification — language-neutral,
detailed enough that two implementations following it must produce
byte-identical output for the same input value on the same schema.

The companion specification for the schemaless self-describing
format is `pjson-spec.md`.

## 1. Purpose and Position

### 1.1 Six design properties

pssz exists to deliver six properties simultaneously, in priority
order:

1. **Relatively small wire format.** Tied with the smallest
   schema-driven binary formats (ssz, bin, borsh, bincode) on
   fixed-width data; smaller than them when adaptive offset widths
   kick in. Larger than varint formats (avro, msgpack, protobuf) on
   sparse-integer payloads — that's the only dimension where pssz
   trades.
2. **Zero-copy views with O(1) random access** to any field. The
   decoded representation is a pointer into the encoded buffer; the
   byte offset of field i is computable in constant time from the
   schema, with no scan over preceding fields.
3. **Schema evolution with forward and backward compatibility.** A
   v1 encoder + v2 decoder, or a v2 encoder + v1 decoder, both
   interoperate without re-encoding.
4. **Fastest in class to encode, decode, validate, and view.**
   Top-tier latency on every operation in the head-to-head bench
   matrix; sub-nanosecond view-one for in-tier shapes.
5. **Canonical deterministic representation.** Every value of every
   type has exactly one canonical encoding, so equal values produce
   equal bytes. Suitable for content-addressed storage and signed
   payloads.
6. **Single-pass encode and decode.** The encoder writes the buffer
   in one forward pass with backpatching of the offset table; the
   decoder reads in one forward pass with no rewind. No second
   accumulation pass to compute sizes.

### 1.2 The DWNC annotation

Several rows in the comparison table below reference **DWNC**, an
abbreviation introduced here for use throughout this document.

A type is **DWNC** ("definition will not change") when the schema
includes a type-level `dwnc` flag committing that the field count,
field types, and field order are frozen — no future schema version
will add, remove, or reorder fields. Encoders of DWNC types skip the
extensibility header (saving W bytes per record); when an
implementation can prove that the in-memory layout of T matches the
wire layout exactly, encode and decode collapse to a single byte-
range copy.

The `dwnc` flag is the same schema annotation referenced in
`pjson-spec.md` §16; both formats consume it from the schema's
type-level annotation channel.

### 1.3 Comparison matrix

#### 1.3.1 Property checklist

Cells marked 🔴 indicate the format does not provide that property;
🟢 indicates full support; 🟡 indicates partial support (caveats apply).
Columns are ordered left-to-right by **similarity to pssz**: each
cell scores 🟢 = 1.0, 🟡 = 0.5, 🔴 = 0.0, summed across the nine
property rows. Column totals appear in the final row.

Rows are also ordered by **how cleanly they track the column gradient**:
properties pssz "wins" cleanly on the left side appear at the top, the
outlier row (schema extensibility — green-but-late under multiple far-
right formats) sinks to the bottom.

| Property                         | **pssz** | fracpack | wit  | ssz  | capnp | flatbuf | avro | bincode | borsh | bin  | protobuf | msgpack |
|----------------------------------|:--------:|:--------:|:----:|:----:|:-----:|:-------:|:----:|:-------:|:-----:|:----:|:--------:|:-------:|
| O(1) random field access         |    🟢    |    🟢    |  🟢  |  🟢  |  🟢   |   🟢    |  🔴  |   🔴    |  🔴   |  🔴  |    🔴    |   🔴    |
| Zero-copy views (no allocation)  |    🟢    |    🟢    |  🟢  |  🟢  |  🟢   |   🟢    |  🔴  |   🔴    |  🔴   |  🔴  |    🔴    |   🔴    |
| Adaptive offset width            |    🟢    |    🟡    |  🔴  |  🔴  |  🔴   |   🔴    |  🔴  |   🔴    |  🔴   |  🔴  |    🔴    |   🔴    |
| Trailing-default pruning         |    🟢    |    🟢    |  🔴  |  🔴  |  🔴   |   🔴    |  🔴  |   🔴    |  🔴   |  🔴  |    🔴    |   🔴    |
| DWNC memcpy fast path            |    🟢    |    🟢    |  🟢  |  🔴  |  🔴   |   🔴    |  🔴  |   🟡    |  🟡   |  🔴  |    🔴    |   🔴    |
| Implicit sizing (no length pfx)  |    🟢    |    🔴    |  🟢  |  🟢  |  🟡   |   🟡    |  🔴  |   🔴    |  🔴   |  🔴  |    🔴    |   🔴    |
| Canonical encoding               |    🟢    |    🟡    |  🟢  |  🟢  |  🟡   |   🟡    |  🟡  |   🟢    |  🟢   |  🟢  |    🔴    |   🔴    |
| Single-pass encode               |    🟢    |    🟡    |  🟡  |  🟡  |  🟡   |   🟡    |  🟢  |   🟢    |  🟢   |  🟢  |    🟡    |   🟢    |
| Schema extensibility             |    🟢    |    🟢    |  🔴  |  🔴  |  🟢   |   🟢    |  🟢  |   🔴    |  🔴   |  🔴  |    🟢    |   🔴    |
| **Similarity score (max 9.0)**   | **9.0**  | **6.5**  |**5.5**|**4.5**|**4.5**| **4.5** |**2.5**| **2.5** |**2.5**|**2.0**|  **1.5** |  **1.0** |

pssz is the only column with 🟢 on every row. Ties at 4.5 (ssz / capnp /
flatbuf) and 2.5 (avro / bincode / borsh) reflect different trade-off
mixes at the same overall coverage — see §1.4 for which axes each
format prioritizes.

#### 1.3.2 Quantitative comparison: geomean ratio vs pssz

The numbers below come from the `psio_bench_vs_externals` snapshot
(Apple M-series, llvm-clang 22.1, `-O3 -DNDEBUG`). For every cell
(format × shape × operation), the ratio is computed as
`format_value / pssz_value`. Cells smaller than 1.00 mean the format
beats pssz on that cell; cells larger than 1.00 mean pssz wins.
Per-format aggregates use the **geometric mean** of those ratios —
magnitude-aware (a 100× outlier counts as 100×, not as one rank
position) and scale-invariant (combining nanosecond cells with byte
cells in the same pool is well-defined because every entry is a
dimensionless ratio).

The "Cumulative" column is the geomean across **all** (size, encode,
decode, view) cells — the single number that summarizes "how much
work does this format ask for relative to pssz, on average". Lower
is better; **1.00 means tied with pssz**.

| Format                | size  | encode | decode | view  | **Cumulative** |
|-----------------------|------:|-------:|-------:|------:|---------------:|
| **pssz**              |**1.00**| **1.00** | **1.00** | **1.00** | **1.00**     |
| ssz                   |  0.99 |   1.72 |   0.99 |  0.91 |  **1.08**      |
| borsh                 |  0.96 |   2.37 |   0.96 |  —    |  **1.16**      |
| bincode               |  1.04 |   2.42 |   0.97 |  —    |  **1.20**      |
| frac32                |  1.06 |   2.74 |   1.00 |  —    |  **1.29**      |
| bin                   |  0.94 |   5.29 |   1.14 |  —    |  **1.55**      |
| wit                   |  1.10 |   7.08 |   1.10 |  1.22 |  **1.62**      |
| capnp (psio)          |  1.63 |  22.77 |   1.16 | 48.71 |  **3.93**      |
| msgpack               |  0.61 |  16.38 |   4.47 |  —    |  **4.09**      |
| avro                  |  0.56 |  28.67 |   4.48 |  —    |  **5.30**      |
| protobuf              |  0.72 |  23.79 |   7.79 |  —    |  **5.37**      |
| libprotobuf/protobuf  |  0.65 |  44.96 |   5.97 |  —    |  **6.43**      |
| libflatbuffers/flatbuf|  1.73 |  56.36 |   1.21 |  2.38 |  **8.56**      |
| msgpack-cxx/msgpack   |  0.61 |  33.20 |  19.55 |  —    | **10.68**      |
| bson                  |  2.29 |  35.30 |   9.94 |  —    | **13.00**      |
| flatbuf (psio)        |  1.68 |  85.39 |   1.73 |  2.39 | **14.83**      |
| pjson                 |  1.48 |  49.49 |  11.25 | 18.38 | **17.57**      |
| libcapnp/capnp        |  1.59 |  78.70 |   5.26 | 48.34 | **24.71**      |
| json                  |  2.21 | 205.91 |  52.60 |  —    | **70.81**      |

(— in the view column means the format has no zero-copy view path
and thus no view_one cell to compare; it doesn't help or hurt the
cumulative, which only averages cells the format participates in.)

How to read the table:

- **pssz wins cumulative.** No format is below 1.00 in the
  Cumulative column. ssz comes closest (1.08) but pays 72% extra on
  encode. Every format that beats pssz on size pays 4×–30× on
  encode and 4×–8× on decode.

- **Size wins are architecturally bought from access-pattern
  losses.** The varint cluster (avro 0.56, msgpack 0.61, protobuf
  0.72) shaves bytes by encoding integers in variable-length groups
  — but a varint's byte count depends on the value, so field N's
  byte offset depends on field N-1's value, which requires a
  sequential walk to reach any field. **This structurally precludes
  zero-copy views and O(1) random access** (look back at the
  property checklist: every format with size < 1.00 has 🔴 on the
  "O(1) random field access" and "Zero-copy views" rows). Trailing-
  default pruning is also off the table for the same reason — you
  can't drop trailing fields safely when the offsets to the kept
  ones depend on the values you'd otherwise drop. The size win and
  the access-pattern losses are two sides of the same architectural
  choice; pssz refuses both — fixed-width offsets buy random access
  at the cost of a few bytes vs varint, and pssz minimizes that
  cost via adaptive width per type.

- **The "size cluster with pssz"** — ssz, bin, borsh, bincode,
  frac32, wit. All within ±10% of pssz on size; differentiator is
  encode latency where pssz's single-pass-with-backpatching wins.
  ssz lacks extensibility + trailing-pruning + DWNC fast path; bin/
  borsh/bincode lack random access.

- **The schema-tooled formats** — capnp and flatbuf. Have zero-copy
  views and extensibility, BUT pay both more size (~1.7×) and much
  more encode time (22-85×) for the cross-language IDL+codegen
  tooling that pssz today doesn't match (capnp/flatbuf have decades
  of compiler pipelines in many languages).

- **The self-describing formats** — pjson, json, bson. Different
  use case (no schema needed at decode); cost shows in the
  cumulative. The pjson row is interesting because pjson DOES have
  zero-copy random access (slot table at the end of each object),
  unlike json/bson — but it's still ~17× pssz cumulatively because
  self-describing data carries the schema in the bytes.

The single architectural sentence that summarizes the matrix:
**zero-copy views and O(1) random access require an offset table
with fixed-width slots, and that table is what costs the few bytes
that varint formats save.** pssz's bet is that the access-pattern
properties are worth more than those bytes — and the cumulative
geomean confirms that bet across every dimension we measure.

The rest of this section explains what each property row means and
why it matters.

### 1.4 Why each property matters

**O(1) random field access.** The decoder can compute the byte offset
of the i-th field in constant time, with no scan over preceding
fields. A pssz reader (zero-copy view) of a record can peek at a
single field in nanoseconds. The mechanism is an **offset table** in
the fixed region (or a vtable equivalent in capnp/flatbuf): each
variable field has a slot whose value is the byte offset of its
payload. ssz, fracpack, and pssz all share this design. Concatenated
formats (bincode, bin, borsh) and tag-stream formats (msgpack,
protobuf, avro) lack this table — they must walk every preceding
field to find the start of field i.

**Zero-copy views.** Closely related: the decoder produces a
read-only pointer into the encoded buffer for any sub-value. No
allocation, no decode of unused fields. Combines with O(1) access to
give "decode the one field I asked for" semantics.

**Adaptive offset width.** When the schema can bound the total
encoded size (via per-field length bounds, a type-level `max-
dynamic-data` cap, or container-relative encoding plus type
analysis), pssz picks 1-, 2-, or 4-byte offsets accordingly. A bound
of ≤ 256 bytes turns every offset slot from 4 bytes to 1 byte —
saving 3 × N bytes for an N-field record. SSZ is fixed at 4-byte
offsets; fracpack has a per-format-tag choice (16-bit vs 32-bit
offsets) but not per-type adaptation.

**Schema extensibility.** A version-1 record can be written with N
fields and read by a version-2 decoder that knows about N+M fields,
or vice versa. The wire format must encode "how many fields the
writer knew about" in a way the reader can detect without going off
the end of the buffer. SSZ has no such mechanism (its layout is
locked at schema-design time); fracpack solves it with a u16 fixed-
region size header; pssz inherits and adapts that header.

**Trailing-default pruning.** When a record's last K fields are at
their default values (None for `optional`, zero for arithmetic,
empty for vector / string), the encoder may drop them entirely.
Combined with the extensibility header, this saves bytes whenever
old encoders happen to use only the early fields of a newer schema.
Critical for high-cardinality records where most fields are unset.

**Implicit sizing (no length prefix).** Variable-length fields' size
is derived from the **next field's offset** (or the end of the
container) — there is no per-field length prefix in the dynamic
region. Saves W bytes per variable field where W is the offset width.
For a record with 4 strings at frac32 → pssz32: 16 bytes saved per
record.

**Canonical encoding.** Every value of every type has exactly one
canonical pssz byte sequence (§15). Encoders that emit canonical
form produce equal bytes for equal values; this is the property
content-addressed stores (CIDs, signed payloads) require. Length-
prefix and varint formats often have multiple valid encodings of
the same value (different prefix widths, leading-zero varint
representations) and need additional discipline to be canonical.

**Single-pass encode.** pssz encodes a value in one forward pass
over the source value, with backpatching of offset slots inside the
fixed region as variable-field payloads land in the dynamic region.
There is no "compute total size" pre-pass. The decoder is also
single-pass: read the extensibility header, walk fields in order,
resolve variable-field payloads as they're reached. This is the
property that turns sub-nanosecond view-one numbers into
sub-microsecond bulk-record encode/decode numbers; formats with a
size-then-emit two-pass encoder (which most are) double the work.

**DWNC memcpy fast path.** When a DWNC record's fields are all
fixed-shape AND the in-memory layout matches the wire layout (no
implicit padding, or `__attribute__((packed))` applied), the
encoder skips the extensibility header and emits the record via a
single `memcpy`. The decoder inverse-memcpys. Sub-nanosecond per
record on modern hardware.

### 1.5 Head-to-head numbers

The pssz benchmark snapshot at gofractally/psiserve commit `0c2004e`
(measured Apple M-series, llvm-clang 22.1, `-O3 -DNDEBUG`):

| Shape                 | bin | borsh | bincode | ssz | pssz | frac32 | wit | flatbuf | capnp | msgpack | json   | bson   |
|-----------------------|----:|------:|--------:|----:|-----:|-------:|----:|--------:|------:|--------:|-------:|-------:|
| Point (2 × i32)       |   8 |     8 |       8 |   8 |   **8** |      8 |   8 |     24  |    24 |       4 |     16 |     19 |
| NameRecord (2 × u64)  |  16 |    16 |      16 |  16 |  **16** |     16 |  16 |     32  |    32 |      15 |     45 |     37 |
| FlatRecord (DWNC)     |  30 |    32 |      40 |  32 |  **32** |     40 |  40 |     64  |    72 |      18 |     53 |     88 |
| Validator (9 × u64+b) |  65 |    65 |      65 |  65 |  **65** |     65 |  72 |     96  |    88 |      26 |    195 |    200 |
| ValidatorList(100)    |6511 |  6512 |    6516 |6512 |**6516** |   6518 |7216 |    7712 |  7240 |    3259 |  20154 |  20427 |

For the rows where pssz is bolded but tied with peers, the
**view-one** (single-field zero-copy access) numbers tell the rest of
the story:

| Shape                 |  ssz | **pssz** | wit  | flatbuf | capnp |
|-----------------------|-----:|---------:|-----:|--------:|------:|
| FlatRecord            | 0.33 | **0.34** | 0.33 |   0.58  | 17.6  |
| Validator             | 0.33 | **0.33** | 0.33 |   0.60  | 17.4  |
| Order (nested)        | 0.47 | **0.56** | 0.32 |   0.94  | 22.4  |
| ValidatorList(100)    | 0.46 | **0.52** | 1.98 |   3.46  | 27.7  |
| Deep4Dwnc (5 levels)  | 0.33 | **0.34** | 0.33 |   –     |   –   |

(units: ns/op; lower is better)

The size race within the binary cluster is close (within 0–1% on
fixed records, ~0.1% on bulk arrays). The differentiator is what
each format gives up to get there:

- **ssz** matches pssz size but cannot evolve schemas.
- **bin / borsh / bincode** match pssz size but cannot do O(1) field
  access — every field requires a sequential walk.
- **flatbuf / capnp** support extensibility but pay 10–20% size
  overhead and 10–50× the view-one latency.
- **msgpack / protobuf** are compact but require a full parse to
  reach any field.

pssz is the row that holds size-tier-with-the-best on every shape,
view-latency-tier-with-the-best on every shape, AND extensibility
plus trailing-default pruning plus DWNC fast path that none of the
top-tier-by-size formats provide.

## 2. Conventions

### 2.1 Byte order

All multi-byte integers in pssz wire bytes are **little-endian**. This
applies to:

- Fixed-region scalar values (u16, u32, u64, signed integers, IEEE
  floats),
- Offset slots in record headers,
- The u{W} extensibility header itself,
- Length / count fields embedded in any structure spec'd below.

A pssz buffer is portable across endian-class systems because the
encoder always writes little-endian regardless of the host.

### 2.2 Width tag W

Every pssz container is encoded under a **width tag** W ∈ {1, 2, 4}:

- W = 1: offsets and the extensibility header are 1 byte each (u8).
  Container's total encoded size must fit in 256 bytes.
- W = 2: offsets and the extensibility header are 2 bytes each (u16).
  Container's total must fit in 65 536 bytes.
- W = 4: offsets and the extensibility header are 4 bytes each (u32).
  Container's total must fit in 4 GiB.

The width tag is **a property of T** (the container type), not of the
encoded buffer — it is derived from the schema, not stored on the
wire. Encoder and decoder must agree on W for each (schema, type)
pair. § 3 specifies the rule that derives W from a type description.

### 2.3 Terminology

**Container.** Any value that has a layout: a record (struct), an
array, a vector, an optional, a variant, a bitvector, a bitlist, or
the top-level pssz document.

**Field.** A named slot inside a record. Field index `i` runs from 0
to N − 1 in declaration order.

**Fixed field.** A field whose encoded size is the same for every
value of its type. Examples: arithmetic primitives, `Array(T, N)`
where T is fixed, `Bitvector(N)`, DWNC records of all-fixed fields.
Fixed field values are written **inline** in the fixed region.

**Variable field.** A field whose encoded size depends on the value.
Examples: `String`, `Vector(T)`, `Bitlist(N)`, `Optional(V)` where
V is variable, `Variant(...)`, non-DWNC records. Variable field
**payloads** live in the dynamic / heap region; the fixed region
holds only an offset slot pointing at the payload.

**Fixed region.** The contiguous bytes immediately following the
extensibility header that hold inline fixed-field values and
variable-field offset slots. Size is `sum(fixed_size_of(field_i))`
across all declared fields.

**Dynamic region (heap).** The bytes after the fixed region that hold
variable-field payloads, packed back-to-back in field-declaration
order.

**DWNC.** "Definition will not change" — a type-level annotation
asserting the schema author commits to never adding, removing, or
reordering fields. Encoders for DWNC types skip the extensibility
header (saving W bytes per record) and may use a memcpy fast path
when the layout matches.

**Container-relative offset.** Each offset slot's value is the byte
distance from **the start of the fixed region** to the variable
field's payload. (Note: this is "fixed region start", not
"container start" — for a non-DWNC record the extensibility header
sits before the fixed region and is *not* included in the offset
arithmetic.)

### 2.4 Notation in this document

- `u8 / u16 / u32` mean unsigned little-endian integer of that width.
- `i8 / i16 / i32 / i64` mean signed two's-complement little-endian.
- `f32 / f64` mean IEEE 754 single / double precision in standard
  binary memory layout (which is identical to little-endian on every
  modern platform).
- `[X | Y | Z]` denotes byte regions concatenated.
- `<X>` denotes a region whose size is described by X.
- `bytes(N)` denotes N raw bytes with no size prefix.

## 3. Width Selection

The width tag W for type T is determined as follows. Every pssz
implementation must compute W identically.

```
fn auto_pssz_width(T) -> {1, 2, 4}:
    eff = effective_max_dynamic(T)         // see §3.1
    if eff is Some(n) and n <= 0xFF:    return 1
    if eff is Some(n) and n <= 0xFFFF:  return 2
    return 4
```

### 3.1 effective_max_dynamic(T)

`effective_max_dynamic(T)` returns the smaller of:

- The **explicit cap**: if T carries a type-level
  `max-dynamic-data: N` annotation, that's the cap. (This is the same
  annotation defined in `pjson-spec.md` §16.)
- The **inferred bound**: `max_encoded_size(T)` — a recursive walk
  that returns Some(n) when every variable-length sub-field of T
  carries a `length_bound{.max = M}` (or `.exact = M`) annotation,
  producing a finite upper bound. Returns None when any sub-field is
  unbounded.

The smaller of the two wins. Both can independently be None; the
combined result is None only when neither constrains the size.

### 3.2 max_encoded_size(T)

| T                                | max_encoded_size(T)                           |
|----------------------------------|-----------------------------------------------|
| `bool`                           | 1                                             |
| `i8 / u8`                        | 1                                             |
| `i16 / u16`                      | 2                                             |
| `i32 / u32 / f32`                | 4                                             |
| `i64 / u64 / f64`                | 8                                             |
| `u128 / i128`                    | 16                                            |
| `u256`                           | 32                                            |
| enum with underlying U           | sizeof(U)                                     |
| `Array(T, N)`               | N × max_encoded_size(T) [None if T is None]   |
| `Bitvector(N)`                   | ⌈N/8⌉                                         |
| `Bitlist(N)`                     | ⌈N/8⌉ + 1   (delimiter bit, see §5.7)         |
| `BoundedString(N)`              | N + W                                         |
| `BoundedList(T, N)`             | N × max_encoded_size(T) + W [None if T None]  |
| `String`                    | None                                          |
| `Vector(T)`                 | None                                          |
| `Optional(T)` where min(T)=0 | 1 + max(T)                                   |
| `Optional(T)` where min(T)>0 | max(T)                                       |
| `Variant(Ts...)`            | 1 + max(max_encoded_size(T_i))                |
| reflected record (DWNC)          | sum(fixed_size_of(field_i))                   |
| reflected record (extensible)    | W + sum(per-field max contribution)           |

Per-field max contribution for an extensible record:

- Fixed field: `fixed_size_of(field)`
- Variable field: `W + max_encoded_size(field)` (the W is the offset
  slot in the fixed region; the rest is the payload in the dynamic
  region; None if field is None)

If any field's contribution is None, the record's
`max_encoded_size` is None.

### 3.3 fixed_size_of(T)

Used to compute fixed-region size. Defined for fixed-shape T only:

| T                                | fixed_size_of(T)                  |
|----------------------------------|-----------------------------------|
| `bool`                           | 1                                 |
| arithmetic / enum                | sizeof(T)                         |
| `u128 / i128`                    | 16                                |
| `u256`                           | 32                                |
| `Array(T, N)`               | N × fixed_size_of(T)              |
| `Bitvector(N)`                   | ⌈N/8⌉                             |
| reflected record (DWNC)          | sum(fixed_size_of(field_i))       |
| reflected record (extensible)    | **W** (only the offset slot)      |
| variable T (string, vector, ...) | **W** (only the offset slot)      |

For a variable field inside a record, the fixed region holds only the
W-byte offset slot pointing at the payload. The payload itself lives
in the dynamic region.

### 3.4 min_encoded_size(T)

Used by the optional-encoding rule (§5.5).

| T                                | min_encoded_size(T)                  |
|----------------------------------|--------------------------------------|
| arithmetic, enum, ext_int, bool  | sizeof(T)                            |
| `Array(T, N)` (N ≥ 1)       | N × min(T)                           |
| `Bitvector(N)`                   | ⌈N/8⌉                                |
| `Bitlist(N)`                     | 1                                    |
| `String`                    | 0                                    |
| `Vector(T)`                 | 0                                    |
| `BoundedString(N)`              | 0                                    |
| `BoundedList(T, N)`             | 0                                    |
| `Optional(T)`               | 0                                    |
| reflected record (DWNC)          | fixed_size_of(T)                     |
| reflected record (extensible)    | W (the empty header alone)           |

A type with `min_encoded_size(T) > 0` cannot encode to zero bytes,
so an enclosing optional can disambiguate None ↔ Some by looking at
the byte-span length alone (§5.5).

## 4. Scalar Encodings

### 4.1 Booleans

A `bool` is one byte: `0x00` for false, `0x01` for true. Decoders
must accept any nonzero value as true (some legacy inputs use
`0xFF`); encoders must emit `0x01`.

### 4.2 Unsigned and signed integers

Sized integers are little-endian raw bytes:

- u8 / i8: 1 byte
- u16 / i16: 2 bytes
- u32 / i32: 4 bytes
- u64 / i64: 8 bytes

Two's-complement representation for signed types.

### 4.3 128-bit and 256-bit integers

`u128` / `i128`: 16 bytes little-endian. `u256`: 32 bytes
little-endian. (psio's `int128` / `uint128` / `uint256` types.)

### 4.4 IEEE floats

`f32`: 4 bytes IEEE 754 binary32 in little-endian memory order. `f64`:
8 bytes IEEE 754 binary64 in little-endian memory order. NaN payloads
are preserved bit-for-bit by both encoder and decoder.

### 4.5 Enums

Encoded as the underlying integral type. Decoder is responsible for
range-checking against the declared variants (encoder may emit a
value the decoder doesn't recognize; decoder rejects with
`enum_out_of_range`).

## 5. Containers

### 5.1 Records (reflected structs)

A record is a heterogeneous collection of named fields in a fixed
declaration order. There are two encoding paths: extensible
(default) and DWNC (opt-in via the `dwnc` flag
annotation).

#### 5.1.1 Extensible record (default)

```
[ u{W} fixed_size  | fixed region         | dynamic region                 ]
[      W bytes     | sum(fixed_size_of)   | concatenated variable payloads ]
```

- **`fixed_size`** — u{W} value equal to the byte size of the fixed
  region as written by the encoder. Lets a newer decoder with more
  fields detect that the writer wrote only the first M fields and
  treat the rest as default. Encoder writes
  `sum(fixed_size_of(field_i))` over the fields it actually emitted
  (which equals the schema's full fixed-region size unless trailing
  defaults are pruned — see §7.2).
- **Fixed region** — for each field i in declaration order:
  - If field i is fixed-shape: the inline value, `fixed_size_of(F)`
    bytes. (E.g., a `u32` field contributes 4 bytes here.)
  - If field i is variable-shape: a u{W} **offset slot**, the
    container-relative byte distance from the start of the fixed
    region to field i's payload in the dynamic region.
- **Dynamic region** — variable-field payloads, concatenated in
  declaration order, no padding, no per-field length prefix. Each
  payload's size is derived from the offset table:

  ```
  size(field_i) = offset(field_{i+1}) - offset(field_i)        // i has a successor
                = fixed_region_end_of_payload - offset(field_i) // i is the last variable field
  ```

  where the synthetic "next offset" for the last variable field is
  the end of the dynamic region (i.e., the buffer end relative to
  fixed-region start).

#### 5.1.2 DWNC record (opt-in)

A record marked the `dwnc` flag skips the extensibility
header:

```
[ fixed region | dynamic region ]
```

The encoded bytes are the same as the extensible form **minus** the
leading u{W} `fixed_size` field. Decoder knows to skip header reading
because the schema says DWNC. Saves W bytes per record.

DWNC implies:
- Field count is fixed forever.
- Field types are fixed forever.
- Field order is fixed forever.

DWNC does NOT require that all fields be fixed-shape. A DWNC record
can still have variable fields; the dynamic region works the same
way.

#### 5.1.3 DWNC + all-fixed-fields = memcpy fast path (informative)

If a record is DWNC, every field is fixed-shape, and the in-memory C
layout matches `fixed_size_of(record)` exactly (no implicit padding,
or `__attribute__((packed))` applied), the encoder may emit the value
via a single `memcpy(buffer, &value, sizeof(value))`. The decoder may
inverse-memcpy.

This is a performance optimization, not a wire-format choice — the
bytes are identical to what the field-by-field walker produces. The
fast path is permitted; an implementation that always uses the
walker is conformant.

#### 5.1.4 Encode example: extensible record with one string

Schema (notation: `Record { name: Type, ... }`):

```
Record Greeting {
    number:  u32      // fixed field
    message: String   // variable field
}
```

Width selection: `message` is unbounded, so `effective_max_dynamic
= none`, so W = 4.

`fixed_size_of(Greeting)` = sizeof(u32) + W = 4 + 4 = 8.

For the value `Greeting { number = 42, message = "hi" }`:

```
Offset  Bytes              Meaning
--------------------------------------------
00      08 00 00 00         u32 fixed_size = 8
04      2A 00 00 00         u32 number = 42
08      08 00 00 00         u32 offset = 8 (relative to fixed-region start = byte 04)
12      68 69               "hi"   ← dynamic region begins at byte 12; relative
                                     to fixed-region start = 12 - 4 = 8 ✓
```

Total: 14 bytes.

(Header at 0..4, fixed region at 4..12, payload at 12..14. Offset
8 = (payload start) - (fixed region start) = 12 - 4 = 8.)

### 5.2 Arrays — `Array(T, N)`

Encoded as N elements of T, concatenated in order, no separator, no
length prefix (N is fixed by the schema).

If T is fixed-shape, the array is fixed-shape with size N ×
fixed_size_of(T).

If T is variable-shape, the array is variable-shape: it has its own
dynamic region with offset slots in its fixed region. (This is the
rare case — typically arrays hold fixed-shape elements.) Layout in
that case:

```
[ N × u{W} offset slots | concatenated variable payloads ]
```

Offsets are relative to the start of the array's fixed region (i.e.,
the start of the offset slots).

### 5.3 Vectors and bounded lists — `Vector(T)`, `BoundedList(T, N)`

A variable-length sequence of T values. As a record field, the
vector's payload lives in the dynamic region of the enclosing record;
the enclosing record's fixed region holds the offset slot.

Within the vector's payload, the layout depends on T's shape:

#### 5.3.1 Vector of fixed-shape T

```
[ T_0 | T_1 | ... | T_{count-1} ]
```

Element count is **derived** from the payload size:
`count = payload_size / fixed_size_of(T)`. No explicit count field —
this is the implicit-sizing property.

A zero-element vector is a zero-byte payload.

#### 5.3.2 Vector of variable-shape T

```
[ count_offsets × u{W} | T_0_payload | T_1_payload | ... ]
```

The count is determined by walking the offset slots until they exceed
the payload's total size. (Implementation hint: the offset table size
is `count × W`, and `offsets[0]` is always equal to that table size,
so count = offsets[0] / W.)

Offsets are container-relative within the vector payload.

#### 5.3.3 String

A specialization of `Vector(u8)`: the payload is the raw UTF-8
bytes, size derived from the enclosing offset arithmetic. No length
prefix. No null terminator.

#### 5.3.4 BoundedString(N)

Same wire form as `String`. The bound N participates in width
selection (§3) but is not encoded on the wire.

### 5.4 Bitvectors and bitlists

#### 5.4.1 Bitvector(N)

Fixed-size bitset of N bits. Encoded as ⌈N/8⌉ bytes, **least
significant bit first** within each byte. Bit i is in byte i/8 at bit
position i%8.

Trailing bits in the final byte (if N is not a multiple of 8) must be
zero; encoders must zero them; decoders must reject inputs with
nonzero trailing bits.

#### 5.4.2 bitlist<N>

Variable-length bitset, capacity N. Encoded with a delimiter bit:

```
bytes = bitlist_data || [delimiter]
```

The delimiter is the first 1-bit at position `length` (0-indexed from
the start). The actual bit count = position of the highest 1-bit in
the encoded bytes.

Concretely: if the bitlist has L bits, encoded byte count =
⌈(L+1)/8⌉, and the byte at position L/8 has bit L%8 set as the
delimiter; bits past that position are zero.

A zero-length bitlist encodes as `[0x01]` (delimiter at position 0).

### 5.5 Optionals — `Optional(T)`

Encoding depends on `min_encoded_size(T)` (§3.4):

#### 5.5.1 No-selector form (min(T) > 0)

```
None  → 0 bytes
Some  → encoding(T)
```

The decoder distinguishes None from Some by **payload size alone**:
zero bytes means None, any nonzero size means Some. This works
because T cannot encode to zero bytes when min(T) > 0.

This is the no-overhead form and applies to: arithmetic primitives,
enums, fixed-size arrays of arithmetic, bitvectors, DWNC records,
extensible records (whose minimum is the W-byte header).

#### 5.5.2 1-byte selector form (min(T) == 0)

```
None  → [0x00]
Some  → [0x01, encoding(T)]
```

Used when T can encode to zero bytes — strings, vectors, bounded
lists, bounded strings, nested optionals — because the size-only
disambiguation would conflate `None` with `Some(empty)`.

The selector byte is part of the payload, not a separate field. An
optional inside a record contributes a single offset slot pointing
at the payload (which is either `[0x00]` or `[0x01, ...]`).

#### 5.5.3 Why not always selector?

The no-selector form saves 1 byte per Some value across many
fixed-shape fields — significant for record types with several
optional u64 fields where the sum-of-bytes matters for cache
locality.

### 5.6 Variants — `Variant(T_0, T_1, ..., T_{n-1})`

A tagged union with up to 256 alternatives.

```
[ u8 selector | encoding(T_selector) ]
```

The selector byte is the alternative index 0..n-1. Decoder rejects
selector ≥ n.

For a variant inside a record field, the entire variant (selector +
payload) lives in the dynamic region; the fixed region holds an
offset slot pointing at the selector byte.

(If a variant has > 256 alternatives, it must be widened to u16.
This is an open extension; today implementations should
static-assert against > 256.)

### 5.7 Top-level encoding

A top-level pssz value (the buffer contents) is encoded **as if it
were a record field of the same type**:

- A top-level reflected record encodes with its u{W} extensibility
  header (or no header if DWNC).
- A top-level scalar encodes inline (no offset slot, no header).
- A top-level vector / string encodes as raw payload bytes (no offset
  slot, no header). The decoder's caller is expected to know the
  buffer's total byte length.
- A top-level optional follows §5.5.

There is no version byte, no magic number, no document framing — the
schema fully describes the bytes. (Compare pjson, which has a magic
byte, version, and flags header at the document level.)

## 6. Document — top-level

### 6.1 Buffer interpretation

A pssz buffer is a sequence of bytes whose interpretation requires:

1. The schema (the type description T at the root).
2. Both encoder and decoder agree on the width tag W derived from T
   per §3.

Given those, encoding is deterministic (modulo trailing-default
pruning, see §7.2) and decoding is total.

### 6.2 Buffer self-delimiting

A pssz buffer is **not** self-delimiting on its own. The decoder must
know the buffer's exact byte length from out-of-band context (e.g.,
TCP message length, file size, RPC framing). Within that length,
container sizes are derived by offset arithmetic.

If a transport requires self-delimiting payloads, it should wrap the
pssz buffer in its own length prefix or framing.

## 7. Schema Evolution

### 7.1 Forward compatibility (new decoder, old encoder)

The decoder schema declares N fields; the encoded buffer was written
by an encoder that knew about M ≤ N fields. The u{W} `fixed_size`
header in the buffer reads as `M × per-field-fixed-contribution` in
the writer's view. The decoder:

1. Reads the u{W} `fixed_size` header.
2. Walks fields 0 .. (some K such that
   `sum_{i=0..K-1}(fixed_size_of(field_i)) ≤ fixed_size`).
3. For each field K..N-1 (those the encoder didn't write), synthesizes
   the field's default value.

The default depends on the field's type:
- Optional: None.
- Vector / string: empty.
- Arithmetic: 0.
- Reflected record: recursively-defaulted record.
- Variant: undefined; decoders should reject extension that adds a
  variant field, since there's no canonical default.

### 7.2 Trailing-default pruning (encoder)

When the trailing K fields of a record are all at their default
values, the encoder MAY drop them entirely:

- Skip writing their fixed-region contribution.
- Write a smaller `fixed_size` header reflecting only the unpruned
  prefix.

Decoders must accept pruned records (the synthesis path in §7.1
covers this case). The encoder's choice to prune is informational —
two pssz encodings of the same value differing only in pruning are
both valid and both must round-trip.

A canonical encoding (§15) requires pruning to maximum extent.

### 7.3 Backward compatibility (old decoder, new encoder)

The decoder schema declares M fields; the encoded buffer was written
by an encoder that knew about N > M fields. The u{W} header indicates
`fixed_size = N × per-field-fixed-contribution`, larger than what the
old decoder expects. The old decoder:

1. Reads the u{W} `fixed_size` header.
2. Walks fields 0 .. M-1 (its own knowledge), reading fixed-region
   bytes for those fields.
3. Computes the dynamic-region start as `fixed_region_start +
   fixed_size` (using the writer's claimed size, NOT the reader's
   M-field fixed_size).
4. Resolves variable-field offsets and reads payloads as usual.
5. Discards any fixed-region bytes between `M × per-field` and
   `fixed_size` (those are the writer's extra fields the reader
   doesn't know about — silent drop).

The dynamic region may also contain payloads for fields the decoder
doesn't know about; those occupy bytes between the last
known-variable-field's payload and the end of the buffer. Decoder
silently ignores them.

### 7.4 Unknown-field tolerance: yes-or-no?

pssz favors the **silent drop** semantics for unknown fields — the
decoder discards bytes it doesn't understand. This matches fracpack
and protobuf 3 default behavior. It does NOT match
"reject-on-unknown" semantics (which some applications need for
security-critical paths).

For applications that require "reject any unknown field", a separate
`validate_strict` entry point should be exposed; the wire format does
not change.

### 7.5 What you can change without breaking old decoders

✓ Append a new field at the end (any type).
✓ Change a field's `length_bound{.max}` annotation to a tighter or
  looser value, provided W doesn't change. (If W changes, callers
  must rebuild against the new schema; the wire format is incompatible.)
✓ Add a new alternative to a variant (provided the new alt is at the
  end of the variant's alternative list).

### 7.6 What you CANNOT change without breaking old decoders

✗ Reorder declared fields.
✗ Change a field's type (even between same-size types — e.g., u32 →
  i32 is a wire-compatible change but a semantic break and decoders
  may legitimately reject the new bytes when they encode a value
  outside the new type's range).
✗ Insert a field in the middle.
✗ Rename a field — the wire format doesn't carry names (compare
  pjson and protobuf), so this is a no-op for the wire but consumers
  using reflected names will break.
✗ Reorder variant alternatives.
✗ Decrease a field's `length_bound{.max}` to a value smaller than
  any encoded buffer. (Width may shift narrower, breaking old wire
  bytes.)
✗ Add the `dwnc` flag to a previously-extensible record.
  (DWNC removes the header, so old decoders reading a header where
  there isn't one will misinterpret bytes.)

## 8. Limits

### 8.1 Width-imposed limits

For a container with width tag W:

- W = 1: total encoded size ≤ 256 bytes.
- W = 2: total encoded size ≤ 65 536 bytes.
- W = 4: total encoded size ≤ 4 GiB. (4 294 967 296 bytes — practical
  limit for in-memory single-buffer parse.)

### 8.2 Cap-imposed limits

If T carries a `max-dynamic-data: N` annotation, the encoded total must
satisfy `total ≤ N`. Encoders that exceed N MUST throw / return an
error rather than silently widen to a larger W. Decoders MAY enforce
the cap by rejecting encoded buffers with `bytes.size() > N`.

### 8.3 Recursion / depth

Implementations should impose a maximum nesting depth (recommended:
64 levels of containers) to bound stack usage during decode.

### 8.4 Variant alternatives

Variant alternative count limited to 256 (selector is u8). Schemas
exceeding 256 must split into multiple variants or use a different
encoding.

## 9. Errors

A pssz decoder can produce the following error categories:

- **buffer_too_short** — the buffer ended before a needed field.
- **offset_out_of_range** — an offset slot points outside the
  buffer.
- **offset_non_monotonic** — offsets[i+1] < offsets[i] (encoder
  emitted a malformed offset table).
- **fixed_size_mismatch** — `fixed_size` header value is inconsistent
  with what the schema expects (e.g., not a multiple of
  per-field-fixed contribution).
- **enum_out_of_range** — an integer field declared as an enum
  carries a value outside the declared variants.
- **bool_invalid** — a bool field contains a value not in {0, 1}
  (some implementations accept any nonzero as true; strict mode
  rejects).
- **variant_index_out_of_range** — variant selector ≥ n alternatives.
- **bitvector_trailing_bits_set** — bitvector has nonzero bits past
  position N-1.
- **bitlist_no_delimiter** — bitlist payload contains no 1-bit, so
  no length can be derived.
- **utf8_invalid** — a string field contains bytes not forming valid
  UTF-8 (strict mode only; default decoders pass strings through
  uninterpreted).
- **cap_exceeded** — buffer size exceeds the type's
  `max-dynamic-data: N` cap.

Errors must be reported with a byte offset where possible.

## 10. Reference Algorithm — Read

```
fn decode(T, bytes, [offset = 0, end = bytes.size()]):
    return decode_value(T, bytes, offset, end)

fn decode_value(T, bytes, pos, end):
    match shape(T):
        FIXED_PRIMITIVE:
            assert end - pos >= sizeof(T)
            return read_le_int(bytes[pos..pos+sizeof(T)])
        STD_ARRAY(elem, N):
            arr = []
            for i in 0..N:
                arr.push(decode_value(elem, bytes, pos, end))
                pos += encoded_size(elem, bytes, pos)
            return arr
        BITVECTOR(N):
            return parse_bitvector(bytes, pos, N)
        BITLIST(N):
            return parse_bitlist(bytes, pos, end)
        STRING / VECTOR (variable):
            assert pos == start_of_payload  // caller ensured
            return parse_payload_until_end(bytes, pos, end)
        OPTIONAL(T):
            return decode_optional(T, bytes, pos, end)
        VARIANT(Ts):
            return decode_variant(Ts, bytes, pos, end)
        REFLECTED_RECORD:
            return decode_record(T, bytes, pos, end)

fn decode_record(T, bytes, pos, end):
    is_dwnc = T.definition_will_not_change?
    W       = auto_pssz_width(T)

    if is_dwnc:
        fixed_size = sum_fixed(T.declared_fields)
        fixed_start = pos
    else:
        fixed_size = read_uint_le(bytes, pos, W)
        fixed_start = pos + W

    fixed_end   = fixed_start + fixed_size
    dynamic_end = end

    record = empty_record(T)
    cursor = fixed_start
    var_offset_slots = []

    for (i, field) in enumerate(T.declared_fields):
        if cursor + fixed_size_of(field) > fixed_end:
            // Trailing-default-pruning case
            record[i] = default(field)
            continue

        if is_fixed(field):
            record[i] = decode_value(field.type, bytes, cursor, fixed_end)
            cursor += fixed_size_of(field)
        else:
            offset = read_uint_le(bytes, cursor, W)
            var_offset_slots.push((i, offset))
            cursor += W

    // Resolve variable fields by walking offset slots in order
    sorted_slots = sort_by_offset(var_offset_slots)
    for j in 0..sorted_slots.len():
        (i, offset)            = sorted_slots[j]
        payload_start          = fixed_start + offset
        payload_end_relative   = (j+1 < sorted_slots.len())
                                 ? sorted_slots[j+1].offset
                                 : (dynamic_end - fixed_start)
        payload_end            = fixed_start + payload_end_relative
        record[i]              = decode_value(field_i.type, bytes,
                                              payload_start, payload_end)

    return record

fn decode_optional(T, bytes, pos, end):
    payload_size = end - pos
    if payload_size == 0:
        return None
    if min_encoded_size(T) > 0:
        return Some(decode_value(T, bytes, pos, end))
    else:
        sel = bytes[pos]
        if sel == 0x00:
            assert payload_size == 1
            return None
        if sel == 0x01:
            return Some(decode_value(T, bytes, pos+1, end))
        error("optional selector invalid")

fn decode_variant(Ts, bytes, pos, end):
    sel = bytes[pos]
    if sel >= len(Ts):
        error("variant selector out of range")
    return Variant(sel, decode_value(Ts[sel], bytes, pos+1, end))
```

## 11. Reference Algorithm — Field Lookup

For zero-copy view access, finding the byte span of field i without
decoding any other field:

```
fn field_span(T, bytes, i):
    is_dwnc = T.definition_will_not_change?
    W       = auto_pssz_width(T)
    fixed_start  = is_dwnc ? 0 : W
    fixed_size   = is_dwnc ? sum_fixed(T) : read_uint_le(bytes, 0, W)
    fixed_end    = fixed_start + fixed_size

    fixed_offset_of_i = sum(fixed_size_of(field_j) for j in 0..i)
    cursor            = fixed_start + fixed_offset_of_i

    if cursor + fixed_size_of(field_i) > fixed_end:
        return EMPTY  // field i is in the pruned tail; default

    if is_fixed(field_i):
        return Span(cursor, fixed_size_of(field_i))

    // Variable field: read offset, find next variable field's offset,
    // span = bytes[fixed_start + offset_i, fixed_start + offset_{i+1})
    offset_i = read_uint_le(bytes, cursor, W)

    next_var = find_next_variable_field(T, after = i)
    if next_var is None:
        end = bytes.size() - fixed_start
    else:
        next_offset_pos = fixed_start +
                          (sum(fixed_size_of(field_j) for j in 0..next_var))
        end = read_uint_le(bytes, next_offset_pos, W)

    return Span(fixed_start + offset_i, end - offset_i)
```

This is **O(1) in the field index** (the sums are computed at
compile time from the schema, not at runtime per access).

## 12. Reference Algorithm — Encode

```
fn encode(T, value, sink):
    encode_value(T, value, sink)

fn encode_value(T, value, sink):
    match shape(T):
        FIXED_PRIMITIVE:
            sink.write_le(value, sizeof(T))
        STD_ARRAY:
            for elem in value:
                encode_value(elem_type, elem, sink)
        BITVECTOR(N):
            sink.write(pack_bits_lsb_first(value, N))
        BITLIST(N):
            sink.write(pack_bits_with_delimiter(value))
        STRING / VECTOR<u8>:
            sink.write(raw_bytes(value))
        VECTOR<T> (T fixed):
            for elem in value:
                encode_value(T, elem, sink)
        VECTOR<T> (T variable):
            offsets_section_size = value.count() * W
            offsets = []
            payload = []
            cursor  = offsets_section_size
            for elem in value:
                offsets.push(cursor)
                tmp = encode_to_buffer(elem)
                payload.append(tmp)
                cursor += tmp.size
            for off in offsets:
                sink.write_le(off, W)
            sink.write(payload)
        OPTIONAL(T):
            if value is None:
                if min_encoded_size(T) > 0:
                    pass    // emit nothing
                else:
                    sink.write(0x00)
            else:
                if min_encoded_size(T) == 0:
                    sink.write(0x01)
                encode_value(T, value, sink)
        VARIANT:
            sink.write(value.index)
            encode_value(value.alternative_type, value.payload, sink)
        REFLECTED_RECORD:
            encode_record(T, value, sink)

fn encode_record(T, value, sink):
    is_dwnc       = T.definition_will_not_change?
    W             = auto_pssz_width(T)
    fixed_size    = sum_fixed(T.declared_fields)

    if !is_dwnc:
        sink.write_le(fixed_size, W)

    fixed_start_pos = sink.position()
    sink.skip(fixed_size)
    cursor = fixed_start_pos
    payloads_after_fixed_region_start = 0

    for field in T.declared_fields:
        if is_fixed(field):
            sink.rewrite(cursor, encode_to_bytes(field, value[field]))
            cursor += fixed_size_of(field)
        else:
            payload = encode_to_buffer(field.type, value[field])
            // First emit the offset; then append payload
            sink.rewrite_le(cursor, payloads_after_fixed_region_start
                                    + fixed_size, W)
            cursor += W
            sink.append(payload)
            payloads_after_fixed_region_start += payload.size

    // Optional: trailing-default pruning. Walk fields backward,
    // re-truncating fixed_size if the trailing field is at default.
    // (Implementation detail; canonical encoding requires this.)
```

In practice, the offset arithmetic and size precomputation can be
fused into a single forward pass with backpatching when the encoder
knows fixed_region size up front.

## 13. Versioning

pssz buffers carry no version byte — the schema fully determines the
layout. "Versioning" means schema versioning, governed by §7.

A consumer who needs to detect schema-mismatch at decode time should
prefix the buffer with their own application-level discriminator
(e.g., a 4-byte schema hash or a u32 schema-version), then strip
that prefix before passing the buffer to the pssz decoder.

## 14. Conformance Tests

A pssz implementation passes conformance when:

1. **Round-trip identity.** For every value `v` of every type T in the
   conformance corpus, `decode(T, encode(T, v)) == v`.
2. **Byte-equality across implementations.** For every value `v`,
   `encode_canonical(T, v)` produces a byte sequence that is identical
   to the reference implementation's output. (Non-canonical encodings
   are permitted to differ in trailing-default pruning; canonical
   encodings must match exactly.)
3. **Cross-language byte equality.** psio's C++ and Rust pssz
   implementations both produce identical bytes for the conformance
   corpus.
4. **Schema-evolution scenarios.** A v1 encoder + v2 decoder
   round-trip preserves all v1 fields verbatim; v2 fields are
   defaulted. Reverse: v2 encoder + v1 decoder reads v1 fields
   correctly and silently drops v2-only fields.
5. **Width-boundary behavior.** A type whose
   `effective_max_dynamic(T)` straddles the 0xff or 0xffff boundary
   must produce different byte counts depending on which side of the
   boundary the encoded value falls — but the schema-derived W is
   determined at type-resolution time, not per-value.

The conformance corpus must include:

- All scalar primitives (smallest, largest, midpoint values).
- Empty / one-element / large-N strings and vectors.
- Optional<fixed> and Optional<variable>, both Some and None.
- Variants with each alternative selected.
- Records with various field-shape mixes (all-fixed, all-variable,
  mixed).
- DWNC records (with header skipped).
- Records where trailing fields are at their defaults (testing
  pruning).
- Recursive records (records containing records).
- Indirection wrappers used as cycle-breakers (recursive types
  that bottom out through a heap-allocated owning pointer)
  passing through transparently.

## 15. Canonical Encoding

A pssz encoding is **canonical** when:

1. Trailing-default pruning is applied to its maximum extent: any
   trailing field at its default value is dropped from the encoding.
2. Variable-field offsets in the fixed region are in field-declaration
   order. (Implementations are technically free to permute payloads
   if they emit consistent offsets, but canonical pssz requires
   declaration order.)
3. No unused / "padding" bytes between payloads in the dynamic
   region.
4. The width tag W is the minimum width permitted by the schema's
   `effective_max_dynamic(T)`. (W = 1 if the schema's effective
   bound is ≤ 0xff, etc. — see §3.)

A canonical encoder always produces a canonical encoding. A
non-canonical decoder accepts both canonical and non-canonical inputs
(as long as the non-canonical input is well-formed).

For applications that hash pssz bytes (signatures, content-addressed
storage), use canonical encoding to ensure equal values produce equal
hashes regardless of which encoder produced them.

## 16. Type-level caps

pssz honors the type-level caps documented in `pjson-spec.md` §16:

- **`max-fields: N`** — at decode time, the validator may reject
  records whose declared field count exceeds N. (Today this is a
  schema-level discipline; pssz is concerned with byte counts more
  than field counts.)
- **`max-dynamic-data: N`** — drives both width selection (§3) and an
  explicit ceiling at encode + decode. Encoders that produce > N
  bytes throw; validators that see > N bytes in the input reject.

The interplay between explicit caps and per-field length bounds is:
explicit cap wins downward (cap < bound ⇒ cap; cap ≥ bound ⇒ bound).
A canonical encoder's W is determined by `effective_max_dynamic(T)
= min(explicit_cap, inferred_bound)`.

When a record is DWNC + all fields are fixed-shape +
`max-dynamic-data: N` is small enough to drop W to 1 and the value's
actual size fits the cap, the encoded record is exactly
`fixed_size_of(T)` bytes — no header, no offsets, no overhead. This
is the minimum-size case for pssz.

## Appendix A. Glossary

- **Container.** A compound type with internal layout (record,
  array, vector, optional, variant, bitvector, bitlist, top-level
  document).
- **Container-relative offset.** Offset measured from the **start of
  the fixed region** of an enclosing container.
- **DWNC.** Definition-will-not-change. Annotation skipping the u{W}
  extensibility header.
- **Dynamic region.** Concatenated payloads of variable-shape fields,
  following the fixed region.
- **Extensibility header.** The u{W} `fixed_size` value at the front
  of an extensible record, indicating how many fixed-region bytes
  the writer emitted.
- **Field.** A named slot in a record, indexed by declaration order.
- **Fixed field.** Field of a fixed-shape type — encoded inline in
  the fixed region.
- **Fixed region.** The contiguous bytes after the extensibility
  header (or at the start, for DWNC) that hold inline fixed-field
  values and variable-field offset slots.
- **Implicit sizing.** Variable-field size is derived from offset
  arithmetic; no per-field length prefix.
- **min_encoded_size(T).** Minimum byte count any value of T encodes
  to. Drives the optional selector decision.
- **max_encoded_size(T).** Maximum byte count any value of T encodes
  to. Drives the auto-width selection.
- **Offset slot.** A u{W} value in the fixed region pointing at a
  variable field's payload in the dynamic region.
- **Trailing-default pruning.** Encoder optimization: drop trailing
  fields at their defaults; reduce `fixed_size` accordingly.
- **Variable field.** Field of a variable-shape type — encoded in
  the dynamic region with an offset slot in the fixed region.
- **Width tag W.** The byte width of offset slots and the
  extensibility header for a given type T. Derived from
  `effective_max_dynamic(T)`. W ∈ {1, 2, 4}.

## Appendix B. Worked Example — A complete pssz buffer

Schema:
```
record UserPrefs {
    user_id    : u32              // fixed
    display    : String           // variable
    last_login : Optional(u64)    // optional fixed (no selector)
    favorites  : Vector(u32)      // variable
}
```

No `dwnc` flag — extensible. No length bounds —
`max_encoded_size(UserPrefs) = None` ⇒ W = 4.

Per-field fixed contribution:
- `user_id` (u32 fixed): 4 bytes inline
- `display` (variable): 4 bytes (W = 4) offset slot
- `last_login` (optional<u64>, no selector): 4 bytes (W = 4) offset slot
- `favorites` (variable): 4 bytes (W = 4) offset slot

`fixed_size_of(UserPrefs)` = 4 + 4 + 4 + 4 = 16.

Value:
```
UserPrefs {
    user_id    = 7,
    display    = "alice",
    last_login = Some(0x000000005FFE0001),
    favorites  = [10, 20, 30],
}
```

Payload sizes:
- `display` payload: "alice" = 5 bytes
- `last_login` payload: Some(u64) → 8 bytes (no selector since
  min(u64) = 8 > 0)
- `favorites` payload: 3 × 4 = 12 bytes

Offsets in the fixed region (relative to fixed-region start):
- `display` payload starts immediately after fixed region:
  `offset = 16` (= `fixed_size_of`).
- `last_login` payload follows display: `offset = 16 + 5 = 21`.
- `favorites` payload follows last_login: `offset = 21 + 8 = 29`.

Encoded bytes (hex):

```
Offset  Bytes                                Meaning
---------------------------------------------------------
00      10 00 00 00                          u32 fixed_size = 16
04      07 00 00 00                          u32 user_id = 7
08      10 00 00 00                          u32 offset(display) = 16
0C      15 00 00 00                          u32 offset(last_login) = 21
10      1D 00 00 00                          u32 offset(favorites) = 29
14      61 6C 69 63 65                       "alice"          (5 B at byte 20)
19      01 00 FE 5F 00 00 00 00              u64 0x5FFE0001 LE
21      0A 00 00 00 14 00 00 00 1E 00 00 00  3 × u32 [10, 20, 30]
```

Total: 4 + 16 + 5 + 8 + 12 = 45 bytes.

(Aside: dropping `last_login` to None would prune it and one offset
slot — a canonical encoder omits trailing defaults; a non-canonical
one may keep the slot with offset = next-payload-start, yielding an
empty payload that the decoder reads as "no Some value" via the
size-zero rule.)

Verification of offset arithmetic from a decoder's perspective:

- `fixed_start` = 4 (after the u32 header).
- `display`: slot at byte 8 reads 16. Payload at fixed_start + 16 = 20.
  Next variable field's slot at byte 0xC reads 21. Payload size =
  21 − 16 = 5. Bytes 20..24 = "alice" ✓
- `last_login`: slot at byte 0xC reads 21. Payload at fixed_start + 21 = 25.
  Next slot at byte 0x10 reads 29. Payload size = 29 − 21 = 8.
  Bytes 25..32 = 0x000000005FFE0001 ✓
  Size > 0 → Some(0x5FFE0001) ✓
- `favorites`: slot at byte 0x10 reads 29. Payload at fixed_start + 29 = 33.
  No subsequent variable slot, so payload extends to buffer end (byte 45).
  Size = 45 − 33 = 12 = 3 × 4 ✓

Round-trip perfect.

## Appendix C. Discussion: why offsets are container-relative, not absolute

Two equivalent encodings exist for the offset table:

1. **Container-relative** (pssz's choice): each offset slot is the
   byte distance from the start of the fixed region.
2. **Buffer-absolute**: each offset slot is the byte distance from
   the start of the buffer.

Container-relative is preferred because:

- A pssz reader (zero-copy view) constructed over a sub-span (a
  pointer into another buffer's interior) doesn't need to know
  its parent buffer's start. Offsets resolve as
  `fixed_start + offset_i`, using the local fixed_start the
  reader holds.
- Recursive sub-records can be passed to nested decode functions
  with a sub-span, no rebasing.
- The single-pass encoder doesn't need to know its absolute
  position in any larger buffer; it just emits offsets relative to
  the start of the fixed region it's currently writing.

The cost: in a non-DWNC top-level record, offset 0 in any slot is
the byte AFTER the W-byte header — never the buffer start. Decoders
must remember to add `fixed_start` (= W for non-DWNC, = 0 for DWNC)
when resolving an offset to an absolute buffer position.

End of spec.
