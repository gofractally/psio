# pjson v1 spec compliance matrix

This file is the load-bearing record of "does our implementation
actually ship the spec." Every normative item in `pjson-spec.md`
has a row here, and every row's cells must be one of:

| symbol | meaning |
|---|---|
| ✅ | implemented + has named test that exercises this rule |
| ⚠️ | implemented but no dedicated test (must not stay across a release) |
| ❌ | not implemented (must appear in `spec-compliance-exceptions.md` with a reason) |

## What "✅" means today vs the end-state

A ✅ in this matrix means **a spec-conformant reference implementation
exists and is tested**. Today many ✅ rows point at code in
`cpp/conformance/pjson_conformance_driver.cpp` and
`rust/psio/src/bin/pjson_conformance_driver.rs` — the conformance
drivers, which are intentionally self-contained reference impls.

**The end-state is that those checkmarks point at the public psio
library API** — the format-tagged CPOs from `docs/psio-overview.md`
§2.4 with policy parameterization:

```
// runtime policy (caller passes a value)
encode<F, T>(value, policy) -> bytes
decode<F, T>(bytes) -> T
validate<F, T>(bytes, policy) -> ok | error

// compile-time policy (zero-cost when policy is statically known)
encode<F, T, P>(value) -> bytes
validate<F, T, P>(bytes) -> ok | error
```

dispatched on the format tag `F` (`Pjson`, `Pssz`, `Fracpack`, …)
and the validation/encoding policy `P` (`DefaultSafe`,
`StrictCanonical`, `JustDontCrash`, or a runtime-built struct).
The conformance drivers become thin consumers that exercise the lib
through these CPOs.

The `<F, T, P>(bytes)` shape lets callers with a known policy at
compile time get monomorphized fast paths; the `<F, T>(bytes,
policy)` shape lets callers parameterize at runtime (CLI flags,
config). Both forms are part of the approved API.

### Library API status (pjson)

| component | reference driver | psio library | status |
|-----------|------------------|--------------|--------|
| `psio::pjson::encode(value)` | ✅ | ✅ live | matches §2.4 `encode<Pjson>` |
| `psio::pjson::decode(bytes)` | ✅ | ✅ live | matches §2.4 `decode<Pjson>` |
| `psio::pjson::validate(bytes)` | ✅ | ⚠️ live but **pre-audit tag layout** (UINT_INLINE=3 etc.); doesn't match the audited spec the conformance driver implements (UINT_INLINE=2 etc.) | wire-format spec-rev reconciliation pending |
| `psio::ValidationPolicy` trait + `DefaultSafe` / `StrictCanonical` / `JustDontCrash` / `DynamicPolicy` preset types | n/a | ✅ live at `psio::{ValidationPolicy, ...}` | format-agnostic, doesn't depend on spec rev |
| Format-tagged CPO dispatch (`encode<F, T>` / `decode<F, T>` / `validate<F, T, P>` at crate root) | n/a | ✅ live at `psio::{encode, decode, validate}` with `Format` trait + `Encode<F>` / `Decode<F>` / `Validate<F>` blanket impls | machinery wired; per-format bodies use whatever wire layout the format module implements |
| `psio::pjson::format::Pjson` format tag | n/a | ✅ live; CPO works end-to-end (`crate_root_cpo_encodes_validates_pjson` test) | uses pre-audit lib bodies, see above |
| Canonicalizing encoder (C-002, F-008, etc.) | ✅ in driver | ❌ pending kernel migration | reference-only |
| Strict-canonical validator (C-006) | ✅ in driver as `validate_canonical` | ❌ pending kernel migration; will live as the `P = StrictCanonical` body of `validate<Pjson, T, P>` | reference-only |
| Width minimization helpers (`canonical_float_width`, `f64_to_f16_exact`, `f128_bits_to_f64_exact`) | ✅ in driver | ❌ pending kernel migration | reference-only |
| Decimal-vs-ieee picker (D-007) | ✅ in driver (JSON ingress) | ❌ pending kernel migration | reference-only |
| NaN canonicalization (`canonicalize_nan_bits`) | ✅ in driver | ❌ pending kernel migration | reference-only |
| `decimal_to_f64_exact` (D-007 internals) | ✅ in driver | ❌ pending kernel migration | reference-only |
| Dual-projection (NS-002): `numeric_string_as_numeric` / `_as_string` | ✅ helpers in driver, unit-tested | ❌ not in lib | reference-only |
| Random-access (RA-002): `row_array_get` | ✅ helper in driver, unit-tested | ❌ not in lib | reference-only |
| C++ counterpart of the lib API | ✅ in `cpp/conformance/...` | ❌ pending kernel migration | reference-only |

The "reference-only" rows have spec-conformant implementations
that real downstream callers can't link against. The migration plan
in dependency order:

1. ~~**Define the `ValidationPolicy` trait + preset types**~~ — done.
   `psio::ValidationPolicy` trait + `JustDontCrash` / `DefaultSafe` /
   `StrictCanonical` / `DynamicPolicy` types live at the crate root.
   Both call shapes from §2.4 work via a single trait (compile-time
   ZST presets, runtime `DynamicPolicy`).

2. ~~**Land the `Format` trait + format-tagged CPO dispatch**~~ — done.
   `psio::Format` trait + `Encode<F>` / `Decode<F>` / `Validate<F>`
   per-type traits + crate-root `psio::{encode, decode, validate}`
   functions. `psio::pjson::format::Pjson` is the first format tag;
   `crate_root_cpo_encodes_validates_pjson` test exercises the full
   round-trip + both validate shapes through the CPO.

3. **Reconcile the `psio::pjson` lib with the audited spec.** The
   pre-audit lib uses tag codes `UINT_INLINE=3`, `DECIMAL=5`,
   `NEGINT=7`, …; the audited spec (and the conformance driver) uses
   `UINT_INLINE=2`, `NEGINT=5`, `DECIMAL=7`, …. The CPO machinery
   doesn't care which the lib implements, but the conformance corpus
   bytes only match the audited layout. Either:
     - Update the lib's tag constants and re-encoding paths to match
       the audited spec (preferred — biggest single payoff), or
     - Add a parallel `psio::pjson_v1` module that implements the
       audited spec, leave `psio::pjson` for legacy callers.
   This is a substantial chunk that touches every encoder/decoder
   path in the lib and most of its existing 670+ tests.

4. **Migrate the canonical-encoder kernel** (NaN canonicalization,
   width minimization, decimal-vs-ieee picker, slot-width
   minimization, mantissa trim) from the conformance drivers into
   the spec-correct `psio::pjson` module. Wire it into the
   `P: ValidationPolicy where verify_canonical()` branch of
   `Validate<Pjson>::validate` and (analogous) `Encode<Pjson>`.

5. **Refactor the conformance drivers** to call the lib CPOs instead
   of their parallel implementations. Net-large code deletion in
   `bin/pjson_conformance_driver.rs` and `cpp/conformance/...`.

6. **Mirror in C++** — same shape: `<psio/format.h>` for the format
   tag, `<psio/policy.h>` for `ValidationPolicy`, `<psio/pjson.h>`
   for the per-format impl matching the audited spec.

## How this file is enforced

- `tools/check-compliance.sh` parses this file and verifies that every
  row's cited test name actually exists in the relevant test suite.
- `tools/run-conformance.sh` runs every fixture under `conformance/`
  through both implementations and compares wire bytes byte-exact.
- CI fails on any ⚠️ or ❌ that does not have a corresponding entry in
  `docs/spec-compliance-exceptions.md` with a reason and (if applicable)
  a tracked issue.

These three gates together make it impossible to land "I implemented
the feature but skipped the test" or "I added the test but it doesn't
actually exercise the feature."

## How to update this file

When a PR adds or modifies functionality:

1. Identify the rows it touches.
2. Update the cells (❌ → ✅ when an implementation lands; remove from
   exceptions file if previously deferred).
3. Add the row(s) to the PR description checklist.
4. Reviewer confirms the cited test actually exercises the row's rule.

When a PR adds a new spec rule (after a `pjson-spec.md` change):

1. Add a new row here, marked ❌ for both langs.
2. Add an entry to `spec-compliance-exceptions.md` with rationale.
3. Schedule the implementation work.

---

## Legend for "Test name" columns

The cited test names live in:
- C++: `cpp/tests/pjson_tests.cpp` (and adjacent files), test name = `TEST_CASE("...")` argument.
- Rust: `rust/psio/src/pjson*.rs` `#[cfg(test)]` modules, test name = `#[test] fn ...` name.

Tests cited in multiple rows are fine — one test can exercise several rules.

---

## §3 — Tag byte structure

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| T-001 | code 0 = `null`, low nibble must be 0 | ✅ | ✅ corpus `null_basic` | ✅ | ✅ corpus `null_basic` |
| T-002 | code 1 = `bool`, low nibble ∈ {0, 1}, others reserved | ✅ | ✅ corpus `bool_{true,false}` | ✅ | ✅ `bool_round_trip` |
| T-003 | code 2 = `uint_inline`, low nibble = value 0..15 | ✅ | ✅ corpus `uint_inline_{0,5,15}` | ✅ | ✅ `uint_inline_round_trip` |
| T-004 | code 3 = `nint_inline`, low nibble 1..15 → value −1..−15; low_nibble 0 reserved | ✅ | ✅ corpus `nint_inline_{minus_1,minus_15}` + `nint_inline_zero_reserved` | ✅ | ✅ `nint_inline_round_trip` + `nint_inline_zero_rejected` |
| T-005 | code 4 = `uint`, low nibble = bc−1 ∈ 0..15 (bc 1..16) | ✅ | ✅ corpus `uint_{16,256,u64_max}` | ✅ | ✅ `uint_full_round_trip` |
| T-006 | code 5 = `negint`, low nibble = bc−1; all-zero payload reserved | ✅ | ✅ corpus `negint_minus_16` + `negint_zero_payload_reserved` | ✅ | ✅ `negint_full_round_trip` + `negint_zero_rejected` |
| T-007 | code 6 = `ieee_float`, low nibble bits 2..0 = log₂(byte_count) ∈ {1,2,3,4}; bit 3 reserved | ✅ | ✅ corpus `ieee_float_{f16,f32,f64}_one` + bit3 + width0 reject | ✅ | ✅ `ieee_float_round_trip` + `ieee_float_reserved_bit3_rejected` + `ieee_float_bad_width_rejected` |
| T-008 | code 7 = `decimal`, low nibble = mantissa bc−1 | ✅ | ✅ corpus `decimal_{one_point_five,12345}` | ✅ | ✅ `decimal_round_trip` |
| T-009 | code 8 = `numeric_string`, low nibble must be 0; body is inner numeric value | ✅ | ✅ corpus `numeric_string_*` | ✅ | ✅ same |
| T-010 | code 9 = `string`, low nibble ∈ {0, 1} = encoding flag; others reserved | ✅ | ✅ corpus `string_*` | ✅ | ✅ same |
| T-011 | code 10 = `bytes`, low nibble ∈ {0,1,2,3} = JSON-emit hint; 4..15 reserved | ✅ | ✅ corpus `bytes_*` | ✅ | ✅ same |
| T-012 | code 11 = `array`, low nibble ∈ {0, 1..10}; 11..15 reserved | ✅ | ✅ corpus `array_*` + `typed_array_*` + `typed_array_low_nibble_11_reserved` | ✅ | ✅ same |
| T-013 | code 12 = `object`, low nibble ∈ {0, 1}; 2..15 reserved | ✅ | ✅ corpus `object_*` + `row_array_*` + `object_low_nibble_reserved` | ✅ | ✅ same |
| T-014 | code 13 = `extension`, low nibble = sub-type id 0..15 | ✅ | ✅ corpus `extension_subtype_{0_empty,5_small,15_max}` | ✅ | ✅ same |
| T-015 | codes 14, 15 reserved → reject | ✅ | ✅ corpus `reserved_code_14_rejected` | ✅ | ✅ `reserved_codes_rejected` + corpus `reserved_code_14_rejected` |
| T-016 | predicates (is_atom, is_integer, is_real, is_numeric_value, is_number_projectable, is_json_string_emit, is_aggregate, is_extension, is_reserved) collapse to range tests | ✅ | ✅ `--self-test` runtime check | ✅ | ✅ `tag_byte_predicates_match_spec_section_3` + `--self-test` |
| T-017 | within is_integer: `bit 0 = sign`, `bit 1 = inline form` | ✅ | ✅ `--self-test` runtime check | ✅ | ✅ `tag_byte_predicates_match_spec_section_3` + `--self-test` |

## §4.1 — `null`

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| N-001 | encode `null` → `0x00`, container size = 1 | ✅ | ✅ corpus `null_basic` | ✅ | ✅ `null_round_trip` + corpus `null_basic` |
| N-002 | decode `0x00` (size 1) → `null` | ✅ | ✅ corpus `null_basic` | ✅ | ✅ `null_round_trip` + corpus `null_basic` |
| N-003 | reject `0x0X` for X ≠ 0 | ✅ | ✅ corpus `null_low_nibble_set_rejected` | ✅ | ✅ corpus `null_low_nibble_set_rejected` |

## §4.2 — `bool`

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| B-001 | encode `false` → `0x10` | ✅ | ✅ corpus `bool_false` | ✅ | ✅ `bool_round_trip` + corpus `bool_false` |
| B-002 | encode `true` → `0x11` | ✅ | ✅ corpus `bool_true` | ✅ | ✅ `bool_round_trip` + corpus `bool_true` |
| B-003 | decode `0x10` → `false`, `0x11` → `true` | ✅ | ✅ corpus `bool_{true,false}` | ✅ | ✅ `bool_round_trip` |
| B-004 | reject `0x12..0x1F` | ✅ | ✅ corpus `bool_low_nibble_reserved` | ✅ | ✅ `bool_round_trip` + corpus `bool_low_nibble_reserved` |

## §4.3 — `uint_inline` (code 2)

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| UI-001 | encode 0..15 → `0x20..0x2F` | ✅ | ✅ corpus `uint_inline_{0,5,15}` | ✅ | ✅ `uint_inline_round_trip` + corpus `uint_inline_{0,5,15}` |
| UI-002 | decode `0x20..0x2F` → 0..15 | ✅ | ✅ corpus `uint_inline_{0,5,15}` | ✅ | ✅ `uint_inline_round_trip` |
| UI-003 | encoder picks `uint_inline` over `uint` for 0..15 | ✅ | ✅ corpus `uint_inline_5` | ✅ | ✅ corpus `uint_inline_5` (encodes 5 as 0x25, not 0x40 0x05) |

## §4.4 — `nint_inline` (code 3)

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| NI-001 | encode −1..−15 → `0x31..0x3F` (low nibble = magnitude) | ✅ | ✅ corpus `nint_inline_{minus_1, minus_15}` | ✅ | ✅ `nint_inline_round_trip` + corpus `nint_inline_{minus_1, minus_15}` |
| NI-002 | decode `0x31..0x3F` → −1..−15 | ✅ | ✅ corpus `nint_inline_{minus_1, minus_15}` | ✅ | ✅ `nint_inline_round_trip` |
| NI-003 | reject `0x30` (negative-zero reserved) | ✅ | ✅ corpus `nint_inline_zero_reserved` | ✅ | ✅ `nint_inline_zero_rejected` + corpus `nint_inline_zero_reserved` |
| NI-004 | encoder picks `nint_inline` over `negint` for −1..−15 | ✅ | ✅ corpus `nint_inline_minus_1` | ✅ | ✅ corpus `nint_inline_minus_1` |

## §4.5 — `uint` and `negint`

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| U-001 | encode `uint` magnitude n with smallest bc 1..16, payload raw LE | ✅ | ✅ corpus `uint_{16,256,u64_max}` | ✅ | ✅ `uint_full_round_trip` + corpus `uint_{16,256,u64_max}` |
| U-002 | decode `uint` of any bc 1..16 | ✅ | ✅ corpus `uint_{16,256,u64_max}` | ✅ | ✅ `uint_full_round_trip` |
| U-003 | encode `negint` magnitude with smallest bc, payload raw LE | ✅ | ✅ corpus `negint_minus_16` | ✅ | ✅ `negint_full_round_trip` + corpus `negint_minus_16` |
| U-004 | decode `negint`, value = −payload | ✅ | ✅ corpus `negint_minus_16` | ✅ | ✅ `negint_full_round_trip` |
| U-005 | reject `negint` with all-zero payload | ✅ | ✅ corpus `negint_zero_payload_reserved` | ✅ | ✅ `negint_zero_rejected` + corpus `negint_zero_payload_reserved` |
| U-006 | reject `uint`/`negint` with bc declared but truncated buffer | ✅ | ✅ corpus `uint_truncated_payload` + `negint_truncated_payload` | ✅ | ✅ corpus `uint_truncated_payload` + `negint_truncated_payload` |
| U-007 | bc 9..16 reaches u128 / i128 range | ✅ | ✅ corpus `uint_u128_max` (bc=16) | ✅ | ✅ corpus `uint_u128_max` (bc=16) |

## §4.6 — `ieee_float`

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| F-001 | binary16 round-trip (low_nibble 1, payload 2 bytes LE) | ✅ | ✅ corpus `ieee_float_f16_one` | ✅ | ✅ `ieee_float_round_trip` + corpus `ieee_float_f16_one` |
| F-002 | binary32 round-trip (low_nibble 2, payload 4 bytes LE) | ✅ | ✅ corpus `ieee_float_f32_one` | ✅ | ✅ `ieee_float_round_trip` + corpus `ieee_float_f32_one` |
| F-003 | binary64 round-trip (low_nibble 3, payload 8 bytes LE) | ✅ | ✅ corpus `ieee_float_f64_one` | ✅ | ✅ `ieee_float_round_trip` + corpus `ieee_float_f64_one` |
| F-004 | binary128 round-trip (low_nibble 4, payload 16 bytes LE) — softfloat widen on decode | ✅ via vendored `psio_softfloat.h` | ✅ corpus `ieee_float_f128_{one,minus_one,pos_inf}` | ✅ via Rust port of softfloat (`f128_bits_to_f64`) | ✅ `f128_widen_canonical_values` + `f128_widen_overflow_to_inf` + `f128_widen_underflow_to_zero` + corpus fixtures |
| F-005 | reject low_nibble bit 3 set | ✅ | ✅ corpus `ieee_float_bit3_set_reserved` | ✅ | ✅ `ieee_float_reserved_bit3_rejected` + corpus `ieee_float_bit3_set_reserved` |
| F-006 | reject low_nibble width selector ∈ {0, 5, 6, 7} | ✅ | ✅ corpus `ieee_float_width_{zero,6,7}_reserved` (widths 0, 6, 7); width 5 by code path | ✅ | ✅ `ieee_float_bad_width_rejected` (widths 0, 5) + corpus widths 0, 6, 7 |
| F-007 | binary16 software widen on decode (no host fp16 support assumed) | ✅ | ✅ corpus `ieee_float_f16_one` renders `1` via `f16_bits_to_f64` | ✅ | ✅ corpus `ieee_float_f16_one` renders JSON `1` via `f16_bits_to_f64` |
| F-008 | NaN at any width preserves quiet-NaN canonical pattern (§15.2.1) | ✅ | ✅ corpus `canonical_nan_{f16,f32,f64,f128}` | ✅ | ✅ same + Rust unit `nan_canonicalized_on_encode` |
| F-009 | ±Inf round-trip at every width | ✅ | ✅ corpus `ieee_float_f64_{pos,neg}_inf` | ✅ | ✅ corpus `ieee_float_f64_{pos,neg}_inf` |

## §4.7 — `decimal` and varscale

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| D-001 | decimal(m, s) → `0x70..0x7F` tag, zigzag mantissa LE, varscale | ✅ | ✅ corpus `decimal_{one_point_five, 12345}` | ✅ | ✅ `decimal_round_trip` + corpus `decimal_{one_point_five, 12345}` |
| D-002 | varscale 1-byte form (scale ∈ −32..31) | ✅ | ✅ corpus `decimal_one_point_five` (scale −1) | ✅ | ✅ `varscale_encode_decode_round_trip` + corpus `decimal_one_point_five` (scale −1) |
| D-003 | varscale 2-byte form (scale ∈ −8192..8191) | ✅ | ✅ corpus `decimal_large_scale_2byte` (scale 33) | ✅ | ✅ `varscale_encode_decode_round_trip` + corpus `decimal_large_scale_2byte` |
| D-004 | varscale 3-byte form | ✅ | ✅ corpus `decimal_large_scale_3byte` (scale 8192) | ✅ | ✅ `varscale_encode_decode_round_trip` + corpus `decimal_large_scale_3byte` |
| D-005 | varscale 4-byte form | ✅ | ✅ corpus `decimal_large_scale_4byte` (scale 2097152) | ✅ | ✅ corpus `decimal_large_scale_4byte` |
| D-006 | varscale: encoder uses smallest byte count that fits | ✅ | ✅ corpus tests covers all four tiers | ✅ | ✅ `varscale_encode_decode_round_trip` (transitions at 32, 8192, etc.) |
| D-007 | decimal-vs-ieee_float encoder rule (§4.7.2): pick decimal when strictly shorter, else smallest bit-exact ieee width | ✅ via `decimal_or_ieee_pick` on JSON ingress | ✅ corpus `json_ingress_fractional_picker` (1.5 → ieee f16 on tie) + `json_ingress_fractional_picks_decimal` (0.1 → decimal, ieee can't represent) | ✅ same | ✅ same |

## §4.8 — `numeric_string`

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| NS-001 | encode wraps a numeric inner value at code 8 (low nibble = 0) | ✅ | ✅ corpus `numeric_string_*` | ✅ | ✅ `numeric_string_round_trip` + corpus |
| NS-002 | decode exposes both `as_<numeric>()` and `as_string()` projections | ✅ via `numeric_string_as_numeric` / `numeric_string_as_string` helpers | ✅ Rust unit `numeric_string_dual_projection` | ✅ same | ✅ same |
| NS-003 | encoder lifts JSON string when grammar matches AND `canonical_decimal(parse(s)) == s`, regardless of byte savings | ✅ via `from_json` + `parse_canonical_json_number_string` | ✅ corpus `lift_canonical_int` + `lift_canonical_decimal` | ✅ same | ✅ `json_ingress_numeric_string_lift_rule` + corpus |
| NS-004 | encoder leaves non-canonical numeric strings (`"01"`, `"1.5e10"`) as plain `string` | ✅ canonical-form check rejects non-canonical | ✅ corpus `no_lift_leading_zero` + `no_lift_sci_notation` | ✅ same | ✅ same |
| NS-005 | reject inner tag whose code is not in {2..7} | ✅ | ✅ corpus `numeric_string_inner_bool_rejected` + `numeric_string_inner_string_rejected` | ✅ | ✅ `numeric_string_round_trip` + corpus |
| NS-006 | reject low_nibble ≠ 0 | ✅ | ✅ corpus `numeric_string_low_nibble_reserved` | ✅ | ✅ `numeric_string_round_trip` + corpus |
| NS-007 | JSON emit always quoted regardless of `int_string_mode` (§7.5) | ✅ | ✅ corpus `emitter_numeric_string_unaffected_by_mode` | ✅ | ✅ same |

## §4.9 — `string`

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| S-001 | `raw_text` encode (low_nibble 0): JSON emit runs per-char escape pass | ✅ | ✅ corpus `string_raw_text_hello` + `string_raw_text_with_quote` | ✅ | ✅ `string_round_trip` + corpus |
| S-002 | `escape_form` encode (low_nibble 1): JSON emit just wraps in quotes | ✅ | ✅ corpus `string_escape_form_hello` | ✅ | ✅ `string_round_trip` + corpus |
| S-003 | reject low_nibble ∈ 2..15 | ✅ | ✅ corpus `string_low_nibble_reserved` | ✅ | ✅ `string_round_trip` + corpus |
| S-004 | length is `size − 1` (no in-value length prefix) | ✅ | ✅ implicit in every string round-trip (size from container) | ✅ | ✅ same |

## §4.10 — `bytes` with encoding hint

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| BY-001 | encoding hint 0 = base64; JSON emit produces standard base64 | ✅ | ✅ corpus `bytes_base64_deadbeef` | ✅ | ✅ `bytes_round_trip_all_hints` + corpus |
| BY-002 | encoding hint 1 = hex; JSON emit produces lowercase hex | ✅ | ✅ corpus `bytes_hex_deadbeef` | ✅ | ✅ same |
| BY-003 | encoding hint 2 = base58; JSON emit produces Bitcoin-alphabet base58 | ✅ | ✅ corpus `bytes_base58_deadbeef` | ✅ | ✅ same |
| BY-004 | encoding hint 3 = base64url; JSON emit produces URL-safe base64 no padding | ✅ | ✅ corpus `bytes_base64url_deadbeef` | ✅ | ✅ same |
| BY-005 | reject low_nibble ∈ 4..15 | ✅ | ✅ corpus `bytes_low_nibble_reserved` | ✅ | ✅ same |
| BY-006 | wire bytes are raw octets regardless of hint | ✅ | ✅ all four hint fixtures share the same payload bytes (`deadbeef`) | ✅ | ✅ same |

## §4.11 — `extension`

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| EX-001 | encode `extension(subtype N, body)` → tag `0xD0 \| N`, body bytes | ✅ | ✅ corpus `extension_subtype_{0_empty,5_small,15_max}` | ✅ | ✅ same |
| EX-002 | decode preserves subtype and body bytes verbatim | ✅ | ✅ corpus `extension_subtype_*` | ✅ | ✅ same |
| EX-003 | unknown sub-type id: parser does not error; surfaces as `Extension(id, bytes)` to caller | ✅ | ✅ corpus `extension_subtype_5_small` (no handler registered, decoded as opaque) | ✅ | ✅ same + `extension_round_trip_and_envelope` |
| EX-004 | extension as object child: enclosing key-lookup works without sub-type handler | ✅ | ✅ corpus `extension_inside_object` | ✅ | ✅ same |
| EX-005 | JSON emit of unknown extension produces `{"__pjson_ext": {...}}` envelope | ✅ | ✅ corpus `extension_subtype_*` (json_compact field) | ✅ | ✅ same + `extension_round_trip_and_envelope` |
| EX-006 | round-trip envelope: emit → parse → re-encode preserves subtype + bytes | ✅ | ✅ corpus `json_ingress_extension_envelope` | ✅ | ✅ same |

## §5 — Containers

### §5.1 — Generic array

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| AG-001 | encode generic array: tag `0xB0`, width byte, value_data, slot table, count u16 LE | ✅ | ✅ corpus `array_empty` + `array_single_uint_inline` + `array_heterogeneous` + `array_nested` | ✅ | ✅ `generic_array_round_trip` + corpus |
| AG-002 | decode: locate slot table from tail, walk forward into value_data | ✅ | ✅ corpus `array_*` round-trip | ✅ | ✅ `generic_array_round_trip` |
| AG-003 | adaptive slot width selection: u8 / u16 / u24 / u32 per value_data size | ✅ | ✅ corpus u8 + Rust unit test `array_adaptive_slot_width` (u16) + `array_adaptive_slot_width_u24_u32` (u24/u32) | ✅ | ✅ `array_adaptive_slot_width{,_u24_u32}` |
| AG-004 | reject slot offsets that violate monotonicity | ✅ via decode bounds check | ✅ corpus `array_slot_non_monotonic_rejected` | ✅ via decode bounds check | ✅ corpus `array_slot_non_monotonic_rejected` |
| AG-005 | reject slot offset ≥ value_data_size | ✅ | ✅ corpus `array_slot_oob_rejected` | ✅ | ✅ corpus `array_slot_oob_rejected` |
| AG-006 | empty array (N=0) round-trip | ✅ | ✅ corpus `array_empty` | ✅ | ✅ `generic_array_round_trip` + corpus |

### §5.1.1 — Typed homogeneous array

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| AT-001 | element code 0 (i8): tag `0xB1`, raw u8 elements | ✅ | ✅ corpus `typed_array_i8_min_zero_max` | ✅ | ✅ corpus |
| AT-002 | element code 1 (i16): tag `0xB2`, raw 2-byte LE elements | ✅ | ✅ corpus `typed_array_i16_minus_zero_plus` | ✅ | ✅ corpus |
| AT-003 | element code 2 (i32): tag `0xB3` | ✅ | ✅ corpus `typed_array_i32_minus_zero_plus` | ✅ | ✅ corpus |
| AT-004 | element code 3 (i64): tag `0xB4` | ✅ | ✅ corpus `typed_array_i64_two` | ✅ | ✅ corpus |
| AT-005 | element code 4 (u8): tag `0xB5` | ✅ | ✅ corpus `typed_array_u8_zero_one_max` | ✅ | ✅ corpus |
| AT-006 | element code 5 (u16): tag `0xB6` | ✅ | ✅ corpus `typed_array_u16_zero_max` | ✅ | ✅ corpus |
| AT-007 | element code 6 (u32): tag `0xB7` | ✅ | ✅ corpus `typed_array_u32_one_to_four` | ✅ | ✅ corpus |
| AT-008 | element code 7 (u64): tag `0xB8` | ✅ | ✅ corpus `typed_array_u64_one` | ✅ | ✅ corpus |
| AT-009 | element code 8 (f32): tag `0xB9` | ✅ | ✅ corpus `typed_array_f32_one_two` | ✅ | ✅ corpus |
| AT-010 | element code 9 (f64): tag `0xBA` | ✅ | ✅ corpus `typed_array_f64_one` | ✅ | ✅ corpus |
| AT-011 | reject low_nibble 11..15 | ✅ | ✅ corpus `typed_array_low_nibble_11_reserved` | ✅ | ✅ corpus |
| AT-012 | empty typed array (N=0) at every element code | ✅ | ✅ corpus `typed_array_empty_{i8,i16,i32,i64,u8,u16,u32,u64,f32,f64}` | ✅ | ✅ same |

### §5.2 — Object (single)

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| O-001 | encode object: tag `0xC0`, value_data, hash[N], slot[N], count u16 LE | ✅ | ✅ corpus `object_*` | ✅ | ✅ `object_round_trip` + corpus |
| O-002 | hash byte = `key_hash8(key)` for each entry (XXH3_64 low byte after suffix strip) | ✅ via XXH3_64bits + rfind('.') | ✅ corpus | ✅ via xxhash-rust + rfind | ✅ `key_hash8_strips_trailing_dot_suffix` + corpus |
| O-003 | suffix strip: `"foo.b64"` and `"foo"` hash to same byte | ✅ | ✅ corpus `object_suffix_strip_collision` | ✅ | ✅ `key_hash8_strips_trailing_dot_suffix` + corpus |
| O-004 | adaptive slot width per value_data size | ✅ | ✅ corpus u8/u16 + Rust unit test `object_long_key_64kib_round_trip` exercises u24/u32 path | ✅ | ✅ same |
| O-005 | empty object (N=0) round-trip | ✅ | ✅ corpus `object_empty` | ✅ | ✅ `object_round_trip` + corpus |
| O-006 | reject hash[i] ≠ key_hash8(stored_key_i) | ✅ via decode hash check | ✅ corpus `object_hash_mismatch_rejected` | ✅ | ✅ corpus `object_hash_mismatch_rejected` |
| O-007 | reject low_nibble ∈ 2..15 | ✅ | ✅ corpus `object_low_nibble_reserved` | ✅ | ✅ corpus |

### §5.2.1 — Row-array

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| RA-001 | encode row_array: tag `0xC1`, shared key block, per-record body | ✅ | ✅ corpus `row_array_*` | ✅ | ✅ `row_array_round_trip` + corpus |
| RA-002 | decode random-access by `(record_index, key)` | ✅ via `row_array_get(v, i, key)` accessor | ✅ Rust unit `row_array_random_access_by_record_and_key` | ✅ same | ✅ same |
| RA-003 | encoder rule: pick row_array when wire size beats N×generic-object | ✅ via `detect_row_array_shape` on JSON ingress | ✅ corpus `json_ingress_array_of_object_lifts_to_row_array` (homogeneous-shape detection in both ingresses) | ✅ same | ✅ same |

### §5.3–5.4 — Hash & long-key escape

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| H-001 | hash collision behavior: lookup verifies stored key, advances on mismatch | ✅ via decode hash verify | ✅ corpus `object_suffix_strip_collision` (round-trip with two same-hash entries) | ✅ | ✅ corpus + `object_round_trip` |
| H-002 | long-key escape: `key_size_byte = 0xFF`, varuint excess inline in entry | ✅ | ✅ corpus `object_long_key_255_escape` | ✅ | ✅ `object_long_key_round_trip` + corpus |
| H-003 | round-trip key of exactly 254 bytes (no escape) | ✅ | ✅ corpus `object_long_key_254` | ✅ | ✅ `object_long_key_round_trip` + corpus |
| H-004 | round-trip key of exactly 255 bytes (escape, excess = 0) | ✅ | ✅ corpus `object_long_key_255_escape` | ✅ | ✅ `object_long_key_round_trip` + corpus |
| H-005 | round-trip key of 64 KiB (4-byte varuint excess) | ✅ | ✅ Rust unit test `object_long_key_64kib_round_trip` (cross-validates against C++ via wire bytes) | ✅ | ✅ `object_long_key_64kib_round_trip` |

## §6 — Document level

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| DOC-001 | top-level value is a single pjson value; size from caller | ✅ | ✅ corpus harness exercises this for every fixture | ✅ | ✅ same |

## §7 — JSON round-trip

### §7.1–7.4 — Mappings & policy

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| J-001 | JSON `null` ↔ pjson `null` | ✅ | ✅ corpus `json_ingress_null` | ✅ | ✅ same |
| J-002 | JSON `false`/`true` ↔ pjson `bool` | ✅ | ✅ corpus `json_ingress_bool_{false,true}` | ✅ | ✅ same |
| J-003 | JSON integer in 0..15 ↔ `uint_inline` | ✅ | ✅ corpus `json_ingress_uint_inline` | ✅ | ✅ same |
| J-004 | JSON integer in −15..−1 ↔ `nint_inline` | ✅ | ✅ corpus `json_ingress_nint_inline` | ✅ | ✅ same |
| J-005 | JSON integer ≥ 16 ↔ `uint` | ✅ | ✅ corpus `json_ingress_uint` | ✅ | ✅ same |
| J-006 | JSON integer ≤ −16 ↔ `negint` | ✅ | ✅ corpus `json_ingress_negint` | ✅ | ✅ same |
| J-007 | JSON fractional/exponent ↔ `decimal` (when shortest) or `ieee_float` | ✅ via D-007 picker | ✅ corpus `json_ingress_fractional_picker` + `json_ingress_fractional_picks_decimal` | ✅ same | ✅ same |
| J-008 | JSON numeric-form string with canonical match ↔ `numeric_string` | ✅ | ✅ corpus `lift_canonical_int` + `lift_canonical_decimal` | ✅ | ✅ same |
| J-009 | JSON non-canonical/non-numeric string ↔ `string` | ✅ | ✅ corpus `no_lift_leading_zero` + `no_lift_sci_notation` | ✅ | ✅ same |
| J-010 | JSON array ↔ `array` (generic for heterogeneous; typed for homogeneous primitive) | ✅ generic only; typed-array auto-detection is Phase 4.3 | ✅ corpus `json_ingress_array` (generic path) | ✅ | ✅ same |
| J-011 | JSON object ↔ `object` | ✅ | ✅ corpus `json_ingress_object` | ✅ | ✅ same |
| J-012 | NaN / ±Inf in JSON: encoder rejects (JSON forbids); decoder rejects when emitting JSON | ✅ JSON parser rejects `NaN`/`Infinity` tokens | ✅ verified by Rust unit `json_ingress_rejects_nan_and_infinity_tokens` (parser-side) | ✅ | ✅ same |
| J-013 | suffix vocabulary: `.b64`, `.hex`, `.base58`, `.b64u` map to bytes encoding hints | ✅ via `from_json_node` suffix dispatch | ✅ corpus `json_ingress_suffix_b64_to_bytes` | ✅ | ✅ same |
| J-014 | suffix-stripping in hash byte applies | ✅ | ✅ corpus `object_suffix_strip_collision` (cross-validates `amount` and `amount.b64` colliding hash) | ✅ | ✅ same |
| J-015 | field encounter order preserved across round-trip | ✅ | ✅ corpus `json_ingress_field_order_preserved` | ✅ | ✅ same |

### §7.5 — Emitter options

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| EM-001 | `pretty=false` (default): no whitespace between tokens | ✅ | ✅ corpus `emitter_pretty_*` (json_compact path) | ✅ | ✅ same |
| EM-002 | `pretty=true`, `indent=N`: each array element / object key on own line, indented N spaces | ✅ | ✅ corpus `emitter_pretty_object` + `emitter_pretty_array` | ✅ | ✅ same |
| EM-003 | `pretty=true`, `indent=0`: tab-indent | ✅ | ✅ corpus `emitter_pretty_tab_indent` (also covers indent=4 alt-width) | ✅ | ✅ same |
| EM-004 | `int_string_mode=Never`: every bare integer unquoted | ✅ | ✅ corpus `emitter_int_string_mode` (json_compact path) | ✅ | ✅ same |
| EM-005 | `int_string_mode=LargeOnly`: integers \|v\| > 2⁵³ − 1 quoted; smaller bare | ✅ | ✅ corpus `emitter_int_string_mode` (LargeOnly path) | ✅ | ✅ same |
| EM-006 | `int_string_mode=All`: every bare integer quoted | ✅ | ✅ corpus `emitter_int_string_mode` (All path) | ✅ | ✅ same |
| EM-007 | `numeric_string` always quoted regardless of mode | ✅ | ✅ corpus `emitter_numeric_string_unaffected_by_mode` | ✅ | ✅ same |
| EM-008 | `ieee_float` and `decimal` not affected by `int_string_mode` | ✅ | ✅ corpus `emitter_float_decimal_unaffected_by_int_string_mode` | ✅ | ✅ same |

## §8 — Limits

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| LIM-001 | container count is u16 LE — encoder rejects > 65 535 fields | ✅ encoder check (`array/object/row_array count > 65 535`) | ✅ Rust unit `limits_enforced` | ✅ | ✅ same |
| LIM-002 | value_data ≤ 4 294 967 295 at `slot_w_code=3`; 16 MiB at u24; 64 KiB at u16; 256 B at u8 | ✅ `pick_slot_width` rejects > 2³² | ✅ encoder reject path covered (`value_data_size > u32`) | ✅ | ✅ same |
| LIM-003 | key length unbounded via long-key escape (§5.4 varuint excess) | ✅ same as H-005 | ✅ Rust unit `object_long_key_64kib_round_trip` | ✅ | ✅ same |
| LIM-004 | integer magnitude ≤ 128 unsigned bits (16-byte raw LE) | ✅ U128 type cap; bc check ≤ 16 in encode | ✅ implicit via `uint_full_round_trip` | ✅ | ✅ same |
| LIM-005 | decimal scale ∈ ±536 870 911 (4-byte varscale) | ✅ varscale_encode rejects out-of-range | ✅ Rust unit `limits_enforced` boundary check | ✅ | ✅ same |
| LIM-006 | nesting depth: configurable cap with suggested default 256; parser rejects deeper | ✅ `MAX_DECODE_DEPTH` + DepthGuard (C++) / `decode_at_depth` (Rust) | ✅ Rust unit `nesting_depth_capped` | ✅ | ✅ same |

## §9 — Errors (parser must reject)

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| E-001 | reserved type code (14, 15) | ✅ | ✅ corpus `reserved_code_14_rejected` | ✅ | ✅ `reserved_codes_rejected` |
| E-002 | reserved low-nibble bits per type | ✅ | ✅ corpus reject fixtures: null/bool/nint_inline/ieee_float/string/bytes/numeric_string/object/typed_array | ✅ | ✅ same |
| E-003 | `uint`/`negint`/`decimal` bc < 1 or bc > 16 | ✅ bc range enforced by 4-bit low nibble; truncated-payload covered by U-006 fixtures | ✅ corpus `uint_truncated_payload` | ✅ same | ✅ corpus `uint_truncated_payload` |
| E-004 | `negint` all-zero payload | ✅ | ✅ corpus `negint_zero_payload_reserved` | ✅ | ✅ `negint_zero_rejected` |
| E-005 | `nint_inline` low_nibble = 0 | ✅ | ✅ corpus `nint_inline_zero_reserved` | ✅ | ✅ `nint_inline_zero_rejected` |
| E-006 | container size too small for stated count | ✅ via decode bounds check (truncated buffer rejection) | ✅ corpus `array_slot_oob_rejected` (also exercises this path) | ✅ | ✅ same |
| E-007 | slot offset ≥ value_data_size | ✅ same as AG-005 | ✅ corpus `array_slot_oob_rejected` | ✅ | ✅ same |
| E-008 | slot offsets non-monotone | ✅ same as AG-004 | ✅ corpus `array_slot_non_monotonic_rejected` | ✅ | ✅ same |
| E-009 | hash[i] ≠ key_hash8(stored_key_i) | ✅ same as O-006 | ✅ corpus `object_hash_mismatch_rejected` | ✅ | ✅ same |
| E-010 | varscale / long-key varuint claims length past buffer | ✅ varscale + long-key paths both bounds-checked | ✅ Rust unit `long_key_varuint_truncation_rejected` (also covered by varscale truncation in `decimal_round_trip` rejects) | ✅ same | ✅ same |
| E-011 | `numeric_string` inner tag's code not in {2..7} | ✅ | ✅ corpus `numeric_string_inner_*_rejected` | ✅ | ✅ same |
| E-012 | `ieee_float` width selector ∈ {0, 5, 6, 7} | ✅ | ✅ corpus `ieee_float_width_zero_reserved` | ✅ | ✅ `ieee_float_bad_width_rejected` |

## §13 — Versioning behavior

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| V-001 | extension framework: unknown sub-type id does not error | ✅ | ✅ corpus `extension_subtype_*` | ✅ | ✅ same |
| V-002 | reserved codes (14, 15) reject without partial decode | ✅ | ✅ corpus `reserved_code_14_rejected` | ✅ | ✅ `reserved_codes_rejected` + corpus `reserved_code_14_rejected` |
| V-003 | parse-fast-path predicate `(tag >> 4) >= 14` | ✅ | ✅ `--self-test` runtime check | ✅ | ✅ `tag_byte_predicates_match_spec_section_3` + `--self-test` |

## §15 — Canonical encoding

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| C-001 | integer canonical: smallest tag form (uint_inline / nint_inline / uint / negint smallest bc) | ✅ | ✅ corpus `uint_inline_*` + `uint_*` + `nint_inline_*` + `negint_*` (encoder picks smallest) | ✅ | ✅ Rust unit `integer_canonical_smallest_tag_form` exhaustively asserts the table |
| C-002 | float canonical: smallest bit-exact width; decimal preferred only when strictly shorter | ✅ via `canonical_float_width` (binary16/32/64/128 narrowing) + D-007 picker | ✅ Rust unit `float_canonical_smallest_width` + `validate_canonical_round_trip` | ✅ same | ✅ same |
| C-003 | NaN canonical bit pattern (§15.2.1) at every width | ✅ via `canonicalize_nan_bits` rewrite at encode | ✅ corpus `canonical_nan_{f16,f32,f64,f128}` | ✅ same | ✅ Rust unit `nan_canonicalized_on_encode` exercises non-canonical → canonical at all 4 widths |
| C-004 | strings: `escape_form` from JSON source preserved byte-for-byte | ✅ | ✅ corpus `string_escape_form_hello` (round-trip preserves encoding_flag and content bytes) | ✅ | ✅ same |
| C-005 | field encounter order preserved (not sorted) | ✅ structural | ✅ corpus `object_*` round-trip preserves order | ✅ | ✅ Rust unit `object_field_encounter_order_preserved` |
| C-006 | strict-canonical validator rejects non-canonical encodings | ✅ via `validate_canonical(wire)` (decode + canonicalize + re-encode + compare) | ✅ Rust unit `validate_canonical_round_trip` | ✅ same | ✅ same |

## §14 — Conformance corpus

The corpus lives at `conformance/` and is run by both implementations.
Each fixture exercises one or more rows above. The "C++ test" /
"Rust test" cells reference fixture names rather than test functions
when applicable.

| id | corpus area | C++ runner | Rust runner |
|----|-------------|------------|-------------|
| CC-001 | corpus loader exists and parses every fixture | ✅ | ✅ |
| CC-002 | corpus harness compares wire bytes byte-exact | ✅ | ✅ |
| CC-003 | corpus harness compares JSON output text-exact | ✅ | ✅ |
| CC-004 | corpus harness compares decoded structural value | ✅ | ✅ |
| CC-005 | cross-validation: C++ encode → Rust decode produces same Value | ✅ | ✅ via xvalidate compare |
| CC-006 | cross-validation: Rust encode → C++ decode produces same Value | ✅ | ✅ via xvalidate compare |
