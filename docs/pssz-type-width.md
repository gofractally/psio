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

Inference originally used four-byte bookkeeping throughout (a record containing
only 252 raw fixed bytes selected two-byte offsets because its conservative
bound was 256). That is no longer the rule: see "Every container measures
itself at its own width" below. The paragraph is kept because the qualification
records referenced here were taken under it.

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
  cross-version matrix is `cpp/tests/pssz_evolution_matrix_tests.cpp`.
  Rust/browser parity remains open.
- Typed `length_bound` validation is closed for reads: `validate`,
  `validate_strict` and the checked readers reject a string or list member
  outside its declared bound (`cpp/tests/pssz_length_bound_tests.cpp`). The
  17-byte label above is now rejected. The encoder checks it too: `encode`
  raises `codec_exception` (aborts without exceptions) and
  `encode_pssz_checked` returns a status for a member outside its bound,
  leaving the sink unchanged. The fail-fast `psio::decode` checks it as
  well (the same exception, or abort): a member bound is part of the type,
  and every pssz entry point that reads or writes a record enforces it.
- Width selection is unchanged: an explicit `maxDynamicData` always rules,
  otherwise the inferred bound applies. Because an appended optional can move
  an inferred bound across a threshold (for example `{array<u8,253>}` is one
  byte and becomes two with an appended `optional<u8>`), such a step is not a
  legal evolution. `psio::pssz_evolves_from<Old, New>` checks this at compile
  time; reserve room with an explicit `maxDynamicData` to evolve.
- Rust parity is no longer open for the typed codec: `rust/psio` has the
  automatic tag `Pssz` with the same width selection (`PsszPack::AUTO_BOUND`
  mirrors `max_encoded_size`, `#[pssz(max_dynamic_data = N)]` is the explicit
  budget), checked against the shared fixture `conformance/pssz-vectors.json`.
  The schema-driven codecs (C++ `pssz_ir.hpp`, Rust `pssz::dynamic`) implement
  the automatic profile from the Schema IR (`docs/pssz-ir-runtime.md`).

## Every container measures itself at its own width

This is a wire change to the automatic profile (`psio::pssz`); the explicit
profiles `pssz_<1|2|4>` are untouched. The normative text is
`docs/pssz-spec.md` §3.1–§3.2; this is the summary.

**What changed.**

1. A list's own offset table narrows. A list — `std::vector` bounded by a
   member annotation, `psio::bounded<T, N>`, a map, a set — whose count and
   elements are bounded used to write its per-element offset table 4 bytes
   wide. It now uses the list's own width: 1, 2 or 4. An unbounded list (no
   count bound, or unbounded elements) stays 4.
2. The bound of a type counts that type's own header / slots / table at the
   width the type ends up with, not at 4. This applies to every type that has
   offsets: lists, arrays of variable elements, maps, sets, tuples, pairs and
   records.

**The rule.** A type that has offsets consists of k words that are W bytes
each (the header of an extensible record or tuple, one slot per variable
member, one table slot per element) and of other bytes bounded by r (the
inline fixed members and the payloads, each payload being the finished bound
of the type it holds, or that type's explicit cap). Its bound is

```
fit(k, r) = k + r      if k + r     <= 255        (W = 1)
          = 2k + r     if 2k + r    <= 65535      (W = 2)
          = 4k + r     otherwise                  (W = 4)
```

and its width is the W of the branch taken, which is also the width class of
the result. `fit` reads only k and r, both known before W is: the definition
is bottom-up over the type tree, with no search and no fixed point, and it
terminates because the tree is finite (a recursive type goes through a box or
an unbounded list, which are unbounded without being entered).

| type | k | r |
|---|---|---|
| `bounded<T, N>` / `vector<T>` under `max<N>`, T variable | N | N × bound(T) |
| `array<T, N>`, T variable | N | N × bound(T) |
| map / set under `max<N>` | N | N × bound(entry) |
| tuple, pair, extensible record | 1 + number of variable members | Σ bound(member) |
| DWNC record | number of variable members | Σ bound(member) |

A list or array of fixed-size elements has no table: N × size. An explicit
`maxDynamicData` still rules for the record that declares it (its W is the
width class of the cap, and whatever holds it reserves the cap).

**In code.** `max_size.hpp` (`fit`, `sequence_bound`, `tuple_bound`, the
record case of `max_bare`) computes the bound; `pssz_impl::width_for<T, A>()`
(the width of T at a position carrying the annotations A) and
`auto_pssz_width<T>()` are the width class of it. The schema-driven codec
mirrors both (`pssz_ir.hpp` `Codec::fit`, `bound_of`, `width_of`); a plain
`List` bounded by the `lengthBound` attribute of the record member that holds
it (the `exact` / `min` forms, which the schema keeps as an attribute) takes
its count from there.

**Views.** A view re-selects each width from the type it views. A member whose
annotations narrow a width below it — now including a plain `max<N>` on a
list of bounded variable elements — is viewed in the format
`psio::pssz_at<A>`, which carries the member's annotations down the view
exactly as the codec carries them (`view_layout::traits<pssz_at<A>>`); every
other member is viewed as `psio::pssz`, as before. Offsets have one
compile-time width per view type, so `operator[]` is O(1) at every width.
`mutable_view` follows the same formats. One case stays unaddressable: a
variant whose alternatives are narrowed by `at<I>(...)` (one view format
serves all alternatives); state those bounds in the alternatives' types.

**Thresholds moved.** `{array<u8,254>}` (1 + 254 = 255) is the last one-byte
record of that shape and `{array<u8,255>}` the first two-byte one; with the
old 4-byte bookkeeping the step was at 251 / 252. A type that needs room to
grow declares `maxDynamicData`, as before.

## A record's cap bounds what lies beneath it

`docs/pssz-spec.md` §3.6. A record with an explicit `maxDynamicData(N)` (a
method's argument tuple with a cap is one) never encodes to more than N
bytes, so nothing inside it does. The cap is the implicit bound of every
position beneath the record, down to the next record, for the purpose of the
offset widths there:

```
width of a list / map / set / array / tuple / pair at a position
   = width class of min(its own bound at that position, the position cap)
```

where the position cap is the explicit cap of the nearest enclosing record
(none if that record has no cap), and an unbounded type counts as infinite.

```cpp
struct Transfer
{
   std::uint32_t                         token;
   std::string                           memo;   // one slot, as wide as Transfer
   std::vector<std::string>              tags;   // 1-byte offset table
   std::vector<std::vector<std::string>> rows;   // 1-byte tables at both levels
   PSIO_REFLECT(Transfer, token, memo, tags, rows, maxDynamicData(255))
};
```

**What an unbounded string or byte list costs.** Nothing of its own: its
payload is the raw bytes, with no length prefix (its size is the distance to
the next offset). The only bookkeeping is its offset slot in the record that
holds it, at that record's width. In a record capped at 255 — the largest cap
with one-byte offsets — a `std::string` or `std::vector<char>` member costs
one byte beyond its content, bounded or not. For
`{u32, u64, u64, utf8 string memo, optional<bytes> data}` capped at 255: 1
(header) + 20 + 2 (slots) = 23 bytes of framing, 232 bytes for memo and data
together (a present `data` spends one of them on its selector), and encoding
one byte more is refused.

**In code.** The cap travels with the annotation carrier of the position
(`detail/annotation_tree.hpp`: every carrier has `cap`; `ann::capped<N>` is a
position with no annotation beneath a cap), set by a record's member carrier
(`pssz_impl::member_ann`) and inherited by `element_ann` / `entry_ann` /
`at_ann`; a transparent type and a `Box` pass the cap on, a record does not.
`pssz_impl::width_for<T, A>()` takes the `min`. The member's C++ type is
unchanged. The schema-driven codec keeps the cap in effect while it walks
(`pssz_ir.hpp` `active_cap`, set on entering a record's members, cleared at a
`Nested` boundary) and needs nothing new in the IR: the record's
`maxDynamicData` attribute is already there.

**Records keep their width.** A reflected record beneath a capped record is
not narrowed: its bytes are the same wherever it appears, which `view<Child>`,
`nested<Child>` and every schema that names `Child` rely on. A child that is
wider than its capped parent can never use that capacity;
`psio::pssz_child_wider_than_cap_v<Parent>` reports it at compile time
(`static_assert(!psio::pssz_child_wider_than_cap_v<Transfer>)`), and the
remedy is a `maxDynamicData` or member bounds on the child. It is a query and
not an error because such types are valid and exist (a capped envelope around
an open-ended payload record).

**No diagnostic for a bound larger than the cap.** `max<1000>` on a member of
a record capped at 255 is clamped for the width and otherwise still means what
it says (it limits the count); it is accepted silently.

