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

pSSZ is the successor to fracpack, a legacy prototype retained as a benchmark
comparison to demonstrate the relative performance of different data-layout
designs.

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

### 1.3 Comparison summary

The detailed format-vs-format comparison — qualitative property
checklist, measured geomean ratios on size / encode / decode /
validate / view, and compressed-wire-size comparison after
lz4 / zstd — lives in [`format-comparison.md`](format-comparison.md).
The headline:

- **pssz wins cumulative.** No competing format scores below 1.00
  on the geomean across (size, encode, decode, validate, view).
  The closest peer is ssz at 1.13× — same offset-table family,
  but missing DWNC memcpy fast paths so it pays 76% on encode.
- **Varint formats are smaller, but pay for it.** avro / msgpack
  / protobuf encode integers in variable-length groups (size 0.56–
  0.65× pssz), but variable-byte fields force a sequential walk to
  reach any later field — so they can't offer zero-copy views,
  O(1) random access, or trailing-default pruning, and clock 30–
  223× pssz on encode and 4–53× on decode.
- **Schema-tooled zero-copy formats pay for their tooling.**
  capnp / flatbuf have zero-copy views and extensibility too, but
  pay more on size (1.59–1.73×) AND much more on encode (56–78×)
  for IDL/codegen pipelines pssz today doesn't match.
- **Self-describing formats** — pjson / json / bson — carry the
  schema in the bytes; geomean lands at 13–42× pssz cumulatively.
- **After zstd-1 the size spread compresses to a 1.56× window**
  (avro 0.87× → flatbuf 1.36×) from a 2.5× raw-size range
  (avro 0.76× → json 1.89×). If the transport compresses, the
  few bytes pssz spends on its offset table cost <13% on the
  wire against any other format — and flatbuf actually
  compresses *worse* than pssz because its vtable u16 values
  have less zero-byte redundancy than pssz's offset table.

The single architectural sentence: **zero-copy views and O(1)
random access require an offset table with fixed-width slots,
and that table is what costs the few bytes that varint formats
save.** pssz's bet is that the access-pattern properties are
worth more than those bytes — and the geomean confirms that
bet across every dimension we measure.

§1.4 below explains what each property row in the comparison
means and why it matters.

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
of ≤ 255 bytes turns every offset slot from 4 bytes to 1 byte —
saving 3 × N bytes for an N-field record, and as much per element in
the offset table of a bounded list. SSZ is fixed at 4-byte
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
For a record with 4 strings at fracpack32 → pssz32: 16 bytes saved per
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

| Shape                 | bin | borsh | bincode | ssz | pssz | fracpack | wit | flatbuf | capnp | msgpack | json   | bson   |
|-----------------------|----:|------:|--------:|----:|-----:|---------:|----:|--------:|------:|--------:|-------:|-------:|
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
  The container's total encoded size is at most 255 bytes.
- W = 2: offsets and the extensibility header are 2 bytes each (u16).
  The container's total is at most 65 535 bytes.
- W = 4: offsets and the extensibility header are 4 bytes each (u32).
  The container's total is below 4 GiB.

A container is anything that writes offsets: a record, tuple or pair
(header and field slots), and an array, list, map or set of variable
elements (the offset table). Each has its own W.

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

`effective_max_dynamic(T)` is:

- The **explicit cap**, when T carries a type-level
  `max-dynamic-data: N` annotation. (This is the same annotation
  defined in `pjson-spec.md` §16.) An explicit cap always rules: it is
  not reduced to the inferred bound when the current fields happen to
  be smaller.
- Otherwise the **inferred bound**: `inferred_bound(T)` — a recursive
  walk that returns Some(n) when every variable-length value inside T
  is bounded, producing a finite upper bound. Returns None when any of
  them is unbounded. A value is bounded by its type
  (`BoundedString(N)`, `BoundedList(T, N)`) or by an annotation on the
  member that holds it, at any depth (§3.5).

The result is None only when T has no explicit cap and its inferred
bound is None; W is then 4. The bound is rounded up to the next width:
1 byte through 255, 2 bytes through 65 535, 4 bytes above.

**Every type that has offsets follows this rule, each for itself.** A
record, a tuple and a pair write a header and one offset slot per
variable member (§5.1); an array, a list (`Vector(T)`,
`BoundedList(T, N)`), a map and a set of variable elements write an
offset table of one slot per element (§5.2, §5.3.2). Each takes its W
from its **own** cap or bound: a list of at most 8 strings of at most 16
bytes has a 1-byte table whatever record holds it, and that record has
its own W. A list has no cap of its own, so its W is the width class of
its inferred bound: 4 for an unbounded list (`Vector(T)`, or a bounded
list of unbounded elements), and 1 or 2 when both its count and its
elements are bounded (by type or by annotation, §3.5) and the bound is
small enough.

**One width per type, known from the schema.** W is never chosen from
the size of a value: a value of a 2-byte list that happens to fit in 200
bytes is still written with 2-byte offsets. Every value has exactly one
encoding.

**Evolution and width.** W is fixed for the lifetime of a type. An
optional field may be appended (§7) only if it does not change W. A
type without an explicit cap therefore has only the slack between its
inferred bound and the top of its width class (255, 65 535). A type
that needs room for future optional fields must declare an explicit
cap large enough to cover them from the start.

### 3.2 inferred_bound(T)

`inferred_bound(T)` is the number widths are chosen from. It is computed
from what T holds and **never from T's own cap**. (Earlier revisions of
this document, and the fixture field before `inferred_bound` was added,
called it `max_encoded_size`. That name now belongs to §3.7, the largest
encoding: for a type with no cap anywhere inside it the two are the same
number, and under a cap they are not.)

| T                                | inferred_bound(T)                           |
|----------------------------------|-----------------------------------------------|
| `bool`                           | 1                                             |
| `i8 / u8`                        | 1                                             |
| `i16 / u16`                      | 2                                             |
| `i32 / u32 / f32`                | 4                                             |
| `i64 / u64 / f64`                | 8                                             |
| `u128 / i128`                    | 16                                            |
| `u256`                           | 32                                            |
| enum with underlying U           | sizeof(U)                                     |
| `Array(T, N)`, T fixed      | N × inferred_bound(T)                       |
| `Array(T, N)`, T variable   | fit(N, N × inferred_bound(T)): the offset table of N slots (§5.2), then the elements [None if T is None] |
| `Bitvector(N)`                   | ⌈N/8⌉ (fixed size)                            |
| `Bitlist(N)`                     | ⌈N/8⌉ + 1   (the bits and the delimiter bit, §5.4.2) |
| `BoundedString(N)`              | N                                             |
| `BoundedList(T, N)`, T fixed    | N × inferred_bound(T)                       |
| `BoundedList(T, N)`, T variable | fit(N, N × inferred_bound(T)): the offset table of N slots (§5.3.2), then the elements [None if T is None] |
| `String`                    | None                                          |
| `Vector(T)`                 | None                                          |
| `Nested(T)` (§5.8)          | None (a member `length_bound` max N: N)       |
| `Optional(T)`               | 1 + inferred_bound(T) [None if T is None]   |
| `Variant(Ts...)`            | 1 + max(inferred_bound(T_i)) [None if any T_i is None] |
| `Tuple(Ts...)`              | that of the extensible record with those members (§5.1.1): fit(1 + v, sum(inferred_bound(T_i))), v = the number of variable elements [None if any element is None] |
| `Pair(A, B)`                | that of `Tuple(A, B)`. (A tuple has at most 255 elements, §8.4.) |
| `Map(K, V)`, `Set(K)`       | that of `Vector(Tuple(K, V))`, `Vector(K)`; bounded to N entries: that of `BoundedList(Tuple(K, V), N)`, `BoundedList(K, N)` (§5.9) |
| boxed T, a member with an alternate presentation | None                     |
| transparent type (§5.10)         | that of its member                            |
| reflected record (DWNC)          | fit(v, sum(inferred_bound(field_i))), v = the number of variable fields |
| reflected record (extensible)    | fit(1 + v, sum(inferred_bound(field_i))): the header and v offset slots |

For a record and a tuple, a fixed field contributes
`fixed_size_of(field)` (which is its `inferred_bound`) and no slot; a
variable field contributes one offset slot — counted in v — and its
payload, `inferred_bound(field)`. If any field is None, the record's
`inferred_bound` is None.

**`fit`: the bound of a type that has offsets.** Such a type consists of
k words that are each W bytes wide (its header, its offset slots, its
offset table) and of other bytes whose total is at most r (inline fixed
fields and the payloads). `fit(k, r)` is the bound for the narrowest W
that can address the whole value:

```
fn fit(k, r) -> Option<n>:
    if r is None:                  return None
    if k * 1 + r <= 0xFF:          return k * 1 + r     // W = 1
    if k * 2 + r <= 0xFFFF:        return k * 2 + r     // W = 2
    return k * 4 + r                                    // W = 4
```

(An arithmetic overflow anywhere in these formulas — in `N × ...`, a
sum, or `k * W + r` — gives None.) The W on the right is the type's own
width: the width class of `fit`'s result is exactly the W of the branch
that returned it, so "W is the width class of the bound" (§3) and "W is
the least width whose worst case fits" are the same statement.

Examples: `BoundedList(BoundedString(16), 8)` is fit(8, 8 × 16) = 136
with W = 1. `BoundedList(BoundedString(16), 15)` is fit(15, 240) = 255,
still W = 1; one more byte per string, `BoundedList(BoundedString(17),
15)`, fails the first test (15 + 255 = 270) and is 15 × 2 + 255 = 285
with W = 2. `BoundedList(BoundedString(255), 255)` is 255 × 2 + 65 025 =
65 535 with W = 2, and with 256 elements it is 256 × 4 + 65 280 =
66 304 with W = 4. A record `{ id: u32, tags: BoundedList(BoundedString(16), 8) }`
is fit(1 + 1, 4 + 136) = 142 with W = 1: its header and its one slot are
1 byte each, and so are the 8 slots of the list inside it.

**The definition is bottom-up and terminates.** `inferred_bound(T)`
is defined by structural recursion on T's type description:

1. *It only looks down.* Every row of the table is a formula over
   constants of T (N, k, fixed sizes) and `inferred_bound` of the
   types T holds — its fields, elements, alternatives, payload. T's own
   W is not an input: `fit` evaluates at most two inequalities whose
   terms (k and r) are already known, and returns a number. There is
   nothing to iterate, no search, and no fixed point.
2. *It ends.* A type description is a finite tree except where a type
   contains itself, which it can only do through a box (`None`, without
   looking inside, §3.2 row "boxed T") or through an unbounded list
   (`None`). A schema-driven implementation that meets, while measuring
   a named type, a reference to that same type treats the reference as
   `None`. So every chain of "the types T holds" is finite.
3. *The chosen W is sufficient.* By induction a field's encoding never
   exceeds what it contributes — its `inferred_bound`, or its explicit
   cap, which encoders and readers enforce (§16) — so a value of T written with W-byte words
   occupies at most k × W + r bytes; `fit` returned that number only if
   it is at most 2^(8W) − 1 (or W = 4). Every offset, and the header, is
   at most the size of the value and therefore fits in W bytes.
4. *It is the narrowest.* k × W + r only grows with W, so the first W
   that passes is the least one, and for every narrower W′ the
   worst-case value needs k × W′ + r > 2^(8W′) − 1 bytes: an offset in
   it could not be written.

**How a bound feeds the enclosing type.** A type holding T uses T's
finished number — `inferred_bound(T)`, which already contains T's own
bookkeeping at T's own W — or T's explicit cap in full when T has one
(§3.1). It adds its own k words at its own W through its own `fit`. The
two widths are independent: narrowing T can narrow what holds T (the
bound gets smaller), never the other way round.

- **Optional.** `Optional(T)` has no offsets of its own; it reserves one
  selector byte, and its payload is T with T's W. A bound written on an
  optional member applies to the payload (§3.5).
- **Nested lists.** `BoundedList(BoundedList(T, M), N)`: the inner list
  takes its W from fit(M, M × max(T)); the outer list is fit(N, N ×
  that) and may have a different W. Each row of a grid uses the inner
  W, the grid's table the outer W.
- **Variant.** One selector byte, then the alternative, which keeps its
  own W. The bound is 1 + the largest alternative.
- **Tuple, pair.** The extensible record of the elements: fit(1 + v, …).
- **Map, set.** The list of the entries / elements (§5.9). A map's
  table has the W of fit(N, N × max(Tuple(K, V))); each entry is a
  tuple with its own header and slots at the tuple's own W.
- **A capped record.** W is the width class of the cap; whatever holds
  the record reserves the cap. The cap also bounds the positions beneath
  the record for the purpose of their widths (§3.6).

A string, a list, a map, a set and a nested value have no bound of
their own: `String` and `Vector(T)` are None. They are bounded by their
type — `BoundedString(N)`, `BoundedList(T, N)` — or, with the type left
as it is, by an annotation on the member that holds them, at any depth
(§3.5). The two say the same thing and give the same number.

`BoundedString(N)` is N: a string's payload has no bookkeeping of its
own, and the slot that locates it is counted by whatever holds it. A
child type that carries an explicit cap contributes that cap in full
(§3.1), not its own inferred bound; an `Optional` reserves one selector
byte whether or not its payload needs one. The result is an upper bound,
not always the exact maximum, and every implementation must compute the
same number: a type's W follows from it.

**Explicit-width profiles.** The profiles `pssz8`, `pssz16` and `pssz32`
(one W for a whole frame, used for byte-parity tests) do not use any of
this: every header, slot and offset table in the frame — a list's table
included — is W bytes wide, whatever is bounded. A value that does not
fit is refused by the encoder. Their bytes are unaffected by how the
automatic profile infers widths.

**Bit types.** `Bitvector(N)` is a fixed-size field of ⌈N/8⌉ bytes: it
is inline in a record and takes no offset slot. `Bitlist(N)` is variable
(one slot) and contributes ⌈N/8⌉ + 1: at most N bits and the delimiter
bit. (The exact largest encoding is ⌊N/8⌋ + 1 bytes, one less when N is
not a multiple of 8; the table's value is the bound every implementation
uses.) A record whose only variable-size members are bit types is
therefore bounded, and narrow, without an explicit cap.

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
| `Array(T, N)` of fixed T    | N × fixed_size_of(T) (0 when N = 0)  |
| `Array(T, N)` of variable T, N ≥ 1 | N × W (the offset table)       |
| `Bitvector(N)`                   | ⌈N/8⌉                                |
| `Bitlist(N)`                     | 1                                    |
| `String`                    | 0                                    |
| `Vector(T)`                 | 0                                    |
| `Nested(T)` (§5.8)          | 0 (whatever min(T) is)               |
| `BoundedString(N)`              | 0                                    |
| `BoundedList(T, N)`             | 0                                    |
| `Optional(T)`, min(T) > 0   | 0 (None is zero bytes)               |
| `Optional(T)`, min(T) = 0   | 1 (the selector byte)                |
| `Variant(...)`              | 1 (the alternative index)            |
| reflected record (DWNC, all fixed) | fixed_size_of(T) (may be 0)        |
| reflected record (DWNC, a variable field) | > 0 (its offset slots)      |
| reflected record (extensible)    | W (the empty header alone)           |

A type with `min_encoded_size(T) > 0` cannot encode to zero bytes,
so an enclosing optional can disambiguate None ↔ Some by looking at
the byte-span length alone (§5.5).

### 3.5 Bounds by annotation

A bound does not have to be in the type. A record member (and a method
parameter) carries annotations that bound the member and any position
below it, with the member's type left as the plain string / list / map
it is:

| annotation        | bounds                                                   |
|-------------------|----------------------------------------------------------|
| `max N`           | the member: bytes of a string, elements of a list, map or set, bytes of a nested value (`exact N` and `min N` too, on the member itself) |
| `utf8`            | the member is a UTF-8 string: `String` instead of raw bytes |
| `element(...)`    | every element of a list, an array or a set               |
| `keys(...)`       | every key of a map                                       |
| `values(...)`     | every mapped value of a map                              |
| `at I (...)`      | element I of a tuple or pair, alternative I of a variant |

An `Optional` is looked through: what is written for an optional member
or position applies to its payload. The position specs nest to any
depth, and below the member itself a length bound is `max N`.

**Order constraints.** Two more annotations constrain the *content* of a
list or array, at the member or at a position below it, without changing
any byte, width or bound:

| annotation | the elements are |
|---|---|
| `sorted` | in non-descending order |
| `unique` (`sorted_unique`) | in strictly ascending order |
| `sorted` descending | the same, descending |
| `unique_keys` | pairwise different by key: the element, or element 0 of a pair / tuple element. No order is required |

Order and equality are those of maps and sets (§5.9.1): bytewise
comparison of the elements' `key` encodings, the same in every language.
A writer refuses a value that violates one and every checking reader
rejects a frame that holds one, in PSSZ, JSON and key alike; a map or set
carries its order by type (§5.9) and needs no annotation. **These two are
not in the Schema IR yet**: a typed C++ reader enforces them, a
schema-driven reader of the same schema does not know them. (C++:
`psio::sorted`, `psio::unique`, `psio::unique_keys`; the other
implementations have no such annotation.)

An annotated member is exactly the type the annotations describe:
`Vector(String)` annotated `max 8, element(utf8, max 16)` **is**
`BoundedList(BoundedString(16), 8)`. It has that type's
`inferred_bound`, that type's widths at every depth (a list, a map, a
set, an array of variable elements, a pair or a tuple at or below the
member takes its W from the bounds its position carries: the annotated
list above has a 1-byte offset table), the
same bytes, the same schema, and the same checks: an encoder refuses a
value outside any bound of the tree, and every reader — validating,
checked and fail-fast — rejects a frame that holds one (§16).

### 3.6 A record's cap bounds what lies beneath it

A record with an explicit cap N (`max-dynamic-data: N`; a method's
argument list with a cap is such a record, §16) never encodes to more
than N bytes, so nothing inside it does either. The cap is therefore an
**implicit bound of every position beneath the record**, and the widths
there use it:

> **Position cap.** The position cap of a value is the explicit cap of
> the nearest enclosing record, if that record has one; otherwise there
> is none. "Nearest enclosing record" is found by walking up from the
> value through lists, arrays, maps, sets, optionals, variants, tuples,
> pairs, boxes and transparent types; the walk stops at the first
> record (a reflected record, or a method argument tuple) and takes
> that record's cap or nothing. It also stops, with nothing, at the
> root and at a `Nested(T)` boundary (the inner value of a nested
> member is a root of its own, §5.8).
>
> **Width under a position cap.** A type that has offsets and is **not
> a record** — an array, list, map or set of variable elements, a tuple,
> a pair — at a position whose cap is C has
>
> ```
> W = width class of min(inferred_bound at that position, C)
> ```
>
> where a `inferred_bound` of None counts as infinite. Without a
> position cap the rule is §3 unchanged.

So in

```
Record Transfer @cap=255 {
    token:  u32
    memo:   String
    tags:   Vector(String)
    rows:   Vector(Vector(String))
}
```

the record has W = 1 (its cap), `tags` has a 1-byte offset table, and so
have `rows` and every row in it, although `Vector(String)` on its own is
unbounded and 4 bytes wide. With `@cap=256` all of them are 2 bytes wide,
and with no cap the lists are 4.

What the rule does and does not touch:

- **It only selects widths.** The position cap is not a `max` in the
  sense of §3.5: it adds no check of its own (the record's cap check,
  §8.2, already covers every byte beneath it), it does not change any
  `inferred_bound` (a capped record contributes its cap to what holds
  it, whatever is inside), and it does not change the schema: the member
  is still `Vector(String)`.
- **A tighter bound of the position's own wins**; a looser one is
  clamped. `tags: BoundedList(BoundedString(16), 8)` under `@cap=60000`
  has a 1-byte table (its own bound, 136); `BoundedList(BoundedString(1000),
  1000)` under `@cap=255` has a 1-byte table too (the cap). A bound
  larger than the cap is legal — it still limits the count and the
  element size where the cap does not — it just cannot widen anything.
- **Strings, byte lists and lists of fixed-size elements have nothing to
  narrow.** Their payload is the raw bytes, with no length prefix and no
  table (§5.3.1, §5.3.3): the only bookkeeping they cost is the one
  offset slot in the record that holds them, which is as wide as that
  record. A `String` member of a record with W = 1 costs exactly one
  byte beyond its characters, whether or not the string is bounded.
- **A record keeps its own width.** The width of a record is part of the
  record type: its bytes are the same wherever it appears (that is what
  lets a view of the record, and a `Nested` value of it, be read without
  knowing what holds it). A record beneath a capped record is not
  narrowed by the outer cap, and the positions beneath the inner record
  take the *inner* record's cap, or none. A child record that should be
  narrow declares its own cap or bounds.
- **Explicit-width profiles** ignore it, like every other inference.

**It is well defined and terminates.** The position cap is an inherited
attribute: it is computed top-down, by one step per level, from a value
that is known before the walk begins (the enclosing record's cap is
written in the schema; it is not derived from anything beneath it). The
bound of §3.2 is a synthesized attribute, computed bottom-up, and it
never reads a position cap: caps only enter the final `min`, after the
bound of the position is finished. The two passes do not feed each
other, so there is no cycle, and each ends because the type tree is
finite (§3.2). The chosen W is sufficient: every offset inside the value
is at most the size of the value, which is at most C (the enclosing
record's encoder and readers enforce C), and at most its own bound; so
it is at most the smaller of the two, which fits in W bytes by the
definition of the width class. And the rule is monotone: a smaller cap
or a smaller bound never gives a larger W.

**From a schema alone.** A schema-driven implementation needs nothing
beyond what the schema already holds: the record's `maxDynamicData`
attribute. While it walks a value it keeps the cap in effect — set to
the record's cap (or to none) on entering a record's members, cleared on
entering the inner value of a `Nested` node, restored on leaving, and
left alone by every other node — and takes the `min` above when it
derives the width of a list, array, map, set or tuple.

### 3.7 max_encoded_size(T): the largest encoding

`inferred_bound(T)` (§3.2) answers "which width?". It is **not**, in
general, the largest encoding of T: it never reads T's cap, and a cap in
a wider class makes T's header and slots wider than the bound counted
them. `max_encoded_size(T)` answers "how many bytes can a value of T
take?" and is an upper bound of every automatic-profile encoding of T.

```
fn max_encoded_size(T) -> Option<n>:
    s = sum_at_real_width(T)
    if T has an explicit cap N:  return min(N, s)      // None counts as infinite: N
    return s
```

`sum_at_real_width(T)` is the table of §3.2 with three changes:

1. **Words at the real width.** Where §3.2 writes `fit(k, r)`, write
   `k × W + r` with W the width T actually has: the width class of its
   cap for a capped record (or capped argument list); otherwise the width
   class of its `inferred_bound`; and for a list, array, map, set, tuple
   or pair beneath a capped record, the width class of
   `min(inferred_bound at that position, C)` (§3.6).
2. **Members at their own maximum.** r sums `max_encoded_size` of the
   fields / elements / alternatives, not `inferred_bound`: a capped
   member counts as min(its cap, its own sum), not as its cap in full.
3. **None.** Any unbounded part gives None, exactly as in §3.2; the
   nearest enclosing capped record turns it into that record's cap.

| | `inferred_bound(T)` | `max_encoded_size(T)` |
|---|---|---|
| reads T's own cap | never | yes: W is the cap's class, and the result is at most the cap |
| a capped member counts as | its cap, in full (room reserved for its growth) | its own `max_encoded_size` |
| a type's words are counted at | the W that `fit` picks from the bound | the W the encoder writes |
| used for | choosing W; the evolution rules of §7 | sizing buffers; "can this value exist?" |
| unbounded member, with a cap N | None | N |
| unbounded member, no cap | None | None |

Properties:

- With no cap anywhere in T the two are equal.
- With no cap **on T itself**, `max_encoded_size(T) ≤ inferred_bound(T)`
  (members count for no more, and W is the one `fit` chose).
- With a cap N on T, `max_encoded_size(T) ≤ N`, and it can be **larger
  than `inferred_bound(T)`** when N is in a wider class.

Examples. `Record @cap=1024 { id: u64, memo: BoundedString(80), note:
BoundedString(16) }` has `inferred_bound` = fit(3, 104) = 107 (W = 1 would
do), W = 2 from the cap, and `max_encoded_size` = 3 × 2 + 104 = 110: the
value with both strings full is 110 bytes. With `@cap=100` it has W = 1
and `max_encoded_size` = min(100, 107) = 100. `Record @cap=300 { text:
String }` has no inferred bound and `max_encoded_size` = 300. A method's
arguments `(u64, u64, u64, memo: BoundedString(80)) @cap=1024` have
`inferred_bound` 106 and `max_encoded_size` 2 × 2 + 104 = 108.

The number keeps the two conservatisms of §3.2 — one selector byte for
every `Optional`, and ⌈N/8⌉ + 1 for a `Bitlist(N)` — so it is an upper
bound, exact for types without those two. It does not affect any byte:
no width is chosen from it. The explicit-width profiles are outside it.

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

An enum is its underlying integer on the wire: the same bytes, size and
alignment as that integer in every position (record field, list element,
optional payload, variant alternative).

Any value in the underlying integer's range is **structurally valid** PSSZ,
whether or not it is a declared enumerator. `validate` accepts it, and a
schema-driven reader (which has only the Schema IR's `Enum` — the integer
plus the enumerator names for text) carries it through unchanged. This is
what lets an old reader keep reading data from a writer that has since
added enumerators.

**The rule, per layer.** The only criterion is that adding an enumerator
stays compatible in both directions: data written with the new enumerator
is still well-formed data for an old reader, and old data is unchanged for
a new one.

| layer | rule |
|---|---|
| wire (PSSZ, key) | the underlying integer; **any** value of that integer is well-formed. No reader of bytes — `validate`, a view, a schema-driven codec — may reject a value because it is not a declared enumerator. |
| Schema IR | `Enum{type, members}`: the integer plus the enumerator names. The names are for text only; they do not restrict the values. |
| text (`docs/json.md` "Enums") | written as the enumerator's **name** when the value is a declared one and as the **number** otherwise; read as the name, the number, the number as a string, or `name:number`. An unknown *name* is an error (there is no value to give it); an undeclared *number* is a value. |
| typed reader | language-specific: whether the reader's **type** can hold an undeclared value is a property of that type, not of the format. A type that cannot hold it must say how it opts in to carry it, and refuses the value otherwise — that refusal is a typed-layer error, never a statement about the bytes. |

The typed layer in each implementation, all of which conform:

- **C++**: an `enum` holds any value of its underlying type, so the typed
  pssz, key and JSON readers accept an undeclared value everywhere, and the
  writers write it back (as its number in text). A type that must reject
  undeclared values says so with an invariant (§18), which makes the
  restriction part of the schema (`Custom{enum, id}`).
- **Rust**: an enum holds only its variants. It opts in with a catch-all
  variant, `#[psio(other)] Unknown(<repr>)`: an undeclared wire value
  decodes into it. A wire value equal to a declared enumerator always
  decodes to the declared variant, and encoding `Unknown(n)` where `n` is a
  declared enumerator is an error, so each value has exactly one in-memory
  representation. Without the catch-all, decoding an undeclared value into
  that type is an error (`enum_out_of_range`) in pssz, key and JSON alike.
- **Python**: `@psio.enum(repr)` refuses an undeclared value by default
  (`DecodeError`, from pssz, key and JSON alike) and carries it as the
  plain integer with `other=True`.
- **JavaScript**: an enum value is its number; any in-range integer is
  accepted and written back.
- **Schema-driven codecs** (every language): the dynamic value is the
  integer; any in-range value is read and written unchanged.

An application that evolves an enum therefore chooses, per reader type,
between carrying unknown enumerators (C++ default, Rust `other`, Python
`other=True`) and refusing them at the typed layer; the bytes and the
schema are the same either way.

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

W is the list's own width (§3.1): 4 for an unbounded list, and the
width class of its bound — 1, 2 or 4 — for a list whose count and
elements are bounded, by its type or by the annotations of its position
(§3.5). It does not depend on the record that holds the list, nor on
the value.

A zero-element list is a zero-byte payload. Otherwise the offset table
size is `count × W` and `offsets[0]` is always equal to that table
size, so `count = offsets[0] / W`.

Offsets are container-relative within the vector payload.

**Indexed access is O(1) at every W.** W is known from the schema before
any byte is read, and all slots of one table have that one width:
element i's offset is the u{W} at byte `i × W` of the payload, and its
end is the next slot (or the end of the payload for the last element).
A reader, a view and an in-place writer (§17) index a 1-, 2- or 4-byte
table the same way.

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
enums, non-empty fixed-size arrays, bitvectors, bitlists, variants,
DWNC records of nonzero size, extensible records and tuples (whose
minimum is the W-byte header), and an optional whose own payload uses
the selector form (it is never empty, e.g. `Optional(Optional(String))`).

#### 5.5.2 1-byte selector form (min(T) == 0)

```
None  → [0x00]
Some  → [0x01, encoding(T)]
```

Used when T can encode to zero bytes — strings, vectors, bounded
lists, bounded strings, zero-size DWNC records, zero-count arrays, and
an optional whose own payload uses the no-selector form (its None is
empty, e.g. `Optional(Optional(u32))`) — because the size-only
disambiguation would conflate `None` with `Some(empty)`. Only selector
values 0 and 1 exist, and nothing follows a `0x00`.

The selector byte is part of the payload, not a separate field. An
optional inside a record contributes a single offset slot pointing
at the payload (which is either `[0x00]` or `[0x01, ...]`).

#### 5.5.3 Why not always selector?

The no-selector form saves 1 byte per Some value across many
fixed-shape fields — significant for record types with several
optional u64 fields where the sum-of-bytes matters for cache
locality.

### 5.6 Variants — `Variant(T_0, T_1, ..., T_{n-1})`

A tagged union with up to 256 alternatives (§8.4).

```
[ u8 selector | encoding(T_selector) ]
```

The selector byte is the alternative index 0..n-1. Decoder rejects
selector ≥ n.

For a variant inside a record field, the entire variant (selector +
payload) lives in the dynamic region; the fixed region holds an
offset slot pointing at the selector byte.

(A variant with more than 256 alternatives is not a PSSZ type, §8.4:
implementations refuse it at compile time or when the schema is
checked.)

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

### 5.8 Nested values — `Nested(T)`

A `Nested(T)` value **is the pssz encoding of a T, carried as a byte
list**. (C++ `psio::nested<T>`, `#include <psio/nested.hpp>`; Schema IR
`Nested{type}`.)

```
[ encoding(T) ]        exactly the payload of Vector(u8) holding those bytes
```

- **Wire.** `encode(Nested(T) holding v) == encode(Vector(u8) holding
  encode(T, v))`. A record with a `Nested(T)` member and the same record
  with a `Vector(u8)` member are byte-identical, so one side of a protocol
  may declare the member typed and the other as bytes (a router forwards
  action arguments it does not interpret; the service that owns them reads
  them typed).
- **Inner profile.** The inner bytes are always T's encoding under the
  automatic profile (§3: every type its own width), whatever profile the
  enclosing value uses.
- **Shape.** Variable-size; `min_encoded_size` 0 (the inner encoding may be
  empty, and the rule does not depend on T); no `inferred_bound` and
  width contribution of its own. `Optional(Nested(T))` therefore always
  uses the selector form (§5.5.2), like `Optional(Vector(u8))`. A member
  `length_bound` on a nested member bounds the **byte count** of the inner
  encoding, exactly as on a `Vector(u8)` member, and enters width selection
  the same way.
- **Validation descends.** Every reader that checks its input validates the
  inner bytes as a T, with the reader's own rule:

  | reader of the enclosing value | inner bytes must be |
  |---|---|
  | compatible (`validate`, `validate_or_throw`, `validate_pssz_compatible`, `decode_pssz_checked`, checked views) | a valid T; unknown appended optional fields inside it are accepted and reported with the enclosing value's |
  | exact (`validate_strict`) | the one canonical encoding of a T: no unknown fields, every type-level rule of T |

  Bytes that are not a T, a fixed-size T followed by a trailing byte, an
  inner record that stores a trailing `None` (§7.2) — all are rejections of
  the enclosing value, with the inner value's error. The trusted
  `psio::decode` copies the bytes without reading them, like every other
  check it leaves to `validate`.
- **Depth** (§8.3). The nested member is a node; the root of the inner value
  is **one level below it**, and the inner value's own levels count on from
  there. The limit of 64 applies to the sum.
- **Deferred validation.** A member may instead be declared *deferred* (C++
  `psio::nested<T, psio::nested_validation::deferred>`). Wire, key, text
  and schema are unchanged, but a reader of the enclosing value treats the
  bytes as opaque in every mode — any bytes are valid — and the holder
  validates explicitly (`member.validate()`) before reading through it. The
  policy is a property of the reading program, not of the data: it is not
  recorded in the Schema IR. A schema-driven reader validates the inner
  bytes by default and takes an option to treat them as opaque
  (`psio::pssz_ir::nested_bytes::opaque`).
- **Encoding does not re-validate.** A nested value holds bytes that were
  produced by the encoder or checked when they were adopted, so encoding an
  enclosing value writes them as they are. Two consequences: bytes adopted
  unchecked (`nested<T>::unchecked`) that are not a T produce a frame every
  reader rejects; and the encoder's depth check covers the nested member
  (which must leave one level for the inner root) but not the levels inside
  the bytes, so a value that only exceeds the depth limit *inside* a nested
  member is written and then rejected by readers.
- **Evolution.** `Nested(A)` → `Nested(B)` is a compatible change exactly
  when A → B is (§7); `Nested(T)` and `Vector(u8)` are different types to
  the schema-upgrade check even though their bytes agree. A typed reader
  keeps the member's bytes: decoding a frame whose inner value carries
  fields it does not know and re-encoding it reproduces the frame, unknown
  inner fields included. A schema-driven reader, whose dynamic value is the
  inner value, re-encodes the canonical encoding of what it read (the
  unknown fields are dropped, as for any typed read) unless it was asked to
  treat nested bytes as opaque.
- **Key.** The key of the byte list (`docs/key-spec.md`): nested values
  order by their inner **bytes**.
- **Text.** JSON and I-JSON carry the inner value's own text, not hex
  (`docs/json.md`, "Nested values").

**Text round trips are value round trips.** Text holds the inner *value*;
reading it stores the canonical encoding of that value. Fields that a newer
writer appended to the inner record, and that this reader's T does not
know, are therefore not carried through text. A holder that must preserve
foreign bytes exactly — a relay, a store of signed payloads, anything that
hashes the bytes — declares a byte list (`Vector(u8)`: hex in I-JSON)
instead of `Nested(T)`, and decodes the bytes where it needs the value.

C++ surface (`psio/nested.hpp`):

```cpp
struct Action {
   AccountNumber              sender;
   MethodNumber               method;
   psio::nested<TransferArgs> args;
   PSIO_REFLECT(Action, sender, method, args)
};

auto n = psio::nested<TransferArgs>::from(args);          // encode (fail-fast)
psio::nested<TransferArgs>::from_checked(args, n);        // encode -> codec_status
psio::nested<TransferArgs>::checked(bytes, n);            // adopt after validate -> codec_status
auto u = psio::nested<TransferArgs>::unchecked(std::move(bytes));  // adopt, caller vouches
n.bytes(); n.size(); std::move(n).release();              // the encoding
n.view();                                                 // psio::view<TransferArgs, psio::pssz>
n.decode();                                               // TransferArgs (trusted)
n.decode_checked(out, &has_unknown);                      // validate, then decode
n.validate(); n.validate(has_unknown); n.validate_strict();
psio::view<Action, psio::pssz>{frame}.get<2>().get();     // view<TransferArgs> over the inner bytes
```

A default-constructed `nested<T>` is the encoding of `T{}`; `==` compares
bytes. The other C++ formats (bin, ssz, frac, ...) have no nested value: it
is an unsupported type there.
### 5.9 Pairs, maps and sets — `Pair(A, B)`, `Map(K, V)`, `Set(K)`

These add no wire shape. A pair is a tuple, and a map or a set is a list
with one constraint on its content.

**Pair.** `Pair(A, B)` is `Tuple(A, B)`: the extensible positional record of
two members (§5.1.1), with the tuple's header, slots, trailing-optional
pruning, width and evolution rules. C++ `std::pair<A, B>`; the entries of a
map are pairs.

**Map and set.** `Map(K, V)` is `Vector(Tuple(K, V))` and `Set(K)` is
`Vector(K)`, with the entries in **strictly ascending key order**:

```
encode(Map(K, V)) = encode(Vector(Tuple(K, V)))   of the entries, sorted by key
encode(Set(K))    = encode(Vector(K))             of the elements, sorted
```

Everything that is derived from the encoding is the list's: the offset width
(§3: 4 for an unbounded map or set; a bounded one — entries counted by a
member bound, keys and values bounded — takes the width class of its bound,
as a bounded list does, and each entry tuple its own), `inferred_bound`
(None unless a member bound makes it finite, as for a vector),
`min_encoded_size` = 0 (so `Optional(Map)` and `Optional(Set)` take the
selector form, §5.5.2), the element rules of §5.3 (fixed elements are packed
with no table, a zero-size element type cannot be counted), node depth (an
entry is one node below the map, its key and value one below the entry), and
views (a view of a map is the view of the list of its entries). A member
`length_bound` counts entries.

#### 5.9.1 The order

One rule, the same in every language and for every key type:

> Entries are ordered by the bytewise (unsigned, lexicographic) order of the
> **`key` encoding of K** (`docs/key-spec.md`). Two entries whose keys have
> equal key bytes are duplicates; a map or set holds at most one of them.

Why this order and not the language's own:

- It is the one total order PSIO already defines for every key type in every
  language. JavaScript has no `<` for composite values, a C++ record's `<` is
  user code, Rust `Ord` derives differ from both.
- It needs nothing but the schema: a tool that knows the type only through
  its Schema IR can check and produce it.
- For the usual key types it is the order a sorted container already has, so
  such a container iterates in wire order:

| K | order |
|---|---|
| unsigned / signed integers, enums, time values | numeric (not the order of the little-endian wire bytes: `2` sorts before `513`) |
| `bool` | `false`, `true` |
| strings, byte lists | bytewise; a prefix sorts first |
| tuples, pairs, records | lexicographic by element / by member in declaration order |
| optional | none first |
| lists, arrays, variants | lexicographic by element; by alternative index, then value |
| floats | key-spec §4: `-0` and `+0` are one key (they cannot both be in a set); NaNs are ordered by their bits, sign-bit-set NaNs below `-inf`, the others above `+inf` |
| nested values (§5.8) | bytewise by the **inner encoding as stored** — not the order of the inner values (a `Nested(u32)` holding 256, bytes `00 01 00 00`, sorts before the one holding 1), and two encodings that differ only in fields the reader does not know are two keys |

A nested value is a key type whatever its inner type is (its key is that of
a byte list); as K it is checked like any nested value — each key's bytes
are validated as the inner type — and ordered by those bytes as they are.
(C++ `psio::nested<T>` has no `<` and no hash: a `std::map` / `std::set`
keyed by it names a comparator, which the codec does not rely on.)

K must have a key encoding (`bitlist` has none), and the order must be
visible in the schema: a C++ type that supplies its own key bytes through a
`sortable_binary_category` adapter cannot be K. Both are compile errors in
C++ ("the key type of a map ... must have a key encoding") and malformed
schemas for the schema-driven codecs.

#### 5.9.2 Writers and readers

- **Encoders** write the canonical order whatever order the container
  iterates in (a hash container, a custom comparator), and refuse a value
  holding two entries with equal key bytes: `encode_pssz_checked` returns
  `pssz: map has duplicate keys` / `pssz: set has duplicate elements`, the
  plain `encode` fails fast as for any value without an encoding.
- **Every reader that checks its input** — `validate`, `validate_or_throw`,
  `validate_strict`, `validate_pssz_compatible`, `decode_pssz_checked`,
  validated views, and the schema-driven codecs — validates the list and
  then requires the order: `pssz: map entries are not in strictly ascending
  key order` / `pssz: set elements are not in strictly ascending order`. A
  repeated key is the same failure. The order holds at every level: a map
  inside a record, an optional, a list, a variant, a map.
- **The trusted `decode`** (validated input) inserts the entries into the
  container. If the container ends with fewer entries than the list had —
  its comparator or equality merges keys the encoding distinguishes — that
  is a decode failure, never silent loss.

Language types:

| | maps | sets | not values |
|---|---|---|---|
| C++ | `std::map`, `std::unordered_map` (any comparator / hash / allocator) | `std::set`, `std::unordered_set` | `std::multimap`, `std::multiset` and their unordered forms: duplicates have no canonical form (compile error) |
| Rust | `BTreeMap`, `HashMap` | `BTreeSet`, `HashSet` | |
| JS | `map(K, V)`: decodes to a `Map` in canonical order, encodes any iterable of entries | `set(K)`: a `Set` / any iterable | |

The container is not part of the type: `std::map<K, V>` and
`std::unordered_map<K, V>` are one value kind with one encoding. No
implementation may rely on the container's own order (`Ord`, `operator<`, JS
identity) unless it is known to coincide with key order; C++ walks a
`std::map` / `std::set` with the default comparator in place when K is made
of integers, enums, `std::string`, time values and arrays / vectors /
optionals / tuples / pairs / variants of those, and sorts by key bytes
otherwise. That is an optimisation: the bytes are the same.

#### 5.9.3 Encode example

`Map(String, u32)` holding `{"b": 2, "a": 1}`. The list and the entry tuples
are unbounded: W = 4 throughout.

```
Offset  Bytes              Meaning
--------------------------------------------
00      08 00 00 00         list offset[0] = 8   (2 entries × W)
04      15 00 00 00         list offset[1] = 21
08      08 00 00 00         entry ("a", 1): tuple fixed_size = 8
0C      08 00 00 00           key offset = 8 (relative to byte 0C)
10      01 00 00 00           value = 1
14      61                    "a"
15      08 00 00 00         entry ("b", 2): tuple fixed_size = 8
19      08 00 00 00           key offset = 8
1D      02 00 00 00           value = 2
21      62                    "b"
```

34 bytes, and exactly `encode(Vector(Tuple(String, u32)))` of
`[("a", 1), ("b", 2)]`. The frame with the two entries exchanged is a valid
`Vector(Tuple(String, u32))` and not a valid `Map(String, u32)`.

`Set(u16)` holding `{513, 2}` is `02 00 01 02`: `2` then `513`, by key
(`00 02` < `02 01`), although the wire bytes of 513 (`01 02`) sort below
those of 2 (`02 00`).

#### 5.9.4 Evolution

`Map(K, V)` → `Map(K, V')` is a compatible change exactly when `V` → `V'`
is (the entry tuple evolves like any tuple). The key type cannot change: a
different key type has a different order. Bytes written as a map are always
readable as the list of entries; the reverse holds only for sorted lists, so
`pssz_evolves_from` does not relate a map to its list type. A pair and the
2-tuple of the same elements are one type for this check.
### 5.10 Transparent types

A **transparent type** is a named type with exactly one member that *is*
that member on the wire. Nothing of the wrapper is encoded: no header, no
offset slot, no selector.

```
encoding(Wrapper{member}) == encoding(member)
```

Everything the format derives from a type is the member's:

| property | value for a transparent type `W` over `M` |
|---|---|
| fixed / variable, `fixed_size_of` | those of `M` |
| `min_encoded_size`, so the optional form of `Optional(W)` (§5.5) | that of `M`: no selector iff `min(M) > 0` |
| `inferred_bound`, a declared `max-dynamic-data` budget, the width W | those of `M` |
| element layout in `Vector(W)` / `Array(W, N)` | that of `M` (a list of fixed wrappers is the contiguous list of members) |
| node depth (§8.3) | the wrapper adds none |
| validation | that of `M`, then the wrapper's invariant if it has one (§18) |
| view | the member's view |

A transparent type is therefore **not a record**: a frame that encodes it
as a one-field record (a header and the member) is a different, and for the
wrapper invalid, encoding. It has no `definitionWillNotChange`, no
`max-fields` and no `max-dynamic-data` of its own, and its member is not an
`Optional` (presence belongs to the record that holds the value: hold an
`Optional(Wrapper)`). A record member of a transparent type takes the
annotations that reach a value through any type (the integer text form); a
length bound on it is an error, as on any member that is not itself a
string or a list — bound the wrapper's own member instead.

**A bounded member.** The single member of a transparent type may carry the
annotations of §3.5 that make a type a bounded type: `utf8` on a string,
`max N` on a string, list, map or set, and `element` / `keys` / `values` /
`at I` below it. The wrapper then **is** that bounded type, everywhere it
appears — bare, a list element, an optional payload, a map key or value:

```
Memo = transparent { text: String  @utf8 @max(80) }      // is BoundedString(80)
```

It has the bytes, the `inferred_bound` (80), the widths, the schema node
(`types: [{name: "Memo", type: -> BoundedString(80)}]`) and the checks of
`BoundedString(80)`: every writer refuses a `Memo` over 80 bytes or not
UTF-8 and every reader rejects its bytes, in PSSZ, key and text alike. This
is one rule for every language (C++ `PSIO_REFLECT(Memo, transparent(),
attr(text, utf8 | max<80>))`; Rust `#[psio(transparent)] struct
Memo(#[psio(max = 80)] String)`; JavaScript `transparent('Memo',
boundedUtf8(80))`; Python `Annotated[str, max(80)]` in a
`@psio.transparent` class). An annotation that a type cannot say is not
allowed there: `exact N` / `min N` (a member attribute in the Schema IR,
and a transparent type has no member in it), `sorted` / `unique`, an
integer text form, a text presentation. Such a constraint is the type's
invariant (§18).

What the wrapper keeps is its **identity**: its name, a text form of its own
(`docs/json.md`), an invariant (§18) and its Schema IR node — a named alias
of the member's type, `types: [{name: "W", type: <M>}]`, or
`Custom{<M>, id}` when it has a text form or an invariant. Schema evolution
(§7) treats it nominally: a transparent type evolves only from a transparent
type of the same name whose member evolves; it does not evolve from or into
its member's type.

C++: `PSIO_REFLECT(W, transparent(), member)` or
`PSIO_TYPE_ATTRS(W, psio::transparent)`; `psio::is_transparent_v<W>`,
`psio::transparent_inner_t<W>` (`psio/transparent.hpp`). Rust:
`#[pssz(transparent)]`.

The formats outside the core set see what is declared, a struct of one
member: `bin`, `borsh` and `bincode` encode that as the member anyway;
`ssz`, `frac` and the foreign-schema formats encode the struct.

## 6. Document — top-level

### 6.1 Buffer interpretation

A pssz buffer is a sequence of bytes whose interpretation requires:

1. The schema (the type description T at the root).
2. Both encoder and decoder agree on the width tag W derived from T
   per §3.

Given those, encoding is deterministic: pruning is mandatory (§7.2),
so each value has exactly one encoding.

A buffer contains exactly one value and **no trailing data**. Decoding
or validating a fixed-size type (including a bare scalar) from a buffer
longer or shorter than the value is an error; variable-size types own
the whole buffer by definition.

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

Pruning is **mandatory**: a value has exactly one encoding.

Any field added after a type is first published must be `Optional(T)`.
It therefore always owns an offset slot, and the only field state that
can be omitted is an absent optional. When the trailing K fields of an
extensible record are all absent optionals, the encoder MUST drop them:

- Skip writing their offset slots.
- Write the smaller `fixed_size` header reflecting only the unpruned
  prefix.

Readers MUST honor the header's fixed-region size rather than assuming
their own schema's, and MUST reject a record whose last written slot is
an absent optional (it should have been pruned). A reader cannot make
that judgement when the writer's last slot is one the reader does not
know; it then accepts the frame and can report that unknown fields were
present. A present-but-empty value (`Some("")`, `Some([])`,
`Some(None)`) is a value, not a default, and is never pruned. A header
that is smaller than the required (non-optional) prefix, that cuts a
field, that exceeds the frame, or whose unknown suffix is not a whole
number of W-byte slots is invalid.

DWNC records carry no header: they are never pruned and never extended.

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

✓ Append a new **optional** field at the end. (A non-optional appended
  field has no way to be absent: old data is rejected by the new reader.
  The wire carries no schema, so this rule is enforced by schema-upgrade
  checks, not by readers. In C++:
  `static_assert(psio::pssz_evolves_from<Old, New>)`, which checks
  every rule of this section and the next: an unchanged width at every
  depth, DWNC status, member annotations and caps.)
✓ **Widen** a length bound — raise a `max`, lower or drop a `min`, replace
  `exact N` by a range that contains N, drop the bound — at the member or
  at any position below it (§3.5), provided no offset width changes
  anywhere (the member's own list / map / tuple, and the record). The new
  reader then accepts every value the old writer produced. An old reader
  rejects a new value that uses the added room, exactly as it would reject
  any value outside its type: widen a bound only once every reader that
  must read the new values has the new schema. (If a W changes, the wire
  format is incompatible.)
✓ Raise a record's `max-dynamic-data` within the same width class, or
  declare one that is at least the bound inferred for the old type.
✓ Add a new alternative to a variant (provided the new alt is at the
  end of the variant's alternative list).
✓ Evolve the inner type of a `Nested(T)` member by these same rules
  (§5.8). The member itself stays a byte list.

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
✗ Change the key type of a map or the element type of a set, or turn a
  list into a map / set of the same entries (§5.9.4).
✗ **Tighten** a length bound — lower a `max`, raise or add a `min`, add a
  bound where there was none, turn a range into `exact N` — at any depth.
  The bytes of an old value may be unchanged, but the new reader rejects
  every old record outside the new bound (and a narrower W would change
  the bytes).
✗ Change `utf8` at any position, in either direction: a raw string is any
  bytes and a UTF-8 string is text; a reader of one is not a reader of the
  other, although the bytes of a valid text are the same.
✗ Change `sorted` / `unique` / `unique_keys` on a list member.
✗ Lower a record's `max-dynamic-data`, or move it to another width class.
✗ Give a record more fields than its `max-fields`, raise `max-fields`, or
  drop it (§16): the cap is a promise about every later revision.
✗ Add the `dwnc` flag to a previously-extensible record.
  (DWNC removes the header, so old decoders reading a header where
  there isn't one will misinterpret bytes.)

## 8. Limits

### 8.1 Width-imposed limits

For a container with width tag W:

- W = 1: total encoded size ≤ 255 bytes.
- W = 2: total encoded size ≤ 65 535 bytes.
- W = 4: total encoded size < 4 GiB (at most 4 294 967 295 bytes — the
  practical limit for an in-memory single-buffer parse).

These are the limits of the offsets a W-byte slot can hold. In the
automatic profile a type's W is chosen so that its largest value meets
them (§3.2); an encoder never widens W for a large value, it refuses the
value.

### 8.2 Cap-imposed limits

If T carries a `max-dynamic-data: N` annotation, the encoded total must
satisfy `total ≤ N`. Encoders that exceed N MUST throw / return an
error rather than silently widen to a larger W. Decoders MAY enforce
the cap by rejecting encoded buffers with `bytes.size() > N`.

### 8.3 Recursion / depth

`validate()` MUST cap structural recursion at **64 levels of nested
containers** (a hard requirement). Validation is the untrusted-input
path: a malicious buffer can claim arbitrarily deep nesting, and
without a hard cap a recursive walker exhausts the C stack long
before the buffer is consumed. Implementations enforce the cap by
threading a `depth` counter through the walker and returning a
`max_depth_exceeded` error (one of the §9 categories) when the
counter exceeds 64 at any container/record entry.

The bytes of a `Nested(T)` value (§5.8) are part of the same walk: the
inner value's root is one level below the nested member, and the cap
applies across the boundary.

Decoders are NOT bound by the cap — they MAY accept deeper trust
when fed pre-validated input, since a successful `validate()` is a
sufficient precondition for a sound decode. The split lets normal
production paths skip per-recursion depth checks at decode time
while keeping the validator safe against adversarial input.

The reference psio implementation exposes the cap as a single
`inline constexpr std::size_t psio::kMaxValidationDepth = 64`
(in `psio/detail/validate_depth.hpp`); every format's validator
checks against that constant so the value is grep-able from one
place.

### 8.4 Element counts

| what | limit | why |
|---|---|---|
| elements of a `Tuple` (a `Pair` is 2; a method's argument list is a tuple) | **255** | a count that fits in 8 bits, and what language tooling can reasonably instantiate |
| alternatives of a `Variant` | **256** | the selector is one byte: indexes 0..255 |
| fields of a record | **255** | a record, a tuple and a method's argument list are one positional shape (§5.1); they share one limit. `max-fields: N` (§16) lowers it for one type |
| parameters of a function (an interface's `Func`) | **255** | its argument list is a tuple |

A tuple of more than 255 elements, and a record of more than 255
fields, is not a PSSZ type: a typed implementation refuses it when the
type is built (C++: a compile-time error), and a schema that holds a
`Tuple` or an `Object` / `Struct` of more than 255 members, or a function
of more than 255 parameters, is invalid — a schema-driven implementation
rejects it before reading or writing any value (C++:
`psio::pssz_ir::validate_schema` for the functions of a whole schema). A variant with more than 256 alternatives is refused
the same way; split it or use a different encoding.

The limit does not change any byte. A tuple's header is the byte size
of its fixed region (§5.1.1), not its element count, and a fixed region
of 255 members can be far larger than 255 bytes; nothing on the wire
counts tuple elements, record fields or list elements.

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
- **utf8_invalid** — a UTF-8 string (`String`: schema id `utf8`) contains
  bytes not forming valid UTF-8. Every reader and the encoder report it. A
  raw string (schema id `string`) is any bytes and is never checked, in
  strict validation either: strict means exact-schema and canonical.
- **cap_exceeded** — buffer size exceeds the type's
  `max-dynamic-data: N` cap.
- **entries_not_ascending** — the entries of a map or the elements of a
  set are not in strictly ascending key order: out of order, or a key
  twice (§5.9).

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
- Nested values (§5.8): byte equality with the byte-list form, inner
  bytes that are not the inner type, unknown fields inside the inner
  bytes, deferred validation, depth across the nesting boundary.

## 15. Canonical Encoding

A pssz encoding is **canonical** when:

1. Trailing pruning is applied to its maximum extent: every trailing
   absent optional field is dropped from the encoding (§7.2). This is
   mandatory, so every valid encoding is canonical in this respect.
2. Variable-field offsets in the fixed region are in field-declaration
   order. (Implementations are technically free to permute payloads
   if they emit consistent offsets, but canonical pssz requires
   declaration order.)
3. No unused / "padding" bytes between payloads in the dynamic
   region.
4. The width tag W is the minimum width permitted by the schema's
   `effective_max_dynamic(T)`. (W = 1 if the schema's effective
   bound is ≤ 0xff, etc. — see §3.)
5. The entries of a map and the elements of a set are in strictly
   ascending key order (§5.9). Like pruning this is mandatory: every
   reader rejects any other order, so a map or set value has exactly one
   encoding.

A canonical encoder always produces a canonical encoding. A
non-canonical decoder accepts both canonical and non-canonical inputs
(as long as the non-canonical input is well-formed).

For applications that hash pssz bytes (signatures, content-addressed
storage), use canonical encoding to ensure equal values produce equal
hashes regardless of which encoder produced them.

## 16. Type-level caps

pssz honors the type-level caps documented in `pjson-spec.md` §16:

- **`max-fields: N`** — a cap on the **number of fields** of a record, or
  of parameters of a method's argument list: the type has, and will ever
  have, at most N. It is not a size: `max-dynamic-data` caps bytes. N is
  at most 255 (§8.4). It is enforced where the field count is known:
  - *the declaration*: a record with more than N fields, a method with
    more than N parameters, or N > 255 is an error (C++: at compile time);
  - *the schema*: an `Object` / `Struct` with more members than its
    `maxFields` attribute, a function with more parameters than its, or a
    `maxFields` that is not a number in 0..255, is a malformed schema that
    every schema-driven reader rejects;
  - *evolution* (§7): a revision may not have more fields than the
    original's `max-fields`, and may not raise or drop it (lowering it to
    no less than the current count is allowed). C++:
    `pssz_evolves_from`; a schema-based upgrade check must apply the same
    rule.
  Nothing on the wire counts fields, so it adds no check of a value.
- **`max-dynamic-data: N`** — drives both width selection (§3) and an
  explicit ceiling at encode + decode. Encoders that produce > N
  bytes throw; validators that see > N bytes in the input reject.

The interplay between explicit caps and per-field length bounds is
that of §3.1: **an explicit cap always rules**. When T carries
`max-dynamic-data: N`, `effective_max_dynamic(T) = N` and W follows
from N alone — also when the bound inferred from the current fields is
smaller (the cap is the type's lifetime width promise: it reserves room
for optional fields appended later, and must not shrink to what the
fields need today) and when it is larger or absent. Only a type with no
explicit cap uses the inferred bound `inferred_bound(T)`. There is no
`min(explicit_cap, inferred_bound)`.

The two are enforced independently. The cap bounds the encoded size of
the record (above). A member `length_bound` (`max` / `exact` / `min`
on a string, list, map, set or nested member, §3.2) bounds that
member, and the annotations written for the positions below it —
its elements, keys, values, tuple elements, at any depth (§3.5) —
bound those: an encoder refuses a value outside any of them, and every
reader — validating, checked and fail-fast — rejects a frame that holds
one. A value can satisfy every bound and still exceed the cap, or the
reverse; either is an error.

**Method arguments.** The arguments of a method are the tuple of its
parameters (§3.2 `Tuple`). A method may declare annotations for its
parameters, which bound the tuple exactly as member annotations bound a
record, and a `max-dynamic-data: N` for the tuple itself: the cap of the
encoded arguments and, as for a record, what fixes the tuple's W for
the method's lifetime. A method that may gain optional trailing
arguments (§7) declares the cap from the start; without one, an added
argument that pushes the inferred bound into the next width class
changes W and is not a compatible change.

When a record is DWNC + all fields are fixed-shape +
`max-dynamic-data: N` is small enough to drop W to 1 and the value's
actual size fits the cap, the encoded record is exactly
`fixed_size_of(T)` bytes — no header, no offsets, no overhead. This
is the minimum-size case for pssz.

## 17. In-place mutation

A pssz buffer can be modified without decoding it, but only in one way:
**a fixed-size value may be overwritten by another value of the same
type.** Everything else requires decode → modify → encode. This
section gives the rule, the reason, and the reference surface
(`psio::mutable_view<T, Fmt>`, `cpp/include/psio/mutable_view.hpp`).

### 17.1 Why most mutations are impossible in place

Three properties of the format decide it:

- **One encoding per value.** Trailing absent optionals are pruned and
  the record header records the pruned size (§7.2); every valid frame
  is canonical in this respect (§15).
- **Sizes are derived, never stored.** A variable-size value ends where
  the next offset, or its container, says it ends (§5, §6.2). There is
  no length field to adjust and no slack to grow into.
- **No padding.** Payloads are contiguous and the frame ends exactly at
  the last payload byte (§15 item 3, §6.2).

So a mutation that changes the **size** of a value (a string, a vector,
a bitlist, a variant switching alternative, a nested extensible record
gaining a field) moves every later byte of every enclosing container
and changes every later offset in each of them — and may no longer fit
the type-derived offset width (§3). A mutation that changes the
**presence** of an optional changes a size as well (None is zero bytes
or one selector byte) and, for a trailing member, the pruning decision
and therefore the header of the enclosing record. Neither is an
in-place operation; both are a re-encode.

### 17.2 Why a fixed-size overwrite is sound

Call a value *fixed-size* when `fixed_size_of(T)` is defined (§3.3):
Bool, integers, floats, enums, durations and time points, 128- and
256-bit integers, `Bitvector(N)`, `Array(T, N)` of fixed-size T, and a
DWNC record whose members are all fixed-size. Its encoding is
`fixed_size_of(T)` bytes for every value, with no header, offset,
selector or count inside it.

In a valid frame the bytes are partitioned: each byte is either
*structural* (a record header, an offset slot, an optional or variant
selector, a bitlist delimiter) or belongs to exactly one scalar leaf.
Offsets are monotonic and spans are exact, so no two values overlap.
Every structural decision a reader makes — where a member starts and
ends, how many elements a vector has, whether an optional is present,
which alternative a variant holds, how many trailing members were
pruned, whether unknown members follow — is a function of the type,
the frame length and the structural bytes only. The bytes of a
fixed-size value are never structural. In particular:

- the count of a `Vector` of fixed-size elements is its span divided
  by the element size, and a selector-less optional is present when
  its span is non-empty (§5.5.1): both are lengths, not contents;
- pruning concerns absent optionals only. `Some(0)` is a value, not a
  default (§7.2), so no content written into a present optional can
  make it prunable;
- offset width, caps and nesting depth are properties of the type.

Therefore, if the bytes of a fixed-size value are replaced by the
encoding of another value `v'` of the same type, every reader computes
the same structure as before and finds `v'` at that position. The
result is byte-for-byte the canonical encoding of the original value
with that one part replaced: exactly what decode → modify → encode
would have produced.

One condition remains: the written bytes must themselves be a valid
encoding. Two fixed-size types constrain their bytes — a Bool is
written as `00` or `01` (§4.1) and the unused high bits of a
`Bitvector(N)` are zero (§5.4.1). A writer MUST produce the encoding of a value (never copy
arbitrary bytes into the span) and MUST refuse a value that has none.

#### 17.2.1 Maps, sets and nested values

Three kinds of value carry a constraint on their **content** in addition
to their structure, so the argument above has to be made again for each:

- **`Map(K, V)` — mapped values only.** The constraint is the order of
  the entries, and the order is a function of the keys' bytes alone
  (§5.9.1). The bytes of a mapped value take no part in it, and an entry
  is a tuple, so a fixed-size leaf of a mapped value is as writable as a
  leaf of a tuple member. The bytes of a **key** are the constraint
  itself: a key is read-only, whatever its type.
- **`Set(K)` — nothing.** Every byte of every element decides the order.
  A set has no mutable position.
- **`Nested(T)` — the inner value.** The constraint is that the bytes
  are the canonical automatic-profile encoding of a T (§5.8). They are a
  frame of their own, so §17.2 applies to them directly: after a
  fixed-size overwrite inside, they are the canonical encoding of the
  modified T, and their count — the only thing the enclosing frame
  derives from them — is unchanged. The exception is a nested value
  whose validation was deferred by its reader (C++ `nested<T,
  nested_validation::deferred>`): no reader of the frame has established
  that its bytes are a T, so it has no mutable position.

A write that changes the bytes of a nested value changes that value's
`key` encoding (`docs/key-spec.md`). Inside a pssz frame this matters
only where an order is derived from it — a map key or a set element —
and those positions are not writable.

### 17.3 The permitted operation

`set(v)` on a position holding a fixed-size value of type T, where the
position is any that exists in the frame:

| Position | Exists when |
|----------|-------------|
| Fixed-size member of a DWNC record | always |
| Fixed-size member of an extensible record | always: only optional members can be pruned, and an optional is never fixed-size |
| Element `i` of `Array(T, N)` | `i < N` |
| Element `i` of `Vector(T)` | `i < count` |
| Payload of `Optional(T)` | the optional is present (Some → Some only) |
| Payload of a variant's active alternative | that alternative is fixed-size |
| Member of a `Tuple` or `Pair` | as for an extensible record |
| Mapped value of entry `i` of `Map(K, V)` | `i < count` |
| Inner value of `Nested(T)` (validated as T) | always; the inner frame is navigated under the automatic profile |
| Any of the above reached through any chain of members, elements, present optionals, active alternatives, mapped values and nested values | every step exists |

Navigation may pass through variable-size values (it only reads
offsets); only the value written must be fixed-size. A fixed-size
record or array may be overwritten as a whole or part by part.

A frame written by a newer schema (unknown trailing optional members,
§7.3) can be mutated through the older type: the unknown members are
not touched and the frame remains the writer's canonical encoding.

### 17.4 What an implementation must reject

The reference implementation rejects each of these at compile time:

- `set` on any variable-size type: String and bounded strings, Vector,
  Bitlist, Optional (presence), Variant (alternative), an extensible
  record, a DWNC record with a variable-size member, an
  adapter-encoded value;
- resizing of any kind — there is no such operation;
- any write beneath a value whose **content is constrained beyond its
  own encoding**: a type with an invariant (§18), or a record member
  whose annotations constrain it (`sorted_spec`, `unique_keys_spec`,
  `element_spec`, any spec with a validator). A leaf write could
  falsify the constraint without changing a single structural byte, so
  such a type has no mutable view and such a member cannot be
  navigated into; `set` of a whole fixed-size value that contains one
  is rejected as well. A type that declares an invariant is blocked by
  that declaration — wherever it sits: a record member, a list element,
  a mapped value, the value inside nested bytes, a transparent wrapper
  with an invariant. (A transparent wrapper without one is its member:
  navigation and `set` are the member's.) The extension point for any
  other constraint is `psio::detail::mutation_blocked_v<T>`, specialized
  to `true` for a type or an annotation spec;
- any write to the **key** of a map entry or to an element of a
  **set**, and navigation into a set at all (§17.2.1): a set has no
  mutable view, and an entry of a mutable map exposes its key for
  reading and only its mapped value for writing;
- navigation into a nested value whose validation is deferred, and
  `set` of a whole nested value (it is variable-size);
- navigation into a member with an `as<Tag>` override: its bytes are
  the adapter's, not the member type's;
- any format other than pssz. The argument above is specific to this
  format.

At run time `set` returns `false` and writes nothing when the position
does not exist (payload of an absent optional, element index out of
range) or the value has no valid encoding (a `Bitvector(N)` with unused
bits set, a null `Box`). It never allocates and never throws.

### 17.5 Preconditions

A mutable view, like the read view, assumes a frame that has been
validated for T (§10) and does not re-validate; `set` on a frame that
was never valid is bounded to the caller's bytes but otherwise
meaningless. After any sequence of successful or refused `set` calls on
a valid frame, the frame passes `validate` and `validate_strict`, and
equals the encoding of the value it decodes to.

```cpp
std::vector<char> bytes = psio::encode(psio::pssz{}, account);
psio::mutable_view<Account, psio::pssz> mv{bytes};   // or span<char>, psio::buffer
bool ok = mv.get<1>().set(std::uint64_t{42});        // member 1, a fixed-size u64
ok = mv.get<3>()[2].get<0>().set(true);              // member 0 of element 2 of member 3
ok = (*mv.get<5>()).set(7);                          // payload of a present optional
ok = mv.get<6>()[0].value().set(9);                  // mapped value of entry 0 of a map
auto name = mv.get<6>()[0].key();                    // its key: a read view, no set
ok = mv.get<7>().get().get<1>().set(5);              // member 1 of the value inside a nested<T>
```

## 18. Type invariants

A type may declare one **invariant**: a predicate over a decoded value that
says whether the value is a value of the type at all — an account name in
canonical form, a key that is a DER sequence, `lo <= hi`. It is part of the
type, not of a call: every reader that checks its input and every writer
enforces it, so a value that violates its invariant has no encoding and
bytes that decode to such a value are rejected.

```cpp
struct AccountNumber {
   std::uint64_t value = 0;
   static psio::codec_status check(const AccountNumber& a) noexcept;
   PSIO_REFLECT(AccountNumber, definitionWillNotChange(), invariant(check), value)
};
```

C++ (`psio/invariant.hpp`): `invariant(fn)` in `PSIO_REFLECT`, or
`PSIO_INVARIANT(Type, fn[, "SchemaId"])` at global scope for a type that is
not a reflected record (an enum, a `std::variant`, a `std::tuple`), with
`fn(const T&) noexcept -> psio::codec_status`. Queries:
`psio::has_invariant_v<T>`, `psio::check_invariant(value)`,
`psio::contains_invariant_v<T>` (T, or any type it holds, has one) and
`psio::any_has_invariant_v<Ts...>` (one of a path of enclosing types has
one).

Rules:

1. **One per type.** Declaring a second is an error. Arithmetic types take
   none. An invariant registered for a `std::variant` or `std::tuple`
   applies to that exact type wherever it is used; a distinct identity is a
   transparent wrapper (§5.10).
2. **After structure.** The check runs on a value only after the structural
   validation of exactly its bytes succeeded: it never sees a malformed
   value, and a structural error is reported as such.
3. **Innermost first.** Nested values are checked where they occur — record
   members, list and array elements, optional and variant payloads, tuple
   and pair elements, the keys and mapped values of a map and the elements
   of a set (§5.9), the value inside nested bytes (§5.8), the member of a
   transparent wrapper, boxed values — before the value that holds them, so
   a type's check may rely on those of its members. The first failure in
   wire order is the one reported (in a map entry: the key's, then the
   mapped value's).
4. **The error is the type's own.** The status the check returns
   (by convention `codec_fail(message, 0, "<type name>")`) is passed through
   unchanged by every entry point.
5. **No cost without one.** A type without an invariant is validated without
   being materialised, exactly as before; a list or array of an enum is
   checked element by element only when the enum has an invariant.

Where it runs:

| entry point | invariant |
|---|---|
| `psio::validate`, `validate_or_throw`, `validate_strict`, `validate_pssz_compatible`, `decode_pssz_checked`, `validated_pssz_view` | **checked**: the value is materialised from its span and checked |
| `psio::encode` (fail-fast), `encode_pssz_checked` (status) | **checked** before any byte is written; `out` is unchanged on failure |
| `psio::decode`, `psio::make_boxed` (trusted: the input was validated) | **not checked** |
| views (`psio::view<T, psio::pssz>`) | **not checked** |
| `key` encode / `to_key`, key `validate`, `validate_strict`, `decode` (`docs/key-spec.md`) | **checked** |
| `json` / `ijson` encode, `encode_json_checked`, `validate`, `validate_strict`, `validate_json`, `decode`, `decode_json_checked`, `make_boxed` (`docs/json.md`) | **checked** |
| formats outside the core set (bin, ssz, frac, borsh, ...) | **not checked** |

**Maps and sets.** The order of the entries (§5.9.1) is defined for every
key, whether or not it satisfies its type's invariant: computing it runs no
check. Each codec checks the keys where it writes or reads them as values —
a writer before any byte, a reader after the structure — also a key read
from a JSON object member name (`docs/json.md`, "Maps and sets"), which is
a value of its type like any other.

**Nested values.** The bytes of a `Nested(T)` are validated as a T, which
runs T's invariants, by every reader of the enclosing value and by the
nested value's own checked constructors (C++ `nested<T>::from` /
`from_checked` / `checked` / `validate`). A writer of the enclosing value
does not look inside (§5.8): bytes adopted without a check (C++
`nested<T>::unchecked`, or the trusted `psio::decode`) are written as they
are, and every reader rejects them. A deferred nested value is not looked
into by an enclosing reader, invariants included.

The explicit-width profiles (`psio::pssz_<W>`) check exactly as the
automatic one. With exceptions disabled every checked entry point reports
the invariant's status; none throws or aborts.

**Through a schema.** A type with an invariant is a *custom type*: its
Schema IR node is `Custom{<wire type>, id}`, as for a type with a custom
text form (`docs/json.md`, "Custom types through a schema"). The id is the
text adapter's `schema_id` if the type has one, else the id given to
`PSIO_INVARIANT`, else the reflected record or enum name; a type with none
of these must provide one. A schema cannot carry a predicate, so the
schema-driven codecs take it as the `check` of the handler registered for
the id:

| registry | a custom node whose invariant fails |
|---|---|
| none | accepted: the node is its wire type, structure only |
| with the id's handler (`check`) | rejected with the invariant's status, on every entry point, binary and text |
| strict (`require_all`) without the id's handler | rejected: "no handler registered for a custom type" |

`validate_pssz(schema, root, bytes, has_unknown)` is the structural check;
`validate_pssz(schema, root, bytes, has_unknown, &handlers)` runs the
handlers' checks, as do `decode_auto` / `encode_auto` /
`decode_compatible<W>` / `decode_exact<W>` / `encode<W>` /
`validate_canonical_for_width<W>` and the text conversions given the
registry. On reads a check runs after the value's structure is accepted, on
writes before any byte is returned, nested values first. The entries of a
map / set and the value inside a `Nested` node are values like any other:
their custom nodes are checked (and, under a strict registry, required to
have a handler) where they occur, unless the nested bytes are read as
opaque.

The fixture classes `invariant` (`conformance/pssz-vectors.md`), the JSON
row kind `invariant` and the key row kind `key-invariant` hold structurally
valid values that violate an invariant, with the outcome for each of the
three registries above (`conformance/custom-types.md`).

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
- **inferred_bound(T).** The size bound computed from what T holds,
  never from T's own cap (§3.2). Drives the auto-width selection of a
  type with no cap.
- **max_encoded_size(T).** Maximum byte count any value of T encodes
  to (§3.7): at the widths actually used, and at most the cap.
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
`inferred_bound(UserPrefs) = None` ⇒ W = 4.

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
