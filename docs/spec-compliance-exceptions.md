# pjson v1 spec compliance — exceptions

This file accompanies `spec-compliance.md` and accounts for every row
currently marked ❌ or ⚠️. The CI gate (`tools/check-compliance.sh`)
fails if any matrix row is unaccounted for.

## How to interpret an entry

Each entry is keyed by rule id and carries:

- **status** — current marker (❌ unimplemented, ⚠️ untested).
- **reason** — why this row isn't ✅ today.
- **plan** — when and how this gap closes (which phase, which PR, which
  task id).

When a row moves to ✅, **delete its entry from this file** (don't keep
historical entries — git history is the audit trail for resolved
exceptions).

## Current exceptions

The matrix was created `2026-05-05` against the post-audit spec. Every
row is ❌ for both languages because the existing pjson implementation
predates the audit and is on a different wire format. The plan below
covers all of them as a block; individual entries will be split out as
they become independently work-tracked.

### Block exception: entire matrix is ❌ pending Phase 1+

**Affected ids** (every row currently in `spec-compliance.md`):

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

**Status:** ❌ for both C++ and Rust.

**Reason:** `pjson-spec.md` was rewritten on `2026-05-05` (commits
`ed43753`, `795734b`) following a tag-byte design audit. The wire
format changed for nine of the thirteen scalar/container codes plus
two additions (`numeric_string`, `extension`). The pre-audit
implementation (`cpp/include/psio/pjson*.hpp`,
`rust/psio/src/pjson*.rs`) compiled and tested correctly against the
old spec but no longer matches the new spec on any wire byte. Rather
than patch incrementally, the implementation is being rebuilt to the
new spec in phases.

**Plan:**

| Phase | scope (matrix prefixes covered) | status |
|---|---|---|
| 1 — wire foundation | `T-*`, `N-*`, `B-*`, `UI-*`, `NI-*`, `U-*`, `F-001..003`, `F-005..009`, `D-*` | not started |
| 2 — containers | `AG-*`, `AT-*`, `O-*`, `RA-*`, `H-*`, `DOC-*` | not started |
| 3 — JSON-side semantics | `NS-*`, `S-*`, `BY-*`, `J-*`, `EM-*` | not started |
| 4 — extensibility | `EX-*`, `V-*`, `C-*` | not started |
| 5 — completeness | `F-004` (binary128 — softfloat already vendored), `E-*` (validators), `CC-*` (cross-validation harness) | softfloat vendor done at `cpp/external/softfloat/`; wiring pending |

Each phase ends with the matrix updated (cells flipping ❌ → ✅) and
the corresponding entries removed from this file.

## Adding new exceptions

When a PR introduces a new spec rule that won't be implemented in the
same PR, add an entry under "Current exceptions" with:

```markdown
### EX-NNN — short name

**Status:** ❌

**Reason:** _why this isn't being shipped now_

**Plan:** _which phase / issue / target date_
```

When the rule id is in a multi-row block (`AT-001..AT-010` for typed
arrays, etc.), it's fine to cover them as a single entry like the
"Block exception" above — but the entry must enumerate the affected
range.
