# pjson v1 spec compliance matrix

This file is the load-bearing record of "does our implementation
actually ship the spec." Every normative item in `pjson-spec.md`
has a row here, and every row's cells must be one of:

| symbol | meaning |
|---|---|
| ✅ | implemented + has named test that exercises this rule |
| ⚠️ | implemented but no dedicated test (must not stay across a release) |
| ❌ | not implemented (must appear in `spec-compliance-exceptions.md` with a reason) |

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
| T-009 | code 8 = `numeric_string`, low nibble must be 0; body is inner numeric value | ❌ | ❌ | ❌ | ❌ |
| T-010 | code 9 = `string`, low nibble ∈ {0, 1} = encoding flag; others reserved | ❌ | ❌ | ❌ | ❌ |
| T-011 | code 10 = `bytes`, low nibble ∈ {0,1,2,3} = JSON-emit hint; 4..15 reserved | ❌ | ❌ | ❌ | ❌ |
| T-012 | code 11 = `array`, low nibble ∈ {0, 1..10}; 11..15 reserved | ❌ | ❌ | ❌ | ❌ |
| T-013 | code 12 = `object`, low nibble ∈ {0, 1}; 2..15 reserved | ❌ | ❌ | ❌ | ❌ |
| T-014 | code 13 = `extension`, low nibble = sub-type id 0..15 | ❌ | ❌ | ❌ | ❌ |
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
| F-008 | NaN at any width preserves quiet-NaN canonical pattern (§15.2.1) | ❌ | ❌ | ⚠️ render-only, no canonicalization yet | ⚠️ |
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
| D-007 | decimal-vs-ieee_float encoder rule (§4.7.2): pick decimal when strictly shorter, else smallest bit-exact ieee width | ❌ | ❌ | ❌ encoder does not yet pick between decimal and ieee | ❌ |

## §4.8 — `numeric_string`

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| NS-001 | encode wraps a numeric inner value at code 8 (low nibble = 0) | ❌ | ❌ | ❌ | ❌ |
| NS-002 | decode exposes both `as_<numeric>()` and `as_string()` projections | ❌ | ❌ | ❌ | ❌ |
| NS-003 | encoder lifts JSON string when grammar matches AND `canonical_decimal(parse(s)) == s`, regardless of byte savings | ❌ | ❌ | ❌ | ❌ |
| NS-004 | encoder leaves non-canonical numeric strings (`"01"`, `"1.5e10"`) as plain `string` | ❌ | ❌ | ❌ | ❌ |
| NS-005 | reject inner tag whose code is not in {2..7} | ❌ | ❌ | ❌ | ❌ |
| NS-006 | reject low_nibble ≠ 0 | ❌ | ❌ | ❌ | ❌ |
| NS-007 | JSON emit always quoted regardless of `int_string_mode` (§7.5) | ❌ | ❌ | ❌ | ❌ |

## §4.9 — `string`

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| S-001 | `raw_text` encode (low_nibble 0): JSON emit runs per-char escape pass | ❌ | ❌ | ❌ | ❌ |
| S-002 | `escape_form` encode (low_nibble 1): JSON emit just wraps in quotes | ❌ | ❌ | ❌ | ❌ |
| S-003 | reject low_nibble ∈ 2..15 | ❌ | ❌ | ❌ | ❌ |
| S-004 | length is `size − 1` (no in-value length prefix) | ❌ | ❌ | ❌ | ❌ |

## §4.10 — `bytes` with encoding hint

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| BY-001 | encoding hint 0 = base64; JSON emit produces standard base64 | ❌ | ❌ | ❌ | ❌ |
| BY-002 | encoding hint 1 = hex; JSON emit produces lowercase hex | ❌ | ❌ | ❌ | ❌ |
| BY-003 | encoding hint 2 = base58; JSON emit produces Bitcoin-alphabet base58 | ❌ | ❌ | ❌ | ❌ |
| BY-004 | encoding hint 3 = base64url; JSON emit produces URL-safe base64 no padding | ❌ | ❌ | ❌ | ❌ |
| BY-005 | reject low_nibble ∈ 4..15 | ❌ | ❌ | ❌ | ❌ |
| BY-006 | wire bytes are raw octets regardless of hint | ❌ | ❌ | ❌ | ❌ |

## §4.11 — `extension`

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| EX-001 | encode `extension(subtype N, body)` → tag `0xD0 \| N`, body bytes | ❌ | ❌ | ❌ | ❌ |
| EX-002 | decode preserves subtype and body bytes verbatim | ❌ | ❌ | ❌ | ❌ |
| EX-003 | unknown sub-type id: parser does not error; surfaces as `Extension(id, bytes)` to caller | ❌ | ❌ | ❌ | ❌ |
| EX-004 | extension as object child: enclosing key-lookup works without sub-type handler | ❌ | ❌ | ❌ | ❌ |
| EX-005 | JSON emit of unknown extension produces `{"__pjson_ext": {...}}` envelope | ❌ | ❌ | ❌ | ❌ |
| EX-006 | round-trip envelope: emit → parse → re-encode preserves subtype + bytes | ❌ | ❌ | ❌ | ❌ |

## §5 — Containers

### §5.1 — Generic array

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| AG-001 | encode generic array: tag `0xB0`, width byte, value_data, slot table, count u16 LE | ❌ | ❌ | ❌ | ❌ |
| AG-002 | decode: locate slot table from tail, walk forward into value_data | ❌ | ❌ | ❌ | ❌ |
| AG-003 | adaptive slot width selection: u8 / u16 / u24 / u32 per value_data size | ❌ | ❌ | ❌ | ❌ |
| AG-004 | reject slot offsets that violate monotonicity | ❌ | ❌ | ❌ | ❌ |
| AG-005 | reject slot offset ≥ value_data_size | ❌ | ❌ | ❌ | ❌ |
| AG-006 | empty array (N=0) round-trip | ❌ | ❌ | ❌ | ❌ |

### §5.1.1 — Typed homogeneous array

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| AT-001 | element code 0 (i8): tag `0xB1`, raw u8 elements | ❌ | ❌ | ❌ | ❌ |
| AT-002 | element code 1 (i16): tag `0xB2`, raw 2-byte LE elements | ❌ | ❌ | ❌ | ❌ |
| AT-003 | element code 2 (i32): tag `0xB3` | ❌ | ❌ | ❌ | ❌ |
| AT-004 | element code 3 (i64): tag `0xB4` | ❌ | ❌ | ❌ | ❌ |
| AT-005 | element code 4 (u8): tag `0xB5` | ❌ | ❌ | ❌ | ❌ |
| AT-006 | element code 5 (u16): tag `0xB6` | ❌ | ❌ | ❌ | ❌ |
| AT-007 | element code 6 (u32): tag `0xB7` | ❌ | ❌ | ❌ | ❌ |
| AT-008 | element code 7 (u64): tag `0xB8` | ❌ | ❌ | ❌ | ❌ |
| AT-009 | element code 8 (f32): tag `0xB9` | ❌ | ❌ | ❌ | ❌ |
| AT-010 | element code 9 (f64): tag `0xBA` | ❌ | ❌ | ❌ | ❌ |
| AT-011 | reject low_nibble 11..15 | ❌ | ❌ | ❌ | ❌ |
| AT-012 | empty typed array (N=0) at every element code | ❌ | ❌ | ❌ | ❌ |

### §5.2 — Object (single)

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| O-001 | encode object: tag `0xC0`, value_data, hash[N], slot[N], count u16 LE | ❌ | ❌ | ❌ | ❌ |
| O-002 | hash byte = `key_hash8(key)` for each entry (XXH3_64 low byte after suffix strip) | ❌ | ❌ | ❌ | ❌ |
| O-003 | suffix strip: `"foo.b64"` and `"foo"` hash to same byte | ❌ | ❌ | ❌ | ❌ |
| O-004 | adaptive slot width per value_data size | ❌ | ❌ | ❌ | ❌ |
| O-005 | empty object (N=0) round-trip | ❌ | ❌ | ❌ | ❌ |
| O-006 | reject hash[i] ≠ key_hash8(stored_key_i) | ❌ | ❌ | ❌ | ❌ |
| O-007 | reject low_nibble ∈ 2..15 | ❌ | ❌ | ❌ | ❌ |

### §5.2.1 — Row-array

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| RA-001 | encode row_array: tag `0xC1`, shared key block, per-record body | ❌ | ❌ | ❌ | ❌ |
| RA-002 | decode random-access by `(record_index, key)` | ❌ | ❌ | ❌ | ❌ |
| RA-003 | encoder rule: pick row_array when wire size beats N×generic-object | ❌ | ❌ | ❌ | ❌ |

### §5.3–5.4 — Hash & long-key escape

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| H-001 | hash collision behavior: lookup verifies stored key, advances on mismatch | ❌ | ❌ | ❌ | ❌ |
| H-002 | long-key escape: `key_size_byte = 0xFF`, varuint excess inline in entry | ❌ | ❌ | ❌ | ❌ |
| H-003 | round-trip key of exactly 254 bytes (no escape) | ❌ | ❌ | ❌ | ❌ |
| H-004 | round-trip key of exactly 255 bytes (escape, excess = 0) | ❌ | ❌ | ❌ | ❌ |
| H-005 | round-trip key of 64 KiB (4-byte varuint excess) | ❌ | ❌ | ❌ | ❌ |

## §6 — Document level

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| DOC-001 | top-level value is a single pjson value; size from caller | ❌ | ❌ | ❌ | ❌ |

## §7 — JSON round-trip

### §7.1–7.4 — Mappings & policy

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| J-001 | JSON `null` ↔ pjson `null` | ❌ | ❌ | ❌ | ❌ |
| J-002 | JSON `false`/`true` ↔ pjson `bool` | ❌ | ❌ | ❌ | ❌ |
| J-003 | JSON integer in 0..15 ↔ `uint_inline` | ❌ | ❌ | ❌ | ❌ |
| J-004 | JSON integer in −15..−1 ↔ `nint_inline` | ❌ | ❌ | ❌ | ❌ |
| J-005 | JSON integer ≥ 16 ↔ `uint` | ❌ | ❌ | ❌ | ❌ |
| J-006 | JSON integer ≤ −16 ↔ `negint` | ❌ | ❌ | ❌ | ❌ |
| J-007 | JSON fractional/exponent ↔ `decimal` (when shortest) or `ieee_float` | ❌ | ❌ | ❌ | ❌ |
| J-008 | JSON numeric-form string with canonical match ↔ `numeric_string` | ❌ | ❌ | ❌ | ❌ |
| J-009 | JSON non-canonical/non-numeric string ↔ `string` | ❌ | ❌ | ❌ | ❌ |
| J-010 | JSON array ↔ `array` (generic for heterogeneous; typed for homogeneous primitive) | ❌ | ❌ | ❌ | ❌ |
| J-011 | JSON object ↔ `object` | ❌ | ❌ | ❌ | ❌ |
| J-012 | NaN / ±Inf in JSON: encoder rejects (JSON forbids); decoder rejects when emitting JSON | ❌ | ❌ | ❌ | ❌ |
| J-013 | suffix vocabulary: `.b64`, `.hex`, `.base58`, `.b64u` map to bytes encoding hints | ❌ | ❌ | ❌ | ❌ |
| J-014 | suffix-stripping in hash byte applies | ❌ | ❌ | ❌ | ❌ |
| J-015 | field encounter order preserved across round-trip | ❌ | ❌ | ❌ | ❌ |

### §7.5 — Emitter options

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| EM-001 | `pretty=false` (default): no whitespace between tokens | ❌ | ❌ | ❌ | ❌ |
| EM-002 | `pretty=true`, `indent=N`: each array element / object key on own line, indented N spaces | ❌ | ❌ | ❌ | ❌ |
| EM-003 | `pretty=true`, `indent=0`: tab-indent | ❌ | ❌ | ❌ | ❌ |
| EM-004 | `int_string_mode=Never`: every bare integer unquoted | ❌ | ❌ | ❌ | ❌ |
| EM-005 | `int_string_mode=LargeOnly`: integers \|v\| > 2⁵³ − 1 quoted; smaller bare | ❌ | ❌ | ❌ | ❌ |
| EM-006 | `int_string_mode=All`: every bare integer quoted | ❌ | ❌ | ❌ | ❌ |
| EM-007 | `numeric_string` always quoted regardless of mode | ❌ | ❌ | ❌ | ❌ |
| EM-008 | `ieee_float` and `decimal` not affected by `int_string_mode` | ❌ | ❌ | ❌ | ❌ |

## §8 — Limits

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| LIM-001 | container count is u16 LE — encoder rejects > 65 535 fields | ❌ | ❌ | ❌ | ❌ |
| LIM-002 | value_data ≤ 4 294 967 295 at `slot_w_code=3`; 16 MiB at u24; 64 KiB at u16; 256 B at u8 | ❌ | ❌ | ❌ | ❌ |
| LIM-003 | key length unbounded via long-key escape (§5.4 varuint excess) | ❌ | ❌ | ❌ | ❌ |
| LIM-004 | integer magnitude ≤ 128 unsigned bits (16-byte raw LE) | ❌ | ❌ | ❌ | ❌ |
| LIM-005 | decimal scale ∈ ±536 870 911 (4-byte varscale) | ❌ | ❌ | ❌ | ❌ |
| LIM-006 | nesting depth: configurable cap with suggested default 256; parser rejects deeper | ❌ | ❌ | ❌ | ❌ |

## §9 — Errors (parser must reject)

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| E-001 | reserved type code (14, 15) | ✅ | ✅ corpus `reserved_code_14_rejected` | ✅ | ✅ `reserved_codes_rejected` |
| E-002 | reserved low-nibble bits per type | ⚠️ partial — covered for bool/nint_inline/ieee_float; remaining types pending | ⚠️ | ⚠️ partial — same | ⚠️ |
| E-003 | `uint`/`negint`/`decimal` bc < 1 or bc > 16 | ✅ bc range enforced by 4-bit low nibble; truncated-payload covered by U-006 fixtures | ✅ corpus `uint_truncated_payload` | ✅ same | ✅ corpus `uint_truncated_payload` |
| E-004 | `negint` all-zero payload | ✅ | ✅ corpus `negint_zero_payload_reserved` | ✅ | ✅ `negint_zero_rejected` |
| E-005 | `nint_inline` low_nibble = 0 | ✅ | ✅ corpus `nint_inline_zero_reserved` | ✅ | ✅ `nint_inline_zero_rejected` |
| E-006 | container size too small for stated count | ❌ | ❌ | ❌ | ❌ |
| E-007 | slot offset ≥ value_data_size | ❌ | ❌ | ❌ | ❌ |
| E-008 | slot offsets non-monotone | ❌ | ❌ | ❌ | ❌ |
| E-009 | hash[i] ≠ key_hash8(stored_key_i) | ❌ | ❌ | ❌ | ❌ |
| E-010 | varscale / long-key varuint claims length past buffer | ⚠️ | ⚠️ | ⚠️ varscale truncation detected; long-key path not yet | ⚠️ |
| E-011 | `numeric_string` inner tag's code not in {2..7} | ❌ | ❌ | ❌ | ❌ |
| E-012 | `ieee_float` width selector ∈ {0, 5, 6, 7} | ✅ | ✅ corpus `ieee_float_width_zero_reserved` | ✅ | ✅ `ieee_float_bad_width_rejected` |

## §13 — Versioning behavior

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| V-001 | extension framework: unknown sub-type id does not error | ❌ | ❌ | ❌ | ❌ |
| V-002 | reserved codes (14, 15) reject without partial decode | ✅ | ✅ corpus `reserved_code_14_rejected` | ✅ | ✅ `reserved_codes_rejected` + corpus `reserved_code_14_rejected` |
| V-003 | parse-fast-path predicate `(tag >> 4) >= 14` | ✅ | ✅ `--self-test` runtime check | ✅ | ✅ `tag_byte_predicates_match_spec_section_3` + `--self-test` |

## §15 — Canonical encoding

| id | rule | C++ impl | C++ test | Rust impl | Rust test |
|----|------|----------|----------|-----------|-----------|
| C-001 | integer canonical: smallest tag form (uint_inline / nint_inline / uint / negint smallest bc) | ❌ | ❌ | ❌ | ❌ |
| C-002 | float canonical: smallest bit-exact width; decimal preferred only when strictly shorter | ❌ | ❌ | ❌ | ❌ |
| C-003 | NaN canonical bit pattern (§15.2.1) at every width | ❌ | ❌ | ❌ | ❌ |
| C-004 | strings: `escape_form` from JSON source preserved byte-for-byte | ❌ | ❌ | ❌ | ❌ |
| C-005 | field encounter order preserved (not sorted) | ❌ | ❌ | ❌ | ❌ |
| C-006 | strict-canonical validator rejects non-canonical encodings | ❌ | ❌ | ❌ | ❌ |

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
