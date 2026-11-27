//! pjson ↔ JSON text transcoder.
//!
//! Counterpart of C++'s `pjson_json::from_json` (JSON → pjson) and
//! `pjson_to_json` (pjson → JSON text).  Implementation reads the text
//! with psio's own JSON cursor (object members stay in document order)
//! and emits / consumes pjson bytes through the canonical
//! encoder/decoder primitives in `pjson.rs`.
//!
//! API (mirrors C++):
//!     fn from_json(text: &str) -> PjsonResult<Vec<u8>>;
//!     fn to_json(buf: &[u8]) -> PjsonResult<String>;
//!
//! The transcoder is tested via round-trip assertions in
//! `tests` below: parse a JSON corpus, encode as pjson, decode back
//! to JSON text, and verify the result re-parses to the same value
//! tree.
//!
//! Sci-source preservation (§4.6, §7.1): when a JSON number contains
//! `e` or `E` in its source representation, the resulting `ieee_float`
//! tag's bit 3 is set (becomes `0x6B` for binary64 instead of `0x63`).
//! `to_json` reads this flag and emits scientific notation iff the
//! flag is set.  The cursor hands over the number's source token, so
//! nothing is asked of `serde_json`: psio enables none of its
//! behaviour-changing features (`arbitrary_precision`, `preserve_order`).

use crate::json::{JsonCursor, JsonKind, TextCursor};
use crate::pjson::{encode, obj_form, str_flag, tag, PjsonError, PjsonResult, Value};

/// JSON text → pjson bytes.
///
/// Reads the text with psio's own JSON cursor, which hands over every
/// number as its source token, so integer precision and the scientific
/// notation hint come from the text itself. Output bytes are
/// wire-equivalent to what the C++ `pjson_json::from_json` produces for
/// the same input (subject to format-detection differences — see
/// `from_json_with` for the controllable level).
pub fn from_json(text: &str) -> PjsonResult<Vec<u8>> {
    from_json_with(text, FromJsonOptions::default())
}

/// `from_json` with options.  Supplied so tests / consumers that
/// don't need sci-source preservation can opt out.
pub fn from_json_with(text: &str, opts: FromJsonOptions) -> PjsonResult<Vec<u8>> {
    const PARSE: PjsonError = PjsonError("pjson_json: parse failed");
    let mut cursor = TextCursor::new(text.as_bytes(), false).map_err(|_| PARSE)?;
    let parsed = JValue::read(&mut cursor).map_err(|_| PARSE)?;
    cursor.finish().map_err(|_| PARSE)?;
    encode_jvalue(&parsed, &opts)
}

/// pjson bytes → JSON text.  The bytes are held against the format first
/// (`pjson::validate`), so what it refuses is an error here too; then one
/// walk appends the text.  A NaN, an infinity or a string that is not
/// UTF-8 has no JSON form and is an error.
pub fn to_json(buf: &[u8]) -> PjsonResult<String> {
    crate::pjson::validate(buf)?;
    let mut out = String::new();
    out.reserve(buf.len() * 2 + 16);
    walk_to_json(buf, &mut out)?;
    Ok(out)
}

/// Options controlling JSON → pjson conversion.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FromJsonOptions {
    /// Preserve scientific notation hint from source per §4.6 / §7.1.
    /// When true, numbers with `e`/`E` in their source token encode
    /// with the sci flag (low_nibble bit 3 set on ieee_float).  When
    /// false, the encoder emits the more compact int / decimal form
    /// regardless.
    pub preserve_sci_notation: bool,
}

impl Default for FromJsonOptions {
    fn default() -> Self {
        Self {
            preserve_sci_notation: true,
        }
    }
}

// ── JSON → pjson (encoder) ───────────────────────────────────────────────

/// A parsed JSON text. A number keeps its source token; an object keeps
/// its members in document order, and a repeated name keeps its first
/// position and its last value.
enum JValue {
    Null,
    Bool(bool),
    Number(String),
    String(String),
    Array(Vec<JValue>),
    Object(Vec<(String, JValue)>),
}

impl JValue {
    fn read(cursor: &mut TextCursor<'_>) -> Result<JValue, crate::json::JsonTextError> {
        Ok(match cursor.kind()? {
            JsonKind::Null => {
                cursor.null()?;
                JValue::Null
            }
            JsonKind::Bool => JValue::Bool(cursor.boolean()?),
            JsonKind::Number => JValue::Number(cursor.number()?.to_string()),
            JsonKind::String => JValue::String(cursor.string()?.0.into_owned()),
            JsonKind::Array => {
                cursor.begin_array()?;
                let mut items = Vec::new();
                while cursor.next_element()? {
                    items.push(JValue::read(cursor)?);
                }
                JValue::Array(items)
            }
            JsonKind::Object => {
                cursor.begin_object()?;
                // Every member, in the order written: an object keeps its
                // encounter order and its duplicate names (docs/pjson-spec.md
                // §5.2, §7.1).
                let mut members: Vec<(String, JValue)> = Vec::new();
                loop {
                    let Some(name) = cursor.next_field()?.map(|name| name.into_owned()) else {
                        break;
                    };
                    let value = JValue::read(cursor)?;
                    members.push((name, value));
                }
                JValue::Object(members)
            }
        })
    }
}

/// Encode a parsed JSON text to pjson, applying the supplied
/// options.  Numbers are dispatched per their source representation:
///
/// * Integer source (no `.`/`e`/`E`) → `Value::UInt` or `Value::NegInt`,
///   emitting uint / uint_inline / negint.
/// * Fractional source with sci marker (`e`/`E`) AND
///   `preserve_sci_notation` → IEEE float with sci flag.
/// * Fractional source without sci marker → IEEE float (binary64).
fn encode_jvalue(v: &JValue, opts: &FromJsonOptions) -> PjsonResult<Vec<u8>> {
    let val = jvalue_to_pjson_value(v, opts)?;
    let mut out = vec![];
    encode(&val, &mut out);
    Ok(out)
}

/// Translate a parsed JSON text to a pjson::Value, recursively; the
/// strings are borrowed from the tree.
fn jvalue_to_pjson_value<'a>(
    v: &'a JValue,
    opts: &FromJsonOptions,
) -> PjsonResult<Value<'a>> {
    Ok(match v {
        JValue::Null => Value::Null,
        JValue::Bool(b) => Value::Bool(*b),
        JValue::Number(n) => json_number_to_pjson(n, opts)?,
        JValue::String(s) => Value::Str(s.as_bytes(), str_flag::RAW_TEXT),
        JValue::Array(items) => {
            let mut children: Vec<Value<'a>> = Vec::with_capacity(items.len());
            for item in items {
                children.push(jvalue_to_pjson_value(item, opts)?);
            }
            Value::Array(children)
        }
        JValue::Object(map) => {
            let mut entries: Vec<(&'a [u8], Value<'a>)> = Vec::with_capacity(map.len());
            for (k, vv) in map.iter() {
                entries.push((k.as_bytes(), jvalue_to_pjson_value(vv, opts)?));
            }
            Value::Object(entries)
        }
    })
}

/// Map a number's source token to a pjson::Value: scan it for `e`/`E`
/// to set the sci flag, and dispatch integer vs. fractional from the
/// digit shape.
/// A JSON number token, exactly: (-1)^neg × mag × 10^scale, with every
/// trailing zero of the digits moved into the scale. A number whose digits
/// do not fit 128 bits has no pjson form.
struct JsonNumber {
    neg: bool,
    mag: u128,
    scale: i64,
}

fn parse_json_number(token: &str) -> PjsonResult<JsonNumber> {
    const WIDE: PjsonError =
        PjsonError("pjson_json: a number with more than 128 bits of significant digits has no pjson form");
    let bytes = token.as_bytes();
    let (neg, rest) = match bytes.first() {
        Some(b'-') => (true, &bytes[1..]),
        Some(b'+') => (false, &bytes[1..]),
        _ => (false, bytes),
    };
    let (mut mag, mut scale, mut zeros, mut seen_dot, mut digits) = (0u128, 0i64, 0i64, false, 0usize);
    let mut exponent = 0i64;
    let mut at = 0;
    while at < rest.len() {
        let c = rest[at];
        at += 1;
        match c {
            b'.' if !seen_dot => seen_dot = true,
            b'e' | b'E' => {
                let text = std::str::from_utf8(&rest[at..]).map_err(|_| PjsonError("pjson_json: bad exponent"))?;
                // (An exponent beyond i64 is beyond any decimal's scale.)
                exponent = text.parse::<i64>().map_err(|_| PjsonError("pjson_json: bad exponent"))?;
                at = rest.len();
            }
            b'0'..=b'9' => {
                digits += 1;
                if seen_dot {
                    scale -= 1;
                }
                if c == b'0' {
                    zeros += 1;
                } else {
                    for _ in 0..zeros {
                        mag = mag.checked_mul(10).ok_or(WIDE)?;
                    }
                    zeros = 0;
                    mag = mag.checked_mul(10).and_then(|m| m.checked_add((c - b'0') as u128)).ok_or(WIDE)?;
                }
            }
            _ => return Err(PjsonError("pjson_json: not a number")),
        }
    }
    if digits == 0 {
        return Err(PjsonError("pjson_json: not a number"));
    }
    let scale = if mag == 0 { 0 } else { scale.saturating_add(zeros).saturating_add(exponent) };
    Ok(JsonNumber { neg, mag, scale })
}

/// A JSON number token → the pjson form that holds its VALUE exactly
/// (docs/pjson-spec.md §7.2, §15.2; gofractally/psio#17):
///
/// * minus zero: the float that has one, at the smallest width;
/// * an integer of up to 128 bits of magnitude: the integer forms;
/// * a number with a fraction or an exponent: binary64 when that holds it
///   exactly (its shortest round-trip decimal is the number), else a
///   decimal;
/// * a number none of these holds exactly is an error — never a rounded
///   double, never infinity.
fn json_number_to_pjson<'a>(
    token: &str,
    opts: &FromJsonOptions,
) -> PjsonResult<Value<'a>> {
    let number = parse_json_number(token)?;
    if number.mag == 0 && number.neg {
        // -0: binary16 negative zero (§15.2.1: the smallest width).
        return Ok(Value::Float { width_log2: 1, bits: 0x8000 });
    }
    let has_dot = token.bytes().any(|b| b == b'.');
    let has_sci = token.bytes().any(|b| b == b'e' || b == b'E');
    // The exact decimal: mantissa × 10^scale, or the error of a number it
    // cannot hold.
    let decimal = |number: &JsonNumber| -> PjsonResult<Value<'a>> {
        if number.mag > i128::MAX as u128 {
            return Err(PjsonError(
                "pjson_json: a number with more than 127 bits of significant digits and an exponent has no pjson form",
            ));
        }
        if number.scale > 536_870_911 || number.scale < -536_870_912 {
            return Err(PjsonError("pjson_json: a number's exponent is outside the range of a decimal"));
        }
        let mantissa = number.mag as i128;
        Ok(Value::Decimal { mantissa: if number.neg { -mantissa } else { mantissa }, scale: number.scale as i32 })
    };
    // An integer literal, or a number whose value is an integer that fits.
    if !has_dot && !has_sci {
        let integer = (0..number.scale).try_fold(number.mag, |m, _| m.checked_mul(10));
        return match integer {
            Some(mag) if number.neg => Ok(Value::NegInt(mag)),
            Some(mag) => Ok(Value::UInt(mag)),
            // More digits than 128 bits, the excess being zeros: exact as a
            // decimal.
            None => decimal(&number),
        };
    }
    // Binary64 only when it is the number.
    if let Ok(f) = token.parse::<f64>() {
        if f.is_finite() {
            let back = parse_json_number(&format!("{f:e}"))?;
            if back.neg == number.neg && back.mag == number.mag && back.scale == number.scale {
                return Ok(if has_sci && opts.preserve_sci_notation {
                    Value::FloatSci(f)
                } else {
                    Value::Float { width_log2: 3, bits: f.to_bits() as u128 }
                });
            }
        }
    }
    decimal(&number)
}

// ── pjson → JSON (decoder/walker) ────────────────────────────────────────

/// Walk pjson bytes and append JSON-text representation to `out`.
fn walk_to_json(buf: &[u8], out: &mut String) -> PjsonResult<()> {
    if buf.is_empty() {
        return Err(PjsonError("pjson_json: empty buffer"));
    }
    let tag_byte = buf[0];
    let high = tag_byte >> 4;
    let low = tag_byte & 0x0F;
    match high {
        tag::NULL => {
            if low != 0 || buf.len() != 1 {
                return Err(PjsonError("pjson_json: invalid null"));
            }
            out.push_str("null");
            Ok(())
        }
        tag::BOOL => {
            if buf.len() != 1 {
                return Err(PjsonError("pjson_json: invalid bool"));
            }
            match low {
                0 => out.push_str("false"),
                1 => out.push_str("true"),
                _ => return Err(PjsonError("pjson_json: reserved bool")),
            }
            Ok(())
        }
        tag::NINT_INLINE => {
            if buf.len() != 1 || low == 0 { return Err(PjsonError("pjson: invalid inline negative integer")); }
            out.push('-');
            out.push_str(&low.to_string());
            Ok(())
        }
        tag::UINT_INLINE => {
            if buf.len() != 1 {
                return Err(PjsonError("pjson_json: invalid uint_inline"));
            }
            // Value 0..15.
            push_u128_decimal(out, low as u128);
            Ok(())
        }
        tag::UINT => {
            let bc = (low as usize) + 1;
            if 1 + bc != buf.len() || bc > 16 {
                return Err(PjsonError("pjson_json: uint size"));
            }
            let mut tmp = [0u8; 16];
            tmp[..bc].copy_from_slice(&buf[1..1 + bc]);
            let mag = u128::from_le_bytes(tmp);
            push_u128_decimal(out, mag);
            Ok(())
        }
        tag::NEGINT => {
            let bc = (low as usize) + 1;
            if 1 + bc != buf.len() || bc > 16 {
                return Err(PjsonError("pjson_json: negint size"));
            }
            let mut tmp = [0u8; 16];
            tmp[..bc].copy_from_slice(&buf[1..1 + bc]);
            let mag = u128::from_le_bytes(tmp);
            if mag == 0 {
                return Err(PjsonError("pjson_json: negint zero reserved"));
            }
            out.push('-');
            push_u128_decimal(out, mag);
            Ok(())
        }
        tag::IEEE_FLOAT => {
            let width_bits = low & 0x07;
            let sci = (low & 0x08) != 0;
            let f = match width_bits {
                1 => {
                    // binary16 — software widen.
                    if 1 + 2 != buf.len() {
                        return Err(PjsonError("pjson_json: f16 size"));
                    }
                    let mut tmp = [0u8; 2];
                    tmp.copy_from_slice(&buf[1..3]);
                    let bits = u16::from_le_bytes(tmp);
                    f16_bits_to_f64(bits)
                }
                2 => {
                    if 1 + 4 != buf.len() {
                        return Err(PjsonError("pjson_json: f32 size"));
                    }
                    let mut tmp = [0u8; 4];
                    tmp.copy_from_slice(&buf[1..5]);
                    f32::from_le_bytes(tmp) as f64
                }
                3 => {
                    if 1 + 8 != buf.len() {
                        return Err(PjsonError("pjson_json: f64 size"));
                    }
                    let mut tmp = [0u8; 8];
                    tmp.copy_from_slice(&buf[1..9]);
                    f64::from_le_bytes(tmp)
                }
                _ => {
                    return Err(PjsonError("pjson_json: ieee_float width"));
                }
            };
            push_f64_text(out, f, sci)?;
            Ok(())
        }
        tag::DECIMAL => {
            // Render via mantissa × 10^scale → f64; not exact, but
            // matches view_to_json/pjson_to_json.
            let bc = (low as usize) + 1;
            if 1 + bc + 1 > buf.len() || bc > 16 {
                return Err(PjsonError("pjson_json: decimal size"));
            }
            let mut tmp = [0u8; 16];
            tmp[..bc].copy_from_slice(&buf[1..1 + bc]);
            let zz = u128::from_le_bytes(tmp);
            let mantissa = ((zz >> 1) as i128) ^ (-((zz & 1) as i128));
            let (scale, _) = read_varuint_signed(&buf[1 + bc..])?;
            push_decimal(out, mantissa, scale as i64);
            Ok(())
        }
        tag::STRING => {
            if low > 1 {
                return Err(PjsonError("pjson_json: reserved string flag"));
            }
            out.push('"');
            let payload = &buf[1..];
            if low == str_flag::RAW_TEXT {
                emit_string_escaping(out, payload)?;
            } else {
                // ESCAPE_FORM — pjson stored the JSON-escape form
                // verbatim; emit unchanged.
                out.push_str(
                    std::str::from_utf8(payload)
                        .map_err(|_| PjsonError("pjson_json: bad utf8"))?,
                );
            }
            out.push('"');
            Ok(())
        }
        tag::BYTES => {
            // Emit as quoted hex string (matches C++ pjson_to_json).
            if low != 0 {
                return Err(PjsonError("pjson_json: reserved bytes nibble"));
            }
            out.push('"');
            push_base64(out, &buf[1..]);
            out.push('"');
            Ok(())
        }
        tag::ARRAY => {
            if low == 0 {
                walk_generic_array_to_json(buf, out)
            } else if (1..=10).contains(&low) {
                walk_typed_array_to_json(buf, low - 1, out)
            } else {
                Err(PjsonError("pjson_json: reserved array form"))
            }
        }
        tag::OBJECT => match low {
            obj_form::SINGLE => walk_object_to_json(buf, out),
            obj_form::ROW_ARRAY => walk_row_array_to_json(buf, out),
            _ => Err(PjsonError("pjson_json: reserved object form")),
        },
        _ => Err(PjsonError("pjson_json: unknown type code")),
    }
}

// ── Container walkers ────────────────────────────────────────────────────

fn walk_generic_array_to_json(buf: &[u8], out: &mut String) -> PjsonResult<()> {
    let size = buf.len();
    if size < 4 {
        return Err(PjsonError("pjson_json: array too small"));
    }
    let n =
        u16::from_le_bytes([buf[size - 2], buf[size - 1]]) as usize;
    let width_byte = buf[1];
    let slot_w = ((width_byte & 0x03) as usize) + 1;
    let value_data_start = 2;
    let slot_table_pos = size
        .checked_sub(2 + slot_w * n)
        .ok_or(PjsonError("pjson_json: array slot overflow"))?;
    let value_data_size = slot_table_pos - value_data_start;

    out.push('[');
    for i in 0..n {
        if i > 0 {
            out.push(',');
        }
        let off_i = read_width_at(buf, slot_table_pos + i * slot_w, slot_w);
        let off_next = if i + 1 < n {
            read_width_at(buf, slot_table_pos + (i + 1) * slot_w, slot_w)
        } else {
            value_data_size
        };
        if off_i > off_next || off_next > value_data_size {
            return Err(PjsonError("pjson_json: array offset"));
        }
        let child = &buf[value_data_start + off_i..value_data_start + off_next];
        walk_to_json(child, out)?;
    }
    out.push(']');
    Ok(())
}

fn walk_typed_array_to_json(buf: &[u8], code: u8, out: &mut String) -> PjsonResult<()> {
    use crate::pjson::{tac, typed_array_elem_size};
    let size = buf.len();
    if size < 3 {
        return Err(PjsonError("pjson_json: typed_array too small"));
    }
    let n =
        u16::from_le_bytes([buf[size - 2], buf[size - 1]]) as usize;
    let elem_size = typed_array_elem_size(code);
    if elem_size == 0 || 1 + n * elem_size + 2 != size {
        return Err(PjsonError("pjson_json: typed_array size"));
    }
    out.push('[');
    let elements = &buf[1..1 + n * elem_size];
    for i in 0..n {
        if i > 0 {
            out.push(',');
        }
        let chunk = &elements[i * elem_size..(i + 1) * elem_size];
        match code {
            tac::I8 => push_i64_decimal(out, chunk[0] as i8 as i64),
            tac::U8 => push_u128_decimal(out, chunk[0] as u128),
            tac::I16 => {
                let v = i16::from_le_bytes([chunk[0], chunk[1]]);
                push_i64_decimal(out, v as i64);
            }
            tac::U16 => {
                let v = u16::from_le_bytes([chunk[0], chunk[1]]);
                push_u128_decimal(out, v as u128);
            }
            tac::I32 => {
                let mut t = [0u8; 4];
                t.copy_from_slice(chunk);
                push_i64_decimal(out, i32::from_le_bytes(t) as i64);
            }
            tac::U32 => {
                let mut t = [0u8; 4];
                t.copy_from_slice(chunk);
                push_u128_decimal(out, u32::from_le_bytes(t) as u128);
            }
            tac::I64 => {
                let mut t = [0u8; 8];
                t.copy_from_slice(chunk);
                push_i64_decimal(out, i64::from_le_bytes(t));
            }
            tac::U64 => {
                let mut t = [0u8; 8];
                t.copy_from_slice(chunk);
                push_u128_decimal(out, u64::from_le_bytes(t) as u128);
            }
            tac::F32 => {
                let mut t = [0u8; 4];
                t.copy_from_slice(chunk);
                push_f64_text(out, f32::from_le_bytes(t) as f64, false)?;
            }
            tac::F64 => {
                let mut t = [0u8; 8];
                t.copy_from_slice(chunk);
                push_f64_text(out, f64::from_le_bytes(t), false)?;
            }
            _ => return Err(PjsonError("pjson_json: typed_array code")),
        }
    }
    out.push(']');
    Ok(())
}

fn walk_object_to_json(buf: &[u8], out: &mut String) -> PjsonResult<()> {
    let size = buf.len();
    if size < 4 {
        return Err(PjsonError("pjson_json: object too small"));
    }
    let n =
        u16::from_le_bytes([buf[size - 2], buf[size - 1]]) as usize;
    let width_byte = buf[1];
    let slot_w = ((width_byte & 0x03) as usize) + 1;
    let entry_stride = slot_w + 1;
    let value_data_start = 2;
    let slot_table_pos = size
        .checked_sub(2 + entry_stride * n)
        .ok_or(PjsonError("pjson_json: object slot overflow"))?;
    let hash_table_pos = slot_table_pos
        .checked_sub(n)
        .ok_or(PjsonError("pjson_json: object hash table"))?;
    let value_data_size = hash_table_pos - value_data_start;

    out.push('{');
    for i in 0..n {
        if i > 0 {
            out.push(',');
        }
        let slot = slot_table_pos + i * entry_stride;
        let off_i = read_width_at(buf, slot, slot_w);
        let key_size_byte = buf[slot + slot_w];
        let off_next = if i + 1 < n {
            read_width_at(buf, slot_table_pos + (i + 1) * entry_stride, slot_w)
        } else {
            value_data_size
        };
        if off_i > off_next || off_next > value_data_size {
            return Err(PjsonError("pjson_json: object offset"));
        }
        let entry = &buf[value_data_start + off_i..value_data_start + off_next];
        let entry_size = entry.len();
        let (klen, klen_bytes) = if key_size_byte != 0xFF {
            (key_size_byte as usize, 0)
        } else {
            let (excess, nb) = crate::pjson::peek_varuint(entry)?;
            (0xFF + excess as usize, nb)
        };
        if klen_bytes + klen > entry_size {
            return Err(PjsonError("pjson_json: object key range"));
        }
        let key = &entry[klen_bytes..klen_bytes + klen];
        out.push('"');
        emit_string_escaping(out, key)?;
        out.push('"');
        out.push(':');
        let value_buf = &entry[klen_bytes + klen..];
        walk_to_json(value_buf, out)?;
    }
    out.push('}');
    Ok(())
}

fn walk_row_array_to_json(buf: &[u8], out: &mut String) -> PjsonResult<()> {
    // For brevity, route through full decode → re-emit for row_array,
    // since row_array is the schema-shared shape and the walker is
    // 100+ lines.  Round-trip correctness is preserved via the
    // canonical Value-tree path.
    let v = crate::pjson::decode(buf)?;
    write_value_as_json(&v, out)
}

fn write_value_as_json<'a>(v: &Value<'a>, out: &mut String) -> PjsonResult<()> {
    match v {
        Value::Null => {
            out.push_str("null");
            Ok(())
        }
        Value::Bool(b) => {
            out.push_str(if *b { "true" } else { "false" });
            Ok(())
        }
        Value::UInt(u) => {
            push_u128_decimal(out, *u);
            Ok(())
        }
        Value::NegInt(mag) => {
            out.push('-');
            push_u128_decimal(out, *mag);
            Ok(())
        }
        Value::Decimal { mantissa, scale } => {
            push_decimal(out, *mantissa, *scale as i64);
            Ok(())
        }
        Value::Float { width_log2, bits } => {
            let f = match *width_log2 {
                1 => f16_bits_to_f64(*bits as u16),
                2 => f32::from_bits(*bits as u32) as f64,
                3 => f64::from_bits(*bits as u64),
                4 => {
                    // binary128 → f64 widening for JSON display is
                    // lossy. Emit the full hex via numeric_string
                    // wrap rather than truncate silently.
                    return Err(PjsonError(
                        "pjson_json: f128 → JSON not yet supported"));
                }
                _ => return Err(PjsonError("pjson_json: float width")),
            };
            push_f64_text(out, f, false)?;
            Ok(())
        }
        Value::FloatSci(f) => {
            push_f64_text(out, *f, true)?;
            Ok(())
        }
        Value::Str(b, fl) => {
            out.push('"');
            if *fl == str_flag::ESCAPE_FORM {
                out.push_str(
                    std::str::from_utf8(b)
                        .map_err(|_| PjsonError("pjson_json: bad utf8"))?,
                );
            } else {
                emit_string_escaping(out, b)?;
            }
            out.push('"');
            Ok(())
        }
        Value::Bytes(b, hint) => {
            // §4.10 hint 0 (the only one the codecs accept): standard
            // base64, padded.
            let _ = hint;
            out.push('"');
            push_base64(out, b);
            out.push('"');
            Ok(())
        }
        Value::Array(items) => {
            out.push('[');
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write_value_as_json(item, out)?;
            }
            out.push(']');
            Ok(())
        }
        Value::TypedArray { code, elements, count } => {
            // Re-route through the same walker by reconstructing a
            // tiny wrapper buffer.  Easiest: format the elements
            // directly per code.
            let n = *count;
            out.push('[');
            for i in 0..n {
                if i > 0 {
                    out.push(',');
                }
                let elem_size = crate::pjson::typed_array_elem_size(*code);
                let chunk = &elements[i * elem_size..(i + 1) * elem_size];
                use crate::pjson::tac;
                match *code {
                    tac::I8 => push_i64_decimal(out, chunk[0] as i8 as i64),
                    tac::U8 => push_u128_decimal(out, chunk[0] as u128),
                    tac::I16 => push_i64_decimal(
                        out,
                        i16::from_le_bytes([chunk[0], chunk[1]]) as i64,
                    ),
                    tac::U16 => push_u128_decimal(
                        out,
                        u16::from_le_bytes([chunk[0], chunk[1]]) as u128,
                    ),
                    tac::I32 => {
                        let mut t = [0u8; 4];
                        t.copy_from_slice(chunk);
                        push_i64_decimal(out, i32::from_le_bytes(t) as i64);
                    }
                    tac::U32 => {
                        let mut t = [0u8; 4];
                        t.copy_from_slice(chunk);
                        push_u128_decimal(out, u32::from_le_bytes(t) as u128);
                    }
                    tac::I64 => {
                        let mut t = [0u8; 8];
                        t.copy_from_slice(chunk);
                        push_i64_decimal(out, i64::from_le_bytes(t));
                    }
                    tac::U64 => {
                        let mut t = [0u8; 8];
                        t.copy_from_slice(chunk);
                        push_u128_decimal(out, u64::from_le_bytes(t) as u128);
                    }
                    tac::F32 => {
                        let mut t = [0u8; 4];
                        t.copy_from_slice(chunk);
                        push_f64_text(out, f32::from_le_bytes(t) as f64, false)?;
                    }
                    tac::F64 => {
                        let mut t = [0u8; 8];
                        t.copy_from_slice(chunk);
                        push_f64_text(out, f64::from_le_bytes(t), false)?;
                    }
                    _ => return Err(PjsonError("pjson_json: typed_array code")),
                }
            }
            out.push(']');
            Ok(())
        }
        Value::Object(entries) => {
            out.push('{');
            for (i, (k, v)) in entries.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                out.push('"');
                emit_string_escaping(out, k)?;
                out.push('"');
                out.push(':');
                write_value_as_json(v, out)?;
            }
            out.push('}');
            Ok(())
        }
        Value::EmptyRowArray(_) => { out.push_str("[]"); Ok(()) }
        Value::RowArray(records) => {
            out.push('[');
            for (i, rec) in records.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                out.push('{');
                for (j, (k, v)) in rec.iter().enumerate() {
                    if j > 0 {
                        out.push(',');
                    }
                    out.push('"');
                    emit_string_escaping(out, k)?;
                    out.push('"');
                    out.push(':');
                    write_value_as_json(v, out)?;
                }
                out.push('}');
            }
            out.push(']');
            Ok(())
        }
    }
}

// ── Helpers ──────────────────────────────────────────────────────────────

#[inline]
fn read_width_at(buf: &[u8], pos: usize, n: usize) -> usize {
    let mut tmp = [0u8; 4];
    tmp[..n].copy_from_slice(&buf[pos..pos + n]);
    u32::from_le_bytes(tmp) as usize
}

fn read_varuint_signed(buf: &[u8]) -> PjsonResult<(i32, usize)> {
    let (zz, n) = crate::pjson::peek_varuint(buf)?;
    let scale = ((zz >> 1) as i32) ^ (-((zz & 1) as i32));
    Ok((scale, n))
}

fn push_u128_decimal(out: &mut String, mut v: u128) {
    if v == 0 {
        out.push('0');
        return;
    }
    // Up to 39 base-10 digits in u128.
    let mut buf = [0u8; 40];
    let mut i = buf.len();
    while v > 0 {
        i -= 1;
        buf[i] = b'0' + (v % 10) as u8;
        v /= 10;
    }
    out.push_str(std::str::from_utf8(&buf[i..]).unwrap());
}

fn push_i64_decimal(out: &mut String, v: i64) {
    if v < 0 {
        out.push('-');
        // Use wrapping_neg so i64::MIN works correctly via u64.
        push_u128_decimal(out, v.unsigned_abs() as u128);
    } else {
        push_u128_decimal(out, v as u128);
    }
}

/// Emit an f64 in JSON-compatible text. If `sci` is true, force
/// scientific notation regardless of magnitude.
fn push_f64_text(out: &mut String, f: f64, sci: bool) -> PjsonResult<()> {
    // §7.3: JSON has no NaN or infinity. An error, never `null` in their
    // place.
    if !f.is_finite() {
        return Err(PjsonError("pjson_json: NaN and infinity have no JSON form"));
    }
    if f == 0.0 && f.is_sign_negative() {
        out.push_str("-0");
        return Ok(());
    }
    if sci {
        // Force scientific notation via the `{:e}` formatter then
        // tidy up the result so it's valid JSON (Rust's `{:e}` emits
        // forms like `1.5e10` which JSON accepts).
        let formatted = format!("{:e}", f);
        out.push_str(&formatted);
    } else {
        // Default emission — use Rust's Display, which avoids
        // scientific notation for moderate values.
        let s = if f.fract() == 0.0 && f.abs() < 1e16 {
            format!("{}", f as i64)
        } else {
            // Use Rust's `Display` which is shortest round-trippable.
            format!("{}", f)
        };
        out.push_str(&s);
    }
    Ok(())
}

/// mantissa × 10^scale as JSON number text, exactly: plain notation while
/// that stays short, an exponent otherwise (`1e400`). (C++
/// `pjson_detail::emit_decimal`: the same text.)
fn push_decimal(out: &mut String, mantissa: i128, scale: i64) {
    if mantissa == 0 {
        out.push('0');
        return;
    }
    let digits = mantissa.unsigned_abs().to_string();
    if mantissa < 0 {
        out.push('-');
    }
    let point = digits.len() as i64 + scale; // digits before the point
    if (0..=20).contains(&scale) {
        out.push_str(&digits);
        for _ in 0..scale {
            out.push('0');
        }
    } else if scale < 0 && point > 0 {
        out.push_str(&digits[..point as usize]);
        out.push('.');
        out.push_str(&digits[point as usize..]);
    } else if scale < 0 && point > -7 {
        out.push_str("0.");
        for _ in 0..-point {
            out.push('0');
        }
        out.push_str(&digits);
    } else {
        out.push_str(&digits[..1]);
        if digits.len() > 1 {
            out.push('.');
            out.push_str(&digits[1..]);
        }
        out.push('e');
        out.push_str(&(point - 1).to_string());
    }
}

fn push_base64(out: &mut String, bytes: &[u8]) {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for chunk in bytes.chunks(3) {
        let v = (chunk[0] as u32) << 16 | (*chunk.get(1).unwrap_or(&0) as u32) << 8 | *chunk.get(2).unwrap_or(&0) as u32;
        out.push(ALPHABET[(v >> 18) as usize] as char);
        out.push(ALPHABET[(v >> 12 & 63) as usize] as char);
        out.push(if chunk.len() > 1 { ALPHABET[(v >> 6 & 63) as usize] as char } else { '=' });
        out.push(if chunk.len() > 2 { ALPHABET[(v & 63) as usize] as char } else { '=' });
    }
}

/// Text as a JSON string body. The bytes are UTF-8 text and are written as
/// the characters they are (the escapes JSON requires, nothing else);
/// bytes that are not UTF-8 are not text and have no JSON string.
fn emit_string_escaping(out: &mut String, bytes: &[u8]) -> PjsonResult<()> {
    let text = std::str::from_utf8(bytes).map_err(|_| PjsonError("pjson_json: a string is not UTF-8"))?;
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\x08' => out.push_str("\\b"),
            '\x0C' => out.push_str("\\f"),
            '\0'..='\x1F' => out.push_str(&format!("\\u{:04x}", c as u32)),
            _ => out.push(c),
        }
    }
    Ok(())
}


// ── binary16 software widen (decode-only, §4.6) ──────────────────────────
//
// Producers may emit ieee_float at any width.  Our encoder always
// emits binary64; the decoder additionally accepts binary16 and
// widens losslessly to f64.  This is a straightforward bit-twiddle
// per IEEE-754 (sign:1, exp:5, mant:10 → sign:1, exp:11, mant:52).

/// Convert a binary16 (IEEE-754) bit pattern to f64, lossless.
pub fn f16_bits_to_f64(bits: u16) -> f64 {
    let sign = ((bits >> 15) & 0x1) as u64;
    let exp = ((bits >> 10) & 0x1F) as i32;
    let mant = (bits & 0x3FF) as u64;

    let f64_bits: u64 = if exp == 0 {
        if mant == 0 {
            // ±0
            sign << 63
        } else {
            // Subnormal — normalize.
            let mut m = mant;
            let mut e = -14_i32;
            while (m & 0x400) == 0 {
                m <<= 1;
                e -= 1;
            }
            m &= 0x3FF; // strip implicit bit
            ((sign as u64) << 63)
                | ((((e + 1023) as u64) & 0x7FF) << 52)
                | (m << (52 - 10))
        }
    } else if exp == 0x1F {
        // Inf / NaN
        let nan_payload: u64 = (mant as u64) << (52 - 10);
        ((sign as u64) << 63) | (0x7FFu64 << 52) | nan_payload
    } else {
        let e = exp - 15 + 1023;
        ((sign as u64) << 63)
            | ((e as u64) << 52)
            | ((mant as u64) << (52 - 10))
    };
    f64::from_bits(f64_bits)
}

// ── Tests ────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value as JValue;
    use crate::pjson::{from_pjson, to_pjson};

    #[test]
    fn round_trip_simple_object() {
        let json = r#"{"name":"alice","age":30,"active":true}"#;
        let pjson = from_json(json).unwrap();
        let json2 = to_json(&pjson).unwrap();
        // Re-parse both as serde_json::Value and compare.
        let a: JValue = serde_json::from_str(json).unwrap();
        let b: JValue = serde_json::from_str(&json2).unwrap();
        assert_eq!(a, b, "round-trip not value-equal: {} vs {}", json, json2);
    }

    #[test]
    fn round_trip_nested_array() {
        let json = r#"{"items":[1,2,3],"nested":{"k":"v"}}"#;
        let pjson = from_json(json).unwrap();
        let json2 = to_json(&pjson).unwrap();
        let a: JValue = serde_json::from_str(json).unwrap();
        let b: JValue = serde_json::from_str(&json2).unwrap();
        assert_eq!(a, b);
    }

    #[test]
    fn round_trip_negative_int() {
        let json = r#"{"x":-42,"y":0,"z":-1}"#;
        let pjson = from_json(json).unwrap();
        let json2 = to_json(&pjson).unwrap();
        let a: JValue = serde_json::from_str(json).unwrap();
        let b: JValue = serde_json::from_str(&json2).unwrap();
        assert_eq!(a, b);
    }

    #[test]
    fn round_trip_string_with_escapes() {
        let json = r#"{"msg":"hello \"world\"\n\tbye"}"#;
        let pjson = from_json(json).unwrap();
        let json2 = to_json(&pjson).unwrap();
        let a: JValue = serde_json::from_str(json).unwrap();
        let b: JValue = serde_json::from_str(&json2).unwrap();
        assert_eq!(a, b);
    }

    #[test]
    fn sci_notation_preserved() {
        // 1.5e10 should round-trip with scientific notation.
        let json = r#"{"x":1.5e10}"#;
        let pjson = from_json(json).unwrap();
        let json2 = to_json(&pjson).unwrap();
        // The output should contain 'e' (scientific notation).
        assert!(
            json2.bytes().any(|b| b == b'e' || b == b'E'),
            "sci flag lost: {}",
            json2
        );
        // Parsed value still equal.
        let a: JValue = serde_json::from_str(json).unwrap();
        let b: JValue = serde_json::from_str(&json2).unwrap();
        // Numbers compare via their f64 value.
        let a_n = a["x"].as_f64().unwrap();
        let b_n = b["x"].as_f64().unwrap();
        assert!(
            (a_n - b_n).abs() < 1e-6,
            "value mismatch: {} vs {}",
            a_n,
            b_n
        );
    }

    #[test]
    fn decimal_form_no_sci_marker() {
        // 15000000000 (no e/E) should stay decimal.
        let json = r#"{"x":15000000000}"#;
        let pjson = from_json(json).unwrap();
        let json2 = to_json(&pjson).unwrap();
        // Should NOT contain `e`.
        assert!(
            !json2.bytes().any(|b| b == b'e' || b == b'E'),
            "decimal form expected, got: {}",
            json2
        );
    }

    #[test]
    fn json_org_object_test_corpus() {
        // Mini JSON corpus inspired by json.org's test set.
        let docs = [
            r#"null"#,
            r#"true"#,
            r#"false"#,
            r#"0"#,
            r#"42"#,
            r#"-1"#,
            r#""hi""#,
            r#"[]"#,
            r#"[1,2,3]"#,
            r#"{}"#,
            r#"{"a":1}"#,
            r#"{"a":[true,false,null]}"#,
        ];
        for doc in docs.iter() {
            let pjson = from_json(doc).unwrap_or_else(|e| {
                panic!("from_json failed on `{}`: {:?}", doc, e)
            });
            let back = to_json(&pjson).unwrap_or_else(|e| {
                panic!("to_json failed on `{}`: {:?}", doc, e)
            });
            let a: JValue = serde_json::from_str(doc).unwrap();
            let b: JValue = serde_json::from_str(&back).unwrap();
            assert_eq!(a, b, "round-trip mismatch on `{}`", doc);
        }
    }

    #[test]
    fn github_user_record_round_trip() {
        // Representative API payload — mirrors the C++
        // bench_pjson_walk.cpp `kJsonLarge` shape.
        let json = r#"{
            "id":1234567890,
            "login":"alice",
            "name":"Alice",
            "active":true,
            "score":98.7,
            "tags":["a","b","c"]
        }"#;
        let pjson = from_json(json).unwrap();
        let back = to_json(&pjson).unwrap();
        let a: JValue = serde_json::from_str(json).unwrap();
        let b: JValue = serde_json::from_str(&back).unwrap();
        assert_eq!(a, b);
    }

    #[test]
    fn binary16_decode_round_trip() {
        // Hand-craft a binary16 buffer for 1.5 (sign 0, exp 0xF, mant
        // 0x200): bits = 0x3E00.  Tag = ieee_float (6) << 4 | width 1
        // = 0x61.
        let mut buf = vec![0x61u8];
        let bits: u16 = 0x3E00;  // 1.5 in binary16
        buf.extend_from_slice(&bits.to_le_bytes());
        let s = to_json(&buf).unwrap();
        // Parse back as f64; should be ~1.5.
        let v: f64 = s.parse().unwrap();
        assert!((v - 1.5).abs() < 1e-6, "got {} from binary16", v);
    }

    #[test]
    fn validator_round_trip() {
        // Basic struct round-trip via the typed Pjson trait — verifies
        // the from_json output decodes through Pjson::pjson_decode for
        // a representative shape.
        let json = r#"{
            "x": -42,
            "y": 77
        }"#;
        let pjson = from_json(json).unwrap();
        // Decode raw value tree.
        let v = crate::pjson::decode(&pjson).unwrap();
        match v {
            crate::pjson::Value::Object(entries) => {
                assert_eq!(entries.len(), 2);
                assert_eq!(entries[0].0, b"x");
                assert_eq!(entries[1].0, b"y");
            }
            _ => panic!("expected object"),
        }
    }
}
