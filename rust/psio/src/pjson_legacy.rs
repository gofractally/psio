//! pjson — binary JSON with zero-copy random access.
//!
//! Wire format spec lives at `docs/pjson-spec.md` in the repo root.
//! This module is the canonical Rust implementation: tag enum, runtime
//! `Value` type, single-pass forward-write encoder, recursive validator
//! / decoder, error type, and the typed `Pjson` trait that user types
//! `#[derive(Pjson)]` (or use the `pjson_struct!` macro) to participate in.
//!
//! Cross-references in the spec:
//!   - §3 Tag byte (high nibble = type code, low nibble = type-specific)
//!   - §4 Scalar encodings (null, bool, uint_inline, uint, negint, decimal, ieee_float, string, bytes)
//!   - §5 Containers (generic array, typed_array, single object, row_array, hash prefilter, long-key escape)
//!   - §10 Reference read / decode algorithm
//!   - §11 Reference object lookup by key
//!   - §12 Reference encode algorithm
//!   - §15 Canonical encoding rules
//!
//! Source-of-truth reminder: the C++ reference implementation under
//! `cpp/include/psio/pjson*.hpp` is being brought into spec conformance
//! on a sibling branch. This module follows the spec as authoritative.
//!
//! Status of this implementation:
//!   - Scalars: null, bool, uint_inline, uint, negint, ieee_float
//!     (binary64 only on encode side; binary16 / binary128 codecs are
//!     reject-on-decode for now — `PjsonError::NotImplemented`),
//!     decimal (with varscale), string (raw_text + escape_form), bytes.
//!   - Containers: generic array, typed_array (codes 0..9 = i8..f64),
//!     generic object, row_array. Adaptive slot-table widths
//!     (u8/u16/u24/u32). Hash prefilter on objects. Long-key escape.
//!   - Runtime `Value` type with encode + decode + validate.
//!   - Typed `Pjson` trait (Pjson::pjson_size / pjson_encode_at /
//!     pjson_decode / pjson_validate) implemented for primitives and
//!     std types; user struct support via the `pjson_struct!` macro
//!     (see `pjson_derive.rs`).
//!   - Errors: PjsonError(&'static str) — zero-allocation, easy to
//!     compare in tests; matches the pssz error style.

use crate::xxh3_64;

// ── Tag byte constants (spec §3) ────────────────────────────────────────────

/// High nibble = type code. Low nibble is type-specific.
pub mod tag {
    pub const NULL: u8 = 0;
    pub const BOOL: u8 = 1;
    // 2 reserved
    pub const UINT_INLINE: u8 = 3;
    pub const UINT: u8 = 4;
    pub const DECIMAL: u8 = 5;
    pub const IEEE_FLOAT: u8 = 6;
    pub const NEGINT: u8 = 7;
    pub const STRING: u8 = 8;
    // 9 reserved
    pub const BYTES: u8 = 0xA;
    pub const ARRAY: u8 = 0xB;
    pub const OBJECT: u8 = 0xC;
    // 13..15 reserved
}

/// String low-nibble flags (§4.7).
pub mod str_flag {
    pub const RAW_TEXT: u8 = 0;
    pub const ESCAPE_FORM: u8 = 1;
}

/// Typed-array element codes (§5.1.1). On the wire the low nibble is `code + 1`.
pub mod tac {
    pub const I8: u8 = 0;
    pub const I16: u8 = 1;
    pub const I32: u8 = 2;
    pub const I64: u8 = 3;
    pub const U8: u8 = 4;
    pub const U16: u8 = 5;
    pub const U32: u8 = 6;
    pub const U64: u8 = 7;
    pub const F32: u8 = 8;
    pub const F64: u8 = 9;
}

/// Per-element byte count for a typed-array element type code.
pub fn typed_array_elem_size(code: u8) -> usize {
    match code {
        tac::I8 | tac::U8 => 1,
        tac::I16 | tac::U16 => 2,
        tac::I32 | tac::U32 | tac::F32 => 4,
        tac::I64 | tac::U64 | tac::F64 => 8,
        _ => 0,
    }
}

// ── Object form selectors (§5.2) ────────────────────────────────────────────

pub mod obj_form {
    pub const SINGLE: u8 = 0;
    pub const ROW_ARRAY: u8 = 1;
}

// ── Errors ──────────────────────────────────────────────────────────────────

/// pjson decode / validate error. Zero-allocation; carries a static
/// description matching pssz's error style.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PjsonError(pub &'static str);

impl core::fmt::Display for PjsonError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str(self.0)
    }
}

impl std::error::Error for PjsonError {}

pub type PjsonResult<T> = Result<T, PjsonError>;

// Helper: build an error.
const fn err(msg: &'static str) -> PjsonError {
    PjsonError(msg)
}

// ── Key prefilter hash (§5.3) ───────────────────────────────────────────────

/// Strip a trailing `.tag` suffix from a key for the prefilter hash.
/// Mirrors `psio::strip_key_suffix` in C++.
#[inline]
pub fn strip_key_suffix(k: &[u8]) -> &[u8] {
    match k.iter().rposition(|&b| b == b'.') {
        Some(idx) => &k[..idx],
        None => k,
    }
}

/// 8-bit prefilter hash: low byte of XXH3-64 over the suffix-stripped key.
#[inline]
pub fn key_hash8(k: &[u8]) -> u8 {
    xxh3_64::hash(strip_key_suffix(k)) as u8
}

// ── 2-bit-prefix varuint (§4.5.1, also used for long-key escape §5.4) ───────

/// Number of bytes required to encode `v` as a 2-bit-prefix varuint.
#[inline]
pub fn varuint_byte_count(v: u64) -> usize {
    if v <= 0x3F {
        1
    } else if v <= 0x3FFF {
        2
    } else if v <= 0x3F_FFFF {
        3
    } else {
        // The spec caps the varuint at 4 bytes / 30 bits. Caller is
        // responsible for ensuring `v <= 0x3FFFFFFF`.
        4
    }
}

/// Write a 2-bit-prefix varuint at dst[pos..]. Returns bytes written.
fn write_varuint(dst: &mut [u8], pos: usize, v: u64) -> usize {
    let n = varuint_byte_count(v);
    let prefix = ((n - 1) as u8) << 6;
    dst[pos] = prefix | ((v & 0x3F) as u8);
    let mut hi = v >> 6;
    for i in 1..n {
        dst[pos + i] = (hi & 0xFF) as u8;
        hi >>= 8;
    }
    n
}

/// Read a 2-bit-prefix varuint from `buf`. Returns `(value, bytes_consumed)`.
pub(crate) fn read_varuint(buf: &[u8]) -> PjsonResult<(u64, usize)> {
    peek_varuint(buf)
}

/// Public read alias used by external transcoders (pjson_json).
/// Equivalent to `read_varuint` but exposed for API consumers.
pub fn peek_varuint(buf: &[u8]) -> PjsonResult<(u64, usize)> {
    if buf.is_empty() {
        return Err(err("pjson: varuint underrun"));
    }
    let b0 = buf[0];
    let n = ((b0 >> 6) as usize) + 1;
    if n > buf.len() {
        return Err(err("pjson: varuint truncated"));
    }
    let mut v = (b0 & 0x3F) as u64;
    for i in 1..n {
        v |= (buf[i] as u64) << (6 + (i - 1) * 8);
    }
    Ok((v, n))
}

/// Public write alias for varuint encoding (used by external
/// transcoders).  Returns bytes written.
pub fn write_varuint_at(dst: &mut [u8], pos: usize, v: u64) -> usize {
    write_varuint(dst, pos, v)
}

// ── Varscale — 2-bit-prefix signed (zigzag) integer for decimal scale ───────

#[inline]
fn zigzag_encode_i32(v: i32) -> u64 {
    ((v as u32 as u64) << 1) ^ ((v as i64 >> 31) as u64)
}

#[inline]
fn zigzag_decode_i32(zz: u64) -> i32 {
    ((zz >> 1) as i64 ^ (-((zz & 1) as i64))) as i32
}

#[inline]
pub fn varscale_byte_count(scale: i32) -> usize {
    varuint_byte_count(zigzag_encode_i32(scale))
}

fn write_varscale(dst: &mut [u8], pos: usize, scale: i32) -> usize {
    write_varuint(dst, pos, zigzag_encode_i32(scale))
}

fn read_varscale(buf: &[u8]) -> PjsonResult<(i32, usize)> {
    let (zz, n) = read_varuint(buf)?;
    Ok((zigzag_decode_i32(zz), n))
}

// ── Adaptive slot widths (§5.6, used by §5.1, §5.2, §5.2.1) ────────────────

#[inline]
fn width_code_for(v: u64) -> u8 {
    if v <= 0xFF {
        0
    } else if v <= 0xFFFF {
        1
    } else if v <= 0xFF_FFFF {
        2
    } else {
        3
    }
}

#[inline]
fn width_bytes(code: u8) -> usize {
    (code as usize) + 1
}

fn write_width(dst: &mut [u8], pos: usize, code: u8, v: u32) {
    let n = width_bytes(code);
    let bytes = v.to_le_bytes();
    dst[pos..pos + n].copy_from_slice(&bytes[..n]);
}

fn read_width(buf: &[u8], pos: usize, code: u8) -> u32 {
    let n = width_bytes(code);
    let mut tmp = [0u8; 4];
    tmp[..n].copy_from_slice(&buf[pos..pos + n]);
    u32::from_le_bytes(tmp)
}

// ── Magnitude helpers for uint / negint (§4.4) ──────────────────────────────

#[inline]
fn magnitude_byte_count_u128(mag: u128) -> usize {
    if mag == 0 {
        1
    } else {
        let bits = 128 - mag.leading_zeros() as usize;
        bits.div_ceil(8)
    }
}

#[inline]
fn read_le_u128(buf: &[u8]) -> u128 {
    let mut tmp = [0u8; 16];
    tmp[..buf.len()].copy_from_slice(buf);
    u128::from_le_bytes(tmp)
}

// ── Runtime Value type ──────────────────────────────────────────────────────

/// Runtime pjson value. Borrows underlying bytes for strings, byte
/// blobs, and typed arrays — see the lifetime parameter.
#[derive(Debug, Clone, PartialEq)]
pub enum Value<'a> {
    Null,
    Bool(bool),
    UInt(u128),
    NegInt(u128), // logical value = -(self as i256)
    Decimal {
        // Mantissa zigzag-encoded; producer chose smallest bc. We store
        // the decoded signed mantissa as i128 (covers spec's 16-byte
        // mantissa range up to ±2^127).
        mantissa: i128,
        scale: i32,
    },
    Float(f64),
    /// `Float` with sci-source hint (§4.6 / §7.1) — encoded as
    /// ieee_float with low-nibble bit 3 set so a downstream JSON
    /// emitter can re-emit scientific notation.  Decode-side only
    /// constructs this variant when the sci flag was observed in the
    /// source buffer.
    FloatSci(f64),
    /// `(text, encoding_flag)` — flag 0 = raw_text, 1 = escape_form.
    Str(&'a [u8], u8),
    Bytes(&'a [u8]),
    Array(Vec<Value<'a>>),
    /// Typed homogeneous array: code (0..9) and element bytes (raw, LE).
    TypedArray {
        code: u8,
        elements: &'a [u8],
        count: usize,
    },
    Object(Vec<(&'a [u8], Value<'a>)>),
    /// row_array — homogeneous-shape array of objects, rendered into a
    /// `Vec<Vec<(key, val)>>` on decode.
    RowArray(Vec<Vec<(&'a [u8], Value<'a>)>>),
}

impl<'a> Value<'a> {
    /// Owned String form for a Str-tagged value (best-effort UTF-8).
    pub fn as_str(&self) -> Option<&'a str> {
        match self {
            Value::Str(b, _) => std::str::from_utf8(b).ok(),
            _ => None,
        }
    }
}

// ── Public top-level API ────────────────────────────────────────────────────

/// Top-level decode entry point — parses a complete pjson document.
pub fn decode(bytes: &[u8]) -> PjsonResult<Value<'_>> {
    parse_value(bytes, bytes.len())
}

/// Hard cap on recursion depth for `validate()`. Mirrors the C++
/// `psio::kMaxValidationDepth` constant — the validator MUST refuse
/// any nesting deeper than this regardless of buffer length, since
/// a malicious buffer can claim arbitrarily deep nesting and exhaust
/// the call stack. Decoders are not bound by the cap; they MAY accept
/// deeper trust on pre-validated input. See docs/pssz-spec.md §8.3.
pub const K_MAX_VALIDATION_DEPTH: usize = 64;

/// Top-level validate entry point — walks the buffer once, asserting
/// every internal invariant without materializing values, and
/// rejecting any buffer whose container nesting exceeds
/// `K_MAX_VALIDATION_DEPTH`.
pub fn validate(bytes: &[u8]) -> PjsonResult<()> {
    validate_value_depth(bytes, bytes.len(), 0)
}

// ── Parser ──────────────────────────────────────────────────────────────────

pub(crate) fn parse_value(buf: &[u8], size: usize) -> PjsonResult<Value<'_>> {
    if size == 0 || size > buf.len() {
        return Err(err("pjson: parse_value bad size"));
    }
    let value = &buf[..size];
    let tag = value[0];
    let high = tag >> 4;
    let low = tag & 0x0F;

    match high {
        tag::NULL => {
            if low != 0 || size != 1 {
                return Err(err("pjson: invalid null tag"));
            }
            Ok(Value::Null)
        }
        tag::BOOL => {
            if size != 1 {
                return Err(err("pjson: bool size != 1"));
            }
            match low {
                0 => Ok(Value::Bool(false)),
                1 => Ok(Value::Bool(true)),
                _ => Err(err("pjson: reserved bool low nibble")),
            }
        }
        2 => Err(err("pjson: reserved type code 2")),
        tag::UINT_INLINE => {
            if size != 1 {
                return Err(err("pjson: uint_inline size != 1"));
            }
            Ok(Value::UInt(low as u128))
        }
        tag::UINT => {
            let bc = (low as usize) + 1;
            if !(1..=16).contains(&bc) || 1 + bc != size {
                return Err(err("pjson: uint bad size"));
            }
            let mag = read_le_u128(&value[1..1 + bc]);
            Ok(Value::UInt(mag))
        }
        tag::DECIMAL => {
            let bc = (low as usize) + 1;
            if !(1..=16).contains(&bc) || 1 + bc + 1 > size {
                return Err(err("pjson: decimal bad size"));
            }
            let mantissa_zz = read_le_u128(&value[1..1 + bc]);
            let (scale, scale_bytes) = read_varscale(&value[1 + bc..])?;
            if 1 + bc + scale_bytes != size {
                return Err(err("pjson: decimal trailing bytes"));
            }
            // Decode the zigzag mantissa into i128.
            let mantissa = ((mantissa_zz >> 1) as i128) ^ (-((mantissa_zz & 1) as i128));
            Ok(Value::Decimal { mantissa, scale })
        }
        tag::IEEE_FLOAT => {
            let width_bits = low & 0x07;
            let sci = (low & 0x08) != 0; // §4.6 sci-source hint
            let w: usize = match width_bits {
                1 => 2,
                2 => 4,
                3 => 8,
                4 => 16,
                _ => return Err(err("pjson: ieee_float reserved width")),
            };
            if 1 + w != size {
                return Err(err("pjson: ieee_float size mismatch"));
            }
            // Spec §4.6: producers may emit any width, but a parser
            // must accept all widths it can decode.  binary16 is
            // widened losslessly via the helper in pjson_json.
            // binary128 is deferred — the IEEE-754 quad codec is too
            // large to inline without a dependency.
            let f = match w {
                2 => crate::pjson_legacy_json::f16_bits_to_f64(u16::from_le_bytes([
                    value[1], value[2],
                ])),
                4 => {
                    let mut tmp = [0u8; 4];
                    tmp.copy_from_slice(&value[1..5]);
                    f32::from_le_bytes(tmp) as f64
                }
                8 => {
                    let mut tmp = [0u8; 8];
                    tmp.copy_from_slice(&value[1..9]);
                    f64::from_le_bytes(tmp)
                }
                16 => return Err(err("pjson: NotImplemented binary128")),
                _ => unreachable!(),
            };
            Ok(if sci {
                Value::FloatSci(f)
            } else {
                Value::Float(f)
            })
        }
        tag::NEGINT => {
            let bc = (low as usize) + 1;
            if !(1..=16).contains(&bc) || 1 + bc != size {
                return Err(err("pjson: negint bad size"));
            }
            let mag = read_le_u128(&value[1..1 + bc]);
            if mag == 0 {
                return Err(err("pjson: negint negative-zero reserved"));
            }
            Ok(Value::NegInt(mag))
        }
        tag::STRING => {
            if low > 1 {
                return Err(err("pjson: reserved string flag"));
            }
            Ok(Value::Str(&value[1..], low))
        }
        9 => Err(err("pjson: reserved type code 9")),
        tag::BYTES => {
            if low != 0 {
                return Err(err("pjson: reserved bytes low nibble"));
            }
            Ok(Value::Bytes(&value[1..]))
        }
        tag::ARRAY => match low {
            0 => parse_generic_array(value),
            1..=10 => {
                let code = low - 1;
                parse_typed_array(value, code)
            }
            _ => Err(err("pjson: reserved array low nibble")),
        },
        tag::OBJECT => match low {
            obj_form::SINGLE => parse_object(value),
            obj_form::ROW_ARRAY => parse_row_array(value),
            _ => Err(err("pjson: reserved object form")),
        },
        _ => Err(err("pjson: reserved type code")),
    }
}

fn parse_generic_array(value: &[u8]) -> PjsonResult<Value<'_>> {
    let size = value.len();
    if size < 4 {
        // tag + width + count(2)
        return Err(err("pjson: array too small"));
    }
    let n = u16::from_le_bytes([value[size - 2], value[size - 1]]) as usize;
    let width_byte = value[1];
    let slot_w_code = width_byte & 0x03;
    if (width_byte >> 2) != 0 {
        return Err(err("pjson: array width byte reserved bits set"));
    }
    let slot_w = width_bytes(slot_w_code);
    let value_data_start = 2;
    let slot_table_pos = match (size).checked_sub(2 + slot_w * n) {
        Some(p) if p >= value_data_start => p,
        _ => return Err(err("pjson: array slot_table_pos invalid")),
    };
    let value_data_size = slot_table_pos - value_data_start;

    let mut children = Vec::with_capacity(n);
    for i in 0..n {
        let off_i = read_width(value, slot_table_pos + i * slot_w, slot_w_code) as usize;
        let off_next = if i + 1 < n {
            read_width(value, slot_table_pos + (i + 1) * slot_w, slot_w_code) as usize
        } else {
            value_data_size
        };
        if off_i > off_next || off_next > value_data_size {
            return Err(err("pjson: array slot offset out of range"));
        }
        let child_size = off_next - off_i;
        let child = parse_value(
            &value[value_data_start + off_i..value_data_start + off_i + child_size],
            child_size,
        )?;
        children.push(child);
    }
    Ok(Value::Array(children))
}

fn parse_typed_array(value: &[u8], code: u8) -> PjsonResult<Value<'_>> {
    let size = value.len();
    let elem_size = typed_array_elem_size(code);
    if elem_size == 0 {
        return Err(err("pjson: typed_array reserved code"));
    }
    if size < 3 {
        return Err(err("pjson: typed_array too small"));
    }
    let n = u16::from_le_bytes([value[size - 2], value[size - 1]]) as usize;
    if 1 + n * elem_size + 2 != size {
        return Err(err("pjson: typed_array size mismatch"));
    }
    Ok(Value::TypedArray {
        code,
        elements: &value[1..1 + n * elem_size],
        count: n,
    })
}

fn parse_object(value: &[u8]) -> PjsonResult<Value<'_>> {
    let size = value.len();
    if size < 4 {
        return Err(err("pjson: object too small"));
    }
    let n = u16::from_le_bytes([value[size - 2], value[size - 1]]) as usize;
    let width_byte = value[1];
    let slot_w_code = width_byte & 0x03;
    if (width_byte >> 2) != 0 {
        return Err(err("pjson: object width byte reserved bits set"));
    }
    let slot_w = width_bytes(slot_w_code);
    let entry_stride = slot_w + 1;
    let value_data_start = 2;
    let slot_table_pos = match size.checked_sub(2 + entry_stride * n) {
        Some(p) if p >= value_data_start + n => p,
        _ => return Err(err("pjson: object slot_table_pos invalid")),
    };
    let hash_table_pos = slot_table_pos - n;
    let value_data_size = hash_table_pos - value_data_start;

    let mut entries = Vec::with_capacity(n);
    for i in 0..n {
        let slot = slot_table_pos + i * entry_stride;
        let off_i = read_width(value, slot, slot_w_code) as usize;
        let key_size_byte = value[slot + slot_w];
        let off_next = if i + 1 < n {
            read_width(value, slot_table_pos + (i + 1) * entry_stride, slot_w_code) as usize
        } else {
            value_data_size
        };
        if off_i > off_next || off_next > value_data_size {
            return Err(err("pjson: object slot offset out of range"));
        }
        let entry = &value[value_data_start + off_i..value_data_start + off_next];
        let entry_size = entry.len();
        let (klen, klen_bytes) = if key_size_byte != 0xFF {
            (key_size_byte as usize, 0)
        } else {
            let (excess, n_bytes) = read_varuint(entry)?;
            (0xFF + excess as usize, n_bytes)
        };
        if klen_bytes + klen > entry_size {
            return Err(err("pjson: object key out of range"));
        }
        let key = &entry[klen_bytes..klen_bytes + klen];
        if value[hash_table_pos + i] != key_hash8(key) {
            return Err(err("pjson: object hash mismatch"));
        }
        let child_buf = &entry[klen_bytes + klen..];
        let child = parse_value(child_buf, child_buf.len())?;
        entries.push((key, child));
    }
    Ok(Value::Object(entries))
}

fn parse_row_array(value: &[u8]) -> PjsonResult<Value<'_>> {
    let size = value.len();
    if size < 5 {
        // tag + width byte + at least 1-byte K varuint + count(2)
        return Err(err("pjson: row_array too small"));
    }
    let width_byte = value[1];
    let slot_w_code = width_byte & 0x03;
    let recoff_w_code = (width_byte >> 2) & 0x03;
    if (width_byte >> 4) != 0 {
        return Err(err("pjson: row_array width byte reserved bits"));
    }
    let slot_w = width_bytes(slot_w_code);
    let recoff_w = width_bytes(recoff_w_code);

    let n = u16::from_le_bytes([value[size - 2], value[size - 1]]) as usize;
    let mut pos = 2;

    let (k, k_bytes) = read_varuint(&value[pos..])?;
    let k = k as usize;
    pos += k_bytes;

    if pos + 4 * k + k > size {
        return Err(err("pjson: row_array schema underrun"));
    }
    let key_slots_pos = pos;
    pos += 4 * k;
    let hash_pos = pos;
    pos += k;

    // Compute keys area size from packed key slots.
    let mut keys_total: usize = 0;
    let mut key_specs: Vec<(usize, usize)> = Vec::with_capacity(k); // (off, klen)
    for j in 0..k {
        let raw = u32::from_le_bytes(
            value[key_slots_pos + 4 * j..key_slots_pos + 4 * j + 4]
                .try_into()
                .unwrap(),
        );
        let off = (raw & 0x00FF_FFFF) as usize;
        let ks_byte = (raw >> 24) as u8;
        // Long-key escape inside the keys area is permitted by the spec
        // but we do not emit it (encoder caps keys < 255). Reject on
        // decode for now — same as the C++ reference implementation.
        if ks_byte == 0xFF {
            return Err(err(
                "pjson: row_array long-key escape in shared schema not supported",
            ));
        }
        let klen = ks_byte as usize;
        key_specs.push((off, klen));
        keys_total = keys_total.max(off + klen);
    }
    let keys_pos = pos;
    pos += keys_total;
    if pos + 2 > size {
        return Err(err("pjson: row_array keys area overruns container"));
    }

    // Verify shared hash bytes match each key.
    for j in 0..k {
        let (off, klen) = key_specs[j];
        let key = &value[keys_pos + off..keys_pos + off + klen];
        if value[hash_pos + j] != key_hash8(key) {
            return Err(err("pjson: row_array shared hash mismatch"));
        }
    }

    // Records body extends from `pos` to (size - 2 - n * recoff_w).
    let records_offsets_pos = match size.checked_sub(2 + n * recoff_w) {
        Some(p) if p >= pos => p,
        _ => return Err(err("pjson: row_array records body overruns")),
    };
    let records_body_start = pos;
    let records_body_size = records_offsets_pos - records_body_start;

    let mut records: Vec<Vec<(&[u8], Value<'_>)>> = Vec::with_capacity(n);
    for i in 0..n {
        let rec_off =
            read_width(value, records_offsets_pos + i * recoff_w, recoff_w_code) as usize;
        let rec_end = if i + 1 < n {
            read_width(value, records_offsets_pos + (i + 1) * recoff_w, recoff_w_code) as usize
        } else {
            records_body_size
        };
        if rec_off > rec_end || rec_end > records_body_size {
            return Err(err("pjson: row_array record offset out of range"));
        }
        let rec_size = rec_end - rec_off;
        let rec_data = &value[records_body_start + rec_off..records_body_start + rec_end];
        let slot_table = rec_size
            .checked_sub(k * slot_w)
            .ok_or_else(|| err("pjson: row_array record body too small"))?;
        let mut entries = Vec::with_capacity(k);
        for j in 0..k {
            let v_off = read_width(rec_data, slot_table + j * slot_w, slot_w_code) as usize;
            let v_end = if j + 1 < k {
                read_width(rec_data, slot_table + (j + 1) * slot_w, slot_w_code) as usize
            } else {
                slot_table
            };
            if v_off > v_end || v_end > slot_table {
                return Err(err("pjson: row_array slot offset out of range"));
            }
            let (off_k, klen) = key_specs[j];
            let key = &value[keys_pos + off_k..keys_pos + off_k + klen];
            let v_size = v_end - v_off;
            let child = parse_value(&rec_data[v_off..v_off + v_size], v_size)?;
            entries.push((key, child));
        }
        records.push(entries);
    }
    Ok(Value::RowArray(records))
}

// ── Validator ───────────────────────────────────────────────────────────────

fn validate_value(buf: &[u8], size: usize) -> PjsonResult<()> {
    // Re-parse-and-discard. The cost is the same; the validator becomes
    // a thin wrapper. If a faster validator is needed later, this can
    // walk without materializing — for now correctness >> speed.
    validate_value_depth(buf, size, 0)
}

/// Depth-tracked validator. Caps recursion at `K_MAX_VALIDATION_DEPTH`
/// container levels. Containers (Array, Object, RowArray,
/// TypedArray) increment the depth; leaf scalars do not.
///
/// Note: this implementation parses the buffer first (via
/// `parse_value`) and then walks the resulting `Value` tree with
/// the depth counter. The parse step itself is currently not
/// depth-bounded — a malicious buffer with > C_STACK levels of
/// nesting would blow the stack inside `parse_value` before the
/// validator's depth check fires. A true byte-walker that doesn't
/// materialize is the upstream fix; for now the cap catches mid-
/// range adversarial inputs (anything between
/// `K_MAX_VALIDATION_DEPTH` and the C-stack limit).
fn validate_value_depth(
    buf: &[u8],
    size: usize,
    depth: usize,
) -> PjsonResult<()> {
    if depth > K_MAX_VALIDATION_DEPTH {
        return Err(err("pjson: max validation depth exceeded"));
    }
    let v = parse_value(buf, size)?;
    match v {
        Value::Array(children) => {
            for c in children {
                validate_value_recurse(&c, depth + 1)?;
            }
        }
        Value::Object(entries) => {
            for (_k, child) in entries {
                validate_value_recurse(&child, depth + 1)?;
            }
        }
        Value::RowArray(records) => {
            for rec in records {
                for (_k, child) in rec {
                    validate_value_recurse(&child, depth + 1)?;
                }
            }
        }
        // Leaf values: no container recursion.
        _ => {}
    }
    Ok(())
}

/// Sub-validator on already-parsed children. Re-checks depth against
/// the cap; for container children, recurses one level deeper.
fn validate_value_recurse(v: &Value<'_>, depth: usize) -> PjsonResult<()> {
    if depth > K_MAX_VALIDATION_DEPTH {
        return Err(err("pjson: max validation depth exceeded"));
    }
    match v {
        Value::Array(children) => {
            for c in children {
                validate_value_recurse(c, depth + 1)?;
            }
        }
        Value::Object(entries) => {
            for (_k, child) in entries {
                validate_value_recurse(child, depth + 1)?;
            }
        }
        Value::RowArray(records) => {
            for rec in records {
                for (_k, child) in rec {
                    validate_value_recurse(child, depth + 1)?;
                }
            }
        }
        _ => {}
    }
    Ok(())
}

// ── Encoder primitives ──────────────────────────────────────────────────────

/// Single-pass forward-write encoder for `Value`. Returns total bytes
/// appended to `out`.
pub fn encode(value: &Value<'_>, out: &mut Vec<u8>) -> usize {
    let pos = out.len();
    let n = value_size(value);
    out.resize(pos + n, 0);
    let written = encode_value_at(out, pos, value);
    debug_assert_eq!(written, n, "value_size disagrees with encode_value_at");
    written
}

/// Compute the encoded byte size of a `Value` without writing anything.
pub fn value_size(value: &Value<'_>) -> usize {
    match value {
        Value::Null => 1,
        Value::Bool(_) => 1,
        Value::UInt(v) => uint_size(*v),
        Value::NegInt(mag) => negint_size(*mag),
        Value::Decimal { mantissa, scale } => decimal_size(*mantissa, *scale),
        Value::Float(_) => 9, // binary64 only on encode
        Value::FloatSci(_) => 9,
        Value::Str(b, _) => 1 + b.len(),
        Value::Bytes(b) => 1 + b.len(),
        Value::Array(children) => array_size(children),
        Value::TypedArray { code, count, .. } => {
            1 + typed_array_elem_size(*code) * count + 2
        }
        Value::Object(entries) => object_size(entries),
        Value::RowArray(records) => row_array_size(records),
    }
}

fn uint_size(v: u128) -> usize {
    if v <= 15 {
        1
    } else {
        1 + magnitude_byte_count_u128(v)
    }
}

fn negint_size(mag: u128) -> usize {
    // Caller has already enforced mag != 0; defensively allow.
    1 + magnitude_byte_count_u128(mag.max(1))
}

fn decimal_size(mantissa: i128, scale: i32) -> usize {
    let zz = ((mantissa as u128) << 1) ^ ((mantissa >> 127) as u128);
    let bc = magnitude_byte_count_u128(zz);
    1 + bc + varscale_byte_count(scale)
}

fn array_size(children: &[Value<'_>]) -> usize {
    let mut vd = 0;
    for c in children {
        vd += value_size(c);
    }
    let n = children.len();
    let slot_w_code = width_code_for(vd as u64);
    let slot_w = width_bytes(slot_w_code);
    1 + 1 + vd + slot_w * n + 2
}

fn object_size(entries: &[(&[u8], Value<'_>)]) -> usize {
    let mut vd = 0;
    for (k, v) in entries {
        let kx = if k.len() >= 0xFF {
            varuint_byte_count((k.len() - 0xFF) as u64)
        } else {
            0
        };
        vd += kx + k.len() + value_size(v);
    }
    let n = entries.len();
    let slot_w_code = width_code_for(vd as u64);
    let slot_w = width_bytes(slot_w_code);
    1 + 1 + vd + n + (slot_w + 1) * n + 2
}

fn row_array_size(records: &[Vec<(&[u8], Value<'_>)>]) -> usize {
    let n = records.len();
    if n == 0 {
        // Empty row_array is unusual (the encoder picks generic array
        // for an empty input); we still admit a representation:
        // tag(1) + width(1) + K_varuint + 2 trailing count bytes.
        return 1 + 1 + varuint_byte_count(0) + 2;
    }
    let k = records[0].len();
    // Compute per-record body sizes and max.
    let mut body_sizes: Vec<u32> = Vec::with_capacity(n);
    let mut max_body: u32 = 0;
    for rec in records {
        debug_assert_eq!(rec.len(), k);
        let mut b: u32 = 0;
        for (_, v) in rec {
            b += value_size(v) as u32;
        }
        body_sizes.push(b);
        if b > max_body {
            max_body = b;
        }
    }
    let slot_w_code = width_code_for(max_body as u64);
    let slot_w = width_bytes(slot_w_code);
    let mut running: u32 = 0;
    for &b in &body_sizes {
        running += b + (k as u32) * (slot_w as u32);
    }
    let recoff_w_code = width_code_for(running as u64);
    let recoff_w = width_bytes(recoff_w_code);
    let keys_area: usize = records[0].iter().map(|(k, _)| k.len()).sum();
    1 // tag
    + 1 // width byte
    + varuint_byte_count(k as u64)
    + 4 * k // packed key slots
    + k     // shared hash[k]
    + keys_area
    + running as usize
    + n * recoff_w
    + 2
}

// ── Encoder body — writes into a pre-sized buffer ──────────────────────────

fn encode_value_at(dst: &mut [u8], pos: usize, value: &Value<'_>) -> usize {
    match value {
        Value::Null => {
            dst[pos] = tag::NULL << 4;
            1
        }
        Value::Bool(b) => {
            dst[pos] = (tag::BOOL << 4) | (if *b { 1 } else { 0 });
            1
        }
        Value::UInt(v) => encode_uint_at(dst, pos, *v),
        Value::NegInt(mag) => encode_negint_at(dst, pos, *mag),
        Value::Decimal { mantissa, scale } => encode_decimal_at(dst, pos, *mantissa, *scale),
        Value::Float(f) => encode_f64_at(dst, pos, *f),
        Value::FloatSci(f) => encode_f64_sci_at(dst, pos, *f),
        Value::Str(b, flag) => encode_string_at(dst, pos, b, *flag),
        Value::Bytes(b) => encode_bytes_at(dst, pos, b),
        Value::Array(children) => encode_generic_array_at(dst, pos, children),
        Value::TypedArray {
            code,
            elements,
            count,
        } => encode_typed_array_raw_at(dst, pos, *code, elements, *count),
        Value::Object(entries) => encode_object_at(dst, pos, entries),
        Value::RowArray(records) => encode_row_array_at(dst, pos, records),
    }
}

pub(crate) fn encode_uint_at(dst: &mut [u8], pos: usize, v: u128) -> usize {
    if v <= 15 {
        dst[pos] = (tag::UINT_INLINE << 4) | (v as u8);
        return 1;
    }
    let bc = magnitude_byte_count_u128(v);
    dst[pos] = (tag::UINT << 4) | ((bc - 1) as u8);
    let bytes = v.to_le_bytes();
    dst[pos + 1..pos + 1 + bc].copy_from_slice(&bytes[..bc]);
    1 + bc
}

pub(crate) fn encode_negint_at(dst: &mut [u8], pos: usize, mag: u128) -> usize {
    debug_assert!(mag != 0, "negint with zero magnitude is reserved");
    let bc = magnitude_byte_count_u128(mag);
    dst[pos] = (tag::NEGINT << 4) | ((bc - 1) as u8);
    let bytes = mag.to_le_bytes();
    dst[pos + 1..pos + 1 + bc].copy_from_slice(&bytes[..bc]);
    1 + bc
}

/// Encode a signed integer via uint / uint_inline / negint dispatch.
pub fn encode_int_at(dst: &mut [u8], pos: usize, v: i128) -> usize {
    if v >= 0 {
        encode_uint_at(dst, pos, v as u128)
    } else {
        // Defensive: for v == i128::MIN, -v overflows. Use wrapping
        // negation + cast — the magnitude is exactly 2^127, which is
        // representable as u128.
        let mag = v.unsigned_abs();
        encode_negint_at(dst, pos, mag)
    }
}

fn encode_decimal_at(dst: &mut [u8], pos: usize, mantissa: i128, scale: i32) -> usize {
    let zz = ((mantissa as u128) << 1) ^ ((mantissa >> 127) as u128);
    let bc = magnitude_byte_count_u128(zz);
    dst[pos] = (tag::DECIMAL << 4) | ((bc - 1) as u8);
    let bytes = zz.to_le_bytes();
    dst[pos + 1..pos + 1 + bc].copy_from_slice(&bytes[..bc]);
    let n = write_varscale(dst, pos + 1 + bc, scale);
    1 + bc + n
}

/// Canonical NaN bit pattern at binary64 (§15.2.1) — quiet NaN with zero payload.
pub const CANONICAL_NAN_F64_BITS: u64 = 0x7FF8_0000_0000_0000;

fn canonicalize_f64(f: f64) -> f64 {
    if f.is_nan() {
        f64::from_bits(CANONICAL_NAN_F64_BITS)
    } else {
        f
    }
}

fn encode_f64_at(dst: &mut [u8], pos: usize, f: f64) -> usize {
    let canon = canonicalize_f64(f);
    // binary64, sci=0 → low nibble = 3.
    dst[pos] = (tag::IEEE_FLOAT << 4) | 0b011;
    dst[pos + 1..pos + 9].copy_from_slice(&canon.to_le_bytes());
    9
}

/// Like `encode_f64_at` but with the sci-source flag set per §4.6 /
/// §7.1.  Used by the JSON transcoder when the source token contained
/// `e` or `E`.
fn encode_f64_sci_at(dst: &mut [u8], pos: usize, f: f64) -> usize {
    let canon = canonicalize_f64(f);
    // binary64, sci=1 → low nibble = 0b1011 = 0xB.
    dst[pos] = (tag::IEEE_FLOAT << 4) | 0b1011;
    dst[pos + 1..pos + 9].copy_from_slice(&canon.to_le_bytes());
    9
}

fn encode_string_at(dst: &mut [u8], pos: usize, s: &[u8], flag: u8) -> usize {
    debug_assert!(flag <= 1, "reserved string flag");
    dst[pos] = (tag::STRING << 4) | flag;
    if !s.is_empty() {
        dst[pos + 1..pos + 1 + s.len()].copy_from_slice(s);
    }
    1 + s.len()
}

fn encode_bytes_at(dst: &mut [u8], pos: usize, b: &[u8]) -> usize {
    dst[pos] = tag::BYTES << 4;
    if !b.is_empty() {
        dst[pos + 1..pos + 1 + b.len()].copy_from_slice(b);
    }
    1 + b.len()
}

fn encode_generic_array_at(dst: &mut [u8], pos_in: usize, children: &[Value<'_>]) -> usize {
    let mut pos = pos_in;
    dst[pos] = tag::ARRAY << 4;
    pos += 1;
    // Compute value_data first to pick slot_w.
    let n = children.len();
    let mut vd: usize = 0;
    for c in children {
        vd += value_size(c);
    }
    let slot_w_code = width_code_for(vd as u64);
    let slot_w = width_bytes(slot_w_code);
    dst[pos] = slot_w_code;
    pos += 1;
    let value_data_start = pos;
    let slot_table_pos = value_data_start + vd;
    let count_pos = slot_table_pos + slot_w * n;
    for (i, c) in children.iter().enumerate() {
        let off = (pos - value_data_start) as u32;
        write_width(dst, slot_table_pos + i * slot_w, slot_w_code, off);
        let written = encode_value_at(dst, pos, c);
        pos += written;
    }
    debug_assert_eq!(pos, slot_table_pos);
    dst[count_pos] = (n & 0xFF) as u8;
    dst[count_pos + 1] = ((n >> 8) & 0xFF) as u8;
    count_pos + 2 - pos_in
}

/// Encode a typed_array directly from a raw element-bytes buffer.
/// Caller is responsible for the buffer length matching `count *
/// element_size(code)`.
fn encode_typed_array_raw_at(
    dst: &mut [u8],
    pos: usize,
    code: u8,
    elements: &[u8],
    count: usize,
) -> usize {
    let elem_size = typed_array_elem_size(code);
    debug_assert_eq!(elements.len(), count * elem_size);
    dst[pos] = (tag::ARRAY << 4) | (code + 1);
    if !elements.is_empty() {
        dst[pos + 1..pos + 1 + elements.len()].copy_from_slice(elements);
    }
    let count_pos = pos + 1 + count * elem_size;
    dst[count_pos] = (count & 0xFF) as u8;
    dst[count_pos + 1] = ((count >> 8) & 0xFF) as u8;
    count_pos + 2 - pos
}

/// Public convenience for typed-array writers — given a slice of LE
/// element bytes already laid out, append a typed_array tag of the
/// given code.
pub fn encode_typed_array_bytes(
    code: u8,
    elements: &[u8],
    count: usize,
    out: &mut Vec<u8>,
) -> usize {
    let elem_size = typed_array_elem_size(code);
    assert_eq!(
        elements.len(),
        count * elem_size,
        "typed_array byte slice length mismatch"
    );
    let total = 1 + elements.len() + 2;
    let pos = out.len();
    out.resize(pos + total, 0);
    encode_typed_array_raw_at(out, pos, code, elements, count)
}

fn encode_object_at(dst: &mut [u8], pos_in: usize, entries: &[(&[u8], Value<'_>)]) -> usize {
    let mut pos = pos_in;
    dst[pos] = tag::OBJECT << 4;
    pos += 1;
    let n = entries.len();
    // Compute value_data.
    let mut vd: usize = 0;
    for (k, v) in entries {
        let kx = if k.len() >= 0xFF {
            varuint_byte_count((k.len() - 0xFF) as u64)
        } else {
            0
        };
        vd += kx + k.len() + value_size(v);
    }
    let slot_w_code = width_code_for(vd as u64);
    let slot_w = width_bytes(slot_w_code);
    dst[pos] = slot_w_code;
    pos += 1;
    let value_data_start = pos;
    let hash_table_pos = value_data_start + vd;
    let slot_table_pos = hash_table_pos + n;
    let count_pos = slot_table_pos + (slot_w + 1) * n;

    let entry_stride = slot_w + 1;
    for (i, (k, v)) in entries.iter().enumerate() {
        let off = (pos - value_data_start) as u32;
        let ks_byte = if k.len() < 0xFF { k.len() as u8 } else { 0xFF };
        let slot_pos = slot_table_pos + i * entry_stride;
        write_width(dst, slot_pos, slot_w_code, off);
        dst[slot_pos + slot_w] = ks_byte;
        dst[hash_table_pos + i] = key_hash8(k);
        if k.len() >= 0xFF {
            pos += write_varuint(dst, pos, (k.len() - 0xFF) as u64);
        }
        if !k.is_empty() {
            dst[pos..pos + k.len()].copy_from_slice(k);
        }
        pos += k.len();
        let written = encode_value_at(dst, pos, v);
        pos += written;
    }
    debug_assert_eq!(pos, hash_table_pos);
    dst[count_pos] = (n & 0xFF) as u8;
    dst[count_pos + 1] = ((n >> 8) & 0xFF) as u8;
    count_pos + 2 - pos_in
}

fn encode_row_array_at(
    dst: &mut [u8],
    pos_in: usize,
    records: &[Vec<(&[u8], Value<'_>)>],
) -> usize {
    let mut pos = pos_in;
    let n = records.len();
    let k = if n > 0 { records[0].len() } else { 0 };

    // Pass 1: per-record body sizes.
    let mut body_sizes: Vec<u32> = Vec::with_capacity(n);
    let mut per_field_offset: Vec<u32> = Vec::with_capacity(n * k);
    let mut max_body: u32 = 0;
    for rec in records {
        debug_assert_eq!(rec.len(), k);
        let mut off: u32 = 0;
        for (_, v) in rec {
            per_field_offset.push(off);
            off += value_size(v) as u32;
        }
        body_sizes.push(off);
        if off > max_body {
            max_body = off;
        }
    }
    let slot_w_code = width_code_for(max_body as u64);
    let slot_w = width_bytes(slot_w_code);
    let mut running: u32 = 0;
    let mut record_offsets: Vec<u32> = Vec::with_capacity(n);
    for &b in &body_sizes {
        record_offsets.push(running);
        running += b + (k as u32) * (slot_w as u32);
    }
    let recoff_w_code = width_code_for(running as u64);
    let recoff_w = width_bytes(recoff_w_code);

    dst[pos] = (tag::OBJECT << 4) | obj_form::ROW_ARRAY;
    pos += 1;
    dst[pos] = slot_w_code | (recoff_w_code << 2);
    pos += 1;
    pos += write_varuint(dst, pos, k as u64);

    // Shared key slots: 4 × K, packed { off:24, klen:8 }.
    let key_slots_pos = pos;
    pos += 4 * k;
    let hash_pos = pos;
    pos += k;
    let keys_pos = pos;
    let mut key_off_running: u32 = 0;
    if n > 0 {
        for j in 0..k {
            let key = records[0][j].0;
            let ks_byte = if key.len() < 0xFF {
                key.len() as u8
            } else {
                // Long-key in shared schema not supported by encoder.
                // Caller is expected to keep keys < 255 bytes.
                panic!("pjson row_array: key >= 255 bytes not supported");
            };
            let raw = (key_off_running & 0x00FF_FFFF) | ((ks_byte as u32) << 24);
            dst[key_slots_pos + 4 * j..key_slots_pos + 4 * j + 4]
                .copy_from_slice(&raw.to_le_bytes());
            dst[hash_pos + j] = key_hash8(key);
            if !key.is_empty() {
                dst[pos..pos + key.len()].copy_from_slice(key);
            }
            pos += key.len();
            key_off_running += key.len() as u32;
        }
    }
    let _ = keys_pos;

    // Records body.
    let records_body_start = pos;
    for (i, rec) in records.iter().enumerate() {
        let record_start = pos;
        let body = body_sizes[i];
        for (j, (_, v)) in rec.iter().enumerate() {
            let off = per_field_offset[i * k + j];
            debug_assert_eq!((pos - record_start) as u32, off);
            let _ = off;
            let written = encode_value_at(dst, pos, v);
            pos += written;
        }
        let slot_pos = record_start + body as usize;
        debug_assert_eq!(pos, slot_pos);
        for j in 0..k {
            let off = per_field_offset[i * k + j];
            write_width(dst, slot_pos + j * slot_w, slot_w_code, off);
        }
        pos = slot_pos + k * slot_w;
    }
    let _ = records_body_start;

    let record_offsets_pos = pos;
    for i in 0..n {
        write_width(
            dst,
            record_offsets_pos + i * recoff_w,
            recoff_w_code,
            record_offsets[i],
        );
    }
    pos += n * recoff_w;

    dst[pos] = (n & 0xFF) as u8;
    dst[pos + 1] = ((n >> 8) & 0xFF) as u8;
    pos + 2 - pos_in
}

// ── Pjson trait — typed encode / decode for user types ─────────────────────

pub trait Pjson: Sized {
    const PJSON_FIXED_TAG: Option<u8> = None;
    fn pjson_size(&self) -> usize;
    fn pjson_encode_at(&self, dst: &mut [u8], pos: usize) -> usize;
    fn pjson_decode(bytes: &[u8]) -> PjsonResult<Self>;
    fn pjson_validate(bytes: &[u8]) -> PjsonResult<()> {
        Self::pjson_decode(bytes).map(|_| ())
    }
}

/// One-shot encode entry point — allocates a `Vec<u8>` exactly sized
/// for the encoded form.
pub fn to_pjson<T: Pjson>(value: &T) -> Vec<u8> {
    let n = value.pjson_size();
    let mut out = vec![0u8; n];
    let written = value.pjson_encode_at(&mut out, 0);
    debug_assert_eq!(written, n);
    out
}

/// One-shot decode entry point.
pub fn from_pjson<T: Pjson>(bytes: &[u8]) -> PjsonResult<T> {
    T::pjson_decode(bytes)
}

// ── Pjson implementations for primitives ───────────────────────────────────

impl Pjson for () {
    const PJSON_FIXED_TAG: Option<u8> = Some(tag::NULL << 4);
    fn pjson_size(&self) -> usize {
        1
    }
    fn pjson_encode_at(&self, dst: &mut [u8], pos: usize) -> usize {
        dst[pos] = tag::NULL << 4;
        1
    }
    fn pjson_decode(bytes: &[u8]) -> PjsonResult<Self> {
        if bytes.len() != 1 || bytes[0] != tag::NULL << 4 {
            return Err(err("pjson: expected null"));
        }
        Ok(())
    }
}

impl Pjson for bool {
    fn pjson_size(&self) -> usize {
        1
    }
    fn pjson_encode_at(&self, dst: &mut [u8], pos: usize) -> usize {
        dst[pos] = (tag::BOOL << 4) | (if *self { 1 } else { 0 });
        1
    }
    fn pjson_decode(bytes: &[u8]) -> PjsonResult<Self> {
        if bytes.len() != 1 {
            return Err(err("pjson: bool size != 1"));
        }
        let tag = bytes[0];
        if (tag >> 4) != tag::BOOL {
            return Err(err("pjson: expected bool tag"));
        }
        match tag & 0x0F {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err(err("pjson: reserved bool low nibble")),
        }
    }
}

// Unsigned integer impls — uint_inline / uint dispatch.
macro_rules! impl_pjson_uint {
    ($($ty:ty),* $(,)?) => {$(
        impl Pjson for $ty {
            fn pjson_size(&self) -> usize {
                let v = *self as u128;
                uint_size(v)
            }
            fn pjson_encode_at(&self, dst: &mut [u8], pos: usize) -> usize {
                encode_uint_at(dst, pos, *self as u128)
            }
            fn pjson_decode(bytes: &[u8]) -> PjsonResult<Self> {
                let v = parse_value(bytes, bytes.len())?;
                match v {
                    Value::UInt(u) => {
                        if u > <$ty>::MAX as u128 {
                            return Err(err("pjson: uint overflow on decode"));
                        }
                        Ok(u as $ty)
                    }
                    _ => Err(err("pjson: expected uint")),
                }
            }
        }
    )*};
}
impl_pjson_uint!(u8, u16, u32, u64, u128, usize);

// Signed integer impls — uint_inline / uint / negint dispatch.
macro_rules! impl_pjson_int {
    ($($ty:ty),* $(,)?) => {$(
        impl Pjson for $ty {
            fn pjson_size(&self) -> usize {
                let v = *self as i128;
                if v >= 0 {
                    uint_size(v as u128)
                } else {
                    let mag = (v as i128).unsigned_abs();
                    1 + magnitude_byte_count_u128(mag)
                }
            }
            fn pjson_encode_at(&self, dst: &mut [u8], pos: usize) -> usize {
                encode_int_at(dst, pos, *self as i128)
            }
            fn pjson_decode(bytes: &[u8]) -> PjsonResult<Self> {
                let v = parse_value(bytes, bytes.len())?;
                match v {
                    Value::UInt(u) => {
                        if u > <$ty>::MAX as u128 {
                            return Err(err("pjson: int overflow on decode"));
                        }
                        Ok(u as $ty)
                    }
                    Value::NegInt(mag) => {
                        // mag <= i128::MIN.unsigned_abs() etc. — for
                        // narrower targets check fit.
                        if mag > 1u128.wrapping_shl(<$ty>::BITS) {
                            return Err(err("pjson: negint magnitude overflow"));
                        }
                        // Recover signed value via wrapping negate.
                        let signed = (mag as i128).wrapping_neg();
                        if signed < <$ty>::MIN as i128 || signed > <$ty>::MAX as i128 {
                            return Err(err("pjson: int range on decode"));
                        }
                        Ok(signed as $ty)
                    }
                    _ => Err(err("pjson: expected int")),
                }
            }
        }
    )*};
}
impl_pjson_int!(i8, i16, i32, i64, i128, isize);

impl Pjson for f64 {
    fn pjson_size(&self) -> usize {
        9
    }
    fn pjson_encode_at(&self, dst: &mut [u8], pos: usize) -> usize {
        encode_f64_at(dst, pos, *self)
    }
    fn pjson_decode(bytes: &[u8]) -> PjsonResult<Self> {
        let v = parse_value(bytes, bytes.len())?;
        match v {
            Value::Float(f) => Ok(f),
            // Allow integer-typed sources to decode as f64 too — the
            // canonical encoder may have chosen uint / decimal.
            Value::UInt(u) => Ok(u as f64),
            Value::NegInt(mag) => Ok(-(mag as f64)),
            _ => Err(err("pjson: expected float")),
        }
    }
}

impl Pjson for f32 {
    fn pjson_size(&self) -> usize {
        9 // we encode as binary64 always (spec allows producers to choose width)
    }
    fn pjson_encode_at(&self, dst: &mut [u8], pos: usize) -> usize {
        encode_f64_at(dst, pos, *self as f64)
    }
    fn pjson_decode(bytes: &[u8]) -> PjsonResult<Self> {
        let v: f64 = <f64 as Pjson>::pjson_decode(bytes)?;
        Ok(v as f32)
    }
}

impl Pjson for String {
    fn pjson_size(&self) -> usize {
        1 + self.len()
    }
    fn pjson_encode_at(&self, dst: &mut [u8], pos: usize) -> usize {
        encode_string_at(dst, pos, self.as_bytes(), str_flag::RAW_TEXT)
    }
    fn pjson_decode(bytes: &[u8]) -> PjsonResult<Self> {
        let v = parse_value(bytes, bytes.len())?;
        match v {
            Value::Str(b, _flag) => std::str::from_utf8(b)
                .map(|s| s.to_owned())
                .map_err(|_| err("pjson: invalid UTF-8 in string")),
            _ => Err(err("pjson: expected string")),
        }
    }
}

// ── Pjson::Vec<T> — generic array (heterogeneous) view ─────────────────────
//
// For Vec<T> where T is a primitive that maps to a typed_array, we
// encode as typed_array (canonical §15.6). For other element types,
// emit a generic array.

/// Helper: does T have a typed_array element code?
///
/// Implemented for each Rust primitive that has a corresponding pjson
/// `tac_*` element type. Non-typed elements implement the trait with
/// `TAC = None` and the `_typed_elem` codec methods unreachable.
pub trait PjsonTypedArrayElem: Pjson {
    const TAC: Option<u8> = None;
    /// Compile-time element byte size — used by Vec<T> when emitting
    /// typed_array.
    const TYPED_ELEM_SIZE: usize = 0;
    /// Write this value as raw element bytes (LE) into the typed_array
    /// payload at dst[pos..]. Only called when TAC.is_some().
    fn pjson_encode_at_typed_elem(&self, _dst: &mut [u8], _pos: usize) {
        unreachable!("pjson_encode_at_typed_elem invoked on non-typed elem");
    }
    /// Decode a single typed_array element from raw element bytes.
    /// Only called when TAC.is_some().
    fn pjson_decode_typed_elem(_bytes: &[u8]) -> PjsonResult<Self> {
        Err(err("pjson: typed-array decode invoked on non-typed elem"))
    }
}

macro_rules! impl_typed_elem {
    ($($ty:ty => $code:expr),* $(,)?) => {$(
        impl PjsonTypedArrayElem for $ty {
            const TAC: Option<u8> = Some($code);
            const TYPED_ELEM_SIZE: usize = std::mem::size_of::<$ty>();
            fn pjson_encode_at_typed_elem(&self, dst: &mut [u8], pos: usize) {
                let bytes = self.to_le_bytes();
                dst[pos..pos + bytes.len()].copy_from_slice(&bytes);
            }
            fn pjson_decode_typed_elem(bytes: &[u8]) -> PjsonResult<Self> {
                if bytes.len() != std::mem::size_of::<$ty>() {
                    return Err(err("pjson: typed_array elem size mismatch"));
                }
                let mut tmp = [0u8; std::mem::size_of::<$ty>()];
                tmp.copy_from_slice(bytes);
                Ok(<$ty>::from_le_bytes(tmp))
            }
        }
    )*};
}
impl_typed_elem!(
    i8  => tac::I8,
    i16 => tac::I16,
    i32 => tac::I32,
    i64 => tac::I64,
    u8  => tac::U8,
    u16 => tac::U16,
    u32 => tac::U32,
    u64 => tac::U64,
    f32 => tac::F32,
    // f64 — note f64 is encoded canonically as either decimal or
    // ieee_float in scalar contexts. Inside a typed_array we encode as
    // raw 8-byte IEEE bytes (tac::F64), which matches the spec for
    // Vec<f64> sources.
    f64 => tac::F64,
);

// Non-typed primitives fall back on the default trait with TAC = None.
impl PjsonTypedArrayElem for bool {}
impl PjsonTypedArrayElem for u128 {}
impl PjsonTypedArrayElem for i128 {}
impl PjsonTypedArrayElem for usize {}
impl PjsonTypedArrayElem for isize {}
impl PjsonTypedArrayElem for () {}
impl PjsonTypedArrayElem for String {}

impl<T: Pjson + PjsonTypedArrayElem> Pjson for Vec<T> {
    fn pjson_size(&self) -> usize {
        if let Some(_code) = T::TAC {
            // typed_array layout
            1 + self.len() * T::TYPED_ELEM_SIZE + 2
        } else {
            generic_array_size(self)
        }
    }
    fn pjson_encode_at(&self, dst: &mut [u8], pos: usize) -> usize {
        if let Some(code) = T::TAC {
            // typed_array — write tag + raw LE bytes + count.
            let elem_size = T::TYPED_ELEM_SIZE;
            dst[pos] = (tag::ARRAY << 4) | (code + 1);
            let mut p = pos + 1;
            for e in self {
                e.pjson_encode_at_typed_elem(dst, p);
                p += elem_size;
            }
            dst[p] = (self.len() & 0xFF) as u8;
            dst[p + 1] = ((self.len() >> 8) & 0xFF) as u8;
            p + 2 - pos
        } else {
            encode_generic_array_typed(dst, pos, self)
        }
    }
    fn pjson_decode(bytes: &[u8]) -> PjsonResult<Self> {
        let v = parse_value(bytes, bytes.len())?;
        match v {
            Value::TypedArray {
                code,
                elements,
                count,
            } => {
                if Some(code) != T::TAC {
                    return Err(err("pjson: typed_array element code mismatch"));
                }
                let elem_size = typed_array_elem_size(code);
                let mut out = Vec::with_capacity(count);
                for i in 0..count {
                    out.push(T::pjson_decode_typed_elem(
                        &elements[i * elem_size..(i + 1) * elem_size],
                    )?);
                }
                Ok(out)
            }
            Value::Array(children) => {
                let mut out = Vec::with_capacity(children.len());
                let bytes_view = bytes;
                // We need the raw byte spans to invoke T::pjson_decode.
                // Re-walk via the slot table for that.
                decode_generic_array_into::<T>(bytes_view, &mut out)?;
                Ok(out)
            }
            _ => Err(err("pjson: expected array")),
        }
    }
}

// Hook point for typed-array element packing.
//
// Stable Rust forbids specialization, so we make every Pjson element
// type carry its own typed-element codec on the `PjsonTypedArrayElem`
// trait. Non-numeric element types implement the trait with const
// `TAC = None` and the `*_typed_elem` methods unreachable — they
// never run because the Vec<T> encoder routes through the generic
// path when TAC is None.

// To avoid the specialization problem, use a `match T::TAC` plus
// reflection-by-size approach. Replace the blanket above.
fn generic_array_size<T: Pjson>(v: &Vec<T>) -> usize {
    let mut vd = 0usize;
    for e in v {
        vd += e.pjson_size();
    }
    let n = v.len();
    let slot_w_code = width_code_for(vd as u64);
    let slot_w = width_bytes(slot_w_code);
    1 + 1 + vd + slot_w * n + 2
}

fn encode_generic_array_typed<T: Pjson>(dst: &mut [u8], pos_in: usize, v: &Vec<T>) -> usize {
    let mut pos = pos_in;
    dst[pos] = tag::ARRAY << 4;
    pos += 1;
    let n = v.len();
    let mut vd: usize = 0;
    for e in v {
        vd += e.pjson_size();
    }
    let slot_w_code = width_code_for(vd as u64);
    let slot_w = width_bytes(slot_w_code);
    dst[pos] = slot_w_code;
    pos += 1;
    let value_data_start = pos;
    let slot_table_pos = value_data_start + vd;
    let count_pos = slot_table_pos + slot_w * n;
    for (i, e) in v.iter().enumerate() {
        let off = (pos - value_data_start) as u32;
        write_width(dst, slot_table_pos + i * slot_w, slot_w_code, off);
        let written = e.pjson_encode_at(dst, pos);
        pos += written;
    }
    debug_assert_eq!(pos, slot_table_pos);
    dst[count_pos] = (n & 0xFF) as u8;
    dst[count_pos + 1] = ((n >> 8) & 0xFF) as u8;
    count_pos + 2 - pos_in
}

fn decode_generic_array_into<T: Pjson>(value: &[u8], out: &mut Vec<T>) -> PjsonResult<()> {
    let size = value.len();
    if size < 4 || (value[0] >> 4) != tag::ARRAY || (value[0] & 0x0F) != 0 {
        return Err(err("pjson: expected generic array"));
    }
    let n = u16::from_le_bytes([value[size - 2], value[size - 1]]) as usize;
    let width_byte = value[1];
    let slot_w_code = width_byte & 0x03;
    let slot_w = width_bytes(slot_w_code);
    let value_data_start = 2;
    let slot_table_pos = size
        .checked_sub(2 + slot_w * n)
        .ok_or_else(|| err("pjson: array slot_table_pos invalid"))?;
    if slot_table_pos < value_data_start {
        return Err(err("pjson: array slot_table_pos underflow"));
    }
    let value_data_size = slot_table_pos - value_data_start;
    out.reserve(n);
    for i in 0..n {
        let off_i = read_width(value, slot_table_pos + i * slot_w, slot_w_code) as usize;
        let off_next = if i + 1 < n {
            read_width(value, slot_table_pos + (i + 1) * slot_w, slot_w_code) as usize
        } else {
            value_data_size
        };
        if off_i > off_next || off_next > value_data_size {
            return Err(err("pjson: array slot offset out of range"));
        }
        let child = &value[value_data_start + off_i..value_data_start + off_next];
        out.push(T::pjson_decode(child)?);
    }
    Ok(())
}

// ── Pjson for Option<T> — encoded as null-or-T ─────────────────────────────

impl<T: Pjson> Pjson for Option<T> {
    fn pjson_size(&self) -> usize {
        match self {
            None => 1, // null
            Some(v) => v.pjson_size(),
        }
    }
    fn pjson_encode_at(&self, dst: &mut [u8], pos: usize) -> usize {
        match self {
            None => {
                dst[pos] = tag::NULL << 4;
                1
            }
            Some(v) => v.pjson_encode_at(dst, pos),
        }
    }
    fn pjson_decode(bytes: &[u8]) -> PjsonResult<Self> {
        if bytes.len() == 1 && bytes[0] == (tag::NULL << 4) {
            return Ok(None);
        }
        Ok(Some(T::pjson_decode(bytes)?))
    }
}

// ── Format tag + crate-root CPO impls ──────────────────────────────────────
//
// `format::PjsonLegacy` is the format tag for the pre-audit pjson
// wire format implemented by this module. The audited-spec format
// tag `psio::pjson::format::Pjson` does not yet exist — it lands
// when the spec-correct rewrite of the module ships. Until then
// downstream callers wanting the audited spec should reach for the
// conformance driver's reference implementation; downstream callers
// using the legacy pre-audit wire form continue to use this tag.

/// Format tag for the pre-audit pjson wire format. **Not the
/// audited-spec pjson** — see `docs/spec-compliance.md` "Library API
/// status (pjson)" for the spec-rev reconciliation plan.
pub mod format {
    #[derive(Debug, Clone, Copy)]
    pub struct PjsonLegacy;

    impl crate::Format for PjsonLegacy {
        type Error = super::PjsonError;
    }
}

impl<T: Pjson> crate::Encode<format::PjsonLegacy> for T {
    fn encode_into(&self, out: &mut Vec<u8>) -> Result<usize, PjsonError> {
        let n = self.pjson_size();
        let pos = out.len();
        out.resize(pos + n, 0);
        let written = self.pjson_encode_at(out, pos);
        debug_assert_eq!(written, n);
        Ok(written)
    }
}

impl<T: Pjson> crate::Decode<format::PjsonLegacy> for T {
    fn decode(bytes: &[u8]) -> PjsonResult<T> {
        T::pjson_decode(bytes)
    }
}

impl<T: Pjson> crate::Validate<format::PjsonLegacy> for T {
    fn validate<P: crate::ValidationPolicy>(
        bytes: &[u8], policy: &P,
    ) -> PjsonResult<()> {
        // The legacy per-type `pjson_validate` always runs the full
        // pre-audit checks; the canonical / sorted-hint flags don't
        // map to legacy invariants, so they return explicit error.
        if policy.verify_canonical() || policy.verify_sorted_hints() {
            return Err(PjsonError(
                "psio::pjson_legacy: verify_canonical / verify_sorted_hints \
                 not applicable to pre-audit wire form",
            ));
        }
        T::pjson_validate(bytes)
    }
}

// ── Tests ──────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn null_round_trip() {
        let v: () = ();
        let b = to_pjson(&v);
        assert_eq!(b, vec![0x00]);
        assert_eq!(from_pjson::<()>(&b).unwrap(), ());
    }

    #[test]
    fn bool_round_trip() {
        assert_eq!(to_pjson(&true), vec![0x11]);
        assert_eq!(to_pjson(&false), vec![0x10]);
        assert!(from_pjson::<bool>(&[0x11]).unwrap());
        assert!(!from_pjson::<bool>(&[0x10]).unwrap());
    }

    #[test]
    fn uint_inline_round_trip() {
        for v in 0u8..=15 {
            let b = to_pjson(&v);
            assert_eq!(b, vec![0x30 | v]);
            assert_eq!(from_pjson::<u8>(&b).unwrap(), v);
        }
    }

    #[test]
    fn uint_round_trip() {
        // 16 → bc=1, payload 0x10
        let b = to_pjson(&16u32);
        assert_eq!(b, vec![0x40, 0x10]);
        assert_eq!(from_pjson::<u32>(&b).unwrap(), 16);

        // 256 → bc=2
        let b = to_pjson(&256u32);
        assert_eq!(b, vec![0x41, 0x00, 0x01]);
        assert_eq!(from_pjson::<u32>(&b).unwrap(), 256);

        // u64::MAX
        let b = to_pjson(&u64::MAX);
        assert_eq!(b, vec![0x47, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]);
        assert_eq!(from_pjson::<u64>(&b).unwrap(), u64::MAX);

        // u128::MAX
        let b = to_pjson(&u128::MAX);
        let mut want = vec![0x4F];
        want.extend_from_slice(&[0xFF; 16]);
        assert_eq!(b, want);
        assert_eq!(from_pjson::<u128>(&b).unwrap(), u128::MAX);
    }

    #[test]
    fn negint_round_trip() {
        // -1 → bc=1, magnitude = 1
        let b = to_pjson(&-1i32);
        assert_eq!(b, vec![0x70, 0x01]);
        assert_eq!(from_pjson::<i32>(&b).unwrap(), -1);

        // -128 → bc=1, magnitude = 128
        let b = to_pjson(&-128i32);
        assert_eq!(b, vec![0x70, 0x80]);
        assert_eq!(from_pjson::<i32>(&b).unwrap(), -128);

        // -256 → bc=2, magnitude = 256
        let b = to_pjson(&-256i32);
        assert_eq!(b, vec![0x71, 0x00, 0x01]);
        assert_eq!(from_pjson::<i32>(&b).unwrap(), -256);

        // i64::MIN
        let b = to_pjson(&i64::MIN);
        assert_eq!(b[0], 0x77);
        assert_eq!(from_pjson::<i64>(&b).unwrap(), i64::MIN);
    }

    #[test]
    fn negint_zero_payload_rejected() {
        let buf = [0x70, 0x00];
        assert!(parse_value(&buf, 2).is_err());
    }

    #[test]
    fn float_round_trip() {
        let f = 3.141592653589793_f64;
        let b = to_pjson(&f);
        assert_eq!(b.len(), 9);
        assert_eq!(b[0], 0x63); // tag::IEEE_FLOAT << 4 | 0b011
        assert_eq!(from_pjson::<f64>(&b).unwrap(), f);
    }

    #[test]
    fn nan_canonicalization() {
        let f = f64::from_bits(0x7FFF_DEAD_BEEF_0000); // arbitrary NaN
        assert!(f.is_nan());
        let b = to_pjson(&f);
        // The 8 payload bytes should be the canonical pattern.
        assert_eq!(
            &b[1..],
            &CANONICAL_NAN_F64_BITS.to_le_bytes()[..]
        );
    }

    #[test]
    fn string_round_trip() {
        let s = String::from("hello, world!");
        let b = to_pjson(&s);
        assert_eq!(b[0], 0x80); // tag::STRING << 4 | flag=0
        assert_eq!(&b[1..], s.as_bytes());
        assert_eq!(from_pjson::<String>(&b).unwrap(), s);
    }

    #[test]
    fn empty_string_round_trip() {
        let s = String::new();
        let b = to_pjson(&s);
        assert_eq!(b, vec![0x80]);
        assert_eq!(from_pjson::<String>(&b).unwrap(), s);
    }

    #[test]
    fn typed_array_u16_round_trip() {
        let v: Vec<u16> = vec![1, 2, 3, 1000];
        let b = to_pjson(&v);
        // tag = 0xB6 (typed_array, code u16 = 5, low_nibble = 6),
        // 4 × 2 bytes = 8 byte payload, count u16 = 0x0004
        assert_eq!(b[0], 0xB6);
        assert_eq!(b.len(), 1 + 8 + 2);
        let n = u16::from_le_bytes([b[b.len() - 2], b[b.len() - 1]]);
        assert_eq!(n, 4);
        assert_eq!(from_pjson::<Vec<u16>>(&b).unwrap(), v);
    }

    #[test]
    fn typed_array_i32_round_trip() {
        let v: Vec<i32> = vec![-100, 0, 100];
        let b = to_pjson(&v);
        assert_eq!(b[0], 0xB3); // typed_array, code i32 = 2, low_nibble = 3
        assert_eq!(b.len(), 1 + 12 + 2);
        assert_eq!(from_pjson::<Vec<i32>>(&b).unwrap(), v);
    }

    #[test]
    fn typed_array_f64_round_trip() {
        let v: Vec<f64> = vec![1.0, 2.5, -3.14];
        let b = to_pjson(&v);
        assert_eq!(b[0], 0xBA); // typed_array, code f64 = 9, low_nibble = 10
        assert_eq!(from_pjson::<Vec<f64>>(&b).unwrap(), v);
    }

    #[test]
    fn typed_array_empty() {
        let v: Vec<u8> = vec![];
        let b = to_pjson(&v);
        // tag::ARRAY | (tac::U8 + 1) = 0xB5; count = 0
        assert_eq!(b, vec![0xB5, 0x00, 0x00]);
        assert_eq!(from_pjson::<Vec<u8>>(&b).unwrap(), v);
    }

    #[test]
    fn varuint_capacities() {
        // 1-byte: 0..=63
        for v in [0u64, 1, 63] {
            let mut buf = [0u8; 4];
            let n = write_varuint(&mut buf, 0, v);
            assert_eq!(n, 1);
            let (decoded, _) = read_varuint(&buf[..n]).unwrap();
            assert_eq!(decoded, v);
        }
        // 2-byte: 64..=16383
        for v in [64u64, 100, 16383] {
            let mut buf = [0u8; 4];
            let n = write_varuint(&mut buf, 0, v);
            assert_eq!(n, 2);
            let (decoded, _) = read_varuint(&buf[..n]).unwrap();
            assert_eq!(decoded, v);
        }
        // 3-byte: 16384..=4194303
        for v in [16384u64, 100000, 4_194_303] {
            let mut buf = [0u8; 4];
            let n = write_varuint(&mut buf, 0, v);
            assert_eq!(n, 3);
            let (decoded, _) = read_varuint(&buf[..n]).unwrap();
            assert_eq!(decoded, v);
        }
        // 4-byte: 4194304..=2^30-1
        for v in [4_194_304u64, 100_000_000, (1u64 << 30) - 1] {
            let mut buf = [0u8; 4];
            let n = write_varuint(&mut buf, 0, v);
            assert_eq!(n, 4);
            let (decoded, _) = read_varuint(&buf[..n]).unwrap();
            assert_eq!(decoded, v);
        }
    }

    #[test]
    fn varscale_round_trip() {
        for s in [-32i32, -1, 0, 1, 31, 32, -8192, 8191, 1_000_000, -1_000_000] {
            let n = varscale_byte_count(s);
            let mut buf = vec![0u8; n];
            let written = write_varscale(&mut buf, 0, s);
            assert_eq!(written, n);
            let (decoded, n2) = read_varscale(&buf).unwrap();
            assert_eq!(n2, n);
            assert_eq!(decoded, s);
        }
    }

    #[test]
    fn key_hash8_strips_suffix() {
        // The hash of "amount" and "amount.decimal" should match.
        let a = key_hash8(b"amount");
        let b = key_hash8(b"amount.decimal");
        assert_eq!(a, b);
        // Different keys hash differently (with very high probability).
        let c = key_hash8(b"unrelated");
        assert_ne!(a, c);
    }

    #[test]
    fn generic_array_value() {
        // An array of mixed types via the runtime `Value`.
        let v = Value::Array(vec![
            Value::UInt(1),
            Value::Bool(true),
            Value::Str(b"hi", str_flag::RAW_TEXT),
        ]);
        let mut out = vec![];
        encode(&v, &mut out);
        let parsed = decode(&out).unwrap();
        assert_eq!(parsed, v);
    }

    #[test]
    fn object_value_round_trip() {
        let v = Value::Object(vec![
            (b"a".as_slice(), Value::UInt(1)),
            (b"b".as_slice(), Value::Str(b"two", str_flag::RAW_TEXT)),
        ]);
        let mut out = vec![];
        encode(&v, &mut out);
        // Per §5.2 (adaptive width): tag(1) + width(1) + vd(7) +
        // hash[2](2) + slot[2]@(slot_w=1)+key_size = 2*(1+1) = 4 +
        // count(2) = 17 bytes. Appendix B in the spec uses an older
        // 4-byte slot layout and is stale relative to §5.2 — see the
        // open spec issue noted in the implementation summary.
        assert_eq!(out.len(), 17);
        let parsed = decode(&out).unwrap();
        assert_eq!(parsed, v);
    }

    #[test]
    fn long_key_escape() {
        // 256-char key triggers long-key escape.
        let key = vec![b'k'; 256];
        let v = Value::Object(vec![(key.as_slice(), Value::UInt(1))]);
        let mut out = vec![];
        encode(&v, &mut out);
        let parsed = decode(&out).unwrap();
        if let Value::Object(entries) = parsed {
            assert_eq!(entries.len(), 1);
            assert_eq!(entries[0].0, key.as_slice());
            assert_eq!(entries[0].1, Value::UInt(1));
        } else {
            panic!("expected object");
        }
    }

    #[test]
    fn row_array_round_trip() {
        // Three records, each with the same 3 keys.
        let recs = vec![
            vec![
                (b"x".as_slice(), Value::UInt(1)),
                (b"y".as_slice(), Value::UInt(2)),
                (b"z".as_slice(), Value::UInt(3)),
            ],
            vec![
                (b"x".as_slice(), Value::UInt(10)),
                (b"y".as_slice(), Value::UInt(20)),
                (b"z".as_slice(), Value::UInt(30)),
            ],
            vec![
                (b"x".as_slice(), Value::UInt(100)),
                (b"y".as_slice(), Value::UInt(200)),
                (b"z".as_slice(), Value::UInt(300)),
            ],
        ];
        let v = Value::RowArray(recs);
        let mut out = vec![];
        encode(&v, &mut out);
        let parsed = decode(&out).unwrap();
        assert_eq!(parsed, v);
    }

    #[test]
    fn validate_rejects_truncated_buffer() {
        let buf = [0x40]; // uint with bc=1 but no payload
        assert!(validate(&buf).is_err());
    }

    /// End-to-end test of the format-tagged CPO. Calls
    /// `psio::encode::<Pjson, _>` and `psio::validate::<Pjson, _, _>`
    /// through the crate-root entry points to verify the CPO machinery
    /// is wired correctly, and that both call shapes from
    /// `docs/psio-overview.md` §2.4 work.
    /// End-to-end test of the format-tagged CPO machinery. Asserts
    /// encode → decode round-trip and that both validation call shapes
    /// (compile-time ZST + runtime DynamicPolicy) reach the right
    /// trait body.
    ///
    /// **Note:** the per-type `psio::pjson::Pjson` trait this CPO
    /// delegates to is the pre-audit implementation, which uses the
    /// older tag-code layout (`UINT_INLINE=3` etc.). The conformance
    /// driver's spec-correct layout (`UINT_INLINE=2`) doesn't match
    /// because the lib hasn't been brought to the audited spec yet.
    /// The CPO machinery is independent of which spec rev the lib
    /// implements — it just dispatches through the trait. Spec-rev
    /// reconciliation is a separate migration tracked in
    /// `docs/spec-compliance.md` (Library API status).
    #[test]
    fn crate_root_cpo_encodes_validates_pjson() {
        use crate::{DefaultSafe, DynamicPolicy, StrictCanonical};

        // psio::encode::<Pjson, T>(&value) → psio::decode::<Pjson, T>(bytes)
        let bytes = crate::encode::<format::PjsonLegacy, u32>(&5_u32)
            .expect("encode should succeed");
        let v: u32 = crate::decode::<format::PjsonLegacy, u32>(&bytes)
            .expect("decode should succeed");
        assert_eq!(v, 5, "round-trip preserves the value");

        // psio::validate::<Pjson, T, P>(bytes, &policy) — runtime path
        // (caller-built DynamicPolicy).
        let runtime_policy = DynamicPolicy {
            reject_reserved: true,
            verify_slot_invariants: true,
            verify_hash_bytes: true,
            verify_numeric_string: true,
            ..DynamicPolicy::default()
        };
        crate::validate::<format::PjsonLegacy, u32, _>(&bytes, &runtime_policy)
            .expect("runtime policy validates");

        // psio::validate::<Pjson, T, P>(bytes, &policy) — compile-time
        // path (ZST preset; trait calls inline to constants).
        crate::validate::<format::PjsonLegacy, u32, _>(&bytes, &DefaultSafe)
            .expect("DefaultSafe ZST policy validates");

        // STRICT_CANONICAL surfaces the explicit "pending kernel
        // migration" error rather than silently doing partial work.
        let r = crate::validate::<format::PjsonLegacy, u32, _>(&bytes, &StrictCanonical);
        assert!(r.is_err(),
                "StrictCanonical must surface pending-migration error");
    }

    #[test]
    fn validate_accepts_canonical_appendix_b() {
        // {"a":1,"b":"two"} should validate
        let v = Value::Object(vec![
            (b"a".as_slice(), Value::UInt(1)),
            (b"b".as_slice(), Value::Str(b"two", str_flag::RAW_TEXT)),
        ]);
        let mut out = vec![];
        encode(&v, &mut out);
        validate(&out).unwrap();
    }

    #[test]
    fn validate_rejects_excessive_depth() {
        // Build an array nested K_MAX_VALIDATION_DEPTH + 1 deep. The
        // validator MUST refuse it; a plain decode of the same buffer
        // is intentionally not bounded.
        let mut v: Value = Value::Array(vec![]);
        for _ in 0..(K_MAX_VALIDATION_DEPTH + 5) {
            v = Value::Array(vec![v]);
        }
        let mut out = vec![];
        encode(&v, &mut out);
        let err = validate(&out).unwrap_err();
        assert!(
            err.0.contains("max validation depth"),
            "expected depth error, got: {}",
            err.0
        );
    }

    #[test]
    fn validate_accepts_depth_at_cap() {
        // Build an array nested exactly to the cap. Cap is 64, so 64
        // arrays + 1 leaf. Should validate cleanly.
        let mut v: Value = Value::Null;
        for _ in 0..K_MAX_VALIDATION_DEPTH {
            v = Value::Array(vec![v]);
        }
        let mut out = vec![];
        encode(&v, &mut out);
        validate(&out).unwrap();
    }

    #[test]
    fn empty_array_value() {
        let v: Value = Value::Array(vec![]);
        let mut out = vec![];
        encode(&v, &mut out);
        // tag(1) + width(1) + count(2) = 4
        assert_eq!(out.len(), 4);
        let parsed = decode(&out).unwrap();
        assert_eq!(parsed, v);
    }

    #[test]
    fn empty_object_value() {
        let v: Value = Value::Object(vec![]);
        let mut out = vec![];
        encode(&v, &mut out);
        assert_eq!(out.len(), 4);
        let parsed = decode(&out).unwrap();
        assert_eq!(parsed, v);
    }

    #[test]
    fn vec_of_strings_generic_array() {
        let v = vec!["alpha".to_string(), "beta".to_string(), "gamma".to_string()];
        let b = to_pjson(&v);
        // tag = 0xB0 (generic array)
        assert_eq!(b[0], 0xB0);
        let decoded = from_pjson::<Vec<String>>(&b).unwrap();
        assert_eq!(decoded, v);
    }

    #[test]
    fn decimal_round_trip() {
        // 1.23 → mantissa=123, scale=-2
        let v = Value::Decimal {
            mantissa: 123,
            scale: -2,
        };
        let mut out = vec![];
        encode(&v, &mut out);
        let parsed = decode(&out).unwrap();
        assert_eq!(parsed, v);
    }

    #[test]
    fn bytes_round_trip() {
        let v = Value::Bytes(&[0xDE, 0xAD, 0xBE, 0xEF]);
        let mut out = vec![];
        encode(&v, &mut out);
        assert_eq!(out[0], 0xA0);
        let parsed = decode(&out).unwrap();
        assert_eq!(parsed, v);
    }

    #[test]
    fn reject_reserved_tag() {
        // Type code 2 reserved.
        let buf = [0x20];
        assert!(parse_value(&buf, 1).is_err());
        // Type code 9 reserved.
        let buf = [0x90];
        assert!(parse_value(&buf, 1).is_err());
        // Type code 13 reserved.
        let buf = [0xD0];
        assert!(parse_value(&buf, 1).is_err());
    }

    #[test]
    fn reject_reserved_string_flag() {
        // String low nibble 2 reserved.
        let buf = [0x82, b'h', b'i'];
        assert!(parse_value(&buf, 3).is_err());
    }

    #[test]
    fn reject_reserved_bool() {
        // Bool low nibble 2 reserved.
        let buf = [0x12];
        assert!(parse_value(&buf, 1).is_err());
    }

    #[test]
    fn negint_u128_magnitude() {
        // Negative magnitude beyond i64.
        let v = Value::NegInt(u128::MAX); // logical = -(u128::MAX), well-formed neg
        let mut out = vec![];
        encode(&v, &mut out);
        let parsed = decode(&out).unwrap();
        assert_eq!(parsed, v);
    }

    #[test]
    fn long_string_round_trip() {
        // 10 KB string — exercises larger size widths via the
        // surrounding container; here just confirms scalar string
        // encoding has no length cap.
        let s = "x".repeat(10_000);
        let b = to_pjson(&s);
        assert_eq!(b.len(), 10_001);
        assert_eq!(b[0], 0x80);
        assert_eq!(from_pjson::<String>(&b).unwrap(), s);
    }

    #[test]
    fn nested_object_in_array() {
        // {"a":[1,{"b":2},"three"]}
        let v = Value::Object(vec![(
            b"a".as_slice(),
            Value::Array(vec![
                Value::UInt(1),
                Value::Object(vec![(b"b".as_slice(), Value::UInt(2))]),
                Value::Str(b"three", str_flag::RAW_TEXT),
            ]),
        )]);
        let mut out = vec![];
        encode(&v, &mut out);
        let parsed = decode(&out).unwrap();
        assert_eq!(parsed, v);
    }

    #[test]
    fn ieee_float_rejects_reserved_widths() {
        // Width selector 5 (low_nibble bits 2..0 = 5) reserved.
        let buf = [0x65, 0x00, 0x00, 0x00, 0x00];
        assert!(parse_value(&buf, 5).is_err());
        // Width 0 reserved.
        let buf = [0x60];
        assert!(parse_value(&buf, 1).is_err());
    }

    #[test]
    fn ieee_float_binary16_decodes_widened() {
        // Width 1 → binary16 (2-byte payload). Decoder widens
        // losslessly to f64 via crate::pjson_legacy_json::f16_bits_to_f64.
        // 1.5 in binary16 = 0x3E00.
        let mut buf = vec![0x61];
        buf.extend_from_slice(&0x3E00u16.to_le_bytes());
        let v = parse_value(&buf, 3).unwrap();
        match v {
            Value::Float(f64v) => assert_eq!(f64v, 1.5),
            other => panic!("expected float, got {:?}", other),
        }

        // ±0 in binary16 = 0x0000 / 0x8000.
        let buf = vec![0x61, 0x00, 0x00];
        let v = parse_value(&buf, 3).unwrap();
        match v {
            Value::Float(f64v) => assert_eq!(f64v, 0.0),
            other => panic!("expected float, got {:?}", other),
        }

        // binary128 is still deferred.
        let buf = vec![0x64; 17];
        assert!(parse_value(&buf, 17).is_err());
    }

    #[test]
    fn ieee_float_binary32_decodes() {
        // Width 2 → binary32 (4-byte payload). Should widen to f64.
        let f: f32 = 1.5;
        let mut buf = vec![0x62];
        buf.extend_from_slice(&f.to_le_bytes());
        let v = parse_value(&buf, 5).unwrap();
        match v {
            Value::Float(f64v) => assert_eq!(f64v, 1.5),
            _ => panic!("expected float"),
        }
    }

    #[test]
    fn ieee_float_sci_hint_decodes_as_floatsci() {
        // Bit 3 set → sci-source hint per §4.6 / §7.1.  The decoder
        // surfaces this via the FloatSci variant so the JSON
        // re-emitter can preserve scientific notation on round-trip.
        let f: f64 = 1.5e10;
        let mut buf = vec![0x6B]; // 6<<4 | 0b1011 = sci=1, width=3 (binary64)
        buf.extend_from_slice(&f.to_le_bytes());
        let v = parse_value(&buf, 9).unwrap();
        match v {
            Value::FloatSci(f64v) => assert_eq!(f64v, f),
            other => panic!("expected FloatSci, got {:?}", other),
        }

        // Without the sci flag, the same payload decodes as Float.
        let mut buf2 = vec![0x63]; // sci=0, width=3
        buf2.extend_from_slice(&f.to_le_bytes());
        let v2 = parse_value(&buf2, 9).unwrap();
        match v2 {
            Value::Float(f64v) => assert_eq!(f64v, f),
            other => panic!("expected Float, got {:?}", other),
        }
    }

    #[test]
    fn typed_array_rejects_reserved_code() {
        // low_nibble = 11 → typed_array code 10 (reserved).
        let buf = [0xBB, 0x00, 0x00];
        assert!(parse_value(&buf, 3).is_err());
    }

    #[test]
    fn object_rejects_reserved_form() {
        // Object form 2 reserved.
        let buf = [0xC2];
        assert!(parse_value(&buf, 1).is_err());
    }

    #[test]
    fn key_hash8_matches_xxh3() {
        // Sanity: key_hash8(k) == low byte of XXH3-64 over k (no suffix).
        let k = b"validator_index";
        let want = (crate::xxh3_64::hash(k) & 0xFF) as u8;
        assert_eq!(key_hash8(k), want);
    }
}
