//! Cap'n Proto serialization (packing).
//!
//! Builds a single-segment Cap'n Proto flat-array message from Rust values.
//! The wire format produced is compatible with the official capnp compiler's
//! `messageToFlatArray` output for structs with sequential ordinals.

use crate::capnp::layout::{CapnpLayout, FieldLoc, MemberDesc};

/// Word-level buffer for constructing a single-segment Cap'n Proto message.
///
/// Words are u64 (8 bytes). The buffer grows as allocations are made.
/// Pointer offsets are relative to the pointer's own position.
#[derive(Debug)]
pub struct WordBuf {
    words: Vec<u64>,
}

impl WordBuf {
    /// Create an empty word buffer.
    pub fn new() -> Self {
        WordBuf { words: Vec::new() }
    }

    /// Allocate `n` zero-initialized words. Returns the word index of the first allocated word.
    pub fn alloc(&mut self, n: u32) -> u32 {
        let off = self.words.len() as u32;
        self.words.resize(off as usize + n as usize, 0);
        off
    }

    /// Get a mutable byte pointer into the buffer at the given word index.
    /// Write a struct pointer at word `at` pointing to `target` with given data_words and ptr_count.
    pub fn write_struct_ptr(&mut self, at: u32, target: u32, dw: u16, pc: u16) {
        let off = target as i32 - at as i32 - 1;
        let word: u64 = ((pc as u64) << 48)
            | ((dw as u64) << 32)
            | (((off << 2) as u32) as u64);
        self.words[at as usize] = word;
    }

    /// Write a list pointer at word `at` pointing to `target` with given element size tag and count.
    pub fn write_list_ptr(&mut self, at: u32, target: u32, elem_sz: u8, count: u32) {
        let off = target as i32 - at as i32 - 1;
        let word: u64 = ((count as u64) << 35)
            | ((elem_sz as u64) << 32)
            | (((off << 2) as u32) as u64)
            | 1u64;
        self.words[at as usize] = word;
    }

    /// Write a composite list tag word at position `at`.
    pub fn write_composite_tag(&mut self, at: u32, count: u32, dw: u16, pc: u16) {
        let word: u64 =
            ((pc as u64) << 48) | ((dw as u64) << 32) | ((count as u64) << 2);
        self.words[at as usize] = word;
    }

    /// Write a scalar value at `struct_start` word + `byte_offset` bytes.
    pub fn write_bytes(&mut self, struct_start: u32, byte_offset: u32, val: &[u8]) {
        let start = struct_start as usize * 8 + byte_offset as usize;
        let bytes: &mut [u8] = unsafe {
            std::slice::from_raw_parts_mut(self.words.as_mut_ptr() as *mut u8, self.words.len() * 8)
        };
        bytes[start..start + val.len()].copy_from_slice(val);
    }

    /// Write a boolean bit at `struct_start` word + `byte_offset` bytes, bit `bit`.
    pub fn write_bool(&mut self, struct_start: u32, byte_offset: u32, bit: u8, val: bool) {
        let idx = struct_start as usize * 8 + byte_offset as usize;
        let bytes: &mut [u8] = unsafe {
            std::slice::from_raw_parts_mut(self.words.as_mut_ptr() as *mut u8, self.words.len() * 8)
        };
        if val {
            bytes[idx] |= 1u8 << bit;
        }
    }

    /// Write a text string. Writes the list pointer at `ptr_word` and the NUL-terminated data.
    pub fn write_text(&mut self, ptr_word: u32, text: &str) {
        let len = text.len() as u32;
        let total = len + 1; // include NUL
        let n_words = (total + 7) / 8;
        let target = self.alloc(n_words);
        self.write_list_ptr(ptr_word, target, 2, total); // elem_sz=2 means byte
        let start = target as usize * 8;
        let bytes: &mut [u8] = unsafe {
            std::slice::from_raw_parts_mut(self.words.as_mut_ptr() as *mut u8, self.words.len() * 8)
        };
        bytes[start..start + text.len()].copy_from_slice(text.as_bytes());
        bytes[start + text.len()] = 0;
    }

    /// Produce a flat-array message: `[segment_table (8 bytes)] [segment_data]`.
    ///
    /// Segment table for single segment: `[0u32 (seg_count-1)] [seg_words u32]`.
    pub fn finish(&self) -> Vec<u8> {
        let seg_size = self.words.len() as u32;
        let mut result = vec![0u8; 8 + seg_size as usize * 8];
        // seg_count - 1 = 0
        result[0..4].copy_from_slice(&0u32.to_le_bytes());
        result[4..8].copy_from_slice(&seg_size.to_le_bytes());
        let word_bytes: &[u8] = unsafe {
            std::slice::from_raw_parts(self.words.as_ptr() as *const u8, self.words.len() * 8)
        };
        result[8..].copy_from_slice(word_bytes);
        result
    }
}

/// Returns the capnp element size tag for a scalar of the given byte size.
pub fn scalar_elem_tag(byte_size: u32) -> u8 {
    match byte_size {
        0 => 1, // bool = bit
        1 => 2,
        2 => 3,
        4 => 4,
        8 => 5,
        _ => panic!("unsupported scalar size: {}", byte_size),
    }
}

/// A value that can be packed into a Cap'n Proto message.
///
/// Implement this for your types to enable serialization.
/// For derive-macro support (future), this will be auto-generated.
pub trait CapnpPack {
    /// Return the member descriptors for layout computation.
    fn member_descs() -> Vec<MemberDesc>;

    /// Pack this value into the given buffer.
    /// `data_start` is the word index of the data section start.
    /// `ptrs_start` is the word index of the pointer section start.
    fn pack_into(&self, buf: &mut WordBuf, data_start: u32, ptrs_start: u32);

    /// Convenience: pack to a complete flat-array message.
    fn capnp_pack(&self) -> Vec<u8> {
        let layout = Self::capnp_layout();
        let mut buf = WordBuf::new();
        let root_ptr = buf.alloc(1);
        let data_start = buf.alloc(layout.data_words as u32);
        let ptrs_start = buf.alloc(layout.ptr_count as u32);
        buf.write_struct_ptr(root_ptr, data_start, layout.data_words, layout.ptr_count);
        self.pack_into(&mut buf, data_start, ptrs_start);
        buf.finish()
    }

    /// Compute the layout for this type.
    fn capnp_layout() -> CapnpLayout {
        CapnpLayout::compute(&Self::member_descs())
    }
}

// ── Scalar implementations ────────────────────────────────────────────

macro_rules! impl_capnp_pack_scalar {
    ($ty:ty, $sz:expr) => {
        impl CapnpPack for $ty {
            fn member_descs() -> Vec<MemberDesc> {
                vec![]
            }
            fn pack_into(&self, _buf: &mut WordBuf, _data_start: u32, _ptrs_start: u32) {
                // Scalars are packed inline by their parent struct
            }
            fn capnp_layout() -> CapnpLayout {
                CapnpLayout {
                    data_words: 1,
                    ptr_count: 0,
                    fields: vec![],
                    variants: vec![],
                }
            }
        }
    };
}

impl_capnp_pack_scalar!(bool, 0);
impl_capnp_pack_scalar!(u8, 1);
impl_capnp_pack_scalar!(i8, 1);
impl_capnp_pack_scalar!(u16, 2);
impl_capnp_pack_scalar!(i16, 2);
impl_capnp_pack_scalar!(u32, 4);
impl_capnp_pack_scalar!(i32, 4);
impl_capnp_pack_scalar!(f32, 4);
impl_capnp_pack_scalar!(u64, 8);
impl_capnp_pack_scalar!(i64, 8);
impl_capnp_pack_scalar!(f64, 8);

impl CapnpPack for String {
    fn member_descs() -> Vec<MemberDesc> {
        vec![]
    }
    fn pack_into(&self, _buf: &mut WordBuf, _data_start: u32, _ptrs_start: u32) {
        // Strings are packed by their parent struct via write_text
    }
    fn capnp_layout() -> CapnpLayout {
        CapnpLayout {
            data_words: 0,
            ptr_count: 1,
            fields: vec![],
            variants: vec![],
        }
    }
}

// ── Vec pack helpers ──────────────────────────────────────────────────

/// Pack a vector of scalars into the buffer, writing the list pointer at `ptr_word`.
pub fn pack_scalar_vec<T: Copy>(buf: &mut WordBuf, ptr_word: u32, vec: &[T], byte_size: u32) {
    if vec.is_empty() {
        return;
    }
    let count = vec.len() as u32;
    let total_bytes = count * byte_size;
    let n_words = (total_bytes + 7) / 8;
    let target = buf.alloc(n_words);
    buf.write_list_ptr(ptr_word, target, scalar_elem_tag(byte_size), count);
    let start = target as usize * 8;
    let src: &[u8] = unsafe {
        std::slice::from_raw_parts(vec.as_ptr() as *const u8, vec.len() * byte_size as usize)
    };
    let bytes: &mut [u8] = unsafe {
        std::slice::from_raw_parts_mut(
            buf.words_mut().as_mut_ptr() as *mut u8,
            buf.words_mut().len() * 8,
        )
    };
    bytes[start..start + src.len()].copy_from_slice(src);
}

/// Pack a vector of booleans as a bit list.
pub fn pack_bool_vec(buf: &mut WordBuf, ptr_word: u32, vec: &[bool]) {
    if vec.is_empty() {
        return;
    }
    let count = vec.len() as u32;
    let n_words = (count + 63) / 64;
    let target = buf.alloc(n_words);
    buf.write_list_ptr(ptr_word, target, 1, count);
    for (i, &v) in vec.iter().enumerate() {
        if v {
            let byte_idx = target as usize * 8 + i / 8;
            let bit_idx = i % 8;
            let bytes: &mut [u8] = unsafe {
                std::slice::from_raw_parts_mut(
                    buf.words_mut().as_mut_ptr() as *mut u8,
                    buf.words_mut().len() * 8,
                )
            };
            bytes[byte_idx] |= 1u8 << bit_idx;
        }
    }
}

/// Pack a vector of strings as a list of pointers to text.
pub fn pack_string_vec(buf: &mut WordBuf, ptr_word: u32, vec: &[String]) {
    if vec.is_empty() {
        return;
    }
    let count = vec.len() as u32;
    let outer = buf.alloc(count);
    buf.write_list_ptr(ptr_word, outer, 6, count); // elem_sz=6 means pointer
    for (i, s) in vec.iter().enumerate() {
        buf.write_text(outer + i as u32, s);
    }
}

/// Pack a vector of structs as a composite list.
pub fn pack_struct_vec<T: CapnpPack>(buf: &mut WordBuf, ptr_word: u32, vec: &[T]) {
    if vec.is_empty() {
        return;
    }
    let el = T::capnp_layout();
    let words_per = el.data_words as u32 + el.ptr_count as u32;
    let count = vec.len() as u32;

    let tag = buf.alloc(1);
    buf.write_composite_tag(tag, count, el.data_words, el.ptr_count);

    let first_elem = buf.alloc(count * words_per);
    buf.write_list_ptr(ptr_word, tag, 7, count * words_per);

    for (i, item) in vec.iter().enumerate() {
        let elem_start = first_elem + i as u32 * words_per;
        item.pack_into(buf, elem_start, elem_start + el.data_words as u32);
    }
}

impl WordBuf {
    /// Expose words mutably (for vec helpers that need raw access).
    pub fn words_mut(&mut self) -> &mut Vec<u64> {
        &mut self.words
    }
}

/// Write a field value into the data section of a struct being packed.
///
/// This is a helper for `pack_into` implementations.
pub fn pack_data_field<T: Copy>(
    buf: &mut WordBuf,
    data_start: u32,
    loc: &FieldLoc,
    val: T,
) {
    let val_bytes: &[u8] =
        unsafe { std::slice::from_raw_parts(&val as *const T as *const u8, std::mem::size_of::<T>()) };
    buf.write_bytes(data_start, loc.offset, val_bytes);
}

/// Write a bool field value.
pub fn pack_bool_field(buf: &mut WordBuf, data_start: u32, loc: &FieldLoc, val: bool) {
    buf.write_bool(data_start, loc.offset, loc.bit_index, val);
}

/// Write a text (string) field.
pub fn pack_text_field(buf: &mut WordBuf, ptrs_start: u32, loc: &FieldLoc, val: &str) {
    if !val.is_empty() {
        buf.write_text(ptrs_start + loc.offset, val);
    }
}

/// Write a nested struct field.
pub fn pack_struct_field<T: CapnpPack>(
    buf: &mut WordBuf,
    ptrs_start: u32,
    loc: &FieldLoc,
    val: &T,
) {
    let inner_layout = T::capnp_layout();
    let cd = buf.alloc(inner_layout.data_words as u32);
    let cp = buf.alloc(inner_layout.ptr_count as u32);
    buf.write_struct_ptr(
        ptrs_start + loc.offset,
        cd,
        inner_layout.data_words,
        inner_layout.ptr_count,
    );
    val.pack_into(buf, cd, cp);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_word_buf_alloc() {
        let mut buf = WordBuf::new();
        let w0 = buf.alloc(2);
        assert_eq!(w0, 0);
        let w1 = buf.alloc(1);
        assert_eq!(w1, 2);
    }

    #[test]
    fn test_finish_produces_valid_header() {
        let mut buf = WordBuf::new();
        buf.alloc(3); // 3 words of segment data
        let result = buf.finish();
        assert_eq!(result.len(), 8 + 3 * 8); // header + data
        // Check header: seg_count-1 = 0
        assert_eq!(u32::from_le_bytes(result[0..4].try_into().unwrap()), 0);
        // Check header: seg_words = 3
        assert_eq!(u32::from_le_bytes(result[4..8].try_into().unwrap()), 3);
    }
}
