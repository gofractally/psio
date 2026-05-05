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
}

// ── Encode (§12 reference algorithm) ────────────────────────────────

#[derive(Debug)]
enum EncodeError {
    /// Value out of range for the spec (e.g. mantissa needs > 16 bytes).
    Overflow(&'static str),
    /// Variant not implemented in Phase 1 yet.
    NotImplementedYet(&'static str),
}

fn encode(v: &Value) -> Result<Vec<u8>, EncodeError> {
    let mut out = Vec::new();
    encode_into(v, &mut out)?;
    Ok(out)
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
            // Lay down byte_count bytes of `bits`, little-endian.
            let raw = bits.to_le_bytes();
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
    }
    Ok(())
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

fn u128_le_minimal_at_least_1(n: u128) -> Vec<u8> {
    let mut v = u128_le_minimal(n);
    if v.is_empty() {
        v.push(0);
    }
    v
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
fn varscale_encode_into(scale: i32, out: &mut Vec<u8>) -> Result<(), EncodeError> {
    let zz = zigzag_encode_i32(scale);
    let total_bytes = if zz < (1u32 << 6) {
        1
    } else if zz < (1u32 << 14) {
        2
    } else if zz < (1u32 << 22) {
        3
    } else if zz < (1u32 << 30) {
        4
    } else {
        return Err(EncodeError::Overflow("varscale"));
    };
    // First byte: prefix + low 6 bits of zigzag.
    let prefix = ((total_bytes - 1) as u8) << 6;
    let lo6 = (zz & 0x3F) as u8;
    out.push(prefix | lo6);
    // Subsequent bytes: 8 bits each, starting at bit 6.
    let mut shifted = zz >> 6;
    for _ in 1..total_bytes {
        out.push((shifted & 0xFF) as u8);
        shifted >>= 8;
    }
    Ok(())
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

fn decode(buf: &[u8]) -> Result<Value, DecodeError> {
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
        8 | 9 | 10 | 11 | 12 => {
            Err(DecodeError::NotImplementedYet(match high {
                8  => "numeric_string",
                9  => "string",
                10 => "bytes",
                11 => "array",
                12 => "object",
                _  => unreachable!(),
            }))
        }
        13 => Err(DecodeError::NotImplementedYet("extension")),
        14 | 15 => Err(DecodeError::ReservedTag(tag)),
        _ => unreachable!(),
    }
}

fn read_u128_le(bytes: &[u8]) -> u128 {
    let mut v: u128 = 0;
    for (i, b) in bytes.iter().enumerate().take(16) {
        v |= (*b as u128) << (8 * i);
    }
    v
}

fn varscale_decode(buf: &[u8]) -> Result<(i32, usize), DecodeError> {
    if buf.is_empty() {
        return Err(DecodeError::Truncated("varscale first byte"));
    }
    let total_bytes = ((buf[0] >> 6) as usize) + 1;
    if buf.len() < total_bytes {
        return Err(DecodeError::Truncated("varscale body"));
    }
    let mut zz: u32 = (buf[0] & 0x3F) as u32;
    for i in 1..total_bytes {
        zz |= (buf[i] as u32) << (6 + 8 * (i - 1));
    }
    Ok((zigzag_decode_u32(zz), total_bytes))
}

// ── JSON projection ─────────────────────────────────────────────────

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
    wire_hex: String,
    json_compact: Option<String>,
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

    Ok(Fixture {
        id,
        input_value,
        wire_hex,
        json_compact,
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
        other => Err(format!("input_value kind '{}' not supported in Phase 1", other)),
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
