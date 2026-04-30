//! Zero-copy Cap'n Proto view.
//!
//! `CapnpView` reads fields directly from borrowed bytes without
//! deserialization or allocation. Validation is performed on construction
//! to ensure all pointer traversals are safe.

use crate::capnp::layout::FieldLoc;
use crate::capnp::wire::{self, CapnpPtr, ListInfo};

/// A zero-copy view over a Cap'n Proto message.
///
/// Holds a reference to the segment bytes and the resolved root struct pointer.
/// Field access reads directly from the underlying bytes.
#[derive(Debug, Clone, Copy)]
pub struct CapnpView<'a> {
    /// The full segment (everything after the segment table).
    segment: &'a [u8],
    /// The resolved root (or sub-struct) pointer.
    ptr: CapnpPtr,
}

impl<'a> CapnpView<'a> {
    /// Create a view over a complete flat-array message.
    ///
    /// Returns `Err` if the message is invalid.
    pub fn from_message(msg: &'a [u8]) -> Result<Self, &'static str> {
        if !wire::validate(msg) {
            return Err("invalid capnp message");
        }
        let (segment, ptr) = wire::resolve_root(msg)?;
        Ok(CapnpView { segment, ptr })
    }

    /// Create a view from a pre-resolved segment and pointer.
    /// Used internally for nested struct access.
    pub fn from_parts(segment: &'a [u8], ptr: CapnpPtr) -> Self {
        CapnpView { segment, ptr }
    }

    /// Whether this view points to a null struct.
    pub fn is_null(&self) -> bool {
        self.ptr.is_null()
    }

    /// Read a boolean field.
    pub fn read_bool(&self, loc: &FieldLoc) -> bool {
        if loc.is_ptr {
            return false;
        }
        let byte_offset = self.ptr.data_offset + loc.offset as usize;
        if byte_offset >= self.segment.len() {
            return false;
        }
        (self.segment[byte_offset] >> loc.bit_index) & 1 != 0
    }

    /// Read a u8 field.
    pub fn read_u8(&self, loc: &FieldLoc) -> u8 {
        if loc.is_ptr {
            return 0;
        }
        let off = self.ptr.data_offset + loc.offset as usize;
        if off >= self.segment.len() {
            return 0;
        }
        self.segment[off]
    }

    /// Read an i8 field.
    pub fn read_i8(&self, loc: &FieldLoc) -> i8 {
        self.read_u8(loc) as i8
    }

    /// Read a u16 field.
    pub fn read_u16(&self, loc: &FieldLoc) -> u16 {
        if loc.is_ptr {
            return 0;
        }
        let off = self.ptr.data_offset + loc.offset as usize;
        if off + 2 > self.segment.len() {
            return 0;
        }
        wire::read_u16(self.segment, off)
    }

    /// Read an i16 field.
    pub fn read_i16(&self, loc: &FieldLoc) -> i16 {
        self.read_u16(loc) as i16
    }

    /// Read a u32 field.
    pub fn read_u32(&self, loc: &FieldLoc) -> u32 {
        if loc.is_ptr {
            return 0;
        }
        let off = self.ptr.data_offset + loc.offset as usize;
        if off + 4 > self.segment.len() {
            return 0;
        }
        wire::read_u32(self.segment, off)
    }

    /// Read an i32 field.
    pub fn read_i32(&self, loc: &FieldLoc) -> i32 {
        self.read_u32(loc) as i32
    }

    /// Read a u64 field.
    pub fn read_u64(&self, loc: &FieldLoc) -> u64 {
        if loc.is_ptr {
            return 0;
        }
        let off = self.ptr.data_offset + loc.offset as usize;
        if off + 8 > self.segment.len() {
            return 0;
        }
        wire::read_u64(self.segment, off)
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

    /// Read a text (string) pointer field.
    pub fn read_text(&self, loc: &FieldLoc) -> &'a str {
        if !loc.is_ptr {
            return "";
        }
        let slot_offset = self.ptr.ptr_slot_offset(loc.offset);
        wire::read_text(self.segment, slot_offset)
    }

    /// Read a nested struct pointer field.
    pub fn read_struct(&self, loc: &FieldLoc) -> CapnpView<'a> {
        if !loc.is_ptr {
            return CapnpView::from_parts(self.segment, CapnpPtr::NULL);
        }
        let slot_offset = self.ptr.ptr_slot_offset(loc.offset);
        match wire::resolve_struct_ptr(self.segment, slot_offset) {
            Some(child) => CapnpView::from_parts(self.segment, child),
            None => CapnpView::from_parts(self.segment, CapnpPtr::NULL),
        }
    }

    /// Read a list pointer field, returning raw list info for further processing.
    pub fn read_list(&self, loc: &FieldLoc) -> Option<(&'a [u8], ListInfo)> {
        if !loc.is_ptr {
            return None;
        }
        let slot_offset = self.ptr.ptr_slot_offset(loc.offset);
        wire::resolve_list_ptr(self.segment, slot_offset).map(|info| (self.segment, info))
    }

    /// Read a variant discriminant (u16 in data section).
    pub fn read_discriminant(&self, disc_loc: &FieldLoc) -> u16 {
        self.read_u16(disc_loc)
    }

    /// Access the underlying segment.
    pub fn segment(&self) -> &'a [u8] {
        self.segment
    }

    /// Access the resolved pointer.
    pub fn ptr(&self) -> &CapnpPtr {
        &self.ptr
    }
}

/// A zero-copy view over a Cap'n Proto list of scalars.
#[derive(Debug, Clone, Copy)]
pub struct ScalarListView<'a> {
    segment: &'a [u8],
    info: ListInfo,
}

impl<'a> ScalarListView<'a> {
    /// Create a list view from segment and list info.
    pub fn new(segment: &'a [u8], info: ListInfo) -> Self {
        ScalarListView { segment, info }
    }

    /// Number of elements.
    pub fn len(&self) -> usize {
        self.info.count as usize
    }

    /// Whether the list is empty.
    pub fn is_empty(&self) -> bool {
        self.info.count == 0
    }

    /// Read element `i` as a u8.
    pub fn get_u8(&self, i: usize) -> u8 {
        let off = self.info.data_offset + i * self.info.elem_stride as usize;
        self.segment[off]
    }

    /// Read element `i` as a u16.
    pub fn get_u16(&self, i: usize) -> u16 {
        let off = self.info.data_offset + i * self.info.elem_stride as usize;
        wire::read_u16(self.segment, off)
    }

    /// Read element `i` as a u32.
    pub fn get_u32(&self, i: usize) -> u32 {
        let off = self.info.data_offset + i * self.info.elem_stride as usize;
        wire::read_u32(self.segment, off)
    }

    /// Read element `i` as a u64.
    pub fn get_u64(&self, i: usize) -> u64 {
        let off = self.info.data_offset + i * self.info.elem_stride as usize;
        wire::read_u64(self.segment, off)
    }

    /// Read element `i` as an i32.
    pub fn get_i32(&self, i: usize) -> i32 {
        self.get_u32(i) as i32
    }

    /// Read element `i` as an f32.
    pub fn get_f32(&self, i: usize) -> f32 {
        f32::from_bits(self.get_u32(i))
    }

    /// Read element `i` as an f64.
    pub fn get_f64(&self, i: usize) -> f64 {
        f64::from_bits(self.get_u64(i))
    }

    /// Read element `i` as a bool (from bit list).
    pub fn get_bool(&self, i: usize) -> bool {
        let byte_off = self.info.data_offset + i / 8;
        let bit = i % 8;
        (self.segment[byte_off] >> bit) & 1 != 0
    }
}

/// A zero-copy view over a Cap'n Proto list of text strings.
#[derive(Debug, Clone, Copy)]
pub struct TextListView<'a> {
    segment: &'a [u8],
    info: ListInfo,
}

impl<'a> TextListView<'a> {
    pub fn new(segment: &'a [u8], info: ListInfo) -> Self {
        TextListView { segment, info }
    }

    pub fn len(&self) -> usize {
        self.info.count as usize
    }

    pub fn is_empty(&self) -> bool {
        self.info.count == 0
    }

    pub fn get(&self, i: usize) -> &'a str {
        let ptr_offset = self.info.data_offset + i * 8;
        wire::read_text(self.segment, ptr_offset)
    }
}

/// A zero-copy view over a Cap'n Proto composite list (list of structs).
#[derive(Debug, Clone, Copy)]
pub struct StructListView<'a> {
    segment: &'a [u8],
    info: ListInfo,
}

impl<'a> StructListView<'a> {
    pub fn new(segment: &'a [u8], info: ListInfo) -> Self {
        StructListView { segment, info }
    }

    pub fn len(&self) -> usize {
        self.info.count as usize
    }

    pub fn is_empty(&self) -> bool {
        self.info.count == 0
    }

    /// Get a view of element `i`.
    pub fn get(&self, i: usize) -> CapnpView<'a> {
        let elem_offset = self.info.data_offset + i * self.info.elem_stride as usize;
        CapnpView::from_parts(
            self.segment,
            CapnpPtr {
                data_offset: elem_offset,
                data_words: self.info.elem_data_words,
                ptr_count: self.info.elem_ptr_count,
            },
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::capnp::layout::*;
    use crate::capnp::pack::*;

    // A simple test struct: { a: u32, b: u64, c: bool }
    struct Simple {
        a: u32,
        b: u64,
        c: bool,
    }

    impl CapnpPack for Simple {
        fn member_descs() -> Vec<MemberDesc> {
            vec![
                MemberDesc::Simple(FieldKind::Scalar(4)),
                MemberDesc::Simple(FieldKind::Scalar(8)),
                MemberDesc::Simple(FieldKind::Bool),
            ]
        }

        fn pack_into(&self, buf: &mut WordBuf, data_start: u32, _ptrs_start: u32) {
            let layout = Self::capnp_layout();
            pack_data_field(buf, data_start, &layout.fields[0], self.a);
            pack_data_field(buf, data_start, &layout.fields[1], self.b);
            pack_bool_field(buf, data_start, &layout.fields[2], self.c);
        }
    }

    #[test]
    fn test_simple_view() {
        let s = Simple {
            a: 42,
            b: 0xDEAD_BEEF_CAFE_BABE,
            c: true,
        };
        let msg = s.capnp_pack();
        let view = CapnpView::from_message(&msg).unwrap();
        let layout = Simple::capnp_layout();

        assert_eq!(view.read_u32(&layout.fields[0]), 42);
        assert_eq!(view.read_u64(&layout.fields[1]), 0xDEAD_BEEF_CAFE_BABE);
        assert_eq!(view.read_bool(&layout.fields[2]), true);
    }
}
