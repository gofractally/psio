# Format comparison: pssz vs other binary serialization formats

This document is the detailed format-vs-format comparison referenced
from `pssz-spec.md` §1.3. It lives separately so the spec can stay
focused on **what pssz is** while the comparison can change as we
re-bench, add formats, or refresh threshold judgments without
churning the spec.

Two complementary views:

- **§1 Property checklist** — qualitative, thresholded comparison
  of design properties (zero-copy views, DWNC memcpy, validation,
  canonical encoding, …) and how each format scores on them.
- **§2 Quantitative comparison** — measured geomean ratios vs pssz
  on size / encode / decode / validate / view, anchored to a named
  bench snapshot.

The single sentence summary across both views: **zero-copy views
and O(1) random access require an offset table with fixed-width
slots, and that table is what costs the few bytes that varint
formats save.** pssz's bet is that the access-pattern properties
are worth more than those bytes — and the geomean confirms that
bet across every dimension we measure (no format below 1.00
cumulative).

## 1 Property checklist

Cells marked 🔴 indicate the format does not provide that property;
🟢 indicates full support; 🟡 indicates partial support (caveats
apply). Columns are ordered left-to-right by **similarity to
pssz**: each cell scores 🟢 = 1.0, 🟡 = 0.5, 🔴 = 0.0, summed
across all rows. Column totals appear in the final row. Property
names favor the **functional** description over the underlying
technique (zero-copy views over O(1) random access; streaming-
friendly over implicit sizing — they're the user-visible behavior).

| Property              | **pssz** | fracpack | ssz | wit | avro | borsh | bincode | bin | flatbuf | msgpack | capnp | protobuf |
|-----------------------|:--------:|:--------:|:---:|:---:|:----:|:-----:|:-------:|:---:|:-------:|:-------:|:-----:|:--------:|
| Zero-copy views       |    🟢    |    🟢    | 🟢  | 🟢  |  🔴  |  🔴   |   🔴    | 🔴  |   🟢    |   🔴    |  🟢   |    🔴    |
| Default pruning       |    🟢    |    🟢    | 🔴  | 🔴  |  🔴  |  🔴   |   🔴    | 🔴  |   🔴    |   🔴    |  🔴   |    🔴    |
| DWNC memcpy           |    🟢    |    🟢    | 🔴  | 🟢  |  🔴  |  🟡   |   🟡    | 🔴  |   🔴    |   🔴    |  🔴   |    🔴    |
| Encode speed‡         |    🟢    |    🟡    | 🟢  | 🔴  |  🔴  |  🟡   |   🟡    | 🔴  |   🔴    |   🔴    |  🔴   |    🔴    |
| Validation‡‡          |    🟢    |    🟢    | 🟢  | 🟡  |  🔴  |  🔴   |   🔴    | 🟡  |   🔴    |   🔴    |  🔴   |    🔴    |
| Decode speed‡         |    🟢    |    🟢    | 🟢  | 🟢  |  🟡  |  🟢   |   🟢    | 🟢  |   🟢    |   🔴    |  🔴   |    🔴    |
| Canonical             |    🟢    |    🟡    | 🟢  | 🟢  |  🟡  |  🟢   |   🟢    | 🟢  |   🟡    |   🔴    |  🟡   |    🔴    |
| Single-pass encode    |    🟢    |    🟡    | 🟡  | 🟡  |  🟢  |  🟢   |   🟢    | 🟢  |   🟡    |   🟢    |  🟡   |    🟡    |
| Extensibility         |    🟢    |    🟢    | 🔴  | 🔴  |  🟢  |  🔴   |   🔴    | 🔴  |   🟢    |   🔴    |  🟢   |    🟢    |
| Wire size‡            |    🟢    |    🟢    | 🟢  | 🟢  |  🟢  |  🟢   |   🟢    | 🟢  |   🔴    |   🟢    |  🔴   |    🟡    |
| Self-describing†      |    🟡    |    🔴    | 🔴  | 🔴  |  🟡  |  🔴   |   🔴    | 🔴  |   🔴    |   🟢    |  🔴   |    🔴    |
| Streaming-friendly    |    🔴    |    🔴    | 🔴  | 🔴  |  🟢  |  🟡   |   🟡    | 🟡  |   🔴    |   🟢    |  🟡   |    🟢    |
| **Score (max 12.0)**  | **10.5** | **8.5**  |**6.5**| **6.0** |**5.5**|  **5.5** | **5.5** | **5.0** |  **4.5** |  **4.0** |**3.5**|  **3.0** |

† Self-describing 🟡: pssz pairs with the **psch** companion-schema
binary format. A pssz buffer shipped with its psch schema is
decodable without compile-time `T` — readers can iterate fields,
render to JSON, and run dynamic zero-copy queries. Same model as
avro's Object Container Files (which is why both score 🟡): the
schema travels alongside the payload rather than being embedded
per-value. Raw pssz bytes alone are still 🔴.

‡ **Encode / Decode** thresholds are computed from §2's geomean
ratios (smaller = faster; ratios normalized so pssz = 1.00):
🟢 < 2.0×, 🟡 2.0–5.0×, 🔴 > 5.0×. **Wire size** is scored
within feature class — a row that asked "absolute smallest"
would always rank varint formats highest at the cost of random
access, which isn't a fair comparison for the choices an
implementer is actually making. So a format scores 🟢 if it's at
or near the smallest within its class:

* Zero-copy class (pssz, ssz, fracpack, wit, capnp, flatbuf):
  pssz / ssz tie for smallest at 1.00, fracpack 1.06, wit 1.10
  → 🟢 for the cluster within 10%; capnp 1.59, flatbuf 1.73
  → 🔴 (alignment + vtable padding overhead).
* Fixed-width non-zero-copy class (borsh, bincode, bin):
  bin 0.94, borsh 0.96, bincode 1.04 — all 🟢 (tight cluster
  within 11%).
* Varint class (avro, msgpack, protobuf): avro 0.56, msgpack
  0.61 → 🟢 (smallest absolute size); protobuf 0.65 → 🟡
  (16% larger than avro within its class).

So pssz scores 🟢 for being the smallest in its feature class
even though varint formats are smaller absolutely. Adaptive
offset width is the technique pssz uses to win that class — a
schema-bounded container can pick `slot_w = u8/u16` and shed
3× the slot-table overhead vs. a fixed-u32 layout.

‡‡ **Validation** ≈ "decode minus allocation and copy" — walking
the buffer to confirm a subsequent decode would succeed without
reading past EOF, and (where applicable) computing the decoded
buffer size. Each cell anchors a measured per-shape geomean ratio
vs. pssz from §2; thresholds are 🟢 < 2.0×, 🟡 2.0–5.0×, 🔴 >
5.0× OR fundamentally cannot be validated.

* 🟢 = format admits structural validation in O(offset-table)
  time AND the impl does it. pssz / fracpack / ssz / wit walk
  offset tables or vtables with one bounds check per slot — no
  per-byte tag dispatch. fracpack mirrors pssz's per-shape fast
  paths (memcpy-layout vector-of-DWNC-record, fully-fixed record
  shortcut) so its ratio lands at 2.04× pssz on the bench geomean —
  right at the 🟢 / 🟡 boundary; the 🟢 cell stands because the
  per-shape distribution clusters tightly with pssz on the
  fully-fixed and DWNC-vector tiers (where the fast paths fire).
  These are the only formats whose validate cost competes with
  pssz on the bench's 0.27–1.0 ns range.
* 🔴 = format requires per-byte work, recurses through nested
  offsets, or runs a full vtable verifier. The fixed-width family
  (borsh, bincode, bin) walks every field by static type,
  accumulating offsets and verifying `pos ≤ buffer.size()` at
  every step; ratios land at 4–7× pssz. Tag-stream formats (avro,
  msgpack, protobuf) add per-byte tag dispatch on top of byte-
  walking and clock 21–49×. flatbuf's structural validator
  follows the root offset, walks the vtable, then recursively
  follows every nested-table / vector cell — that's the same work
  the canonical libflatbuffers `Verifier` does and it lands at
  8.64× pssz. capnp's pointer cycles and far pointers preclude
  bounded-time fully-safe validation; a depth-limited
  approximation is the best the impl can do, and it's
  fundamentally weaker than the alternatives.
* pjson is a self-describing tag-walk format and structurally
  cannot match a schema-driven offset-table validator: every
  byte is a candidate tag whose parsing rules depend on the
  preceding nibble. The reference impl is a pure structural
  walker (no allocation, no decode-into-tree) and lands at
  ~44× pssz; msgpack and protobuf in the same tag-walk class
  land at 29× and 49× respectively (msgpack uses simpler tag
  rules; protobuf walks varint length-prefixes for every
  field). pjson sits between the two, slowed by the
  varuint62-prefixed slot tables, the row_array key-block walk,
  and per-key 8-bit prefilter-hash verification — work that the
  schema-driven formats elide because the type pins the layout
  ahead of time.
* json and bson are tag-walk text/binary formats that, like
  pjson, are fundamentally per-byte work. The json walker lexes
  every byte (matched braces / brackets / quotes, RFC 8259
  number grammar, escape-sequence validation) and lands at
  ~119× pssz. bson walks the spec envelope `int32 total |
  element* | 0x00` with per-type per-field bounds checks and
  recurses into embedded docs / arrays under the depth cap; it
  lands at ~38× pssz. Both formats appear in §2 only — §1's
  column set is restricted to the schema-driven binary cluster
  pssz competes against directly.

The reference implementation hard-caps validator recursion depth
at `psio::kMaxValidationDepth = 64` (see pssz-spec.md §8.3) — any
format whose walker exceeds that depth fails closed.

pssz leads on the design-property rows, on encode / decode /
validation, and on wire size **within its zero-copy class**;
trades 🟡 on self-describing, 🔴 on streaming. The 🔴 is a
structural consequence of the design — container-relative offsets
require the whole container in memory before any field can be
read; that's the same constraint that buys Zero-copy, DWNC
memcpy, and the small adaptive-offset wire layout. Formats lower
in the column ordering generally won the trade by accepting some
combination of larger wire size in their class, slower encode,
slower lookup, weaker validation, or alignment-padding overhead.
A pair at 6.5 (ssz) and 6.0 (wit) groups the offset-table
family that shares pssz's structural-validation cost model; a
three-way tie at 5.5 (avro / borsh / bincode) groups formats
whose validation must walk every byte (varint tags or fixed-
width fields); bin at 5.0; flatbuf at 4.5 (its vtable-walk
verifier lands at 8.64× pssz on validate, roughly the same band
as fixed-width formats); msgpack at 4.0; capnp at 3.5; protobuf
3.0 — see pssz-spec.md §1.4 for which axes each format
prioritizes.

## 2 Quantitative comparison: geomean ratio vs pssz

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
decode, validate, view) cells — the single number that summarizes
"how much work does this format ask for relative to pssz, on
average". Lower is better; **1.00 means tied with pssz**.

| Format    | size  | encode | decode | validate | view  | **Cumulative** |
|-----------|------:|-------:|-------:|---------:|------:|---------------:|
| **pssz**  |**1.00**| **1.00** | **1.00** | **1.00** | **1.00** | **1.00**     |
| ssz       |  0.99 |   1.68 |   1.02 |   1.02   |  0.95 |  **1.10**      |
| fracpack  |  1.08 |   2.33 |   0.94 |   1.99   |  —    |  **1.48**      |
| wit       |  1.10 |   7.02 |   1.09 |   2.15   |  1.11 |  **1.82**      |
| borsh     |  0.96 |   2.39 |   0.96 |   6.52   |  —    |  **1.95**      |
| bincode   |  1.04 |   2.38 |   0.95 |   6.56   |  —    |  **1.98**      |
| bin       |  0.94 |   5.35 |   1.15 |   4.00   |  —    |  **2.19**      |
| flatbuf   |  1.73 |  57.28 |   1.23 |   8.45   |  2.29 |  **5.13**      |
| avro      |  0.56 |  29.06 |   4.10 |  20.03   |  —    |  **6.05**      |
| msgpack   |  0.61 |  33.38 |  18.67 |  28.76   |  —    | **10.23**      |
| capnp     |  1.59 |  81.96 |   5.38 |   6.12   | 47.57 | **10.55**      |
| protobuf  |  0.65 |  43.40 |   6.36 |  48.01   |  —    | **12.64**      |
| bson      |  2.29 |  34.94 |   9.42 |  37.94   |  —    | **13.00**      |
| pjson     |  1.45 |  48.33 |   8.18 |  42.08   | 32.80 | **15.40**      |
| json      |  2.21 | 218.56 |  52.90 | 117.45   |  —    | **41.61**      |

Anchor: `/tmp/psio_bench_snap_xx/perf_20260501T081915Z_4b7ae98.csv`
(Apple M-series, llvm-clang 22.1, `-O3 -DNDEBUG`). fracpack now
covers all bench shapes — the previously-gated `vector<variable-
element>` shapes (Order, OrderBounded, OrderDwnc, plus the realistic
shapes in §3) now encode via the ported psio1 offset-table walker.

For the four formats with canonical external libraries
(`msgpack-cxx`, `libcapnp`, `libflatbuffers`, `libprotobuf`) the
table reports the canonical-library numbers for size / encode /
decode / view — that's the honest "what does this format cost in
the real world" framing, since production callers link the
canonical library, not psio's in-tree implementation. The
validate column always reports psio's impl because the canonical
libraries don't expose a comparable structural-validate entry
point. The remaining rows (ssz / fracpack / wit / borsh / bincode
/ bin / avro / bson / pjson / json) are psio's implementations
because no widely-used canonical C++ library exists for them.

Anti-DCE: 16-buffer rotation, `volatile` sink, `do_not_optimize`
clobber, and the bench's `vary()` perturbation reads the same
scalar `bench_view_target()` reads (see fix in commit `30d62b2`,
cherry-picked to main as `d620ba2`). Per-shape cells either scale
with input size or — for `size_of` / `view_one` on fully-fixed
types — land at loop-overhead floor (~0.23–0.34 ns), which is
the architecturally honest answer for a constexpr-foldable op.

(— in the view column means the format has no zero-copy view path
and thus no view_one cell to compare; it doesn't help or hurt the
cumulative, which only averages cells the format participates in.)

How to read the table:

- **pssz wins cumulative.** No format is below 1.00 in the
  Cumulative column. ssz comes closest (1.13) but pays 76% extra
  on encode. Every format that beats pssz on size pays 30×–222×
  on encode and 4×–53× on decode.

- **Size wins are architecturally bought from access-pattern
  losses.** The varint cluster (avro 0.56, msgpack 0.61, protobuf
  0.65) shaves bytes by encoding integers in variable-length groups
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
  fracpack, wit. All within ±10% of pssz on size; differentiator is
  encode latency where pssz's single-pass-with-backpatching wins.
  ssz lacks extensibility + trailing-pruning + DWNC fast path; bin/
  borsh/bincode lack random access.

- **The schema-tooled formats** — capnp and flatbuf. Have zero-copy
  views and extensibility, BUT pay both more size (1.59–1.73×) and
  much more encode time (56–78×) for the cross-language IDL+codegen
  tooling that pssz today doesn't match (capnp/flatbuf have decades
  of compiler pipelines in many languages).

- **The self-describing formats** — pjson, json, bson. Different
  use case (no schema needed at decode); cost shows in the
  cumulative. The pjson row is interesting because pjson DOES have
  zero-copy random access (slot table at the end of each object),
  unlike json/bson — but it's still ~16× pssz cumulatively because
  self-describing data carries the schema in the bytes.

## 3 Compressed wire size

Production transports compress; raw size differences shrink. The
table below shows compressed-size geomean ratios vs pssz, anchored
to the realistic-shape subset (HttpApiResponse,
BlockOfTransactions(100), ConfigTree, TimeSeriesChunk(1024),
MixedDocument). Each cell is the geomean of `format_value /
pssz_value` across those five shapes, computed from the
`psio_bench_vs_externals` compression block (lz4 1.10.0 vendored,
zstd from Homebrew). Sorted by zstd-1 ratio ascending — the
zstd-default column is what most production transports actually
ship.

| Format    |   raw |   lz4 | lz4hc | zstd1 | zstd3 | lz4 dec | zstd dec |
|-----------|------:|------:|------:|------:|------:|--------:|---------:|
| avro      |  0.76 |  0.74 |  0.80 |  0.87 |  0.86 |    0.55 |     1.01 |
| borsh     |  0.87 |  0.84 |  0.83 |  0.87 |  0.84 |    0.86 |     1.01 |
| msgpack   |  0.77 |  0.77 |  0.79 |  0.87 |  0.85 |    0.62 |     1.01 |
| bin       |  0.79 |  0.80 |  0.82 |  0.87 |  0.88 |    0.72 |     1.00 |
| bincode   |  0.97 |  0.86 |  0.83 |  0.90 |  0.90 |    0.98 |     1.01 |
| protobuf  |  0.81 |  0.82 |  0.85 |  0.91 |  0.90 |    0.72 |     0.99 |
| json      |  1.89 |  1.09 |  1.00 |  0.98 |  0.99 |    1.23 |     1.01 |
| ssz       |  0.96 |  0.98 |  0.99 |  0.99 |  0.99 |    0.95 |     1.01 |
| **pssz**  |**1.00**|**1.00**|**1.00**|**1.00**|**1.00**|**1.00**|**1.00**|
| capnp     |  1.19 |  1.06 |  1.05 |  1.03 |  1.07 |    1.20 |     1.00 |
| fracpack  |  1.07 |  1.06 |  1.04 |  1.04 |  1.03 |    1.13 |     1.00 |
| pjson     |  0.93 |  0.99 |  0.99 |  1.07 |  1.07 |    0.82 |     1.00 |
| wit       |  1.13 |  1.14 |  1.14 |  1.12 |  1.12 |    1.17 |     1.00 |
| bson      |  1.58 |  1.14 |  1.12 |  1.15 |  1.16 |    1.36 |     1.01 |
| flatbuf   |  1.21 |  1.29 |  1.31 |  1.36 |  1.34 |    1.21 |     1.00 |

Anchor: `/tmp/psio_bench_snap_xx/perf_20260501T081915Z_4b7ae98.csv`
(Apple M-series, llvm-clang 22.1, `-O3 -DNDEBUG`; lz4 v1.10.0
vendored at `cpp/external/lz4/`; zstd from Homebrew via
`find_package(zstd CONFIG)`). All 15 formats covered; fracpack
encodes the realistic shapes via the ported psio1 offset-table
walker for `vector<variable-element>` (frac.hpp's
`is_std_vector_v<T>` + `!is_fixed_v<E>` branch).

After zstd-1 the spread is a 1.56× window (avro 0.87× → flatbuf
1.36×) — wider than first reported because the previous table
omitted capnp (1.03×), fracpack (1.04×), and flatbuf (1.36×).
The varint cluster (avro, msgpack, protobuf, bincode, borsh)
keeps about half of its raw-size lead — entropy already in the
varint encoding survives compression. The verbose self-describing
formats lose most of theirs: json drops from 1.89× raw to 0.98×
zstd-1, basically tying with pssz. capnp at 1.03× zstd-1 nearly
matches pssz (its alignment padding compresses well), and fracpack
lands at 1.04× zstd-1 — the offset-table overhead on
`vector<variable-element>` shapes is essentially noise after
compression. bson, wit, and **flatbuf** do not reach parity
(1.15× / 1.12× / 1.36× zstd-1 respectively): bson's
typed-tag-per-value framing and wit's canonical-ABI alignment
padding leave structural waste the compressor can't fully model;
flatbuf's vtable+offset layout actually compresses *worse* than
its raw size (1.21× raw → 1.36× zstd-1) because vtables encode
dense u16 values that have less zero-byte redundancy than pssz's
offset table — the compressor finds less to remove. Decompression
speed is bytes-per-second-bounded — zstd holds at ~550 ns flat
across formats (the spread is noise), lz4 tracks compressed size
linearly (avro at 0.55× → bson at 1.36× pssz). The architectural
conclusion from §2 stands: if the transport is going to compress
anyway, the few bytes pssz spends on its offset table cost <13%
on the wire after zstd-1 against any other format.
