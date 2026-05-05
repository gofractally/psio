# pjson v1 spec compliance — exceptions

This file accompanies `spec-compliance.md` and accounts for every row
currently marked ❌ or ⚠️. The CI gate (`tools/check-compliance.sh`)
fails if any matrix row is unaccounted for, so every entry below is
an explicit, documented decision rather than a silent gap.

A row appears here as long as **any** of its four cells (C++ impl,
C++ test, Rust impl, Rust test) is not ✅. When all four cells flip
to ✅, the id is removed from this file.

---

## Block A — C++ has no driver yet (universal ❌ on C++ side)

The pre-audit C++ implementation in `cpp/include/psio/pjson*.hpp` is
on the old wire format and does not match the post-audit spec
(commits `ed43753`, `795734b`). The Phase 1 deliverable for C++ is
to mirror the Rust driver at `cpp/conformance/pjson_conformance_driver.cpp`
sharing the conformance corpus.

**Affected ids** (all 167 — every C++ impl + C++ test cell is ❌):

```
AG-001 AG-002 AG-003 AG-004 AG-005 AG-006
AT-001 AT-002 AT-003 AT-004 AT-005 AT-006 AT-007 AT-008 AT-009 AT-010 AT-011 AT-012
B-001 B-002 B-003 B-004
BY-001 BY-002 BY-003 BY-004 BY-005 BY-006
C-001 C-002 C-003 C-004 C-005 C-006
CC-001 CC-002 CC-003 CC-004 CC-005 CC-006
D-001 D-002 D-003 D-004 D-005 D-006 D-007
DOC-001
E-001 E-002 E-003 E-004 E-005 E-006 E-007 E-008 E-009 E-010 E-011 E-012
EM-001 EM-002 EM-003 EM-004 EM-005 EM-006 EM-007 EM-008
EX-001 EX-002 EX-003 EX-004 EX-005 EX-006
F-001 F-002 F-003 F-004 F-005 F-006 F-007 F-008 F-009
H-001 H-002 H-003 H-004 H-005
J-001 J-002 J-003 J-004 J-005 J-006 J-007 J-008 J-009 J-010 J-011 J-012 J-013 J-014 J-015
LIM-001 LIM-002 LIM-003 LIM-004 LIM-005 LIM-006
N-001 N-002 N-003
NI-001 NI-002 NI-003 NI-004
NS-001 NS-002 NS-003 NS-004 NS-005 NS-006 NS-007
O-001 O-002 O-003 O-004 O-005 O-006 O-007
RA-001 RA-002 RA-003
S-001 S-002 S-003 S-004
T-001 T-002 T-003 T-004 T-005 T-006 T-007 T-008 T-009 T-010 T-011 T-012 T-013 T-014 T-015 T-016 T-017
U-001 U-002 U-003 U-004 U-005 U-006 U-007
UI-001 UI-002 UI-003
V-001 V-002 V-003
```

**Plan:** mirror `rust/psio/src/bin/pjson_conformance_driver.rs` in
C++ as the Phase-1-C++ deliverable. When that lands, the Phase-1
scalar-set ids (the same set listed under "Block B" below) will flip
to ✅ on C++ in the same commit, and they'll be removed from this
file.

---

## Block B — Phase 2+ scope on Rust (deliberately ❌ pending phase)

These rows are not Phase-1 scope and are deliberately ❌ on Rust
until the relevant phase lands. The driver returns
`Err(NotImplementedYet)` for them today, so a fixture targeting any
of these will fail loud rather than silently mishandle.

```
AG-001 AG-002 AG-003 AG-004 AG-005 AG-006
AT-001 AT-002 AT-003 AT-004 AT-005 AT-006 AT-007 AT-008 AT-009 AT-010 AT-011 AT-012
BY-001 BY-002 BY-003 BY-004 BY-005 BY-006
C-001 C-002 C-003 C-004 C-005 C-006
CC-001 CC-002 CC-003 CC-004 CC-005 CC-006
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
D-007
```

**Phase mapping:**

| prefix | phase | what unblocks it |
|--------|-------|------------------|
| AG-*, AT-*, O-*, RA-*, H-*, DOC-* | Phase 2 (containers) | array, object, slot tables, hash byte, long-key escape |
| NS-*, S-*, BY-*, J-*, EM-*, T-009..T-011 | Phase 3 (JSON-side) | numeric_string, string, bytes, JSON mappings, emitter options |
| EX-*, T-014, V-001 | Phase 4 (extensibility) | extension framework |
| C-*, F-008, D-007 | Phase 4/5 (canonical encoding) | canonical-form picker rules |
| F-004 | Phase 5 (binary128) | wire softfloat into Rust JSON-render |
| CC-* | gated on C++ driver | cross-validation harness |
| LIM-* | covered alongside the underlying types | (no separate work) |

---

## Block C — Phase 1 Rust ⚠️ rows (impl exists, dedicated test pending)

Phase 1's Rust driver implements these rules but doesn't have a
dedicated test that fails when the rule is violated. They will move
to ✅ in a follow-up commit before Phase 1 closes.

```
N-003 T-016 T-017 U-006 U-007 F-006 F-008 F-009 D-005 E-002 E-003 E-010 V-003
```

(F-008 / F-009 also appear under Block B's Phase-4/5 / canonical
plan — F-008 needs canonicalization beyond Phase 1's "round-trip
bits as-is.")

| id | gap | action |
|----|-----|--------|
| N-003 | no fixture for `null` with non-zero low nibble | add `null_low_nibble_set_rejected.json` |
| T-016 | predicate range tests not asserted | add unit test enumerating 0..255 tag space |
| T-017 | bit pattern within is_integer not asserted | same |
| U-006 | no fixture for truncated uint/negint | add `uint_truncated.json`, `negint_truncated.json` |
| U-007 | no fixture for u128-range value (bc 9..16) | add `uint_u128_max.json` |
| F-006 | widths 6, 7 covered by code path but no fixture | add `ieee_float_width_{6,7}_reserved.json` |
| F-008 | NaN canonicalization not implemented | Phase 5 |
| F-009 | no ±Inf round-trip fixture | add `f64_pos_inf.json`, `f64_neg_inf.json` |
| D-005 | no varscale-4-byte-form fixture | add fixture with very large scale |
| E-002 | reserved low-nibble bits per type — partial coverage | per-type as their phase lands |
| E-003 | bc out of range — impossible to express in 4-bit low nibble; defensive only | add fixture where tag claims bc=8 but body is 4 bytes |
| E-010 | varscale truncation tested; long-key varuint not yet | Phase 2 with object support |
| V-003 | predicate `(tag >> 4) >= 14` is implementation-property | add unit test asserting branch behavior |

These are tracked as small follow-ups; none block starting Phase 2 in
parallel.

---

## How to update this file

When a PR moves a row to ✅ on **both** language sides, delete its id
from every block above (and from any gap-table entry). When a PR
introduces a new spec rule with no test in the same change, add an
entry under Block C with rationale and concrete plan.
