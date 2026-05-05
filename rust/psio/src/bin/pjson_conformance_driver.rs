//! pjson v1 spec conformance driver.
//!
//! This binary is the **reference implementation** of the
//! `docs/pjson-spec.md` wire format, used to validate the conformance
//! corpus end-to-end without depending on the pre-audit pjson library
//! code in `src/pjson*.rs`. It is intentionally self-contained; once
//! the implementation stabilizes through all five phases of
//! `docs/spec-compliance.md`, the library kernel will be migrated to
//! use this code (or a derivative) as its core.
//!
//! ## CLI
//!
//! ```text
//! pjson_conformance_driver --check     < fixture.json   # exit 0 = pass
//! pjson_conformance_driver --xvalidate < fixture.json   # canonical form on stdout
//! ```
//!
//! `--check` mode reads a fixture, runs encode + decode + JSON render
//! against the fixture's expectations, exits 0 if everything matches
//! and 1 otherwise (with details on stderr).
//!
//! `--xvalidate` mode emits a stable canonical representation
//! (`wire:HEX json:STR`) so the cross-language harness can compare
//! C++ and Rust results.
//!
//! ## Scope (Phase 1)
//!
//! - Tag dispatch + reserved-code rejection (T-*, V-*).
//! - `null`, `bool` (N-*, B-*).
//! - `uint_inline`, `nint_inline` (UI-*, NI-*).
//! - `uint`, `negint` to 128-bit magnitude (U-*).
//! - `ieee_float` widths binary16/32/64 (F-001..003, F-005..009).
//! - `decimal` with all four varscale tiers (D-*).
//!
//! Phase 2+ types (containers, strings, bytes, numeric_string,
//! extension) return `Err(DecodeError::NotImplementedYet)` until they
//! land — kept explicit so a fixture that targets unimplemented
//! coverage fails loud instead of being silently mishandled.

use serde_json::Value as Json;
use std::io::{self, Read};
use std::process::ExitCode;

// ── Spec-defined Value model ────────────────────────────────────────

/// In-memory representation of a pjson value, matching the
/// fixture-DSL (`conformance/README.md`) and the spec's Value model.
#[derive(Debug, Clone, PartialEq)]
enum Value {
    Null,
    Bool(bool),
    /// Non-negative integer up to u128::MAX. Encoded as `uint_inline`
    /// (0..15) or `uint` (16..) per §15.2 canonical rules.
    Uint(u128),
    /// Negative integer down to -(2^128 - 1). Encoded as
    /// `nint_inline` (-1..-15) or `negint` per §15.2.
    NegInt(u128),
    /// IEEE-754 binary16/32/64 carried as the raw bit pattern. Width
    /// is stored explicitly so f16/f32 don't get auto-promoted to f64
    /// during round-trip.
    Float { width_log2: u8, bits: u128 },
    /// Exact decimal `mantissa × 10^scale`. Mantissa is signed.
    Decimal { mantissa: i128, scale: i32 },
    /// Generic array (§5.1) — heterogeneous children. Tail-indexed
    /// layout with adaptive slot width.
    Array(Vec<Value>),
    /// Typed homogeneous array (§5.1.1) — element_code ∈ 0..9 maps to
    /// i8/i16/i32/i64/u8/u16/u32/u64/f32/f64. Raw bytes are stored
    /// little-endian, fixed-width per element_code.
    TypedArray { element_code: u8, raw: Vec<u8> },
    /// Object (§5.2) — ordered list of (key, value) entries. Encounter
    /// order is preserved; field iteration is the slot-table order.
    /// Hash bytes (XXH3-64 low byte over suffix-stripped keys, §5.3)
    /// power the prefilter scan in lookups.
    Object(Vec<(String, Value)>),
    /// Row-array (§5.2.1) — homogeneous-shape array of objects.
    /// `keys` holds the K shared field names, in order; `rows` holds
    /// N records each with K values. The shared key block lives once
    /// at the array level, removing per-record key bytes and per-
    /// record hash tables vs `Object × N`.
    RowArray { keys: Vec<String>, rows: Vec<Vec<Value>> },
    /// String (§4.9) — UTF-8 text. encoding_flag 0 = raw_text (the
    /// JSON emitter must run a per-character escape pass); 1 =
    /// escape_form (text is already in JSON-escape form, the emitter
    /// just wraps it in quotes).
    String { encoding_flag: u8, content: Vec<u8> },
    /// Bytes (§4.10) — raw octets with a JSON-emit encoding hint
    /// (0=base64, 1=hex, 2=base58, 3=base64url, 4..15 reserved).
    /// Wire bytes are always raw octets; the hint only governs JSON
    /// projection.
    Bytes { encoding_hint: u8, content: Vec<u8> },
    /// Numeric-string (§4.8) — a dual-form value. Wire body is a
    /// numeric inner (codes 2..7); JSON projection renders the inner
    /// as canonical decimal, wrapped in quotes. Preserves "this was a
    /// JSON string" type information for consumers that distinguish
    /// `typeof === "string"` from `typeof === "number"`.
    NumericString(Box<Value>),
    /// Extension (§4.11) — soft-evolution path. `subtype` is the
    /// sub-type id (0..15); `bytes` is the opaque body.  A parser
    /// that doesn't recognize `subtype` still surfaces the value via
    /// this variant; the JSON projection uses an envelope so the
    /// round-trip preserves both id and bytes through unknown-aware
    /// readers.
    Extension { subtype: u8, bytes: Vec<u8> },
}

/// §4.8 numeric_string requires an inner of code 2..7 (any of the
/// numeric types: uint_inline, nint_inline, uint, negint, ieee_float,
/// decimal). Aggregates / strings / wrapped numeric_strings are not
/// permitted.
fn is_numeric_value(v: &Value) -> bool {
    matches!(v,
        Value::Uint(_) | Value::NegInt(_) | Value::Float { .. } | Value::Decimal { .. })
}

/// §4.8 / §7.1 numeric-string lift: returns Some(numeric_value) when
/// `s` is a canonical-form JSON-number-grammar string (suitable for
/// lifting to numeric_string), or None otherwise. The "canonical form"
/// rule per spec:
///
///   - matches  ^-?(0|[1-9][0-9]*)(\.[0-9]*[1-9])?$
///   - rejects  "-0"  (no negative-zero integer)
///   - rejects  "1.0", "1.50"  (trailing zeros after decimal)
///   - rejects  "01", "00123"  (leading zeros)
///   - rejects  "1e5", "1.5e10"  (sci notation)
///   - rejects  "+1"  (leading plus)
///   - rejects  ".5", "1."  (missing integer or fractional part)
///
/// When the rule matches, the parsed numeric value is returned ready
/// to wrap in `Value::NumericString`.
fn parse_canonical_json_number_string(s: &str) -> Option<Value> {
    let bytes = s.as_bytes();
    if bytes.is_empty() { return None; }

    let mut i = 0;
    let negative = bytes[0] == b'-';
    if negative { i += 1; }
    if i >= bytes.len() { return None; }

    // Integer part: 0 OR [1-9][0-9]*
    let int_start = i;
    if bytes[i] == b'0' {
        i += 1;
    } else if (b'1'..=b'9').contains(&bytes[i]) {
        i += 1;
        while i < bytes.len() && bytes[i].is_ascii_digit() { i += 1; }
    } else {
        return None;
    }
    let int_part = &bytes[int_start..i];

    // Optional fractional: . then digits, must end in non-zero
    let frac_part: &[u8] = if i < bytes.len() && bytes[i] == b'.' {
        i += 1;
        let frac_start = i;
        while i < bytes.len() && bytes[i].is_ascii_digit() { i += 1; }
        if i == frac_start { return None; }       // empty fractional
        if bytes[i - 1] == b'0' { return None; }  // trailing zero → non-canonical
        &bytes[frac_start..i]
    } else { b"" };

    // Must consume entire string — any trailing char (e/E/space/etc.) → reject.
    if i != bytes.len() { return None; }

    // "-0" is non-canonical (canonical is "0") and negint magnitude 0 is
    // reserved (§4.4) — reject either way.
    if negative && int_part == b"0" && frac_part.is_empty() { return None; }

    if frac_part.is_empty() {
        // Integer
        let s_str = std::str::from_utf8(int_part).ok()?;
        let n: u128 = s_str.parse().ok()?;
        if negative {
            if n == 0 { return None; }
            Some(Value::NegInt(n))
        } else {
            Some(Value::Uint(n))
        }
    } else {
        // Decimal: combine int + frac into mantissa, scale = -frac.len()
        let mut mantissa_str = String::with_capacity(int_part.len() + frac_part.len() + 1);
        if negative { mantissa_str.push('-'); }
        mantissa_str.push_str(std::str::from_utf8(int_part).ok()?);
        mantissa_str.push_str(std::str::from_utf8(frac_part).ok()?);
        let mantissa: i128 = mantissa_str.parse().ok()?;
        let scale = -(frac_part.len() as i32);
        Some(Value::Decimal { mantissa, scale })
    }
}

/// JSON ingress: parse a JSON text into a pjson Value, applying the
/// numeric_string lift rule. The "host type controls" rule (E-005)
/// holds: byte-inspection of strings happens **only** here, on the
/// JSON-source path. Native typed callers go through the `Value`
/// constructors directly.
fn from_json(text: &str) -> Result<Value, String> {
    let v: serde_json::Value = serde_json::from_str(text)
        .map_err(|e| format!("from_json parse: {}", e))?;
    Ok(from_serde_json(&v))
}

fn from_serde_json(v: &serde_json::Value) -> Value {
    match v {
        serde_json::Value::Null => Value::Null,
        serde_json::Value::Bool(b) => Value::Bool(*b),
        serde_json::Value::Number(n) => {
            // Use the source token string to preserve "1.5" vs "1" exactly.
            let token: &str = n.as_str();
            if let Some(val) = parse_canonical_json_number_string(token) {
                // D-007: for a fractional (Decimal) result, apply the
                // §4.7.2 picker to choose between decimal and ieee_float.
                if let Value::Decimal { mantissa, scale } = val {
                    return decimal_or_ieee_pick(mantissa, scale);
                }
                return val;
            }
            // Fall back: sci-notation, "1.0", "-0", or out-of-range.
            if let Some(f) = n.as_f64() {
                // No exact decimal source token — encode through the
                // ieee path with width minimization (C-002).
                let (w, b) = canonical_float_width(3, f.to_bits() as u128);
                Value::Float { width_log2: w, bits: b }
            } else {
                Value::Decimal { mantissa: 0, scale: 0 }
            }
        }
        serde_json::Value::String(s) => {
            // Numeric_string lift: §4.8 rule.
            if let Some(num) = parse_canonical_json_number_string(s) {
                Value::NumericString(Box::new(num))
            } else {
                Value::String { encoding_flag: 0, content: s.as_bytes().to_vec() }
            }
        }
        serde_json::Value::Array(arr) => {
            let children: Vec<Value> = arr.iter().map(from_serde_json).collect();
            // §5.2.1.5 / RA-003: detect homogeneous array-of-object.
            // If every child is an Object and all share the same keys
            // in the same order, lift to row_array — the shared key
            // block lives once at the array level. The consume-or-
            // give-back signature lets the lift happen with zero
            // value clones; on heterogeneous input we get the original
            // Vec back unchanged.
            match try_lift_to_row_array(children) {
                Ok((keys, rows)) => Value::RowArray { keys, rows },
                Err(children)    => Value::Array(children),
            }
        }
        serde_json::Value::Object(obj) => {
            // §4.11 envelope round-trip: an object of the exact shape
            // `{"__pjson_ext": {"subtype": N, "bytes_b64": "..."}}` is
            // an unknown-extension envelope that should reconstruct
            // back to an Extension wire form. The check is shape-
            // sensitive (single key, sub-object with exact keys) to
            // avoid hijacking arbitrary user data.
            if obj.len() == 1 {
                if let Some(inner) = obj.get("__pjson_ext") {
                    if let serde_json::Value::Object(ext) = inner {
                        let sub = ext.get("subtype").and_then(|x| x.as_u64());
                        let b64 = ext.get("bytes_b64").and_then(|x| x.as_str());
                        if let (Some(s), Some(b64)) = (sub, b64) {
                            if s <= 15 && ext.len() == 2 {
                                if let Some(bytes) = b64_decode(b64) {
                                    return Value::Extension {
                                        subtype: s as u8, bytes,
                                    };
                                }
                            }
                        }
                    }
                }
            }
            Value::Object(
                obj.iter()
                    .map(|(k, v)| {
                        // §7.4 / J-013: a key ending in a suffix-vocabulary
                        // term tells the ingress to treat the string value
                        // as opaque binary with the matching encoding hint.
                        // The literal suffix is preserved on the key.
                        if let Some(hint) = bytes_hint_for_key_suffix(k) {
                            if let serde_json::Value::String(s) = v {
                                if let Some(content) = decode_bytes_by_hint(hint, s) {
                                    return (k.clone(), Value::Bytes {
                                        encoding_hint: hint, content,
                                    });
                                }
                            }
                        }
                        (k.clone(), from_serde_json(v))
                    })
                    .collect()
            )
        }
    }
}

/// §7.4 — known suffix vocabulary mapping a JSON key suffix to a
/// `bytes` encoding hint.
fn bytes_hint_for_key_suffix(key: &str) -> Option<u8> {
    if      key.ends_with(".b64")    { Some(0) }
    else if key.ends_with(".hex")    { Some(1) }
    else if key.ends_with(".base58") { Some(2) }
    else if key.ends_with(".b64u")   { Some(3) }
    else                             { None }
}

fn decode_bytes_by_hint(hint: u8, s: &str) -> Option<Vec<u8>> {
    match hint {
        0 => b64_decode(s),
        1 => hex_decode(s),
        2 => base58_decode(s),
        3 => b64url_decode(s),
        _ => None,
    }
}

/// §5.3 — 8-bit prefilter hash. Strip the key from the last `.`
/// onward (e.g. `"amount.decimal"` → `"amount"`), hash with XXH3-64,
/// return the low byte. Suffix-stripping enables presentation-tag-
/// suffix matching where `"foo"` and `"foo.b64"` collide on the
/// prefilter, then byte-equal compare distinguishes them.
fn key_hash8(key: &str) -> u8 {
    let stripped = match key.rfind('.') {
        Some(idx) => &key[..idx],
        None      => key,
    };
    (xxhash_rust::xxh3::xxh3_64(stripped.as_bytes()) & 0xFF) as u8
}

/// §5.4 long-key escape — 2-bit-prefix variable-length **unsigned**
/// integer (same byte-count tiers as varscale, no zigzag step).
fn varuint_encode_into(value: u32, out: &mut Vec<u8>) -> Result<(), EncodeError> {
    let total_bytes = if value < (1u32 << 6) {
        1
    } else if value < (1u32 << 14) {
        2
    } else if value < (1u32 << 22) {
        3
    } else if value < (1u32 << 30) {
        4
    } else {
        return Err(EncodeError::Overflow("varuint"));
    };
    let prefix = ((total_bytes - 1) as u8) << 6;
    let lo6 = (value & 0x3F) as u8;
    out.push(prefix | lo6);
    let mut shifted = value >> 6;
    for _ in 1..total_bytes {
        out.push((shifted & 0xFF) as u8);
        shifted >>= 8;
    }
    Ok(())
}

fn varuint_decode(buf: &[u8]) -> Result<(u32, usize), DecodeError> {
    if buf.is_empty() {
        return Err(DecodeError::Truncated("varuint first byte"));
    }
    let total_bytes = ((buf[0] >> 6) as usize) + 1;
    if buf.len() < total_bytes {
        return Err(DecodeError::Truncated("varuint body"));
    }
    let mut v: u32 = (buf[0] & 0x3F) as u32;
    for i in 1..total_bytes {
        v |= (buf[i] as u32) << (6 + 8 * (i - 1));
    }
    Ok((v, total_bytes))
}

/// §5.1.1 — element_size in bytes for each typed-array element_code.
fn typed_array_element_size(code: u8) -> Result<usize, &'static str> {
    match code {
        0 | 4 => Ok(1), // i8, u8
        1 | 5 => Ok(2), // i16, u16
        2 | 6 | 8 => Ok(4), // i32, u32, f32
        3 | 7 | 9 => Ok(8), // i64, u64, f64
        _ => Err("typed_array element_code out of range"),
    }
}

// ── Encode (§12 reference algorithm) ────────────────────────────────

#[derive(Debug)]
enum EncodeError {
    /// Value out of range for the spec (e.g. mantissa needs > 16 bytes).
    Overflow(&'static str),
    /// Variant not implemented in Phase 1 yet.
    NotImplementedYet(&'static str),
}

/// RA-003 / §5.2.1.5 — homogeneous-shape detection + lift.
///
/// Two-phase to avoid touching value bytes twice:
///
///   1. Borrow-pass: walk `children` by reference and verify that
///      every element is `Value::Object` with the same keys in the
///      same order as the first. Bail early on the first mismatch.
///   2. Move-pass: only on success, consume `children` and move
///      values out of each Object into the per-row `Vec<Value>`.
///      Zero clones on the lift path.
///
/// On heterogeneous input the function returns `Err(children)` so
/// the caller can fall through to `Value::Array(children)` without
/// reallocating the outer Vec.
fn try_lift_to_row_array(
    mut children: Vec<Value>,
) -> Result<(Vec<String>, Vec<Vec<Value>>), Vec<Value>> {
    if children.is_empty() { return Err(children); }
    // Phase 1 — borrow check.
    let first_keys: Vec<String> = match &children[0] {
        Value::Object(entries) if !entries.is_empty() => {
            entries.iter().map(|(k, _)| k.clone()).collect()
        }
        _ => return Err(children),
    };
    for c in children.iter().skip(1) {
        match c {
            Value::Object(entries) => {
                if entries.len() != first_keys.len() { return Err(children); }
                for (i, (k, _)) in entries.iter().enumerate() {
                    if k != &first_keys[i] { return Err(children); }
                }
            }
            _ => return Err(children),
        }
    }
    // Phase 2 — consume and move.
    let mut rows: Vec<Vec<Value>> = Vec::with_capacity(children.len());
    for c in children.drain(..) {
        match c {
            Value::Object(entries) => {
                let mut row: Vec<Value> = Vec::with_capacity(first_keys.len());
                for (_k, v) in entries.into_iter() {
                    row.push(v);
                }
                rows.push(row);
            }
            _ => unreachable!("phase 1 verified all children are Object"),
        }
    }
    Ok((first_keys, rows))
}

/// RA-002 — random-access accessor for a `RowArray`. Returns the
/// value at `(record_index, key)` without materializing the entire
/// row. Returns None on out-of-range index or unknown key.
fn row_array_get<'a>(v: &'a Value, record_index: usize, key: &str) -> Option<&'a Value> {
    if let Value::RowArray { keys, rows } = v {
        let row = rows.get(record_index)?;
        let col = keys.iter().position(|k| k == key)?;
        return row.get(col);
    }
    None
}

/// NS-002 — dual-projection accessors for a numeric_string. Returns
/// `Some(inner_numeric)` and `Some(canonical_decimal_string)` for a
/// `Value::NumericString`; `None` for any other variant.
fn numeric_string_as_numeric(v: &Value) -> Option<&Value> {
    if let Value::NumericString(inner) = v { Some(inner) } else { None }
}

fn numeric_string_as_string(v: &Value) -> Option<String> {
    let inner = numeric_string_as_numeric(v)?;
    Some(render_json(inner))
}

/// §4.7.2 / D-007 — decimal-vs-ieee_float picker. Given a JSON
/// fractional source whose canonical-form (mantissa, scale) decimal
/// is known, choose the encoding with smaller wire size. On a tie,
/// prefer ieee_float (faster decode per §15.2 rule 5).
///
/// This is invoked **only** on the JSON-ingress path. Typed-decimal
/// callers (passing `Value::Decimal` directly) keep their
/// exact-decimal identity per §15.2's typed-source rule.
fn decimal_or_ieee_pick(mantissa: i128, scale: i32) -> Value {
    // Decimal wire size: tag (1) + zigzag mantissa minimal bytes
    // (1..16) + varscale (1..4).
    let zz = zigzag_encode_i128(mantissa);
    let m_bc = u128_minimal_byte_count(zz);
    let scale_bc = match scale.unsigned_abs() {
        0..=31           => 1,
        32..=8191        => 2,
        8192..=2_097_151 => 3,
        _                => 4,
    };
    let decimal_size = 1 + m_bc + scale_bc;

    // ieee_float candidate: convert (m, s) to f64, find smallest
    // bit-exact width.
    let f = (mantissa as f64) * 10f64.powi(scale);
    // Verify the f64 round-trips through the (m, s) form, otherwise
    // ieee can't represent the decimal exactly — use decimal.
    if !decimal_f64_roundtrips(mantissa, scale, f) {
        return Value::Decimal { mantissa, scale };
    }
    let (w, bits) = canonical_float_width(3, f.to_bits() as u128);
    let ieee_size = 1 + (1usize << w);

    // Strictly shorter decimal wins; otherwise prefer ieee (rule 5).
    if decimal_size < ieee_size {
        Value::Decimal { mantissa, scale }
    } else {
        Value::Float { width_log2: w, bits }
    }
}

/// True when `f` (a double) round-trips back to the supplied
/// (mantissa, scale) form bit-exactly. Used by the D-007 picker to
/// rule out ieee_float for decimals that double can't represent
/// (e.g., 0.1, 0.2 — every rational with a non-power-of-2 denominator
/// of more than ~17 significant digits).
fn decimal_f64_roundtrips(mantissa: i128, scale: i32, f: f64) -> bool {
    // Reconstruct mantissa from f at the same scale.
    let scaled = f * 10f64.powi(-scale);
    if !scaled.is_finite() { return false; }
    let rounded = scaled.round();
    if (scaled - rounded).abs() > 1e-9 { return false; }
    let m_back = rounded as i128;
    m_back == mantissa
}

/// §15.7 / C-006 — strict-canonical validator. Returns `Ok(())` if
/// `wire` is the canonical encoding of its decoded value, or
/// `Err(reason)` describing the first non-canonical aspect found.
///
/// Implementation: decode + canonicalize + re-encode, then compare
/// byte-for-byte. A canonical wire round-trips through this pipeline
/// unchanged; any non-canonical wire produces different bytes on
/// re-encode (e.g., `0x40 0x05` for the value 5 re-encodes as `0x25`).
fn validate_canonical(wire: &[u8]) -> Result<(), String> {
    let mut v = decode(wire).map_err(|e| format!("decode: {:?}", e))?;
    canonicalize_in_place(&mut v);
    let re = encode(&v).map_err(|e| format!("re-encode: {:?}", e))?;
    if re == wire { Ok(()) } else {
        Err(format!("non-canonical: re-encoded as {} bytes (vs {})",
                    re.len(), wire.len()))
    }
}

/// §15.1 — canonical encoder. Applies width minimization (C-002),
/// trailing-zero mantissa trim (decimal canonical), and the
/// decimal-vs-ieee picker (D-007), then defers to `encode`.
///
/// Containers recurse element-wise. The non-canonical `encode` path
/// is preserved for fixtures that want to test bit-for-bit
/// preservation of the supplied form (e.g., asserting that an f64
/// stays f64 even when 1.5 fits in f16).
fn encode_canonical(v: &Value) -> Result<Vec<u8>, EncodeError> {
    let mut owned = v.clone();
    canonicalize_in_place(&mut owned);
    encode(&owned)
}

/// Recursive in-place canonical transform. Touches only nodes that
/// would change (Float width, Decimal trailing zeros) — TypedArray,
/// String, Bytes, Bool, Null, Uint, NegInt, and Extension are skipped
/// without traversal cost beyond a discriminant test.
fn canonicalize_in_place(v: &mut Value) {
    match v {
        Value::Float { width_log2, bits } => {
            let (w, b) = canonical_float_width(*width_log2, *bits);
            *width_log2 = w;
            *bits = b;
        }
        Value::Decimal { mantissa, scale } => {
            let (m, s) = trim_decimal(*mantissa, *scale);
            *mantissa = m;
            *scale = s;
        }
        Value::Array(children) => {
            for c in children.iter_mut() { canonicalize_in_place(c); }
        }
        Value::Object(entries) => {
            for (_k, v) in entries.iter_mut() { canonicalize_in_place(v); }
        }
        Value::RowArray { keys: _, rows } => {
            for row in rows.iter_mut() {
                for v in row.iter_mut() { canonicalize_in_place(v); }
            }
        }
        Value::NumericString(inner) => {
            canonicalize_in_place(inner);
        }
        // TypedArray, String, Bytes, Bool, Null, Uint, NegInt,
        // Extension are canonical by construction — no traversal.
        _ => {}
    }
}

/// Backward-compatible wrapper: returns a fresh canonicalized clone.
/// Tests use this; production code should prefer `canonicalize_in_place`.
fn canonicalize_value(v: &Value) -> Value {
    let mut owned = v.clone();
    canonicalize_in_place(&mut owned);
    owned
}

/// C-002 — find the smallest IEEE width in {1, 2, 3, 4} (binary16/32
/// /64/128) for which `bits` (interpreted at `from_w`) round-trips
/// bit-exact. Returns `(width_log2, bits_at_that_width)`.
///
/// Width-1 (binary16) and width-4 (binary128) require bit manipulation
/// since Rust has no native f16/f128 — width-1 is checked via
/// f64↔f16 conversions; width-4 is left in place if the source was
/// already f128 (no narrower target tried beyond what fits in f64).
fn canonical_float_width(from_w: u8, bits: u128) -> (u8, u128) {
    // ±0 always renders identically at every width — pick the smallest.
    if is_float_zero(from_w, bits) {
        let sign = (bits >> (8 * (1 << from_w) - 1)) & 1;
        return (1, (sign << 15) as u128);    // ±0.0 in binary16
    }
    // ±Inf and NaN at every width is representable at binary16.
    // (NaN has already been canonicalized to the quiet pattern.)
    if is_float_inf(from_w, bits) {
        let sign = (bits >> (8 * (1 << from_w) - 1)) & 1;
        return (1, ((sign << 15) | 0x7C00) as u128);
    }
    if is_float_nan(from_w, bits) {
        // canonicalize_nan_bits will produce 0x7E00 at width 1.
        return (1, 0x7E00u128);
    }

    // Lift to f64 for narrowing trials. f128 sources only narrow to
    // f64 if the value fits exactly; if not, keep as f128.
    let as_f64: Option<f64> = match from_w {
        1 => Some(f16_bits_to_f64((bits & 0xFFFF) as u16)),
        2 => Some(f32::from_bits(bits as u32) as f64),
        3 => Some(f64::from_bits(bits as u64)),
        4 => f128_bits_to_f64_exact(bits),
        _ => None,
    };
    let f = match as_f64 {
        Some(f) => f,
        None => return (from_w, bits),  // f128 with no exact f64
    };

    // Try binary16.
    if let Some(f16_bits) = f64_to_f16_exact(f) {
        return (1, f16_bits as u128);
    }
    // Try binary32.
    let f32_val = f as f32;
    if (f32_val as f64).to_bits() == f.to_bits() {
        return (2, f32_val.to_bits() as u128);
    }
    // Otherwise keep at binary64.
    (3, f.to_bits() as u128)
}

fn is_float_zero(w: u8, bits: u128) -> bool {
    let mask = match w {
        1 => 0x7FFFu128,
        2 => 0x7FFF_FFFFu128,
        3 => 0x7FFF_FFFF_FFFF_FFFFu128,
        4 => 0x7FFF_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF_FFFFu128,
        _ => return false,
    };
    (bits & mask) == 0
}

fn is_float_inf(w: u8, bits: u128) -> bool {
    let (exp_mask, mant_mask) = match w {
        1 => (0x7C00u128,                                 0x03FFu128),
        2 => (0x7F80_0000u128,                            0x007F_FFFFu128),
        3 => (0x7FF0_0000_0000_0000u128,                  0x000F_FFFF_FFFF_FFFFu128),
        4 => (0x7FFF_0000_0000_0000_0000_0000_0000_0000u128,
              0x0000_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF_FFFFu128),
        _ => return false,
    };
    (bits & exp_mask) == exp_mask && (bits & mant_mask) == 0
}

fn is_float_nan(w: u8, bits: u128) -> bool {
    let (exp_mask, mant_mask) = match w {
        1 => (0x7C00u128,                                 0x03FFu128),
        2 => (0x7F80_0000u128,                            0x007F_FFFFu128),
        3 => (0x7FF0_0000_0000_0000u128,                  0x000F_FFFF_FFFF_FFFFu128),
        4 => (0x7FFF_0000_0000_0000_0000_0000_0000_0000u128,
              0x0000_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF_FFFFu128),
        _ => return false,
    };
    (bits & exp_mask) == exp_mask && (bits & mant_mask) != 0
}

/// f64 → f16 conversion that succeeds only if the value is bit-exact
/// representable in binary16. Returns the 16-bit pattern on success.
fn f64_to_f16_exact(f: f64) -> Option<u16> {
    let bits = f.to_bits();
    let sign = ((bits >> 63) & 1) as u16;
    let exp_f64 = ((bits >> 52) & 0x7FF) as i32;
    let mant_f64 = bits & 0x000F_FFFF_FFFF_FFFF;

    if exp_f64 == 0 {
        // Subnormal or zero. Zero handled by is_float_zero earlier.
        // Subnormals in f64 don't fit in f16 unless mantissa is small.
        // Conservative: only zero is exact.
        if mant_f64 == 0 { return Some(sign << 15); }
        return None;
    }
    if exp_f64 == 0x7FF { return None; }    // Inf/NaN handled earlier.

    let exp_unbiased = exp_f64 - 1023;
    // f16 normal range: exp_unbiased ∈ [-14, 15].
    if exp_unbiased < -14 || exp_unbiased > 15 { return None; }
    // Mantissa must use only top 10 bits — bottom 42 bits zero.
    if mant_f64 & ((1u64 << 42) - 1) != 0 { return None; }
    let mant_f16 = (mant_f64 >> 42) as u16;
    let exp_f16 = (exp_unbiased + 15) as u16;
    Some((sign << 15) | (exp_f16 << 10) | mant_f16)
}

/// f128 bits → f64 if bit-exact representable; else None.
fn f128_bits_to_f64_exact(bits: u128) -> Option<f64> {
    // f128: 1 sign + 15 exp + 112 mantissa.
    let sign = ((bits >> 127) & 1) as u64;
    let exp_f128 = ((bits >> 112) & 0x7FFF) as i32;
    let mant_f128 = bits & ((1u128 << 112) - 1);

    if exp_f128 == 0 {
        if mant_f128 == 0 {
            return Some(f64::from_bits(sign << 63));
        }
        return None;       // f128 subnormals — punt
    }
    if exp_f128 == 0x7FFF { return None; }

    let exp_unbiased = exp_f128 - 16383;
    if exp_unbiased < -1022 || exp_unbiased > 1023 { return None; }
    // Mantissa must use only top 52 bits.
    if mant_f128 & ((1u128 << 60) - 1) != 0 { return None; }
    let mant_f64 = (mant_f128 >> 60) as u64;
    let exp_f64 = ((exp_unbiased + 1023) as u64) << 52;
    let out = (sign << 63) | exp_f64 | mant_f64;
    Some(f64::from_bits(out))
}

/// Trim trailing zeros from a base-10 mantissa. (mantissa=10, scale=0)
/// becomes (mantissa=1, scale=1). Stops when the last digit is non-zero
/// or scale rolls over toward zero.
fn trim_decimal(mantissa: i128, scale: i32) -> (i128, i32) {
    let mut m = mantissa;
    let mut s = scale;
    while m != 0 && (m % 10) == 0 {
        m /= 10;
        s = s.saturating_add(1);
    }
    (m, s)
}

fn encode(v: &Value) -> Result<Vec<u8>, EncodeError> {
    // Pre-size the output buffer to a rough upper bound so the inner
    // `out.push` / `extend_from_slice` calls don't trigger geometric
    // re-allocations on large inputs. The estimator is conservative
    // (always ≥ actual size) so excess capacity ends up trimmed by
    // the caller's `Vec` consumer if needed.
    let mut out = Vec::with_capacity(estimate_encoded_size(v));
    encode_into(v, &mut out)?;
    Ok(out)
}

/// Conservative upper bound for the encoded byte count. Used only
/// to pre-size the output buffer in `encode` — accuracy isn't
/// required, but it should never under-report (would force a
/// realloc) for typical inputs.
fn estimate_encoded_size(v: &Value) -> usize {
    match v {
        Value::Null | Value::Bool(_) => 1,
        Value::Uint(n) | Value::NegInt(n) => {
            // Worst case: 1 tag + 16 magnitude bytes.
            if *n <= 15 { 1 } else { 1 + u128_minimal_byte_count(*n) }
        }
        Value::Float { width_log2, .. } => 1 + (1usize << *width_log2),
        Value::Decimal { mantissa, .. } => {
            // tag + zigzag mantissa + up to 4 varscale bytes.
            1 + u128_minimal_byte_count(zigzag_encode_i128(*mantissa)) + 4
        }
        Value::String { content, .. } | Value::Bytes { content, .. } => {
            1 + content.len()
        }
        Value::NumericString(inner) => 1 + estimate_encoded_size(inner),
        Value::Extension { bytes, .. } => 1 + bytes.len(),
        Value::Array(children) => {
            // tag + width + sum(children) + slot table (≤4·N) + count(2)
            let body: usize = children.iter().map(estimate_encoded_size).sum();
            4 + body + 4 * children.len()
        }
        Value::TypedArray { element_code, raw } => {
            // tag + count varuint(≤5) + raw bytes (already sized by elements)
            let _ = element_code;
            1 + 5 + raw.len()
        }
        Value::Object(entries) => {
            // tag + width + per-entry (key + child + slot+ksize + hash) + count(2)
            let body: usize = entries.iter()
                .map(|(k, v)| {
                    // 1 (key_size_byte) + 4 (long-key varuint, worst) +
                    // key.len() + child + 4 (slot offset) + 1 (hash byte)
                    1 + 4 + k.len() + estimate_encoded_size(v) + 4 + 1
                })
                .sum();
            4 + body
        }
        Value::RowArray { keys, rows } => {
            // tag + width(1) + counts (~6) + key block + slots + per-record body.
            let key_bytes: usize = keys.iter().map(|k| k.len() + 5).sum();
            let n_records = rows.len();
            let cells_per: usize = if n_records > 0 { rows[0].len() } else { 0 };
            let body_size: usize = rows.iter()
                .flat_map(|r| r.iter())
                .map(estimate_encoded_size)
                .sum();
            16 + key_bytes + body_size + 4 * cells_per * n_records
        }
    }
}

fn encode_into(v: &Value, out: &mut Vec<u8>) -> Result<(), EncodeError> {
    match v {
        Value::Null => {
            out.push(0x00);
        }
        Value::Bool(false) => out.push(0x10),
        Value::Bool(true) => out.push(0x11),

        Value::Uint(n) => {
            if *n <= 15 {
                // uint_inline: §4.3, code 2
                out.push(0x20 | (*n as u8));
            } else {
                // uint: §4.5, code 4. bc-1 in low nibble; magnitude
                // raw LE in payload. bc is the smallest 1..16 that
                // fits.
                let bytes = u128_le_minimal(*n);
                let bc = bytes.len();
                if bc == 0 || bc > 16 {
                    return Err(EncodeError::Overflow("uint magnitude"));
                }
                out.push(0x40 | ((bc - 1) as u8));
                out.extend_from_slice(&bytes);
            }
        }

        Value::NegInt(mag) => {
            // §4.4 reserves magnitude 0 (no negative-zero integer).
            if *mag == 0 {
                return Err(EncodeError::Overflow("negint magnitude 0 reserved"));
            }
            if *mag <= 15 {
                // nint_inline: §4.4, code 3. low_nibble = magnitude.
                out.push(0x30 | (*mag as u8));
            } else {
                // negint: §4.5, code 5. Same encoding shape as uint.
                let bytes = u128_le_minimal(*mag);
                let bc = bytes.len();
                if bc == 0 || bc > 16 {
                    return Err(EncodeError::Overflow("negint magnitude"));
                }
                out.push(0x50 | ((bc - 1) as u8));
                out.extend_from_slice(&bytes);
            }
        }

        Value::Float { width_log2, bits } => {
            // §4.6: tag = 0x60 | (1..4) where the low nibble is
            // log2(byte_count). bit 3 reserved (must be 0).
            if !(1..=4).contains(width_log2) {
                return Err(EncodeError::Overflow("ieee_float width_log2"));
            }
            let byte_count: usize = 1 << (*width_log2 as usize);
            out.push(0x60 | width_log2);
            // §15.2.1: any NaN bit pattern is rewritten to the canonical
            // quiet-NaN-with-zero-payload at the chosen width before
            // emission. Non-NaN values pass through verbatim.
            let canon_bits = canonicalize_nan_bits(*width_log2, *bits);
            let raw = canon_bits.to_le_bytes();
            out.extend_from_slice(&raw[..byte_count]);
        }

        Value::Decimal { mantissa, scale } => {
            // §4.7: tag = 0x70 | (bc - 1); zigzag mantissa LE; varscale.
            let zz = zigzag_encode_i128(*mantissa);
            let m_bytes = u128_le_minimal_at_least_1(zz);
            let bc = m_bytes.len();
            if bc > 16 {
                return Err(EncodeError::Overflow("decimal mantissa"));
            }
            out.push(0x70 | ((bc - 1) as u8));
            out.extend_from_slice(&m_bytes);
            varscale_encode_into(*scale, out)?;
        }

        Value::Array(children) => {
            // §5.1 generic array.
            //   tag (0xB0)
            //   width byte (slot_w_code in low 2 bits)
            //   value_data (concatenated child encodings)
            //   slot[N] (N × slot_w bytes — offsets within value_data)
            //   count (u16 LE)
            let n = children.len();
            if n > 0xFFFF {
                return Err(EncodeError::Overflow("array count > 65 535 (LIM-001)"));
            }
            // Encode children into a scratch buffer, recording each
            // child's start offset within value_data.
            let mut value_data = Vec::new();
            let mut offsets = Vec::with_capacity(n);
            for child in children {
                offsets.push(value_data.len());
                encode_into(child, &mut value_data)?;
            }
            let value_data_size = value_data.len();
            let (slot_w_code, slot_w) = pick_slot_width(value_data_size)?;
            // Emit.
            out.push(0xB0);
            out.push(slot_w_code);
            out.extend_from_slice(&value_data);
            for off in &offsets {
                let bytes = (*off as u32).to_le_bytes();
                out.extend_from_slice(&bytes[..slot_w]);
            }
            out.extend_from_slice(&(n as u16).to_le_bytes());
        }

        Value::Object(entries) => {
            // §5.2 single object.
            //   tag (0xC0)
            //   width byte (slot_w_code in low 2 bits)
            //   value_data (per-entry: [key_excess varuint?][key bytes][child])
            //   hash[N]    (1 byte each — XXH3-64 low byte, suffix-stripped)
            //   slot[N]    ((slot_w + 1) bytes each: offset_LE + key_size_byte)
            //   count (u16 LE)
            let n = entries.len();
            if n > 0xFFFF {
                return Err(EncodeError::Overflow("object count > 65 535 (LIM-001)"));
            }
            let mut value_data = Vec::new();
            let mut offsets = Vec::with_capacity(n);
            let mut key_size_bytes = Vec::with_capacity(n);
            for (key, child) in entries {
                offsets.push(value_data.len());
                let key_len = key.len();
                if key_len < 0xFF {
                    key_size_bytes.push(key_len as u8);
                    value_data.extend_from_slice(key.as_bytes());
                } else {
                    // Long-key escape: §5.4
                    key_size_bytes.push(0xFF);
                    let excess = key_len - 0xFF;
                    if excess > u32::MAX as usize {
                        return Err(EncodeError::Overflow("key length > u32 + 0xFF (LIM-003)"));
                    }
                    varuint_encode_into(excess as u32, &mut value_data)?;
                    value_data.extend_from_slice(key.as_bytes());
                }
                encode_into(child, &mut value_data)?;
            }
            let value_data_size = value_data.len();
            let (slot_w_code, slot_w) = pick_slot_width(value_data_size)?;
            out.push(0xC0);
            out.push(slot_w_code);
            out.extend_from_slice(&value_data);
            // hash[N]
            for (key, _) in entries {
                out.push(key_hash8(key));
            }
            // slot[N]: offset_LE (slot_w bytes) + key_size_byte (1 byte)
            for (off, ksize) in offsets.iter().zip(key_size_bytes.iter()) {
                let off_bytes = (*off as u32).to_le_bytes();
                out.extend_from_slice(&off_bytes[..slot_w]);
                out.push(*ksize);
            }
            out.extend_from_slice(&(n as u16).to_le_bytes());
        }

        Value::String { encoding_flag, content } => {
            // §4.9: tag = 0x90 | flag (flag ∈ {0, 1}); content N-1 bytes.
            if *encoding_flag > 1 {
                return Err(EncodeError::Overflow("string encoding_flag must be 0 or 1"));
            }
            out.push(0x90 | encoding_flag);
            out.extend_from_slice(content);
        }

        Value::Bytes { encoding_hint, content } => {
            // §4.10: tag = 0xA0 | hint (hint ∈ {0..3}); raw octets.
            if *encoding_hint > 3 {
                return Err(EncodeError::Overflow("bytes encoding_hint must be 0..3"));
            }
            out.push(0xA0 | encoding_hint);
            out.extend_from_slice(content);
        }

        Value::NumericString(inner) => {
            // §4.8: tag = 0x80; body = full encoding of inner numeric.
            if !is_numeric_value(inner) {
                return Err(EncodeError::Overflow("numeric_string inner must be a numeric type (codes 2..7)"));
            }
            out.push(0x80);
            encode_into(inner, out)?;
        }

        Value::Extension { subtype, bytes } => {
            // §4.11: tag = 0xD0 | subtype (0..15); body = opaque bytes.
            if *subtype > 15 {
                return Err(EncodeError::Overflow("extension subtype must be 0..15"));
            }
            out.push(0xD0 | subtype);
            out.extend_from_slice(bytes);
        }

        Value::RowArray { keys, rows } => {
            // §5.2.1 row_array.
            //   tag (0xC1)
            //   width byte: low 2 bits = slot_w_code, bits 2..3 = recoff_w_code
            //   K (varuint)
            //   shared_key_slots[K] (each u32 LE = key_size:8 << 24 | key_offset:24)
            //   hash[K]
            //   shared keys area (sum of key_sizes bytes)
            //   records body (per record: value_data + slot_i[K])
            //   record_offsets[N] (each recoff_w bytes LE)
            //   count (u16 LE)
            let k = keys.len();
            let n = rows.len();
            if k > u32::MAX as usize {
                return Err(EncodeError::Overflow("row_array K too large"));
            }
            if n > 0xFFFF {
                return Err(EncodeError::Overflow("row_array count > 65 535 (LIM-001)"));
            }
            // Validate row shapes.
            for row in rows {
                if row.len() != k {
                    return Err(EncodeError::Overflow("row_array row arity mismatch"));
                }
            }
            // Build shared key slots + keys area.
            let mut keys_area = Vec::new();
            let mut shared_slots = Vec::with_capacity(k);
            for key in keys {
                let offset = keys_area.len();
                if offset > 0x00FF_FFFF {
                    return Err(EncodeError::Overflow("row_array key_offset > u24"));
                }
                let key_size = key.len();
                if key_size > 0xFF {
                    return Err(EncodeError::Overflow("row_array key > 255 bytes (no long-key escape in shared block)"));
                }
                shared_slots.push(((key_size as u32) << 24) | (offset as u32 & 0x00FF_FFFF));
                keys_area.extend_from_slice(key.as_bytes());
            }
            // Encode each record into a per-record buffer + record offsets.
            let mut record_value_data: Vec<Vec<u8>> = Vec::with_capacity(n);
            let mut record_offsets_within: Vec<Vec<usize>> = Vec::with_capacity(n);
            for row in rows {
                let mut value_data = Vec::new();
                let mut offs = Vec::with_capacity(k);
                for v in row {
                    offs.push(value_data.len());
                    encode_into(v, &mut value_data)?;
                }
                record_value_data.push(value_data);
                record_offsets_within.push(offs);
            }
            let max_value_data_size = record_value_data.iter()
                .map(|d| d.len()).max().unwrap_or(0);
            let (slot_w_code, slot_w) = pick_slot_width(max_value_data_size)?;
            // Compute records_body bytes.
            let mut records_body = Vec::new();
            let mut record_offsets: Vec<usize> = Vec::with_capacity(n);
            for (value_data, offs) in record_value_data.iter().zip(record_offsets_within.iter()) {
                record_offsets.push(records_body.len());
                records_body.extend_from_slice(value_data);
                for off in offs {
                    let off_bytes = (*off as u32).to_le_bytes();
                    records_body.extend_from_slice(&off_bytes[..slot_w]);
                }
            }
            let (recoff_w_code, recoff_w) = pick_slot_width(records_body.len())?;
            // Emit.
            out.push(0xC1);
            out.push(slot_w_code | (recoff_w_code << 2));
            varuint_encode_into(k as u32, out)?;
            for slot in &shared_slots {
                out.extend_from_slice(&slot.to_le_bytes());
            }
            for key in keys {
                out.push(key_hash8(key));
            }
            out.extend_from_slice(&keys_area);
            out.extend_from_slice(&records_body);
            for off in &record_offsets {
                let bytes = (*off as u32).to_le_bytes();
                out.extend_from_slice(&bytes[..recoff_w]);
            }
            out.extend_from_slice(&(n as u16).to_le_bytes());
        }

        Value::TypedArray { element_code, raw } => {
            // §5.1.1: tag = 0xB0 | (element_code + 1); raw N × esize
            // bytes LE; count u16 LE.
            if *element_code > 9 {
                return Err(EncodeError::Overflow("typed_array element_code"));
            }
            let esize = typed_array_element_size(*element_code)
                .map_err(EncodeError::Overflow)?;
            if raw.len() % esize != 0 {
                return Err(EncodeError::Overflow("typed_array raw size not a multiple of element size"));
            }
            let n = raw.len() / esize;
            if n > 0xFFFF {
                return Err(EncodeError::Overflow("typed_array count > 65 535"));
            }
            out.push(0xB0 | (element_code + 1));
            out.extend_from_slice(raw);
            out.extend_from_slice(&(n as u16).to_le_bytes());
        }
    }
    Ok(())
}

/// §5.6 — pick the smallest slot width that fits a value_data of the
/// given size.  slot_w_code 0/1/2/3 → byte widths 1/2/3/4.
fn pick_slot_width(value_data_size: usize) -> Result<(u8, usize), EncodeError> {
    if value_data_size <= 0xFF              { Ok((0, 1)) }
    else if value_data_size <= 0xFFFF       { Ok((1, 2)) }
    else if value_data_size <= 0xFF_FF_FF   { Ok((2, 3)) }
    else if value_data_size <= 0xFFFF_FFFF  { Ok((3, 4)) }
    else { Err(EncodeError::Overflow("value_data_size > u32 (LIM-002)")) }
}

/// Smallest little-endian byte representation of a u128, with leading
/// zero bytes stripped. Returns an empty Vec for value 0 — callers
/// that need at least one byte should use `u128_le_minimal_at_least_1`.
fn u128_le_minimal(n: u128) -> Vec<u8> {
    if n == 0 {
        return Vec::new();
    }
    let raw = n.to_le_bytes();
    let mut len = 16;
    while len > 0 && raw[len - 1] == 0 {
        len -= 1;
    }
    raw[..len].to_vec()
}

/// Minimal byte count to represent `n` little-endian. Returns the
/// same value as `u128_le_minimal_at_least_1(n).len()` without
/// allocating — used by hot-path size estimators (e.g., D-007 picker).
#[inline]
fn u128_minimal_byte_count(n: u128) -> usize {
    if n == 0 { 1 }
    else { (((128 - n.leading_zeros()) as usize) + 7) / 8 }
}

fn u128_le_minimal_at_least_1(n: u128) -> Vec<u8> {
    let mut v = u128_le_minimal(n);
    if v.is_empty() {
        v.push(0);
    }
    v
}

/// §15.2.1 — Canonical NaN bit patterns.
///
/// IEEE-754 admits many bit patterns for "a NaN" (exponent all 1s,
/// mantissa non-zero). For determinism and content-addressability,
/// pjson chooses exactly one: quiet NaN with zero payload. If
/// `bits` (interpreted at the given width) is NaN but not the
/// canonical pattern, this function returns the canonical bits.
/// Non-NaN values (including ±Inf, ±0, finite values) pass
/// through unchanged.
fn canonicalize_nan_bits(width_log2: u8, bits: u128) -> u128 {
    // (exp_mask, mant_mask, canon_nan) per width.
    let (exp_mask, mant_mask, canon): (u128, u128, u128) = match width_log2 {
        1 => (0x7C00_u128,                     0x03FF_u128,                     0x7E00_u128),
        2 => (0x7F80_0000_u128,                0x007F_FFFF_u128,                0x7FC0_0000_u128),
        3 => (0x7FF0_0000_0000_0000_u128,      0x000F_FFFF_FFFF_FFFF_u128,      0x7FF8_0000_0000_0000_u128),
        4 => (0x7FFF_0000_0000_0000_0000_0000_0000_0000_u128,
              0x0000_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF_u128,
              0x7FFF_8000_0000_0000_0000_0000_0000_0000_u128),
        _ => return bits,
    };
    let is_nan = (bits & exp_mask) == exp_mask && (bits & mant_mask) != 0;
    if is_nan { canon } else { bits }
}

/// Zigzag encoding of a signed integer (per §4.7's varscale and
/// decimal mantissa). Maps -1 → 1, 1 → 2, -2 → 3, 2 → 4, ...
fn zigzag_encode_i128(v: i128) -> u128 {
    ((v << 1) ^ (v >> 127)) as u128
}

fn zigzag_decode_u128(z: u128) -> i128 {
    ((z >> 1) as i128) ^ -((z & 1) as i128)
}

/// 2-bit-prefix variable-length signed integer per §4.7.1.
///
/// Layout:
///   first byte:  bits 7..6 = total_bytes - 1 (0..3); bits 5..0 = low 6 bits of zigzag(s)
///   subsequent:  next 8 bits of zigzag(s) per byte, little-endian
///
/// Capacities by total_bytes: 1=6 bits, 2=14, 3=22, 4=30.
/// §4.7.1 varscale — signed wrapper over varuint via zigzag.
fn varscale_encode_into(scale: i32, out: &mut Vec<u8>) -> Result<(), EncodeError> {
    varuint_encode_into(zigzag_encode_i32(scale), out)
}

fn zigzag_encode_i32(v: i32) -> u32 {
    ((v << 1) ^ (v >> 31)) as u32
}

fn zigzag_decode_u32(z: u32) -> i32 {
    ((z >> 1) as i32) ^ -((z & 1) as i32)
}

// ── Decode (§10 reference algorithm) ────────────────────────────────

#[derive(Debug)]
enum DecodeError {
    Truncated(&'static str),
    ReservedTag(u8),
    ReservedLowNibble(&'static str, u8),
    BadVarscale,
    NegintZero,
    NintInlineZero,
    BadWidth(u8),
    NotImplementedYet(&'static str),
}

impl std::fmt::Display for DecodeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            DecodeError::Truncated(s)            => write!(f, "truncated: {}", s),
            DecodeError::ReservedTag(b)          => write!(f, "reserved tag 0x{:02X}", b),
            DecodeError::ReservedLowNibble(t, n) => write!(f, "reserved low_nibble for {}: 0x{:X}", t, n),
            DecodeError::BadVarscale             => write!(f, "malformed varscale"),
            DecodeError::NegintZero              => write!(f, "negint with all-zero payload (negative zero reserved)"),
            DecodeError::NintInlineZero          => write!(f, "nint_inline with low_nibble 0 (negative zero reserved)"),
            DecodeError::BadWidth(w)             => write!(f, "ieee_float bad width selector {}", w),
            DecodeError::NotImplementedYet(s)    => write!(f, "Phase ≥2: {}", s),
        }
    }
}

/// LIM-006 — suggested default cap on recursive container nesting.
/// Decoders refuse to descend past this depth so an adversarial wire
/// can't blow the parser's stack via runaway recursion.
const MAX_DECODE_DEPTH: u32 = 256;

fn decode(buf: &[u8]) -> Result<Value, DecodeError> {
    decode_at_depth(buf, 0)
}

fn decode_at_depth(buf: &[u8], depth: u32) -> Result<Value, DecodeError> {
    if depth >= MAX_DECODE_DEPTH {
        return Err(DecodeError::Truncated("nesting depth exceeded (LIM-006)"));
    }
    if buf.is_empty() {
        return Err(DecodeError::Truncated("empty buffer"));
    }
    let tag = buf[0];
    let high = tag >> 4;
    let low = tag & 0x0F;

    match high {
        0 => {
            if low != 0 {
                return Err(DecodeError::ReservedLowNibble("null", low));
            }
            Ok(Value::Null)
        }
        1 => match low {
            0 => Ok(Value::Bool(false)),
            1 => Ok(Value::Bool(true)),
            _ => Err(DecodeError::ReservedLowNibble("bool", low)),
        },
        2 => Ok(Value::Uint(low as u128)),
        3 => {
            if low == 0 {
                return Err(DecodeError::NintInlineZero);
            }
            Ok(Value::NegInt(low as u128))
        }
        4 => {
            // uint
            let bc = (low as usize) + 1;
            if buf.len() < 1 + bc {
                return Err(DecodeError::Truncated("uint magnitude"));
            }
            Ok(Value::Uint(read_u128_le(&buf[1..1 + bc])))
        }
        5 => {
            // negint
            let bc = (low as usize) + 1;
            if buf.len() < 1 + bc {
                return Err(DecodeError::Truncated("negint magnitude"));
            }
            let mag = read_u128_le(&buf[1..1 + bc]);
            if mag == 0 {
                return Err(DecodeError::NegintZero);
            }
            Ok(Value::NegInt(mag))
        }
        6 => {
            // ieee_float
            if low & 0x08 != 0 {
                return Err(DecodeError::ReservedLowNibble("ieee_float (bit 3)", low));
            }
            let width_log2 = low & 0x07;
            if !(1..=4).contains(&width_log2) {
                return Err(DecodeError::BadWidth(width_log2));
            }
            let byte_count: usize = 1 << (width_log2 as usize);
            if buf.len() < 1 + byte_count {
                return Err(DecodeError::Truncated("ieee_float payload"));
            }
            Ok(Value::Float {
                width_log2,
                bits: read_u128_le(&buf[1..1 + byte_count]),
            })
        }
        7 => {
            // decimal
            let bc = (low as usize) + 1;
            if buf.len() < 1 + bc {
                return Err(DecodeError::Truncated("decimal mantissa"));
            }
            let zz = read_u128_le(&buf[1..1 + bc]);
            let mantissa = zigzag_decode_u128(zz);
            let (scale, scale_bytes) = varscale_decode(&buf[1 + bc..])?;
            // Phase 1 validates exact-size match by allowing buf to
            // extend exactly through the decoded bytes; trailing
            // garbage is not flagged here because containers handle
            // that level of bounds-checking in Phase 2.
            let _ = scale_bytes;
            Ok(Value::Decimal { mantissa, scale })
        }
        11 => {
            // §5.1/§5.1.1 array.
            if low == 0 {
                return decode_generic_array(buf, depth);
            }
            if (1..=10).contains(&low) {
                return decode_typed_array(buf, low - 1);
            }
            Err(DecodeError::ReservedLowNibble("array", low))
        }
        12 => {
            if low == 0 { return decode_object(buf, depth); }
            if low == 1 { return decode_row_array(buf, depth); }
            Err(DecodeError::ReservedLowNibble("object", low))
        }
        9 => {
            // §4.9 string. low_nibble = encoding flag.
            if low > 1 {
                return Err(DecodeError::ReservedLowNibble("string", low));
            }
            Ok(Value::String { encoding_flag: low, content: buf[1..].to_vec() })
        }
        10 => {
            // §4.10 bytes.
            if low > 3 {
                return Err(DecodeError::ReservedLowNibble("bytes", low));
            }
            Ok(Value::Bytes { encoding_hint: low, content: buf[1..].to_vec() })
        }
        8 => {
            // §4.8 numeric_string. low_nibble must be 0; body is an
            // inner numeric value (codes 2..7).
            if low != 0 {
                return Err(DecodeError::ReservedLowNibble("numeric_string", low));
            }
            let inner = decode_at_depth(&buf[1..], depth + 1)?;
            if !is_numeric_value(&inner) {
                return Err(DecodeError::Truncated("numeric_string inner must be numeric (codes 2..7)"));
            }
            Ok(Value::NumericString(Box::new(inner)))
        }
        13 => {
            // §4.11 extension. low_nibble is the sub-type id; rest of
            // the buffer is the opaque body. Unknown sub-type ids
            // surface as Extension(id, bytes) — they DO NOT error.
            Ok(Value::Extension { subtype: low, bytes: buf[1..].to_vec() })
        }
        14 | 15 => Err(DecodeError::ReservedTag(tag)),
        _ => unreachable!(),
    }
}

/// §5.1 generic-array decode. Buffer starts at the tag byte (0xB0).
fn decode_generic_array(buf: &[u8], depth: u32) -> Result<Value, DecodeError> {
    if buf.len() < 4 {
        // tag + width + 0 slots + count(2) = 4 bytes minimum
        return Err(DecodeError::Truncated("array minimum size"));
    }
    let width_byte = buf[1];
    let slot_w_code = (width_byte & 0x03) as usize;
    if (width_byte & 0xFC) != 0 {
        return Err(DecodeError::ReservedLowNibble("array width byte high bits", width_byte));
    }
    let slot_w = slot_w_code + 1;     // codes 0..3 → widths 1..4
    let n = u16::from_le_bytes([buf[buf.len() - 2], buf[buf.len() - 1]]) as usize;
    // value_data_size = container - 2 (tag + width) - slot_w*N - 2 (count)
    let overhead = 4 + slot_w * n;
    if buf.len() < overhead {
        return Err(DecodeError::Truncated("array slots/count"));
    }
    let value_data_size = buf.len() - overhead;
    // Bounds: pick_slot_width's choice must agree with value_data_size.
    let expected_slot_w = match value_data_size {
        0..=0xFF              => 1,
        0x100..=0xFFFF        => 2,
        0x1_0000..=0xFF_FFFF  => 3,
        _                     => 4,
    };
    if slot_w != expected_slot_w {
        // Note: the spec requires slot_w match value_data_size; a buffer
        // with a wider-than-needed slot is technically non-canonical
        // but the spec only mandates encoder compliance, not decoder
        // rejection.  Phase 1 driver accepts any matching slot width
        // (and rejects a slot too small to hold the offsets, since
        // those wouldn't fit).
        // For now: enforce strict canonical, which the encoder
        // produces.  Strict-canonical decoder is the conservative pick.
        if slot_w < expected_slot_w {
            return Err(DecodeError::Truncated("slot width too small for value_data"));
        }
        // wider-than-needed: tolerate (non-canonical but well-formed)
    }
    let value_data_start = 2;
    let slot_table_start = value_data_start + value_data_size;
    let mut children = Vec::with_capacity(n);
    for i in 0..n {
        let slot_pos = slot_table_start + i * slot_w;
        let slot_bytes = &buf[slot_pos..slot_pos + slot_w];
        let mut off_buf = [0u8; 4];
        off_buf[..slot_w].copy_from_slice(slot_bytes);
        let off = u32::from_le_bytes(off_buf) as usize;
        let next_off = if i + 1 < n {
            let np = slot_table_start + (i + 1) * slot_w;
            let nb = &buf[np..np + slot_w];
            let mut nbuf = [0u8; 4];
            nbuf[..slot_w].copy_from_slice(nb);
            u32::from_le_bytes(nbuf) as usize
        } else {
            value_data_size
        };
        if off > value_data_size || next_off < off || next_off > value_data_size {
            return Err(DecodeError::Truncated("array slot offset OOB or non-monotonic"));
        }
        let child_size = next_off - off;
        let child = decode_at_depth(&buf[value_data_start + off..value_data_start + off + child_size], depth + 1)?;
        children.push(child);
    }
    Ok(Value::Array(children))
}

/// §5.2.1 row_array decode. `buf` starts at the tag byte (0xC1).
fn decode_row_array(buf: &[u8], depth: u32) -> Result<Value, DecodeError> {
    // Minimum: tag + width + K_varuint(1) + count(2) = 5 bytes (K=0, N=0).
    if buf.len() < 5 {
        return Err(DecodeError::Truncated("row_array minimum size"));
    }
    let width_byte = buf[1];
    let slot_w_code = (width_byte & 0x03) as usize;
    let recoff_w_code = ((width_byte >> 2) & 0x03) as usize;
    if (width_byte & 0xF0) != 0 {
        return Err(DecodeError::ReservedLowNibble("row_array width byte high bits", width_byte));
    }
    let slot_w = slot_w_code + 1;
    let recoff_w = recoff_w_code + 1;
    let n = u16::from_le_bytes([buf[buf.len() - 2], buf[buf.len() - 1]]) as usize;

    // Read K (varuint at offset 2).
    let (k_u32, k_used) = varuint_decode(&buf[2..])?;
    let k = k_u32 as usize;
    let mut pos = 2 + k_used;

    // Shared key slots: K × u32 LE.
    let need_slots = k * 4;
    if buf.len() < pos + need_slots {
        return Err(DecodeError::Truncated("row_array shared key slots"));
    }
    let mut key_slots: Vec<(u32, u8)> = Vec::with_capacity(k);
    for i in 0..k {
        let s_pos = pos + i * 4;
        let s = u32::from_le_bytes([buf[s_pos], buf[s_pos + 1], buf[s_pos + 2], buf[s_pos + 3]]);
        let key_offset = s & 0x00FF_FFFF;
        let key_size = (s >> 24) as u8;
        key_slots.push((key_offset, key_size));
    }
    pos += need_slots;

    // hash[K]
    if buf.len() < pos + k {
        return Err(DecodeError::Truncated("row_array hash array"));
    }
    let hash_start = pos;
    pos += k;

    // Shared keys area: sum of key_sizes.
    let total_key_size: usize = key_slots.iter().map(|(_, s)| *s as usize).sum();
    if buf.len() < pos + total_key_size {
        return Err(DecodeError::Truncated("row_array shared keys area"));
    }
    let keys_area_start = pos;
    pos += total_key_size;

    // Resolve keys into Strings, verify against hashes.
    let mut keys: Vec<String> = Vec::with_capacity(k);
    for (i, (off, ksize)) in key_slots.iter().enumerate() {
        let off = *off as usize;
        let ks = *ksize as usize;
        if off + ks > total_key_size {
            return Err(DecodeError::Truncated("row_array key slot OOB"));
        }
        let key_bytes = &buf[keys_area_start + off..keys_area_start + off + ks];
        let key = std::str::from_utf8(key_bytes)
            .map_err(|_| DecodeError::Truncated("row_array key UTF-8"))?
            .to_string();
        if buf[hash_start + i] != key_hash8(&key) {
            return Err(DecodeError::Truncated("row_array hash byte mismatch"));
        }
        keys.push(key);
    }

    // record_offsets[N] live just before the count u16 at the tail.
    let record_offsets_end = buf.len() - 2;
    let record_offsets_start = record_offsets_end.checked_sub(n * recoff_w)
        .ok_or(DecodeError::Truncated("row_array record_offsets"))?;
    if record_offsets_start < pos {
        return Err(DecodeError::Truncated("row_array record_offsets overlap header"));
    }
    let records_body_start = pos;
    let records_body_end = record_offsets_start;
    let records_body_size = records_body_end - records_body_start;

    let read_recoff = |i: usize| -> usize {
        let p = record_offsets_start + i * recoff_w;
        let mut buf4 = [0u8; 4];
        buf4[..recoff_w].copy_from_slice(&buf[p..p + recoff_w]);
        u32::from_le_bytes(buf4) as usize
    };

    // Walk each record.
    let mut rows: Vec<Vec<Value>> = Vec::with_capacity(n);
    for i in 0..n {
        let rec_off = read_recoff(i);
        let next_off = if i + 1 < n { read_recoff(i + 1) } else { records_body_size };
        if rec_off > records_body_size || next_off < rec_off || next_off > records_body_size {
            return Err(DecodeError::Truncated("row_array record offset OOB or non-monotonic"));
        }
        let rec_size = next_off - rec_off;
        if rec_size < k * slot_w {
            return Err(DecodeError::Truncated("row_array record too small for slot table"));
        }
        let value_data_size = rec_size - k * slot_w;
        let rec_start = records_body_start + rec_off;
        let slot_table_start = rec_start + value_data_size;

        let read_slot = |j: usize| -> usize {
            let p = slot_table_start + j * slot_w;
            let mut buf4 = [0u8; 4];
            buf4[..slot_w].copy_from_slice(&buf[p..p + slot_w]);
            u32::from_le_bytes(buf4) as usize
        };

        let mut row: Vec<Value> = Vec::with_capacity(k);
        for j in 0..k {
            let off = read_slot(j);
            let next_field_off = if j + 1 < k { read_slot(j + 1) } else { value_data_size };
            if off > value_data_size || next_field_off < off || next_field_off > value_data_size {
                return Err(DecodeError::Truncated("row_array slot offset OOB or non-monotonic"));
            }
            let val_bytes = &buf[rec_start + off..rec_start + next_field_off];
            row.push(decode_at_depth(val_bytes, depth + 1)?);
        }
        rows.push(row);
    }

    Ok(Value::RowArray { keys, rows })
}

/// §5.2 object decode. `buf` starts at the tag byte (0xC0).
fn decode_object(buf: &[u8], depth: u32) -> Result<Value, DecodeError> {
    if buf.len() < 4 {
        return Err(DecodeError::Truncated("object minimum size"));
    }
    let width_byte = buf[1];
    let slot_w_code = (width_byte & 0x03) as usize;
    if (width_byte & 0xFC) != 0 {
        return Err(DecodeError::ReservedLowNibble("object width byte high bits", width_byte));
    }
    let slot_w = slot_w_code + 1;
    let n = u16::from_le_bytes([buf[buf.len() - 2], buf[buf.len() - 1]]) as usize;
    // Total: tag(1) + width(1) + V + hash(N) + slot((slot_w+1)*N) + count(2)
    let overhead = 4 + n + (slot_w + 1) * n;
    if buf.len() < overhead {
        return Err(DecodeError::Truncated("object slots/hash/count"));
    }
    let value_data_size = buf.len() - overhead;
    let value_data_start = 2;
    let hash_table_start = value_data_start + value_data_size;
    let slot_table_start = hash_table_start + n;

    let mut entries = Vec::with_capacity(n);
    for i in 0..n {
        let slot_pos = slot_table_start + i * (slot_w + 1);
        // Offset (slot_w bytes LE).
        let mut off_buf = [0u8; 4];
        off_buf[..slot_w].copy_from_slice(&buf[slot_pos..slot_pos + slot_w]);
        let off = u32::from_le_bytes(off_buf) as usize;
        // key_size_byte
        let ksize_byte = buf[slot_pos + slot_w];
        // Next entry's offset (or value_data_size for last).
        let next_off = if i + 1 < n {
            let np = slot_table_start + (i + 1) * (slot_w + 1);
            let mut nb = [0u8; 4];
            nb[..slot_w].copy_from_slice(&buf[np..np + slot_w]);
            u32::from_le_bytes(nb) as usize
        } else {
            value_data_size
        };
        if off > value_data_size || next_off < off || next_off > value_data_size {
            return Err(DecodeError::Truncated("object slot offset OOB or non-monotonic"));
        }
        let entry_size = next_off - off;
        let entry = &buf[value_data_start + off..value_data_start + off + entry_size];

        // Decode key.
        let (key, key_size, key_prefix_size) = if ksize_byte != 0xFF {
            let ks = ksize_byte as usize;
            if ks > entry.len() {
                return Err(DecodeError::Truncated("object key bytes"));
            }
            (
                std::str::from_utf8(&entry[..ks])
                    .map_err(|_| DecodeError::Truncated("object key UTF-8"))?
                    .to_string(),
                ks,
                0,
            )
        } else {
            // Long-key escape: §5.4
            let (excess, prefix) = varuint_decode(entry)?;
            let ks = 0xFFusize + excess as usize;
            if prefix + ks > entry.len() {
                return Err(DecodeError::Truncated("object long-key bytes"));
            }
            (
                std::str::from_utf8(&entry[prefix..prefix + ks])
                    .map_err(|_| DecodeError::Truncated("object long-key UTF-8"))?
                    .to_string(),
                ks,
                prefix,
            )
        };

        // Verify hash byte.
        let stored_hash = buf[hash_table_start + i];
        let computed = key_hash8(&key);
        if stored_hash != computed {
            return Err(DecodeError::Truncated("object hash byte mismatch"));
        }

        // Decode child.
        let child_start = key_prefix_size + key_size;
        let child = decode_at_depth(&entry[child_start..], depth + 1)?;
        entries.push((key, child));
    }
    Ok(Value::Object(entries))
}

/// §5.1.1 typed-array decode. `buf` starts at the tag byte; the
/// caller has already extracted element_code from the low nibble.
fn decode_typed_array(buf: &[u8], element_code: u8) -> Result<Value, DecodeError> {
    let esize = typed_array_element_size(element_code)
        .map_err(DecodeError::Truncated)?;
    if buf.len() < 3 {
        // tag + 0 elements + count(2) = 3 bytes minimum
        return Err(DecodeError::Truncated("typed_array minimum size"));
    }
    let n = u16::from_le_bytes([buf[buf.len() - 2], buf[buf.len() - 1]]) as usize;
    let body_len = n.checked_mul(esize)
        .ok_or(DecodeError::Truncated("typed_array body overflow"))?;
    let expected = 1 + body_len + 2;
    if buf.len() != expected {
        return Err(DecodeError::Truncated("typed_array size mismatch"));
    }
    Ok(Value::TypedArray {
        element_code,
        raw: buf[1..1 + body_len].to_vec(),
    })
}

fn read_u128_le(bytes: &[u8]) -> u128 {
    let mut v: u128 = 0;
    for (i, b) in bytes.iter().enumerate().take(16) {
        v |= (*b as u128) << (8 * i);
    }
    v
}

fn varscale_decode(buf: &[u8]) -> Result<(i32, usize), DecodeError> {
    let (zz, used) = varuint_decode(buf)?;
    Ok((zigzag_decode_u32(zz), used))
}

// ── JSON projection ─────────────────────────────────────────────────

// ── §7.5 JSON emitter options ───────────────────────────────────────

/// `int_string_mode` (§7.5):
///   - Never: every bare integer unquoted (default).
///   - LargeOnly: bare integers with |v| > 2^53-1 quoted; smaller bare.
///   - All: every bare integer quoted regardless of magnitude.
/// Always: `numeric_string` (§4.8) is quoted regardless of mode.
/// Always: `ieee_float` and `decimal` are unaffected.
#[derive(Debug, Clone, Copy, PartialEq)]
enum IntStringMode { Never, LargeOnly, All }

/// `EmitOptions` controls per-call JSON-projection behavior.
#[derive(Debug, Clone, Copy)]
struct EmitOptions {
    pretty: bool,
    indent: u8,                    // spaces per level when pretty=true; 0 = tabs
    int_string_mode: IntStringMode,
}

impl Default for EmitOptions {
    fn default() -> Self {
        Self { pretty: false, indent: 2, int_string_mode: IntStringMode::Never }
    }
}

const JS_MAX_SAFE_INTEGER: u128 = (1u128 << 53) - 1;

/// Returns true if a bare integer of magnitude `value_abs` should be
/// emitted quoted under the given `mode`.
fn quote_int_for_mode(value_abs: u128, mode: IntStringMode) -> bool {
    match mode {
        IntStringMode::Never     => false,
        IntStringMode::All       => true,
        IntStringMode::LargeOnly => value_abs > JS_MAX_SAFE_INTEGER,
    }
}

/// Pretty-print indentation: writes a newline + N indent units to
/// `out`. No-op when `pretty=false`.
fn write_indent(opts: &EmitOptions, depth: usize, out: &mut String) {
    if !opts.pretty { return; }
    out.push('\n');
    if opts.indent == 0 {
        for _ in 0..depth { out.push('\t'); }
    } else {
        for _ in 0..(depth * opts.indent as usize) { out.push(' '); }
    }
}

/// Default render — compact, no quoting of bare integers. Equivalent
/// to `render_json_with(v, &EmitOptions::default())`.
fn render_json(v: &Value) -> String {
    match v {
        Value::Null => "null".to_string(),
        Value::Bool(true) => "true".to_string(),
        Value::Bool(false) => "false".to_string(),
        Value::Uint(n) => format!("{}", n),
        Value::NegInt(mag) => {
            // -mag, but mag is u128 so we have to be careful with i128::MIN.
            if *mag <= i128::MAX as u128 + 1 {
                if *mag == i128::MAX as u128 + 1 {
                    "-170141183460469231731687303715884105728".to_string()
                } else {
                    format!("-{}", mag)
                }
            } else {
                // u128-magnitude negint exceeds i128::MIN range; emit as bigint
                format!("-{}", mag)
            }
        }
        Value::Float { width_log2, bits } => {
            // Phase 1 JSON projection: render the value as the
            // shortest decimal that round-trips at the source width.
            // Special cases: f16/f32 are widened to f64 for printing.
            let f = match width_log2 {
                3 => f64::from_bits(*bits as u64),
                2 => f32::from_bits(*bits as u32) as f64,
                1 => f16_bits_to_f64(*bits as u16),
                4 => f128_bits_to_f64(*bits),
                _ => f64::NAN,
            };
            // JSON forbids NaN and ±Inf; emit a non-conforming token
            // here so fixture comparisons surface the issue (an
            // emitter targeting strict JSON would reject the value).
            if f.is_nan() {
                "NaN".to_string()
            } else if f == f64::INFINITY {
                "Infinity".to_string()
            } else if f == f64::NEG_INFINITY {
                "-Infinity".to_string()
            } else if f == f.trunc() && f.abs() < 1e16 {
                // Whole-number float — emit without trailing ".0"
                // unless that would change parsing.
                format!("{:.0}", f)
            } else {
                format!("{}", f)
            }
        }
        Value::Decimal { mantissa, scale } => format_decimal(*mantissa, *scale),
        Value::Array(children) => {
            let mut s = String::from("[");
            for (i, c) in children.iter().enumerate() {
                if i > 0 { s.push(','); }
                s.push_str(&render_json(c));
            }
            s.push(']');
            s
        }
        Value::String { encoding_flag, content } => {
            let text = std::str::from_utf8(content).unwrap_or("<invalid utf8>");
            let mut s = String::with_capacity(text.len() + 2);
            s.push('"');
            if *encoding_flag == 0 {
                json_escape_into(text, &mut s);
            } else {
                // escape_form — content already JSON-escape-encoded.
                s.push_str(text);
            }
            s.push('"');
            s
        }
        Value::NumericString(inner) => {
            // §4.8: render inner as canonical decimal, wrap in quotes.
            let body = render_json(inner);
            let mut s = String::with_capacity(body.len() + 2);
            s.push('"');
            s.push_str(&body);
            s.push('"');
            s
        }
        Value::Extension { subtype, bytes } => {
            // §4.11: emit as opaque envelope object so a v1 emitter
            // round-trips an unknown extension without losing the id
            // or the body. Subsequent encoders aware of the envelope
            // (see from_json) reconstruct the Extension value.
            format!(
                r#"{{"__pjson_ext":{{"subtype":{},"bytes_b64":"{}"}}}}"#,
                subtype, b64_encode(bytes)
            )
        }
        Value::Bytes { encoding_hint, content } => {
            let body = match *encoding_hint {
                0 => b64_encode(content),
                1 => hex_encode(content),
                2 => base58_encode(content),
                3 => b64url_encode(content),
                _ => "<bad bytes hint>".to_string(),
            };
            let mut s = String::with_capacity(body.len() + 2);
            s.push('"');
            s.push_str(&body);
            s.push('"');
            s
        }
        Value::Object(entries) => {
            let mut s = String::from("{");
            for (i, (key, value)) in entries.iter().enumerate() {
                if i > 0 { s.push(','); }
                s.push('"');
                json_escape_into(key, &mut s);
                s.push('"');
                s.push(':');
                s.push_str(&render_json(value));
            }
            s.push('}');
            s
        }
        Value::RowArray { keys, rows } => {
            // Render as a JSON array of objects: [{"k1":v1,"k2":v2}, ...]
            let mut s = String::from("[");
            for (i, row) in rows.iter().enumerate() {
                if i > 0 { s.push(','); }
                s.push('{');
                for (j, value) in row.iter().enumerate() {
                    if j > 0 { s.push(','); }
                    s.push('"');
                    s.push_str(&keys[j]);
                    s.push('"');
                    s.push(':');
                    s.push_str(&render_json(value));
                }
                s.push('}');
            }
            s.push(']');
            s
        }
        Value::TypedArray { element_code, raw } => {
            let esize = typed_array_element_size(*element_code).unwrap_or(1);
            let n = raw.len() / esize;
            let mut s = String::from("[");
            for i in 0..n {
                if i > 0 { s.push(','); }
                let elem = &raw[i * esize..(i + 1) * esize];
                let rendered = match *element_code {
                    0 => format!("{}", i8::from_le_bytes([elem[0]])),
                    1 => format!("{}", i16::from_le_bytes([elem[0], elem[1]])),
                    2 => format!("{}", i32::from_le_bytes([elem[0], elem[1], elem[2], elem[3]])),
                    3 => format!("{}", i64::from_le_bytes(elem.try_into().unwrap())),
                    4 => format!("{}", u8::from_le_bytes([elem[0]])),
                    5 => format!("{}", u16::from_le_bytes([elem[0], elem[1]])),
                    6 => format!("{}", u32::from_le_bytes([elem[0], elem[1], elem[2], elem[3]])),
                    7 => format!("{}", u64::from_le_bytes(elem.try_into().unwrap())),
                    8 => {
                        let f = f32::from_le_bytes([elem[0], elem[1], elem[2], elem[3]]);
                        format_typed_float(f as f64)
                    }
                    9 => {
                        let f = f64::from_le_bytes(elem.try_into().unwrap());
                        format_typed_float(f)
                    }
                    _ => "?".to_string(),
                };
                s.push_str(&rendered);
            }
            s.push(']');
            s
        }
    }
}

/// §7.5 emitter with options. Differs from `render_json` only for:
///   - bare integers (Uint, NegInt) — quoted per `int_string_mode`
///   - aggregates (Array, Object, RowArray, TypedArray) — when
///     `pretty=true`, each element/entry on its own indented line
///   - object keys + JSON-strings — emitted with key escaping; same
///     content as `render_json`
/// `numeric_string` and `bytes` are always quoted regardless of mode.
fn render_json_with(v: &Value, opts: &EmitOptions) -> String {
    let mut out = String::new();
    render_with_opts_into(v, opts, 0, &mut out);
    out
}

fn render_with_opts_into(v: &Value, opts: &EmitOptions, depth: usize, out: &mut String) {
    match v {
        Value::Null     => out.push_str("null"),
        Value::Bool(b)  => out.push_str(if *b { "true" } else { "false" }),
        Value::Uint(n) => {
            let quoted = quote_int_for_mode(*n, opts.int_string_mode);
            if quoted { out.push('"'); }
            out.push_str(&format!("{}", n));
            if quoted { out.push('"'); }
        }
        Value::NegInt(mag) => {
            let quoted = quote_int_for_mode(*mag, opts.int_string_mode);
            if quoted { out.push('"'); }
            out.push('-');
            out.push_str(&format!("{}", mag));
            if quoted { out.push('"'); }
        }
        // For everything else we can fall back to the simple renderer
        // and post-process aggregates for pretty-print indentation.
        Value::Float { .. } | Value::Decimal { .. }
        | Value::String { .. } | Value::NumericString(_) | Value::Bytes { .. }
        | Value::TypedArray { .. } | Value::Extension { .. } => {
            // None of these arms have nested aggregates that pretty-
            // print would affect (TypedArray is flat numbers/floats,
            // never wraps a Value), and none participate in
            // int_string_mode. Reuse the default renderer.
            out.push_str(&render_json(v));
        }
        Value::Array(children) => {
            if children.is_empty() { out.push_str("[]"); return; }
            out.push('[');
            for (i, c) in children.iter().enumerate() {
                if i > 0 { out.push(','); }
                write_indent(opts, depth + 1, out);
                render_with_opts_into(c, opts, depth + 1, out);
            }
            write_indent(opts, depth, out);
            out.push(']');
        }
        Value::Object(entries) => {
            if entries.is_empty() { out.push_str("{}"); return; }
            out.push('{');
            for (i, (key, value)) in entries.iter().enumerate() {
                if i > 0 { out.push(','); }
                write_indent(opts, depth + 1, out);
                out.push('"');
                json_escape_into(key, out);
                out.push('"');
                out.push(':');
                if opts.pretty { out.push(' '); }
                render_with_opts_into(value, opts, depth + 1, out);
            }
            write_indent(opts, depth, out);
            out.push('}');
        }
        Value::RowArray { keys, rows } => {
            if rows.is_empty() { out.push_str("[]"); return; }
            out.push('[');
            for (i, row) in rows.iter().enumerate() {
                if i > 0 { out.push(','); }
                write_indent(opts, depth + 1, out);
                out.push('{');
                for (j, value) in row.iter().enumerate() {
                    if j > 0 { out.push(','); }
                    write_indent(opts, depth + 2, out);
                    out.push('"');
                    json_escape_into(&keys[j], out);
                    out.push('"');
                    out.push(':');
                    if opts.pretty { out.push(' '); }
                    render_with_opts_into(value, opts, depth + 2, out);
                }
                write_indent(opts, depth + 1, out);
                out.push('}');
            }
            write_indent(opts, depth, out);
            out.push(']');
        }
    }
}

fn format_typed_float(f: f64) -> String {
    if f.is_nan() { return "NaN".to_string(); }
    if f == f64::INFINITY { return "Infinity".to_string(); }
    if f == f64::NEG_INFINITY { return "-Infinity".to_string(); }
    if f == f.trunc() && f.abs() < 1e16 {
        format!("{:.0}", f)
    } else {
        format!("{}", f)
    }
}

fn f16_bits_to_f64(bits: u16) -> f64 {
    let sign = (bits >> 15) & 1;
    let exp = (bits >> 10) & 0x1F;
    let mant = (bits & 0x3FF) as u64;
    let sign_bit = (sign as u64) << 63;
    if exp == 0 {
        if mant == 0 {
            return f64::from_bits(sign_bit);
        }
        // subnormal: normalise
        let mut m = mant;
        let mut e: i32 = -14;
        while (m & 0x400) == 0 {
            m <<= 1;
            e -= 1;
        }
        m &= 0x3FF;
        let new_exp = ((e + 1023) as u64) << 52;
        return f64::from_bits(sign_bit | new_exp | (m << (52 - 10)));
    }
    if exp == 0x1F {
        let new_exp = 0x7FFu64 << 52;
        return f64::from_bits(sign_bit | new_exp | (mant << (52 - 10)));
    }
    let new_exp = ((exp as i32 - 15 + 1023) as u64) << 52;
    f64::from_bits(sign_bit | new_exp | (mant << (52 - 10)))
}

/// IEEE-754 binary128 → binary64 narrowing conversion (round-to-
/// nearest-even). Rust port of the vendored SoftFloat subset at
/// `cpp/external/softfloat/psio_softfloat.h`. Mirrors the algorithm
/// step-by-step so the C and Rust drivers agree byte-for-byte.
///
/// Modifications relative to upstream Berkeley SoftFloat-3e match the
/// C side: RNE-only rounding, no exception flags, NaN canonicalized
/// with sign preserved.
fn f128_bits_to_f64(bits: u128) -> f64 {
    let lo = bits as u64;
    let hi = (bits >> 64) as u64;

    let sign = (hi >> 63) & 1;
    let exp = ((hi >> 48) & 0x7FFF) as i32;
    let frac64 = hi & 0x0000_FFFF_FFFF_FFFF; // top 48 bits of mantissa
    let frac0  = lo;                          // bottom 64 bits of mantissa

    if exp == 0x7FFF {
        // ±Inf or NaN.
        if (frac64 | frac0) != 0 {
            // NaN — canonical quiet, sign preserved.
            return f64::from_bits((sign << 63) | 0x7FF8_0000_0000_0000);
        }
        // ±Inf.
        return f64::from_bits((sign << 63) | (0x7FFu64 << 52));
    }

    // softfloat_shortShiftLeft128(frac64, frac0, 14):
    //   z.v64 = (a64 << dist) | (a0 >> (64 - dist));
    //   z.v0  = a0 << dist;
    let shifted_v64 = (frac64 << 14) | (frac0 >> 50);
    let shifted_v0  = frac0 << 14;
    let frac64 = shifted_v64 | (if shifted_v0 != 0 { 1 } else { 0 });

    if exp == 0 && frac64 == 0 {
        return f64::from_bits(sign << 63); // ±0
    }

    let exp = exp - 0x3C01;
    round_pack_to_f64(sign != 0, exp as i16, frac64 | 0x4000_0000_0000_0000)
}

/// Reduced subset of `softfloat_roundPackToF64` — RNE-only, no
/// exception-flag tracking. Verbatim algorithm minus the rounding-
/// mode and tininess-detection branches.
fn round_pack_to_f64(sign: bool, mut exp: i16, mut sig: u64) -> f64 {
    const ROUND_INCREMENT: u64 = 0x200; // RNE: half-LSB

    // The C cast `(uint16_t) exp` makes both negative `exp` (subnormal)
    // and large positive `exp` (overflow) trigger the special block.
    let needs_special = (exp as u16) >= 0x7FD;
    if needs_special {
        if exp < 0 {
            // Subnormal output — shift sig right with sticky-jam.
            sig = shift_right_jam_u64(sig, (-exp) as u32);
            exp = 0;
        } else if exp > 0x7FD || sig.wrapping_add(ROUND_INCREMENT) >= 0x8000_0000_0000_0000 {
            // Overflow → ±inf.
            let sign_bit = if sign { 1u64 << 63 } else { 0 };
            return f64::from_bits(sign_bit | (0x7FFu64 << 52));
        }
    }

    let round_bits = sig & 0x3FF;
    sig = (sig.wrapping_add(ROUND_INCREMENT)) >> 10;
    // Tie-to-even: clear the LSB when round_bits is exactly 0x200.
    sig &= !(if (round_bits ^ 0x200) == 0 { 1u64 } else { 0 });
    if sig == 0 { exp = 0; }

    let sign_bit = if sign { 1u64 << 63 } else { 0 };
    // packToF64UI uses `+` so sig's bit 52 (the implicit-1 carrier)
    // carries into the exponent field — that's why exp comes in
    // 1 less than the f64-encoded exponent.
    f64::from_bits(sign_bit
                       .wrapping_add((exp as u64) << 52)
                       .wrapping_add(sig))
}

/// Shifts `a` right by `dist` (which must not be zero). Any nonzero
/// bits shifted off are jammed into the LSB. Verbatim from upstream
/// `softfloat_shiftRightJam64`.
fn shift_right_jam_u64(a: u64, dist: u32) -> u64 {
    if dist < 63 {
        let neg_dist = ((-(dist as i32)) & 63) as u32;
        (a >> dist) | (if (a << neg_dist) != 0 { 1 } else { 0 })
    } else if a != 0 {
        1
    } else {
        0
    }
}

// ── Base-N encoding helpers for §4.10 bytes JSON emit ───────────────

/// Standard base64 (§4.10 hint 0). RFC 4648 §4 alphabet, `=` padding.
fn b64_encode(bytes: &[u8]) -> String {
    const A: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity((bytes.len() + 2) / 3 * 4);
    let mut chunks = bytes.chunks_exact(3);
    for chunk in chunks.by_ref() {
        let n = ((chunk[0] as u32) << 16) | ((chunk[1] as u32) << 8) | (chunk[2] as u32);
        out.push(A[((n >> 18) & 0x3F) as usize] as char);
        out.push(A[((n >> 12) & 0x3F) as usize] as char);
        out.push(A[((n >> 6) & 0x3F) as usize] as char);
        out.push(A[(n & 0x3F) as usize] as char);
    }
    let rem = chunks.remainder();
    match rem.len() {
        1 => {
            let n = (rem[0] as u32) << 16;
            out.push(A[((n >> 18) & 0x3F) as usize] as char);
            out.push(A[((n >> 12) & 0x3F) as usize] as char);
            out.push('=');
            out.push('=');
        }
        2 => {
            let n = ((rem[0] as u32) << 16) | ((rem[1] as u32) << 8);
            out.push(A[((n >> 18) & 0x3F) as usize] as char);
            out.push(A[((n >> 12) & 0x3F) as usize] as char);
            out.push(A[((n >> 6) & 0x3F) as usize] as char);
            out.push('=');
        }
        _ => {}
    }
    out
}

/// Standard base64 decode. Returns None on any malformed input
/// (illegal char, wrong padding). Used by the §4.11 envelope
/// round-trip on the JSON-ingress side.
fn b64_decode(s: &str) -> Option<Vec<u8>> {
    fn val(c: u8) -> Option<u8> {
        match c {
            b'A'..=b'Z' => Some(c - b'A'),
            b'a'..=b'z' => Some(26 + c - b'a'),
            b'0'..=b'9' => Some(52 + c - b'0'),
            b'+' => Some(62),
            b'/' => Some(63),
            _ => None,
        }
    }
    let s = s.as_bytes();
    if s.len() % 4 != 0 { return None; }
    let mut out = Vec::with_capacity(s.len() / 4 * 3);
    let mut i = 0;
    while i < s.len() {
        let q0 = val(s[i])?;
        let q1 = val(s[i + 1])?;
        let q2_pad = s[i + 2] == b'=';
        let q3_pad = s[i + 3] == b'=';
        let q2 = if q2_pad { 0 } else { val(s[i + 2])? };
        let q3 = if q3_pad { 0 } else { val(s[i + 3])? };
        let n: u32 = ((q0 as u32) << 18) | ((q1 as u32) << 12)
                   | ((q2 as u32) << 6)  | (q3 as u32);
        out.push((n >> 16) as u8);
        if !q2_pad { out.push((n >> 8) as u8); }
        if !q3_pad { out.push(n as u8); }
        i += 4;
    }
    Some(out)
}

fn hex_decode(s: &str) -> Option<Vec<u8>> {
    if s.len() % 2 != 0 { return None; }
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(s.len() / 2);
    let nyb = |c: u8| -> Option<u8> {
        match c {
            b'0'..=b'9' => Some(c - b'0'),
            b'a'..=b'f' => Some(10 + c - b'a'),
            b'A'..=b'F' => Some(10 + c - b'A'),
            _ => None,
        }
    };
    for chunk in bytes.chunks(2) {
        out.push((nyb(chunk[0])? << 4) | nyb(chunk[1])?);
    }
    Some(out)
}

fn b64url_decode(s: &str) -> Option<Vec<u8>> {
    // Re-pad and translate to standard alphabet, then reuse b64_decode.
    let mut std: String = s.chars()
        .map(|c| match c { '-' => '+', '_' => '/', other => other })
        .collect();
    while std.len() % 4 != 0 { std.push('='); }
    b64_decode(&std)
}

fn base58_decode(s: &str) -> Option<Vec<u8>> {
    const A: &[u8; 58] = b"123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    let val = |c: u8| -> Option<u8> {
        A.iter().position(|&x| x == c).map(|p| p as u8)
    };
    let bytes = s.as_bytes();
    let zeros = bytes.iter().take_while(|&&c| c == b'1').count();
    // Output byte count ≤ ceil(payload_len * log(58)/log(256)) ≈ 0.733.
    // Reserve ahead so the inner mul-add loop never reallocates.
    let payload = bytes.len() - zeros;
    let cap = (payload * 733) / 1000 + 1;
    // Internal accumulator is little-endian — append-only during the
    // mul-add loop, then reverse once before returning.
    let mut acc: Vec<u8> = Vec::with_capacity(cap);
    for &c in &bytes[zeros..] {
        let mut carry = val(c)? as u32;
        for byte in acc.iter_mut() {
            carry += (*byte as u32) * 58;
            *byte = (carry & 0xFF) as u8;
            carry >>= 8;
        }
        while carry > 0 {
            acc.push((carry & 0xFF) as u8);
            carry >>= 8;
        }
    }
    acc.reverse();
    let mut out = Vec::with_capacity(zeros + acc.len());
    out.resize(zeros, 0);
    out.extend_from_slice(&acc);
    Some(out)
}

/// URL-safe base64 (§4.10 hint 3). RFC 4648 §5 alphabet, no padding.
fn b64url_encode(bytes: &[u8]) -> String {
    let std = b64_encode(bytes);
    std.trim_end_matches('=')
        .chars()
        .map(|c| match c { '+' => '-', '/' => '_', other => other })
        .collect()
}

/// Lowercase hex (§4.10 hint 1).
fn hex_encode(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        out.push_str(&format!("{:02x}", b));
    }
    out
}

/// Base58 with the Bitcoin alphabet (§4.10 hint 2). No `0`/`O`/`I`/`l`.
fn base58_encode(bytes: &[u8]) -> String {
    const A: &[u8; 58] = b"123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    if bytes.is_empty() { return String::new(); }
    // Count leading zeros.
    let zeros = bytes.iter().take_while(|&&b| b == 0).count();
    // Convert big-endian bytes to base58 via repeated division.
    let mut digits: Vec<u8> = Vec::new();
    let mut input = bytes.to_vec();
    while !input.iter().all(|&b| b == 0) {
        let mut carry: u32 = 0;
        for b in input.iter_mut() {
            let cur = ((carry as u32) << 8) | (*b as u32);
            *b = (cur / 58) as u8;
            carry = cur % 58;
        }
        digits.push(carry as u8);
    }
    let mut out = String::with_capacity(zeros + digits.len());
    for _ in 0..zeros { out.push('1'); }
    for d in digits.iter().rev() {
        out.push(A[*d as usize] as char);
    }
    out
}

/// Per-character JSON-string escape pass (RFC 8259 §7). Used by
/// the JSON renderer for `raw_text` strings and for object keys.
fn json_escape_into(text: &str, out: &mut String) {
    for c in text.chars() {
        match c {
            '"'  => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\x08' => out.push_str("\\b"),
            '\x0c' => out.push_str("\\f"),
            c if (c as u32) < 0x20 => {
                use std::fmt::Write;
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
}

fn format_decimal(mantissa: i128, scale: i32) -> String {
    // Render `mantissa × 10^scale` as a plain decimal string.
    // Handles negative scale (insert decimal point) and positive
    // scale (append zeros).
    if scale == 0 {
        return mantissa.to_string();
    }
    let sign = if mantissa < 0 { "-" } else { "" };
    let mag_str = mantissa.unsigned_abs().to_string();
    if scale > 0 {
        let mut s = String::with_capacity(sign.len() + mag_str.len() + scale as usize);
        s.push_str(sign);
        s.push_str(&mag_str);
        for _ in 0..scale {
            s.push('0');
        }
        s
    } else {
        let neg = (-scale) as usize;
        if neg < mag_str.len() {
            let dot = mag_str.len() - neg;
            let mut s = String::new();
            s.push_str(sign);
            s.push_str(&mag_str[..dot]);
            s.push('.');
            s.push_str(&mag_str[dot..]);
            s
        } else {
            // Less than 1 in magnitude — leading "0."
            let leading_zeros = neg - mag_str.len();
            let mut s = String::new();
            s.push_str(sign);
            s.push_str("0.");
            for _ in 0..leading_zeros {
                s.push('0');
            }
            s.push_str(&mag_str);
            s
        }
    }
}

// ── Fixture parsing ─────────────────────────────────────────────────

#[derive(Debug)]
struct Fixture {
    id: String,
    input_value: Option<Value>,
    input_json: Option<String>,
    wire_hex: String,
    json_compact: Option<String>,
    /// §7.5 emitter options. Each, when present, asserts that
    /// rendering the decoded value with the corresponding mode
    /// produces this text:
    json_pretty: Option<String>,                  // pretty=true, indent=2
    json_pretty_tab: Option<String>,              // pretty=true, indent=0 (tabs) — EM-003
    json_pretty_indent_4: Option<String>,         // pretty=true, indent=4
    json_int_string_largeonly: Option<String>,    // mode=LargeOnly
    json_int_string_all: Option<String>,          // mode=All
    must_round_trip: bool,
    must_validate: bool,
    must_reject: bool,
}

fn parse_fixture(json_text: &str) -> Result<Fixture, String> {
    let v: Json = serde_json::from_str(json_text)
        .map_err(|e| format!("fixture json parse: {}", e))?;
    let obj = v.as_object().ok_or("fixture root is not an object")?;

    let id = obj
        .get("id")
        .and_then(|x| x.as_str())
        .unwrap_or("<unnamed>")
        .to_string();

    let must_reject = obj
        .get("must_reject")
        .and_then(|x| x.as_bool())
        .unwrap_or(false);
    let must_round_trip = obj
        .get("must_round_trip")
        .and_then(|x| x.as_bool())
        .unwrap_or(false);
    let must_validate = obj
        .get("must_validate")
        .and_then(|x| x.as_bool())
        .unwrap_or(false);

    let wire_hex = obj
        .get("wire_hex")
        .and_then(|x| x.as_str())
        .ok_or("fixture missing wire_hex")?
        .to_string();
    let json_compact = obj
        .get("json_compact")
        .and_then(|x| x.as_str())
        .map(|s| s.to_string());

    let input_value = match obj.get("input_value") {
        Some(Json::Null) | None => None,
        Some(j) => Some(parse_value(j)?),
    };
    let input_json = obj.get("input_json")
        .and_then(|x| x.as_str())
        .map(|s| s.to_string());

    let json_pretty = obj.get("json_pretty")
        .and_then(|x| x.as_str()).map(|s| s.to_string());
    let json_pretty_tab = obj.get("json_pretty_tab")
        .and_then(|x| x.as_str()).map(|s| s.to_string());
    let json_pretty_indent_4 = obj.get("json_pretty_indent_4")
        .and_then(|x| x.as_str()).map(|s| s.to_string());
    let json_int_string_largeonly = obj.get("json_int_string_largeonly")
        .and_then(|x| x.as_str()).map(|s| s.to_string());
    let json_int_string_all = obj.get("json_int_string_all")
        .and_then(|x| x.as_str()).map(|s| s.to_string());

    Ok(Fixture {
        id,
        input_value,
        input_json,
        wire_hex,
        json_compact,
        json_pretty,
        json_pretty_tab,
        json_pretty_indent_4,
        json_int_string_largeonly,
        json_int_string_all,
        must_round_trip,
        must_validate,
        must_reject,
    })
}

fn parse_value(j: &Json) -> Result<Value, String> {
    let obj = j.as_object().ok_or("input_value must be an object")?;
    let kind = obj
        .get("kind")
        .and_then(|x| x.as_str())
        .ok_or("input_value missing 'kind'")?;
    match kind {
        "null" => Ok(Value::Null),
        "bool" => {
            let b = obj
                .get("value")
                .and_then(|x| x.as_bool())
                .ok_or("bool missing 'value'")?;
            Ok(Value::Bool(b))
        }
        "uint" => {
            let v_field = obj.get("value").ok_or("uint missing 'value'")?;
            // Accept either an unsigned JSON integer or a numeric
            // string for values up to u128.
            let n: u128 = if let Some(s) = v_field.as_str() {
                s.parse().map_err(|e| format!("uint string parse: {}", e))?
            } else if let Some(u) = v_field.as_u64() {
                u as u128
            } else {
                return Err("uint 'value' must be unsigned integer or string".to_string());
            };
            Ok(Value::Uint(n))
        }
        "int" => {
            // Signed integer: encoder picks form (uint*/negint*).
            let v_field = obj.get("value").ok_or("int missing 'value'")?;
            let i: i128 = if let Some(s) = v_field.as_str() {
                s.parse().map_err(|e| format!("int string parse: {}", e))?
            } else if let Some(i) = v_field.as_i64() {
                i as i128
            } else {
                return Err("int 'value' must be signed integer or string".to_string());
            };
            if i >= 0 {
                Ok(Value::Uint(i as u128))
            } else {
                let mag = i.unsigned_abs();
                Ok(Value::NegInt(mag))
            }
        }
        "float" => {
            let width = obj
                .get("width")
                .and_then(|x| x.as_u64())
                .ok_or("float missing 'width'")?;
            let width_log2 = match width {
                16 => 1u8,
                32 => 2,
                64 => 3,
                128 => 4,
                _ => return Err(format!("float width {} not in {{16,32,64,128}}", width)),
            };
            let bits_str = obj
                .get("bits_hex")
                .and_then(|x| x.as_str())
                .ok_or("float missing 'bits_hex'")?
                .trim_start_matches("0x")
                .trim_start_matches("0X");
            let bits = u128::from_str_radix(bits_str, 16)
                .map_err(|e| format!("float bits_hex parse: {}", e))?;
            Ok(Value::Float { width_log2, bits })
        }
        "decimal" => {
            let m_str = obj
                .get("mantissa")
                .and_then(|x| x.as_str())
                .ok_or("decimal missing 'mantissa' (string)")?;
            let mantissa: i128 = m_str
                .parse()
                .map_err(|e| format!("decimal mantissa parse: {}", e))?;
            let scale = obj
                .get("scale")
                .and_then(|x| x.as_i64())
                .ok_or("decimal missing 'scale'")? as i32;
            Ok(Value::Decimal { mantissa, scale })
        }
        "array" => {
            let arr = obj.get("children")
                         .and_then(|x| x.as_array())
                         .ok_or("array missing 'children' (array)")?;
            let mut children = Vec::with_capacity(arr.len());
            for child_json in arr {
                children.push(parse_value(child_json)?);
            }
            Ok(Value::Array(children))
        }
        "object" => {
            let arr = obj.get("entries")
                         .and_then(|x| x.as_array())
                         .ok_or("object missing 'entries' (array)")?;
            let mut entries = Vec::with_capacity(arr.len());
            for entry_json in arr {
                let entry_obj = entry_json.as_object()
                    .ok_or("object entry must be object")?;
                let key = entry_obj.get("key")
                    .and_then(|x| x.as_str())
                    .ok_or("object entry missing 'key'")?
                    .to_string();
                let value = entry_obj.get("value")
                    .ok_or("object entry missing 'value'")?;
                entries.push((key, parse_value(value)?));
            }
            Ok(Value::Object(entries))
        }
        "numeric_string" => {
            let inner_json = obj.get("inner")
                .ok_or("numeric_string missing 'inner'")?;
            let inner = parse_value(inner_json)?;
            Ok(Value::NumericString(Box::new(inner)))
        }
        "extension" => {
            let subtype = obj.get("subtype")
                .and_then(|x| x.as_u64())
                .ok_or("extension missing 'subtype'")? as u8;
            if subtype > 15 {
                return Err(format!("extension subtype {} > 15", subtype));
            }
            let bytes_hex = obj.get("bytes_hex")
                .and_then(|x| x.as_str())
                .ok_or("extension missing 'bytes_hex'")?;
            let bytes = parse_hex(bytes_hex)
                .map_err(|e| format!("extension bytes_hex parse: {}", e))?;
            Ok(Value::Extension { subtype, bytes })
        }
        "bytes" => {
            let encoding = obj.get("encoding")
                .and_then(|x| x.as_str())
                .ok_or("bytes missing 'encoding' (one of base64/hex/base58/base64url)")?;
            let encoding_hint = match encoding {
                "base64"    => 0u8,
                "hex"       => 1,
                "base58"    => 2,
                "base64url" => 3,
                other => return Err(format!("bytes encoding '{}' not in {{base64,hex,base58,base64url}}", other)),
            };
            let bytes_hex = obj.get("bytes_hex")
                .and_then(|x| x.as_str())
                .ok_or("bytes missing 'bytes_hex'")?;
            let content = parse_hex(bytes_hex).map_err(|e| format!("bytes_hex parse: {}", e))?;
            Ok(Value::Bytes { encoding_hint, content })
        }
        "string" => {
            let encoding = obj.get("encoding")
                .and_then(|x| x.as_str())
                .ok_or("string missing 'encoding' (\"raw_text\" or \"escape_form\")")?;
            let encoding_flag = match encoding {
                "raw_text" => 0,
                "escape_form" => 1,
                other => return Err(format!("string encoding '{}' must be raw_text or escape_form", other)),
            };
            let text = obj.get("text")
                .and_then(|x| x.as_str())
                .ok_or("string missing 'text'")?;
            Ok(Value::String { encoding_flag, content: text.as_bytes().to_vec() })
        }
        "row_array" => {
            let keys_json = obj.get("keys")
                .and_then(|x| x.as_array())
                .ok_or("row_array missing 'keys'")?;
            let mut keys: Vec<String> = Vec::with_capacity(keys_json.len());
            for k in keys_json {
                keys.push(k.as_str().ok_or("row_array key not string")?.to_string());
            }
            let rows_json = obj.get("rows")
                .and_then(|x| x.as_array())
                .ok_or("row_array missing 'rows'")?;
            let mut rows: Vec<Vec<Value>> = Vec::with_capacity(rows_json.len());
            for row_json in rows_json {
                let row_arr = row_json.as_array()
                    .ok_or("row_array row not array")?;
                let mut row: Vec<Value> = Vec::with_capacity(row_arr.len());
                for cell in row_arr {
                    row.push(parse_value(cell)?);
                }
                rows.push(row);
            }
            Ok(Value::RowArray { keys, rows })
        }
        "typed_array" => {
            let element_code = obj.get("element_code")
                .and_then(|x| x.as_u64())
                .ok_or("typed_array missing 'element_code'")? as u8;
            if element_code > 9 {
                return Err(format!("typed_array element_code {} out of range", element_code));
            }
            let elements = obj.get("elements")
                .and_then(|x| x.as_array())
                .ok_or("typed_array missing 'elements'")?;
            let esize = typed_array_element_size(element_code)
                .map_err(|e| e.to_string())?;
            let mut raw = Vec::with_capacity(elements.len() * esize);
            for e in elements {
                match element_code {
                    0 => {
                        let v = e.as_i64().ok_or("typed_array i8 element not integer")?;
                        raw.push(v as i8 as u8);
                    }
                    1 => {
                        let v = e.as_i64().ok_or("typed_array i16 element not integer")? as i16;
                        raw.extend_from_slice(&v.to_le_bytes());
                    }
                    2 => {
                        let v = e.as_i64().ok_or("typed_array i32 element not integer")? as i32;
                        raw.extend_from_slice(&v.to_le_bytes());
                    }
                    3 => {
                        let v = e.as_i64().ok_or("typed_array i64 element not integer")?;
                        raw.extend_from_slice(&v.to_le_bytes());
                    }
                    4 => {
                        let v = e.as_u64().ok_or("typed_array u8 element not integer")? as u8;
                        raw.push(v);
                    }
                    5 => {
                        let v = e.as_u64().ok_or("typed_array u16 element not integer")? as u16;
                        raw.extend_from_slice(&v.to_le_bytes());
                    }
                    6 => {
                        let v = e.as_u64().ok_or("typed_array u32 element not integer")? as u32;
                        raw.extend_from_slice(&v.to_le_bytes());
                    }
                    7 => {
                        let v = e.as_u64().ok_or("typed_array u64 element not integer")?;
                        raw.extend_from_slice(&v.to_le_bytes());
                    }
                    8 => {
                        let v = e.as_f64()
                            .or_else(|| e.as_i64().map(|i| i as f64))
                            .ok_or("typed_array f32 element not number")? as f32;
                        raw.extend_from_slice(&v.to_le_bytes());
                    }
                    9 => {
                        let v = e.as_f64()
                            .or_else(|| e.as_i64().map(|i| i as f64))
                            .ok_or("typed_array f64 element not number")?;
                        raw.extend_from_slice(&v.to_le_bytes());
                    }
                    _ => unreachable!(),
                }
            }
            Ok(Value::TypedArray { element_code, raw })
        }
        other => Err(format!("input_value kind '{}' not supported yet", other)),
    }
}

fn parse_hex(s: &str) -> Result<Vec<u8>, String> {
    let s = s.trim();
    if s.len() % 2 != 0 {
        return Err(format!("hex string length not even: {:?}", s));
    }
    let mut out = Vec::with_capacity(s.len() / 2);
    let bytes = s.as_bytes();
    for i in (0..s.len()).step_by(2) {
        let hi = hex_digit(bytes[i])?;
        let lo = hex_digit(bytes[i + 1])?;
        out.push((hi << 4) | lo);
    }
    Ok(out)
}

fn hex_digit(c: u8) -> Result<u8, String> {
    match c {
        b'0'..=b'9' => Ok(c - b'0'),
        b'a'..=b'f' => Ok(10 + c - b'a'),
        b'A'..=b'F' => Ok(10 + c - b'A'),
        _ => Err(format!("not a hex digit: {:?}", c as char)),
    }
}

fn to_hex(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push_str(&format!("{:02x}", b));
    }
    s
}

// ── Check-mode harness ──────────────────────────────────────────────

fn check(fixture: &Fixture) -> Result<(), String> {
    let expected_wire = parse_hex(&fixture.wire_hex)?;

    if fixture.must_reject {
        // Decoder must error on this wire.
        match decode(&expected_wire) {
            Ok(v) => Err(format!(
                "expected reject but decoded successfully: {:?}",
                v
            )),
            Err(_) => Ok(()),
        }
    } else {
        // Positive case: decode must succeed.
        let decoded = decode(&expected_wire).map_err(|e| format!("decode failed: {}", e))?;

        // If fixture has an input_value, compare structural equality.
        if let Some(expected) = &fixture.input_value {
            if &decoded != expected {
                return Err(format!(
                    "decode mismatch:\n  expected: {:?}\n  got:      {:?}",
                    expected, decoded
                ));
            }
        }

        // Encode round-trip: encode(decoded) must match the wire.
        if fixture.must_round_trip {
            let re_encoded =
                encode(&decoded).map_err(|e| format!("re-encode failed: {:?}", e))?;
            if re_encoded != expected_wire {
                return Err(format!(
                    "round-trip mismatch:\n  expected wire: {}\n  got:           {}",
                    fixture.wire_hex,
                    to_hex(&re_encoded)
                ));
            }
            // And if input_value is provided, encoding it directly
            // must also produce the wire.
            if let Some(v) = &fixture.input_value {
                let e2 = encode(v).map_err(|e| format!("encode-from-input failed: {:?}", e))?;
                if e2 != expected_wire {
                    return Err(format!(
                        "encode-from-input mismatch:\n  expected wire: {}\n  got:           {}",
                        fixture.wire_hex,
                        to_hex(&e2)
                    ));
                }
            }
            // If input_json is provided, transcode JSON → Value, encode,
            // compare to wire. Exercises the §4.8 / §7.1 ingress path.
            if let Some(json_text) = &fixture.input_json {
                let from_json_value = from_json(json_text)
                    .map_err(|e| format!("from_json failed: {}", e))?;
                let e3 = encode(&from_json_value)
                    .map_err(|e| format!("encode-from-json failed: {:?}", e))?;
                if e3 != expected_wire {
                    return Err(format!(
                        "encode-from-json mismatch:\n  expected wire: {}\n  got:           {}",
                        fixture.wire_hex,
                        to_hex(&e3)
                    ));
                }
            }
        }

        // JSON projection check.
        if let Some(expected_json) = &fixture.json_compact {
            let got = render_json(&decoded);
            if &got != expected_json {
                return Err(format!(
                    "json mismatch:\n  expected: {:?}\n  got:      {:?}",
                    expected_json, got
                ));
            }
        }
        // §7.5 emitter-option checks. Each is independent: the fixture
        // can specify one, several, or none. When present, render the
        // decoded value with the matching opts and compare.
        if let Some(expected_pretty) = &fixture.json_pretty {
            let got = render_json_with(&decoded,
                &EmitOptions { pretty: true, indent: 2,
                                int_string_mode: IntStringMode::Never });
            if &got != expected_pretty {
                return Err(format!(
                    "json_pretty mismatch:\n  expected: {:?}\n  got:      {:?}",
                    expected_pretty, got));
            }
        }
        if let Some(expected) = &fixture.json_pretty_tab {
            let got = render_json_with(&decoded,
                &EmitOptions { pretty: true, indent: 0,
                                int_string_mode: IntStringMode::Never });
            if &got != expected {
                return Err(format!(
                    "json_pretty_tab mismatch:\n  expected: {:?}\n  got:      {:?}",
                    expected, got));
            }
        }
        if let Some(expected) = &fixture.json_pretty_indent_4 {
            let got = render_json_with(&decoded,
                &EmitOptions { pretty: true, indent: 4,
                                int_string_mode: IntStringMode::Never });
            if &got != expected {
                return Err(format!(
                    "json_pretty_indent_4 mismatch:\n  expected: {:?}\n  got:      {:?}",
                    expected, got));
            }
        }
        if let Some(expected) = &fixture.json_int_string_largeonly {
            let got = render_json_with(&decoded,
                &EmitOptions { pretty: false, indent: 2,
                                int_string_mode: IntStringMode::LargeOnly });
            if &got != expected {
                return Err(format!(
                    "json_int_string_largeonly mismatch:\n  expected: {:?}\n  got:      {:?}",
                    expected, got));
            }
        }
        if let Some(expected) = &fixture.json_int_string_all {
            let got = render_json_with(&decoded,
                &EmitOptions { pretty: false, indent: 2,
                                int_string_mode: IntStringMode::All });
            if &got != expected {
                return Err(format!(
                    "json_int_string_all mismatch:\n  expected: {:?}\n  got:      {:?}",
                    expected, got));
            }
        }

        let _ = fixture.must_validate; // Phase 1 has no separate validator path

        Ok(())
    }
}

fn xvalidate(fixture: &Fixture) -> Result<String, String> {
    let wire = parse_hex(&fixture.wire_hex)?;
    if fixture.must_reject {
        let err = match decode(&wire) {
            Ok(_) => "<DID-NOT-REJECT>".to_string(),
            Err(e) => format!("{}", e),
        };
        return Ok(format!("reject:{}", err));
    }
    let v = decode(&wire).map_err(|e| format!("decode: {}", e))?;
    let json = render_json(&v);
    let re_wire = encode(&v).map_err(|e| format!("re-encode: {:?}", e))?;
    Ok(format!("wire:{} json:{}", to_hex(&re_wire), json))
}

// ── main ────────────────────────────────────────────────────────────

/// Runtime self-test (T-016, T-017, V-003). Mirror of the `#[cfg(test)]`
/// `tag_byte_predicates_match_spec_section_3` so the property is
/// verifiable from the production binary without `cargo test`.
fn self_test() -> ExitCode {
    let mut failed = 0u32;
    let mut check = |cond: bool, msg: &str| {
        if !cond { eprintln!("  FAIL: {msg}"); failed += 1; }
    };

    for high in 0u8..=15 {
        let is_atom              = high <= 1;
        let is_integer           = (2..=5).contains(&high);
        let is_real              = (6..=7).contains(&high);
        let is_numeric_value     = (2..=7).contains(&high);
        let is_number_projectable= (2..=8).contains(&high);
        let is_json_string_emit  = (8..=10).contains(&high);
        let is_aggregate         = (11..=12).contains(&high);
        let is_extension         = high == 13;
        let is_reserved          = high >= 14;

        let class_count = [is_atom, is_integer, is_real, is_json_string_emit,
                           is_aggregate, is_extension, is_reserved]
                          .iter().filter(|x| **x).count();
        check(class_count == 1, &format!("high {high} class_count==1"));

        check(is_numeric_value == (is_integer || is_real),
              &format!("high {high} numeric composite"));
        check(is_number_projectable == (is_numeric_value || high == 8),
              &format!("high {high} number_projectable composite"));

        let tag: u8 = high << 4;
        check(((tag >> 4) >= 14) == is_reserved,
              &format!("high {high} V-003 predicate"));
    }

    for high in 2u8..=5 {
        let sign_bit   = (high & 1) != 0;
        let inline_bit = (high & 2) != 0;
        let expected_signed = matches!(high, 3 | 5);
        let expected_inline = matches!(high, 2 | 3);
        check(sign_bit   == expected_signed, &format!("code {high} sign bit"));
        check(inline_bit == expected_inline, &format!("code {high} inline bit"));
    }

    for high in 0u8..=15 {
        let tag = high << 4;
        match decode(&[tag]) {
            Ok(_) => check(high < 14, &format!("high {high} not reserved")),
            Err(DecodeError::ReservedTag(_)) => {
                check(high >= 14, &format!("high {high} ReservedTag iff >= 14"));
            }
            Err(_) => check(high < 14, &format!("high {high} non-reserved error")),
        }
    }

    if failed == 0 {
        println!("self-test: PASSED");
        ExitCode::SUCCESS
    } else {
        eprintln!("self-test: {failed} FAILED");
        ExitCode::FAILURE
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mode = match args.first().map(String::as_str) {
        Some("--check") => "check",
        Some("--xvalidate") => "xvalidate",
        Some("--self-test") => return self_test(),
        _ => {
            eprintln!("usage: pjson_conformance_driver --check|--xvalidate|--self-test < fixture.json");
            return ExitCode::from(2);
        }
    };

    let mut input = String::new();
    if io::stdin().read_to_string(&mut input).is_err() {
        eprintln!("read stdin failed");
        return ExitCode::from(2);
    }

    let fixture = match parse_fixture(&input) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("fixture parse: {}", e);
            return ExitCode::from(2);
        }
    };

    match mode {
        "check" => match check(&fixture) {
            Ok(()) => ExitCode::SUCCESS,
            Err(e) => {
                eprintln!("FAIL [{}]: {}", fixture.id, e);
                ExitCode::FAILURE
            }
        },
        "xvalidate" => match xvalidate(&fixture) {
            Ok(s) => {
                println!("{}", s);
                ExitCode::SUCCESS
            }
            Err(e) => {
                eprintln!("FAIL [{}]: {}", fixture.id, e);
                ExitCode::FAILURE
            }
        },
        _ => unreachable!(),
    }
}

// ── self-tests for the codec primitives ─────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn null_round_trip() {
        let v = Value::Null;
        assert_eq!(encode(&v).unwrap(), vec![0x00]);
        assert_eq!(decode(&[0x00]).unwrap(), v);
    }

    #[test]
    fn bool_round_trip() {
        assert_eq!(encode(&Value::Bool(false)).unwrap(), vec![0x10]);
        assert_eq!(encode(&Value::Bool(true)).unwrap(), vec![0x11]);
        assert_eq!(decode(&[0x10]).unwrap(), Value::Bool(false));
        assert_eq!(decode(&[0x11]).unwrap(), Value::Bool(true));
        assert!(decode(&[0x12]).is_err());
    }

    #[test]
    fn uint_inline_round_trip() {
        for n in 0u128..=15 {
            let v = Value::Uint(n);
            let enc = encode(&v).unwrap();
            assert_eq!(enc, vec![0x20 | (n as u8)], "uint_inline {}", n);
            assert_eq!(decode(&enc).unwrap(), v);
        }
    }

    #[test]
    fn nint_inline_round_trip() {
        for mag in 1u128..=15 {
            let v = Value::NegInt(mag);
            let enc = encode(&v).unwrap();
            assert_eq!(enc, vec![0x30 | (mag as u8)], "nint_inline -{}", mag);
            assert_eq!(decode(&enc).unwrap(), v);
        }
    }

    #[test]
    fn nint_inline_zero_rejected() {
        assert!(matches!(decode(&[0x30]), Err(DecodeError::NintInlineZero)));
    }

    #[test]
    fn uint_full_round_trip() {
        let cases = [
            (16u128,                       vec![0x40, 0x10]),
            (255,                          vec![0x40, 0xFF]),
            (256,                          vec![0x41, 0x00, 0x01]),
            (65535,                        vec![0x41, 0xFF, 0xFF]),
            (u64::MAX as u128,             vec![0x47, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]),
        ];
        for (n, expected) in cases {
            let v = Value::Uint(n);
            assert_eq!(encode(&v).unwrap(), expected, "uint {}", n);
            assert_eq!(decode(&expected).unwrap(), v);
        }
    }

    #[test]
    fn negint_full_round_trip() {
        let cases = [
            (16u128, vec![0x50, 0x10]),
            (255,    vec![0x50, 0xFF]),
            (256,    vec![0x51, 0x00, 0x01]),
        ];
        for (mag, expected) in cases {
            let v = Value::NegInt(mag);
            assert_eq!(encode(&v).unwrap(), expected, "negint -{}", mag);
            assert_eq!(decode(&expected).unwrap(), v);
        }
    }

    #[test]
    fn negint_zero_rejected() {
        assert!(matches!(decode(&[0x50, 0x00]), Err(DecodeError::NegintZero)));
    }

    #[test]
    fn ieee_float_round_trip() {
        // f64 1.0 = 0x3FF0000000000000 (LE: 00 00 00 00 00 00 F0 3F)
        let v = Value::Float {
            width_log2: 3,
            bits: 0x3FF0000000000000,
        };
        let enc = encode(&v).unwrap();
        assert_eq!(enc, vec![0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F]);
        assert_eq!(decode(&enc).unwrap(), v);

        // f32 1.0 = 0x3F800000 (LE: 00 00 80 3F)
        let v32 = Value::Float {
            width_log2: 2,
            bits: 0x3F800000,
        };
        let enc32 = encode(&v32).unwrap();
        assert_eq!(enc32, vec![0x62, 0x00, 0x00, 0x80, 0x3F]);
        assert_eq!(decode(&enc32).unwrap(), v32);

        // f16 1.0 = 0x3C00 (LE: 00 3C)
        let v16 = Value::Float {
            width_log2: 1,
            bits: 0x3C00,
        };
        let enc16 = encode(&v16).unwrap();
        assert_eq!(enc16, vec![0x61, 0x00, 0x3C]);
        assert_eq!(decode(&enc16).unwrap(), v16);
    }

    #[test]
    fn ieee_float_reserved_bit3_rejected() {
        // tag 0x6B = ieee_float low_nibble 0x0B = bits 1011: bit 3 set
        // -> reserved.
        let r = decode(&[0x6B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F]);
        assert!(matches!(r, Err(DecodeError::ReservedLowNibble(_, _))));
    }

    #[test]
    fn ieee_float_bad_width_rejected() {
        // tag 0x60 = width selector 0 -> reserved.
        assert!(matches!(decode(&[0x60]), Err(DecodeError::BadWidth(0))));
        // tag 0x65 = width selector 5 -> reserved.
        assert!(matches!(decode(&[0x65]), Err(DecodeError::BadWidth(5))));
    }

    #[test]
    fn varscale_encode_decode_round_trip() {
        for s in [-32i32, -1, 0, 1, 31, 32, -33, 8191, -8192, 8192, 100_000, -100_000] {
            let mut buf = Vec::new();
            varscale_encode_into(s, &mut buf).unwrap();
            let (decoded, used) = varscale_decode(&buf).unwrap();
            assert_eq!(decoded, s, "varscale {}", s);
            assert_eq!(used, buf.len());
        }
    }

    #[test]
    fn decimal_round_trip() {
        // 1.5 = 15 * 10^-1
        // mantissa zigzag(15) = 30 = 0x1E, bc=1, low=0
        // varscale zigzag(-1) = 1, byte_count_field = 0, byte = 0x01
        let v = Value::Decimal { mantissa: 15, scale: -1 };
        let enc = encode(&v).unwrap();
        assert_eq!(enc, vec![0x70, 0x1E, 0x01]);
        assert_eq!(decode(&enc).unwrap(), v);

        // 12345 (scale 0)
        // zigzag(12345) = 24690 = 0x6072, bc=2, low=1
        // varscale zigzag(0) = 0, byte = 0x00
        let v2 = Value::Decimal { mantissa: 12345, scale: 0 };
        let enc2 = encode(&v2).unwrap();
        assert_eq!(enc2, vec![0x71, 0x72, 0x60, 0x00]);
        assert_eq!(decode(&enc2).unwrap(), v2);
    }

    #[test]
    fn reserved_codes_rejected() {
        assert!(matches!(decode(&[0xE0]), Err(DecodeError::ReservedTag(0xE0))));
        assert!(matches!(decode(&[0xF0]), Err(DecodeError::ReservedTag(0xF0))));
    }

    #[test]
    fn json_render_basics() {
        assert_eq!(render_json(&Value::Null), "null");
        assert_eq!(render_json(&Value::Bool(true)), "true");
        assert_eq!(render_json(&Value::Uint(5)), "5");
        assert_eq!(render_json(&Value::NegInt(7)), "-7");
        assert_eq!(
            render_json(&Value::Float { width_log2: 3, bits: 0x3FF0000000000000 }),
            "1"
        );
        assert_eq!(
            render_json(&Value::Decimal { mantissa: 15, scale: -1 }),
            "1.5"
        );
        assert_eq!(
            render_json(&Value::Decimal { mantissa: 1, scale: -2 }),
            "0.01"
        );
    }

    #[test]
    fn f128_widen_canonical_values() {
        // ±0.0
        assert_eq!(f128_bits_to_f64(0u128).to_bits(), 0u64);
        assert_eq!(f128_bits_to_f64(1u128 << 127).to_bits(), 1u64 << 63);

        // 1.0: sign=0, exp=16383, mantissa=0
        // bits = 0x3FFF_0000_0000_0000_0000_0000_0000_0000
        let one_bits = 0x3FFF_0000_0000_0000_0000_0000_0000_0000u128;
        assert_eq!(f128_bits_to_f64(one_bits), 1.0);

        // -1.0: sign=1, exp=16383, mantissa=0
        let neg_one_bits = 0xBFFF_0000_0000_0000_0000_0000_0000_0000u128;
        assert_eq!(f128_bits_to_f64(neg_one_bits), -1.0);

        // 2.0: sign=0, exp=16384, mantissa=0
        let two_bits = 0x4000_0000_0000_0000_0000_0000_0000_0000u128;
        assert_eq!(f128_bits_to_f64(two_bits), 2.0);

        // 0.5: sign=0, exp=16382, mantissa=0
        let half_bits = 0x3FFE_0000_0000_0000_0000_0000_0000_0000u128;
        assert_eq!(f128_bits_to_f64(half_bits), 0.5);

        // +Inf
        let pos_inf_bits = 0x7FFF_0000_0000_0000_0000_0000_0000_0000u128;
        assert_eq!(f128_bits_to_f64(pos_inf_bits), f64::INFINITY);

        // -Inf
        let neg_inf_bits = 0xFFFF_0000_0000_0000_0000_0000_0000_0000u128;
        assert_eq!(f128_bits_to_f64(neg_inf_bits), f64::NEG_INFINITY);

        // NaN — canonical quiet, sign-preserved
        let nan_bits = 0x7FFF_8000_0000_0000_0000_0000_0000_0000u128;
        let result = f128_bits_to_f64(nan_bits);
        assert!(result.is_nan());
        assert_eq!(result.to_bits(), 0x7FF8_0000_0000_0000);
    }

    #[test]
    fn f128_widen_overflow_to_inf() {
        // f128 with exp >= 16384 + 1024 (= 17408) overflows f64's
        // 11-bit exponent range (max f64 exp = 1023).
        // 2^1024 in f128: exp = 16383 + 1024 = 17407, mantissa = 0
        // Largest finite f128 ≪ DBL_MAX → f64 inf.
        let huge_bits = 0x7FFE_0000_0000_0000_0000_0000_0000_0000u128;
        // exp = 0x7FFE = 32766. After -=0x3C01 → exp=17405. Way > 0x7FD=2045.
        assert_eq!(f128_bits_to_f64(huge_bits), f64::INFINITY);
    }

    #[test]
    fn f128_widen_underflow_to_zero() {
        // Smallest positive subnormal f128 (exp=0, mantissa=...0001)
        //   value = 2^-16494, way below f64 min subnormal 2^-1074.
        //   Rounds to +0.
        let tiny_bits = 1u128;
        assert_eq!(f128_bits_to_f64(tiny_bits).to_bits(), 0u64);
    }

    #[test]
    fn generic_array_round_trip() {
        // Empty array: tag 0xB0, width 0x00, no value_data, no slots, count 0x00 0x00
        let v0 = Value::Array(vec![]);
        let enc0 = encode(&v0).unwrap();
        assert_eq!(enc0, vec![0xB0, 0x00, 0x00, 0x00], "empty array");
        assert_eq!(decode(&enc0).unwrap(), v0);

        // [1] → tag 0xB0, width 0x00, value_data [0x21], slot[0]=0 (1 byte),
        //         count 0x01 0x00
        let v1 = Value::Array(vec![Value::Uint(1)]);
        let enc1 = encode(&v1).unwrap();
        assert_eq!(enc1, vec![0xB0, 0x00, 0x21, 0x00, 0x01, 0x00], "[1]");
        assert_eq!(decode(&enc1).unwrap(), v1);

        // [1, true]: value_data [0x21, 0x11], slots [0x00, 0x01], count 0x02 0x00
        let v2 = Value::Array(vec![Value::Uint(1), Value::Bool(true)]);
        let enc2 = encode(&v2).unwrap();
        assert_eq!(enc2,
            vec![0xB0, 0x00, 0x21, 0x11, 0x00, 0x01, 0x02, 0x00],
            "[1, true]");
        assert_eq!(decode(&enc2).unwrap(), v2);

        // Nested: [[1, 2], [3]]
        let nested = Value::Array(vec![
            Value::Array(vec![Value::Uint(1), Value::Uint(2)]),
            Value::Array(vec![Value::Uint(3)]),
        ]);
        let enc_nested = encode(&nested).unwrap();
        let dec_nested = decode(&enc_nested).unwrap();
        assert_eq!(dec_nested, nested);
        // JSON projection: [[1,2],[3]]
        assert_eq!(render_json(&dec_nested), "[[1,2],[3]]");
    }

    #[test]
    fn object_round_trip() {
        // Empty object: tag 0xC0, width 0, no value_data, no hash, no slots, count 0
        let v0 = Value::Object(vec![]);
        let enc0 = encode(&v0).unwrap();
        assert_eq!(enc0, vec![0xC0, 0x00, 0x00, 0x00], "empty object");
        assert_eq!(decode(&enc0).unwrap(), v0);

        // {"a": 1}
        let v1 = Value::Object(vec![("a".to_string(), Value::Uint(1))]);
        let enc1 = encode(&v1).unwrap();
        let dec1 = decode(&enc1).unwrap();
        assert_eq!(dec1, v1);
        // wire shape: tag(1) + width(1) + 'a'(1) + uint_inline(1) + hash(1) + slot(2) + count(2) = 9
        assert_eq!(enc1.len(), 9);

        // Multi-field {"x":1, "y":-1, "z":true}
        let v2 = Value::Object(vec![
            ("x".to_string(), Value::Uint(1)),
            ("y".to_string(), Value::NegInt(1)),
            ("z".to_string(), Value::Bool(true)),
        ]);
        let enc2 = encode(&v2).unwrap();
        let dec2 = decode(&enc2).unwrap();
        assert_eq!(dec2, v2);
        assert_eq!(render_json(&dec2), r#"{"x":1,"y":-1,"z":true}"#);

        // Nested: {"outer": {"inner": 5}}
        let v3 = Value::Object(vec![(
            "outer".to_string(),
            Value::Object(vec![("inner".to_string(), Value::Uint(5))]),
        )]);
        let enc3 = encode(&v3).unwrap();
        assert_eq!(decode(&enc3).unwrap(), v3);
    }

    #[test]
    fn object_long_key_round_trip() {
        // Key of exactly 254 bytes — short-form (key_size byte is the length).
        let key254 = "a".repeat(254);
        let v = Value::Object(vec![(key254, Value::Uint(1))]);
        assert_eq!(decode(&encode(&v).unwrap()).unwrap(), v);

        // Key of exactly 255 bytes — long-key escape (key_size byte = 0xFF,
        // varuint excess = 0).
        let key255 = "b".repeat(255);
        let v = Value::Object(vec![(key255, Value::Uint(1))]);
        assert_eq!(decode(&encode(&v).unwrap()).unwrap(), v);

        // Key of 1024 bytes — long-key escape with non-zero excess.
        let key1024 = "c".repeat(1024);
        let v = Value::Object(vec![(key1024, Value::Uint(1))]);
        assert_eq!(decode(&encode(&v).unwrap()).unwrap(), v);
    }

    #[test]
    fn extension_round_trip_and_envelope() {
        // Encode/decode a basic extension.
        let v = Value::Extension { subtype: 5, bytes: vec![0xDE, 0xAD, 0xBE, 0xEF] };
        let enc = encode(&v).unwrap();
        assert_eq!(enc, vec![0xD5, 0xDE, 0xAD, 0xBE, 0xEF]);
        assert_eq!(decode(&enc).unwrap(), v);

        // JSON projection: opaque envelope.
        assert_eq!(
            render_json(&v),
            r#"{"__pjson_ext":{"subtype":5,"bytes_b64":"3q2+7w=="}}"#
        );

        // Envelope round-trip via JSON ingress: the JSON-text form of
        // the envelope must reconstruct back to an Extension wire.
        let from = from_json(
            r#"{"__pjson_ext":{"subtype":5,"bytes_b64":"3q2+7w=="}}"#).unwrap();
        assert_eq!(from, v);
        assert_eq!(encode(&from).unwrap(), enc);

        // Envelope with sub-type 0
        let v0 = Value::Extension { subtype: 0, bytes: vec![] };
        assert_eq!(encode(&v0).unwrap(), vec![0xD0]);
        assert_eq!(render_json(&v0),
                   r#"{"__pjson_ext":{"subtype":0,"bytes_b64":""}}"#);

        // A regular object that happens to share keys but with extra
        // fields stays a regular object (envelope detection is shape-
        // sensitive: single key, exactly two inner keys).
        let regular = from_json(r#"{"__pjson_ext":{"subtype":5,"bytes_b64":"3q2+7w==","extra":1}}"#).unwrap();
        assert!(matches!(regular, Value::Object(_)));
    }

    #[test]
    fn json_ingress_numeric_string_lift_rule() {
        // Lift cases (canonical match)
        assert_eq!(parse_canonical_json_number_string("0"),    Some(Value::Uint(0)));
        assert_eq!(parse_canonical_json_number_string("1"),    Some(Value::Uint(1)));
        assert_eq!(parse_canonical_json_number_string("123"),  Some(Value::Uint(123)));
        assert_eq!(parse_canonical_json_number_string("-1"),   Some(Value::NegInt(1)));
        assert_eq!(parse_canonical_json_number_string("-123"), Some(Value::NegInt(123)));
        assert_eq!(parse_canonical_json_number_string("100"),  Some(Value::Uint(100)));
        assert_eq!(parse_canonical_json_number_string("0.1"),
                   Some(Value::Decimal { mantissa: 1, scale: -1 }));
        assert_eq!(parse_canonical_json_number_string("3.14"),
                   Some(Value::Decimal { mantissa: 314, scale: -2 }));
        assert_eq!(parse_canonical_json_number_string("123456789012345678"),
                   Some(Value::Uint(123456789012345678)));

        // Don't-lift cases
        assert_eq!(parse_canonical_json_number_string(""),       None);
        assert_eq!(parse_canonical_json_number_string("01"),     None);    // leading zero
        assert_eq!(parse_canonical_json_number_string("00123"),  None);    // leading zeros
        assert_eq!(parse_canonical_json_number_string("-0"),     None);    // negative zero
        assert_eq!(parse_canonical_json_number_string("1.0"),    None);    // trailing zero
        assert_eq!(parse_canonical_json_number_string("1.50"),   None);    // trailing zero
        assert_eq!(parse_canonical_json_number_string("1e5"),    None);    // sci notation
        assert_eq!(parse_canonical_json_number_string("1.5e10"), None);    // sci notation
        assert_eq!(parse_canonical_json_number_string("+1"),     None);    // leading plus
        assert_eq!(parse_canonical_json_number_string("1."),     None);    // empty fractional
        assert_eq!(parse_canonical_json_number_string(".5"),     None);    // empty integer
        assert_eq!(parse_canonical_json_number_string("hello"),  None);    // non-numeric

        // End-to-end through from_json
        let v = from_json(r#""123""#).unwrap();
        assert_eq!(v, Value::NumericString(Box::new(Value::Uint(123))));

        let v2 = from_json(r#""01""#).unwrap();
        match v2 {
            Value::String { encoding_flag: 0, content } => assert_eq!(content, b"01"),
            other => panic!("expected raw_text string, got {:?}", other),
        }

        let v3 = from_json(r#""hello""#).unwrap();
        match v3 {
            Value::String { encoding_flag: 0, content } => assert_eq!(content, b"hello"),
            other => panic!("expected string, got {:?}", other),
        }

        // JSON null/bool/integer/array/object
        assert_eq!(from_json("null").unwrap(),  Value::Null);
        assert_eq!(from_json("true").unwrap(),  Value::Bool(true));
        assert_eq!(from_json("false").unwrap(), Value::Bool(false));
        assert_eq!(from_json("42").unwrap(),    Value::Uint(42));
        assert_eq!(from_json("-42").unwrap(),   Value::NegInt(42));
        let arr = from_json("[1,2,3]").unwrap();
        assert_eq!(arr, Value::Array(vec![Value::Uint(1), Value::Uint(2), Value::Uint(3)]));
        let obj = from_json(r#"{"a":1,"b":true}"#).unwrap();
        assert_eq!(obj, Value::Object(vec![
            ("a".to_string(), Value::Uint(1)),
            ("b".to_string(), Value::Bool(true)),
        ]));
    }

    #[test]
    fn numeric_string_round_trip() {
        // wrap uint 42: tag 0x80 + uint encoding (0x40 0x2A) = 3 bytes
        let v = Value::NumericString(Box::new(Value::Uint(42)));
        let enc = encode(&v).unwrap();
        assert_eq!(enc, vec![0x80, 0x40, 0x2A]);
        assert_eq!(decode(&enc).unwrap(), v);
        // JSON projection: "42" with quotes
        assert_eq!(render_json(&v), r#""42""#);

        // wrap uint_inline 5: tag 0x80 + 0x25 = 2 bytes
        let v2 = Value::NumericString(Box::new(Value::Uint(5)));
        let enc2 = encode(&v2).unwrap();
        assert_eq!(enc2, vec![0x80, 0x25]);
        assert_eq!(decode(&enc2).unwrap(), v2);
        assert_eq!(render_json(&v2), r#""5""#);

        // wrap nint_inline -1: tag 0x80 + 0x31
        let v3 = Value::NumericString(Box::new(Value::NegInt(1)));
        let enc3 = encode(&v3).unwrap();
        assert_eq!(enc3, vec![0x80, 0x31]);
        assert_eq!(render_json(&v3), r#""-1""#);

        // Reject low_nibble != 0
        assert!(matches!(decode(&[0x81]), Err(DecodeError::ReservedLowNibble(_, _))));

        // Reject inner = bool (not numeric)
        // Wire: 0x80 0x11 — should fail at the inner-numeric check
        let r = decode(&[0x80, 0x11]);
        assert!(matches!(r, Err(DecodeError::Truncated(_))));

        // Reject encoder for non-numeric inner
        let bad = Value::NumericString(Box::new(Value::Bool(true)));
        assert!(matches!(encode(&bad), Err(EncodeError::Overflow(_))));
    }

    #[test]
    fn bytes_round_trip_all_hints() {
        let raw = vec![0xDE, 0xAD, 0xBE, 0xEF];

        // Hint 0 = base64
        let v0 = Value::Bytes { encoding_hint: 0, content: raw.clone() };
        assert_eq!(encode(&v0).unwrap(), vec![0xA0, 0xDE, 0xAD, 0xBE, 0xEF]);
        assert_eq!(decode(&encode(&v0).unwrap()).unwrap(), v0);
        assert_eq!(render_json(&v0), r#""3q2+7w==""#);

        // Hint 1 = hex
        let v1 = Value::Bytes { encoding_hint: 1, content: raw.clone() };
        assert_eq!(encode(&v1).unwrap(), vec![0xA1, 0xDE, 0xAD, 0xBE, 0xEF]);
        assert_eq!(render_json(&v1), r#""deadbeef""#);

        // Hint 2 = base58 (Bitcoin alphabet, no `0`/`O`/`I`/`l`)
        let v2 = Value::Bytes { encoding_hint: 2, content: raw.clone() };
        // 0xDEADBEEF = 3735928559. base58 encoding...
        // We trust the round-trip; just check it's not empty and has no forbidden chars.
        let json2 = render_json(&v2);
        assert!(json2.starts_with('"') && json2.ends_with('"'));
        let body = &json2[1..json2.len()-1];
        for c in body.chars() {
            assert!(!"0OIl".contains(c), "base58 must not use 0/O/I/l");
        }

        // Hint 3 = base64url (no `=` padding)
        let v3 = Value::Bytes { encoding_hint: 3, content: raw.clone() };
        assert_eq!(render_json(&v3), r#""3q2-7w""#);

        // Reject low_nibble 4..15
        assert!(matches!(decode(&[0xA4]), Err(DecodeError::ReservedLowNibble(_, _))));
    }

    #[test]
    fn string_round_trip() {
        // raw_text "hello" → tag 0x90 + "hello" bytes
        let v = Value::String { encoding_flag: 0, content: b"hello".to_vec() };
        let enc = encode(&v).unwrap();
        assert_eq!(enc, vec![0x90, b'h', b'e', b'l', b'l', b'o']);
        assert_eq!(decode(&enc).unwrap(), v);
        assert_eq!(render_json(&v), r#""hello""#);

        // escape_form "hi" → tag 0x91 + "hi" bytes
        let v2 = Value::String { encoding_flag: 1, content: b"hi".to_vec() };
        let enc2 = encode(&v2).unwrap();
        assert_eq!(enc2, vec![0x91, b'h', b'i']);
        assert_eq!(decode(&enc2).unwrap(), v2);

        // raw_text containing a quote: bytes are `say "hi"`. JSON
        // emit escapes the quote: `"say \"hi\""`.
        let v3 = Value::String { encoding_flag: 0, content: b"say \"hi\"".to_vec() };
        assert_eq!(render_json(&v3), r#""say \"hi\"""#);

        // escape_form containing a literal backslash-quote: bytes
        // are exactly `say \"hi\"` (8 chars). JSON emit just wraps —
        // no extra escaping (the bytes ARE already escape-form).
        let v4 = Value::String { encoding_flag: 1, content: b"say \\\"hi\\\"".to_vec() };
        assert_eq!(render_json(&v4), r#""say \"hi\"""#);

        // Reject low_nibble > 1.
        assert!(matches!(decode(&[0x92, b'a']), Err(DecodeError::ReservedLowNibble(_, _))));
    }

    #[test]
    fn row_array_round_trip() {
        // Empty row_array (no keys, no rows).
        let v0 = Value::RowArray { keys: vec![], rows: vec![] };
        let enc0 = encode(&v0).unwrap();
        let dec0 = decode(&enc0).unwrap();
        assert_eq!(dec0, v0);

        // Single record [{x: 1, y: 2}]
        let v1 = Value::RowArray {
            keys: vec!["x".to_string(), "y".to_string()],
            rows: vec![vec![Value::Uint(1), Value::Uint(2)]],
        };
        let enc1 = encode(&v1).unwrap();
        let dec1 = decode(&enc1).unwrap();
        assert_eq!(dec1, v1);
        assert_eq!(render_json(&dec1), r#"[{"x":1,"y":2}]"#);

        // Three records, each with three keys (a, b, c)
        let v3 = Value::RowArray {
            keys: vec!["a".to_string(), "b".to_string(), "c".to_string()],
            rows: vec![
                vec![Value::Uint(1), Value::Bool(true),  Value::Null],
                vec![Value::Uint(2), Value::Bool(false), Value::NegInt(1)],
                vec![Value::Uint(3), Value::Bool(true),  Value::Uint(42)],
            ],
        };
        let enc3 = encode(&v3).unwrap();
        let dec3 = decode(&enc3).unwrap();
        assert_eq!(dec3, v3);
        assert_eq!(
            render_json(&dec3),
            r#"[{"a":1,"b":true,"c":null},{"a":2,"b":false,"c":-1},{"a":3,"b":true,"c":42}]"#
        );
    }

    #[test]
    fn key_hash8_strips_trailing_dot_suffix() {
        // "amount.decimal" and "amount" share the same hash byte
        // (suffix-stripping for the prefilter, §5.3).
        assert_eq!(key_hash8("amount.decimal"), key_hash8("amount"));
        assert_eq!(key_hash8("avatar.b64"), key_hash8("avatar"));
        // Different unsuffixed keys should generally differ — this
        // isn't guaranteed (8-bit hash) but is overwhelmingly likely
        // for random pairs:
        assert_ne!(key_hash8("foo"), key_hash8("bar"));
    }

    #[test]
    fn array_adaptive_slot_width() {
        // Small array uses u8 slots.
        let small = Value::Array((0u128..3).map(Value::Uint).collect());
        let enc_small = encode(&small).unwrap();
        // width byte = 0 (u8 slots)
        assert_eq!(enc_small[1], 0x00);

        // Array large enough to force u16 slots: needs value_data > 256
        // bytes. Each child u128_max (Uint with bc=16) is 17 bytes. 16
        // children = 272 bytes value_data, forcing slot_w_code = 1.
        let big_uint = Value::Uint(u128::MAX);
        let big = Value::Array(vec![big_uint; 16]);
        let enc_big = encode(&big).unwrap();
        assert_eq!(enc_big[1], 0x01, "u16 slots when value_data > 256");
        // Round-trip preserves all 16 children byte-exact.
        assert_eq!(decode(&enc_big).unwrap(), big);
    }

    #[test]
    fn long_key_varuint_truncation_rejected() {
        // E-010: build an object whose key uses the long-key escape
        // (key_size_byte = 0xFF) but the varuint excess claims more
        // bytes than remain in the entry. Decoder must reject without
        // running off the end.
        //
        // Start with a valid 255-byte-key wire, then overwrite the
        // varuint excess byte to claim an additional 0x40 (=64) bytes
        // beyond what the entry actually holds.
        let key = "b".repeat(255);
        let mut wire = encode(&Value::Object(vec![
            (key, Value::Uint(1)),
        ])).unwrap();
        // First byte of the long-key entry's value_data is the varuint
        // excess. value_data starts at offset 2 (tag + width byte).
        // Varuint zero is one byte (0x00); patch to 0x40 → claims an
        // extra 0x10 (16) bytes since varuint payload is in top 6 bits.
        let varuint_pos = 2;
        wire[varuint_pos] = 0xFD;   // 2-bit prefix = 11 → 4-byte varuint
        wire[varuint_pos + 1] = 0xFF;
        wire[varuint_pos + 2] = 0xFF;
        wire[varuint_pos + 3] = 0xFF;
        let r = decode(&wire);
        assert!(r.is_err(), "long-key truncated varuint must be rejected");
    }

    #[test]
    fn row_array_random_access_by_record_and_key() {
        // RA-002: decode a row_array and access (record_index, key)
        // pairs without iterating the full structure. The shared key
        // block lives once at the array level — random access only
        // needs the keys table + the slot offset for the given row.
        let keys = vec!["a".to_string(), "b".to_string()];
        let rows = vec![
            vec![Value::Uint(1), Value::Uint(10)],
            vec![Value::Uint(2), Value::Uint(20)],
            vec![Value::Uint(3), Value::Uint(30)],
        ];
        let v = Value::RowArray { keys: keys.clone(), rows: rows.clone() };
        let enc = encode(&v).unwrap();
        let dec = decode(&enc).unwrap();

        // Access (1, "b") → 20 — the canonical "random access" target.
        let got = row_array_get(&dec, 1, "b").expect("(1, b) lookup");
        assert_eq!(*got, Value::Uint(20));

        // Access (2, "a") → 3.
        let got = row_array_get(&dec, 2, "a").expect("(2, a) lookup");
        assert_eq!(*got, Value::Uint(3));

        // Out-of-range record returns None (not panic).
        assert!(row_array_get(&dec, 99, "a").is_none());
        // Unknown key returns None.
        assert!(row_array_get(&dec, 0, "c").is_none());
    }

    #[test]
    fn numeric_string_dual_projection() {
        // NS-002: a numeric_string value can be projected as either
        // its inner numeric (e.g., uint 7) or as the string form
        // ("7"). The dual access enables consumers to either parse
        // the number for arithmetic or read the string verbatim.
        let v = Value::NumericString(Box::new(Value::Uint(7)));

        // as_numeric: returns the inner numeric value.
        let inner = numeric_string_as_numeric(&v).expect("inner is numeric");
        assert_eq!(*inner, Value::Uint(7));

        // as_string: returns the canonical decimal source string.
        let s = numeric_string_as_string(&v).expect("can render as string");
        assert_eq!(s, "7");

        // Decimal inner: as_numeric returns Decimal, as_string returns
        // the canonical-form decimal string.
        let v2 = Value::NumericString(Box::new(
            Value::Decimal { mantissa: 314, scale: -2 }));
        assert_eq!(*numeric_string_as_numeric(&v2).unwrap(),
                   Value::Decimal { mantissa: 314, scale: -2 });
        assert_eq!(numeric_string_as_string(&v2).unwrap(), "3.14");

        // Non-numeric_string values: both projections return None.
        let plain = Value::Uint(7);
        assert!(numeric_string_as_numeric(&plain).is_none());
        assert!(numeric_string_as_string(&plain).is_none());
    }

    #[test]
    fn nesting_depth_capped() {
        // LIM-006: build an array nest exactly at the cap (passes) and
        // one over (fails). Build inside-out: innermost is a leaf,
        // each outer wraps the previous in a single-element array.
        let cap = MAX_DECODE_DEPTH as usize;
        let mut v = Value::Uint(0);
        for _ in 0..cap - 1 {
            v = Value::Array(vec![v]);
        }
        let enc = encode(&v).unwrap();
        assert!(decode(&enc).is_ok(), "depth {} must succeed", cap);

        // One more level — over the cap.
        let too_deep = Value::Array(vec![v]);
        let enc2 = encode(&too_deep).unwrap();
        let r = decode(&enc2);
        assert!(r.is_err(), "depth {} must reject (LIM-006)", cap + 1);
    }

    #[test]
    fn limits_enforced() {
        // LIM-001: container count > 65 535 → encoder rejects.
        let n = 65_536;
        let arr = Value::Array((0u128..n as u128).map(Value::Uint).collect());
        let r = encode(&arr);
        assert!(r.is_err(), "array count {} must be rejected", n);

        // LIM-005: varscale scale outside ±2^29-1 → encoder rejects.
        let bad_scale = Value::Decimal { mantissa: 1, scale: 600_000_000 };
        assert!(encode(&bad_scale).is_err());
        let bad_neg = Value::Decimal { mantissa: 1, scale: -600_000_000 };
        assert!(encode(&bad_neg).is_err());

        // LIM-005 boundary: scale = ±(2^29 - 1) succeeds.
        let max_scale = Value::Decimal { mantissa: 1, scale: 536_870_911 };
        assert!(encode(&max_scale).is_ok());
        let min_scale = Value::Decimal { mantissa: 1, scale: -536_870_911 };
        assert!(encode(&min_scale).is_ok());
    }

    #[test]
    fn json_ingress_rejects_nan_and_infinity_tokens() {
        // J-012: NaN / ±Inf are not valid JSON tokens. The JSON parser
        // must reject them — they cannot enter a pjson value via JSON
        // ingress. (Float NaN/Inf can only enter via typed float input,
        // which goes through encode() directly, not from_json().)
        for bad in ["NaN", "Infinity", "-Infinity", "nan", "inf"] {
            let r = from_json(bad);
            assert!(r.is_err(), "JSON parser must reject {:?}", bad);
        }
    }

    #[test]
    fn integer_canonical_smallest_tag_form() {
        // C-001 / §15.2: encoder must pick the smallest tag form per
        // value range.
        for n in 0u128..=15 {
            let enc = encode(&Value::Uint(n)).unwrap();
            assert_eq!(enc, vec![0x20 | n as u8],
                       "uint {} must use uint_inline (1 byte)", n);
        }
        // 16: smallest non-inline; bc=1
        let enc = encode(&Value::Uint(16)).unwrap();
        assert_eq!(enc, vec![0x40, 0x10], "uint 16 must use uint bc=1");
        // 255: bc=1
        let enc = encode(&Value::Uint(255)).unwrap();
        assert_eq!(enc, vec![0x40, 0xFF]);
        // 256: bc=2
        let enc = encode(&Value::Uint(256)).unwrap();
        assert_eq!(enc, vec![0x41, 0x00, 0x01]);
        // u64::MAX: bc=8
        let enc = encode(&Value::Uint(u64::MAX as u128)).unwrap();
        assert_eq!(enc[0], 0x47);
        // u64::MAX + 1: bc=9
        let enc = encode(&Value::Uint((u64::MAX as u128) + 1)).unwrap();
        assert_eq!(enc[0], 0x48);

        // negint: -1..-15 inline
        for mag in 1u128..=15 {
            let enc = encode(&Value::NegInt(mag)).unwrap();
            assert_eq!(enc, vec![0x30 | mag as u8],
                       "negint -{} must use nint_inline", mag);
        }
        // -16: smallest negint; bc=1
        let enc = encode(&Value::NegInt(16)).unwrap();
        assert_eq!(enc, vec![0x50, 0x10]);
    }

    #[test]
    fn object_field_encounter_order_preserved() {
        // C-005 / §15.4: field order is application-defined and MUST be
        // preserved across round-trip. pjson does NOT canonicalize via
        // sorting at the format level.
        let entries = vec![
            ("zebra".to_string(),  Value::Uint(1)),
            ("alpha".to_string(),  Value::Uint(2)),
            ("middle".to_string(), Value::Uint(3)),
        ];
        let v = Value::Object(entries.clone());
        let enc = encode(&v).unwrap();
        let dec = decode(&enc).unwrap();
        match dec {
            Value::Object(got) => assert_eq!(got, entries,
                "field encounter order must be preserved (no sort)"),
            _ => panic!("decoded non-Object"),
        }
        // The wire bytes reflect the source order verbatim — an alpha-
        // sorted version would produce different bytes.
        let sorted_entries = vec![
            ("alpha".to_string(),  Value::Uint(2)),
            ("middle".to_string(), Value::Uint(3)),
            ("zebra".to_string(),  Value::Uint(1)),
        ];
        let enc_sorted = encode(&Value::Object(sorted_entries)).unwrap();
        assert_ne!(enc, enc_sorted,
            "different field orders must produce different wire bytes");
    }

    #[test]
    fn float_canonical_smallest_width() {
        // C-002: encode_canonical narrows IEEE width to the smallest
        // bit-exact form. 1.0 fits in f16; 1.0f32 stays at f32 if
        // canonical_float_width sees it as f32 source — but it would
        // narrow to f16 too since 1.0 is exact at every width.

        // 1.0 stored as f64 → canonical narrows to f16.
        let f64_one = Value::Float { width_log2: 3,
            bits: 1.0f64.to_bits() as u128 };
        let canon = canonicalize_value(&f64_one);
        match canon {
            Value::Float { width_log2: 1, bits } => {
                assert_eq!(bits, 0x3C00); // f16(1.0)
            }
            other => panic!("expected width=1, got {:?}", other),
        }

        // 1.5 stored as f64 → narrows to f16 (also exact).
        let v = Value::Float { width_log2: 3,
            bits: 1.5f64.to_bits() as u128 };
        match canonicalize_value(&v) {
            Value::Float { width_log2: 1, .. } => {}
            other => panic!("1.5 should narrow to f16, got {:?}", other),
        }

        // A value that doesn't fit in f16 but fits in f32: 1e-7
        let v = Value::Float { width_log2: 3,
            bits: (1e-7f32 as f64).to_bits() as u128 };
        match canonicalize_value(&v) {
            Value::Float { width_log2: w, .. } if w == 1 || w == 2 => {}
            other => panic!("1e-7 should narrow to f16 or f32, got {:?}", other),
        }

        // π in f64 — doesn't fit in f32, stays at f64.
        let pi = Value::Float { width_log2: 3, bits: std::f64::consts::PI.to_bits() as u128 };
        match canonicalize_value(&pi) {
            Value::Float { width_log2: 3, .. } => {}
            other => panic!("π should stay f64, got {:?}", other),
        }
    }

    #[test]
    fn validate_canonical_round_trip() {
        // C-006: a canonical wire validates; a non-canonical one
        // (uint encoding 5 as bc=1 instead of inline) does not.
        let canonical = encode(&Value::Uint(5)).unwrap();   // 0x25
        assert_eq!(canonical, vec![0x25]);
        assert!(validate_canonical(&canonical).is_ok());

        // Hand-craft a non-canonical 5 as uint with bc=1 → 0x40 0x05.
        let noncanonical = vec![0x40, 0x05];
        assert!(validate_canonical(&noncanonical).is_err());

        // f64(1.0) — non-canonical because 1.0 fits in f16.
        let mut nc_float = vec![0x63];
        nc_float.extend_from_slice(&1.0f64.to_bits().to_le_bytes());
        assert!(validate_canonical(&nc_float).is_err());

        // f16(1.0) — canonical: 0x61 + LE bytes 00 3C
        let canon_one = vec![0x61, 0x00, 0x3C];
        assert!(validate_canonical(&canon_one).is_ok());
    }

    #[test]
    fn nan_canonicalized_on_encode() {
        // §15.2.1 / F-008: encoders rewrite any NaN bit pattern to the
        // canonical quiet-NaN-zero-payload form at the chosen width.
        // Inputs are non-canonical NaNs; the wire bytes after encode
        // must match the canonical pattern.
        //
        // binary16: any NaN → 0x7E00 (LE: 00 7E)
        let nc16 = Value::Float { width_log2: 1, bits: 0x7E01u128 }; // payload bit set
        let enc = encode(&nc16).unwrap();
        assert_eq!(&enc[1..], &[0x00, 0x7E], "binary16 NaN canonical");

        // binary32: any NaN → 0x7FC00000 (LE: 00 00 C0 7F)
        let nc32 = Value::Float { width_log2: 2, bits: 0x7FC00001u128 };
        let enc = encode(&nc32).unwrap();
        assert_eq!(&enc[1..], &[0x00, 0x00, 0xC0, 0x7F]);

        // binary64: any NaN → 0x7FF8000000000000
        let nc64 = Value::Float { width_log2: 3, bits: 0x7FF8000000000123u128 };
        let enc = encode(&nc64).unwrap();
        assert_eq!(&enc[1..], &[0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x7F]);

        // binary128: any NaN → 0x7FFF8000_00000000_00000000_00000000
        // Build a non-canonical 128-bit NaN: exp = 0x7FFF, with stray
        // mantissa bits beyond the quiet bit.
        let nc128_bits: u128 = (0x7FFFu128 << 112) | (1u128 << 110) | 0x42u128;
        let nc128 = Value::Float { width_log2: 4, bits: nc128_bits };
        let enc = encode(&nc128).unwrap();
        let canon128: u128 = (0x7FFFu128 << 112) | (1u128 << 111);
        assert_eq!(&enc[1..], &canon128.to_le_bytes());

        // ±Inf passes through unchanged (mantissa is zero — not a NaN).
        let plus_inf = Value::Float { width_log2: 3, bits: 0x7FF0000000000000u128 };
        let enc = encode(&plus_inf).unwrap();
        assert_eq!(&enc[1..], &0x7FF0000000000000u128.to_le_bytes()[..8]);

        // Finite values pass through.
        let one = Value::Float { width_log2: 3, bits: (1.0f64).to_bits() as u128 };
        let enc = encode(&one).unwrap();
        assert_eq!(&enc[1..], &(1.0f64).to_bits().to_le_bytes());
    }

    #[test]
    fn array_adaptive_slot_width_u24_u32() {
        // u24 selection: value_data ∈ [65536, 2^24). One String of 65540
        // bytes (tag+size header + content) lands the value_data over
        // 65536, but a single-element array still uses u8. Build many
        // mid-size strings to push past the boundary while keeping >1
        // child so multiple slots are written.
        let s_mid = "x".repeat(8200);
        let many: Vec<Value> = (0..10)
            .map(|_| Value::String { encoding_flag: 0, content: s_mid.clone().into_bytes() })
            .collect();
        let big = Value::Array(many);
        let enc = encode(&big).unwrap();
        assert_eq!(enc[1], 0x02, "u24 slots (width_code=2) for value_data > 65535");
        assert_eq!(decode(&enc).unwrap(), big);

        // u32 selection: > 2^24 bytes value_data. Each string is 1.7M
        // bytes; 10 of them push value_data past 16 MiB.
        let s_big = "y".repeat(1_700_000);
        let many32: Vec<Value> = (0..10)
            .map(|_| Value::String { encoding_flag: 0, content: s_big.clone().into_bytes() })
            .collect();
        let huge = Value::Array(many32);
        let enc32 = encode(&huge).unwrap();
        assert_eq!(enc32[1], 0x03, "u32 slots (width_code=3) for value_data > 2^24");
        assert_eq!(decode(&enc32).unwrap(), huge);
    }

    #[test]
    fn object_long_key_64kib_round_trip() {
        // H-005: a single object with a 64 KiB key. The §5.2 long-key
        // escape stores `key_size_byte = 0xFF` and a varuint excess
        // inline; for keys of 65535+ bytes the varuint reaches its
        // 4-byte form (varuint encodes (size − 254)).
        let huge_key = "k".repeat(65_536);
        let entries = vec![(huge_key.clone(), Value::Uint(7))];
        let v = Value::Object(entries);
        let enc = encode(&v).unwrap();
        let dec = decode(&enc).unwrap();
        assert_eq!(dec, v);
    }

    #[test]
    fn zigzag_round_trip() {
        for s in [0i32, 1, -1, 100, -100, i32::MAX, i32::MIN, i32::MAX - 1] {
            assert_eq!(zigzag_decode_u32(zigzag_encode_i32(s)), s, "i32 {}", s);
        }
        for s in [0i128, 1, -1, i128::MAX, i128::MIN, i128::MIN + 1] {
            assert_eq!(zigzag_decode_u128(zigzag_encode_i128(s)), s, "i128 {}", s);
        }
    }

    /// T-016, T-017, V-003 — verify that the §3 type-class predicates
    /// hold for every possible high nibble (0..15), and that the
    /// dispatch sub-bits within `is_integer` (codes 2..5) match the
    /// spec's stated layout (`bit 0 = sign`, `bit 1 = inline form`).
    ///
    /// These predicates aren't called by the driver directly — the
    /// driver dispatches via a `match` on the high nibble, which is
    /// equivalent. The test makes the spec's claims runtime-checkable
    /// so a future change that breaks the layout fails loud.
    #[test]
    fn tag_byte_predicates_match_spec_section_3() {
        for high in 0u8..=15 {
            // Property: every high nibble lands in exactly one type-class.
            let is_atom              = high <= 1;
            let is_integer           = (2..=5).contains(&high);
            let is_real              = (6..=7).contains(&high);
            let is_numeric_value     = (2..=7).contains(&high);
            let is_number_projectable= (2..=8).contains(&high);
            let is_json_string_emit  = (8..=10).contains(&high);
            let is_aggregate         = (11..=12).contains(&high);
            let is_extension         = high == 13;
            let is_reserved          = high >= 14;

            // Mutual exclusion among the disjoint classes.
            let class_count = [is_atom, is_integer, is_real,
                               is_aggregate, is_extension, is_reserved,
                               // string/bytes/numeric_string region:
                               (8..=10).contains(&high)]
                              .iter().filter(|x| **x).count();
            assert!(class_count == 1, "high {high} ambiguous class");

            // Composite predicates (overlapping with their components).
            assert_eq!(is_numeric_value, is_integer || is_real);
            assert_eq!(is_number_projectable, is_numeric_value || high == 8);
            assert_eq!(is_json_string_emit, high == 8 || (9..=10).contains(&high));
        }

        // V-003: reserved-tag fast-path predicate. The parser sees a
        // full tag byte and computes (tag >> 4) — for high nibble N
        // this is just N. The "reserved" predicate `(tag >> 4) >= 14`
        // is therefore identical to `high >= 14`.
        for high in 0u8..=15 {
            let tag: u8 = high << 4;
            let predicate = (tag >> 4) >= 14;
            assert_eq!(predicate, high >= 14, "high {high}");
        }

        // T-017: bit pattern within is_integer.
        // code 2 = uint_inline:   sign=0, inline=1 → bits 0010
        // code 3 = nint_inline:   sign=1, inline=1 → bits 0011
        // code 4 = uint:          sign=0, inline=0 → bits 0100
        // code 5 = negint:        sign=1, inline=0 → bits 0101
        for high in 2u8..=5 {
            let sign_bit   = high & 0x01 != 0;
            let inline_bit = high & 0x02 != 0;
            let expected_signed   = matches!(high, 3 | 5);
            let expected_inline   = matches!(high, 2 | 3);
            assert_eq!(sign_bit,   expected_signed,   "code {high} sign bit");
            assert_eq!(inline_bit, expected_inline,   "code {high} inline bit");
        }

        // Round-trip the predicates against the driver's actual decode
        // dispatch: every reserved high nibble must error on the
        // smallest-possible buffer, and every defined high nibble
        // (excluding reserved) must NOT raise ReservedTag.
        for high in 0u8..=15 {
            let tag: u8 = high << 4;
            // Some defined arms still error (e.g. truncated payload),
            // but the error is NOT ReservedTag for high < 14.
            let result = decode(&[tag]);
            if high >= 14 {
                assert!(matches!(result, Err(DecodeError::ReservedTag(_))),
                        "high {high} should be ReservedTag");
            } else {
                if let Err(DecodeError::ReservedTag(_)) = result {
                    panic!("high {high} should not be ReservedTag");
                }
            }
        }
    }
}
