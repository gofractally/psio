# Automatic PSSZ widths belong to logical types

This repair applies to the existing C++ `psio::pssz` codec. Before the repair,
`Small` selected one-byte offsets by itself, but encoding it inside a four-byte
parent propagated the parent's width into the child. The automatic view selected
the child's one-byte layout, so its bytes were incompatible with the encoder.
The reproducer now emits the same `01016869` child bytes both ways; the reference
at `552ca3744fc5a0cd3c69530cfd4f1e466eadd8f7` emitted
`04000000040000006869` for the nested child.

Each automatic size/encode/decode/validate/throwing-validation recursion now
selects the encountered logical type's own compile-time width. A record owns its
header and field offset slots; a vector or array owns its element offset table.
Their variable child payloads use the child's canonical layout. This also covers
the optimized in-place record decode path used by vectors. No payload-size-based
width selection, record framing, identifiers, or new codec is introduced.

The explicit compatibility tags `pssz_<1>`, `pssz_<2>`, and `pssz_<4>` retain their
existing inherited width through recursion. Twenty-four typed reference encodings
were generated and validated using the exact clean reference headers. The repair
checks those bytes, and rejects oversized stored offsets instead of truncating
them. An explicit narrow profile may still have a large final payload when its
end is not stored in a narrow offset; the test suite preserves that behavior.

## Budget and bound selection

An explicit `maxDynamicData(N)` is the logical type's authoritative width budget:
`N <= 255` selects one byte, `N <= 65535` selects two, and other budgets select
four. A large budget does not shrink because the current fields happen to be
small. The existing runtime cap compares the complete encoded size against N;
this repair preserves that interpretation and enforces it on nested values,
including memcpy fast paths. The annotation's name should not be interpreted as
a newly introduced dynamic-only cap.

Without an explicit budget, `max_encoded_size<T>()` infers a conservative bound
from field/type/wrapper annotations. The corrected composition includes record
headers, variable field slots, array/vector element offset tables, and conservative
optional presence overhead. A child with a declared budget reserves that entire
budget in its parent. Addition/multiplication overflow or an unbounded child
returns `nullopt`, selecting four bytes rather than wrapping to a small bound.

Inference uses four-byte bookkeeping throughout. It deliberately allows threshold
slack: a record containing only 252 raw fixed bytes selects two-byte offsets
because its conservative bound is 256, even though today's one-byte header plus
payload would fit in 253 bytes. There is no circular search for the narrowest
self-consistent layout. More precise inference would itself be a wire ABI change.

For annotation-only types, a consuming SDK must record and compare the compiled
width in its ABI manifest. An annotation cannot predict future appended fields;
the consumer must reject an incompatible width change. The fixture consumer emits
a JSON width/budget/bound manifest for this qualification. It is evidence for the
typed fixtures, not a complete psibase SDK manifest.

## Views and rejection

Nested optional/variant views now preserve the containing root span and accept
the two-span constructor already used by record views. Fixed-count array views
use the array's own logical width, which can differ from a vector with the same
element type. Existing nonempty record view layouts are unchanged.

Sizing checks addition, multiplication, every stored offset/header, and the
existing 32-bit variable-span ceiling before allocating an output buffer. An
oversized value fails without widening; a size failure leaves an append
destination unchanged. Compile-time and runtime declared caps cannot be skipped
by fixed-record/vector memcpy shortcuts. A cap on a fixed element is enforced
only when that element is present; an empty vector is valid.

## Qualification and open gates

Run `cpp/tests/qualify_type_width.py --help` for the reproducible native driver.
It retains compiler/hardware/load/disk records, exact commands, raw output,
source and binary hashes, failure-before/success-after probes, the 24 explicit
reference encodings, sixteen native test targets, and a fixture width manifest.
`--reference-source` requires a clean tracked source at the exact 552ca baseline.
The optional WASI step compiles the real typed consumer using the existing SDK;
it does not qualify guest linking or runtime behavior. Linux's explicit
`CATCH_CONFIG_NO_POSIX_SIGNALS` option only bypasses the bundled Catch2 v2 crash
reporter's incompatibility with dynamic glibc `SIGSTKSZ`; assertions stay enabled.

These results qualify the width repair and the covered existing tests. They are
not a global strict-semantic or complete malformed-input certification. Open
existing-codec gates before PSSZ activation include:

- The generic strict walker explicitly omits typed `length_bound` checks. A
  17-byte label annotated with maximum 16, inside a valid 64-byte type budget,
  passes current structural and strict validation. The failing audit probe is
  retained separately; no successful constraint-enforcement claim is made.
- A zero-sized fixed `Unit` payload makes `optional<Unit>` presence ambiguous
  under the current fixed-payload shortcut. The existing optional presence
  mechanism needs a separate repair; no record boundary is introduced here.
- `vector<Unit>` cannot preserve element count using zero-sized elements in the
  current format. Nonempty encoding and malformed spans need explicit rejection
  rather than a new count prefix. Those types are excluded from this qualification.
- Appended-field compatibility, dynamic schema parity, Rust/browser parity, and
  complete malformed-input safety remain independent activation gates. The
  existing limited static/dynamic parity tests passing here do not prove them.

Canonical user sources remain untouched. This branch consumes the existing codec
and metadata APIs; it does not copy a codec into psibase or create a wire family.

## Changes at the evolution merge

- The explicit-profile reference encodings are no longer all the 552ca bytes:
  `some-*`, `none-*` and `relay-*` in `pssz_type_width_explicit_goldens.hpp`
  were regenerated because `optional<record>` lost its selector byte
  (`docs/pssz-spec.md` 5.5). The other entries are unchanged.
- Of the open gates listed above, zero-size optionals/vectors, appended-field
  compatibility and record-header validation are now closed in C++: readers
  honor the writer's fixed-region header, pruning is mandatory, and the
  cross-version matrix is `cpp/tests/pssz_evolution_matrix_tests.cpp`. Typed
  `length_bound` validation and Rust/browser parity remain open.
- Width selection is unchanged: an explicit `maxDynamicData` always rules,
  otherwise the inferred bound applies. Because an appended optional can move
  an inferred bound across a threshold (for example `{array<u8,251>}` is one
  byte and becomes two with an appended `optional<u8>`), such a step is not a
  legal evolution. `psio::pssz_evolves_from<Old, New>` checks this at compile
  time; reserve room with an explicit `maxDynamicData` to evolve.
