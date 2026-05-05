# pjson v1 spec compliance — exceptions

This file accompanies `spec-compliance.md` and accounts for every row
currently marked ❌ or ⚠️. The CI gate (`tools/check-compliance.sh`)
fails if any matrix row is unaccounted for, so every entry below is
an explicit, documented decision rather than a silent gap.

A row appears here as long as **any** of its four cells (C++ impl,
C++ test, Rust impl, Rust test) is not ✅. When all four cells flip
to ✅, the id is removed from this file.

---

## Phase 1 status

Phase 1 — scalar wire format — is **functionally complete** in both
languages. The Rust driver at `rust/psio/src/bin/pjson_conformance_driver.rs`
and the C++ driver at `cpp/conformance/pjson_conformance_driver.cpp`
implement the same set of types and pass the conformance corpus
byte-exact. Cross-validation (`CC-005`, `CC-006`) is green.

What's left for Phase 1 closure: a handful of fixture / unit-test
follow-ups (Block C below) and the binary128 decode path (`F-004` —
softfloat vendored at `cpp/external/softfloat/`, just needs wiring).

Rows newly ✅ on **both** languages this commit:

```
T-001..T-008, T-015          (tag dispatch for Phase-1 codes)
N-001, N-002                 (null)
B-001..B-003                 (bool — B-004 ⚠️ on C++ no fixture)
UI-001..UI-003               (uint_inline)
NI-001..NI-004               (nint_inline)
U-001..U-005                 (uint, negint)
F-001..F-003, F-005, F-007   (ieee_float 16/32/64)
D-001, D-002                 (decimal — varscale tier 1)
E-001, E-004, E-005, E-012   (errors)
V-002                        (versioning behavior)
CC-001..CC-006               (corpus harness + cross-validation)
```

---

## Block A — Phase 2+ scope on both langs (deliberately ❌ pending phase)

These rows are not Phase-1 scope and are deliberately ❌ on both
languages until the relevant phase lands. Both drivers return
`NotImplementedYet` errors for them today, so a fixture targeting
any of these will fail loud rather than silently mishandle.

```
AG-001 AG-002 AG-003 AG-004 AG-005 AG-006
AT-001 AT-002 AT-003 AT-004 AT-005 AT-006 AT-007 AT-008 AT-009 AT-010 AT-011 AT-012
BY-001 BY-002 BY-003 BY-004 BY-005 BY-006
C-001 C-002 C-003 C-004 C-005 C-006
D-007
DOC-001
E-006 E-007 E-008 E-009 E-011
EM-001 EM-002 EM-003 EM-004 EM-005 EM-006 EM-007 EM-008
EX-001 EX-002 EX-003 EX-004 EX-005 EX-006
H-001 H-002 H-003 H-004 H-005
J-001 J-002 J-003 J-004 J-005 J-006 J-007 J-008 J-009 J-010 J-011 J-012 J-013 J-014 J-015
LIM-001 LIM-002 LIM-003 LIM-004 LIM-005 LIM-006
NS-001 NS-002 NS-003 NS-004 NS-005 NS-006 NS-007
O-001 O-002 O-003 O-004 O-005 O-006 O-007
RA-001 RA-002 RA-003
S-001 S-002 S-003 S-004
T-009 T-010 T-011 T-012 T-013 T-014
V-001
```

**Phase mapping:**

| prefix | phase | what unblocks it |
|--------|-------|------------------|
| AG-*, AT-*, O-*, RA-*, H-*, DOC-*, T-012, T-013 | Phase 2 (containers) | array, object, slot tables, hash byte, long-key escape |
| NS-*, S-*, BY-*, J-*, EM-*, T-009..T-011 | Phase 3 (JSON-side) | numeric_string, string, bytes, JSON mappings, emitter options |
| EX-*, T-014, V-001 | Phase 4 (extensibility) | extension framework |
| C-*, F-008, D-007 | Phase 4/5 (canonical encoding) | canonical-form picker rules |
| F-004 | Phase 5 (binary128) | wire softfloat into JSON-render path |
| LIM-* | covered alongside the underlying types | (no separate work) |

---

## Block B — Phase 1 ⚠️ rows (impl exists, dedicated test pending)

Both drivers implement these rules but at least one cell is ⚠️ for a
test gap. Each will close in a follow-up before Phase 1 is officially
sealed.

```
B-004 D-003 D-004 D-005 D-006 E-002 E-003 E-010 F-006 F-008 F-009
N-003 T-016 T-017 U-006 U-007 V-003
```

| id | gap | action |
|----|-----|--------|
| B-004 | C++ test only ⚠️: no corpus reject fixture for `0x12..0x1F` | add `bool_low_nibble_reserved.json` |
| D-003 | C++ test ⚠️: no corpus fixture for varscale 2-byte form | add `decimal_large_scale_2byte.json` |
| D-004 | C++ test ⚠️: no corpus fixture for varscale 3-byte form | add `decimal_large_scale_3byte.json` |
| D-005 | encoder dispatches but no large-scale fixture (4-byte form) | add `decimal_xlarge_scale_4byte.json` |
| D-006 | C++ test ⚠️: no dedicated transition fixture | covered if D-002..D-005 fixtures cover all four tiers |
| E-002 | reserved low-nibble bits per type — partial coverage | per-type as their phase lands |
| E-003 | bc out of range — impossible in 4-bit low nibble; defensive | add fixture where tag claims bc=8 but body is 4 bytes |
| E-010 | varscale truncation tested by code path; long-key path is Phase 2 | resolved by Phase 2 |
| F-006 | widths 6, 7 covered by code path but no fixture | add `ieee_float_width_{6,7}_reserved.json` |
| F-008 | NaN canonicalization (§15.2.1) not implemented | Phase 5 |
| F-009 | no ±Inf round-trip fixture | add `f64_pos_inf.json`, `f64_neg_inf.json` |
| N-003 | no fixture for `null` with non-zero low nibble | add `null_low_nibble_set_rejected.json` |
| T-016 | predicate range tests not asserted | add unit test enumerating 0..255 tag space (per language) |
| T-017 | bit pattern within is_integer not asserted | same |
| U-006 | no fixture for truncated uint/negint | add `uint_truncated.json`, `negint_truncated.json` |
| U-007 | only u64::MAX fixture; no u128-range value (bc 9..16) | add `uint_u128_max.json` |
| V-003 | predicate `(tag >> 4) >= 14` is implementation-property | add unit test asserting branch behavior |

These are tracked as small follow-ups; none block starting Phase 2 in
parallel. None reflect a silent skip — each has a concrete next
action.

---

## How to update this file

When a PR moves a row to ✅ on **both** language sides, delete its id
from the Block A or Block B list (and from any gap-table entry).

When a PR introduces a new spec rule with no test in the same change,
add an entry under Block B with rationale and concrete plan.
