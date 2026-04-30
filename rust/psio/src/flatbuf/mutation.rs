//! FlatBuffers in-place scalar mutation.
//!
//! `FbMut<'a>` provides mutable access to scalar fields within a
//! FlatBuffer-encoded buffer. Only scalars can be mutated in-place;
//! strings and vectors are stored via offsets and cannot be resized
//! without rebuilding the buffer.
//!
//! Fields are located via vtable lookup: the vtable maps slot indices
//! to byte offsets within the table data.

/// Mutable view over a FlatBuffer message buffer.
///
/// Allows in-place mutation of scalar fields. Strings and vectors are
/// read-only (they would require reallocation to resize).
pub struct FbMut<'a> {
    buf: &'a mut [u8],
    /// Byte offset of the root table within `buf`.
    table_pos: usize,
    /// Byte offset of the vtable within `buf`.
    vt_pos: usize,
    /// Size of the vtable in bytes.
    vt_size: u16,
}

impl<'a> FbMut<'a> {
    /// Create a mutable view from a complete FlatBuffer (root offset at byte 0).
    ///
    /// Returns `None` if the buffer is too small or has an invalid root.
    pub fn from_buffer(buf: &'a mut [u8]) -> Option<Self> {
        if buf.len() < 4 {
            return None;
        }
        let root_off = u32::from_le_bytes(buf[0..4].try_into().ok()?) as usize;
        Self::from_table(buf, root_off)
    }

    /// Create a mutable view pointing at a specific table position.
    pub fn from_table(buf: &'a mut [u8], table_pos: usize) -> Option<Self> {
        if table_pos + 4 > buf.len() {
            return None;
        }
        let soff = i32::from_le_bytes(buf[table_pos..table_pos + 4].try_into().ok()?);
        let vt_pos = (table_pos as i64 - soff as i64) as usize;
        if vt_pos + 4 > buf.len() {
            return None;
        }
        let vt_size = u16::from_le_bytes(buf[vt_pos..vt_pos + 2].try_into().ok()?);
        Some(FbMut {
            buf,
            table_pos,
            vt_pos,
            vt_size,
        })
    }

    /// Get the field byte offset for a given vtable slot.
    /// Returns `None` if the field is absent (slot beyond vtable or value is 0).
    #[inline]
    fn field_pos(&self, slot: u16) -> Option<usize> {
        let vt_byte = 4 + 2 * slot;
        if vt_byte + 2 > self.vt_size {
            return None;
        }
        let fo = u16::from_le_bytes(
            self.buf[self.vt_pos + vt_byte as usize..self.vt_pos + vt_byte as usize + 2]
                .try_into()
                .unwrap(),
        );
        if fo == 0 {
            None
        } else {
            Some(self.table_pos + fo as usize)
        }
    }

    // ── Scalar reads ────────────────────────────────────────────────────

    /// Read a bool field. Returns `def` if absent.
    pub fn read_bool(&self, slot: u16, def: bool) -> bool {
        match self.field_pos(slot) {
            Some(pos) => self.buf[pos] != 0,
            None => def,
        }
    }

    /// Read a u8 field. Returns `def` if absent.
    pub fn read_u8(&self, slot: u16, def: u8) -> u8 {
        match self.field_pos(slot) {
            Some(pos) => self.buf[pos],
            None => def,
        }
    }

    /// Read an i8 field. Returns `def` if absent.
    pub fn read_i8(&self, slot: u16, def: i8) -> i8 {
        match self.field_pos(slot) {
            Some(pos) => self.buf[pos] as i8,
            None => def,
        }
    }

    /// Read a u16 field. Returns `def` if absent.
    pub fn read_u16(&self, slot: u16, def: u16) -> u16 {
        match self.field_pos(slot) {
            Some(pos) => u16::from_le_bytes(self.buf[pos..pos + 2].try_into().unwrap()),
            None => def,
        }
    }

    /// Read an i16 field. Returns `def` if absent.
    pub fn read_i16(&self, slot: u16, def: i16) -> i16 {
        match self.field_pos(slot) {
            Some(pos) => i16::from_le_bytes(self.buf[pos..pos + 2].try_into().unwrap()),
            None => def,
        }
    }

    /// Read a u32 field. Returns `def` if absent.
    pub fn read_u32(&self, slot: u16, def: u32) -> u32 {
        match self.field_pos(slot) {
            Some(pos) => u32::from_le_bytes(self.buf[pos..pos + 4].try_into().unwrap()),
            None => def,
        }
    }

    /// Read an i32 field. Returns `def` if absent.
    pub fn read_i32(&self, slot: u16, def: i32) -> i32 {
        match self.field_pos(slot) {
            Some(pos) => i32::from_le_bytes(self.buf[pos..pos + 4].try_into().unwrap()),
            None => def,
        }
    }

    /// Read a u64 field. Returns `def` if absent.
    pub fn read_u64(&self, slot: u16, def: u64) -> u64 {
        match self.field_pos(slot) {
            Some(pos) => u64::from_le_bytes(self.buf[pos..pos + 8].try_into().unwrap()),
            None => def,
        }
    }

    /// Read an i64 field. Returns `def` if absent.
    pub fn read_i64(&self, slot: u16, def: i64) -> i64 {
        match self.field_pos(slot) {
            Some(pos) => i64::from_le_bytes(self.buf[pos..pos + 8].try_into().unwrap()),
            None => def,
        }
    }

    /// Read an f32 field. Returns `def` if absent.
    pub fn read_f32(&self, slot: u16, def: f32) -> f32 {
        match self.field_pos(slot) {
            Some(pos) => f32::from_le_bytes(self.buf[pos..pos + 4].try_into().unwrap()),
            None => def,
        }
    }

    /// Read an f64 field. Returns `def` if absent.
    pub fn read_f64(&self, slot: u16, def: f64) -> f64 {
        match self.field_pos(slot) {
            Some(pos) => f64::from_le_bytes(self.buf[pos..pos + 8].try_into().unwrap()),
            None => def,
        }
    }

    // ── Scalar writes ───────────────────────────────────────────────────

    /// Write a bool field. No-op if the field is absent in the vtable.
    pub fn write_bool(&mut self, slot: u16, val: bool) {
        if let Some(pos) = self.field_pos(slot) {
            self.buf[pos] = if val { 1 } else { 0 };
        }
    }

    /// Write a u8 field.
    pub fn write_u8(&mut self, slot: u16, val: u8) {
        if let Some(pos) = self.field_pos(slot) {
            self.buf[pos] = val;
        }
    }

    /// Write an i8 field.
    pub fn write_i8(&mut self, slot: u16, val: i8) {
        if let Some(pos) = self.field_pos(slot) {
            self.buf[pos] = val as u8;
        }
    }

    /// Write a u16 field.
    pub fn write_u16(&mut self, slot: u16, val: u16) {
        if let Some(pos) = self.field_pos(slot) {
            self.buf[pos..pos + 2].copy_from_slice(&val.to_le_bytes());
        }
    }

    /// Write an i16 field.
    pub fn write_i16(&mut self, slot: u16, val: i16) {
        if let Some(pos) = self.field_pos(slot) {
            self.buf[pos..pos + 2].copy_from_slice(&val.to_le_bytes());
        }
    }

    /// Write a u32 field.
    pub fn write_u32(&mut self, slot: u16, val: u32) {
        if let Some(pos) = self.field_pos(slot) {
            self.buf[pos..pos + 4].copy_from_slice(&val.to_le_bytes());
        }
    }

    /// Write an i32 field.
    pub fn write_i32(&mut self, slot: u16, val: i32) {
        if let Some(pos) = self.field_pos(slot) {
            self.buf[pos..pos + 4].copy_from_slice(&val.to_le_bytes());
        }
    }

    /// Write a u64 field.
    pub fn write_u64(&mut self, slot: u16, val: u64) {
        if let Some(pos) = self.field_pos(slot) {
            self.buf[pos..pos + 8].copy_from_slice(&val.to_le_bytes());
        }
    }

    /// Write an i64 field.
    pub fn write_i64(&mut self, slot: u16, val: i64) {
        if let Some(pos) = self.field_pos(slot) {
            self.buf[pos..pos + 8].copy_from_slice(&val.to_le_bytes());
        }
    }

    /// Write an f32 field.
    pub fn write_f32(&mut self, slot: u16, val: f32) {
        if let Some(pos) = self.field_pos(slot) {
            self.buf[pos..pos + 4].copy_from_slice(&val.to_le_bytes());
        }
    }

    /// Write an f64 field.
    pub fn write_f64(&mut self, slot: u16, val: f64) {
        if let Some(pos) = self.field_pos(slot) {
            self.buf[pos..pos + 8].copy_from_slice(&val.to_le_bytes());
        }
    }

    // ── Read-only string access ─────────────────────────────────────────

    /// Read a string field (read-only; strings cannot be resized in-place).
    /// Returns `None` if absent.
    pub fn read_str(&self, slot: u16) -> Option<&str> {
        let pos = self.field_pos(slot)?;
        let str_pos = pos + u32::from_le_bytes(
            self.buf[pos..pos + 4].try_into().ok()?
        ) as usize;
        if str_pos + 4 > self.buf.len() {
            return None;
        }
        let len = u32::from_le_bytes(
            self.buf[str_pos..str_pos + 4].try_into().ok()?
        ) as usize;
        if str_pos + 4 + len > self.buf.len() {
            return None;
        }
        std::str::from_utf8(&self.buf[str_pos + 4..str_pos + 4 + len]).ok()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::flatbuf::builder::{FbBuilder, FbTableWriter};
    use crate::flatbuf::layout::FbLayout;
    use crate::flatbuf::view::FbView;

    #[test]
    fn fb_mut_u32_roundtrip() {
        // Build a table with one u32 field at slot 0
        let layout = FbLayout::simple(1);
        let mut b = FbBuilder::default();
        let mut tw = FbTableWriter::new(&mut b, &layout);
        tw.add_u32(0, 42, 0);
        let root = tw.finish();
        b.finish(root);
        let mut buf = b.data().to_vec();

        {
            let mut m = FbMut::from_buffer(&mut buf).unwrap();
            assert_eq!(m.read_u32(0, 0), 42);
            m.write_u32(0, 99);
            assert_eq!(m.read_u32(0, 0), 99);
        }

        // Verify via FbView
        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_u32(0, 0), 99);
    }

    #[test]
    fn fb_mut_u64_roundtrip() {
        let layout = FbLayout::simple(1);
        let mut b = FbBuilder::default();
        let mut tw = FbTableWriter::new(&mut b, &layout);
        tw.add_u64(0, 0xDEAD_BEEF_CAFE, 0);
        let root = tw.finish();
        b.finish(root);
        let mut buf = b.data().to_vec();

        {
            let mut m = FbMut::from_buffer(&mut buf).unwrap();
            assert_eq!(m.read_u64(0, 0), 0xDEAD_BEEF_CAFE);
            m.write_u64(0, 0x1234_5678_9ABC_DEF0);
        }

        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_u64(0, 0), 0x1234_5678_9ABC_DEF0);
    }

    #[test]
    fn fb_mut_bool_roundtrip() {
        // Both bools must be non-default so they are present in the vtable.
        // FlatBuffers omits fields that equal their default value.
        let layout = FbLayout::simple(2);
        let mut b = FbBuilder::default();
        let mut tw = FbTableWriter::new(&mut b, &layout);
        tw.add_bool(0, true, false);
        tw.add_bool(1, true, false);
        let root = tw.finish();
        b.finish(root);
        let mut buf = b.data().to_vec();

        {
            let mut m = FbMut::from_buffer(&mut buf).unwrap();
            assert_eq!(m.read_bool(0, false), true);
            assert_eq!(m.read_bool(1, false), true);
            m.write_bool(0, false);
            assert_eq!(m.read_bool(0, false), false);
            assert_eq!(m.read_bool(1, false), true); // unchanged
        }

        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_bool(0, false), false);
        assert_eq!(view.read_bool(1, false), true);
    }

    #[test]
    fn fb_mut_f64_roundtrip() {
        let layout = FbLayout::simple(1);
        let mut b = FbBuilder::default();
        let mut tw = FbTableWriter::new(&mut b, &layout);
        tw.add_f64(0, 3.14, 0.0);
        let root = tw.finish();
        b.finish(root);
        let mut buf = b.data().to_vec();

        {
            let mut m = FbMut::from_buffer(&mut buf).unwrap();
            let v = m.read_f64(0, 0.0);
            assert!((v - 3.14).abs() < 1e-10);
            m.write_f64(0, std::f64::consts::E);
        }

        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_f64(0, 0.0), std::f64::consts::E);
    }

    #[test]
    fn fb_mut_multiple_fields() {
        // Token { kind: u16, text: String }
        // Mutate kind, verify text unchanged via FbView
        let layout = FbLayout::simple(2);
        let mut b = FbBuilder::default();
        let text_off = b.create_string("hello");

        let mut tw = FbTableWriter::new(&mut b, &layout);
        tw.add_u16(0, 7, 0);
        tw.add_offset(1, text_off);
        let root = tw.finish();
        b.finish(root);
        let mut buf = b.data().to_vec();

        {
            let mut m = FbMut::from_buffer(&mut buf).unwrap();
            assert_eq!(m.read_u16(0, 0), 7);
            m.write_u16(0, 42);
        }

        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_u16(0, 0), 42);
        assert_eq!(view.read_str(1), Some("hello"));
    }

    #[test]
    fn fb_mut_i16() {
        let layout = FbLayout::simple(1);
        let mut b = FbBuilder::default();
        let mut tw = FbTableWriter::new(&mut b, &layout);
        tw.add_i16(0, -42, 0);
        let root = tw.finish();
        b.finish(root);
        let mut buf = b.data().to_vec();

        {
            let mut m = FbMut::from_buffer(&mut buf).unwrap();
            assert_eq!(m.read_i16(0, 0), -42);
            m.write_i16(0, 1000);
            assert_eq!(m.read_i16(0, 0), 1000);
        }

        let view = FbView::from_buffer(&buf).unwrap();
        assert_eq!(view.read_i16(0, 0), 1000);
    }
}
