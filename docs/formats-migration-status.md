# Formats migration — status and handover

Branch `codex/formats` (worktree `~/psio-formats`), from `e12e4f6`. Brings
first-class Cap'n Proto and FlatBuffers from the first generation
(`~/psiserve/libraries/psio1`, read-only) into this library.

## Handover (newest first)

### After F1 (Cap'n Proto) — tip of this note

State: F1 committed (`babc96a`), main `2c17312` merged (`4ec83a4`);
393/393 ctest in both simdjson configurations (the two new targets are
`psio_capnp_tests`, 823 assertions, and `psio_capnp_interop_tests`, 3,621
assertions against libcapnp 1.3.0). `capnp.hpp` + `capnp_view.hpp` compile
with `-fno-exceptions -fno-rtti` (syntax check of a view / edit program;
not wired into ctest). User documentation: `docs/capnp.md`.

Measured with the new view doing what the official reader does (struct
size bounds, far pointers, native unions, bit lists); message written by
libcapnp; one run, load 7.4, min of 11 trials, ns/op
(`~/psio-formats-logs/f1-capnp-run1.txt`):

| Operation | g1 view | g2 `capnp_view` | libcapnp, reader per op | libcapnp, held root |
|---|---:|---:|---:|---:|
| one field of the root | 0.38 | 0.54 | 17.6 | 0.25 |
| all 5 fields of a nested struct | 1.67 | 2.40 | 29.8 | 12.4 |
| nested scalar | 0.70 | 0.99 | 21.6 | 3.59 |
| nested string | 1.07 | 1.65 | 25.3 | 7.58 |
| string field | 0.75 | 1.01 | 21.7 | 3.84 |
| iterate 16 structs (range-for) | 3.10 | 3.55 | 49.5 | 31.8 |
| iterate, all fields | 8.20 | 8.83 | 115 | 97.6 |
| decode to a C++ struct | 96 | 92 | 203 | |
| validate | 58 | 55 (typed) | | |
| encode | 229 | 121 | 461 | |

So, today: against a reader constructed per message psio is 12–33× faster
on reads; against a held root 3.5–11×, EXCEPT a single scalar of the root
struct where libcapnp's held root is 2× faster (the psio view resolves the
root pointer each time it is constructed; libcapnp did that when the
reader was made). Encode 3.8×, decode 2.2× faster. The new view is 1.1–1.5×
slower than the first generation's, which is the price of the checks g1
skipped. Needs a second run for spread before the numbers go into
`docs/format-comparison.md` (F6).

Not done in F1 (carry to later stages): the safe / trusted split for the
capnp views (they are "trusted" in the sense of the proposal below: they
rely on validation); switching a union member through `capnp_document`;
`std::map` / `std::set` / `std::tuple` / wrapper types; packed encoding;
porting the first generation's 74 view tests one by one (the new tests
cover the same surface on the native layout, but it is not a port — g1's
union tests assert the non-native layout); a `-fno-exceptions` ctest
target; the existing `emit_capnp` has not been checked against the new
mapping (F3: it must emit unions for variants / optional scalars and
defaults for initialisers).

### After the frame pass (PSSZ / SSZ / WIT views, requested after F0)

State: main line merged (`50092a4`, G46–G51). Two frame commits on top;
391/391 ctest in both simdjson configurations. F1 still uncommitted in
`~/psio-formats-scratch/f1-wip/`.

What was done, and what it bought. ns/op, minimum of 11 interleaved trials,
two runs each (run 1 / run 2), binaries built through CMake
(`psio_bench_view_generations`), 1-minute load 5.9–7.8 throughout. "before"
is the merged tip with the F0 lookup fix; "after" adds the two commits.

| Operation | old (g1) | before | after |
|---|---:|---:|---:|
| PSSZ one field | 0.23 (frac 0.28) | 0.31 / 0.31 | 0.31 / 0.31 |
| PSSZ all 5 fields | 0.70 | 2.84 / 2.82 | 2.42 / 2.63 |
| PSSZ nested scalar | 0.24 | 1.05 / 1.04 | 0.94 / 1.02 |
| PSSZ nested string | 0.24 | 1.42 / 1.42 | 1.41 / 1.48 |
| PSSZ string field | 0.24 | 0.69 / 0.68 | 0.68 / 0.70 |
| PSSZ iterate 16 records | 2.52 | 10.6 / 10.6 | 10.6 / 11.4 |
| PSSZ iterate, all fields | 8.44 (frac 6.86) | 22.7 / 19.4 | 18.6 / 20.0 |
| SSZ all 5 fields | 0.66 | 1.32 / 1.34 | 1.31 / 1.31 |
| SSZ nested scalar | 0.31 | 0.48 / 0.47 | 0.42 / 0.42 |
| SSZ iterate 16 records | 2.64 | 6.30 / 6.25 | 6.59 / 6.60 |
| SSZ range-for | — | 6.00 / 5.86 | 9.58 / 10.4 |
| SSZ iterate, all fields | — | 10.8 / 10.8 | 11.9 / 11.9 |
| WIT all 5 fields | 0.53 | 0.61 / 0.61 | 0.61 / 0.61 |
| WIT iterate 16 records | 2.96 | 4.75 / 4.71 | 4.73 / 4.69 |
| set of 64 ints, hit | 1.41 | 2.60 / 2.73 | 2.72 / 2.85 |
| map of 32 strings, hit | 7.7 | 19.0 / 18.0 | 17.6 / 19.1 |

1. **Per-record state** (`view.hpp` "Per-record state", `pssz_view.hpp`
   `record_state`). A PSSZ record view reads the extensible-record header
   and checks the fixed region once, at construction, and keeps the fixed
   size (the view grows from 32 to 40 bytes: two spans + one word). Same
   results for every input, including empty and unusable bytes
   (`PSIO_VIEW_NO_RECORD_STATE` builds the old path; the new tests run
   both). It buys about 10 % on "all fields" and "nested scalar" and
   nothing measurable elsewhere: after inlining the compiler was already
   merging most of the repeated header work. SSZ and the canonical ABI have
   no per-record header, so there is nothing to hoist; their one size
   compare per field is the safety guarantee itself.
2. **A hole in the guarantee, closed** (`pssz_view.hpp`, `ssz_view.hpp`
   `vector_element_span`). The premise "a view never reads out of bounds on
   unvalidated input" was NOT true before this pass and was not tested: a
   list of variable-size elements took the element's start and end from
   the offset table without comparing them with each other or with the
   list, so corrupted offsets produced a span outside the buffer (the new
   test crashed with SIGBUS on the PSSZ case and failed on the SSZ one).
   Elements are now bounded. This is why SSZ iteration is slower after
   than before (range-for 6.0 → ~10, iterate-all +10 %): the guarantee
   costs two more compares per element, and for range-for a worse loop.
   `cpp/tests/view_unvalidated_tests.cpp` (new; 15,056 assertions) pins:
   default-constructed views and `at(i)` past the end read as zero / empty
   for PSSZ, SSZ and WIT; every prefix and every single-byte corruption of
   an encoded record yields only spans inside the buffer. Not covered:
   optional / variant / map members under corruption, `unpack()` (which is
   a trusted decode and is NOT safe on unvalidated bytes).
3. Not done: caching the count / table base in the list view. In the
   loops measured the compiler already hoists the count; what remains per
   element is not bookkeeping (below).

**What is left, and why.** Per element of a PSSZ list of records the safe
view executes 4 loads and 5–6 compare-and-branch pairs (element start,
element end, their order and bound; the element's header and its bound;
the member against the header) where the first generation executes 2 loads
and none. The two generations differ in three things that bookkeeping
cannot remove:

* **The guarantee.** Every offset read from the bytes is compared before it
  is used, so that unvalidated bytes cannot make a view read outside the
  buffer. g1 compares nothing.
* **Eager sizes.** A sub-view is a span, so its size is computed (one more
  load: the next offset) even when only a fixed member is then read. g1's
  cursor is a pointer; it never learns the size.
* **The wire's evolution rule** (PSSZ only): a member is present only if
  the writer's header covers it. One compare per member; g1's pssz32 did
  not implement pruned trailing optionals at all.

### Proposal (NOT implemented): views of bytes already validated

The remaining gap is the price of the guarantee, so closing it is a
semantic choice: let the caller say the bytes are validated.

* **Spelling.** `psio::pssz_view<T>{bytes}` stays the safe view.
  `psio::pssz_view<T>{psio::trusted{bytes}}` — and the view returned by a
  validating entry point (`validated_pssz_view`, `from_F`-style checked
  constructors, `as_view(buffer)` for a buffer that validated on entry) —
  is a distinct type, `view<T, Fmt, storage::trusted_borrow>` (a new
  storage kind beside `const_borrow`), so trust is in the type: it
  propagates to every sub-view, cannot be obtained by accident, and a
  function that takes `pssz_view<T>` does not silently accept or produce
  it. A trusted view converts to the safe view of the same bytes (one
  way); the reverse needs `psio::trusted{}` again.
* **What is skipped.** Every compare whose only purpose is unvalidated
  input: element start/end order and bound, header ≤ size, variable-member
  start/stop bounds, the scalar size test, `i < size()` inside
  `operator[]`. Sub-view sizes are not computed until asked for (the
  trusted cursor is a pointer plus the enclosing end, like g1's).
* **What is kept.** The PSSZ header compare (it is the wire's meaning, not
  a check): a member the writer did not write still reads as absent.
  `at(i)` stays checked. A default-constructed trusted view reads as
  zero / empty (one null test, shared by all members of the record).
* **What the caller guarantees.** The bytes passed `validate_F<T>` for
  this T (or were produced by `to_F` in this process, or are a row this
  service wrote), and are not modified while the view lives. If not: out
  of bounds reads, exactly as `from_F<T>(psio::trusted{bytes})` today.
* **Const-safety and hand-off.** Unchanged: a trusted view is read-only
  and borrows. `mutable_view` keeps its own rule (it already requires
  validated bytes and only overwrites fixed-size values, which cannot
  invalidate what a trusted view assumes).
* **What it buys** (the g1 column is the measured ceiling for the same
  work on the same machine): all 5 fields of a nested record 2.4 → ~0.7
  ns; one nested scalar 0.94 → ~0.25; a string member 0.68 → ~0.25;
  iterating 16 records 10.6 → ~2.5–3 (0.66 → ~0.17 ns per element);
  map of strings lookup 18 → ~8. The psibase hot path — a few fields of a
  row the service itself wrote — is the "all fields" / "nested" row:
  roughly 3–4×.
* **Cost.** A second instantiation of the view templates per type used
  both ways; the format traits gain unchecked forms of each function (or
  the capnp / flatbuf style: a format-owned cursor view). The Rust, JS and
  Python views need the same split to keep the surface aligned.

### After F0 — view frames head to head

State: F0 measured and committed. F1 (Cap'n Proto) is started but NOT
committed; its work in progress is in `~/psio-formats-scratch/f1-wip/`
(see "F1 work in progress" below). F2–F6 not started.

Decisions taken in F0:

1. The span-based frame is **not** as fast as the first generation's
   pointer-based one today (numbers below). The cause is not the span, the
   root span or the named-reader proxy: it is the bounds checks the SSZ /
   PSSZ / WIT layout traits repeat on every access (instruction counts:
   26 against 57 for the five fields of a record in SSZ), and for PSSZ the
   extensible-record header read on every field.
2. Fixed in the frame: map / set lookup (`contains`, `find`, `at`,
   `value_or`, `lower_bound`) encoded every probed key into a freshly
   allocated vector. Integer and string keys now compare in place
   (`pssz_view.hpp`); 72 ns → 3.4 ns for a set of 64 ints, 156 → 20 ns for
   a 32-entry string map.
3. NOT changed: the per-access checks in the SSZ / PSSZ / WIT traits. They
   are what makes a default-constructed or out-of-range (`at(i)`) view read
   as zero, PSSZ is consensus-critical, and another branch is working in
   those files. Removing them is a decision for the owner of those traits;
   the measured ceiling is the g1 column.
4. Cap'n Proto and FlatBuffers views will be **format-owned
   specializations of `psio::view` that hold a cursor** (a struct's data
   pointer + section sizes + message, a table pointer), not instances of
   the span-based shape templates. Reasons: a capnp value is not a
   contiguous byte range (two sections behind a pointer, one-bit Bools,
   scalars XORed with defaults, far pointers), and a FlatBuffers scalar
   that is absent from the vtable has no bytes at all. The frame already
   allows this (PSSZ's own tuple / map / enum views are format-owned
   specializations); it needs **no change to `view.hpp`**. Named readers
   (`reflect<T>::proxy`), `view_iterator`, `unpack()`, comparisons and the
   `F_view` alias are reused. The benchmark has a slot for the new views
   next to g1's (`G2_HAVE_CAPNP_VIEW`), so parity with g1 is checked
   before F1 is called done.

How to run everything:

```sh
# configure (tests + benchmarks), both JSON configurations
cmake -S ~/psio-formats -B ~/psio-formats-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang -DCMAKE_PREFIX_PATH=/opt/homebrew \
  -DPSIO_ENABLE_TESTS=ON -DPSIO_ENABLE_SIMDJSON=ON -DPSIO_ENABLE_BENCHMARKS=ON \
  -DPSIO_BENCH_GEN1_INCLUDE=$HOME/psiserve/libraries/psio1/cpp/include
cmake -S ~/psio-formats -B ~/psio-formats-build-nosimd -G Ninja ... -DPSIO_ENABLE_SIMDJSON=OFF
cmake --build ~/psio-formats-build -j 3 && ctest --test-dir ~/psio-formats-build -j 3

# F0 benchmark (needs the first generation's headers; Homebrew clang)
cmake --build ~/psio-formats-build -j 3 --target psio_bench_view_generations
~/psio-formats-build/cpp/benchmarks/psio_bench_view_generations            # all groups
~/psio-formats-build/cpp/benchmarks/psio_bench_view_generations --quick ssz # one group

# capnp layout against the official compiler (F1, needs `capnp` on PATH)
python3 cpp/tests/capnp/layout_fuzz.py --cxx /opt/homebrew/opt/llvm/bin/clang++ \
  --include cpp/include --seed 1 --structs 120
```

Logs: `~/psio-formats-logs/` (`f0-run1.txt`, `f0-run2.txt` are the two
benchmark runs quoted below).

## F0 results

`cpp/benchmarks/bench_view_generations.cpp`. Apple silicon, Homebrew clang
22, `-O3`. ns per operation, minimum of 11 interleaved trials, two runs
(run 1 / run 2); 1-minute load 2.9–5.7 during run 1, 5.7–8.2 during run 2.
Trial-to-trial spread (max−min)/min was 2–58 % per cell — the machine was
not quiet — so differences under ~25 % are not established. Every cell
passes the buffer address through an optimisation barrier before the
operation and consumes the result through one.

Shape: `Order {id, customer: UserProfile{id, name, email, age, verified},
items: 16 × LineItem{product, qty, unit_price}, total, note}`.

g1 = first generation, g2 = this library.

### Same bytes in both generations

| Operation | SSZ g1 | SSZ g2 | WIT g1 | WIT g2 |
|---|---:|---:|---:|---:|
| one field (`id`) | 0.24 / 0.27 | 0.25 / 0.30 | 0.25 / 0.27 | 0.26 / 0.31 |
| all 5 fields of `customer` | 0.67 / 0.81 | 1.33 / 1.62 | 0.57 / 0.68 | 0.68 / 0.82 |
| nested scalar | 0.32 / 0.39 | 0.48 / 0.58 | 0.24 / 0.27 | 0.24 / 0.31 |
| nested string | 0.32 / 0.39 | 0.71 / 0.87 | 0.24 / 0.26 | 0.30 / 0.41 |
| string field | 0.24 / 0.31 | 0.34 / 0.40 | 0.24 / 0.26 | 0.31 / 0.41 |
| iterate 16 records, one scalar each | 2.70 / 3.69 | 6.28 / 8.04 | 3.14 / 4.41 | 4.74 / 5.73 |
| range-for | not offered | 6.24 / 7.59 | 3.10 / 4.40 | 4.88 / 6.12 |
| iterate, all 3 fields | not offered | 11.2 / 13.3 | 4.81 / 14.0 | 6.57 / 8.49 |

(g1's `ssz_view` has no `begin()`/`end()` and no view of a `double`.)

### PSSZ (each generation's own wire) and g1's fracpack

| Operation | g1 pssz32 | g1 fracpack | g2 PSSZ |
|---|---:|---:|---:|
| one field | 0.24 / 0.26 | 0.24 / 0.27 | 0.33 / 0.40 |
| all 5 fields | 0.73 / 0.91 | 0.81 / 0.98 | 2.97 / 3.59 |
| nested scalar | 0.24 / 0.29 | 0.24 / 0.31 | 1.07 / 1.38 |
| nested string | 0.25 / 0.32 | 0.35 / 0.50 | 1.53 / 1.87 |
| string field | 0.26 / 0.31 | 0.27 / 0.33 | 0.74 / 0.90 |
| iterate 16 records | 3.04 / 3.76 | 3.16 / 3.95 | 11.7 / 14.6 |
| iterate, all fields | 10.5 / 11.6 | 8.7 / 8.7 | 24.8 / 28.0 |

g2's PSSZ record view reads the extensibility header and tests the field
against it on every access (that is the wire's evolution rule, not a
defensive check), then re-checks the offsets it read.

### Map / set lookup (g1 fracpack, g2 PSSZ)

| Operation | g1 | g2 before | g2 after the fix |
|---|---:|---:|---:|
| set of 64 ints, hit | 1.8 / 2.1 | 72 | 3.4 / 4.0 |
| set, miss | 2.5 / 2.9 | 81 | 3.5 / 4.0 |
| map of 32 strings, hit | 9.4 / 9.9 | 156 | 20.0 / 23.8 |
| map, miss | 10.8 / 10.9 | 159 | 17.9 / 22.5 |

("before" from one quick run at load 19; the ratio, not the digits.)

### Cap'n Proto — the message is written by libcapnp 1.3.0

| Operation | g1 `view<T, cp>` | libcapnp, reader per op | libcapnp, root held |
|---|---:|---:|---:|
| one field | 0.55 / 0.60 | 20.1 / 20.3 | 0.29 / 0.31 |
| all 5 fields | 1.90 / 2.14 | 33.2 / 35.3 | 13.7 / 15.0 |
| nested scalar | 0.82 / 0.91 | 22.5 / 26.4 | 3.79 / 4.38 |
| nested string | 1.16 / 1.41 | 26.5 / 29.8 | 8.06 / 8.72 |
| string field | 0.82 / 0.97 | 22.6 / 24.7 | 4.06 / 4.69 |
| iterate 16 structs | 3.56 / 3.83 | 53.8 / 57.7 | 34.7 / 37.0 |
| iterate, all fields | 10.2 / 10.9 | 138 / 138 | 107 / 118 |
| decode to a C++ struct | 119 (g1) · 102 (g2 before F1) | 249 / 256 | |
| encode | 275 (g1) · 165 (g2 before F1) | 563 / 589 | |
| validate | 70 (g1) · 66 (g2 before F1) | (done inside the reader) | |

"Reader per op" constructs a `FlatArrayMessageReader` each time (what a
program receiving a message does); "root held" keeps one reader and
measures accessors only. The old claim "reads Cap'n Proto 10–36× faster
than libcapnp" reproduces against a reader per operation (12–37×) and is
5–10× against a held root, **except** one scalar of the root struct, where
libcapnp's held root is faster (0.29 against 0.55: the g1 view re-reads the
segment table). The g1 view gets its speed partly by not doing what the
official reader does: it does not bound a field by the size of the struct
the writer wrote (so a message from an older schema reads out of bounds),
has no far pointers, reads `List(Bool)` as bytes, and lays unions out
differently from `capnp compile` whenever two members could share space.
F1 fixes all four; the F1 view must be measured again.

### FlatBuffers — the buffer is written by Google's library 25.12.19

| Operation | g1 `view<T, fb>` | flatbuffers generated accessors |
|---|---:|---:|
| one field | 0.56 / 0.56 | 0.60 / 0.60 |
| all 5 fields | 3.10 / 2.82 | 2.64 / 2.66 |
| nested string | 1.10 / 0.98 | 1.05 / 0.91 |
| iterate 16 tables | 9.11 / 8.99 | 9.07 / 8.76 |
| iterate, all fields | 17.3 / 18.0 | 16.6 / 17.8 |
| decode to a C++ struct | 130 (g1) · 136 (g2 MVP) | |
| verify | 102 (g1) | 80 / 84 |
| encode | 411 (g1, 1000 B) · 689 (g2 MVP, 888 B) | 932 / 894 (888 B) |

Reads are at parity with Google's accessors (both are a vtable lookup);
"faster than their native API" holds for FlatBuffers **encode** only (g1:
2.2×, with a 12 % larger buffer), not for reads or verification.

## F1 findings

* `psio::capnp::layout<T>` is a constexpr port of the official compiler's
  `StructLayout` (holes, union members sharing and growing locations,
  discriminant placed when the second member is added).
  `layout_fuzz.py` compared 360 random records (unions of mixed sizes,
  optional scalars, shuffled `field<N>`) against `capnp compile -ocapnp`:
  0 mismatches.
* The first generation's union layout — and the current Rust crate's,
  which is a port of it — is **not** the official one: it gives every
  alternative its own slot and places the discriminant after all members.
  `union { a @0 :UInt32; b @1 :UInt32; }` is 8 bytes officially (both at
  bits 0–32, tag at 32–48) and 16 in g1 / Rust. Rust needs the same
  allocator before C++ and Rust can exchange unions.
* Mapping chosen: `std::optional<pointer type>` is the pointer (null = no
  value); `std::optional<scalar>` is `union { none :Void; some :S }`;
  `std::variant` is a union with one member per alternative. The old
  length-1-list convention is gone.
* Ordinals: ascending `field<N>` (declaration order by default); a variant
  takes one ordinal per alternative. Consequence to document: growing a
  variant that is not the last member shifts later ordinals.
* Size-changing edits: append-and-repoint (sound for capnp: pointers are
  relative, no existing byte moves; the replaced object remains as
  unreachable bytes until `compact()`), single segment only.

## Not done

Everything in F1–F6 beyond the above. Nothing in `rust/`, `js/`,
`python/` was touched.
