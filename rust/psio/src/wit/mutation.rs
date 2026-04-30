//! WIT Canonical ABI in-place mutation.
//!
//! `WitMut<'a>` provides mutable access to scalar fields within a
//! WIT-encoded buffer. Only scalars can be mutated in-place; strings
//! and lists would require reallocation and are read-only through this
//! interface.

use super::layout::{align_up, WitLayout};
use super::view::{WitListView, WitViewError};
use std::marker::PhantomData;

/// Mutable view over a WIT Canonical ABI encoded buffer.
///
/// Allows in-place mutation of scalar fields. Strings and lists are
/// read-only (they can't be resized without reallocating the buffer).
pub struct WitMut<'a> {
    base: &'a mut [u8],
    rec_offset: u32,
}

impl<'a> WitMut<'a> {
    /// Create a mutable view rooted at the start of the buffer.
    pub fn from_buffer(buf: &'a mut [u8]) -> Self {
        Self {
            base: buf,
            rec_offset: 0,
        }
    }

    /// Create a mutable view at a specific record offset within the buffer.
    pub fn at_offset(buf: &'a mut [u8], offset: u32) -> Self {
        Self {
            base: buf,
            rec_offset: offset,
        }
    }

    // ── Scalar reads ────────────────────────────────────────────────────────

    fn read_bytes(&self, off: u32, len: u32) -> &[u8] {
        let start = (self.rec_offset + off) as usize;
        &self.base[start..start + len as usize]
    }

    /// Read a bool field.
    pub fn read_bool(&self, field_offset: u32) -> bool {
        self.base[(self.rec_offset + field_offset) as usize] != 0
    }

    /// Read a u8 field.
    pub fn read_u8(&self, field_offset: u32) -> u8 {
        self.base[(self.rec_offset + field_offset) as usize]
    }

    /// Read an i8 field.
    pub fn read_i8(&self, field_offset: u32) -> i8 {
        self.base[(self.rec_offset + field_offset) as usize] as i8
    }

    /// Read a u16 field.
    pub fn read_u16(&self, field_offset: u32) -> u16 {
        let bytes = self.read_bytes(field_offset, 2);
        u16::from_le_bytes(bytes.try_into().unwrap())
    }

    /// Read an i16 field.
    pub fn read_i16(&self, field_offset: u32) -> i16 {
        self.read_u16(field_offset) as i16
    }

    /// Read a u32 field.
    pub fn read_u32(&self, field_offset: u32) -> u32 {
        let bytes = self.read_bytes(field_offset, 4);
        u32::from_le_bytes(bytes.try_into().unwrap())
    }

    /// Read an i32 field.
    pub fn read_i32(&self, field_offset: u32) -> i32 {
        self.read_u32(field_offset) as i32
    }

    /// Read a u64 field.
    pub fn read_u64(&self, field_offset: u32) -> u64 {
        let bytes = self.read_bytes(field_offset, 8);
        u64::from_le_bytes(bytes.try_into().unwrap())
    }

    /// Read an i64 field.
    pub fn read_i64(&self, field_offset: u32) -> i64 {
        self.read_u64(field_offset) as i64
    }

    /// Read an f32 field.
    pub fn read_f32(&self, field_offset: u32) -> f32 {
        let bytes = self.read_bytes(field_offset, 4);
        f32::from_le_bytes(bytes.try_into().unwrap())
    }

    /// Read an f64 field.
    pub fn read_f64(&self, field_offset: u32) -> f64 {
        let bytes = self.read_bytes(field_offset, 8);
        f64::from_le_bytes(bytes.try_into().unwrap())
    }

    // ── Scalar writes ───────────────────────────────────────────────────────

    /// Write a bool field.
    pub fn write_bool(&mut self, field_offset: u32, v: bool) {
        self.base[(self.rec_offset + field_offset) as usize] = if v { 1 } else { 0 };
    }

    /// Write a u8 field.
    pub fn write_u8(&mut self, field_offset: u32, v: u8) {
        self.base[(self.rec_offset + field_offset) as usize] = v;
    }

    /// Write an i8 field.
    pub fn write_i8(&mut self, field_offset: u32, v: i8) {
        self.base[(self.rec_offset + field_offset) as usize] = v as u8;
    }

    /// Write a u16 field.
    pub fn write_u16(&mut self, field_offset: u32, v: u16) {
        let start = (self.rec_offset + field_offset) as usize;
        self.base[start..start + 2].copy_from_slice(&v.to_le_bytes());
    }

    /// Write an i16 field.
    pub fn write_i16(&mut self, field_offset: u32, v: i16) {
        self.write_u16(field_offset, v as u16);
    }

    /// Write a u32 field.
    pub fn write_u32(&mut self, field_offset: u32, v: u32) {
        let start = (self.rec_offset + field_offset) as usize;
        self.base[start..start + 4].copy_from_slice(&v.to_le_bytes());
    }

    /// Write an i32 field.
    pub fn write_i32(&mut self, field_offset: u32, v: i32) {
        self.write_u32(field_offset, v as u32);
    }

    /// Write a u64 field.
    pub fn write_u64(&mut self, field_offset: u32, v: u64) {
        let start = (self.rec_offset + field_offset) as usize;
        self.base[start..start + 8].copy_from_slice(&v.to_le_bytes());
    }

    /// Write an i64 field.
    pub fn write_i64(&mut self, field_offset: u32, v: i64) {
        self.write_u64(field_offset, v as u64);
    }

    /// Write an f32 field.
    pub fn write_f32(&mut self, field_offset: u32, v: f32) {
        let start = (self.rec_offset + field_offset) as usize;
        self.base[start..start + 4].copy_from_slice(&v.to_le_bytes());
    }

    /// Write an f64 field.
    pub fn write_f64(&mut self, field_offset: u32, v: f64) {
        let start = (self.rec_offset + field_offset) as usize;
        self.base[start..start + 8].copy_from_slice(&v.to_le_bytes());
    }

    /// Read a char field. Validates Unicode scalar value range.
    pub fn read_char(&self, field_offset: u32) -> Option<char> {
        let raw = self.read_u32(field_offset);
        char::from_u32(raw)
    }

    /// Write a char field as u32 little-endian.
    pub fn write_char(&mut self, field_offset: u32, v: char) {
        self.write_u32(field_offset, v as u32);
    }

    // ── Read-only access to strings/lists ───────────────────────────────────

    /// Read a string field (read-only; strings can't be resized in-place).
    pub fn read_string(&self, field_offset: u32) -> Result<&str, WitViewError> {
        let str_off = self.read_u32(field_offset);
        let str_len = self.read_u32(field_offset + 4);

        let end = str_off + str_len;
        if end as usize > self.base.len() {
            return Err(WitViewError::StringOutOfBounds {
                offset: str_off,
                length: str_len,
                buf_size: self.base.len() as u32,
            });
        }

        let bytes = &self.base[str_off as usize..end as usize];
        std::str::from_utf8(bytes).map_err(|_| WitViewError::InvalidUtf8 {
            offset: str_off,
            length: str_len,
        })
    }

    /// Read a list field (read-only; lists can't be resized in-place).
    pub fn read_list<T: WitLayout>(&self, field_offset: u32) -> Result<WitListView<'_, T>, WitViewError> {
        let arr_off = self.read_u32(field_offset);
        let count = self.read_u32(field_offset + 4);
        let es = T::wit_size();

        let end = arr_off as u64 + count as u64 * es as u64;
        if end > self.base.len() as u64 {
            return Err(WitViewError::ListOutOfBounds {
                offset: arr_off,
                count,
                elem_size: es,
                buf_size: self.base.len() as u32,
            });
        }

        Ok(WitListView {
            base: self.base,
            arr_offset: arr_off,
            count,
            _marker: PhantomData,
        })
    }

    // ── Result mutation ─────────────────────────────────────────────────────

    /// Read a result discriminant. Returns 0 for Ok, 1 for Err.
    pub fn read_result_discriminant(&self, field_offset: u32) -> u8 {
        self.read_u8(field_offset)
    }

    /// Get the payload offset for a result field.
    /// `T` and `E` are the Ok and Err types respectively.
    pub fn result_payload_offset<T: WitLayout, E: WitLayout>(&self, field_offset: u32) -> u32 {
        let max_case_align = std::cmp::max(T::wit_alignment(), E::wit_alignment());
        field_offset + align_up(1, max_case_align)
    }

    // ── Variant mutation ────────────────────────────────────────────────────

    /// Read a variant discriminant (u8/u16/u32 depending on case_count).
    pub fn read_variant_discriminant(&self, field_offset: u32, case_count: usize) -> u32 {
        let disc_sz = super::layout::discriminant_size(case_count);
        match disc_sz {
            1 => self.read_u8(field_offset) as u32,
            2 => self.read_u16(field_offset) as u32,
            _ => self.read_u32(field_offset),
        }
    }

    /// Get the payload offset for a variant field.
    pub fn variant_payload_offset(&self, field_offset: u32, case_count: usize, max_payload_align: u32) -> u32 {
        field_offset + super::layout::variant_payload_offset(case_count, max_payload_align)
    }

    // ── Option mutation ─────────────────────────────────────────────────────

    /// Read an optional discriminant.
    pub fn read_option_discriminant(&self, field_offset: u32) -> bool {
        self.read_u8(field_offset) != 0
    }

    /// Get the payload offset for an option field (for scalar mutation within).
    /// Returns a field-relative offset suitable for passing to `write_*` methods.
    pub fn option_payload_offset<T: WitLayout>(&self, field_offset: u32) -> u32 {
        let ea = T::wit_alignment();
        field_offset + align_up(1, ea)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wit::pack::WitPack;
    use crate::wit::view::WitUnpack;

    #[test]
    fn mutate_u32() {
        let mut buf = 42u32.wit_pack();
        {
            let mut m = WitMut::from_buffer(&mut buf);
            assert_eq!(m.read_u32(0), 42);
            m.write_u32(0, 99);
            assert_eq!(m.read_u32(0), 99);
        }
        assert_eq!(u32::wit_unpack(&buf).unwrap(), 99);
    }

    #[test]
    fn mutate_bool() {
        let mut buf = false.wit_pack();
        {
            let mut m = WitMut::from_buffer(&mut buf);
            assert_eq!(m.read_bool(0), false);
            m.write_bool(0, true);
        }
        assert_eq!(bool::wit_unpack(&buf).unwrap(), true);
    }

    #[test]
    fn mutate_f64() {
        let mut buf = 1.0f64.wit_pack();
        {
            let mut m = WitMut::from_buffer(&mut buf);
            m.write_f64(0, std::f64::consts::E);
        }
        assert_eq!(f64::wit_unpack(&buf).unwrap(), std::f64::consts::E);
    }

    #[test]
    fn mutate_tuple_field() {
        // Tuple (u8, u32): u8 at offset 0, u32 at offset 4
        let val: (u8, u32) = (10, 20);
        let mut buf = val.wit_pack();
        {
            let mut m = WitMut::from_buffer(&mut buf);
            // u32 is at offset 4
            assert_eq!(m.read_u32(4), 20);
            m.write_u32(4, 999);
        }
        let result = <(u8, u32)>::wit_unpack(&buf).unwrap();
        assert_eq!(result, (10, 999));
    }

    #[test]
    fn read_string_through_mut() {
        let s = String::from("immutable");
        let mut buf = s.wit_pack();
        {
            let m = WitMut::from_buffer(&mut buf);
            assert_eq!(m.read_string(0).unwrap(), "immutable");
        }
    }

    #[test]
    fn mutate_option_payload() {
        let val: Option<u32> = Some(42);
        let mut buf = val.wit_pack();
        {
            let mut m = WitMut::from_buffer(&mut buf);
            assert!(m.read_option_discriminant(0));
            // payload is at align_up(1, 4) = 4
            let payload_off = m.option_payload_offset::<u32>(0);
            // Write directly at the absolute offset (rec_offset is 0)
            m.write_u32(payload_off, 100);
        }
        assert_eq!(Option::<u32>::wit_unpack(&buf).unwrap(), Some(100));
    }
}
