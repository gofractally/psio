//! Canonical typed view — the ~5 ns "view_one" path described in §1
//! of the spec.
//!
//! The canonical-typed view is the fastest pjson reader: it
//! pre-computes a "schema-hash template" (the sequence of bytes the
//! buffer must contain at canonical positions for the schema to
//! match — primarily the hash[K] table, the slot table widths, and
//! the per-slot key_size bytes), validates the buffer with one
//! `memcmp` against that template, then reads each field via direct
//! offset-table indexing — no per-field hash scan.
//!
//! The template is keyed off the layout the canonical encoder
//! produced for a given record schema. If the buffer was produced by
//! a non-canonical encoder (slots out of order, key padding, missing
//! fields, etc.), the memcmp fails and the caller falls back to the
//! schemaless `pjson_view::View` path.
//!
//! This module provides the building blocks; the per-type views
//! (with named accessors) are emitted by the `pjson_struct!` macro.

use crate::pjson::{key_hash8, obj_form, tag, PjsonError, PjsonResult};
use crate::pjson_view::View;

/// Type-erased canonical typed view. Holds a slice into the
/// underlying buffer and a "field index" pointing at the slot table.
#[derive(Debug, Clone, Copy)]
pub struct TypedView<'a> {
    pub data: &'a [u8],
    pub n: usize,
    pub slot_w: usize,
    pub value_data_start: usize,
    pub slot_table_pos: usize,
    pub hash_table_pos: usize,
    pub value_data_size: usize,
}

impl<'a> TypedView<'a> {
    /// Adopt a buffer, parse the object header, and capture the index
    /// positions. Does NOT verify the canonical-template hash; callers
    /// that have one should call `verify_template`.
    #[inline]
    pub fn from_buffer(data: &'a [u8]) -> PjsonResult<Self> {
        if data.len() < 4 {
            return Err(PjsonError("pjson typed: buffer too small"));
        }
        let tag_byte = data[0];
        if (tag_byte >> 4) != tag::OBJECT || (tag_byte & 0x0F) != obj_form::SINGLE {
            return Err(PjsonError("pjson typed: not a single object"));
        }
        let size = data.len();
        let n = u16::from_le_bytes([data[size - 2], data[size - 1]]) as usize;
        let width_byte = data[1];
        let slot_w_code = width_byte & 0x03;
        let slot_w = (slot_w_code as usize) + 1;
        let entry_stride = slot_w + 1;
        let value_data_start = 2;
        let slot_table_pos = size
            .checked_sub(2 + entry_stride * n)
            .ok_or(PjsonError("pjson typed: slot table overflow"))?;
        let hash_table_pos = slot_table_pos
            .checked_sub(n)
            .ok_or(PjsonError("pjson typed: hash table underflow"))?;
        if hash_table_pos < value_data_start {
            return Err(PjsonError("pjson typed: layout invariant"));
        }
        let value_data_size = hash_table_pos - value_data_start;
        Ok(Self {
            data,
            n,
            slot_w,
            value_data_start,
            slot_table_pos,
            hash_table_pos,
            value_data_size,
        })
    }

    /// Verify a canonical-template hash byte sequence against the
    /// hash[N] table at the canonical position. Returns Err on
    /// mismatch.
    ///
    /// The template is a slice of `n` bytes — `key_hash8(key_i)` for
    /// each canonical schema field i, in canonical order. A typical
    /// derive-macro output stores this as a static array.
    #[inline]
    pub fn verify_template(&self, expected_hash_table: &[u8]) -> PjsonResult<()> {
        if expected_hash_table.len() != self.n {
            return Err(PjsonError(
                "pjson typed: template length doesn't match buffer field count",
            ));
        }
        if &self.data[self.hash_table_pos..self.hash_table_pos + self.n]
            != expected_hash_table
        {
            return Err(PjsonError("pjson typed: template hash mismatch"));
        }
        Ok(())
    }

    /// Sub-view for the i-th canonical field. No bounds check beyond
    /// the value_data range — callers must hold a valid template.
    #[inline]
    pub fn field(&self, i: usize) -> View<'a> {
        let entry_stride = self.slot_w + 1;
        let slot = self.slot_table_pos + i * entry_stride;
        let off_i = self.read_width(slot) as usize;
        let key_size_byte = self.data[slot + self.slot_w];
        let off_next = if i + 1 < self.n {
            self.read_width(self.slot_table_pos + (i + 1) * entry_stride) as usize
        } else {
            self.value_data_size
        };
        let entry = &self.data[self.value_data_start + off_i..self.value_data_start + off_next];
        // For the canonical layout, key_size_byte is a short key —
        // skip those bytes to reach the value tag.
        let klen = key_size_byte as usize;
        let value_buf = &entry[klen..];
        View::new(value_buf)
    }

    /// Read a slot's first `slot_w` bytes as a u32 LE.
    #[inline]
    fn read_width(&self, pos: usize) -> u32 {
        let mut tmp = [0u8; 4];
        tmp[..self.slot_w].copy_from_slice(&self.data[pos..pos + self.slot_w]);
        u32::from_le_bytes(tmp)
    }
}

/// Build the canonical hash-template for a list of keys (in canonical
/// order). Used by derive macros to emit a `static` template.
pub fn template_for(keys: &[&[u8]]) -> Vec<u8> {
    keys.iter().map(|k| key_hash8(k)).collect()
}

/// One-shot canonical-typed reader that **combines** header parse,
/// template verification, and field<i> read into a single inlined
/// call. This is the headline fast path the spec's §1 numbers claim:
/// one memcmp + one slot lookup + one field decode, all in one
/// procedure so the optimizer can fold the bounds checks across
/// boundaries.
///
/// Returns the field's [`View`] on success; returns `None` if the
/// buffer header is malformed, the template doesn't match, or `i`
/// is out of range.
///
/// ```ignore
/// let view = read_canonical_field(buf, &TEMPLATE, 0)?;
/// let value = view.as_uint()?;
/// ```
#[inline]
pub fn read_canonical_field<'a>(
    data: &'a [u8],
    expected_hash_table: &[u8],
    i: usize,
) -> Option<View<'a>> {
    if data.len() < 4 {
        return None;
    }
    let tag_byte = data[0];
    if (tag_byte >> 4) != tag::OBJECT || (tag_byte & 0x0F) != obj_form::SINGLE {
        return None;
    }
    let size = data.len();
    let n = u16::from_le_bytes([data[size - 2], data[size - 1]]) as usize;
    if expected_hash_table.len() != n || i >= n {
        return None;
    }
    let width_byte = data[1];
    let slot_w = ((width_byte & 0x03) as usize) + 1;
    let entry_stride = slot_w + 1;
    let value_data_start = 2;
    let slot_table_pos = size.checked_sub(2 + entry_stride * n)?;
    let hash_table_pos = slot_table_pos.checked_sub(n)?;
    if hash_table_pos < value_data_start {
        return None;
    }
    let value_data_size = hash_table_pos - value_data_start;
    if data.get(hash_table_pos..hash_table_pos + n)? != expected_hash_table {
        return None;
    }
    let slot = slot_table_pos + i * entry_stride;
    let mut tmp = [0u8; 4];
    tmp[..slot_w].copy_from_slice(data.get(slot..slot + slot_w)?);
    let off_i = u32::from_le_bytes(tmp) as usize;
    let key_size_byte = *data.get(slot + slot_w)?;
    let off_next = if i + 1 < n {
        let mut tmp2 = [0u8; 4];
        let p = slot_table_pos + (i + 1) * entry_stride;
        tmp2[..slot_w].copy_from_slice(data.get(p..p + slot_w)?);
        u32::from_le_bytes(tmp2) as usize
    } else {
        value_data_size
    };
    let entry = data.get(value_data_start + off_i..value_data_start + off_next)?;
    let klen = key_size_byte as usize;
    let value_buf = entry.get(klen..)?;
    Some(View::new(value_buf))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pjson::{encode, Value};

    #[test]
    fn typed_view_basic() {
        let v = Value::Object(vec![
            (b"x".as_slice(), Value::UInt(10)),
            (b"y".as_slice(), Value::UInt(20)),
            (b"z".as_slice(), Value::UInt(30)),
        ]);
        let mut buf = vec![];
        encode(&v, &mut buf);
        let tv = TypedView::from_buffer(&buf).unwrap();
        let template = template_for(&[b"x", b"y", b"z"]);
        tv.verify_template(&template).unwrap();
        assert_eq!(tv.field(0).as_uint().unwrap(), 10);
        assert_eq!(tv.field(1).as_uint().unwrap(), 20);
        assert_eq!(tv.field(2).as_uint().unwrap(), 30);
    }

    #[test]
    fn template_mismatch_rejected() {
        let v = Value::Object(vec![
            (b"x".as_slice(), Value::UInt(10)),
            (b"y".as_slice(), Value::UInt(20)),
        ]);
        let mut buf = vec![];
        encode(&v, &mut buf);
        let tv = TypedView::from_buffer(&buf).unwrap();
        // Wrong template (different keys).
        let template = template_for(&[b"alpha", b"beta"]);
        assert!(tv.verify_template(&template).is_err());
    }

    #[test]
    fn template_field_count_mismatch() {
        let v = Value::Object(vec![
            (b"x".as_slice(), Value::UInt(10)),
            (b"y".as_slice(), Value::UInt(20)),
        ]);
        let mut buf = vec![];
        encode(&v, &mut buf);
        let tv = TypedView::from_buffer(&buf).unwrap();
        // Wrong template length.
        let template = template_for(&[b"x", b"y", b"z"]);
        assert!(tv.verify_template(&template).is_err());
    }
}

// Re-export helpers used by the derive macro.
pub use crate::pjson::key_hash8 as derive_key_hash8;
