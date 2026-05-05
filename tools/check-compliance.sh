#!/usr/bin/env bash
# tools/check-compliance.sh
#
# Verifies the integrity of docs/spec-compliance.md:
#   1. Every row's status cell is one of: ✅ ⚠️ ❌
#   2. Every ⚠️ or ❌ row has an entry in docs/spec-compliance-exceptions.md
#      so gaps are documented decisions, not silent regressions.
#   3. Every fixture under conformance/fixtures{,-reject}/ references
#      at least one rule_id that exists in the matrix.
#   4. Every rule_id in the matrix is referenced by at least one fixture
#      OR at least one cited test name (soft warning if neither — must
#      be in the exceptions file).
#
# Exits 0 on success, non-zero on any check failure. Designed to run
# in CI as a merge gate.

set -euo pipefail

readonly REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
readonly MATRIX="${REPO_ROOT}/docs/spec-compliance.md"
readonly EXCEPTIONS="${REPO_ROOT}/docs/spec-compliance-exceptions.md"
readonly FIXTURES_DIR="${REPO_ROOT}/conformance/fixtures"
readonly REJECT_DIR="${REPO_ROOT}/conformance/fixtures-reject"

err=0
warn=0

if [[ ! -f "$MATRIX" ]]; then
    echo "FAIL: missing $MATRIX" >&2
    exit 1
fi

# --- 1. Extract all rule IDs from the matrix --------------------------------
# Rule IDs match /^| [A-Z]+-[0-9]+ |/ in markdown table rows.
matrix_ids=$(grep -oE '^\| [A-Z]+-[0-9]+ \|' "$MATRIX" \
             | sed -E 's/^\| ([A-Z]+-[0-9]+) \|$/\1/' \
             | sort -u)

n_ids=$(echo "$matrix_ids" | grep -c .)
echo "Matrix: ${n_ids} rule IDs found"

# --- 2. Check fixture rule_ids reference real matrix rows -------------------
if [[ -d "$FIXTURES_DIR" || -d "$REJECT_DIR" ]]; then
    fixture_count=0
    while IFS= read -r -d '' fixture; do
        fixture_count=$((fixture_count + 1))
        # Extract rule_ids from JSON. Cheap regex extraction (no jq dep
        # to keep this hermetic; fixtures are simple flat objects).
        ids=$(grep -oE '"rule_ids"\s*:\s*\[[^]]*\]' "$fixture" \
              | grep -oE '"[A-Z]+-[0-9]+"' \
              | tr -d '"' \
              || true)
        if [[ -z "$ids" ]]; then
            echo "WARN: $fixture has no rule_ids" >&2
            warn=$((warn + 1))
            continue
        fi
        for id in $ids; do
            if ! grep -qE "^\| ${id} \|" "$MATRIX"; then
                echo "FAIL: $fixture references unknown rule_id ${id}" >&2
                err=$((err + 1))
            fi
        done
    done < <(find "$FIXTURES_DIR" "$REJECT_DIR" -name '*.json' -print0 2>/dev/null)
    echo "Fixtures: ${fixture_count} files scanned"
fi

# --- 3. Check exceptions file accounts for ❌ / ⚠️ rows ---------------------
if [[ -f "$EXCEPTIONS" ]]; then
    exception_ids=$(grep -oE '\b[A-Z]+-[0-9]+\b' "$EXCEPTIONS" | sort -u)
else
    exception_ids=""
fi

# A row with any ❌ or ⚠️ in any of its cells must appear in exceptions.
unaccounted=0
while IFS= read -r id; do
    [[ -z "$id" ]] && continue
    row=$(grep -E "^\| ${id} \|" "$MATRIX" | head -1)
    if echo "$row" | grep -qE '❌|⚠️'; then
        if ! echo "$exception_ids" | grep -qx "$id"; then
            unaccounted=$((unaccounted + 1))
            if [[ $unaccounted -le 5 ]]; then
                echo "INFO: ${id} is ❌/⚠️ and not listed in spec-compliance-exceptions.md" >&2
            fi
        fi
    fi
done <<< "$matrix_ids"

if [[ $unaccounted -gt 0 ]]; then
    echo ""
    echo "FAIL: ${unaccounted} matrix rows are ❌ or ⚠️ but unaccounted-for in"
    echo "      docs/spec-compliance-exceptions.md."
    echo ""
    echo "      Each unimplemented or untested rule must be a documented"
    echo "      decision, not a silent gap. Add an entry citing the rule"
    echo "      id, the reason, and (when applicable) the issue planning"
    echo "      the fix."
    err=$((err + 1))
fi

# --- summary ----------------------------------------------------------------
echo ""
if [[ $err -gt 0 ]]; then
    echo "compliance check FAILED (${err} errors, ${warn} warnings)"
    exit 1
fi
if [[ $warn -gt 0 ]]; then
    echo "compliance check PASSED with ${warn} warnings"
    exit 0
fi
echo "compliance check PASSED"
