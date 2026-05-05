#!/usr/bin/env bash
# tools/run-conformance.sh
#
# Runs every fixture under conformance/{fixtures,fixtures-reject}/
# through the implementations and reports any mismatch.
#
# Each language exposes a small driver binary that reads a fixture
# JSON on stdin and writes the result on stdout in a stable format
# the harness compares.
#
# Usage:
#   tools/run-conformance.sh                        # both langs + cross-validation
#   tools/run-conformance.sh --lang cpp             # C++ only
#   tools/run-conformance.sh --lang rust            # Rust only
#   tools/run-conformance.sh --filter '4.3_*'       # subset by glob

set -euo pipefail

readonly REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
readonly FIXTURES_DIR="${REPO_ROOT}/conformance/fixtures"
readonly REJECT_DIR="${REPO_ROOT}/conformance/fixtures-reject"

# Driver binaries — built by Phase 1.
readonly CPP_DRIVER="${REPO_ROOT}/cpp/build/conformance/pjson_conformance_driver"
readonly RUST_DRIVER="${REPO_ROOT}/rust/target/release/pjson_conformance_driver"

lang="both"
filter="*"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --lang)
            lang="$2"
            shift 2
            ;;
        --filter)
            filter="$2"
            shift 2
            ;;
        --help|-h)
            sed -n '/^# Usage/,/^$/p' "$0"
            exit 0
            ;;
        *)
            echo "unknown arg: $1" >&2
            exit 2
            ;;
    esac
done

run_one_lang() {
    local driver_path="$1"
    local lang_name="$2"

    if [[ ! -x "$driver_path" ]]; then
        echo "SKIP: ${lang_name} driver not built at ${driver_path}"
        case "$lang_name" in
            cpp)
                echo "  build with: cmake --build cpp/build --target pjson_conformance_driver"
                ;;
            rust)
                echo "  build with: cargo build --release -p psio --bin pjson_conformance_driver"
                ;;
        esac
        return 100
    fi

    local total=0 pass=0 fail=0
    while IFS= read -r -d '' fixture; do
        # shellcheck disable=SC2053
        [[ "$(basename "$fixture")" == $filter || "$(dirname "$fixture")" == *$filter* ]] || continue
        total=$((total + 1))
        if "$driver_path" --check < "$fixture" >/dev/null 2>"${fixture}.err"; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            echo "FAIL [${lang_name}]: $(realpath --relative-to="$REPO_ROOT" "$fixture")"
            sed 's/^/    /' "${fixture}.err"
        fi
        rm -f "${fixture}.err"
    done < <(find "$FIXTURES_DIR" "$REJECT_DIR" -name '*.json' -print0 2>/dev/null)

    echo "${lang_name}: ${pass}/${total} passed"
    [[ $fail -eq 0 ]]
}

cross_validate() {
    if [[ ! -x "$CPP_DRIVER" || ! -x "$RUST_DRIVER" ]]; then
        echo "SKIP: cross-validation requires both drivers"
        return 100
    fi
    local total=0 pass=0 fail=0
    while IFS= read -r -d '' fixture; do
        total=$((total + 1))
        # C++ encode → Rust decode; Rust encode → C++ decode.
        # The driver's --xvalidate mode emits the round-tripped bytes
        # so we can compare across implementations.
        local cpp_out rust_out
        cpp_out=$("$CPP_DRIVER"  --xvalidate < "$fixture" 2>/dev/null) || true
        rust_out=$("$RUST_DRIVER" --xvalidate < "$fixture" 2>/dev/null) || true
        if [[ "$cpp_out" == "$rust_out" && -n "$cpp_out" ]]; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            echo "XVALIDATE FAIL: $(realpath --relative-to="$REPO_ROOT" "$fixture")"
            diff <(echo "$cpp_out") <(echo "$rust_out") | sed 's/^/    /'
        fi
    done < <(find "$FIXTURES_DIR" -name '*.json' -print0 2>/dev/null)
    echo "cross-validation: ${pass}/${total} matched"
    [[ $fail -eq 0 ]]
}

# --- run ---------------------------------------------------------------------
exit_code=0
case "$lang" in
    cpp)
        run_one_lang "$CPP_DRIVER" cpp || exit_code=$?
        ;;
    rust)
        run_one_lang "$RUST_DRIVER" rust || exit_code=$?
        ;;
    both)
        run_one_lang "$CPP_DRIVER"  cpp  || exit_code=$?
        run_one_lang "$RUST_DRIVER" rust || exit_code=$?
        cross_validate                   || exit_code=$?
        ;;
    *)
        echo "unknown lang: $lang" >&2
        exit 2
        ;;
esac

# Exit 100 from a sub-step means "skipped because driver not built" — that's
# acceptable while Phase 1 implementations are in flight, but CI can choose
# to treat it as failure once Phase 1 is supposed to be complete.
exit $exit_code
