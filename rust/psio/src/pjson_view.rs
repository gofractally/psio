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

use crate::pjson::{
    key_hash8, obj_form, parse_value, read_varuint, tag, typed_array_elem_size, PjsonError,
    PjsonResult, Value,
};

/// Width-byte helpers (mirrors §5.6).
#[inline(always)]
fn width_bytes(code: u8) -> usize {
    (code as usize) + 1
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
        // (No bytes are no value: a reserved tag, which every accessor
        // refuses.)
        self.data.first().copied().unwrap_or(0xFF)
    }

    #[inline(always)]
    pub fn type_code(&self) -> u8 {
        self.tag() >> 4
    }

    #[inline(always)]
    pub fn low_nibble(&self) -> u8 {
        self.tag() & 0x0F
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
        matches!(self.type_code(), tag::NINT_INLINE | tag::NEGINT)
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
        self.data.get(1..).unwrap_or(&[])
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
            tag::NINT_INLINE => {
                if self.data.len() != 1 || self.low_nibble() == 0 {
                    return Err(PjsonError("pjson view: invalid inline negative integer"));
                }
                Ok(-(self.low_nibble() as i128))
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
            1 if self.data.len() == 3 => {
                Ok(crate::pjson_json::f16_bits_to_f64(u16::from_le_bytes([self.data[1], self.data[2]])))
            }
            2 if self.data.len() == 5 => {
                let mut tmp = [0u8; 4];
                tmp.copy_from_slice(&self.data[1..5]);
                Ok(f32::from_le_bytes(tmp) as f64)
            }
            3 if self.data.len() == 9 => {
                let mut tmp = [0u8; 8];
                tmp.copy_from_slice(&self.data[1..9]);
                Ok(f64::from_le_bytes(tmp))
            }
            1..=3 => Err(PjsonError("pjson view: not a pjson value: a float is cut short")),
            _ => Err(PjsonError("pjson view: NotImplemented float width")),
        }
    }

    // ── Placing what is read ────────────────────────────────────────────
    //
    // A view may be handed any bytes (gofractally/psio#15, #17). Every
    // accessor places what it reads inside them first; a member it cannot
    // place is an `Err` (`find` and the other `Option` forms: `None`), never
    // a panic, and a count is compared with the bytes before anything is
    // walked by it.

    /// The index of a generic array or a single object, at its tail.
    fn layout(&self, object: bool) -> PjsonResult<Layout> {
        const CUT: PjsonError = PjsonError("pjson view: not a pjson value: a container is cut short");
        let size = self.data.len();
        if size < 4 {
            return Err(CUT);
        }
        if self.data[1] & 0xFC != 0 {
            return Err(PjsonError("pjson view: not a pjson value: reserved bits in a container's width byte"));
        }
        let n = u16::from_le_bytes([self.data[size - 2], self.data[size - 1]]) as usize;
        let slot_w_code = self.data[1] & 0x03;
        let stride = width_bytes(slot_w_code) + object as usize;
        let per = stride + object as usize; // + the hash byte
        if n > (size - 4) / per {
            return Err(PjsonError("pjson view: not a pjson value: a container's count exceeds its bytes"));
        }
        let slot_table_pos = size - 2 - stride * n;
        let hash_table_pos = slot_table_pos - if object { n } else { 0 };
        Ok(Layout { n, slot_w_code, stride, slot_table_pos, hash_table_pos, value_data_size: hash_table_pos - 2 })
    }

    /// The bytes of child `i` (an array element; an object entry: key then
    /// value).
    fn child(&self, layout: &Layout, i: usize) -> PjsonResult<&'a [u8]> {
        const RANGE: PjsonError =
            PjsonError("pjson view: not a pjson value: a child's offsets are out of order or out of range");
        let offset = |index: usize| -> PjsonResult<usize> {
            let at = layout.slot_table_pos + index * layout.stride;
            let n = width_bytes(layout.slot_w_code);
            let mut tmp = [0u8; 4];
            tmp[..n].copy_from_slice(self.data.get(at..at + n).ok_or(RANGE)?);
            Ok(u32::from_le_bytes(tmp) as usize)
        };
        let start = offset(i)?;
        let end = if i + 1 < layout.n { offset(i + 1)? } else { layout.value_data_size };
        if start > end || end > layout.value_data_size {
            return Err(RANGE);
        }
        Ok(&self.data[2 + start..2 + end])
    }

    /// An object entry split into its key and its value.
    fn entry(&self, layout: &Layout, i: usize) -> PjsonResult<(&'a [u8], View<'a>)> {
        const KEY: PjsonError = PjsonError("pjson view: not a pjson value: a key lies outside its entry");
        let entry = self.child(layout, i)?;
        let key_size_byte = self.data[layout.slot_table_pos + i * layout.stride + layout.stride - 1];
        let (klen, klen_bytes) = if key_size_byte != 0xFF {
            (key_size_byte as usize, 0)
        } else {
            let (excess, n_bytes) = read_varuint(entry)?;
            (0xFFusize.checked_add(excess as usize).ok_or(KEY)?, n_bytes)
        };
        let key_end = klen_bytes.checked_add(klen).ok_or(KEY)?;
        let key = entry.get(klen_bytes..key_end).ok_or(KEY)?;
        Ok((key, View { data: &entry[key_end..] }))
    }

    // ── Array accessors ─────────────────────────────────────────────────

    /// The number of elements of an array: a generic array, a typed array
    /// (§5.1.1) or a row array (§5.2.1, an array of objects that share
    /// their keys).
    pub fn array_len(&self) -> PjsonResult<usize> {
        if self.is_row_array() {
            return Ok(self.rows()?.len());
        }
        if !self.is_array() {
            return Err(PjsonError("pjson view: not an array"));
        }
        if self.low_nibble() != 0 {
            return self.typed_len();
        }
        Ok(self.layout(false)?.n)
    }

    /// Element `i` of a generic array. (A typed array's elements are
    /// numbers without a tag: `typed_int_at` / `typed_f64_at`; a row
    /// array's are records: `rows()`.)
    pub fn array_get(&self, i: usize) -> PjsonResult<View<'a>> {
        if !self.is_array() {
            return Err(PjsonError("pjson view: not an array"));
        }
        if self.low_nibble() != 0 {
            return Err(PjsonError("pjson view: typed_array on generic accessor"));
        }
        let layout = self.layout(false)?;
        if i >= layout.n {
            return Err(PjsonError("pjson view: array index out of range"));
        }
        Ok(View { data: self.child(&layout, i)? })
    }

    pub fn typed_array_code(&self) -> PjsonResult<u8> {
        if !self.is_typed_array() {
            return Err(PjsonError("pjson view: not a typed_array"));
        }
        Ok(self.low_nibble() - 1)
    }

    /// The count of a typed array, which its bytes must hold exactly.
    fn typed_len(&self) -> PjsonResult<usize> {
        let elem_size = typed_array_elem_size(self.typed_array_code()?);
        let size = self.data.len();
        if size < 3 || elem_size == 0 {
            return Err(PjsonError("pjson view: not a pjson value: a typed array is cut short"));
        }
        let n = u16::from_le_bytes([self.data[size - 2], self.data[size - 1]]) as usize;
        if size != 3 + n * elem_size {
            return Err(PjsonError("pjson view: not a pjson value: a typed array's count does not match its bytes"));
        }
        Ok(n)
    }

    pub fn typed_array_bytes(&self) -> PjsonResult<&'a [u8]> {
        let n = self.typed_len()?;
        let elem_size = typed_array_elem_size(self.typed_array_code()?);
        Ok(&self.data[1..1 + elem_size * n])
    }

    /// The little-endian bytes of element `i` of a typed array.
    fn typed_elem(&self, i: usize) -> PjsonResult<(u8, &'a [u8])> {
        let n = self.typed_len()?;
        if i >= n {
            return Err(PjsonError("pjson view: typed array index out of range"));
        }
        let code = self.typed_array_code()?;
        let elem_size = typed_array_elem_size(code);
        Ok((code, &self.data[1 + i * elem_size..1 + (i + 1) * elem_size]))
    }

    /// Element `i` of a typed array of integers (codes i8..u64), without
    /// decoding the array.
    pub fn typed_int_at(&self, i: usize) -> PjsonResult<i128> {
        let (code, b) = self.typed_elem(i)?;
        Ok(match code {
            0 => i8::from_le_bytes([b[0]]) as i128,
            1 => i16::from_le_bytes([b[0], b[1]]) as i128,
            2 => i32::from_le_bytes([b[0], b[1], b[2], b[3]]) as i128,
            3 => i64::from_le_bytes(b.try_into().map_err(|_| PjsonError("pjson view: typed array element"))?) as i128,
            4 => b[0] as i128,
            5 => u16::from_le_bytes([b[0], b[1]]) as i128,
            6 => u32::from_le_bytes([b[0], b[1], b[2], b[3]]) as i128,
            7 => u64::from_le_bytes(b.try_into().map_err(|_| PjsonError("pjson view: typed array element"))?) as i128,
            _ => return Err(PjsonError("pjson view: a typed array of floats has no integer element")),
        })
    }

    /// Element `i` of a typed array as an `f64` (exact for the float codes
    /// and for integers up to 2^53).
    pub fn typed_f64_at(&self, i: usize) -> PjsonResult<f64> {
        let (code, b) = self.typed_elem(i)?;
        Ok(match code {
            8 => f32::from_le_bytes([b[0], b[1], b[2], b[3]]) as f64,
            9 => f64::from_le_bytes(b.try_into().map_err(|_| PjsonError("pjson view: typed array element"))?),
            _ => self.typed_int_at(i)? as f64,
        })
    }

    /// The rows of a row array (§5.2.1): `rows()?.get(i)?.find(b"key")`
    /// reaches one field of one record without decoding anything else.
    pub fn rows(&self) -> PjsonResult<RowsView<'a>> {
        RowsView::new(self.data)
    }

    // ── Object accessors ────────────────────────────────────────────────

    pub fn object_len(&self) -> PjsonResult<usize> {
        if !self.is_object() {
            return Err(PjsonError("pjson view: not an object"));
        }
        if self.low_nibble() != obj_form::SINGLE {
            return Err(PjsonError("pjson view: not a generic object"));
        }
        Ok(self.layout(true)?.n)
    }

    pub fn object_at(&self, i: usize) -> PjsonResult<(&'a [u8], View<'a>)> {
        self.object_len()?;
        let layout = self.layout(true)?;
        if i >= layout.n {
            return Err(PjsonError("pjson view: object index out of range"));
        }
        self.entry(&layout, i)
    }

    /// The value of the first field named `query`, through the hash
    /// prefilter (§5.3, §11). `None` for a key that is not there, and for
    /// bytes that are not an object.
    pub fn find(&self, query: &[u8]) -> Option<View<'a>> {
        if !self.is_object() || self.low_nibble() != obj_form::SINGLE {
            return None;
        }
        let layout = self.layout(true).ok()?;
        let hashes = &self.data[layout.hash_table_pos..layout.hash_table_pos + layout.n];
        let want = key_hash8(query);
        for (i, _) in hashes.iter().enumerate().filter(|(_, &hash)| hash == want) {
            let (key, value) = self.entry(&layout, i).ok()?;
            if key == query {
                return Some(value);
            }
        }
        None
    }
}

struct Layout {
    n: usize,
    slot_w_code: u8,
    stride: usize,
    slot_table_pos: usize,
    hash_table_pos: usize,
    value_data_size: usize,
}

/// A row array (docs/pjson-spec.md §5.2.1): N records that share K keys,
/// stored once. Constructed by [`View::rows`], which compares the header
/// with the bytes.
#[derive(Debug, Clone, Copy)]
pub struct RowsView<'a> {
    data: &'a [u8],
    k: usize,
    slot_w_code: u8,
    recoff_w_code: u8,
    key_slots_pos: usize,
    hash_pos: usize,
    keys_pos: usize,
    keys_area_size: usize,
    records_body_start: usize,
    record_offsets_pos: usize,
    n: usize,
}

impl<'a> RowsView<'a> {
    fn new(data: &'a [u8]) -> PjsonResult<Self> {
        const BAD: PjsonError = PjsonError("pjson view: not a pjson value: a row array's header does not fit its bytes");
        let size = data.len();
        if size < 4 || data[0] != (tag::OBJECT << 4) | obj_form::ROW_ARRAY {
            return Err(PjsonError("pjson view: not a row array"));
        }
        if data[1] & 0xF0 != 0 {
            return Err(BAD);
        }
        let slot_w_code = data[1] & 0x03;
        let recoff_w_code = (data[1] >> 2) & 0x03;
        let (k, k_bytes) = read_varuint(&data[2..size - 2])?;
        let key_slots_pos = 2 + k_bytes;
        // K keys take at least five bytes each (slot + hash) in the header.
        if k == 0 || key_slots_pos > size - 2 || k > ((size - 2 - key_slots_pos) / 5) as u64 {
            return Err(BAD);
        }
        let k = k as usize;
        let hash_pos = key_slots_pos + 4 * k;
        let keys_pos = hash_pos + k;
        let last = u32::from_le_bytes(data[key_slots_pos + (k - 1) * 4..key_slots_pos + k * 4].try_into().map_err(|_| BAD)?);
        let keys_area_size = (last & 0x00FF_FFFF) as usize + (last >> 24) as usize;
        if keys_area_size > size - 2 - keys_pos {
            return Err(BAD);
        }
        let records_body_start = keys_pos + keys_area_size;
        let n = u16::from_le_bytes([data[size - 2], data[size - 1]]) as usize;
        if n > (size - 2 - records_body_start) / width_bytes(recoff_w_code) {
            return Err(BAD);
        }
        let record_offsets_pos = size - 2 - n * width_bytes(recoff_w_code);
        Ok(Self { data, k, slot_w_code, recoff_w_code, key_slots_pos, hash_pos, keys_pos, keys_area_size, records_body_start, record_offsets_pos, n })
    }

    /// The number of records.
    pub fn len(&self) -> usize {
        self.n
    }
    pub fn is_empty(&self) -> bool {
        self.n == 0
    }
    /// The number of fields of every record.
    pub fn field_count(&self) -> usize {
        self.k
    }

    /// Key `j` of the shared schema.
    pub fn key(&self, j: usize) -> PjsonResult<&'a [u8]> {
        const BAD: PjsonError = PjsonError("pjson view: not a pjson value: a row array key lies outside the keys");
        if j >= self.k {
            return Err(PjsonError("pjson view: row field index out of range"));
        }
        let at = self.key_slots_pos + j * 4;
        let slot = u32::from_le_bytes(self.data[at..at + 4].try_into().map_err(|_| BAD)?);
        let (offset, size) = ((slot & 0x00FF_FFFF) as usize, (slot >> 24) as usize);
        if offset + size > self.keys_area_size {
            return Err(BAD);
        }
        Ok(&self.data[self.keys_pos + offset..self.keys_pos + offset + size])
    }

    /// Record `i`.
    pub fn get(&self, i: usize) -> PjsonResult<RowView<'a>> {
        const BAD: PjsonError =
            PjsonError("pjson view: not a pjson value: a row's offsets are out of order or out of range");
        if i >= self.n {
            return Err(PjsonError("pjson view: row index out of range"));
        }
        let w = width_bytes(self.recoff_w_code);
        let body_size = self.record_offsets_pos - self.records_body_start;
        let offset = |index: usize| -> usize {
            let at = self.record_offsets_pos + index * w;
            let mut tmp = [0u8; 4];
            tmp[..w].copy_from_slice(&self.data[at..at + w]);
            u32::from_le_bytes(tmp) as usize
        };
        let start = offset(i);
        let end = if i + 1 < self.n { offset(i + 1) } else { body_size };
        if start > end || end > body_size || end - start < self.k * width_bytes(self.slot_w_code) {
            return Err(BAD);
        }
        Ok(RowView { rows: *self, record: &self.data[self.records_body_start + start..self.records_body_start + end] })
    }
}

/// One record of a row array: its values, and the keys of the array.
#[derive(Debug, Clone, Copy)]
pub struct RowView<'a> {
    rows: RowsView<'a>,
    record: &'a [u8],
}

impl<'a> RowView<'a> {
    /// The number of fields.
    pub fn len(&self) -> usize {
        self.rows.k
    }
    pub fn is_empty(&self) -> bool {
        self.rows.k == 0
    }

    /// Field `j`: its key (of the shared schema) and its value.
    pub fn field(&self, j: usize) -> PjsonResult<(&'a [u8], View<'a>)> {
        const BAD: PjsonError =
            PjsonError("pjson view: not a pjson value: a row field's offsets are out of order or out of range");
        let key = self.rows.key(j)?;
        let w = width_bytes(self.rows.slot_w_code);
        let slots = self.record.len() - self.rows.k * w;
        let offset = |index: usize| -> usize {
            let at = slots + index * w;
            let mut tmp = [0u8; 4];
            tmp[..w].copy_from_slice(&self.record[at..at + w]);
            u32::from_le_bytes(tmp) as usize
        };
        let start = offset(j);
        let end = if j + 1 < self.rows.k { offset(j + 1) } else { slots };
        if start > end || end > slots {
            return Err(BAD);
        }
        Ok((key, View { data: &self.record[start..end] }))
    }

    /// The value of the field named `query`: the hash prefilter of the
    /// shared schema, then one key compare per hit.
    pub fn find(&self, query: &[u8]) -> Option<View<'a>> {
        let hashes = &self.rows.data[self.rows.hash_pos..self.rows.hash_pos + self.rows.k];
        let want = key_hash8(query);
        for (j, _) in hashes.iter().enumerate().filter(|(_, &hash)| hash == want) {
            if self.rows.key(j).ok()? == query {
                return self.field(j).ok().map(|(_, value)| value);
            }
        }
        None
    }
}

pub fn view_of(buf: &[u8]) -> View<'_> {
    View::new(buf)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pjson::{encode, str_flag, Value};

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
        let buf = crate::pjson::to_pjson(&v);
        let view = View::new(&buf);
        assert!(view.is_typed_array());
        assert_eq!(view.typed_array_code().unwrap(), crate::pjson::tac::I32);
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
