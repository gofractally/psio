//! Cap'n Proto in-place scalar mutation.
//!
//! `CapnpMut<'a>` provides mutable access to scalar fields within a
//! Cap'n Proto encoded buffer. Only scalars can be mutated in-place;
//! strings and lists live in the pointer section and cannot be resized.
//!
//! Field locations are described by [`FieldLoc`] from the layout module.
//! Booleans are bit-packed within the data section (one bit per bool),
//! so mutation requires bit-level set/clear operations.

use super::layout::FieldLoc;
use super::wire;

/// Mutable view over a Cap'n Proto message buffer.
///
/// Allows in-place mutation of scalar fields in the data section.
/// Strings and lists are read-only (they live in the pointer section
/// and cannot be resized without rebuilding the message).
pub struct CapnpMut<'a> {
    /// The full message buffer (segment table + segment data).
    buf: &'a mut [u8],
    /// Byte offset of the segment data within `buf`.
    seg_start: usize,
    /// Byte offset of the root struct's data section within the segment.
    data_offset: usize,
    /// Number of data words in the root struct.
    data_words: u16,
    /// Number of pointer slots in the root struct (kept for read_text).
    #[allow(dead_code)]
    ptr_count: u16,
}

impl<'a> CapnpMut<'a> {
    /// Create a mutable view over a complete flat-array Cap'n Proto message.
    ///
    /// Returns `None` if the message is invalid or cannot be parsed.
    pub fn from_message(buf: &'a mut [u8]) -> Option<Self> {
        // Parse segment table to find segment start
        let seg_start = {
            let (start, _) = wire::parse_segment_table(buf).ok()?;
            start
        };

        // Resolve root struct pointer
        let segment = &buf[seg_start..];
        let ptr = wire::resolve_struct_ptr(segment, 0)?;

        Some(CapnpMut {
            data_offset: ptr.data_offset,
            data_words: ptr.data_words,
            ptr_count: ptr.ptr_count,
            seg_start,
            buf,
        })
    }

    /// Absolute byte offset in `buf` for a data-section field.
    #[inline]
    fn abs_offset(&self, loc: &FieldLoc) -> usize {
        debug_assert!(!loc.is_ptr, "cannot mutate pointer fields in-place");
        self.seg_start + self.data_offset + loc.offset as usize
    }

    // ── Scalar reads ────────────────────────────────────────────────────

    /// Read a bool field (bit-packed).
    pub fn read_bool(&self, loc: &FieldLoc) -> bool {
        let off = self.abs_offset(loc);
        if off >= self.buf.len() {
            return false;
        }
        (self.buf[off] >> loc.bit_index) & 1 != 0
    }

    /// Read a u8 field.
    pub fn read_u8(&self, loc: &FieldLoc) -> u8 {
        let off = self.abs_offset(loc);
        if off >= self.buf.len() {
            return 0;
        }
        self.buf[off]
    }

    /// Read an i8 field.
    pub fn read_i8(&self, loc: &FieldLoc) -> i8 {
        self.read_u8(loc) as i8
    }

    /// Read a u16 field.
    pub fn read_u16(&self, loc: &FieldLoc) -> u16 {
        let off = self.abs_offset(loc);
        if off + 2 > self.buf.len() {
            return 0;
        }
        u16::from_le_bytes(self.buf[off..off + 2].try_into().unwrap())
    }

    /// Read an i16 field.
    pub fn read_i16(&self, loc: &FieldLoc) -> i16 {
        self.read_u16(loc) as i16
    }

    /// Read a u32 field.
    pub fn read_u32(&self, loc: &FieldLoc) -> u32 {
        let off = self.abs_offset(loc);
        if off + 4 > self.buf.len() {
            return 0;
        }
        u32::from_le_bytes(self.buf[off..off + 4].try_into().unwrap())
    }

    /// Read an i32 field.
    pub fn read_i32(&self, loc: &FieldLoc) -> i32 {
        self.read_u32(loc) as i32
    }

    /// Read a u64 field.
    pub fn read_u64(&self, loc: &FieldLoc) -> u64 {
        let off = self.abs_offset(loc);
        if off + 8 > self.buf.len() {
            return 0;
        }
        u64::from_le_bytes(self.buf[off..off + 8].try_into().unwrap())
    }

    /// Read an i64 field.
    pub fn read_i64(&self, loc: &FieldLoc) -> i64 {
        self.read_u64(loc) as i64
    }

    /// Read an f32 field.
    pub fn read_f32(&self, loc: &FieldLoc) -> f32 {
        f32::from_bits(self.read_u32(loc))
    }

    /// Read an f64 field.
    pub fn read_f64(&self, loc: &FieldLoc) -> f64 {
        f64::from_bits(self.read_u64(loc))
    }

    // ── Scalar writes ───────────────────────────────────────────────────

    /// Write a bool field (bit-level set/clear).
    pub fn write_bool(&mut self, loc: &FieldLoc, val: bool) {
        let off = self.abs_offset(loc);
        if off >= self.buf.len() {
            return;
        }
        if val {
            self.buf[off] |= 1u8 << loc.bit_index;
        } else {
            self.buf[off] &= !(1u8 << loc.bit_index);
        }
    }

    /// Write a u8 field.
    pub fn write_u8(&mut self, loc: &FieldLoc, val: u8) {
        let off = self.abs_offset(loc);
        if off >= self.buf.len() {
            return;
        }
        self.buf[off] = val;
    }

    /// Write an i8 field.
    pub fn write_i8(&mut self, loc: &FieldLoc, val: i8) {
        self.write_u8(loc, val as u8);
    }

    /// Write a u16 field.
    pub fn write_u16(&mut self, loc: &FieldLoc, val: u16) {
        let off = self.abs_offset(loc);
        if off + 2 > self.buf.len() {
            return;
        }
        self.buf[off..off + 2].copy_from_slice(&val.to_le_bytes());
    }

    /// Write an i16 field.
    pub fn write_i16(&mut self, loc: &FieldLoc, val: i16) {
        self.write_u16(loc, val as u16);
    }

    /// Write a u32 field.
    pub fn write_u32(&mut self, loc: &FieldLoc, val: u32) {
        let off = self.abs_offset(loc);
        if off + 4 > self.buf.len() {
            return;
        }
        self.buf[off..off + 4].copy_from_slice(&val.to_le_bytes());
    }

    /// Write an i32 field.
    pub fn write_i32(&mut self, loc: &FieldLoc, val: i32) {
        self.write_u32(loc, val as u32);
    }

    /// Write a u64 field.
    pub fn write_u64(&mut self, loc: &FieldLoc, val: u64) {
        let off = self.abs_offset(loc);
        if off + 8 > self.buf.len() {
            return;
        }
        self.buf[off..off + 8].copy_from_slice(&val.to_le_bytes());
    }

    /// Write an i64 field.
    pub fn write_i64(&mut self, loc: &FieldLoc, val: i64) {
        self.write_u64(loc, val as u64);
    }

    /// Write an f32 field.
    pub fn write_f32(&mut self, loc: &FieldLoc, val: f32) {
        let off = self.abs_offset(loc);
        if off + 4 > self.buf.len() {
            return;
        }
        self.buf[off..off + 4].copy_from_slice(&val.to_le_bytes());
    }

    /// Write an f64 field.
    pub fn write_f64(&mut self, loc: &FieldLoc, val: f64) {
        let off = self.abs_offset(loc);
        if off + 8 > self.buf.len() {
            return;
        }
        self.buf[off..off + 8].copy_from_slice(&val.to_le_bytes());
    }

    // ── Read-only access to text (pointer section) ──────────────────────

    /// Read a text field (read-only; strings live in the pointer section
    /// and cannot be resized in-place).
    pub fn read_text(&self, loc: &FieldLoc) -> &str {
        debug_assert!(loc.is_ptr, "read_text expects a pointer field");
        let segment = &self.buf[self.seg_start..];
        let slot_offset = self.data_offset
            + self.data_words as usize * 8
            + loc.offset as usize * 8;
        wire::read_text(segment, slot_offset)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::capnp::layout::{CapnpLayout, FieldKind, MemberDesc};
    use crate::capnp::pack::{pack_bool_field, pack_data_field, WordBuf};
    use crate::capnp::view::CapnpView;

    // Helper: build a capnp message for struct { a: u32, b: u64 }
    fn make_u32_u64_msg(a: u32, b: u64) -> (Vec<u8>, CapnpLayout) {
        let layout = CapnpLayout::compute(&[
            MemberDesc::Simple(FieldKind::Scalar(4)),
            MemberDesc::Simple(FieldKind::Scalar(8)),
        ]);
        let mut buf = WordBuf::new();
        let root_ptr = buf.alloc(1);
        let data_start = buf.alloc(layout.data_words as u32);
        let _ptrs_start = buf.alloc(layout.ptr_count as u32);
        buf.write_struct_ptr(root_ptr, data_start, layout.data_words, layout.ptr_count);
        pack_data_field(&mut buf, data_start, &layout.fields[0], a);
        pack_data_field(&mut buf, data_start, &layout.fields[1], b);
        (buf.finish(), layout)
    }

    #[test]
    fn capnp_mut_u32_roundtrip() {
        let (mut msg, layout) = make_u32_u64_msg(42, 100);
        {
            let mut m = CapnpMut::from_message(&mut msg).unwrap();
            assert_eq!(m.read_u32(&layout.fields[0]), 42);
            m.write_u32(&layout.fields[0], 99);
            assert_eq!(m.read_u32(&layout.fields[0]), 99);
        }
        // Verify via CapnpView
        let view = CapnpView::from_message(&msg).unwrap();
        assert_eq!(view.read_u32(&layout.fields[0]), 99);
    }

    #[test]
    fn capnp_mut_u64_roundtrip() {
        let (mut msg, layout) = make_u32_u64_msg(1, 0xDEAD_BEEF_CAFE);
        {
            let mut m = CapnpMut::from_message(&mut msg).unwrap();
            assert_eq!(m.read_u64(&layout.fields[1]), 0xDEAD_BEEF_CAFE);
            m.write_u64(&layout.fields[1], 0x1234_5678_9ABC_DEF0);
        }
        let view = CapnpView::from_message(&msg).unwrap();
        assert_eq!(view.read_u64(&layout.fields[1]), 0x1234_5678_9ABC_DEF0);
    }

    #[test]
    fn capnp_mut_bool_bit_packing() {
        // struct { a: bool, b: bool, c: bool }
        let layout = CapnpLayout::compute(&[
            MemberDesc::Simple(FieldKind::Bool),
            MemberDesc::Simple(FieldKind::Bool),
            MemberDesc::Simple(FieldKind::Bool),
        ]);
        let mut buf = WordBuf::new();
        let root_ptr = buf.alloc(1);
        let data_start = buf.alloc(layout.data_words as u32);
        buf.write_struct_ptr(root_ptr, data_start, layout.data_words, layout.ptr_count);
        pack_bool_field(&mut buf, data_start, &layout.fields[0], true);
        pack_bool_field(&mut buf, data_start, &layout.fields[1], false);
        pack_bool_field(&mut buf, data_start, &layout.fields[2], true);
        let mut msg = buf.finish();

        {
            let mut m = CapnpMut::from_message(&mut msg).unwrap();
            assert_eq!(m.read_bool(&layout.fields[0]), true);
            assert_eq!(m.read_bool(&layout.fields[1]), false);
            assert_eq!(m.read_bool(&layout.fields[2]), true);

            // Flip b to true and a to false
            m.write_bool(&layout.fields[0], false);
            m.write_bool(&layout.fields[1], true);

            assert_eq!(m.read_bool(&layout.fields[0]), false);
            assert_eq!(m.read_bool(&layout.fields[1]), true);
            assert_eq!(m.read_bool(&layout.fields[2]), true); // unchanged
        }

        let view = CapnpView::from_message(&msg).unwrap();
        assert_eq!(view.read_bool(&layout.fields[0]), false);
        assert_eq!(view.read_bool(&layout.fields[1]), true);
        assert_eq!(view.read_bool(&layout.fields[2]), true);
    }

    #[test]
    fn capnp_mut_f64() {
        let layout = CapnpLayout::compute(&[
            MemberDesc::Simple(FieldKind::Scalar(8)),
        ]);
        let mut buf = WordBuf::new();
        let root_ptr = buf.alloc(1);
        let data_start = buf.alloc(layout.data_words as u32);
        buf.write_struct_ptr(root_ptr, data_start, layout.data_words, layout.ptr_count);
        pack_data_field(&mut buf, data_start, &layout.fields[0], 3.14f64);
        let mut msg = buf.finish();

        {
            let mut m = CapnpMut::from_message(&mut msg).unwrap();
            let v = m.read_f64(&layout.fields[0]);
            assert!((v - 3.14).abs() < 1e-10);
            m.write_f64(&layout.fields[0], std::f64::consts::E);
        }

        let view = CapnpView::from_message(&msg).unwrap();
        assert_eq!(view.read_f64(&layout.fields[0]), std::f64::consts::E);
    }

    #[test]
    fn capnp_mut_i16() {
        let layout = CapnpLayout::compute(&[
            MemberDesc::Simple(FieldKind::Scalar(2)),
        ]);
        let mut buf = WordBuf::new();
        let root_ptr = buf.alloc(1);
        let data_start = buf.alloc(layout.data_words as u32);
        buf.write_struct_ptr(root_ptr, data_start, layout.data_words, layout.ptr_count);
        pack_data_field(&mut buf, data_start, &layout.fields[0], -42i16);
        let mut msg = buf.finish();

        {
            let mut m = CapnpMut::from_message(&mut msg).unwrap();
            assert_eq!(m.read_i16(&layout.fields[0]), -42);
            m.write_i16(&layout.fields[0], 1000);
            assert_eq!(m.read_i16(&layout.fields[0]), 1000);
        }

        let view = CapnpView::from_message(&msg).unwrap();
        assert_eq!(view.read_i16(&layout.fields[0]), 1000);
    }
}
