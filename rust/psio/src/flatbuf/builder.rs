//! FlatBuffers builder — constructs FlatBuffer messages back-to-front.
//!
//! The builder grows a buffer from the end toward the beginning (high addresses
//! to low), matching the official FlatBuffers builder strategy. Sub-objects
//! (strings, vectors, nested tables) are written first, then the table that
//! references them is built with relative offsets.
//!
//! # Wire format summary
//!
//! - **Root**: `[root_offset: u32]` at buffer start, pointing to the root table.
//! - **Table**: `[soffset_to_vtable: i32, field_data...]`
//! - **Vtable**: `[vt_size: u16, tbl_size: u16, field_offsets: u16...]`
//! - **String**: `[len: u32, utf8_bytes..., NUL]`
//! - **Vector**: `[len: u32, elements...]`

use super::layout::FbLayout;

/// Records the position and vtable byte-offset of one field within a table
/// being constructed.
#[derive(Clone, Copy)]
struct FieldLoc {
    /// Absolute offset from the end of the buffer where this field's data starts.
    off: u32,
    /// Byte offset into the vtable (4 + 2 * slot_index).
    vt: u16,
}

/// FlatBuffer builder that constructs messages back-to-front.
///
pub struct FbBuilder {
    buf: Vec<u8>,
    head: usize, // write cursor, starts at capacity, grows toward 0
    min_align: usize,

    // Table construction state
    fields: Vec<FieldLoc>,
    tbl_start: u32,
    // The vtable being written, kept for its allocation.
    vt_scratch: Vec<u8>,

    // The vtables written so far (their offsets from the end of the buffer):
    // a table whose vtable has the same bytes as an earlier one shares it,
    // as the official builder and the C++ encoder do.
    vtables: Vec<u32>,
}

impl FbBuilder {
    /// Create a new builder with the given initial capacity.
    pub fn new(initial_capacity: usize) -> Self {
        let cap = initial_capacity.max(64);
        FbBuilder {
            buf: vec![0u8; cap],
            head: cap,
            min_align: 1,
            fields: Vec::new(),
            vt_scratch: Vec::new(),
            tbl_start: 0,
            vtables: Vec::new(),
        }
    }

    /// Current size of written data.
    #[inline]
    fn sz(&self) -> usize {
        self.buf.len() - self.head
    }

    /// Grow the buffer to accommodate `needed` more bytes.
    fn grow(&mut self, needed: usize) {
        let old_cap = self.buf.len();
        let new_cap = (old_cap * 2).max(old_cap + needed);
        let tail = self.sz();
        assert!(
            needed <= super::verify::MAX_BUFFER_SIZE - tail.min(super::verify::MAX_BUFFER_SIZE),
            "flatbuf: the buffer would exceed 2 GiB"
        );
        let mut new_buf = vec![0u8; new_cap];
        new_buf[new_cap - tail..].copy_from_slice(&self.buf[self.head..]);
        self.buf = new_buf;
        self.head = new_cap - tail;
    }

    /// Allocate `n` bytes at the front of the written region, returning a
    /// mutable slice to write into.
    fn alloc(&mut self, n: usize) -> &mut [u8] {
        if n > self.head {
            self.grow(n);
        }
        self.head -= n;
        &mut self.buf[self.head..self.head + n]
    }

    /// Write `n` zero bytes.
    fn zero_pad(&mut self, n: usize) {
        let slice = self.alloc(n);
        // alloc returns from a vec initialized to 0, but we should be safe:
        for b in slice.iter_mut() {
            *b = 0;
        }
    }

    fn track(&mut self, a: usize) {
        if a > self.min_align {
            self.min_align = a;
        }
    }

    /// Align the write cursor to `a` (must be a power of 2).
    fn align(&mut self, a: usize) {
        let p = (!self.sz()).wrapping_add(1) & (a - 1);
        if p > 0 {
            self.zero_pad(p);
        }
        self.track(a);
    }

    /// Pre-align: ensure that after writing `len` bytes, the cursor will be
    /// aligned to `a`.
    fn pre_align(&mut self, len: usize, a: usize) {
        if len == 0 {
            return;
        }
        let p = (!(self.sz() + len)).wrapping_add(1) & (a - 1);
        if p > 0 {
            self.zero_pad(p);
        }
        self.track(a);
    }

    /// Push a little-endian value onto the buffer.
    fn push_u8(&mut self, v: u8) {
        self.alloc(1)[0] = v;
    }

    fn push_u16(&mut self, v: u16) {
        let bytes = v.to_le_bytes();
        self.alloc(2).copy_from_slice(&bytes);
    }

    fn push_u32(&mut self, v: u32) {
        let bytes = v.to_le_bytes();
        self.alloc(4).copy_from_slice(&bytes);
    }

    fn push_i32(&mut self, v: i32) {
        let bytes = v.to_le_bytes();
        self.alloc(4).copy_from_slice(&bytes);
    }

    fn push_u64(&mut self, v: u64) {
        let bytes = v.to_le_bytes();
        self.alloc(8).copy_from_slice(&bytes);
    }

    fn push_i64(&mut self, v: i64) {
        let bytes = v.to_le_bytes();
        self.alloc(8).copy_from_slice(&bytes);
    }

    fn push_f32(&mut self, v: f32) {
        let bytes = v.to_le_bytes();
        self.alloc(4).copy_from_slice(&bytes);
    }

    fn push_f64(&mut self, v: f64) {
        let bytes = v.to_le_bytes();
        self.alloc(8).copy_from_slice(&bytes);
    }

    // ── Sub-object creation ──────────────────────────────────────────────

    /// Write a string (length-prefixed UTF-8 with NUL terminator).
    /// Returns the offset (from end of buffer) for later reference.
    pub fn create_string(&mut self, s: &str) -> u32 {
        self.create_bytes_string(s.as_bytes())
    }

    /// Write a string of any bytes (FlatBuffers does not require UTF-8).
    pub fn create_bytes_string(&mut self, s: &[u8]) -> u32 {
        let len = s.len();
        self.pre_align(len + 1, 4);
        // NUL terminator
        self.push_u8(0);
        // String bytes
        self.alloc(len).copy_from_slice(s);
        // Length prefix
        self.align(4);
        self.push_u32(len as u32);
        self.sz() as u32
    }

    /// Write a vector of scalar values. Returns the offset from end of buffer.
    pub fn create_vec_scalar(&mut self, data: &[u8], elem_size: usize, count: usize) -> u32 {
        let body = count * elem_size;
        self.pre_align(body, 4);
        self.pre_align(body, elem_size);
        if body > 0 {
            let dest = self.alloc(body);
            dest.copy_from_slice(&data[..body]);
        }
        self.align(4);
        self.push_u32(count as u32);
        self.sz() as u32
    }

    /// Write a vector of offsets (for vectors of strings/tables).
    /// `offs` contains offsets from end-of-buffer for each sub-object.
    pub fn create_vec_offsets(&mut self, offs: &[u32]) -> u32 {
        let count = offs.len();
        self.pre_align(count * 4, 4);
        for i in (0..count).rev() {
            self.align(4);
            let rel = self.sz() as u32 - offs[i] + 4;
            self.push_u32(rel);
        }
        self.align(4);
        self.push_u32(count as u32);
        self.sz() as u32
    }

    /// Starts a vector of `count` elements of `elem_size` bytes aligned to
    /// `elem_align`: returns their storage, zeroed, for the caller to fill.
    /// [`end_vector`](Self::end_vector) writes the length.
    pub fn start_vector(&mut self, count: usize, elem_size: usize, elem_align: usize) -> &mut [u8] {
        let body = count * elem_size;
        self.pre_align(body, 4);
        self.pre_align(body, elem_align);
        let dest = self.alloc(body);
        dest.fill(0);
        dest
    }

    /// Finishes a vector begun with [`start_vector`](Self::start_vector).
    /// Returns its offset from the end of the buffer.
    pub fn end_vector(&mut self, count: usize) -> u32 {
        self.align(4);
        self.push_u32(count as u32);
        self.sz() as u32
    }

    /// Adds a field of `size` bytes aligned to `align` to the table under
    /// construction — a scalar or a struct — and returns its storage,
    /// zeroed. `vt` is the field's position in the vtable (4 + 2 × slot).
    pub fn add_inline(&mut self, vt: u16, align: usize, size: usize) -> &mut [u8] {
        self.align(align);
        self.alloc(size).fill(0);
        self.record_field(vt);
        let head = self.head;
        &mut self.buf[head..head + size]
    }

    // ── Table construction ───────────────────────────────────────────────

    /// Begin constructing a new table.
    pub fn start_table(&mut self) {
        self.fields.clear();
        self.tbl_start = self.sz() as u32;
    }

    /// Add a scalar field, omitting it if it equals the default value.
    pub fn add_scalar_u8(&mut self, vt: u16, val: u8, def: u8) {
        if val == def {
            return;
        }
        self.align(1);
        self.push_u8(val);
        self.record_field(vt);
    }

    /// Add a scalar field, always written (for optional scalars that are present).
    pub fn add_scalar_u8_force(&mut self, vt: u16, val: u8) {
        self.align(1);
        self.push_u8(val);
        self.record_field(vt);
    }

    pub fn add_scalar_i8(&mut self, vt: u16, val: i8, def: i8) {
        if val == def {
            return;
        }
        self.align(1);
        self.push_u8(val as u8);
        self.record_field(vt);
    }

    pub fn add_scalar_u16(&mut self, vt: u16, val: u16, def: u16) {
        if val == def {
            return;
        }
        self.align(2);
        self.push_u16(val);
        self.record_field(vt);
    }

    pub fn add_scalar_i16(&mut self, vt: u16, val: i16, def: i16) {
        if val == def {
            return;
        }
        self.align(2);
        self.push_i16(val);
        self.record_field(vt);
    }

    fn push_i16(&mut self, v: i16) {
        let bytes = v.to_le_bytes();
        self.alloc(2).copy_from_slice(&bytes);
    }

    pub fn add_scalar_u32(&mut self, vt: u16, val: u32, def: u32) {
        if val == def {
            return;
        }
        self.align(4);
        self.push_u32(val);
        self.record_field(vt);
    }

    pub fn add_scalar_i32(&mut self, vt: u16, val: i32, def: i32) {
        if val == def {
            return;
        }
        self.align(4);
        self.push_i32(val);
        self.record_field(vt);
    }

    pub fn add_scalar_u64(&mut self, vt: u16, val: u64, def: u64) {
        if val == def {
            return;
        }
        self.align(8);
        self.push_u64(val);
        self.record_field(vt);
    }

    pub fn add_scalar_i64(&mut self, vt: u16, val: i64, def: i64) {
        if val == def {
            return;
        }
        self.align(8);
        self.push_i64(val);
        self.record_field(vt);
    }

    pub fn add_scalar_f32(&mut self, vt: u16, val: f32, def: f32) {
        if val.to_bits() == def.to_bits() {
            return;
        }
        self.align(4);
        self.push_f32(val);
        self.record_field(vt);
    }

    pub fn add_scalar_f64(&mut self, vt: u16, val: f64, def: f64) {
        if val.to_bits() == def.to_bits() {
            return;
        }
        self.align(8);
        self.push_f64(val);
        self.record_field(vt);
    }

    pub fn add_scalar_bool(&mut self, vt: u16, val: bool, def: bool) {
        self.add_scalar_u8(vt, val as u8, def as u8);
    }

    /// Add an offset field (string, vector, or nested table).
    /// `off` is the offset from end-of-buffer returned by create_string etc.
    /// A zero offset means "absent" and the field is omitted.
    pub fn add_offset_field(&mut self, vt: u16, off: u32) {
        if off == 0 {
            return;
        }
        self.align(4);
        let rel = self.sz() as u32 - off + 4;
        self.push_u32(rel);
        self.record_field(vt);
    }

    fn record_field(&mut self, vt: u16) {
        self.fields.push(FieldLoc { off: self.sz() as u32, vt });
    }

    /// Finish the current table, writing the vtable and patching the soffset.
    /// Returns the offset from end-of-buffer for this table.
    pub fn end_table(&mut self) -> u32 {
        // Write the placeholder soffset (will be patched)
        self.align(4);
        self.push_i32(0);
        let tbl_off = self.sz() as u32;

        // Compute vtable size
        let mut max_vt: u16 = 0;
        for f in &self.fields {
            if f.vt > max_vt {
                max_vt = f.vt;
            }
        }
        let vt_size = (max_vt + 2).max(4);
        let object = tbl_off - self.tbl_start;
        assert!(object <= 0xffff, "flatbuf: a table's inline data exceeds 64 KiB");
        let tbl_obj_sz = object as u16;

        // The vtable is built aside first: it is compared with the earlier
        // ones before it is known whether it stays in the buffer.
        let vt_bytes = vt_size as usize;
        let mut vt_buf = std::mem::take(&mut self.vt_scratch);
        vt_buf.clear();
        vt_buf.resize(vt_bytes, 0);
        vt_buf[0..2].copy_from_slice(&vt_size.to_le_bytes());
        vt_buf[2..4].copy_from_slice(&tbl_obj_sz.to_le_bytes());
        for f in &self.fields {
            let fo = (tbl_off - f.off) as u16;
            let slot_off = f.vt as usize;
            if slot_off + 2 <= vt_bytes {
                vt_buf[slot_off..slot_off + 2].copy_from_slice(&fo.to_le_bytes());
            }
        }

        // Now write the vtable into the buffer
        let dest = self.alloc(vt_bytes);
        dest.copy_from_slice(&vt_buf);

        let mut vt_off = self.sz() as u32;

        // Share an identical vtable written earlier.
        let len = self.buf.len();
        let shared = self.vtables.iter().copied().find(|&off| {
            let at = len - off as usize;
            self.buf[at..].len() >= vt_bytes && self.buf[at..at + vt_bytes] == vt_buf[..]
        });
        match shared {
            Some(off) => {
                self.head += vt_bytes;
                vt_off = off;
            }
            None => self.vtables.push(vt_off),
        }

        // Patch the soffset in the table to point to the vtable
        // soffset = vtable_offset - table_offset (as seen from table position)
        let soff = vt_off as i32 - tbl_off as i32;
        let tbl_pos = self.buf.len() - tbl_off as usize;
        self.buf[tbl_pos..tbl_pos + 4].copy_from_slice(&soff.to_le_bytes());
        self.vt_scratch = vt_buf;

        tbl_off
    }

    /// Finish the buffer by writing the root offset.
    pub fn finish(&mut self, root: u32) {
        self.pre_align(4, self.min_align);
        self.align(4);
        let rel = self.sz() as u32 - root + 4;
        self.push_u32(rel);
    }

    /// Finishes the buffer with a file identifier and / or a size prefix.
    pub fn finish_with(&mut self, root: u32, opts: &super::verify::FbOptions) {
        let prefix = if opts.size_prefixed { 4 } else { 0 };
        self.track(prefix);
        let ident = if opts.file_identifier.is_some() { 4 } else { 0 };
        self.pre_align(prefix + 4 + ident, self.min_align);
        if let Some(id) = opts.file_identifier {
            self.alloc(4).copy_from_slice(&id);
        }
        self.align(4);
        let rel = self.sz() as u32 - root + 4;
        self.push_u32(rel);
        if opts.size_prefixed {
            let size = self.sz() as u32;
            self.push_u32(size);
        }
    }

    /// Get the finished buffer data.
    pub fn data(&self) -> &[u8] {
        &self.buf[self.head..]
    }

    /// Consume the builder and return the finished buffer as a Vec<u8>.
    pub fn into_vec(self) -> Vec<u8> {
        let head = self.head;
        let mut buf = self.buf;
        // Move data to start of vec and truncate
        let len = buf.len() - head;
        buf.copy_within(head.., 0);
        buf.truncate(len);
        buf
    }

    /// Reset the builder for reuse (keeps allocation).
    pub fn clear(&mut self) {
        self.head = self.buf.len();
        self.min_align = 1;
        self.fields.clear();
        self.tbl_start = 0;
        self.vtables.clear();
    }
}

impl Default for FbBuilder {
    fn default() -> Self {
        Self::new(1024)
    }
}

// ── Multi-field table helper ─────────────────────────────────────────────

/// Helper for building multi-field tables without derive macros.
///
/// Usage pattern:
/// 1. Pre-create sub-objects (strings, vectors, nested tables)
/// 2. Call `builder.start_table()`
/// 3. Add fields in alignment order (smallest first for back-to-front packing)
/// 4. Call `builder.end_table()`
/// 5. Call `builder.finish(root_off)`
///
/// See the tests module for examples.
pub struct FbTableWriter<'a> {
    builder: &'a mut FbBuilder,
    layout: &'a FbLayout,
}

impl<'a> FbTableWriter<'a> {
    pub fn new(builder: &'a mut FbBuilder, layout: &'a FbLayout) -> Self {
        builder.start_table();
        FbTableWriter { builder, layout }
    }

    /// Add a u8 field at the given field index.
    pub fn add_u8(&mut self, field_idx: usize, val: u8, def: u8) {
        let vt = self.layout.field(field_idx).vt_offset();
        self.builder.add_scalar_u8(vt, val, def);
    }

    /// Add a bool field at the given field index.
    pub fn add_bool(&mut self, field_idx: usize, val: bool, def: bool) {
        let vt = self.layout.field(field_idx).vt_offset();
        self.builder.add_scalar_bool(vt, val, def);
    }

    /// Add a u16 field at the given field index.
    pub fn add_u16(&mut self, field_idx: usize, val: u16, def: u16) {
        let vt = self.layout.field(field_idx).vt_offset();
        self.builder.add_scalar_u16(vt, val, def);
    }

    /// Add an i16 field at the given field index.
    pub fn add_i16(&mut self, field_idx: usize, val: i16, def: i16) {
        let vt = self.layout.field(field_idx).vt_offset();
        self.builder.add_scalar_i16(vt, val, def);
    }

    /// Add a u32 field at the given field index.
    pub fn add_u32(&mut self, field_idx: usize, val: u32, def: u32) {
        let vt = self.layout.field(field_idx).vt_offset();
        self.builder.add_scalar_u32(vt, val, def);
    }

    /// Add an i32 field at the given field index.
    pub fn add_i32(&mut self, field_idx: usize, val: i32, def: i32) {
        let vt = self.layout.field(field_idx).vt_offset();
        self.builder.add_scalar_i32(vt, val, def);
    }

    /// Add a u64 field at the given field index.
    pub fn add_u64(&mut self, field_idx: usize, val: u64, def: u64) {
        let vt = self.layout.field(field_idx).vt_offset();
        self.builder.add_scalar_u64(vt, val, def);
    }

    /// Add an i64 field at the given field index.
    pub fn add_i64(&mut self, field_idx: usize, val: i64, def: i64) {
        let vt = self.layout.field(field_idx).vt_offset();
        self.builder.add_scalar_i64(vt, val, def);
    }

    /// Add an f32 field at the given field index.
    pub fn add_f32(&mut self, field_idx: usize, val: f32, def: f32) {
        let vt = self.layout.field(field_idx).vt_offset();
        self.builder.add_scalar_f32(vt, val, def);
    }

    /// Add an f64 field at the given field index.
    pub fn add_f64(&mut self, field_idx: usize, val: f64, def: f64) {
        let vt = self.layout.field(field_idx).vt_offset();
        self.builder.add_scalar_f64(vt, val, def);
    }

    /// Add an offset field (string, vector, sub-table) at the given field index.
    pub fn add_offset(&mut self, field_idx: usize, off: u32) {
        let vt = self.layout.field(field_idx).vt_offset();
        self.builder.add_offset_field(vt, off);
    }

    /// Finish the table and return its offset.
    pub fn finish(self) -> u32 {
        self.builder.end_table()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn flatbuf_builder_scalar_table() {
        let mut b = FbBuilder::default();
        b.start_table();
        b.add_scalar_u32(4, 42, 0);
        let root = b.end_table();
        b.finish(root);

        let data = b.data();
        assert!(data.len() >= 4, "buffer too small: {} bytes", data.len());

        // Root offset at position 0
        let root_off = u32::from_le_bytes(data[0..4].try_into().unwrap());
        let table = root_off as usize;

        // soffset to vtable
        let soff = i32::from_le_bytes(data[table..table + 4].try_into().unwrap());
        let vt_pos = (table as i32 - soff) as usize;

        // Vtable: vt_size, tbl_size
        let vt_size = u16::from_le_bytes(data[vt_pos..vt_pos + 2].try_into().unwrap());
        let _tbl_size = u16::from_le_bytes(data[vt_pos + 2..vt_pos + 4].try_into().unwrap());

        // Field 0 offset (vtable byte 4)
        assert!(vt_size >= 6, "vtable too small for field");
        let field_off = u16::from_le_bytes(data[vt_pos + 4..vt_pos + 6].try_into().unwrap());
        assert_ne!(field_off, 0, "field should be present");

        // Read the field value
        let val =
            u32::from_le_bytes(data[table + field_off as usize..table + field_off as usize + 4].try_into().unwrap());
        assert_eq!(val, 42);
    }

    #[test]
    fn flatbuf_builder_string_table() {
        let mut b = FbBuilder::default();
        let s_off = b.create_string("hello");
        b.start_table();
        b.add_offset_field(4, s_off);
        let root = b.end_table();
        b.finish(root);

        let data = b.data();

        // Navigate to root table
        let root_off = u32::from_le_bytes(data[0..4].try_into().unwrap()) as usize;
        let soff = i32::from_le_bytes(data[root_off..root_off + 4].try_into().unwrap());
        let vt_pos = (root_off as i32 - soff) as usize;

        // Field 0 offset
        let field_off =
            u16::from_le_bytes(data[vt_pos + 4..vt_pos + 6].try_into().unwrap()) as usize;
        assert_ne!(field_off, 0);

        // Dereference the offset to find the string
        let str_rel_pos = root_off + field_off;
        let str_rel =
            u32::from_le_bytes(data[str_rel_pos..str_rel_pos + 4].try_into().unwrap()) as usize;
        let str_pos = str_rel_pos + str_rel;

        // Read string length + data
        let str_len =
            u32::from_le_bytes(data[str_pos..str_pos + 4].try_into().unwrap()) as usize;
        let str_data = &data[str_pos + 4..str_pos + 4 + str_len];
        assert_eq!(std::str::from_utf8(str_data).unwrap(), "hello");
    }

    #[test]
    fn flatbuf_builder_default_omission() {
        // A field equal to its default should be omitted from the vtable
        let mut b = FbBuilder::default();
        b.start_table();
        b.add_scalar_u32(4, 0, 0); // matches default, should be omitted
        b.add_scalar_u32(6, 99, 0); // does not match, should be present
        let root = b.end_table();
        b.finish(root);

        let data = b.data();
        let root_off = u32::from_le_bytes(data[0..4].try_into().unwrap()) as usize;
        let soff = i32::from_le_bytes(data[root_off..root_off + 4].try_into().unwrap());
        let vt_pos = (root_off as i32 - soff) as usize;
        let vt_size =
            u16::from_le_bytes(data[vt_pos..vt_pos + 2].try_into().unwrap()) as usize;

        // Field 0 (vt offset 4): should be 0 (absent)
        let f0 = if vt_size > 5 {
            u16::from_le_bytes(data[vt_pos + 4..vt_pos + 6].try_into().unwrap())
        } else {
            0
        };
        assert_eq!(f0, 0, "default-value field should be absent");

        // Field 1 (vt offset 6): should be non-zero
        let f1 = if vt_size > 7 {
            u16::from_le_bytes(data[vt_pos + 6..vt_pos + 8].try_into().unwrap())
        } else {
            0
        };
        assert_ne!(f1, 0, "non-default field should be present");
    }
}
