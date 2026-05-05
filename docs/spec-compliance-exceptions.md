# pjson v1 spec compliance — exceptions

This file accompanies `spec-compliance.md` and accounts for every row
currently marked ❌ or ⚠️. The CI gate (`tools/check-compliance.sh`)
fails if any matrix row is unaccounted for, so every entry below is
an explicit, documented decision rather than a silent gap.

A row appears here as long as **any** of its four cells (C++ impl,
C++ test, Rust impl, Rust test) is not ✅. When all four cells flip
to ✅, the id is removed from this file.

---

## Status

**Phase 1 is fully sealed. Phase 2.1 (generic array) and 2.2 (typed
homogeneous array) are also green** — all six AG-* and all twelve
AT-* rows are ✅ on both languages plus T-012 ✅.

Tally:
- 88 rows ✅ on **both** Rust and C++ (Phase 1 + 2.1 + 2.2).
- 5 rows ⚠️: F-008 (NaN canonicalization, Phase 5); AG-003/004/005 fixture
  follow-ups (impl ✅, no dedicated reject fixture); AT-012 partial
  fixture coverage.
- 79 rows ❌ — Phase 2.3/3/4 scope, all enumerated below.

---

## Block A — Phase 2+ scope on both langs (deliberately ❌ pending phase)

These rows are not Phase-1 scope and are deliberately ❌ on both
languages until the relevant phase lands. Both drivers return
`NotImplementedYet` errors for them today, so a fixture targeting
any of these will fail loud rather than silently mishandle.

```
BY-001 BY-002 BY-003 BY-004 BY-005 BY-006
C-001 C-002 C-003 C-004 C-005 C-006
D-007
DOC-001
E-006 E-007 E-008 E-009 E-010 E-011
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
E-002
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
| E-002 | per-type as their phase lands | covered by the type-specific phases |
| E-006..E-010 | Phase 2 (containers) | container error paths |

---

## Block B — ⚠️ rows (impl exists, dedicated test pending)

```
F-008 AG-003 AG-004 AG-005 T-012
```

| id | gap | action |
|----|-----|--------|
| F-008 | NaN canonicalization (§15.2.1) not implemented — round-trips bits as-is rather than emitting canonical pattern | Phase 5 (canonical encoding rules) |
| AG-003 | adaptive slot-width selection works in both impls (Rust unit test asserts u16-slot threshold) but no corpus fixture exercises u16/u24/u32 paths via cross-validation | add fixtures: array with 16+ u128 children → forces u16 slots; very large array → u24, etc. |
| AG-004 | slot-monotonicity reject path exists in code (`array slot offset OOB or non-monotonic`) but no fixture | add reject fixture with hand-crafted non-monotonic slots |
| AG-005 | slot-OOB reject path exists in code but no fixture | add reject fixture with slot offset > value_data_size |
| AT-012 | empty typed array fixture only exists for element_code 0 (i8); other 9 codes covered by code path but no per-code empty fixture | add 9 more empty-typed-array fixtures (cheap; ~1 minute of work) |

---

## How to update this file

When a PR moves a row to ✅ on **both** language sides, delete its id
from the Block A or Block B list (and from any gap-table entry).

When a PR introduces a new spec rule with no test in the same change,
add an entry under Block B with rationale and concrete plan.
