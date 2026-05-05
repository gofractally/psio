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

**Phase 1 is fully sealed. Phases 2.1 (generic array), 2.2 (typed
array), 2.3 (object), and 2.4 (row_array) are all green.** All
container types of pjson v1 are now implemented end-to-end with
byte-equivalent cross-validation between Rust and C++.

Tally:
- 105 rows ✅ on **both** Rust and C++.
- 8 rows ⚠️: F-008 (Phase 5); AG-003/004/005 fixture follow-ups;
  AT-012 partial; H-005; O-004 partial; O-006; RA-002 partial;
  RA-003 partial.
- 60 rows ❌ — Phase 3/4 scope (JSON-side semantics + extensibility).

---

## Block A — Phase 2.4+ scope on both langs (deliberately ❌)

These rows are not yet implemented. Both drivers return
`NotImplementedYet` errors for them today, so a fixture targeting
any of these will fail loud rather than silently mishandle.

```
BY-001 BY-002 BY-003 BY-004 BY-005 BY-006
C-001 C-002 C-003 C-004 C-005 C-006
D-007
E-006 E-007 E-008 E-009 E-010 E-011
EM-001 EM-002 EM-003 EM-004 EM-005 EM-006 EM-007 EM-008
EX-001 EX-002 EX-003 EX-004 EX-005 EX-006
J-001 J-002 J-003 J-004 J-005 J-006 J-007 J-008 J-009 J-010 J-011 J-012 J-013 J-014 J-015
LIM-001 LIM-002 LIM-003 LIM-004 LIM-005 LIM-006
NS-001 NS-002 NS-003 NS-004 NS-005 NS-006 NS-007
S-001 S-002 S-003 S-004
T-009 T-010 T-011 T-014
V-001
E-002
```

**Phase mapping:**

| prefix | phase | what unblocks it |
|--------|-------|------------------|
| RA-*, T-013 partial | Phase 2.4 (row_array) | shared key block + per-record body layout |
| NS-*, S-*, BY-*, J-*, EM-*, T-009..T-011 | Phase 3 (JSON-side) | numeric_string, string, bytes, JSON mappings, emitter options |
| EX-*, T-014, V-001 | Phase 4 (extensibility) | extension framework |
| C-*, F-008, D-007 | Phase 4/5 (canonical encoding) | canonical-form picker rules |
| LIM-* | covered alongside the underlying types | (no separate work) |
| E-002 | per-type as their phase lands | covered by the type-specific phases |
| E-006..E-009 | container error paths — partly covered (slot OOB / monotonicity) | dedicated reject fixtures pending |
| E-010 | varscale / long-key truncation — partly covered | dedicated reject fixtures pending |
| E-011 | numeric_string inner-tag rejection | Phase 3 |

---

## Block B — ⚠️ rows (impl exists, dedicated test pending)

```
F-008 AG-003 AG-004 AG-005 AT-012 H-005 O-004 O-006 RA-002 RA-003
```

| id | gap | action |
|----|-----|--------|
| F-008 | NaN canonicalization (§15.2.1) not implemented — round-trips bits as-is rather than emitting canonical pattern | Phase 5 (canonical encoding rules) |
| AG-003 | adaptive slot-width selection works; corpus exercises u8 only via cross-validation; Rust unit test exercises u16 | add fixtures for u24, u32 paths |
| AG-004 | slot-monotonicity reject path exists but no fixture | add reject fixture with non-monotonic slots |
| AG-005 | slot-OOB reject path exists but no fixture | add reject fixture with slot offset > value_data_size |
| AT-012 | empty typed array fixture only exists for element_code 0 (i8) | add 9 more empty-typed-array fixtures |
| H-005 | encoder supports 4-byte varuint excess for very long keys; no fixture | add 64 KiB-key fixture |
| O-004 | adaptive slot width — exercised at u8 (single_field/multi_field) and u16 (long_key_255_escape); no u24/u32 fixtures | add larger-value_data fixtures |
| O-006 | hash-mismatch reject path exists; no dedicated reject fixture (would need hand-crafted bad hash) | add reject fixture |
| RA-002 | random-access by `(record_index, key)` works through full decode but no dedicated random-access view fixture | add fixture that decodes a single (i, key) pair without full row materialization |
| RA-003 | encoder accepts row_array via DSL; auto-detection from `array of object` is Phase 3 (JSON-side encoder) | when JSON parser lands, add the homogeneity-detect pass per §5.2.1.5 |