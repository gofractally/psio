# Independent review of pjson tag numbering — 2026-09-16

Recommendation: adopt the May audited tag map for the stable format, then bring
the public C++ implementation into conformance and use it as the cross-language
reference. Its integer pairs are clearer, its small-negative encoding saves a
byte, and its remaining type families have useful contiguous ranges. C++ being
the reference implementation does not require preserving its older assignments.

Follow-up: the user approved this recommendation. The codecs and golden
fixtures now use wire revision 2; see `wire-contract.md`. The observations below
describe the pre-migration comparison. Adoption of a tag map also does not establish acceptance of every
semantic rule or feature in the May draft.

## Evidence and chronology

- Public C++ constants: `cpp/include/psio/pjson.hpp`, `detail` tag enum near
  line 276. Integer sizing and encoding near lines 657 and 798 confirm the
  behavior; header comments alone are insufficient because some are stale.
- Commit `ed4375331f77ccfccaa77a00536e695a0d9618fb` (May 5) deliberately redesigned
  the tag map in `docs/pjson-spec.md` and explicitly deferred implementation.
- Commit `f671e907538a37e8ee075a607dfdb21b5afa7507` (May 6) updated the public Rust
  library. Its diff and message explicitly leave public C++ migration pending.
- The standalone C++ conformance driver implements the May map; the public C++
  headers implement the older map. Agreement with the driver was never proof
  that the public C++ library used the May format.

The historical statement that nobody used the format yet was made in May.
It does not establish whether persisted data or external consumers exist now.

## Proposed assignments

All numbers below are hexadecimal **high-nibble type codes**, not complete
tag bytes. The low nibble keeps its type-specific role.

| Code | Current public C++ | Recommended May map |
|---|---|---|
| 0 | null | null |
| 1 | bool | bool |
| 2 | reserved | inline unsigned integer, 0..15 |
| 3 | inline unsigned integer | inline negative integer, -1..-15 |
| 4 | unsigned magnitude | unsigned magnitude |
| 5 | decimal | negative magnitude |
| 6 | IEEE float | IEEE float |
| 7 | negative magnitude | decimal |
| 8 | string | numeric-string wrapper |
| 9 | reserved | string |
| A | bytes | bytes |
| B | array | array |
| C | object / shared-schema row array | object / shared-schema row array |
| D | reserved | opaque extension |
| E–F | reserved | reserved |

Assignments for numeric strings and extensions can be fixed while their
implementation and release support are decided separately. Unsupported features
must be explicitly rejected and documented; assigning a code is not implementing
it.

## Why this map wins

The integer block becomes `2,3,4,5`: positive/negative inline forms are adjacent,
and positive/negative magnitude forms are adjacent. After checking membership
in this block, `code & 1` means **negative**, and `code & 2` means **inline**.
"Negative" is more accurate than the draft's "signed": a positive value from a
signed C++ or Rust source uses the positive encoding too.

Integers occupy 2..5, floats/decimals occupy 6..7, and all ordinary numeric values
occupy 2..7. Numeric-string at 8 bridges numeric access and quoted JSON output;
ordinary text and bytes follow at 9 and A. Containers remain B and C. Extensions
have a separate code D and E/F remain available. These are useful organizational
properties, independent of which language implements the format first.

The size improvement comes from adding negative inline values, not moving type
numbers. Fresh probes compiled from the public C++ header and standalone draft
driver produced:

| Value | Current C++ bytes | May-map bytes | Size change |
|---|---|---|---|
| 0 | `30` | `20` | 1 → 1 |
| 5 | `35` | `25` | 1 → 1 |
| 15 | `3f` | `2f` | 1 → 1 |
| 16 | `40 10` | `40 10` | 2 → 2 |
| -1 | `70 01` | `31` | 2 → 1 |
| -5 | `70 05` | `35` | 2 → 1 |
| -15 | `70 0f` | `3f` | 2 → 1 |
| -16 | `70 10` | `50 10` | 2 → 2 |
| -255 | `70 ff` | `50 ff` | 2 → 2 |
| -256 | `71 00 01` | `51 00 01` | 3 → 3 |

This saves one byte per qualifying tagged scalar. Typed homogeneous arrays
already omit per-element tags and receive no such benefit. Real document savings
depend on values, container selection, and offset-width thresholds.

Evidence: `/Users/dlarimer/psiserve/outputs/pjson-tag-review-2026-09-16/comparison.json`
and `public-probe.cpp`. Both probes were compiled with Homebrew Clang, C++23,
`-O2`; the draft driver's tag-predicate self-test also passed. This was a focused
format check, not a performance benchmark or a full draft-conformance run.

## Limits of the draft's rationale

- Current C++ already has a contiguous numeric block (3..7). The improvement is
  integer subgrouping and paired signs, not the invention of a numeric range.
- A range or bit test is convenient source code, but a compiler can optimize
  switches too. There is no measured runtime speedup from this review.
- Type predicates apply after checking the relevant family. They do not validate
  payload sizes, reserved low bits, or integer ranges.
- Numeric-string still needs inner-value dispatch and text rendering. Its
  placement does not remove that work. JSON projection predicates also depend
  on emitter options and extension handlers.
- Type order is not numeric sort order for encoded bytes. Negative magnitudes
  and little-endian payloads preclude that interpretation.

## Alternatives considered

Keeping current assignments and using reserved code 2 for negative-inline would
preserve the meanings of existing valid bytes while adding the same scalar size
saving. Existing readers would reject the newly used code. This is attractive
if maintaining an already deployed format dominates the decision, but its
positive/negative families remain irregular. Current compatibility evidence
does not by itself establish that such deployment exists.

A third layout could put all four integer forms at aligned codes 4..7 and move
float/decimal to 2..3. Integer membership would then be a mask test. That is a
plausible alternative bit layout, but gives no wire-size improvement, requires
another new mapping beyond both existing versions, and has no demonstrated
workload benefit. The May map is the better practical choice for this project.

Keep the low-nibble magnitude convention for negative-inline (`-low`, with zero
reserved). Using `-1-low` would fit -16 too, but introduces a different magnitude
rule for one extra value; there is no workload evidence to justify that change.

## Separate semantic decisions before a stable release

1. Numeric strings: distinguish wire support from automatic string conversion.
   Preserve the original string exactly, including zeros and exponent spelling;
   use a wrapper only when its defined rendering reproduces that string.
   The draft's unconditional lifting policy can add work or bytes and merits
   separate review. Explicit typed annotations are also useful.
2. IEEE bit 3: prefer reserving it, as in the draft. A scientific-notation hint
   only partially preserves presentation. Removing it changes accepted bytes,
   not wire size, and belongs in the migration contract.
3. Extensions: D is a sensible location, but subtype allocation, unknown-value
   preservation, and JSON envelope collisions need their own contract.
4. Float width, numeric range, and canonicalization: binary128 support, full
   128-bit magnitudes, decimal/float selection, signed zero, and NaN handling
   are independent of the high-nibble assignments. A passing retagged corpus
   cannot establish these semantics.
5. Bytes hints, typed-array element codes, and container layouts: top-level
   renumbering does not require changing these. Review any additional changes
   explicitly rather than bundling them into a tag migration.

## Migration and acceptance

`35` is valid under both maps: public C++ decodes it as **+5** and the freshly
built draft driver decodes it as **-5**. Trying one decoder and falling back to
the other is unsafe. Version selection must come from an explicit envelope,
API, or known data provenance; the current payload has no in-band version.

If the recommendation is adopted:

1. Freeze one named wire revision and specify how old stored data is identified
   and converted. Do not auto-detect these maps from payload validity.
2. Update C++ encode, decode, validation, views, typed codecs, and JSON bridges
   together. Add explicit byte assertions independent of the implementation,
   including boundaries and the ambiguous-byte example above.
3. Bring Rust and JS/TS to that contract while retaining applicable independent
   safety and packaging fixes from the current local work. Avoid blanket file
   restores, which would discard unrelated fixes.
4. Regenerate C++ golden fixtures and validate the public APIs and packaged
   consumers in all three languages. Use draft-driver fixtures as additional
   semantic evidence rather than treating the separate driver as the library.
5. Update the wire contract, compliance documentation, and status report to
   identify the adopted revision and its implemented features precisely.
