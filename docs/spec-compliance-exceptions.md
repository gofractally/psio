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

**Phase 1, all of Phase 2 (containers), all of Phase 3 (string,
bytes encoding hint, numeric_string, JSON ingress, emitter
options), Phase 4.1 (extension framework: T-014, V-001,
EX-001..006), and the Phase 2/3 ⚠️ punch-list (AG-003/004/005,
AT-012, H-005, O-004/006, EM-003/008) are sealed.** Cross-validation
between Rust and C++ is byte-equivalent across every fixture
(87/87 matches).

Tally:
- 152 rows ✅ on **both** Rust and C++.
- 4 rows ⚠️: F-008 (NaN canonicalization — Phase 4.2);
  RA-002 (random-access view fixture); RA-003 (encoder auto-detect
  array-of-object); NS-002 (no dual-projection API).
- 19 rows ❌ — Phase 4.2 canonical encoding (C-* + D-007), the
  J-* JSON-mapping bookkeeping rows (overlap with already-✅
  ingress code), LIM-* limits enforcement, and a few residual
  E-* error-class rows that overlap with already-✅ rejects.

---

## Block A — Phase 2.4+ scope on both langs (deliberately ❌)

These rows are not yet implemented. Both drivers return
`NotImplementedYet` errors for them today, so a fixture targeting
any of these will fail loud rather than silently mishandle.

```
C-001 C-002 C-003 C-004 C-005 C-006
D-007
E-006 E-007 E-008 E-009 E-010
J-001 J-002 J-003 J-004 J-005 J-006 J-007 J-010 J-011 J-012 J-013 J-014 J-015
LIM-001 LIM-002 LIM-003 LIM-004 LIM-005 LIM-006
E-002
```

**Phase mapping:**

| prefix | phase | what unblocks it |
|--------|-------|------------------|
| RA-*, T-013 partial | Phase 2.4 (row_array) | shared key block + per-record body layout |
| NS-*, S-*, BY-*, J-*, EM-*, T-009..T-011 | Phase 3 (JSON-side) | numeric_string, string, bytes, JSON mappings, emitter options |
| C-*, F-008, D-007 | Phase 4.2 / Phase 5 (canonical encoding) | canonical-form picker rules |
| LIM-* | covered alongside the underlying types | (no separate work) |
| E-002 | per-type as their phase lands | covered by the type-specific phases |
| E-006..E-009 | container error paths — partly covered (slot OOB / monotonicity) | dedicated reject fixtures pending |
| E-010 | varscale / long-key truncation — partly covered | dedicated reject fixtures pending |
| E-011 | numeric_string inner-tag rejection | Phase 3 |

---

## Block B — ⚠️ rows (impl exists, dedicated test pending)

```
F-008 RA-002 RA-003 NS-002
```

| id | gap | action |
|----|-----|--------|
| F-008 | NaN canonicalization (§15.2.1) not implemented — round-trips bits as-is rather than emitting canonical pattern | Phase 4.2 (canonical encoding rules) |
| RA-002 | random-access by `(record_index, key)` works through full decode but no dedicated random-access view fixture | add fixture that decodes a single (i, key) pair without full row materialization |
| RA-003 | encoder accepts row_array via DSL; auto-detection from `array of object` is Phase 3 (JSON-side encoder) | when JSON parser lands, add the homogeneity-detect pass per §5.2.1.5 |
| NS-002 | dedicated dual-projection API (`as_<numeric>()` + `as_string()`) | add API surface and unit test that demonstrates both projections from a single decoded value |