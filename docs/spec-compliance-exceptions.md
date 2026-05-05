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
languages with all rule rows fully ✅ except F-008 (NaN
canonicalization, deferred to Phase 5) and the predicate properties
T-016/T-017/V-003 which are ✅ on Rust (unit-tested) and ⚠️ on C++
(true by construction in match dispatch but no dedicated C++ test
framework yet).

Tally as of this commit:
- 68 rows ✅ on **both** Rust and C++ (added F-004 binary128 — softfloat
  vendored at `cpp/external/softfloat/`, Rust port in driver).
- 3 rows ⚠️ on C++ only (T-016, T-017, V-003 — predicate properties
  true by construction, no C++ test framework yet).
- 1 row ⚠️ on both langs: F-008 (NaN canonicalization, Phase 5).
- 95 rows ❌ — Phase 2/3/4 scope, all enumerated below.

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

## Block B — Phase 1 ⚠️ rows (impl exists, dedicated test pending)

```
F-008 T-016 T-017 V-003
```

| id | gap | action |
|----|-----|--------|
| F-008 | NaN canonicalization (§15.2.1) not implemented — round-trips bits as-is rather than emitting canonical pattern | Phase 5 (canonical encoding rules) |
| T-016 | predicate range tests are tested in Rust (`tag_byte_predicates_match_spec_section_3`); C++ has no test framework yet — property holds by construction in match dispatch | add a Catch2 (or ad-hoc) test framework to `cpp/conformance/` and mirror the Rust test |
| T-017 | bit-pattern within `is_integer` — same as T-016 | same |
| V-003 | parse-fast-path predicate — same as T-016 | same |

These three (T-016, T-017, V-003) form a single "C++ unit test
framework" follow-up. None are wire-format issues; they're
implementation properties. Marking ⚠️ on C++ until tested explicitly.

---

## How to update this file

When a PR moves a row to ✅ on **both** language sides, delete its id
from the Block A or Block B list (and from any gap-table entry).

When a PR introduces a new spec rule with no test in the same change,
add an entry under Block B with rationale and concrete plan.
