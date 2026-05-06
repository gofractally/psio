//! Zero-copy pjson views — schemaless lookup over a buffer.
//!
//! Mirrors C++'s `pjson_view` from `cpp/include/psio/pjson_view.hpp`.
//! Field accessors are pointer arithmetic over the underlying buffer
//! (no allocation, no value materialization).
//!
//! Use this when you have a pjson buffer and don't know its top-level
//! schema at compile time — e.g. servicing arbitrary RPC requests.
//! For canonical-typed access (memcmp + indexed offsets) see
//! `pjson_typed::TypedView`.

use crate::pjson_legacy::{
    key_hash8, obj_form, parse_value, read_varuint, tag, typed_array_elem_size, PjsonError,
    PjsonResult, Value,
};

/// Width-byte helpers (mirrors §5.6).
#[inline(always)]
fn width_bytes(code: u8) -> usize {
    (code as usize) + 1
}

#[inline(always)]
fn read_width(buf: &[u8], pos: usize, code: u8) -> u32 {
    let n = width_bytes(code);
    let mut tmp = [0u8; 4];
    tmp[..n].copy_from_slice(&buf[pos..pos + n]);
    u32::from_le_bytes(tmp)
}

/// Schemaless zero-copy view over a pjson buffer.
#[derive(Debug, Clone, Copy)]
pub struct View<'a> {
    pub data: &'a [u8],
}

impl<'a> View<'a> {
    #[inline(always)]
    pub fn new(data: &'a [u8]) -> Self {
        Self { data }
    }

    #[inline(always)]
    pub fn raw(&self) -> &'a [u8] {
        self.data
    }

    #[inline(always)]
    pub fn tag(&self) -> u8 {
        self.data[0]
    }

    #[inline(always)]
    pub fn type_code(&self) -> u8 {
        self.data[0] >> 4
    }

    #[inline(always)]
    pub fn low_nibble(&self) -> u8 {
        self.data[0] & 0x0F
    }

    #[inline(always)]
    pub fn is_null(&self) -> bool {
        self.type_code() == tag::NULL
    }
    #[inline(always)]
    pub fn is_bool(&self) -> bool {
        self.type_code() == tag::BOOL
    }
    #[inline(always)]
    pub fn is_uint(&self) -> bool {
        self.type_code() == tag::UINT_INLINE || self.type_code() == tag::UINT
    }
    #[inline(always)]
    pub fn is_negint(&self) -> bool {
        self.type_code() == tag::NEGINT
    }
    #[inline(always)]
    pub fn is_string(&self) -> bool {
        self.type_code() == tag::STRING
    }
    #[inline(always)]
    pub fn is_bytes(&self) -> bool {
        self.type_code() == tag::BYTES
    }
    #[inline(always)]
    pub fn is_array(&self) -> bool {
        self.type_code() == tag::ARRAY
    }
    #[inline(always)]
    pub fn is_object(&self) -> bool {
        self.type_code() == tag::OBJECT
    }

    /// True iff this is a typed_array (`is_array && low_nibble != 0`).
    pub fn is_typed_array(&self) -> bool {
        self.is_array() && self.low_nibble() != 0 && self.low_nibble() <= 10
    }

    /// True iff this is a row_array (`is_object && low_nibble == 1`).
    pub fn is_row_array(&self) -> bool {
        self.is_object() && self.low_nibble() == obj_form::ROW_ARRAY
    }

    /// Materialized value (allocates).
    pub fn to_value(&self) -> PjsonResult<Value<'a>> {
        parse_value(self.data, self.data.len())
    }

    // ── String / bytes accessors ─────────────────────────────────────────

    /// Bytes between the tag and the end (no length prefix). For
    /// strings, this is the UTF-8 (or escape-form) text. For bytes,
    /// the raw octets.
    pub fn payload(&self) -> &'a [u8] {
        &self.data[1..]
    }

    /// String encoding flag (only meaningful for `is_string()`).
    pub fn string_flag(&self) -> u8 {
        self.low_nibble()
    }

    /// String text as UTF-8 view (returns Err on invalid UTF-8).
    pub fn as_str(&self) -> PjsonResult<&'a str> {
        if !self.is_string() {
            return Err(PjsonError("pjson view: not a string"));
        }
        std::str::from_utf8(self.payload())
            .map_err(|_| PjsonError("pjson view: invalid UTF-8"))
    }

    /// Boolean value (panics if not a bool).
    pub fn as_bool(&self) -> PjsonResult<bool> {
        if !self.is_bool() {
            return Err(PjsonError("pjson view: not a bool"));
        }
        match self.low_nibble() {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err(PjsonError("pjson view: reserved bool low nibble")),
        }
    }

    /// Unsigned integer value as `u128`. Returns Err if the tag is
    /// neither uint_inline nor uint.
    #[inline]
    pub fn as_uint(&self) -> PjsonResult<u128> {
        match self.type_code() {
            tag::UINT_INLINE => Ok(self.low_nibble() as u128),
            tag::UINT => {
                let bc = self.low_nibble() as usize + 1;
                if 1 + bc != self.data.len() {
                    return Err(PjsonError("pjson view: uint size mismatch"));
                }
                let mut tmp = [0u8; 16];
                tmp[..bc].copy_from_slice(&self.data[1..1 + bc]);
                Ok(u128::from_le_bytes(tmp))
            }
            _ => Err(PjsonError("pjson view: not a uint")),
        }
    }

    /// Signed integer value as `i128`. Negint is decoded by negating
    /// the magnitude; for `i128::MIN` the magnitude is exactly 2^127
    /// and `wrapping_neg` produces the correct value.
    #[inline]
    pub fn as_int(&self) -> PjsonResult<i128> {
        match self.type_code() {
            tag::UINT_INLINE | tag::UINT => {
                let u = self.as_uint()?;
                if u > i128::MAX as u128 {
                    return Err(PjsonError("pjson view: int overflow"));
                }
                Ok(u as i128)
            }
            tag::NEGINT => {
                let bc = self.low_nibble() as usize + 1;
                if 1 + bc != self.data.len() {
                    return Err(PjsonError("pjson view: negint size mismatch"));
                }
                let mut tmp = [0u8; 16];
                tmp[..bc].copy_from_slice(&self.data[1..1 + bc]);
                let mag = u128::from_le_bytes(tmp);
                if mag == 0 {
                    return Err(PjsonError("pjson view: negint zero reserved"));
                }
                Ok((mag as i128).wrapping_neg())
            }
            _ => Err(PjsonError("pjson view: not an int")),
        }
    }

    /// f64 value. Decodes binary64 directly; binary32 is widened.
    pub fn as_f64(&self) -> PjsonResult<f64> {
        if self.type_code() != tag::IEEE_FLOAT {
            return Err(PjsonError("pjson view: not a float"));
        }
        let width_bits = self.low_nibble() & 0x07;
        match width_bits {
            2 => {
                let mut tmp = [0u8; 4];
                tmp.copy_from_slice(&self.data[1..5]);
                Ok(f32::from_le_bytes(tmp) as f64)
            }
            3 => {
                let mut tmp = [0u8; 8];
                tmp.copy_from_slice(&self.data[1..9]);
                Ok(f64::from_le_bytes(tmp))
            }
            _ => Err(PjsonError("pjson view: NotImplemented float width")),
        }
    }

    // ── Array accessors ─────────────────────────────────────────────────

    /// For generic and typed arrays: the element count.
    pub fn array_len(&self) -> PjsonResult<usize> {
        if !self.is_array() {
            return Err(PjsonError("pjson view: not an array"));
        }
        let size = self.data.len();
        if size < 3 {
            return Err(PjsonError("pjson view: array too small"));
        }
        Ok(u16::from_le_bytes([self.data[size - 2], self.data[size - 1]]) as usize)
    }

    /// Generic array — return the i-th element as a sub-view. Returns
    /// `Err` for typed arrays (use `typed_array_*` accessors instead).
    pub fn array_get(&self, i: usize) -> PjsonResult<View<'a>> {
        if !self.is_array() {
            return Err(PjsonError("pjson view: not an array"));
        }
        if self.low_nibble() != 0 {
            return Err(PjsonError("pjson view: typed_array on generic accessor"));
        }
        let size = self.data.len();
        let n = u16::from_le_bytes([self.data[size - 2], self.data[size - 1]]) as usize;
        if i >= n {
            return Err(PjsonError("pjson view: array index out of range"));
        }
        let width_byte = self.data[1];
        let slot_w_code = width_byte & 0x03;
        let slot_w = width_bytes(slot_w_code);
        let value_data_start = 2;
        let slot_table_pos = size
            .checked_sub(2 + slot_w * n)
            .ok_or(PjsonError("pjson view: array slot_table overflow"))?;
        let value_data_size = slot_table_pos - value_data_start;
        let off_i = read_width(self.data, slot_table_pos + i * slot_w, slot_w_code) as usize;
        let off_next = if i + 1 < n {
            read_width(
                self.data,
                slot_table_pos + (i + 1) * slot_w,
                slot_w_code,
            ) as usize
        } else {
            value_data_size
        };
        Ok(View {
            data: &self.data[value_data_start + off_i..value_data_start + off_next],
        })
    }

    /// Typed array — element type code (low_nibble - 1).
    pub fn typed_array_code(&self) -> PjsonResult<u8> {
        if !self.is_typed_array() {
            return Err(PjsonError("pjson view: not a typed_array"));
        }
        Ok(self.low_nibble() - 1)
    }

    /// Typed array — element bytes (raw LE).
    pub fn typed_array_bytes(&self) -> PjsonResult<&'a [u8]> {
        let code = self.typed_array_code()?;
        let elem_size = typed_array_elem_size(code);
        let n = self.array_len()?;
        Ok(&self.data[1..1 + elem_size * n])
    }

    // ── Object accessors ────────────────────────────────────────────────

    /// Generic object — number of fields.
    pub fn object_len(&self) -> PjsonResult<usize> {
        if !self.is_object() {
            return Err(PjsonError("pjson view: not an object"));
        }
        if self.low_nibble() != obj_form::SINGLE {
            return Err(PjsonError("pjson view: not a generic object"));
        }
        let size = self.data.len();
        if size < 4 {
            return Err(PjsonError("pjson view: object too small"));
        }
        Ok(u16::from_le_bytes([self.data[size - 2], self.data[size - 1]]) as usize)
    }

    /// Generic object — return a (key, value) sub-view by index.
    pub fn object_at(&self, i: usize) -> PjsonResult<(&'a [u8], View<'a>)> {
        let n = self.object_len()?;
        if i >= n {
            return Err(PjsonError("pjson view: object index out of range"));
        }
        let size = self.data.len();
        let width_byte = self.data[1];
        let slot_w_code = width_byte & 0x03;
        let slot_w = width_bytes(slot_w_code);
        let entry_stride = slot_w + 1;
        let value_data_start = 2;
        let slot_table_pos = size
            .checked_sub(2 + entry_stride * n)
            .ok_or(PjsonError("pjson view: object slot_table overflow"))?;
        let hash_table_pos = slot_table_pos - n;
        let value_data_size = hash_table_pos - value_data_start;

        let slot = slot_table_pos + i * entry_stride;
        let off_i = read_width(self.data, slot, slot_w_code) as usize;
        let key_size_byte = self.data[slot + slot_w];
        let off_next = if i + 1 < n {
            read_width(
                self.data,
                slot_table_pos + (i + 1) * entry_stride,
                slot_w_code,
            ) as usize
        } else {
            value_data_size
        };
        let entry =
            &self.data[value_data_start + off_i..value_data_start + off_next];
        let entry_size = entry.len();
        let (klen, klen_bytes) = if key_size_byte != 0xFF {
            (key_size_byte as usize, 0)
        } else {
            let (excess, n_bytes) = read_varuint(entry)?;
            (0xFF + excess as usize, n_bytes)
        };
        if klen_bytes + klen > entry_size {
            return Err(PjsonError("pjson view: object key out of range"));
        }
        let key = &entry[klen_bytes..klen_bytes + klen];
        let value_buf = &entry[klen_bytes + klen..];
        Ok((key, View { data: value_buf }))
    }

    /// Look up a field by key (§11). Hash-prefilter scan + key-equal verify.
    pub fn find(&self, query: &[u8]) -> Option<View<'a>> {
        if !self.is_object() || self.low_nibble() != obj_form::SINGLE {
            return None;
        }
        let size = self.data.len();
        if size < 4 {
            return None;
        }
        let n = u16::from_le_bytes([self.data[size - 2], self.data[size - 1]]) as usize;
        let width_byte = self.data[1];
        let slot_w_code = width_byte & 0x03;
        let slot_w = width_bytes(slot_w_code);
        let entry_stride = slot_w + 1;
        let value_data_start = 2;
        let slot_table_pos = size.checked_sub(2 + entry_stride * n)?;
        let hash_table_pos = slot_table_pos.checked_sub(n)?;
        if hash_table_pos < value_data_start {
            return None;
        }
        let value_data_size = hash_table_pos - value_data_start;

        let want = key_hash8(query);
        let mut i = 0usize;
        while i < n {
            // Find first hash-byte hit at or after i.
            let mut hit: Option<usize> = None;
            for j in i..n {
                if self.data[hash_table_pos + j] == want {
                    hit = Some(j);
                    break;
                }
            }
            let j = hit?;
            i = j;

            // Verify by reading the slot.
            let slot = slot_table_pos + i * entry_stride;
            let off_i = read_width(self.data, slot, slot_w_code) as usize;
            let key_size_byte = self.data[slot + slot_w];
            let off_next = if i + 1 < n {
                read_width(
                    self.data,
                    slot_table_pos + (i + 1) * entry_stride,
                    slot_w_code,
                ) as usize
            } else {
                value_data_size
            };
            let entry =
                &self.data[value_data_start + off_i..value_data_start + off_next];
            let (klen, klen_bytes) = if key_size_byte != 0xFF {
                (key_size_byte as usize, 0)
            } else {
                let (excess, nb) = read_varuint(entry).ok()?;
                (0xFF + excess as usize, nb)
            };
            if klen == query.len() && entry[klen_bytes..klen_bytes + klen] == *query {
                let value_buf = &entry[klen_bytes + klen..];
                return Some(View { data: value_buf });
            }

            i += 1;
        }
        None
    }
}

/// Convenience to wrap a buffer in a view.
pub fn view_of(buf: &[u8]) -> View<'_> {
    View::new(buf)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pjson_legacy::{encode, str_flag, Value};

    fn pjson_for(v: &Value<'_>) -> Vec<u8> {
        let mut out = vec![];
        encode(v, &mut out);
        out
    }

    #[test]
    fn view_object_find() {
        let v = Value::Object(vec![
            (b"alpha".as_slice(), Value::UInt(1)),
            (b"beta".as_slice(), Value::Str(b"two", str_flag::RAW_TEXT)),
            (b"gamma".as_slice(), Value::Bool(true)),
        ]);
        let buf = pjson_for(&v);
        let view = View::new(&buf);
        assert!(view.is_object());
        assert_eq!(view.object_len().unwrap(), 3);

        let alpha = view.find(b"alpha").unwrap();
        assert_eq!(alpha.as_uint().unwrap(), 1);
        let beta = view.find(b"beta").unwrap();
        assert_eq!(beta.as_str().unwrap(), "two");
        let gamma = view.find(b"gamma").unwrap();
        assert!(gamma.as_bool().unwrap());

        assert!(view.find(b"missing").is_none());
    }

    #[test]
    fn view_typed_array() {
        let v: Vec<i32> = vec![-1, 0, 1, 100, 1000];
        let buf = crate::pjson_legacy::to_pjson(&v);
        let view = View::new(&buf);
        assert!(view.is_typed_array());
        assert_eq!(view.typed_array_code().unwrap(), crate::pjson_legacy::tac::I32);
        assert_eq!(view.array_len().unwrap(), 5);
        let bytes = view.typed_array_bytes().unwrap();
        assert_eq!(bytes.len(), 20);
    }

    #[test]
    fn view_generic_array_index() {
        let v = Value::Array(vec![
            Value::UInt(10),
            Value::UInt(20),
            Value::UInt(30),
        ]);
        let buf = pjson_for(&v);
        let view = View::new(&buf);
        assert_eq!(view.array_len().unwrap(), 3);
        assert_eq!(view.array_get(0).unwrap().as_uint().unwrap(), 10);
        assert_eq!(view.array_get(2).unwrap().as_uint().unwrap(), 30);
    }
}
