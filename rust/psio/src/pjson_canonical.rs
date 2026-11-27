//! The canonical form of a pjson value.
//!
//! The counterpart of C++ `psio/pjson_number_model.hpp` and
//! `psio/pjson_canonical.hpp` (docs/pjson-spec.md §15; gofractally/psio#17).
//!
//! Canonical is ONE ENCODING PER VALUE OF A GIVEN TYPE. A typed encoder
//! writes what its type says, with no look at the data: an integer in the
//! integer forms with the fewest bytes, a float as its bits at its declared
//! width, a list of numbers as a typed array of its declared element code, a
//! list of records as a row array, a string as raw UTF-8 text.
//!
//! What has no type is decided from the data, by the canonical pass
//! ([`canonical_of_valid`]): a number is the integer forms, or the smaller
//! of decimal and binary64; a generic array whose elements are all integers
//! of at most 64 bits is a typed array of the narrowest code, one whose
//! elements are all numbers binary64 carries is typed f64 when that is not
//! larger, one whose elements are all objects with the same names is a row
//! array.
//!
//! A float met on that path, and a float written as JSON text, is the
//! shortest decimal that reads back to it at its own width
//! ([`f32_reads_as`], [`f64_reads_as`]): a binary32 holding 0.1 is 0.1.

use crate::pjson::{
    key_hash8, tac, tag, typed_array_elem_size, varscale_byte_count, varuint_byte_count, PjsonError, PjsonResult,
};
use crate::pjson_view::View;

/// A finite number: `(-1)^neg * mag * 10^scale`. Normal form: `mag` is not
/// a multiple of 10, or the number is zero (`mag` 0, `scale` 0, not `neg`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Decimal {
    pub neg: bool,
    pub mag: u128,
    pub scale: i32,
}

/// A pjson number.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Number {
    Finite(Decimal),
    MinusZero,
    Nan,
    PlusInf,
    MinusInf,
}

impl Decimal {
    pub fn normalized(mut self) -> Decimal {
        if self.mag == 0 {
            return Decimal::default();
        }
        while self.mag % 10 == 0 {
            self.mag /= 10;
            self.scale += 1;
        }
        self
    }
    /// `mag * 10^scale` as an integer below 2^128, when it is one.
    pub fn as_integer(&self) -> Option<u128> {
        if self.scale < 0 {
            return None;
        }
        if self.scale > 38 {
            return (self.mag == 0).then_some(0);
        }
        let mut v = self.mag;
        for _ in 0..self.scale {
            v = v.checked_mul(10)?;
        }
        Some(v)
    }
    fn digits(&self) -> u32 {
        let mut n = 1;
        let mut m = self.mag / 10;
        while m != 0 {
            n += 1;
            m /= 10;
        }
        n
    }
    fn text(&self) -> String {
        format!("{}{}e{}", if self.neg { "-" } else { "" }, self.mag, self.scale)
    }
}

/// The shortest decimal that reads back to a finite, non-zero float: its
/// fewest significant digits, times a power of ten (`{:e}`).
fn shortest(text: &str) -> Decimal {
    let (neg, body) = match text.strip_prefix('-') {
        Some(rest) => (true, rest),
        None => (false, text),
    };
    let (digits, exponent) = body.split_once('e').expect("a float in scientific form");
    let exponent: i32 = exponent.parse().expect("an exponent");
    let (whole, fraction) = digits.split_once('.').unwrap_or((digits, ""));
    let mut mag: u128 = 0;
    for c in whole.bytes().chain(fraction.bytes()) {
        mag = mag * 10 + (c - b'0') as u128;
    }
    Decimal { neg, mag, scale: exponent - fraction.len() as i32 }.normalized()
}

/// The number an `f64` stands for.
pub fn f64_reads_as(f: f64) -> Number {
    if f.is_nan() {
        return Number::Nan;
    }
    if f.is_infinite() {
        return if f > 0.0 { Number::PlusInf } else { Number::MinusInf };
    }
    if f == 0.0 {
        return if f.is_sign_negative() { Number::MinusZero } else { Number::Finite(Decimal::default()) };
    }
    // The common case, without a decimal conversion: an integer small
    // enough to be exact and to print as itself.
    if f.abs() < 9_007_199_254_740_992.0 && f == f.trunc() {
        return Number::Finite(Decimal { neg: f < 0.0, mag: f.abs() as u128, scale: 0 }.normalized());
    }
    Number::Finite(shortest(&format!("{f:e}")))
}

/// The number an `f32` stands for.
pub fn f32_reads_as(f: f32) -> Number {
    if f.is_nan() {
        return Number::Nan;
    }
    if f.is_infinite() {
        return if f > 0.0 { Number::PlusInf } else { Number::MinusInf };
    }
    if f == 0.0 {
        return if f.is_sign_negative() { Number::MinusZero } else { Number::Finite(Decimal::default()) };
    }
    if f.abs() < 16_777_216.0 && f == f.trunc() {
        return Number::Finite(Decimal { neg: f < 0.0, mag: f.abs() as u128, scale: 0 }.normalized());
    }
    Number::Finite(shortest(&format!("{f:e}")))
}

fn nearest_f64(d: &Decimal) -> f64 {
    d.text().parse::<f64>().unwrap_or(0.0)
}
/// Does a binary64 frame carry the number: does it read back as exactly it?
pub fn carried_by_f64(v: &Number) -> Option<f64> {
    match v {
        Number::MinusZero => Some(-0.0),
        Number::Nan => Some(f64::NAN),
        Number::PlusInf => Some(f64::INFINITY),
        Number::MinusInf => Some(f64::NEG_INFINITY),
        Number::Finite(d) => {
            if d.mag == 0 {
                return Some(0.0);
            }
            if d.digits() > 17 {
                return None;
            }
            let x = nearest_f64(d);
            (x.is_finite() && x != 0.0 && f64_reads_as(x) == *v).then_some(x)
        }
    }
}

impl Number {
    /// The number as an `f64`: the nearest one.
    pub fn to_f64(&self) -> f64 {
        match self {
            Number::MinusZero => -0.0,
            Number::Nan => f64::NAN,
            Number::PlusInf => f64::INFINITY,
            Number::MinusInf => f64::NEG_INFINITY,
            Number::Finite(d) => {
                if d.mag == 0 {
                    0.0
                } else {
                    nearest_f64(d)
                }
            }
        }
    }
    /// The integer this number is, when it is one below 2^128: `(negative,
    /// magnitude)`.
    pub fn as_integer(&self) -> Option<(bool, u128)> {
        match self {
            Number::Finite(d) => d.as_integer().map(|whole| (d.neg && whole != 0, whole)),
            _ => None,
        }
    }
}

// ── Numbers on the wire ──────────────────────────────────────────────────

fn f16_to_f32(bits: u16) -> f32 {
    crate::pjson_json::f16_bits_to_f64(bits) as f32 // exact: a binary32 holds every binary16
}

/// The number of a float frame's bits (`width_log2`: 1, 2 or 3).
pub fn float_frame_number(width_log2: u8, bits: &[u8]) -> Number {
    match width_log2 {
        3 => f64_reads_as(f64::from_le_bytes(bits[..8].try_into().unwrap())),
        2 => f32_reads_as(f32::from_le_bytes(bits[..4].try_into().unwrap())),
        _ => f32_reads_as(f16_to_f32(u16::from_le_bytes([bits[0], bits[1]]))),
    }
}

/// The number of a scalar numeric frame (a well-formed one).
pub fn frame_number(frame: &[u8]) -> PjsonResult<Number> {
    const NOT: PjsonError = PjsonError("pjson: not a number");
    let (&head, rest) = frame.split_first().ok_or(NOT)?;
    let (t, low) = (head >> 4, head & 0x0F);
    Ok(match t {
        tag::UINT_INLINE => Number::Finite(Decimal { neg: false, mag: low as u128, scale: 0 }.normalized()),
        tag::NINT_INLINE => Number::Finite(Decimal { neg: true, mag: low as u128, scale: 0 }.normalized()),
        tag::UINT | tag::NEGINT => {
            if rest.is_empty() || rest.len() > 16 {
                return Err(NOT);
            }
            let mut tmp = [0u8; 16];
            tmp[..rest.len()].copy_from_slice(rest);
            let mag = u128::from_le_bytes(tmp);
            Number::Finite(Decimal { neg: t == tag::NEGINT && mag != 0, mag, scale: 0 }.normalized())
        }
        tag::DECIMAL => {
            let bc = low as usize + 1;
            if rest.len() <= bc {
                return Err(NOT);
            }
            let mut tmp = [0u8; 16];
            tmp[..bc].copy_from_slice(&rest[..bc]);
            let zz = u128::from_le_bytes(tmp);
            let mantissa = ((zz >> 1) as i128) ^ -((zz & 1) as i128);
            let (scale, _) = crate::pjson::read_varscale(&rest[bc..])?;
            Number::Finite(Decimal { neg: mantissa < 0, mag: mantissa.unsigned_abs(), scale }.normalized())
        }
        tag::IEEE_FLOAT => {
            let width = low & 0x07;
            if !(1..=3).contains(&width) || rest.len() != 1 << width {
                return Err(NOT);
            }
            float_frame_number(width, rest)
        }
        _ => return Err(NOT),
    })
}

fn push_integer(out: &mut Vec<u8>, neg: bool, mag: u128) {
    if mag <= 15 {
        out.push(((if neg && mag != 0 { tag::NINT_INLINE } else { tag::UINT_INLINE }) << 4) | mag as u8);
        return;
    }
    let bc = (128 - mag.leading_zeros() as usize + 7) / 8;
    out.push(((if neg { tag::NEGINT } else { tag::UINT }) << 4) | (bc as u8 - 1));
    out.extend_from_slice(&mag.to_le_bytes()[..bc]);
}

/// The encoding of a number with no declared type, appended to `out`: an
/// integer below 2^128 in the integer forms; any other finite number as the
/// smaller of decimal (no trailing zeros) and binary64, where binary64
/// carries it (a tie goes to the float); -0 (and NaN and the infinities,
/// which never arrive without a type) as binary64.
pub fn push_canonical_number(out: &mut Vec<u8>, v: &Number) -> PjsonResult<()> {
    let wide = |out: &mut Vec<u8>, f: f64| {
        out.push((tag::IEEE_FLOAT << 4) | 3);
        out.extend_from_slice(&(if f.is_nan() { f64::from_bits(0x7FF8_0000_0000_0000) } else { f }).to_le_bytes());
    };
    let Number::Finite(d) = v else {
        wide(out, carried_by_f64(v).expect("a special is a binary64"));
        return Ok(());
    };
    if let Some(whole) = d.as_integer() {
        push_integer(out, d.neg, whole);
        return Ok(());
    }
    // decimal: 1 + mantissa bytes + scale bytes (a mantissa of 2^127 or
    // more has no decimal form).
    let mut decimal_size = usize::MAX;
    let mut zigzag = 0u128;
    if d.mag >> 127 == 0 {
        let mantissa = if d.neg { -(d.mag as i128) } else { d.mag as i128 };
        zigzag = ((mantissa << 1) ^ (mantissa >> 127)) as u128;
        let bc = ((128 - zigzag.leading_zeros() as usize + 7) / 8).max(1);
        decimal_size = 1 + bc + varscale_byte_count(d.scale);
    }
    if decimal_size >= 9 {
        if let Some(f) = carried_by_f64(v) {
            wide(out, f);
            return Ok(());
        }
    }
    if decimal_size == usize::MAX {
        return Err(PjsonError("pjson: a number has no canonical form"));
    }
    push_decimal_frame(out, zigzag, d.scale);
    Ok(())
}

fn push_decimal_frame(out: &mut Vec<u8>, zigzag: u128, scale: i32) {
    let bc = ((128 - zigzag.leading_zeros() as usize + 7) / 8).max(1);
    out.push((tag::DECIMAL << 4) | (bc as u8 - 1));
    out.extend_from_slice(&zigzag.to_le_bytes()[..bc]);
    let at = out.len();
    out.resize(at + varscale_byte_count(scale), 0);
    crate::pjson::write_varscale(out, at, scale);
}

// ── Arrays of numbers ────────────────────────────────────────────────────

/// What the elements of an array are, as far as its form goes.
#[derive(Default)]
struct Run {
    not_integers: bool,
    any_negative: bool,
    max_positive: u64,
    max_negative: u64,
}
impl Run {
    fn add(&mut self, v: &Number) {
        match v.as_integer() {
            Some((neg, mag)) if mag >> 64 == 0 && !(neg && mag > 1 << 63) => {
                let m = mag as u64;
                if neg {
                    self.any_negative = true;
                    self.max_negative = self.max_negative.max(m);
                } else {
                    self.max_positive = self.max_positive.max(m);
                }
            }
            _ => self.not_integers = true,
        }
    }
    /// The typed-array code of an all-integer array.
    fn integer_code(&self) -> Option<u8> {
        if self.not_integers {
            return None;
        }
        if !self.any_negative {
            return Some(match self.max_positive {
                0..=0xFF => tac::U8,
                0x100..=0xFFFF => tac::U16,
                0x1_0000..=0xFFFF_FFFF => tac::U32,
                _ => tac::U64,
            });
        }
        let fits = |bits: u32| self.max_positive <= (1u64 << (bits - 1)) - 1 && self.max_negative <= 1u64 << (bits - 1);
        if fits(8) {
            Some(tac::I8)
        } else if fits(16) {
            Some(tac::I16)
        } else if fits(32) {
            Some(tac::I32)
        } else if self.max_positive <= i64::MAX as u64 {
            Some(tac::I64)
        } else {
            None
        }
    }
}

fn push_count(out: &mut Vec<u8>, n: usize) {
    out.extend_from_slice(&(n as u16).to_le_bytes());
}
fn push_width(out: &mut Vec<u8>, code: u8, v: u32) {
    out.extend_from_slice(&v.to_le_bytes()[..code as usize + 1]);
}
fn width_code_for(v: usize) -> u8 {
    match v {
        0..=0xFF => 0,
        0x100..=0xFFFF => 1,
        0x1_0000..=0xFF_FFFF => 2,
        _ => 3,
    }
}
const EMPTY_ARRAY: [u8; 4] = [tag::ARRAY << 4, 0, 0, 0];

/// The array of numbers (at least one) that have no declared element type:
/// every element an integer of at most 64 bits -> a typed array of the
/// narrowest code; else binary64 as below; else `false` (the caller writes
/// a generic array).
pub fn push_number_array(out: &mut Vec<u8>, numbers: &[Number]) -> bool {
    let n = numbers.len();
    let mut run = Run::default();
    for v in numbers {
        run.add(v);
        if run.not_integers {
            break;
        }
    }
    if let Some(code) = run.integer_code() {
        let w = typed_array_elem_size(code);
        out.push((tag::ARRAY << 4) | (code + 1));
        for v in numbers {
            let (neg, mag) = v.as_integer().unwrap();
            let x = if neg { (mag as u64).wrapping_neg() } else { mag as u64 };
            out.extend_from_slice(&x.to_le_bytes()[..w]);
        }
        push_count(out, n);
        return true;
    }
    // Else binary64, when it carries every element and the typed array is
    // not larger than the generic one would be.
    let wide: Option<Vec<f64>> = numbers.iter().map(carried_by_f64).collect();
    let Some(floats) = wide else { return false };
    let mut scratch = Vec::new();
    for v in numbers {
        if push_canonical_number(&mut scratch, v).is_err() {
            return false;
        }
    }
    let generic = 2 + scratch.len() + n * (width_code_for(scratch.len()) as usize + 1) + 2;
    if 1 + n * 8 + 2 > generic {
        return false;
    }
    out.push((tag::ARRAY << 4) | (tac::F64 + 1));
    for f in floats {
        out.extend_from_slice(&(if f.is_nan() { f64::from_bits(0x7FF8_0000_0000_0000) } else { f }).to_le_bytes());
    }
    push_count(out, n);
    true
}

/// The number of one element of a typed array.
pub fn typed_element_number(code: u8, bytes: &[u8]) -> Number {
    match code {
        tac::F32 => f32_reads_as(f32::from_le_bytes(bytes[..4].try_into().unwrap())),
        tac::F64 => f64_reads_as(f64::from_le_bytes(bytes[..8].try_into().unwrap())),
        _ => {
            let w = typed_array_elem_size(code);
            let mut tmp = [0u8; 8];
            tmp[..w].copy_from_slice(&bytes[..w]);
            let raw = u64::from_le_bytes(tmp);
            if code <= tac::I64 {
                let shift = 64 - 8 * w as u32;
                let x = ((raw << shift) as i64) >> shift;
                Number::Finite(Decimal { neg: x < 0, mag: x.unsigned_abs() as u128, scale: 0 }.normalized())
            } else {
                Number::Finite(Decimal { neg: false, mag: raw as u128, scale: 0 }.normalized())
            }
        }
    }
}

// ── Text ─────────────────────────────────────────────────────────────────

/// JSON escape form -> the text. `None` when the escapes are malformed or
/// do not name text (a lone surrogate, an unescaped quote or control).
pub fn unescaped(p: &[u8]) -> Option<Vec<u8>> {
    let hex4 = |at: usize| -> Option<u32> {
        let digits = p.get(at..at + 4)?;
        let mut v = 0u32;
        for &c in digits {
            v = (v << 4) | (c as char).to_digit(16)?;
        }
        Some(v)
    };
    let mut out = Vec::with_capacity(p.len());
    let mut i = 0;
    while i < p.len() {
        let c = p[i];
        if c == b'"' || c < 0x20 {
            return None;
        }
        if c != b'\\' {
            out.push(c);
            i += 1;
            continue;
        }
        i += 1;
        let escape = *p.get(i)?;
        i += 1;
        match escape {
            b'"' => out.push(b'"'),
            b'\\' => out.push(b'\\'),
            b'/' => out.push(b'/'),
            b'b' => out.push(0x08),
            b'f' => out.push(0x0C),
            b'n' => out.push(b'\n'),
            b'r' => out.push(b'\r'),
            b't' => out.push(b'\t'),
            b'u' => {
                let mut cp = hex4(i)?;
                i += 4;
                if (0xD800..=0xDBFF).contains(&cp) {
                    if p.get(i) != Some(&b'\\') || p.get(i + 1) != Some(&b'u') {
                        return None;
                    }
                    let low = hex4(i + 2)?;
                    if !(0xDC00..=0xDFFF).contains(&low) {
                        return None;
                    }
                    i += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                let ch = char::from_u32(cp)?;
                let mut buf = [0u8; 4];
                out.extend_from_slice(ch.encode_utf8(&mut buf).as_bytes());
            }
            _ => return None,
        }
    }
    std::str::from_utf8(&out).ok()?;
    Some(out)
}

// ── A well-formed frame -> its canonical frame ───────────────────────────

const NAME: PjsonError = PjsonError("pjson: a member name is not UTF-8: it has no canonical form");

fn is_number(v: &View<'_>) -> bool {
    matches!(v.type_code(), tag::UINT_INLINE | tag::NINT_INLINE | tag::UINT | tag::NEGINT | tag::DECIMAL | tag::IEEE_FLOAT)
}

fn value(v: View<'_>, out: &mut Vec<u8>, anonymous: bool) -> PjsonResult<()> {
    let raw = v.raw();
    match v.type_code() {
        _ if is_number(&v) => number(raw, out, anonymous),
        tag::STRING => {
            out.push(tag::STRING << 4);
            if v.low_nibble() == 1 {
                let text = unescaped(&raw[1..]).ok_or(PjsonError(
                    "pjson: a string is not text (its escapes do not name UTF-8): it has no canonical form",
                ))?;
                out.extend_from_slice(&text);
            } else {
                std::str::from_utf8(&raw[1..])
                    .map_err(|_| PjsonError("pjson: a string is not UTF-8: it has no canonical form"))?;
                out.extend_from_slice(&raw[1..]);
            }
            Ok(())
        }
        tag::ARRAY if v.is_typed_array() => typed_array(v, out, anonymous),
        tag::ARRAY => array(v, out, anonymous),
        tag::OBJECT if v.is_row_array() => rows_of_row_array(v, out, anonymous),
        tag::OBJECT => object(v, out, anonymous),
        _ => {
            out.extend_from_slice(raw); // null, bool, bytes: one form
            Ok(())
        }
    }
}

/// A scalar number. Typed mode keeps its type: an integer in the fewest
/// bytes, a decimal without trailing zeros (an integer when it is one), a
/// float as its bits (the notation hint cleared, a NaN the quiet one).
fn number(raw: &[u8], out: &mut Vec<u8>, anonymous: bool) -> PjsonResult<()> {
    if anonymous {
        return push_canonical_number(out, &frame_number(raw)?);
    }
    if raw[0] >> 4 == tag::IEEE_FLOAT {
        let width = raw[0] & 0x07;
        let mut tmp = [0u8; 8];
        tmp[..raw.len() - 1].copy_from_slice(&raw[1..]);
        let mut bits = u64::from_le_bytes(tmp);
        let (exponent, fraction, quiet) = match width {
            3 => (0x7FF0_0000_0000_0000u64, 0x000F_FFFF_FFFF_FFFFu64, 0x7FF8_0000_0000_0000u64),
            2 => (0x7F80_0000, 0x007F_FFFF, 0x7FC0_0000),
            _ => (0x7C00, 0x03FF, 0x7E00),
        };
        if bits & exponent == exponent && bits & fraction != 0 {
            bits = quiet;
        }
        out.push(raw[0] & !0x08);
        out.extend_from_slice(&bits.to_le_bytes()[..raw.len() - 1]);
        return Ok(());
    }
    let Number::Finite(d) = frame_number(raw)? else { unreachable!("an integer or decimal frame is finite") };
    match d.as_integer() {
        Some(whole) => push_integer(out, d.neg, whole),
        None => {
            let mantissa = if d.neg { -(d.mag as i128) } else { d.mag as i128 };
            push_decimal_frame(out, ((mantissa << 1) ^ (mantissa >> 127)) as u128, d.scale);
        }
    }
    Ok(())
}

/// A generic array of numbers, each with no type.
fn generic_numbers(out: &mut Vec<u8>, numbers: &[Number]) -> PjsonResult<()> {
    out.push(tag::ARRAY << 4);
    let width_at = out.len();
    out.push(0);
    let data = out.len();
    let mut offsets: Vec<u32> = Vec::with_capacity(numbers.len());
    for v in numbers {
        offsets.push((out.len() - data) as u32);
        push_canonical_number(out, v)?;
    }
    let code = width_code_for(out.len() - data);
    out[width_at] = code;
    for offset in offsets {
        push_width(out, code, offset);
    }
    push_count(out, numbers.len());
    Ok(())
}

fn typed_array(v: View<'_>, out: &mut Vec<u8>, anonymous: bool) -> PjsonResult<()> {
    let code = v.typed_array_code()?;
    let w = typed_array_elem_size(code);
    let bytes = v.typed_array_bytes()?;
    if bytes.is_empty() {
        out.extend_from_slice(&EMPTY_ARRAY);
        return Ok(());
    }
    // A typed array has its element type: typed mode keeps it.
    if !anonymous {
        out.extend_from_slice(v.raw());
        return Ok(());
    }
    // Without a type the form is the values': the narrowest integer code,
    // binary64, or a generic array of numbers.
    let numbers: Vec<Number> = bytes.chunks_exact(w).map(|e| typed_element_number(code, e)).collect();
    if !push_number_array(out, &numbers) {
        generic_numbers(out, &numbers)?;
    }
    Ok(())
}

fn push_key_checked(key: &[u8]) -> PjsonResult<()> {
    std::str::from_utf8(key).map(drop).map_err(|_| NAME)
}

fn object(v: View<'_>, out: &mut Vec<u8>, anonymous: bool) -> PjsonResult<()> {
    let n = v.object_len()?;
    out.push(tag::OBJECT << 4);
    let width_at = out.len();
    out.push(0);
    let data = out.len();
    let mut slots: Vec<(u32, u8, u8)> = Vec::with_capacity(n);
    for i in 0..n {
        let (key, field) = v.object_at(i)?;
        push_key_checked(key)?;
        slots.push(((out.len() - data) as u32, key.len().min(0xFF) as u8, key_hash8(key)));
        if key.len() >= 0xFF {
            let excess = (key.len() - 0xFF) as u64;
            let at = out.len();
            out.resize(at + varuint_byte_count(excess), 0);
            crate::pjson::write_varuint_at(out, at, excess);
        }
        out.extend_from_slice(key);
        value(field, out, anonymous)?;
    }
    let code = width_code_for(out.len() - data);
    out[width_at] = code;
    out.extend(slots.iter().map(|s| s.2));
    for (offset, key_size, _) in &slots {
        push_width(out, code, *offset);
        out.push(*key_size);
    }
    push_count(out, n);
    Ok(())
}

/// A row array of the records; `names` are the shared names, `record(i)`
/// gives record i's field values in order.
fn rows<'a>(
    out: &mut Vec<u8>,
    names: &[&[u8]],
    n: usize,
    record: &dyn Fn(usize) -> PjsonResult<Vec<View<'a>>>,
    anonymous: bool,
) -> PjsonResult<()> {
    let k = names.len();
    for name in names {
        push_key_checked(name)?;
    }
    let mut slot_code = 0u8;
    loop {
        let start = out.len();
        out.push((tag::OBJECT << 4) | 1);
        out.push(0);
        let at = out.len();
        out.resize(at + varuint_byte_count(k as u64), 0);
        crate::pjson::write_varuint_at(out, at, k as u64);
        let mut key_at = 0u32;
        for name in names {
            out.extend_from_slice(&(key_at | ((name.len() as u32) << 24)).to_le_bytes());
            key_at += name.len() as u32;
        }
        out.extend(names.iter().map(|name| key_hash8(name)));
        for name in names {
            out.extend_from_slice(name);
        }
        let body = out.len();
        let mut offsets: Vec<u32> = Vec::with_capacity(n);
        let mut refit = false;
        for i in 0..n {
            offsets.push((out.len() - body) as u32);
            let start_of_record = out.len();
            let mut fields: Vec<u32> = Vec::with_capacity(k);
            for field in record(i)? {
                fields.push((out.len() - start_of_record) as u32);
                value(field, out, anonymous)?;
            }
            if width_code_for(out.len() - start_of_record) > slot_code {
                refit = true;
                break;
            }
            for offset in fields {
                push_width(out, slot_code, offset);
            }
        }
        if refit {
            out.truncate(start);
            slot_code += 1;
            continue;
        }
        let record_code = width_code_for(out.len() - body);
        out[start + 1] = slot_code | (record_code << 2);
        for offset in offsets {
            push_width(out, record_code, offset);
        }
        push_count(out, n);
        return Ok(());
    }
}

fn rows_of_row_array(v: View<'_>, out: &mut Vec<u8>, anonymous: bool) -> PjsonResult<()> {
    let table = v.rows()?;
    if table.is_empty() {
        out.extend_from_slice(&EMPTY_ARRAY);
        return Ok(());
    }
    let names: Vec<&[u8]> = (0..table.field_count()).map(|j| table.key(j)).collect::<PjsonResult<_>>()?;
    rows(out, &names, table.len(), &|i| {
        let row = table.get(i)?;
        (0..row.len()).map(|j| row.field(j).map(|f| f.1)).collect()
    }, anonymous)
}

/// The member names of an element, when it is a single object.
fn shape<'a>(e: &View<'a>) -> PjsonResult<Option<Vec<&'a [u8]>>> {
    if e.type_code() != tag::OBJECT || e.is_row_array() {
        return Ok(None);
    }
    let k = e.object_len()?;
    (0..k).map(|j| e.object_at(j).map(|f| f.0)).collect::<PjsonResult<Vec<_>>>().map(Some)
}

fn array(v: View<'_>, out: &mut Vec<u8>, anonymous: bool) -> PjsonResult<()> {
    let n = v.array_len()?;
    if n == 0 {
        out.extend_from_slice(&EMPTY_ARRAY);
        return Ok(());
    }
    let elements: Vec<View<'_>> = (0..n).map(|i| v.array_get(i)).collect::<PjsonResult<_>>()?;
    // All numbers?
    if elements.iter().all(is_number) {
        let numbers: Vec<Number> = elements.iter().map(|e| frame_number(e.raw())).collect::<PjsonResult<_>>()?;
        if push_number_array(out, &numbers) {
            return Ok(());
        }
    }
    // All objects of one shape?
    if let Some(names) = shape(&elements[0])? {
        let mut same = !names.is_empty() && names.iter().all(|name| name.len() < 0xFF);
        for e in &elements[1..] {
            if !same {
                break;
            }
            same = shape(e)?.as_deref() == Some(&names[..]);
        }
        if same {
            return rows(out, &names, n, &|i| {
                let e = &elements[i];
                (0..names.len()).map(|j| e.object_at(j).map(|f| f.1)).collect()
            }, anonymous);
        }
    }
    out.push(tag::ARRAY << 4);
    let width_at = out.len();
    out.push(0);
    let data = out.len();
    let mut offsets: Vec<u32> = Vec::with_capacity(n);
    for e in elements {
        offsets.push((out.len() - data) as u32);
        value(e, out, anonymous)?;
    }
    let code = width_code_for(out.len() - data);
    out[width_at] = code;
    for offset in offsets {
        push_width(out, code, offset);
    }
    push_count(out, n);
    Ok(())
}

/// The canonical pass over a well-formed frame (`pjson::lazy::validate`
/// accepts `bytes`) as a document WITHOUT types: numbers and arrays take the
/// form their values give them. An error when the frame holds what has no
/// canonical form: a string or a member name that is not UTF-8.
pub fn canonical_of_valid(bytes: &[u8]) -> PjsonResult<Vec<u8>> {
    let mut out = Vec::with_capacity(bytes.len());
    value(View::new(bytes), &mut out, true)?;
    Ok(out)
}

/// `bytes` in the same types, each in its one encoding: the frame itself
/// exactly when it is canonical.
pub fn normalized_of_valid(bytes: &[u8]) -> PjsonResult<Vec<u8>> {
    let mut out = Vec::with_capacity(bytes.len());
    value(View::new(bytes), &mut out, false)?;
    Ok(out)
}
