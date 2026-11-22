# Format comparison: pssz vs other binary serialization formats

This document is the detailed format-vs-format comparison referenced
from `pssz-spec.md` §1.3. It lives separately so the spec can stay
focused on **what pssz is** while the comparison can change as we
re-bench, add formats, or refresh threshold judgments without
churning the spec.

Fracpack is an experimental format (`psio::experimental::frac32`,
`psio/frac.hpp`), not a supported wire format. It is in these benchmarks to
measure one design trade-off against pSSZ: fracpack stores a size with every
variable-size value (a byte-count prefix on each string and vector, a
fixed-region size on each extensible record), while pSSZ stores none and
infers each size from the offsets around the value. Its comparison columns
record that experiment. It is not byte-compatible with psibase's fracpack in general.

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
are worth more than those bytes. §2 measures what it costs: the
varint formats are 4–16× slower to decode, while the fixed-layout
formats that give up extensibility (ssz, borsh, bincode) currently
decode 16–20% faster than pssz and encode into a reused buffer about
15% faster.

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

The numbers below are measured: three full runs of
`psio_bench_vs_externals` on 2026-11-22, at the commit that made pssz's
sizing pass check each container once (Apple M-series, Homebrew clang
22.1, `-O3 -DNDEBUG`), each cell the **median over the three runs** of
the run's best-of-7 time. For every cell
(format × shape × operation) the ratio is `format_value / pssz_value`:
below 1.00 the format beats pssz on that cell, above 1.00 pssz wins.
Per-format figures are the **geometric mean** of those ratios over the
20 shapes.

The "Cumulative" column is the geomean across all cells of the row. It
mixes bytes and nanoseconds and weighs a validator that checks little
the same as one that checks everything (see the caveats): read the
columns, not the total.

| Format | size | encode (new buffer) | encode (reused buffer) | decode | validate | view | **Cumulative** |
|---|---:|---:|---:|---:|---:|---:|---:|
| ssz | 0.99 | 0.98 | 0.84 | 0.84 | 0.15 | 0.75 | **0.65** |
| wit | 1.10 | 1.22 | — | 0.97 | 0.33 | 0.95 | **0.83** |
| bin | 0.94 | 1.07 | 1.00 | 0.95 | 0.37 | — | **0.85** |
| borsh | 0.96 | 0.95 | 0.85 | 0.80 | 1.19 | — | **0.94** |
| bincode | 1.04 | 0.95 | 0.84 | 0.80 | 1.20 | — | **0.95** |
| **pssz** | **1.00** | **1.00** | **1.00** | **1.00** | **1.00** | **1.00** | **1.00** |
| fracpack | 1.11 | 0.98 | 0.96 | 1.01 | 4.17 | — | **1.34** |
| flatbuf | 1.73 | 4.83 | 5.57 | 1.42 | 1.28 | 1.91 | **2.17** |
| avro | 0.56 | 3.01 | 4.22 | 3.68 | 3.61 | — | **2.49** |
| msgpack | 0.61 | 3.39 | 2.20 | 15.50 | 4.08 | — | **3.10** |
| capnp | 1.59 | 6.07 | — | 5.07 | 0.86 | 36.78 | **3.45** |
| protobuf | 0.65 | 3.73 | 3.63 | 5.62 | 6.78 | — | **3.55** |
| pjson | 1.45 | 5.01 | 6.62 | 8.06 | 6.38 | 27.47 | **6.26** |
| bson | 2.29 | 9.26 | 11.99 | 8.06 | 5.42 | — | **6.44** |
| json | 2.21 | 5.17 | — | 27.33 | 27.31 | — | **9.61** |

**Caveats — read these before quoting a number.**

- **Load.** The machine was shared with other builds while the runs
  were made (1-minute load average 22 at the start of the first run, 17
  at the second, 6 at the third, on an 18-core machine). Differences under
  about 15% in this table are not established; rerun on an idle machine
  before relying on them.
- **Validators do not check the same things.** pssz and borsh check
  every `bool` byte of every element; `ssz` and `bin` validate a list of
  fixed-size records by its byte count alone (ValidatorList(100): ssz
  1 ns, bin 13 ns, pssz 267 ns, borsh 678 ns), and so did fracpack until
  `ecb8126`, which is why it no longer looks twice as fast as it is
  (its per-element walk is now the slow one: 1 756 ns). The `validate`
  column therefore rewards checking less. `bin`'s validator also rejects
  its own encoding of six shapes (DWNC records with variable members,
  "bin: trailing bytes"); those cells are excluded, and the benchmark no
  longer records a validate cell whose validator rejects the input.
- **What a cell observes.** Before `fe42696`'s companion change to the
  benchmark, an encode cell observed only the size and first byte of its
  output and a decode cell one scalar of the decoded value, and the
  compiler elided the rest: fixed-shape cells measured under 1 ns for
  work they did not do. Every timed body now hands its whole output —
  the encoded buffer, the decoded value and everything it points to — to
  a barrier the optimiser must assume reads it, and every input is
  laundered so it is not a compile-time constant; before timing, each
  input is decoded and compared with the value it encodes. Cells under
  1 ns remain, and are real: decoding an 8- or 16-byte fixed record is
  one load and one store (0.5 ns), validating one is a length compare,
  and `size_of` of a fixed-size type is a constant.
- **Canonical libraries.** For msgpack, protobuf, flatbuf and capnp the
  size / encode / decode / view figures are those of the canonical C++
  library (msgpack-cxx, libprotobuf, libflatbuffers, libcapnp), which is
  what a production user links; `validate` is always psio's
  implementation. `—` means the format has no such operation here (no
  reusable-buffer encoder, no zero-copy view).
- **Encode has two columns.** "new buffer" returns a fresh
  `std::vector` per call (allocation included); "reused buffer" appends
  to one the caller keeps. The earlier table had one encode column.
- The numbers that this table replaces (fracpack: encode 2.33, decode
  0.94, validate 1.99; every other row likewise) came from the old
  harness and are not reproducible. §1's per-cell prose still quotes
  ratios from that snapshot; where it disagrees with this table, this
  table is the measurement.

### pssz and fracpack, shape by shape

fracpack time / pssz time (above 1.00 pssz is faster), and pssz's
absolute times:

| shape | size | encode (new) | encode (reused) | decode | validate | pssz ns: enc new / enc reused / dec / val |
|---|---:|---:|---:|---:|---:|---|
| Point | 1.00 | 0.99 | 1.51 | 0.94 | 17.10 | 9.7 / 1.6 / 0.5 / 0.3 |
| NameRecord | 1.00 | 0.91 | 0.99 | 0.92 | 16.91 | 9.5 / 2.5 / 0.5 / 0.3 |
| FlatRecord | 1.17 | 1.02 | 0.93 | 1.20 | 1.90 | 17.7 / 7.4 / 18.1 / 8.6 |
| Record | 1.14 | 1.01 | 0.78 | 1.36 | 2.37 | 19.8 / 11.1 / 19.9 / 13.1 |
| Validator | 1.00 | 1.01 | 0.99 | 1.00 | 7.90 | 9.8 / 3.4 / 1.1 / 2.4 |
| Order | 1.09 | 1.08 | 1.00 | 1.16 | 2.14 | 26.8 / 29.5 / 59.0 / 43.3 |
| ValidatorList(100) | 1.00 | 0.92 | 1.06 | 1.01 | 6.57 | 165.2 / 142.8 / 97.1 / 267.1 |
| FlatRecordBounded | 1.29 | 0.92 | 1.01 | 1.14 | 1.75 | 18.9 / 8.5 / 21.0 / 9.3 |
| RecordBounded | 1.30 | 0.82 | 0.77 | 1.30 | 2.24 | 22.9 / 12.6 / 22.0 / 13.6 |
| OrderBounded | 1.29 | 0.83 | 0.78 | 1.00 | 2.04 | 34.9 / 37.8 / 67.0 / 45.6 |
| ValidatorListBounded(100) | 1.00 | 1.05 | 1.01 | 0.91 | 6.60 | 168.3 / 144.7 / 102.5 / 279.2 |
| FlatRecordDwnc | 1.25 | 1.04 | 1.03 | 1.36 | 2.49 | 16.3 / 6.9 / 16.0 / 6.7 |
| RecordDwnc | 1.21 | 1.01 | 0.93 | 1.72 | 2.98 | 19.1 / 9.9 / 15.9 / 9.8 |
| OrderDwnc | 1.15 | 1.27 | 1.19 | 1.71 | 2.52 | 27.8 / 28.4 / 48.3 / 40.9 |
| ValidatorListDwnc(100) | 1.00 | 0.87 | 0.93 | 0.94 | 6.52 | 189.1 / 163.6 / 108.0 / 274.1 |
| Deep4Ext | 1.41 | 0.89 | 0.34 | 0.17 | 2.28 | 11.1 / 11.1 / 6.4 / 16.9 |
| Deep4Dwnc | 1.00 | 0.99 | 1.43 | 0.97 | 1.27 | 10.5 / 1.8 / 0.7 / 14.1 |
| MlEmbedding | 1.01 | 1.03 | 1.08 | 0.98 | 1.71 | 18.2 / 7.1 / 14.2 / 4.1 |
| BlobPayload | 1.01 | 0.95 | 1.07 | 0.89 | 12.12 | 21.1 / 7.2 / 15.5 / 0.5 |
| WideRecord | 1.00 | 1.00 | 1.00 | 1.06 | 45.68 | 10.2 / 4.8 / 1.5 / 1.4 |

Geomean, fracpack / pssz: size 1.11, encode into a new buffer 0.98,
encode into a reused buffer 0.96, decode 1.01, validate 4.17. pssz is
smaller and validates faster, and the two now encode and decode at the
same speed on the geomean; fracpack still encodes the bounded
record-with-strings shapes faster (RecordBounded 0.77–0.82, OrderBounded
0.78–0.83), and pssz a few of the others.

**The sizing pass.** Until this measurement the gap on those shapes was
pssz's `size_of`, not the wire format: on Order it cost 15.7 ns against
fracpack's 2.4 ns, more than pssz's whole write pass, because it
re-checked offset and size overflow for every field and every element
through a run-time "report or throw" pointer. The pass now takes the
failure policy as a template parameter (the fail-fast pass carries no
pointer), sums each container's payloads without a branch and checks
overflow and the largest offset once per container, and looks at the
pruning count only for trailing optional fields. Measured on the same
machine, before → after (pssz, ns):

| shape | size_of | encode (new) | encode (reused) |
|---|---:|---:|---:|
| FlatRecord | 3.4 → 0.8 | 20.8 → 17.7 | 10.0 → 7.4 |
| Record | 5.0 → 1.3 | 22.5 → 19.8 | 11.3 → 11.1 |
| Order | 15.7 → 3.0 | 37.7 → 26.8 | 52.7 → 29.5 |
| OrderBounded | 16.7 → 2.9 | 49.9 → 34.9 | 57.2 → 37.8 |
| OrderDwnc | 16.7 → 2.8 | 34.6 → 27.8 | 47.5 → 28.4 |
| RecordBounded | 5.0 → 1.4 | 25.3 → 22.9 | 16.3 → 12.6 |

The bytes and the error of every conformance vector are unchanged. A
record whose members are all plain numbers written in full is now
validated by one size compare instead of a walk over its fields
(WideRecord, 32 `u32` fields: 41.4 → 1.4 ns). Not addressed: decoding
nested extensible records (Deep4Ext 6.4 ns against fracpack's 1.1) and
the cost a bounded shape still pays over its unbounded twin on encode
(OrderBounded 34.9 / 37.8 ns against Order 26.8 / 29.5; its cause has
not been isolated).

How to read the table:

- **pssz does not win every column.** On the measured cells the
  fixed-layout formats without extensibility — ssz, borsh, bincode —
  encode into a reused buffer about 15% faster and decode 16–20% faster
  than pssz (into a new buffer the allocation dominates and they are
  within 5%), and bin and wit are level with it. The rows below 1.00 in
  "Cumulative" (ssz, wit, bin, borsh, bincode) get there partly on
  those cells and, for ssz, wit and bin, largely through a `validate`
  that checks less than pssz's. What pssz has that none of those five
  have together is in §1: extensibility with pruning, per-type offset
  widths, zero-copy views and full validation.

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
  fracpack, wit. All within about ±10% of pssz on size, and within
  about ±20% of it on encode and decode: at this distance the order
  among them is decided by implementation detail (and, on this run, by
  machine load), not by the format. ssz lacks extensibility,
  trailing-pruning and the DWNC fast path; bin / borsh / bincode lack
  random access.

- **The schema-tooled formats** — capnp and flatbuf. They have
  zero-copy views and extensibility, and pay more size (1.59–1.73×)
  and more encode time (5–6×) through their canonical libraries, whose
  builders allocate and copy; capnp's reader set-up makes its
  single-field view 37× pssz's.

- **The self-describing formats** — pjson, json, bson. A different
  use case (no schema needed at decode); the cost shows in every
  column: 5–12× on encode, 8–27× on decode. pjson does have zero-copy
  random access (a slot table at the end of each object), unlike json
  and bson.

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
