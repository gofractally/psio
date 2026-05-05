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

**All 167 rule rows in the compliance matrix are ✅ on both C++ and
Rust.** This file currently lists no exceptions. The pjson v1 spec
is fully implemented and tested across the cross-validating
implementations.

Cross-validation between Rust and C++ is byte-equivalent across
every fixture (105/105 matches at last green).

Phases sealed:
- Phase 1 — encoder/decoder spine, hash, varuint/varscale
- Phase 2 — generic_array, typed_array, object, row_array
- Phase 3 — string (raw_text + escape_form), bytes (4 hint codes),
  numeric_string lift, JSON ingress, §7.5 emitter options
- Phase 4.1 — extension framework (T-014, V-001, EX-001..006)
- Phase 4.2 — canonical encoding rules (C-001..006, F-008, D-007)
  including width minimization (binary16/32/64/128), NaN
  canonicalization, decimal-vs-ieee picker, strict-canonical
  validator
- §8 limits (LIM-001..006) including nesting depth cap (default 256)
- §11 errors — every reject path covered by either fixture or unit
  test in both languages

If a future spec rev adds new rules, add them here as ❌ rows with
the reason and a link to the issue tracking the work.
