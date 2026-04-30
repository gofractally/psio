//! Zero-copy FlatBuffer view types.
//!
//! `FbView` provides zero-copy access to FlatBuffer data. Fields are read
//! lazily via vtable-based lookup:
//!
//! 1. Navigate to the vtable (table - soffset)
//! 2. Read the field offset from the vtable slot
//! 3. If non-zero, read the field data at that offset within the table
//! 4. If zero, the field is absent — return the default value
//!
//! For strings, this returns `&str` pointing directly into the buffer.
//! For vectors, this returns `FbVecView` which provides indexed access.

use std::fmt;

/// Error type for FlatBuffer view operations.
#[derive(Debug, Clone)]
pub enum FbViewError {
    /// Buffer is too small to contain a valid FlatBuffer.
    BufferTooSmall,
    /// Invalid root offset.
    InvalidRootOffset,
    /// Invalid vtable pointer.
    InvalidVtable,
    /// String is not valid UTF-8.
    InvalidUtf8,
    /// Field offset is out of bounds.
    FieldOutOfBounds,
}

impl fmt::Display for FbViewError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            FbViewError::BufferTooSmall => write!(f, "buffer too small for FlatBuffer"),
            FbViewError::InvalidRootOffset => write!(f, "invalid root offset"),
            FbViewError::InvalidVtable => write!(f, "invalid vtable pointer"),
            FbViewError::InvalidUtf8 => write!(f, "string is not valid UTF-8"),
            FbViewError::FieldOutOfBounds => write!(f, "field offset out of bounds"),
        }
    }
}

impl std::error::Error for FbViewError {}

/// Zero-copy view into a FlatBuffer table.
///
/// This holds a reference to the entire buffer and the position of one table
/// within it. Fields are accessed by vtable slot index.
#[derive(Clone, Copy)]
pub struct FbView<'a> {
    /// The entire FlatBuffer.
    buf: &'a [u8],
    /// Byte offset of this table within `buf`.
    table_pos: usize,
    /// Byte offset of this table's vtable within `buf`.
    vt_pos: usize,
    /// Size of the vtable in bytes.
    vt_size: u16,
}

impl<'a> FbView<'a> {
    /// Create a view from a complete FlatBuffer (with root offset at position 0).
    pub fn from_buffer(buf: &'a [u8]) -> Result<Self, FbViewError> {
        if buf.len() < 4 {
            return Err(FbViewError::BufferTooSmall);
        }
        let root_off = read_u32(buf, 0) as usize;
        if root_off >= buf.len() || root_off + 4 > buf.len() {
            return Err(FbViewError::InvalidRootOffset);
        }
        Self::from_table(buf, root_off)
    }

    /// Create a view pointing at a specific table position within the buffer.
    pub fn from_table(buf: &'a [u8], table_pos: usize) -> Result<Self, FbViewError> {
        if table_pos + 4 > buf.len() {
            return Err(FbViewError::InvalidVtable);
        }
        let soff = read_i32(buf, table_pos);
        let vt_pos = (table_pos as i64 - soff as i64) as usize;
        if vt_pos + 4 > buf.len() {
            return Err(FbViewError::InvalidVtable);
        }
        let vt_size = read_u16(buf, vt_pos);
        Ok(FbView {
            buf,
            table_pos,
            vt_pos,
            vt_size,
        })
    }

    /// Get the underlying buffer.
    pub fn buffer(&self) -> &'a [u8] {
        self.buf
    }

    /// Get the field offset for a given vtable byte position.
    /// Returns 0 if the field is absent (slot beyond vtable or value is 0).
    #[inline]
    fn field_offset(&self, vt_byte: u16) -> u16 {
        if vt_byte + 2 <= self.vt_size {
            read_u16(self.buf, self.vt_pos + vt_byte as usize)
        } else {
            0
        }
    }

    /// Read a boolean field. Returns `def` if absent.
    pub fn read_bool(&self, slot: u16, def: bool) -> bool {
        let vt_byte = 4 + 2 * slot;
        let fo = self.field_offset(vt_byte);
        if fo != 0 {
            self.buf[self.table_pos + fo as usize] != 0
        } else {
            def
        }
    }

    /// Read a u8 field. Returns `def` if absent.
    pub fn read_u8(&self, slot: u16, def: u8) -> u8 {
        let vt_byte = 4 + 2 * slot;
        let fo = self.field_offset(vt_byte);
        if fo != 0 {
            self.buf[self.table_pos + fo as usize]
        } else {
            def
        }
    }

    /// Read an i8 field. Returns `def` if absent.
    pub fn read_i8(&self, slot: u16, def: i8) -> i8 {
        let vt_byte = 4 + 2 * slot;
        let fo = self.field_offset(vt_byte);
        if fo != 0 {
            self.buf[self.table_pos + fo as usize] as i8
        } else {
            def
        }
    }

    /// Read a u16 field. Returns `def` if absent.
    pub fn read_u16(&self, slot: u16, def: u16) -> u16 {
        let vt_byte = 4 + 2 * slot;
        let fo = self.field_offset(vt_byte);
        if fo != 0 {
            read_u16(self.buf, self.table_pos + fo as usize)
        } else {
            def
        }
    }

    /// Read an i16 field. Returns `def` if absent.
    pub fn read_i16(&self, slot: u16, def: i16) -> i16 {
        let vt_byte = 4 + 2 * slot;
        let fo = self.field_offset(vt_byte);
        if fo != 0 {
            read_i16(self.buf, self.table_pos + fo as usize)
        } else {
            def
        }
    }

    /// Read a u32 field. Returns `def` if absent.
    pub fn read_u32(&self, slot: u16, def: u32) -> u32 {
        let vt_byte = 4 + 2 * slot;
        let fo = self.field_offset(vt_byte);
        if fo != 0 {
            read_u32(self.buf, self.table_pos + fo as usize)
        } else {
            def
        }
    }

    /// Read an i32 field. Returns `def` if absent.
    pub fn read_i32(&self, slot: u16, def: i32) -> i32 {
        let vt_byte = 4 + 2 * slot;
        let fo = self.field_offset(vt_byte);
        if fo != 0 {
            read_i32(self.buf, self.table_pos + fo as usize)
        } else {
            def
        }
    }

    /// Read a u64 field. Returns `def` if absent.
    pub fn read_u64(&self, slot: u16, def: u64) -> u64 {
        let vt_byte = 4 + 2 * slot;
        let fo = self.field_offset(vt_byte);
        if fo != 0 {
            read_u64(self.buf, self.table_pos + fo as usize)
        } else {
            def
        }
    }

    /// Read an i64 field. Returns `def` if absent.
    pub fn read_i64(&self, slot: u16, def: i64) -> i64 {
        let vt_byte = 4 + 2 * slot;
        let fo = self.field_offset(vt_byte);
        if fo != 0 {
            read_i64(self.buf, self.table_pos + fo as usize)
        } else {
            def
        }
    }

    /// Read an f32 field. Returns `def` if absent.
    pub fn read_f32(&self, slot: u16, def: f32) -> f32 {
        let vt_byte = 4 + 2 * slot;
        let fo = self.field_offset(vt_byte);
        if fo != 0 {
            read_f32(self.buf, self.table_pos + fo as usize)
        } else {
            def
        }
    }

    /// Read an f64 field. Returns `def` if absent.
    pub fn read_f64(&self, slot: u16, def: f64) -> f64 {
        let vt_byte = 4 + 2 * slot;
        let fo = self.field_offset(vt_byte);
        if fo != 0 {
            read_f64(self.buf, self.table_pos + fo as usize)
        } else {
            def
        }
    }

    /// Read a string field. Returns `None` if absent.
    pub fn read_str(&self, slot: u16) -> Option<&'a str> {
        let vt_byte = 4 + 2 * slot;
        let fo = self.field_offset(vt_byte);
        if fo == 0 {
            return None;
        }
        let str_ref_pos = self.table_pos + fo as usize;
        let str_pos = deref_offset(self.buf, str_ref_pos);
        let len = read_u32(self.buf, str_pos) as usize;
        let bytes = &self.buf[str_pos + 4..str_pos + 4 + len];
        // FlatBuffer strings should be valid UTF-8
        std::str::from_utf8(bytes).ok()
    }

    /// Read a string field, returning empty string if absent.
    pub fn read_str_or_empty(&self, slot: u16) -> &'a str {
        self.read_str(slot).unwrap_or("")
    }

    /// Read a nested table field. Returns `None` if absent.
    pub fn read_table(&self, slot: u16) -> Option<FbView<'a>> {
        let vt_byte = 4 + 2 * slot;
        let fo = self.field_offset(vt_byte);
        if fo == 0 {
            return None;
        }
        let ref_pos = self.table_pos + fo as usize;
        let sub_table_pos = deref_offset(self.buf, ref_pos);
        FbView::from_table(self.buf, sub_table_pos).ok()
    }

    /// Read a vector field. Returns `None` if absent.
    pub fn read_vec(&self, slot: u16) -> Option<FbVecView<'a>> {
        let vt_byte = 4 + 2 * slot;
        let fo = self.field_offset(vt_byte);
        if fo == 0 {
            return None;
        }
        let ref_pos = self.table_pos + fo as usize;
        let vec_pos = deref_offset(self.buf, ref_pos);
        let count = read_u32(self.buf, vec_pos);
        Some(FbVecView {
            buf: self.buf,
            data_pos: vec_pos + 4,
            count,
        })
    }

    /// Check if a field is present (vtable slot is non-zero).
    pub fn has_field(&self, slot: u16) -> bool {
        let vt_byte = 4 + 2 * slot;
        self.field_offset(vt_byte) != 0
    }
}

impl<'a> fmt::Debug for FbView<'a> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("FbView")
            .field("table_pos", &self.table_pos)
            .field("vt_pos", &self.vt_pos)
            .field("vt_size", &self.vt_size)
            .finish()
    }
}

/// Zero-copy view into a FlatBuffer vector.
///
/// Elements are accessed by index. The element type determines how each
/// entry is read (scalars inline, strings/tables via offset).
#[derive(Clone, Copy, Debug)]
pub struct FbVecView<'a> {
    buf: &'a [u8],
    /// Byte offset of the first element (right after the length prefix).
    data_pos: usize,
    /// Number of elements.
    count: u32,
}

impl<'a> FbVecView<'a> {
    /// Number of elements in the vector.
    pub fn len(&self) -> u32 {
        self.count
    }

    /// Whether the vector is empty.
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// Read a scalar u8 element at index `i`.
    pub fn get_u8(&self, i: u32) -> u8 {
        self.buf[self.data_pos + i as usize]
    }

    /// Read a scalar i8 element at index `i`.
    pub fn get_i8(&self, i: u32) -> i8 {
        self.buf[self.data_pos + i as usize] as i8
    }

    /// Read a scalar u16 element at index `i`.
    pub fn get_u16(&self, i: u32) -> u16 {
        read_u16(self.buf, self.data_pos + i as usize * 2)
    }

    /// Read a scalar i16 element at index `i`.
    pub fn get_i16(&self, i: u32) -> i16 {
        read_i16(self.buf, self.data_pos + i as usize * 2)
    }

    /// Read a scalar u32 element at index `i`.
    pub fn get_u32(&self, i: u32) -> u32 {
        read_u32(self.buf, self.data_pos + i as usize * 4)
    }

    /// Read a scalar i32 element at index `i`.
    pub fn get_i32(&self, i: u32) -> i32 {
        read_i32(self.buf, self.data_pos + i as usize * 4)
    }

    /// Read a scalar u64 element at index `i`.
    pub fn get_u64(&self, i: u32) -> u64 {
        read_u64(self.buf, self.data_pos + i as usize * 8)
    }

    /// Read a scalar i64 element at index `i`.
    pub fn get_i64(&self, i: u32) -> i64 {
        read_i64(self.buf, self.data_pos + i as usize * 8)
    }

    /// Read a scalar f32 element at index `i`.
    pub fn get_f32(&self, i: u32) -> f32 {
        read_f32(self.buf, self.data_pos + i as usize * 4)
    }

    /// Read a scalar f64 element at index `i`.
    pub fn get_f64(&self, i: u32) -> f64 {
        read_f64(self.buf, self.data_pos + i as usize * 8)
    }

    /// Read a bool element at index `i`.
    pub fn get_bool(&self, i: u32) -> bool {
        self.buf[self.data_pos + i as usize] != 0
    }

    /// Read a string element at index `i` (vector of string offsets).
    pub fn get_str(&self, i: u32) -> Option<&'a str> {
        let entry_pos = self.data_pos + i as usize * 4;
        let str_pos = deref_offset(self.buf, entry_pos);
        let len = read_u32(self.buf, str_pos) as usize;
        let bytes = &self.buf[str_pos + 4..str_pos + 4 + len];
        std::str::from_utf8(bytes).ok()
    }

    /// Read a table element at index `i` (vector of table offsets).
    pub fn get_table(&self, i: u32) -> Option<FbView<'a>> {
        let entry_pos = self.data_pos + i as usize * 4;
        let tbl_pos = deref_offset(self.buf, entry_pos);
        FbView::from_table(self.buf, tbl_pos).ok()
    }
}

// ── Read primitives (little-endian, unaligned-safe) ──────────────────────

#[inline]
fn read_u16(buf: &[u8], pos: usize) -> u16 {
    u16::from_le_bytes(buf[pos..pos + 2].try_into().unwrap())
}

#[inline]
fn read_i16(buf: &[u8], pos: usize) -> i16 {
    i16::from_le_bytes(buf[pos..pos + 2].try_into().unwrap())
}

#[inline]
fn read_u32(buf: &[u8], pos: usize) -> u32 {
    u32::from_le_bytes(buf[pos..pos + 4].try_into().unwrap())
}

#[inline]
fn read_i32(buf: &[u8], pos: usize) -> i32 {
    i32::from_le_bytes(buf[pos..pos + 4].try_into().unwrap())
}

#[inline]
fn read_u64(buf: &[u8], pos: usize) -> u64 {
    u64::from_le_bytes(buf[pos..pos + 8].try_into().unwrap())
}

#[inline]
fn read_i64(buf: &[u8], pos: usize) -> i64 {
    i64::from_le_bytes(buf[pos..pos + 8].try_into().unwrap())
}

#[inline]
fn read_f32(buf: &[u8], pos: usize) -> f32 {
    f32::from_le_bytes(buf[pos..pos + 4].try_into().unwrap())
}

#[inline]
fn read_f64(buf: &[u8], pos: usize) -> f64 {
    f64::from_le_bytes(buf[pos..pos + 8].try_into().unwrap())
}

/// Dereference a u32 offset: position + value at position = target position.
#[inline]
fn deref_offset(buf: &[u8], pos: usize) -> usize {
    pos + read_u32(buf, pos) as usize
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::flatbuf::builder::FbBuilder;

    #[test]
    fn flatbuf_view_scalar_field() {
        let mut b = FbBuilder::default();
        b.start_table();
        b.add_scalar_u32(4, 42, 0); // slot 0
        let root = b.end_table();
        b.finish(root);

        let view = FbView::from_buffer(b.data()).unwrap();
        assert_eq!(view.read_u32(0, 0), 42);
    }

    #[test]
    fn flatbuf_view_absent_field_default() {
        let mut b = FbBuilder::default();
        b.start_table();
        // Add only slot 1, leave slot 0 absent
        b.add_scalar_u32(6, 99, 0); // slot 1
        let root = b.end_table();
        b.finish(root);

        let view = FbView::from_buffer(b.data()).unwrap();
        // Slot 0 is absent, should return default
        assert_eq!(view.read_u32(0, 777), 777);
        // Slot 1 is present
        assert_eq!(view.read_u32(1, 0), 99);
    }

    #[test]
    fn flatbuf_view_string_field() {
        let mut b = FbBuilder::default();
        let s_off = b.create_string("world");
        b.start_table();
        b.add_offset_field(4, s_off); // slot 0
        let root = b.end_table();
        b.finish(root);

        let view = FbView::from_buffer(b.data()).unwrap();
        assert_eq!(view.read_str(0), Some("world"));
    }

    #[test]
    fn flatbuf_view_absent_string() {
        let mut b = FbBuilder::default();
        b.start_table();
        // No string field added
        let root = b.end_table();
        b.finish(root);

        let view = FbView::from_buffer(b.data()).unwrap();
        assert_eq!(view.read_str(0), None);
        assert_eq!(view.read_str_or_empty(0), "");
    }

    #[test]
    fn flatbuf_view_multiple_fields() {
        let mut b = FbBuilder::default();
        let s_off = b.create_string("hello");
        b.start_table();
        // Add fields in reverse alignment order (smallest align first) as
        // the builder writes back-to-front
        b.add_scalar_u8(4, 7, 0);     // slot 0: u8
        b.add_scalar_u16(6, 1000, 0); // slot 1: u16
        b.add_scalar_u32(8, 42, 0);   // slot 2: u32
        b.add_offset_field(10, s_off); // slot 3: string
        let root = b.end_table();
        b.finish(root);

        let view = FbView::from_buffer(b.data()).unwrap();
        assert_eq!(view.read_u8(0, 0), 7);
        assert_eq!(view.read_u16(1, 0), 1000);
        assert_eq!(view.read_u32(2, 0), 42);
        assert_eq!(view.read_str(3), Some("hello"));
    }
}
